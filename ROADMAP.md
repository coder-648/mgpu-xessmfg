# ROADMAP

只列**还没做、且想清楚了怎么做**的事。历史结论见
`docs/findings/CONCLUSIONS.md`，踩坑清单见 `docs/findings/FINAL_CONFIG.md` §5。

## 功能

### 1. `--overlay-monitor N` —— 组合 4 的正解 ✅ 已实现（2026-11-08）

**问题**：A 卡渲染 + N 卡上屏（或反过来）时，本机没跑通：游戏的 swapchain
反复重建后退出。这不是本项目能修的 —— 游戏不接受"输出设备换了"。

**做法**：不要动游戏的窗口。把覆盖窗口（DirectComposition）直接创建在
**另一块屏对应的输出**上，bridge 在那里上屏；游戏窗口原地不动。

- `--overlay-monitor N` 已实现（`--capture-monitor` 的镜像：一个管抓哪块屏、一个管上屏到哪块屏）；
- 覆盖窗口的位置/尺寸现在取**目标屏的 `rcMonitor`**（原来写死 `(0,0)`，只在游戏位于主屏时成立）；
- `g_refresh_hz` 改成读**目标屏**的显示模式（整拍按上屏屏算，不再按主屏）；
- 目标屏若不由消费卡驱动会先 WARNING 再回落到游戏窗口所在屏（不会黑屏）。

**实测限制（本机）**：一台机器上一块屏接 AMD、一块接 NVIDIA 时，`--overlay-monitor`
会把 XeFG 换到渲染卡（"驱动上屏屏"优先），插帧与游戏抢同一张 GPU：
`gpu` 3.2 → 66–72 ms。⇒ **要它真正有用，两块屏必须都接帧生成卡**。
证据与数字见 `docs/findings/PERF_BASELINE.md` §3.12。

**同时到手的诊断开关** `--overlay-hidden`：交换链照建、Present 照跑，窗口不显示。
它证明了一件事 —— 把覆盖窗口藏起来，抓帧晚一拍的比率从 **8.5% 掉到 0.7%**、
空刷新从 **6.6% 掉到 1.2%**，即**抖动就是同屏覆盖窗口造成的**（游戏失去 independent flip）。


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

### 0. RE Engine / PRAGMATA：NGX 钩子 + 加载慢 + 目录重定向 ✅ 已实现并验证（v0.13.1）

**问题**：PRAGMATA 走 Streamline，NGX 入口在游戏目录自带的 `nvngx_dlss.dll` 上；
旧的"只认 `_nvngx.dll`"判据让它一个钩子都没装（add-on 被 ReShade 正常加载但无数据）。

**已做并验证**：① `in_ngx.cpp` 判据改为"文件名含 `nvngx` ＋ 真的导出 `EvaluateFeature` ＋
排除 `dlssg/dlssd/dlssnr`"；② `[capture] LoaderHook=0` 解决加载变慢；③ 采集暂停改为
"帧数或 8 秒"；④ 面包屑 + ini/日志回退到 `%LOCALAPPDATA%\mgpu`；⑤ 安装器 6/6 镜像
`_storage_` 这类重定向目录。PRAGMATA 实测：`input source: ngx` + `shared ready` + bridge 出帧。

**下次做**：

1. 游戏目录里双击 `apply-mgpu-addon.bat`（会先确认进程退出，再替换成 `.new`）；
2. 进世界跑十几秒，看 `game_capture.log` 是否出现
   `NGX provider: ...nvngx_dlss.dll` / `NGX hooks active` / `input source: ngx`；
3. 通了就重建发布 **v0.13.1**，并用 dump 看 RE Engine 的 MV 是否可用
   （`7 - UE5 MV A-B.bat <exe> dump 1` + `analyse_dump.py`）。

细节：`docs/findings/RE_ENGINE_NOTES.md`。

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
