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

## 2026-11-22 UI 平面（HUDless / UI）的压缩比

`ui_ratio_test.cpp` 用同一个 `fr_codec.h` 内核测合成 UI（4K R10A2 按 u32/像素）：

```bash
g++ -O2 -std=c++17 -I. ui_ratio_test.cpp -o ui_ratio_test && ./ui_ratio_test
```

4K 结果：空 0.26 MB（128x）、文字 HUD 3% 1.05 MB（32x）、密集 HUD 10% 2.82 MB（12x）、
整屏半透明面板 3.00 MB（11x），全部用例 `decode(encode(x)) == x` **bit-exact**。
**UI 不需要压缩**（半分辨率原样只有 8.29 MB）；这组数字的用途是给"全分辨率 UI 才上码流"
这个判据。成本账见
[`docs/design/UI_PLANE_TRANSPORT_COST.md`](../../docs/design/UI_PLANE_TRANSPORT_COST.md)。

「今天的 150% 流」压缩后（`downsample_test`）：MV **1.48x**（9.95 MB）、depth **1.77x**（8.34 MB）。
结论与成本账见 `docs/design/LOSSLESS_TRANSPORT.md` §12：**替代 ❌、叠加 ✅**。

## 2026-11-23 用真实数据集复测**生产内核**（v2.2 例外编码）

数据集：`E:\mgpu_dataset\20261121_155534_full\seg-full`（32 帧、4K、CP2077、`--dump` 格式，
`manifest.json` 里有抓取命令）。内核 = `fr_codec.h`（与 `lock_frame/shaders/fr_codec.hlsl`
逐字节一致），Linux + g++，**不需要 GPU**。

```bash
g++ -O2 -std=c++17 -I. fr_codec_test.cpp -o fr_codec_test
./fr_codec_test <dir>/*_mv.raw <dir>/*_depth.raw      # 回环 + 压缩比，64/64 bit-exact
```

```text
stream  n  raw MB   coded MB (min/med/max)   ratio p5 / med / max
MV     32    33.2   9.54 / 12.42 / 15.64     2.13 / 2.71 / 3.48
depth  32    33.2  11.52 / 13.09 / 13.61     2.46 / 2.53 / 2.88
colour 32    66.4          45.46             1.46          (仅供对照：颜色不走码流)

MV+depth 每帧 med 25.34 MB / max 27.95 MB  ->  40 fps 下 1.01 GB/s
```

**注意**：本文件上面 2026-11-21 那组（`mv 21.5 MB / 1.53x`、`depth 17.6 / 1.89x`）用的是
**v1 内核**（每 tile 单一 max 位宽，即 `lpbench.cu` / `lossless_ratio.py` 的模型）。生产内核
从 v2.2 起有例外编码（mask + patch），MV 21.5 → 12.4 MB、depth 17.6 → 13.1 MB。
**不要再用 1.53x / 1.89x 给渲染卡或链路做预算**；同理那行"150% 压缩后 MV 1.48x / 9.95 MB"
也是 v1 的数，v2.2 下是 2.54x / 5.81 MB。

### `tile_stats.cpp`（新）：每 tile 的 mode / 位宽分布

```bash
g++ -O2 -std=c++17 -I. tile_stats.cpp -o tile_stats && ./tile_stats <dir>/*_mv.raw
```

49 帧 4K MV+depth 的聚合：

```text
mode: plain 16.0%（均值）/ exception 84.0% / raw escape 0.0%（49 帧全部为 0）
width <= 4 bit 的 tile: 均值 45.9%（1% – 82%）
```

两个结论，直接用于 `docs/design/PERF_OPT_2026-11-23.md` §5 的消融判读：

1. **raw escape（mode 2）在真实数据上从不出现** ⇒ 内核里 `mode == 2` 的早退分支对 MV/depth
   不起作用，**每个 tile 都要付全部 13 次 `GroupMemoryBarrierWithGroupSync`**。所以
   `FrCodecAblate=1`（去扫描）测出的差值不会被"跳过的 tile"稀释，可以直接和 1.02 ms 比。
2. 位宽集中在 **1–3 bit**（MV，约 65% 的 tile）和 **7–8 / 14–16 bit**（depth）⇒ 打包的算术量
   很小，编码器是 **barrier / 延迟受限**，不是算术受限。这支持"`cs_5_0` 下界在 0.5–0.6 ms"
   的估计；要显著更快只能减少同步（DXC + `cs_6_0` 的 wave 内前缀和）。

### `CodecStreamPercent=150` 的真实字节（`downsample_test.cpp`）

```bash
g++ -O2 -std=c++17 -I. downsample_test.cpp -o downsample_test
./downsample_test <dir>/*_mv.raw <dir>/*_depth.raw
```

```text
frame   codec(100%) MV+depth   codec(150%) MV+depth   省
f00     24.78 MB               12.36 MB              -50%
f18     27.95 MB               13.62 MB              -51%
f30     26.87 MB               13.16 MB              -51%
```

即 `CodecStreamPercent=150` 把链路字节减半（40 fps 下 1.01 → ~0.51 GB/s），tile 数按 4/9 下降
⇒ 编解码 dispatch 的工作量同比例下降。**真机上 `fr: coded` 应落到 MV 5.8–7.6 MB、depth
6.0–6.6 MB**（本机的 150% 重采样是 3×3 中值近似，shader 是 medoid 取样，实测字节会略高）。

### 2026-11-23 追加：depth 先降到 fp16 再编码（§16.4 第 2 项的实测）

`f00_depth.raw`（4K、reversed-Z、p50=0.0030）先做 `float16 → float32` 量化，再用同一个内核编码：

```text
                      coded      ratio    max |err|    mean |err|   max rel err   mean rel err
fp32（今天）          13.61 MB    2.44x          -             -             -             -
fp16                  8.95 MB    3.71x     3.05e-05      1.22e-06       0.089%       0.0175%
```

- **字节 −34%**（13.61 → 8.95 MB），MV+depth 从 25.3 → 20.6 MB/帧。
- 误差按**线性距离**理解：reversed-Z 下 `d ∝ 1/z`，所以相对距离误差就是上表的相对 z 误差
  —— 最大 0.089%、平均 0.017%。对遮挡判断（XeFG 用深度只做 disocclusion）可以忽略。
- 为什么省这么多：量化后 **70% 的 tile 位宽 = 0**（残差全 0），而 fp32 时只有 1.0%。
  位宽直方图从 `7–8/14–16 bit 为主` 变成 `70% 是 0 + 26% 是 15–17 bit`。

**这顺带指出一个比"改编码器"更划算的方向**：既然 70% 的 tile 残差全 0，就该给码流加一个
**"常量 tile"（mode 4）**——只存绝对像素、跳过 scan/原子/pack/flush。按 `FrCodecAblate` 的
归因逻辑，depth 编码的 1.02 ms 里约有 70% 的 tile 会直接返回。代价是位流要加一种模式
（协议版本 +1），收益是 depth 的编码与解码同时变便宜，**而且这对天空/平坦区域本来就成立**。
这条路只在决定"depth 走 fp16"之后才值得做，所以顺序是：
先做 `CodecStreamPercent`/depth-fp16 的取舍，再加 mode 4。


### 这套数据集**不能**做什么

- GPU 时间：任何 ms 数都要真机（`ProfGpu=1` + `FrCodecAblate`，见 PERF_OPT §5）。

### 这套数据集**是静态镜头** —— 两处必须打折使用

用 conda 的 numpy（`/home/cainiao/miniconda3/bin/python`）跑
`scripts/analyse_dump.py` 的结果（32 帧全跑）：

```text
colour 帧间全局位移:  |dx| <= 0.33 px, |dy| <= 0.71 px     （整段几乎没有平移）
MV:                    |mv| max 0.06-0.11 px, p99 0.057-0.099 px,
                       大于 1 px 的像素占 0.000%
MV 单位:               直接是"像素"（不是 UV；乘 2160 的读法自相矛盾）
depth:                 min 0 / max 0.075 / mean 0.007 / **p50 0.003**, 恰好为 0 的占 1.1%
                       => reversed-Z（近=1 远=0），整段都是远景
```

1. **MV 的压缩比是"低运动"情形**：`|mv| < 0.1 px` ⇒ 残差只有 1–3 bit（正好对应
   `tile_stats` 的位宽分布）。所以 **2.71x 中位数不能当游戏内预算**，请用最差帧
   （15.64 MB / 2.12x），真实战斗场景可能更低。`CodecStreamPercent=150` 的"减半"结论不受影响
   （它比的是同一内容降采样前后的字节）。
2. **帧间配对 / MV 时序语义无法回答**：脚本自己的结论就是"picture barely moved …
   capture a panning scene"。要回答"帧 i 的 MV 配 i-1→i 还是 i→i+1"，必须**重新抓一段
   带平移的镜头**（`capture_dataset.ps1` 仍可用：dump 的 colour 取自 WGC sink）。
3. 也正因为整段静止，`raw escape`（mode 2）在这批数据上一次都没出现 —— 见上文 `tile_stats`；
   在**高熵/运动**场景下这个结论需要复测。



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

