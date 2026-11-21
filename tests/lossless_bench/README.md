# lossless_bench —— “无损压缩替代降采样”的成本侧微基准

对应设计文档：[`docs/design/LOSSLESS_TRANSPORT.md`](../../docs/design/LOSSLESS_TRANSPORT.md)。

这个目录**不是**产品代码，也不参与 Windows 构建；它只回答一个问题：
**在渲染卡上，一次“读全分辨率、写压缩结果”的 pass 要多少毫秒？**

## 跑法（Linux + CUDA，任意 NVIDIA 卡；数字随卡的显存带宽缩放）

```bash
cd tests/lossless_bench
nvcc -O3 -arch=sm_86 lpbench.cu -o lpbench     # sm_86 = 3080 Ti；按自己的卡改
./lpbench
```

`-arch` 换成目标卡的计算能力（`nvidia-smi --query-gpu=compute_cap --format=csv`）。

## 2026-11-21 真实数据集（32 帧 4K，`E:\mgpu_dataset\20261121_155534_full`）

```text
stream   n   raw MB   out MB
mv      32     33.2     21.5   p5=1.500 p50=1.530 p95=1.570 min=1.490 max=1.850
depth   32     33.2     17.6   p5=1.880 p50=1.890 p95=1.900 min=1.870 max=1.900
colour  32     66.4     55.7   p5=1.190 p50=1.200 p95=1.200 min=1.190 max=1.200
96/96 frames bit-exact
```

「今天的 150% 流」压缩后（`downsample_test`）：MV **1.48x**（9.95 MB）、depth **1.77x**（8.34 MB）。
结论与成本账见 `docs/design/LOSSLESS_TRANSPORT.md` §12：**替代 ❌、叠加 ✅**。

## 2026-11-21 在 3080 Ti 上的结果（与项目的生产端同型号）

```text
device: NVIDIA GeForce RTX 3080 Ti  SMs=80  sm_clock=1695 MHz  bus_bw=912 GB/s  sm_86
copy 66MB -> 66MB (plain copy)        0.188 ms     707 GB/s effective
pack 2:1  66MB -> 33MB                0.130 ms     767 GB/s effective
unpack 2:1 33MB -> 66MB               0.148 ms     671 GB/s effective
tile bit-width scan (64k tiles)       0.257 ms     258 GB/s effective
downsample 4K->150% 9-tap median      0.307 ms     271 GB/s effective
```

读法：

- **固定码率 2:1 编码 0.13 ms**（66 MB 输入）—— 比它想替代的 9-tap 中值降采样
  （0.31 ms）还便宜。所以“压缩会更吃渲染卡”在固定码率下不成立。
- **需要规约/两趟的 pass 立刻贵一倍以上**（位宽扫描只读 66 MB 就要 0.26 ms），
  这是 GPU 上 LZ/ANS 类熵编码昂贵的结构性原因。
- 所有 kernel 都是**数据无关**的流式 kernel，所以耗时能外推到真实帧；
  **压缩率不能**，必须用真帧 dump 量（`scripts/lossless_ratio.py`）。

## 相关工具

| 工具 | 作用 |
|---|---|
| `scripts/lossless_ratio.py <dump-dir>` | 快速一维近似：三种压缩率（zlib 代理 / 32-bit 字 / fp16 双平面），只迭代一遍，纯 Python |
| `fr_codec.h` + `fr_codec_test.cpp` | **§6.1 的可执行规格**：2D 16×16 tile、左/上预测、per-lane 绝对值、raw 回退；编码 + 解码 + bit-exact 回环 + 300 组 fuzz |
| `downsample_test.cpp` | 先按同一个 3×3 足迹降到 150%，再分别量「今天的流」和「全分辨率」压缩后各多少（回答“叠加 vs 升级”） |
| `ratio_stats.py` | 把 `fr_codec_test` 的逐帧输出汇成 p5/p50/p95/min/max（gate 1 的判据是 p5） |
| `epe_test.cpp` | gate 2：全分辨率 MV → 150%/200% → 双线性重建 → 与真值比 EPE（全图 + 边界带）：`./epe_test 150 f00_mv.raw ...` |
| `temporal_test.cpp` | 帧间差分值不值：逐帧对比 intra / temporal / 每 tile 自适应（实测：不值） |
| `predictor_test.cpp` | left / MED / Paeth / avg 四种预测器对比（实测：差别 <1%，因为位宽被离群值支配） |
| `exception_test.cpp` | **例外编码**（主位宽 + mask + patch）vs 每 tile 最大值：MV +67.6%、depth +30.6%、colour +22.4% |
| `scripts/analyse_dump.py <dump-dir>` | 已有的配对与 MV 语义分析（同一套 raw 格式） |

## 参考实现的用法与结果

```bash
bash tests/lossless_bench/run.sh                  # 只跑合成用例（13 个边界 + 300 组 fuzz）
bash tests/lossless_bench/run.sh /path/to/dump    # 再在真实 --dump 帧上量精确比率
```

2026-11-21，真实 CP2077 帧（`/mnt/c/temp/dump3`、`dump2077b`）：

```text
f00_depth.raw   R32      2560x1440 nw=1   14.75 MB -> 7.62 MB   1.94x  bit-exact
f00_colour.raw  RGBA16F  3840x2160 nw=2   66.36 MB -> 54.55 MB  1.22x  bit-exact
f00_mv.raw      RG16F    2560x1440 nw=1   14.75 MB -> 0.12 MB   128x   bit-exact  (全零：dump 用了 --no-mv)
```

- **depth 1.94x** —— 比 zlib 的 1.54x 好，比一维近似（1.88x）也好；平滑浮点 + 有序整数
  预测正是这个码流擅长的。
- **colour 1.22x** —— 比一维近似（1.06x）好，但远不如 zlib（1.96x）：HDR 颜色的低尾数是
  噪声，任何“预测 + 固定位宽”都会被单个 32 位样本拖成整块 raw。
- **MV 128x 是假数** —— 那一帧是零缓冲，见 `docs/design/LOSSLESS_TRANSPORT.md` §11.2。

