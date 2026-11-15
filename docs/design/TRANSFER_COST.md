# 传输成本分析：MV + depth + HUDless + UI + else（自有宿主）

实测输入（Wuchang，2026-11-11，自有宿主，`SlInput=1`、`SkipDepth=0`、`Color=0`）：

```text
ngx: sr mv=… 3840x2160 fmt=34 (R16G16_FLOAT)  |  depth=… 2560x1440 fmt=19 (D32_FLOAT_S8X24)
render=2558x1439      output/immediate=2560x1440      78 fps
槽位：固定 3840x2880？不，kStreamMaxW/H=3840x2160，kMaxSlots=4
```

## 1. 现在每帧到底搬了多少字节

| 流 | 生产者（游戏 GPU） | 消费者（第二 GPU） |
|---|---|---|
| MV 3840x2160 RG16F | 着色器 pass：读 66.4 MB + 写 66.4 MB | 槽位→本地拷贝：读 66.4 + 写 66.4 |
| depth 2560x1440 D32S8X24 | 先整幅拷进 typeless RT（+29.5 读/+29.5 写），再 pass 写 14.7 | 槽位→本地拷贝：读 14.7 + 写 14.7 |
| HUDless（当前关） | 0 | 0 |
| UI（当前无） | 0 | 0 |
| else（相机/jitter/mvscale/常量） | 共享内存里几百字节 | 同左 |

合计 ≈ **生产者 178 MB/帧 + 消费者 162 MB/帧 = 340 MB/帧**，78 fps 下 ≈ 26 GB/s；
跨卡（PCIe）侧是消费者读槽位的 81 MB/帧 ≈ 6.3 GB/s。

CPU 侧记录时间不是问题（实测 `begin` 0.002 ms/帧），开销全在**显存带宽与跨卡读**上。

## 2. 结论：可以低很多，而且不需要换架构

七条改动，前三条就是数量级的差别：

| # | 改动 | 每帧流量 | 相对现在 |
|---|---|---|---|
| A | **消费者不再拷贝槽位**，直接把跨卡共享资源当 SRV 采样 | 340 → 178 MB | **1.9x** |
| B | MV / depth 传**半分辨率**（MV 1920x1080、depth 1280x720） | 178 → ~45 MB | **7.5x** |
| C | 单个打包 pass 写 MV+depth+UI（1 对 barrier，而不是 2 个 pass / 6 次转换） | 字节不变，省 0.1–0.3 ms 停顿 | 时间 |
| D | depth 用 `R32_FLOAT_X8X24_TYPELESS` 的 SRV 直读，省掉 typeless 来回 | depth 103 → 33 MB | 3x（depth 单项） |
| E | **UI 改用 1/4 分辨率 R8 掩码**（同一 pass 里由 backbuffer≠hudless 生成），而不是传 UI 颜色 | +0.5 MB/帧 | 比传 UI 颜色便宜 30x |
| F | HUDless：消费者直接读生产者 slot（不拷贝） | 0 额外 | — |
| G | `--fg-of`（我们用近零成本光流代替游戏 MV） | MV 流归零 | 极端档 |

A+B 合起来：**340 MB/帧 → 45 MB/帧（约 7.5 倍），跨卡读 81 → 17 MB/帧**。

## 3. 为什么自有宿主让这些更容易

```text
① 我们包装了 list，知道每个资源的真实 state → A/C/D 里的转换可以只在"确实需要"时发一次
② 我们包装了 queue → 拷贝可以记到 copy 队列（offload_ok），不与游戏主队列争带宽
③ 我们持有提交点 → UI 掩码可以在"本帧最后一个 submit"里算，此时 backbuffer（含 UI）与
   HUDless 都在手，帧号天然对齐
④ 跨卡共享已经就位：layout 里 slot 就是 ALLOW_CROSS_ADAPTER 的 place 资源，
   所以 A 只需要把消费者的 g_mv_local / g_depth_local 换成直接绑定
```

## 4. 各条的落地方式与验证判据

```text
A. bridge.cpp: 去掉 copy_content(g_mv_local, g_mv) / 深度同理，直接把 g_mv[slot] 绑给 FG
   判据: verify_pipeline.py 9/9；probe 行不变；PCIe 读从 81 MB/帧降到 ~0（消费者不读了？

B. capture: StreamScale=2 → record_downsample 输出到 slot_w/2 的内容矩形；
   MV 用 nearest（跨边缘不平均），depth 用 2x2 平均
   判据: --dump 的 MV 采样相邻像素仍单调；FG 画面在快速平移下无拖影

C. 一个 UAV pass 输出 3 个流的打包布局（相同 slot 内偏移），barrier 从 6 次降到 2 次
   判据: perf 行里 barrier 累计时间下降；GPU 时间线无多段串行

D. 深度 SRV 用 R32_FLOAT_X8X24_TYPELESS 直读源，删掉 g_depth_tl 中转
   判据: [depth] 行仍打印正确尺寸；dump 深度直方图与现状一致

E. UiMask=1：1/4 分辨率 R8，|backbuffer-hudless| > 阈值 → 255
   判据: 打开 FG 后字幕/准星不再被插值撕裂（这是 UI 掩码唯一要证明的事）

F. Color=1 时不再额外拷一份，直接把 HUDless slot 给消费者
   判据: Color 开启后 used 内存不再翻倍
```

## 5. 不建议做的事

```text
× 用 CPU 回读（staging）走一遍再上传：66 MB/帧回读 = 双倍带宽 + 同步等待
× 传全分辨率 UI 颜色：掩码已经够用，颜色贵 30 倍
× 为省钱改传 8-bit MV：MV 是亚像素量，8 bit 量化误差直接变成抖动
```

## 6. 一句话回答

**能，而且不用换架构。** 现在 340 MB/帧里有 162 MB 是消费者白拷的、66 MB 是 MV 传了 4 倍的
分辨率；把这两处去掉（A+B），再合并 pass、深度直读、UI 用掩码（C/D/E），
可以做到 **~45 MB/帧（约 7.5 倍下降）**，且画质项只有"MV/depth 半分辨率"一个真正的取舍，
其余都是纯去掉浪费。
