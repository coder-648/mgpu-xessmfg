# 已移除的实验脚本（归档记录）

2026-11-08 整理：`scripts/archive/` 下的 **60 个一次性对照实验脚本已从仓库移除**，
避免新读者被历史参数误导。本文保留**文件名、当时的参数与结论**，
这样 `docs/findings/FINAL_CONFIG.md` §9 里的每条结论仍然能追到"是哪次实验测出来的"。

**日常只用 `scripts/recipes/` 下那几份配方脚本**：
`run_3x_of.bat`（3x 光流，推荐）/ `run_2x_quality.bat`（2x 画质）/
`run_3x_nomv.bat`（最低开销）/ `run_3x_of_nodepth.bat`（省一半总线的 A/B）/
`run_dump.bat`（抓帧对比）。

> ⚠️ 下表里的 `--no-mv` 与注释中的"MV 是死输入"来自 2026-11-07 之前的结论，
> **已被推翻**（XeFG 确实消费 MV，见 `CONCLUSIONS.md` §2）。
> `--fg-jitter-zero` 已弃用，现在只打一行提示。

## 分类

| 前缀 / 关键词 | 当时在验证什么 | 结论 |
|---|---|---|
| `*_of_*`（`const_px/const_uv/huge/huge_hires/uv/px/ndc/native/plain/flip/zero/nodepth/jitmv/jz/jz_only/onlyinterp/hi`） | `--fg-of` 光流场的单位、符号、常量覆盖、缩放、深度引导 | 当时判为"不影响画面"；2026-11-08 用 `--mv-shadow` 复核后改为：**MV 确实被消费**，光流成为 3x 的首选来源 |
| `*depthlag*` | 深度"延迟 1/2/3 帧"能否改善对齐 | 更差；默认 `--depth-lag 0` |
| `*_native`（2x/3x/4x_4k） | 抓取分辨率 vs 渲染分辨率 | 渲染分辨率足够 |
| `*const_*`、`*plain*`、`*uv*`、`*ndc*` | 直接伪造 MV 单位，验证消费端是否读 tag | 当时判为"读不到"；后被 `--mv-shadow` 推翻 |
| `_FAILED_*` | 3x@4K、`--pace`、`--scale 0.72` | 见 `FINAL_CONFIG.md` §9.11 |
| `_FAILED_run_mgpu_pbz_cadence` | 变倍率生成（`--interp-cadence`）：逐帧改 count | 输入 26→63–78 ms、输出 63–79 fps，比常数倍率更差；§9.18 |
| `run_mgpu_pbz_dyn*` / `dynmfg*` | `--dyn-mfg` 动态倍率的各版本接线 | 定稿为"锁帧 + 固定倍率"（§9.19），`--dyn-mfg` 仅作实验开关保留 |
| `run_mgpu_5k*` / `run_mgpu_pbz_100_4x25` / `run_mgpu_pbz_120_5x24` | 无 VRR 时的精确节拍解（120 Hz 下的整数 vblank 组合） | 见 §9.15 |
## 完整清单

| 脚本 | 当时在验证什么（原文摘录） | bridge 参数 | 结论 / 去向 |
|---|---|---|---|
| `_FAILED_run_mgpu_3x_4k.bat` | ##  !!!  FAILED EXPERIMENT - DO NOT USE  !!!                ## | `--scale 0.75 --pool 4 --mv-newest --interp 2 --unlock-mfg --fg-ui none` | 已实测失败（§9.11） |
| `_FAILED_run_mgpu_3x_4k_slack.bat` | ##  !!!  FAILED EXPERIMENT - DO NOT USE  !!!                ## | `--scale 0.75 --pool 4 --mv-newest --interp 2 --unlock-mfg --fg-ui none --slack 1` | 已实测失败（§9.11） |
| `_FAILED_run_mgpu_4x_4k_072.bat` | ##  !!!  FAILED EXPERIMENT - DO NOT USE  !!!                ## | `--scale 0.72 --pool 4 --mv-newest --interp 3 --unlock-mfg --fg-ui none` | 已实测失败（§9.11） |
| `_FAILED_run_mgpu_4x_4k_pace.bat` | ##  !!!  FAILED EXPERIMENT - DO NOT USE  !!!                ## | `--scale 0.75 --pool 4 --mv-newest --interp 3 --unlock-mfg --fg-ui none --pace 30` | 已实测失败（§9.11） |
| `_FAILED_run_mgpu_pbz_cadence.bat` | run_mgpu_pbz_cadence.bat  -  PBZ: variable-rate FG, NO frame lock The problem: a constant multiplier only lands on whole vblanks when the in | `--game PBZ.exe --den 1 --pool 4 --interp 5 --interp-cadence --fg-rt-mode median --unlock-mfg --fg-ui none --fg-proj-rz --fg-fov 58.7 --no-mv --fg-jitter-zero` | 变倍率生成：实测失败（§9.18） |
| `run_mgpu.bat` | PERFORMANCE profile  (1440p interpolated -> 76-80 fps) deploy: <TOOLS_DIR>\run_mgpu.bat   (next to bridge.exe + the two | `--den 2 --pool 4 --no-mv` | 早期配方（被 `scripts/recipes/` 取代） |
| `run_mgpu_2x_4k_native.bat` | 2x at native 4K, for a 59/60 Hz panel Why this exists: XeFG can only push (interp+1) frames per input frame, s | `--den 1 --pool 4 --mv-newest --interp 1 --fg-ui none` | 抓取 vs 渲染分辨率对照 |
| `run_mgpu_3x_40.bat` | 3x at 1440p with a 40 fps lock 40 x 3 = 120 fps, so the cadence is 1:1 with a 120 Hz panel - the same as | `--den 2 --pool 4 --no-mv --interp 2 --unlock-mfg --fg-ui none` | 早期配方（被 `scripts/recipes/` 取代） |
| `run_mgpu_3x_4k_native.bat` | 3x at native 4K, 40 fps in -> 120 fps out Same cadence as 4x@30 (1:1 with a 120 Hz panel) but the input frame | `--den 1 --pool 4 --mv-newest --interp 2 --unlock-mfg --fg-ui none` | 抓取 vs 渲染分辨率对照 |
| `run_mgpu_4x.bat` | MAX SMOOTHNESS  (1440p interpolation, 4x: 30 -> 120 fps) deploy: <TOOLS_DIR>\run_mgpu_4x.bat   (next to bridge.exe + | `--den 2 --pool 4 --no-mv --interp 3 --unlock-mfg` | 早期配方（被 `scripts/recipes/` 取代） |
| `run_mgpu_4x_4k.bat` | 4x with the downsample BEFORE frame generation The panel is 4K; VSR makes the game render 5K for detail. That supersa | `--scale 0.75 --pool 4 --no-mv --interp 3 --unlock-mfg --fg-ui none` | 早期配方（被 `scripts/recipes/` 取代） |
| `run_mgpu_4x_4k_hq.bat` | same as run_mgpu_4x_4k.bat, but the 5K -> 4K resize uses a real low-pass filter instead of a single bilinear tap. | `--scale 0.75 --pool 4 --mv-newest --interp 3 --unlock-mfg --fg-ui none --down 1` | 早期配方（被 `scripts/recipes/` 取代） |
| `run_mgpu_4x_4k_native.bat` | 4x at native 4K: no VSR, no resize anywhere Setup this profile expects: | `--den 1 --pool 4 --no-mv --interp 3 --unlock-mfg --fg-ui none` | 抓取 vs 渲染分辨率对照 |
| `run_mgpu_4x_4k_nodepth.bat` | 4x at 4K, depth stream not imported Same chain as run_mgpu_4x_4k_native.bat plus --no-depth: XeFG gets a dumm | `--den 1 --pool 4 --mv-newest --interp 3 --unlock-mfg --fg-ui none --no-depth` | 开关深度的对照 |
| `run_mgpu_4x_4k_pbz.bat` | 4x profile for another game (PBZ UE5) Same chain as run_mgpu_4x_4k_native.bat, plus --game so the bridge looks | `--game PBZ.exe --den 1 --pool 4 --mv-newest --interp 3 --unlock-mfg --fg-ui none --fg-proj-rz --fg-fov 58.7 --no-mv` | 早期配方（被 `scripts/recipes/` 取代） |
| `run_mgpu_4x_4k_pbz_depthlag1.bat` | PBZ 4K, DEPTH ONLY (no motion vectors) Why: a controlled test proved the tagged motion-vector resource | `--game PBZ.exe --den 1 --pool 4 --interp 3 --unlock-mfg --fg-ui none --fg-proj-rz --fg-fov 58.7 --no-mv --depth-lag 1` | 深度延迟 1/2/3 帧：更差 |
| `run_mgpu_4x_4k_pbz_depthlag2.bat` | PBZ 4K, DEPTH ONLY (no motion vectors) Why: a controlled test proved the tagged motion-vector resource | `--game PBZ.exe --den 1 --pool 4 --interp 3 --unlock-mfg --fg-ui none --fg-proj-rz --fg-fov 58.7 --no-mv --depth-lag 2` | 深度延迟 1/2/3 帧：更差 |
| `run_mgpu_4x_4k_pbz_depthlag3.bat` | PBZ 4K, DEPTH ONLY (no motion vectors) Why: a controlled test proved the tagged motion-vector resource | `--game PBZ.exe --den 1 --pool 4 --interp 3 --unlock-mfg --fg-ui none --fg-proj-rz --fg-fov 58.7 --no-mv --depth-lag 3` | 深度延迟 1/2/3 帧：更差 |
| `run_mgpu_4x_4k_pbz_depthonly.bat` | PBZ 4K, DEPTH ONLY (no motion vectors) Why: a controlled test proved the tagged motion-vector resource | `--game PBZ.exe --den 1 --pool 4 --interp 3 --unlock-mfg --fg-ui none --fg-proj-rz --fg-fov 58.7 --no-mv` | 开关深度的对照 |
| `run_mgpu_4x_4k_pbz_of.bat` | PBZ 4K with MOTION VECTORS FROM OUR OWN optical flow (AMD FidelityFX) instead of the game's MV stream. | `--game PBZ.exe --den 1 --pool 4 --mv-newest --interp 3 --unlock-mfg --fg-ui none --fg-of --fg-proj-rz --fg-fov 58.7` | 光流实验；2026-11-08 复核：MV 确实被消费 |
| `run_mgpu_4x_4k_pbz_of_const_px.bat` | PBZ 4K, MV *and* depth at the optical flow field's own size (480x270 = 4K/8), so neither stream is re | `--game PBZ.exe --den 1 --pool 4 --mv-newest --interp 3 --unlock-mfg --fg-ui none --fg-of --fg-of-const 200 0 --fg-of-units px --fg-proj-rz --fg-fov 58.7` | 光流实验；2026-11-08 复核：MV 确实被消费 |
| `run_mgpu_4x_4k_pbz_of_const_uv.bat` | PBZ 4K, MV *and* depth at the optical flow field's own size (480x270 = 4K/8), so neither stream is re | `--game PBZ.exe --den 1 --pool 4 --mv-newest --interp 3 --unlock-mfg --fg-ui none --fg-of --fg-of-const 200 0 --fg-of-units uv --fg-proj-rz --fg-fov 58.7` | 光流实验；2026-11-08 复核：MV 确实被消费 |
| `run_mgpu_4x_4k_pbz_of_depthinv.bat` | PBZ 4K, MV *and* depth at the optical flow field's own size (480x270 = 4K/8), so neither stream is re | `--game PBZ.exe --den 1 --pool 4 --mv-newest --interp 3 --unlock-mfg --fg-ui none --fg-of --fg-depth-inv --fg-proj-rz --fg-fov 58.7` | 光流实验；2026-11-08 复核：MV 确实被消费 |
| `run_mgpu_4x_4k_pbz_of_flip.bat` | PBZ 4K with MOTION VECTORS FROM OUR OWN optical flow (AMD FidelityFX) instead of the game's MV stream. | `--game PBZ.exe --den 1 --pool 4 --mv-newest --interp 3 --unlock-mfg --fg-ui none --fg-of --fg-of-sign -1 --fg-proj-rz --fg-fov 58.7` | 光流实验；2026-11-08 复核：MV 确实被消费 |
| `run_mgpu_4x_4k_pbz_of_mvpx.bat` | run_mgpu_4x_4k_pbz_of_mvpx.bat optical-flow MV in the units XeSS-FG actually wants | `--game PBZ.exe --den 1 --pool 4 --interp 3 --unlock-mfg --fg-ui none --fg-of --fg-proj-rz --fg-fov 58.7` | 光流实验；2026-11-08 复核：MV 确实被消费 |
| `run_mgpu_4x_4k_pbz_of_native.bat` | PBZ 4K, MV *and* depth at the optical flow field's own size (480x270 = 4K/8), so neither stream is resa | `--game PBZ.exe --den 1 --pool 4 --mv-newest --interp 3 --unlock-mfg --fg-ui none --fg-of --fg-proj-rz --fg-fov 58.7` | 光流实验；2026-11-08 复核：MV 确实被消费 |
| `run_mgpu_4x_4k_pbz_of_ndc.bat` | PBZ 4K, MV *and* depth at the optical flow field's own size (480x270 = 4K/8), so neither stream is resampl | `--game PBZ.exe --den 1 --pool 4 --mv-newest --interp 3 --unlock-mfg --fg-ui none --fg-of --fg-of-units ndc --fg-proj-rz --fg-fov 58.7` | 光流实验；2026-11-08 复核：MV 确实被消费 |
| `run_mgpu_4x_4k_pbz_of_nodepth.bat` | PBZ 4K with MOTION VECTORS FROM OUR OWN optical flow (AMD FidelityFX) instead of the game's MV stream. | `--game PBZ.exe --den 1 --pool 4 --mv-newest --interp 3 --unlock-mfg --fg-ui none --fg-of --no-depth --fg-proj-rz --fg-fov 58.7` | 光流实验；2026-11-08 复核：MV 确实被消费 |
| `run_mgpu_4x_4k_pbz_of_plain.bat` | PBZ 4K with MOTION VECTORS FROM OUR OWN optical flow (AMD FidelityFX) instead of the game's MV stream. | `--game PBZ.exe --den 1 --pool 4 --mv-newest --interp 3 --unlock-mfg --fg-ui none --fg-of --fg-of-guide 0 --fg-proj-rz --fg-fov 58.7` | 光流实验；2026-11-08 复核：MV 确实被消费 |
| `run_mgpu_4x_4k_pbz_of_uv.bat` | PBZ 4K, MV *and* depth at the optical flow field's own size (480x270 = 4K/8), so neither stream is resample | `--game PBZ.exe --den 1 --pool 4 --mv-newest --interp 3 --unlock-mfg --fg-ui none --fg-of --fg-of-units uv --fg-proj-rz --fg-fov 58.7` | 光流实验；2026-11-08 复核：MV 确实被消费 |
| `run_mgpu_4x_4k_pbz_of_zero.bat` | PBZ 4K with MOTION VECTORS FROM OUR OWN optical flow (AMD FidelityFX) instead of the game's MV stream. | `--game PBZ.exe --den 1 --pool 4 --mv-newest --interp 3 --unlock-mfg --fg-ui none --fg-of --fg-of-scale 0 --fg-proj-rz --fg-fov 58.7` | 光流实验；2026-11-08 复核：MV 确实被消费 |
| `run_mgpu_4x_b.bat` | same as run_mgpu_4x.bat plus the tuning flags that OptiScaler exposes for its own XeFG output (A/B against the baselin | `--den 2 --pool 4 --mv-newest --interp 3 --unlock-mfg --fg-ui none --fg-rt 0` | 早期配方（被 `scripts/recipes/` 取代） |
| `run_mgpu_4x_c.bat` | 4x at 1440p with --fg-ui none only (A/B round 3) deploy: <TOOLS_DIR>\run_mgpu_4x_c.bat | `--den 2 --pool 4 --no-mv --interp 3 --unlock-mfg --fg-ui none` | 早期配方（被 `scripts/recipes/` 取代） |
| `run_mgpu_4x_pbz_1440.bat` | 4x for PBZ, interpolating at 1440p (not 4K) Why: with real motion vectors XeFG's GPU cost at 4K jumps from ~22 | `--game PBZ.exe --den 2 --pool 4 --mv-newest --interp 3 --unlock-mfg --fg-ui none` | 早期配方（被 `scripts/recipes/` 取代） |
| `run_mgpu_5k.bat` | QUALITY profile  (native 5120x2880 -> 58-61 fps) deploy: <TOOLS_DIR>\run_mgpu_5k.bat   (next to bridge.exe + DLLs) | `--den 1 --pool 4 --no-mv --wait-ms 0` | 无 VRR 的精确节拍解（§9.15） |
| `run_mgpu_5k_mfg.bat` | QUALITY + MFG  (native 5K, 30 fps -> 120 fps at 4x) deploy: <TOOLS_DIR>\run_mgpu_5k_mfg.bat   (next to bridge.exe | `--den 1 --pool 4 --no-mv --wait-ms 0 --interp 3 --unlock-mfg` | 无 VRR 的精确节拍解（§9.15） |
| `run_mgpu_cp2077_const_px.bat` | 4K native + CONSTANT 200px MV as px (consumption test) Setup this profile expects: | `--den 1 --pool 4 --mv-newest --interp 3 --unlock-mfg --fg-ui none --fg-of --fg-of-const 200 0 --fg-of-units px` | 伪造 MV 单位（当时结论后被推翻） |
| `run_mgpu_cp2077_const_uv.bat` | 4K native + CONSTANT 200px MV as UV (consumption test) Setup this profile expects: | `--den 1 --pool 4 --mv-newest --interp 3 --unlock-mfg --fg-ui none --fg-of --fg-of-const 200 0 --fg-of-units uv` | 伪造 MV 单位（当时结论后被推翻） |
| `run_mgpu_cp2077_dump.bat` | 4x at native 4K: no VSR, no resize anywhere Setup this profile expects: | `--den 1 --pool 4 --no-mv --interp 3 --unlock-mfg --fg-ui none --dump 5 --dump-after 300 --dump-dir <STAGE_DIR` | `--dump` 抓帧做离线配对分析 |
| `run_mgpu_cp2077_of_huge.bat` | MV = 100000 px (consumption test: clean=ignored, destroyed=consumed) Setup this profile expects: | `--den 1 --pool 4 --mv-newest --interp 3 --unlock-mfg --fg-ui none --fg-of --fg-of-const 100000 0 --fg-of-units px` | 光流实验；2026-11-08 复核：MV 确实被消费 |
| `run_mgpu_cp2077_of_huge_hires.bat` | HUGE MV + HIGH_RES_MV init flag (does the flag make XeFG read our MV?) Setup this profile expects: | `--den 1 --pool 4 --mv-newest --interp 3 --unlock-mfg --fg-ui none --fg-of --fg-of-const 100000 0 --fg-of-units px --fg-hi-res-mv` | 光流实验；2026-11-08 复核：MV 确实被消费 |
| `run_mgpu_cp2077_of_px.bat` | 4K native + our optical-flow MV, pixel units Setup this profile expects: | `--den 1 --pool 4 --mv-newest --interp 3 --unlock-mfg --fg-ui none --fg-of --fg-of-units px` | 光流实验；2026-11-08 复核：MV 确实被消费 |
| `run_mgpu_cp2077_of_uv.bat` | 4K native + our optical-flow MV, UV units Setup this profile expects: | `--den 1 --pool 4 --mv-newest --interp 3 --unlock-mfg --fg-ui none --fg-of --fg-of-units uv` | 光流实验；2026-11-08 复核：MV 确实被消费 |
| `run_mgpu_cp2077_of_zero.bat` | MV forced to ZERO (does MV matter at all?) Setup this profile expects: | `--den 1 --pool 4 --mv-newest --interp 3 --unlock-mfg --fg-ui none --fg-of --fg-of-scale 0` | 光流实验；2026-11-08 复核：MV 确实被消费 |
| `run_mgpu_pbz_100_4x25.bat` | PBZ: 100 Hz panel + 25 fps input + 4x  = 100 fps (exactly 1 vblank per output frame) | `--game PBZ.exe --den 1 --pool 4 --interp 3 --fg-rt-mode median --unlock-mfg --fg-ui none --fg-proj-rz --fg-fov 58.7 --no-mv` | 无 VRR 的精确节拍解（§9.15） |
| `run_mgpu_pbz_120_5x24.bat` | PBZ: 5x of a 24 fps input = EXACTLY 120 fps Why this exists (measured 2026-11-07): | `--game PBZ.exe --den 1 --pool 4 --interp 4 --fg-rt-mode median --unlock-mfg --fg-ui none --fg-proj-rz --fg-fov 58.7 --no-mv` | 无 VRR 的精确节拍解（§9.15） |
| `run_mgpu_pbz_3x40.bat` | PBZ: 3x at 4K with a 40 fps input (lower latency) Why this is possible now: motion vectors turned out to be a dead | `--game PBZ.exe --den 1 --pool 4 --interp 2 --unlock-mfg --fg-ui none --fg-proj-rz --fg-fov 58.7 --no-mv` | 早期配方（被 `scripts/recipes/` 取代） |
| `run_mgpu_pbz_3x40_nvgpu.bat` | same as run_mgpu_pbz_3x40.bat, but frame generation runs on the NVIDIA card (--gpu 10de) instead of the AMD | `--game PBZ.exe --gpu 10de --den 1 --pool 4 --interp 2 --unlock-mfg --fg-ui none --fg-proj-rz --fg-fov 58.7 --no-mv` | 早期配方（被 `scripts/recipes/` 取代） |
| `run_mgpu_pbz_3x_slack1.bat` | PBZ: FIXED 3x + absorb a late capture frame Why (measured 2026-11-07, bridge_pbz_dynmfg2.txt, 83 windows): | `--game PBZ.exe --den 1 --pool 4 --interp 2 --slack 1 --unlock-mfg --fg-ui none --fg-proj-rz --fg-fov 58.7 --no-mv` | `--slack` 用延迟换窗口（§9.9 无效） |
| `run_mgpu_pbz_dyn.bat` | PBZ: DYNAMIC frame times (no RTSS lock), smoothness A/B Symptom this targets: with a *variable* game frame time the | `--game PBZ.exe --den 1 --pool 4 --interp 3 --interp-auto --fg-rt-mode quant --unlock-mfg --fg-ui none --fg-proj-rz --fg-fov 58.7 --no-mv` | 动态倍率各版本；定稿改固定倍率（§9.19） |
| `run_mgpu_pbz_dyn_ema.bat` | CONTROL for run_mgpu_pbz_dyn_rt.bat Identical to run_mgpu_pbz_dyn_rt.bat except the frameRenderTime feed is the | `--game PBZ.exe --den 1 --pool 4 --interp 3 --fg-rt-mode ema --unlock-mfg --fg-ui none --fg-proj-rz --fg-fov 58.7 --no-mv` | 动态倍率各版本；定稿改固定倍率（§9.19） |
| `run_mgpu_pbz_dyn_rt.bat` | PBZ: dynamic frame times, only the frame-time feed is changed (no adaptive interpolation count) | `--game PBZ.exe --den 1 --pool 4 --interp 3 --fg-rt-mode median --unlock-mfg --fg-ui none --fg-proj-rz --fg-fov 58.7 --no-mv` | 动态倍率各版本；定稿改固定倍率（§9.19） |
| `run_mgpu_pbz_dyn_vrr.bat` | PBZ with VRR/tearing: the only real fix for an input cadence that never divides the refresh rate | `--game PBZ.exe --den 1 --pool 4 --interp 3 --fg-rt-mode median --tearing --unlock-mfg --fg-ui none --fg-proj-rz --fg-fov 58.7 --no-mv` | 动态倍率各版本；定稿改固定倍率（§9.19） |
| `run_mgpu_pbz_dynmfg.bat` | PBZ: OptiScaler-style Dynamic MFG (auto 1x..6x) Reference: OptiScaler/framegen/xefg/XeFG_DynamicMFG.cpp + XeFG_D | `--game PBZ.exe --den 1 --pool 4 --interp 5 --dyn-mfg --fg-rt-mode median --unlock-mfg --fg-ui none --fg-proj-rz --fg-fov 58.7 --no-mv` | 动态倍率各版本；定稿改固定倍率（§9.19） |
| `run_mgpu_pbz_dynmfg2.bat` | PBZ: Dynamic MFG with headroom for the loop Why this variant (measured 2026-11-07, bridge_pbz_dynmfg.txt): | `--game PBZ.exe --den 1 --pool 4 --interp 3 --dyn-mfg --fg-target 110 --fg-round 0 --fg-rt-mode median --unlock-mfg --fg-ui none --fg-proj-rz --fg-fov 58.7 --no-mv` | 动态倍率各版本；定稿改固定倍率（§9.19） |
| `run_mgpu_pbz_of_hi.bat` | PBZ 4K with HIGH-RESOLUTION depth/MV streams Requires [capture] RenderWidth=3840 RenderHeight=2160 in game_captur | `--game PBZ.exe --den 1 --pool 4 --interp 3 --unlock-mfg --fg-ui none --fg-of --fg-jitter-zero --fg-proj-rz --fg-fov 58.7` | 光流实验；2026-11-08 复核：MV 确实被消费 |
| `run_mgpu_pbz_of_jitmv.bat` | run_mgpu_pbz_of_jitmv.bat JITTERED_MV flag: our field is measured on jittered frames | `--game PBZ.exe --den 1 --pool 4 --interp 3 --unlock-mfg --fg-ui none --fg-of --fg-jittered-mv --fg-proj-rz --fg-fov 58.7` | 光流实验；2026-11-08 复核：MV 确实被消费 |
| `run_mgpu_pbz_of_jz.bat` | run_mgpu_pbz_of_jz.bat jitter forced to 0 (our colour is already resolved by DLSS) | `--game PBZ.exe --den 1 --pool 4 --interp 3 --unlock-mfg --fg-ui none --fg-of --fg-jitter-zero --fg-proj-rz --fg-fov 58.7` | 光流实验；2026-11-08 复核：MV 确实被消费 |
| `run_mgpu_pbz_of_jz_only.bat` | run_mgpu_pbz_of_jz_only.bat jitter 0 + only interpolated frames (best judgement view) | `--game PBZ.exe --den 1 --pool 4 --interp 3 --unlock-mfg --fg-ui none --fg-of --fg-jitter-zero --fg-debug-only-interp --fg-proj-rz --fg-fov 58.7` | 光流实验；2026-11-08 复核：MV 确实被消费 |
| `run_mgpu_pbz_of_onlyinterp.bat` | run_mgpu_pbz_of_onlyinterp.bat only interpolated frames are shown (artifacts no longer hidden) | `--game PBZ.exe --den 1 --pool 4 --interp 3 --unlock-mfg --fg-ui none --fg-of --fg-debug-only-interp --fg-proj-rz --fg-fov 58.7` | 光流实验；2026-11-08 复核：MV 确实被消费 |

## 说明

- `_fix_bridge_dup.py`（一次性源码修补脚本）也一并移除。
- 这些脚本里的 `--mv-newest`、`--fg-hi-res-mv`、`--fg-jittered-mv`、`--fg-of-*` 在当前
  bridge 里已不存在或是 no-op；要复现实验请按 `MGPU_FG_OF` 宏的说明重新编译。
- 需要脚本本体时看 git 历史：`git show 0be8b10:scripts/archive/<名字>.bat`。
