# ROADMAP

只列**还没做、且想清楚了怎么做**的事。历史结论见
`docs/findings/CONCLUSIONS.md`，踩坑清单见 `docs/findings/FINAL_CONFIG.md` §5。

## 功能

### 1. `--overlay-monitor N` —— 组合 4 的正解

**问题**：A 卡渲染 + N 卡上屏（或反过来）时，本机没跑通：游戏的 swapchain
反复重建后退出。这不是本项目能修的 —— 游戏不接受"输出设备换了"。

**做法**：不要动游戏的窗口。把覆盖窗口（DirectComposition）直接创建在
**另一块屏对应的输出**上，bridge 在那里上屏；游戏窗口原地不动。

- 现有代码里 `--capture-monitor` 已经能指定"抓哪块屏"，`--overlay-monitor`
  是它的镜像：**上屏放哪块屏**；
- 关键点：覆盖窗口的输出只决定 DirectComposition 的 target，XeFG 的输出尺寸
  仍由 `--den` / `--scale` 决定，两者独立；
- 验收：双屏（一块接 A 卡、一块接 N 卡）下，游戏在屏 1、成品在屏 2，
  `gen N/N` 配对不掉。

### 2. 5K 原生路径

现状：5K（5120×2880）只有 `2x@30 ≈ 60 fps` 能跑，因为每生成一个 5K 帧太贵。

- 短期：把 `--scale 0.75` 作为**文档化的推荐折衷**（现在只是散落在实验里）；
- 中期：让 `--den` 与生成分辨率解耦（在 1440p 生成、上屏时放回 5K），
  代价是上采样一次的画质损失，需要用 `--dump` 做 A/B 量化；
- 不做：指望 XeFG 在 5K 下变便宜。

### 3. `--dump` 逐像素对比工具

`--dump` 现在能同时写出 colour / depth / MV，`scripts/analyse_dump.py` 只做了
最基础的统计。补一个"光流 MV vs 游戏 MV"的对比：

- 读 `f<NN>_mv.raw`（R16G16_FLOAT）+ `_meta.txt`；
- 输出：逐像素差的直方图、相关系数、符号/单位是否一致的判定；
- 用途：换 `libxess_fg` 版本后**快速复验**"MV 是否仍被消费、方向是否仍正确"。

### 4. `SlotPolicy=1` 标为遗留

`SlotPolicy=1`（没空槽就等 `SlotWaitUs`）实测**没有收益**，v10 的信用窗口
（`consumed[]`）才是正解。文档里已标注为遗留开关
（`docs/ops/CONFIG.md` §3），代码里保留（协议兼容），但**不再推荐**。

### 5. 严格配对校验：`frame_id` ↔ WGC `SystemRelativeTime`（优先做这个）

**问题**：颜色来自 WGC（晚一帧，10–70 ms 浮动），MV/深度来自共享槽，两者按
**轮转槽位**近似配对，没有帧号级保证（详见 `docs/design/PROTOCOL.md` 的
"颜色与输入流的配对语义"一节）。生产端丢帧时配对会偏 1 帧并保持偏。

**做法**（零带宽、几十行）：

1. `bridge` 在收到 WGC 帧时读 `IDirect3D11CaptureFrame::SystemRelativeTime`；
2. 与消费端读到的 `frames[slot].frame_id` / 本地时间戳一起记下，算**实测偏差**；
3. 偏差超过阈值就写一条日志（`[pair] colour is N frames ahead/behind the slot`），
   `--dump` 的 `_meta.txt` 也带上；
4. 有了实测数据再决定要不要动颜色通路（见 §6）。

> 先测量、后改架构。当前推荐配方（3x + 光流）里 MV 是同帧现算的，
> 这个偏差对画质没有可见影响；它值得修的是"可观测性"。

### 6. `--colour shared`：颜色也走共享槽（**先别做**）

唯一实质收益是**严格同帧**（颜色/MV/深度同槽同一次发布），把 2x + 游戏 MV
配方里残留的 ±1 帧不确定彻底消掉。代价与前提见
`docs/design/ARCHITECTURE.md` §6.1：4K +33 MB/帧、渲染卡 **80%→95%**、
颜色槽常驻 225 MiB 系统内存、SDR 游戏格式不匹配直接跳过。

真要做，四个前置条件缺一不可：

1. 消费端加"从 `streams[0].offset` 读颜色"的通路（现在**完全不读**）；
2. 生产端加 `Color=2`：在 `on_present` 拷，避开 `on_begin_effects` 的 UI 时序问题；
3. **颜色拷贝失败必须降级为 warning**，不能再走 `ok = false`（现在会连坐 MV/深度）；
4. 颜色槽按实际分辨率放置，别固定 5120×2880。

## 工程债

性能相关的基线数据与候选优化（含"怎么验"）见
`docs/findings/PERF_BASELINE.md` §3，这里不重复。

| 项 | 说明 |
|---|---|
| `bridge.cpp` 6.9k 行 | 里面混着 OSD、XeFG 接线、五个诊断工具（`--dump`/`--mv-shadow`/`--probe-colour`/`--fg-probe`/`--list-modes`）。值得按"帧循环 / 诊断 / 输出"拆三个 TU |
| `--dyn-mfg` | 逐字移植自 OptiScaler 的控制器（GPL-3.0）。实测「固定倍率」更好（§9.19），可以考虑移除 —— 移除后才有资格谈宽松许可（`docs/ops/LEGAL.md` §5） |
| 里程碑探针 | `tests/`、`src/m2_probe`、`src/m3_depth`、`osd_test.cpp` 不参与构建，只作可追溯用 |

## 明确不做

- **不支持** TSR / FSR1 / 原生 TAA（没有 MV，插值质量不可接受）；
- **不做**消费者侧锁帧（`--lock` 的边界见 §9.16）；
- **不修**游戏 swapchain 重建崩溃（那是游戏/驱动侧的行为）。
