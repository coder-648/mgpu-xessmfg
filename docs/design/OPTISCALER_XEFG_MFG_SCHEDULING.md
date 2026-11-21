# OptiScaler 是怎么"调度"XeSS MFG 的，以及我们的 3x/4x 伪影该怎么拆

最后更新：2026-11-22。对象：`third_party/OptiScaler-master/OptiScaler/framegen/xefg/`、
`inputs/FG/XeFG_Inputs_Dx12.cpp`；对照 `lock_frame/modules/{040_cadence,140_frame,030_devices_capture_fg}.inc`
与 `src/faker_fg/backend_xefg.cpp`。

> **一句话结论**
> OptiScaler 对多帧**没有模型层的调度**——它做的只有三件事：
> ① init 时把 `maxInterpolatedFrames` 设成**上限**；② 每次 Present 前 `Dispatch()` 下发**当前 count**；
> ③ 可选的两个闭环控制器（`XeFG_DynamicCap` / `XeFG_DynamicCount`）决定这个 count。
> 除 count 之外能影响 3x/4x 画质的，只有**帧常量**（jitter / mvScale / near-far / 投影 / frameRenderTime /
> reset）和**输入装配**（MV/depth 尺寸、插值矩形、camera-motion fill）。
> 所以"OptiScaler 的 MFG 更干净"通常不是调度更好，而是**有效 count 更低**（它用 limiter 把 presented
> 帧率压到 target，DynamicCap 只取"够用"的最小 count）+ 它做了逐引擎的投影/远平面修正。

---

## 1. OptiScaler 的三个调度层（代码逐条）

### 1.1 建链：init 只写"上限"，不写"当前倍率"

`XeFG_Dx12::CreateSwapchain1Internal`（`XeFG_Dx12.cpp:464-486`）：

```cpp
int intTarget = _maxInterpolationCount;              // GetProperties 的上限
if (XeFGProxy::SetNumInterpolatedFrames() == nullptr) // 旧库只能靠 init 定值
    intTarget = Config::Instance()->FGInterpolationCount(...);
params.maxInterpolatedFrames = intTarget;            // 写的是上限
```

然后 `Dispatch()` 每帧再落真正的 count（`:729-777`）。SDK 语义（`xefg_swapchain_d3d12.h:41`）是
"**不会超过**这个数"，初始化后的默认产出就是该上限（`xefg_swapchain.h:465`），所以初始化后到
第一次 `SetNumInterpolatedFrames` 之间，库按上限出帧。

**与我们对照**：`lock_frame/modules/040_cadence.inc:1129`（Desc 分支）与 `:1273`（InitFromSwapChain
分支）都写 `ip.maxInterpolatedFrames = want_interp`，即**当前倍率**；固定倍率下与 OptiScaler 等价，
但 `--dyn-mfg` / `--interp-auto` 一旦启用，升档会被库以 `INVALID_ARGUMENT` 拒绝（`TRUE_MFG_OPTIONS.md`
§8.3 已记录）。落地写法见该文档 §8.3 / `MFG.md` §7.7。

还有一个只在 MFG 下才重要的细节（`XeFG_DynamicCount.h:10-11` 的注释）：
**1↔2 的跨越会在库内部重建（shader 编译/资源重分配），2 以上互切是下一帧生效**。这解释了为什么
我们对 `SetEnabled(0)→(1)` 的反复开关比"改 count"贵得多（见 §3.D）。

### 1.2 每帧：`Dispatch()` = 帧常量 + `SetPresentId`，顺带改 count

`XeFG_Dx12::Present()`（`:1265-1367`）每次 Present 调 `Dispatch()`；`Dispatch()` 里：

| 顺序 | 动作 | 行号 |
|---|---|---|
| 1 | `GetDispatchIndex(willDispatchFrame)`，`Depth`/`Velocity` 未就绪则**直接 false**（不关 FG，只这一帧不出） | `:687-703` |
| 2 | UI composition 变化 → `WAR_xefgRequestFGToggle` | `:707-727` |
| 3 | `targetCount = clamp(config count, 1..max)` | `:738-740` |
| 4 | `_dynamic.Update(targetCount, _framesToInterpolate, _generationOff, state.lastFGPresentMs, hwnd)` | `:744-745` |
| 5 | 1X 切换 → `SetGenerationOff()`（`SetEnabled(0)`，**count 保留**） | `:750-751`, `:1006-1023` |
| 6 | count 变了 → `SetNumInterpolatedFrames(targetCount)`；**动态控制器自己的变化不 toggle**，其余变化置 `WAR_xefgRequestFGToggle` | `:755-777` |
| 7 | `WAR_xefgRequestFGToggle` → `Deactivate()` + `UpdateTarget()`（暂停 10 帧） | `:779-787` |
| 8 | `TagFrameConstants`（view/proj/jitter/mvScale/reset/frameRenderTime） | `:841-929` |
| 9 | `SetPresentId(frameId)` | `:931-941` |
| 10 | 非 Upscaler 车道才 tag `BACKBUFFER`（插值矩形） | `:944-991` |

**与我们对照**：consumer 的每帧顺序 `TagFrameResource(MV/depth)` → `TagFrameConstants` → `SetPresentId`
→ `Present`（`140_frame.inc:1042-1067, 1433-1445`）与之一致；本地链路在 Evaluate 里 tag 输入、Present
里发常量与 id（`backend_xefg.cpp:1484-1533, 395-415`）。**顺序上没有差异**。

差异在"变了之后怎么办"：我们没有 `WAR_xefgRequestFGToggle` 这一层（`030_devices_capture_fg.inc:428-451`
直接调 `SetNumInterpolatedFrames`，失败只打日志）。固定倍率无影响，动态启用时要补。

### 1.3 帧常量里的三个"逐引擎修正"（最容易被忽略，也最像"模型伪影"）

`XeFG_Inputs_Dx12::TagFrameConstants`（`XeFG_Inputs_Dx12.cpp:430-534`）做了三件我们没做的事：

1. **near/far 从投影矩阵反算**，而不是信游戏的 near/far 参数：
   `nearZ = d/c`、`farZ = d/(c±1)`（`:449-459`）；
2. **无限远平面按引擎改写**：UE 系列拿到 `farZ == 0` 时**不**用无限远，改成 `75000.0f`
   （`:464-478`，注释直接写 "Unreal's own DLSSG integration sends this one instead"）；
3. **单位自适应的 `frameRenderTime`**：前 10 帧观察，`<1` 判为秒并 ×1000（`:296-315`）；
   注释点名 CP2077 / The Witcher 3 传的是**秒**。

再往下才是 jitter / mvScale / reset（`:510-531`）：

```cpp
if (CameraMotionFill())                      { mvScaleX = 1.0f; mvScaleY = 1.0f; }
else if (initFlags & USE_NDC_VELOCITY)       { mvScaleX *= _mvSize.x * 0.5f;
                                               mvScaleY *= _mvSize.y * -0.5f; }
fg->SetJitter(...); fg->SetMVScale(...); fg->SetReset(...); fg->SetFrameTimeDelta(...);
```

**与我们对照**：

| 项 | OptiScaler | 我们（consumer） | 影响 |
|---|---|---|---|
| near/far | 从 projection 反算 + UE 强制 75000 | `140_frame.inc:1202-1284`：producer 相机或 `--fg-fov/--fg-near-z` + reversed-Z 近似；`--fg-use-matrix` 默认关 | 远景视差，**误差越往边缘越大** |
| `frameRenderTime` | 输入帧时间，秒→ms 自动 | `fg_render_time_ms()`：接受帧间隔（丢帧时是 2T），可选模式 | 库的批次铺法 |
| NDC mvScale | 按 **tag 时的 MV 尺寸** ×(w/2, −h/2) | producer 声明 + `USE_NDC_VELOCITY`（`040_cadence.inc:1138`） | 尺寸不一致时全场偏移 |
| camera motion | `CameraMotionFill` quirk：自己算 reprojection + 计算着色器填 MV | 无 | 游戏 MV 为 0 处的边缘 |

### 1.4 动态 MFG：两套控制器，本质是"闭环 presented fps"

`XeFG_DynamicMFG::Update`（`XeFG_DynamicMFG.cpp:8-141`）只采集三样东西：

```cpp
const double sleepMs   = InputCommon::take_sleep_ms();     // 在 OptiScaler 里睡掉的
const double waitMs    = sleepMs + presentWaitMs;          // 低延迟/限帧 sleep + Present 阻塞
const auto   refresh   = DisplayRefreshFps(now, window);
const auto   target    = TargetFps(now, window);           // 目标 = min(FG target, FPS limit)，缺省 floor(R - R²/3600)
```

- `XeFG_DynamicCap`（`:41-187`，"Use target as cap"，推荐）：因为 OptiScaler 的限帧器把
  **presented** 帧率钉在 target，count 只需要"够高"，于是它取**能撑住 target 的最低 count**。
  - Up：presented < 97% target 持续 0.3 s；
  - Down（有证据）：`reachable * count >= target * 1.04` 持续 1.5 s；
    `reachable = 1000 / (frameMs - sleepMs)`（`:59, :77`）；
  - Down（无证据，被 V-Sync/外部限帧钉住）：按 back-off 试探（`:163-184`）；
  - 每次变更后：跳过 **3 个在途帧**，至少 6 帧 / 0.4 s 再平均，1.5 s 内做结论；
    不达标就 **Undo**，并让 back-off 翻倍（5 s→60 s）（`:85-119`）。
- `XeFG_DynamicCount`（`XeFG_DynamicCount.h:6-61`）：未被 cap 时用 presented 帧率选"最接近 target 的 count"，
  另有一套 band/undo/学习"每张生成帧的 GPU 代价"的规则（要求每张生成帧至少挣回 30% 的增益）。

**与我们对照**：我们的 `cadence_core` 是 **f→k 的开环算术**（`DYNAMIC_MF_ANALYSIS.md` §4.1），
信号是输入帧间隔；`optimal` 那条路径甚至拿"我们的捕获间隔"当闭环量（同文档 §8.6 已判为错误信号）。
OptiScaler 两条控制器的共同点是：**控制量是 presented fps，约束量是 reachable（去掉 sleep 的工作时间）**，
而 count 变更一律带"在途帧 + 观察窗 + undo + back-off"。

> 这一条对画质有直接含义：**count 抖动本身就是伪影源**。每次从 3x 掉到 2x 再回 3x，那一批的
> 时间几何（t 的分配）都变一次；keep2x 模型下 t≠0.5 的输出本来就更脏，**只会在换档那几帧被看到**。
> 我们的 cadence 有 ~8 次/窗的换档记录（`DYNAMIC_MF_ANALYSIS.md` §6.7），OptiScaler 的最短换档间隔
> 是 0.5 s（`kMinUpGapMs`）/ 2 s（`kMinDownGapMs`），差距是数量级。

### 1.5 激活/暂停的门控

`TagFrameConstants` 里顺带 `fg->EvaluateState()`，而 `Activate()` 只在
`_enabled && cameraKnown && FGEnabled && !IsActive() && !IsPaused()` 时发生（`XeFG_Inputs_Dx12.cpp:492-505`）。
`Dispatch()` 自己在 `IsPaused()` 时**直接返回 false**（`:691`），不关 FG。

**与我们对照**：`backend_xefg.cpp:1556-1633` 复刻了 `Activate()` 的四个条件与 `UpdateTarget`(=10 帧暂停)；
consumer 侧的 MV gate 是"MV 缺失 → `SetEnabled(0)`，回来 → `SetEnabled(1)`"（`140_frame.inc:960-1006`）。
后者比 OptiScaler 激进得多：OptiScaler 在输入不齐时**只是这一帧不 dispatch**，不会把 FG 关掉。
每次 `SetEnabled(0/1)` 都要重新积累历史，而且 **1→2 的跨越在库内部是重建**（§1.1）。
菜单/加载/瞬断时反复开关，会周期性出现"孤立真实帧 + 下一批节奏错位"，在 3x/4x 下更像模型伪影。

---

## 2. 边界：OptiScaler 也解决不了什么

- **模型层**：它同样只有"解锁补丁"这一条路（`[XeFG] UnlockMFG`，和 `xefg_unlock.h` 同一套 6 站点）。
  `fg_keep_2x_model` 打开时，**每一张生成帧都是 2x 模型**（`MFG.md` §1、§7.1）。
  OptiScaler 没有 model 15 的真多帧，也不做"一次光流 N 次合成"。
- **真正的 true MFG 只有三条路**（`TRUE_MFG_OPTIONS.md` §3）：Intel 副卡 / 换 build / 移植 sm86 引擎。
  我们实测 model 15→16 的 CP2077 A/B **边缘伪影更重**（`TRUE_MFG_OPTIONS.md` §8.2），
  所以"换真模型就能修边框"这个假设目前**没有证据**。
- **插值矩形**：OptiScaler 用 HUDLESS/`BACKBUFFER` tag 的 size 设 `SetInterpolationRect/Pos`
  （`XeFG_Inputs_Dx12.cpp:549-557`）。我们 consumer 不 tag `BACKBUFFER`，只在 ui-unblend 时 tag
  HUDLESS，尺寸取 `draw_w/draw_h`（= 链缓冲尺寸，`140_frame.inc:838, 1078`），所以**正常情况下矩形
  就是整幅**；只有当链尺寸与内容区不一致（`--scale` / `--den` / letterbox）时才可能留下"没被插值的边框带"。
  这是一条**可验证项**，不是默认主因。

---

## 3. 伪影归因（按证据强弱，逐条给验证方法）

### A. 模型复用（最强证据，且**改不了**）

3x/4x 走 `fg_keep_2x_model`：库的多帧循环对每张生成帧复用 2x 模型。t≠0.5 时边缘/遮挡判定的误差
本来就更明显，且**每多一张就多一次**。这是 `MFG.md` §7.1 的结论，也是 4x 观感"更稀"的来源。

- 判据：同一场景 4x 的 t=1/2 帧与 2x 生成帧**逐位相同**（`TRUE_MFG_OPTIONS.md` §1）。
- 工具：`EnableDebugFeature(SHOW_ONLY_INTERPOLATION)` 只看生成帧；再开 `TAG_INTERPOLATED_FRAMES`
  看每张的标记（`backend_xefg.cpp:1043-1055` 已接，`--debug-*`）。
- 若三张生成帧的边缘误差**随 t 单调变化**，就锁定是模型层，不要再去调 jitter/FOV。

### B. 几何与帧常量（边框伪影最可能的**可修**来源）

`HANDOFF.md` §0.1 第 2 条已给出判据：**"投影误差是中心 0、越往边缘越大，所以只剩边缘伪影就等于 FOV 不对"**。
MFG 把这条放大了 k−1 倍（k 张生成帧各自在不同的 t 上做同一套错误重投影）。

需要逐项核对：

| 量 | 现状 | 风险 |
|---|---|---|
| FOV / aspect | `--fg-fov`（默认合成值）或 `--fg-use-matrix` 反算 | 2x 上验证过的 58.7 不一定对每台机器 |
| near / far | `--fg-near-z` + reversed-Z 近似；infinite far 见 `140_frame.inc:1202-1210` | **UE 的无限远**：OptiScaler 强制 75000，我们保留 infinite |
| 深度方向 | `INVERTED_DEPTH` 来自 producer flags | 错一侧 = 全场错，但边缘最明显 |
| NDC velocity | `USE_NDC_VELOCITY` + `mv_scale_y` 取负（`OPTISCALER_QUIRKS.md` §3.1） | 漏 flag 时速度小 2× |
| mvScale 与 tag 尺寸 | 用 stream 尺寸；`--upsample` 会同时改 MV/depth 尺寸 | 两者不一致 = 全场偏移 |
| jitter | producer 原值，**不要传 0**（`FINAL_CONFIG.md:237`：传 0 → 闪烁 + 模型边缘伪影） | — |
| reset | `kInputFlagResetHistory`；没有时靠库自己的 scene threshold 0.7 | 场景切换后 k 张都吃旧历史 |

### C. 节奏（看起来像模型伪影，实际是 pacing）

- `frameRenderTime` 决定库把 k+1 张铺在哪个时长里。consumer 用**接受帧间隔**（丢帧时 2T），
  local 路径用**生成关闭时测的常数**（`backend_xefg.cpp:1242`）。两者在不同档位下含义都成立，
  但**从未验证过库的铺法**（`DYNAMIC_MF_ANALYSIS.md` §8.3 明确列为空白）。
- 若铺法与我们的 vblank 预算不一致，3x/4x 会呈现"周期性一顿"，边缘视觉上正好是"抖一下"。
- 验证：固定 k=3，把 `frameRenderTime` 从 0.8T 扫到 1.2T（`--fg-rt` / `--fg-rt-mode`），
  看 `presents/vblanks` 与边缘伪影是否相关。**相关 = pacing，不是模型。**

### D. 输入配对 / 门控抖动

- consumer 的 MV gate 在 pair miss 时 `SetEnabled(0)`，回来再 `(1)`（`140_frame.inc:960-1006`）；
- OptiScaler 的等价情形只是**跳过这一帧的 dispatch**（`XeFG_Dx12.cpp:696-703`），FG 保持开启；
- 每次 `SetEnabled(0/1)` 都清历史；1→2 还会在库内部重建；
- 结果：菜单/加载/瞬断附近出现"孤立真实帧"，随后第一批几何错位 —— 在 3x/4x 上被当成模型伪影。
- 验证：统计一窗内 `SetEnabled` 次数与 `xN` 分布；若 `xN` 在 3/1 之间跳，先修门控而不是修画质。

### E. 换档过于频繁（把 A 的代价反复暴露）

- 我们 cadence 的换档间隔约每 5 帧一次（`DYNAMIC_MF_ANALYSIS.md` §6.7 实测 8.17/窗）；
- OptiScaler 最短 0.5 s up / 2 s down，且带 undo/back-off；
- 每换一次档，keep2x 的 t 分配就变一次 —— **伪影出现在换档那几帧**，肉眼读成"3x 一直在抖"。
- 验证：固定倍率（关 `--interp-cadence/--dyn-mfg`）跑一轮；若伪影显著减少，就是换档放大。

---

## 4. 建议的动作顺序（低成本 → 高成本，每步都能单独出结论）

| # | 动作 | 成本 | 判据 |
|---|---|---|---|
| 1 | 固定倍率（无 cadence/无 dyn）跑 2x/3x/4x 各一轮，开 `SHOW_ONLY_INTERPOLATION` | 三次跑 | 生成帧是否有边缘误差、是否随 t 变 |
| 2 | `frameRenderTime` 扫描（0.8T/1.0T/1.2T，`--fg-rt`/`--fg-rt-mode`） | 三次跑 | 边缘伪影随 rt 变 ⇒ pacing |
| 3 | ~~FOV/near/far 扫描~~ → **已落地**（见 §7）：near/far 现在从游戏矩阵派生；A/B 用 `--fg-far-finite 75000\|auto` + `--fg-fov` | — | `near/far:` 日志行确认来源与无限远处理 |
| 4 | ~~MV gate 去抖~~ → **已落地**（见 §7）：`--fg-gate N` / `--fg-gate-up M` | — | `holding FG on` 与 `MV back -> FG on` 的次数、`xN` 分布 |
| 5 | 若开 ui-unblend：确认 HUDless 的 `resourceSize` == 链尺寸，或显式 tag `BACKBUFFER` 整幅 | 小改 | 边框带是否消失（关 unblend 对照） |
| 6 | `--fg-scene 0.8` A/B（HANDOFF §0.1 记录的"换掉边框糊"） | 一次跑 | 边框糊 vs 平滑度 |
| 7 | 动态多帧真正启用前：init 改上限（§1.1）；count 变化补 `SetEnabled(0→1)`；换档规则换成 OptiScaler 的"在途 3 帧 + 观察窗 + undo + back-off" | 中 | 换档次数降到 /s 级以下 |
| 8 | 产品层：3x/4x 只承诺帧率，不承诺单帧画质；优先 2x@60（`MFG.md` §7.4） | 文案 | — |

**不要先做的**：换 model 15（实测边缘更差）、加 `--no-upsample`、改 jitter 符号
（`TRUE_MFG_OPTIONS.md` §8.2 已 A/B 过，都没改善）。

---

## 5. 还需要确认的三件事

1. **keep2x 下库给每张生成帧的 `t` 是不是不同**（都按 0.5 还是 i/(N+1)）。
   这决定"B 类修正"能否缓解 A 类伪影：如果 t 全 0.5，几何修正的意义就只剩对齐；如果 t 不同，
   near/far/FOV 的精度在 3x/4x 下比 2x 重要得多。用 `SHOW_ONLY_INTERPOLATION` + 跳帧真值可以直接看出。
2. **`frameRenderTime` 的真实铺法**（`DYNAMIC_MF_ANALYSIS.md` §8.3 的空白；OptiScaler 也只做"传输入帧时间"，没有反推过）。
3. **XeLL generated-frame 计数与 count 是否同步**（解锁第 6 站 `ll_generated_gate`）。
   若不同步，低延迟 sleep 的预算与真实批次不符，表现为节奏抖动 —— 也会被误读成模型伪影。

---

## 6. 参考位置一览

| 主题 | 文件:行 |
|---|---|
| init 上限 / 当前 count | `XeFG_Dx12.cpp:335-355, 464-486, 729-777` |
| 每帧 Dispatch 顺序 | `XeFG_Dx12.cpp:682-996` |
| Present 的 >3 掉线规则 | `XeFG_Dx12.cpp:1265-1367` |
| 1X（`SetGenerationOff`） | `XeFG_Dx12.cpp:1006-1023` |
| Activate/Deactivate 门控 | `XeFG_Dx12.cpp:583-655` |
| 动态控制器（cap / count） | `XeFG_DynamicCap.h:41-214`、`XeFG_DynamicCount.h:6-61`、`XeFG_DynamicMFG.cpp:8-141` |
| 帧常量 / near-far / UE 75000 / 秒→ms | `XeFG_Inputs_Dx12.cpp:296-315, 430-534` |
| 插值矩形 / NDC / camera-motion fill | `XeFG_Inputs_Dx12.cpp:200-294, 536-608` |
| 我们的 init / count / scene | `040_cadence.inc:1129, 1138, 1273, 1349`、`030_devices_capture_fg.inc:428-451` |
| 我们的 tag / 常量 / 门控 | `140_frame.inc:960-1088, 1288-1445` |
| 我们的本地链路 | `src/faker_fg/backend_xefg.cpp:1173-1268, 1484-1633` |
| 伪影历史结论 | `docs/findings/HANDOFF.md:50-63`、`MFG.md` §7、`TRUE_MFG_OPTIONS.md` §8.2 |

---

## 7. 落地记录（2026-11-22）

改的是发布路径 `lock_frame/`（`src/xess_mfg_consumer` 是 v22 旧包，按约定不随主版本同步）。

### 7.1 新开关

| 开关 | 默认 | 作用 |
|---|---|---|
| `--fg-gate N` | `0`（= 旧行为） | 连续 N 帧"只是缺 MV 流"时**不关 FG**：生成保持开启，但这一帧不给 XeFG 任何输入、也不发 present id（OptiScaler `Dispatch()` 的 early-out 形状）。第 N+1 帧仍缺才 `SetEnabled(0)`。**配对失败（`pair_ok=0`）永不宽限**。 |
| `--fg-gate-up M` | `1`（= 旧行为） | 需要 M 帧连续有 MV 才 `SetEnabled(1)`，避免 hit/miss 交替时每帧开关。 |
| `--fg-far-finite <Z\|auto>` | `0`（= 保留无限远） | 仅当 far 为无限（producer 声明或矩阵派生）时，把它换成有限值并切到有限投影。`auto` = 75000，即 OptiScaler 对 UE 的做法。 |

日志：

```text
[lock] XeFG tuning: rt=input ui=auto scene=0.00 depthInv=0 jitteredMV=0 (...) gate=3/2 farFinite=75000
[lock] MV missing this frame -> holding FG on (1/3 tolerated) [pair=1 producer_mv=0 of=0/0]
[lock] MV back -> FG on (OK, 0) [2 consecutive]
[lock] near/far: zn=0.100 zf=inf (infinite) (derived from the game's view-to-clip), Unreal shipping build (camera=0 matrix=1 farFinite=75000)
```

### 7.2 near/far：默认行为确实变了（但没有开关）

`140_frame.inc` 的投影段现在按三条来源取值：

1. producer 声明的相机（FSR 3.1 有）；
2. **没有相机但拿到了新鲜的 `view_to_clip`** → 用 `derive_near_far_from_matrix()`
   （`020_state_overlay.inc`，公式取自 `XeFG_Inputs_Dx12.cpp:449-462`）派生：
   - 标准透视：`near = d/c`、`far = d/(c±1)`；
   - **reversed-Z + 无限远**（`m[10]==0 && m[11]==-1 && m[14]>0`）：`near = m[14]`、`far = ∞`；
   - 形状/数值不合理则拒绝，继续用 `--fg-zn` / 10000。
3. `--fg-far-finite` 再把无限远换成有限值。

第 2 条是**唯一默认改变行为**的部分：以前"没有相机"一律 `zn=10, zf=10000`（对 reversed-Z
标题是错的），现在优先用游戏自己的矩阵；判据是 `near/far:` 日志里的
`(derived from the game's view-to-clip)`。若想回到旧行为，`--fg-no-proj` 会关掉矩阵路径。

### 7.3 为什么 `--fg-gate` 默认 0

"生成开 + 不喂输入"是**未经实机验证**的状态：库可能把本帧当成"无新输入、只出真实帧"
（预期，也是 OptiScaler 的行为），也可能复用上一份输入生成一张重复帧（那就比现在更差）。
所以先保持旧行为，A/B 用：

```bat
:: 宽限 2 帧、回来要 2 帧连续（菜单/加载/瞬断的抖动应该消失）
lock_frame.exe --interp 1 --fg-gate 2 --fg-gate-up 2 ...

:: 对照：旧行为
lock_frame.exe --interp 1 ...
```

观察三件事：`holding FG on` 是否只在菜单/加载出现；`MV back -> FG on` 的次数是否显著下降；
`xN` 是否还在 3/1 之间跳。若出现"重复帧"或 `xN` 反而更差，去掉 `--fg-gate` 即可完全回滚。

**已知反例（重要）**：本地链路曾实测"presentId 跳号 → 库回 `SetPresentId` OK 但不再出帧"
（`src/faker_fg/backend_xefg.cpp:433-438`；那次是多个消费者共用一个计数器，跳幅远大于 1）。
上游 OptiScaler 的 early-out 也是"消耗 id 但不 `SetPresentId`"，所以这里照上游形状实现；
如果 `--fg-gate` 之后看到 `framesPresented` 掉到 1、`fgEnabled=1`、`xN=1.00` 持续，
先退回 `--fg-gate 0`（那说明这份库不接受跳号，只能换成"宽限帧照样 `SetPresentId`、
只跳过 tag"的另一种形状再试）。

### 7.4 改动位置

| 文件 | 改动 |
|---|---|
| `lock_frame/modules/020_state_overlay.inc` | `--fg-gate/-up/-far-finite` 状态；`derive_near_far_from_matrix()` |
| `lock_frame/modules/140_frame.inc` | MV gate 去抖 + 宽限帧跳过 `TagFrameConstants`/`SetPresentId`；near/far 三来源 + `near/far:` 日志 |
| `lock_frame/modules/160_main.inc` | 开关解析；`g_game_is_unreal`（UE shipping 检测处）；`XeFG tuning:` 行加 `gate`/`farFinite` |

没有改 `src/faker_fg/backend_xefg.cpp`（本地 XeFG 链路的 near/far 来自 upscaler 参数块，
不存在这次的合成投影问题）。

---

## 8. 第一次 A/B 日志（2026-11-22，CP2077 4K，120 Hz）

7 条日志，全部 `pair exact=40/40`、`xN` 与倍率一致、0 `MISMATCH`、0 device removed。

| 日志 | 输入 | 窗口 | presented | xNmiss | gpu p90 | total p90 | load p90 | show p50 |
|---|---|---|---|---|---|---|---|---|
| ab_01 2x baseline | 60 | 32 | 2 (31/32) | 1 | 4.2 ms | 14.1 ms | 77% | 119 |
| ab_02 3x baseline | 40 | 19 | 3 (18/19) | 1 | 7.3 ms | 17.4 ms | 65% | 120 |
| ab_03 3x gate | 40 | 21 | 3 (20/21) | 1 | 10.0 ms | 18.5 ms | 73% | 120 |
| ab_04 3x far | 40 | 19 | 3 (18/19) | 1 | 11.1 ms | 17.8 ms | 70% | 120 |
| ab_05 3x gate+far | 40 | 18 | 3 (17/18) | 1 | 10.7 ms | 20.0 ms | 79% | 120 |
| ab_06 4x baseline | 30 | 15 | 4 (14/15) | 1 | 14.2 ms | 25.2 ms | 72% | 119 |
| ab_07 4x gate+far | 30 | 15 | 4 (14/15) | 1 | 15.8 ms | 24.8 ms | 74% | 120 |

（"1 个窗口 presented 偏小" 每次都是启动窗口；`show<110` 也只出现在启动窗口。）

### 8.1 结论一：#4 的测试没有触发 —— CP2077 根本没有 MV 抖动

7 条日志里 `MV missing this frame -> holding FG on` 与 `-> 1X` 都是 **0**。
也就是说这条标题从进游戏到退出，**MV 一次都没缺过**，门控没有开合，
`--fg-gate` 这次是纯空转（它只能在菜单/加载/断流时才起作用）。

含义：
- 之前假设的"MV gate 反复开关放大伪影"在 CP2077 上**不成立**，至少这次没有发生；
- `--fg-gate` 保留为安全网（默认仍是 0），但它不能解释这次的边框/模型伪影；
- 要验证它需要"确定性缺 MV"（菜单中段/加载屏，或加一个仅供测试的强制注入开关）。

### 8.2 结论二：#3 实际没生效 —— 派生函数拒绝了 CP2077 的矩阵（已修）

每条日志都是：

```text
[lock] near/far: zn=10.000 zf=10000 (camera=0 matrix=1 farFinite=75000)
```

`matrix=1` 但**没有** `(derived from the game's view-to-clip)`，说明
`derive_near_far_from_matrix()` 返回了 false，`--fg-far-finite` 也就没有可替换的对象
（它只在 far 为无限时生效）。原因在日志里的矩阵（ab_02:109）：

```text
[+1.1792 +0.0000 +0.0000 +0.0000]
[+0.0000 +2.0963 +0.0000 +0.0000]
[+0.0000 +0.0000 -0.0000 +1.0000]
[+0.0000 +0.0000 +0.0200 +0.0000]
```

w 在 **m[11] = +1**、near 在 **m[14] = 0.02**、`m[10] = 0` —— 标准的 reversed-Z 无限远，
但 w 是 `+1` 而不是 `-1`。第一版 helper 抄了 OptiScaler 的 `e < 0` 分支，于是把它判成非法，
静默退回 `zn=10 / zf=10000`。**CP2077 真实 near 是 0.02，我们一直在喂 10（差 500 倍）；
真实 far 是无限，我们喂的是 10000 的有限正向投影。**

已改成与符号/行列布局无关的推导（`020_state_overlay.inc`）：

```text
depth(z) = (c*z + d) / (e*z)      c=m[10], d=m[14], e=m[11]
c == 0                     -> near = d/|e|, far = ∞
c != 0                     -> |z0| = |-d/c| (depth 0), |z1| = |d/(e-c)| (depth 1)
                              near = min(|z0|,|z1|), far = max(...)
```

用这条真实矩阵做的单测：`zn=0.0200 zf=inf`（旧版：拒绝）。修完重新构建并已覆盖部署
（旧 exe 备份 `lock_frame.exe.bak_farnf_*`），日志会变成：

```text
[lock] near/far: zn=0.020 zf=inf (infinite) (derived from the game's view-to-clip) (camera=0 matrix=1 farFinite=0)
[lock] near/far: zn=0.020 zf=75000 (derived from the game's view-to-clip) (camera=0 matrix=1 farFinite=75000)
```

### 8.3 但"修好了也不一定改变画面" —— 这条库可能不读投影

`FINAL_CONFIG.md` §5.16 的实测（PBZ 2x）：`--fg-fov 30/58.7/120` 三组画面**完全一致**，
`--no-depth` 画质也不变，结论是 `libxess_fg 1.3.1` 的 FG 只用 MV 重投影，不读深度、不读投影矩阵。
如果这条结论也适用于 CP2077，那么 #3 的价值是：

- 日志/语义正确（不再用 10/10000 这种假数据）；
- 对 `--mfg-model=port`（model 15 真多帧）或其它 SDK 版本有意义；
- **不会**修掉 keep2x 的边框/模型伪影。

所以重跑 02 / 04 / 05 / 07 的判据是"画面是否**完全没变**"：
不变 ⇒ 投影这条路对 keep2x 到此为止；变了 ⇒ 说明这条 build 会读投影，需要单独 A/B。

### 8.4 下一步

| # | 动作 | 目的 |
|---|---|---|
| 1 | 重跑 `ab_02`（对照）/ `ab_04` / `ab_05` / `ab_07`（新 exe 已就位） | 确认 `near/far:` 出现 derived+infinite，并看画面是否变化 |
| 2 | `--no-depth` 一轮（3x） | 深度每帧 7.9 MB + 一次 tag；若库不读深度，这是白付的渲染卡成本 |
| 3 | `--fg-jittered-mv` 强制一轮（3x） | producer 声明 `jittered=0`；若 MV 实际带 jitter，边缘会糊 |
| 4 | 若要验证 #4：加"确定性缺 MV"测试开关（每 K 个 present 假装缺 2 帧）或在菜单/加载处抓一段 | 验证跳号后库是否仍出帧（当前完全没被测到） |

## 9. 第二轮：model 15（port 路线）实测（2026-11-22）

`--mfg-model=port --unlock-mfg --interp 2`（3x），6 条日志。**6/6 都真的走了 port**：

```text
xefg-mfg: shader_port_applied "programs":54,"replacements":5
xefg-mfg: model_port_applied "donor_model":15,"target_model":16,"edits":167, ...
--unlock-mfg: ported MFG model route active (model 15 -> 16, 5 sites, no fg_keep_2x_model)
```

运行时 `shader_supplied` 28 条（keep2x 轮次是 0）——替换后的程序集确实被库请求了，
`shader_native_lookup` 28 条。AMD 上按预期保留两个 Intel U-Net（`aa016078`/`efbe3788`）。

| 日志 | 配置 | 结果 |
|---|---|---|
| ab_10 | port 基准 | xN=3.00、presented=3、0 error、gpu p90 10.6 ms、total 17.7 ms |
| ab_11 | + far-finite | 同上；**far 有限化未触发**（见 §9.4） |
| ab_12 | + `--no-depth` | xN=3.00、presented=3、0 error；bus 7.9 MB/frame |
| ab_13 | + `--fg-jittered-mv` | **生成完全死掉**（见 §9.2） |
| ab_14 | + `--fg-jitter-flip` | xN=3.00；场景更重（gpu p90 16.9 / load 92%） |
| ab_15 | + `--fg-use-matrix` | xN=3.00；矩阵与合成投影每帧交替（见 §9.3） |

### 9.1 port 链路 = 真多帧模型在跑

结合 `shader_supplied` 的 28 个 key（如 `34510306390392534204181`，8520 B），
可以确认不是"解锁了个空壳"：库在 count=2 时加载的是 model 15 的程序集，
而不是 model 18。keep2x 轮次里这个计数是 0。

### 9.2 `--fg-jittered-mv` 会直接把生成打死（不要再试）

ab_13 的 `initFlags=0x11`（jittered_mv=1）之后：

```text
[xefg error] XeFG: Invalid internal state
[xefg error] Execute called before initialization.
```

27/27 个窗口 `presented=1`、`xN=1.00`、`xNmiss=1041`、`show=40`、`load=7%`、`gpu=0`。
库没有给出可解释的拒绝码，只是**静默退回 1X**。结论：

- producer 声明 `jittered=0` 是对的（`producer semantics` 与 `initFlags=0x1` 一致）；
- 不要对 CP2077 强制 `--fg-jittered-mv`；这也说明"jitter 契约"这条线里，
  **声明位**是被库严格校验的，不能随便改。

### 9.3 `--no-depth` 仍然 3x —— 但只是"生成不依赖"，不等于"画质不变"

ab_12 的日志：

```text
[lock] --no-depth: depth stream skipped, XeFG gets a dummy depth
       (disocclusion quality will drop, transfer halves)
[lock]   bus: mv 1920x1080 7.9MB + depth 1920x1080 (not sent) 0.0MB = 7.9MB/frame
```

24 个窗口全部 `presented=3`、`xN=3.00`、0 error。也就是说 **model 15 不要求真实深度才能出帧**；
但代码自己给的注释是 dummy depth 会降低 disocclusion 质量，所以
"画面是否变化"只能靠眼睛。若没变化，深度这 7.9 MB/frame 可以永久省掉。

### 9.4 CP2077 的 far 是有限的 16777，不是无限

派生结果（每条日志）：

```text
near/far: zn=0.020 zf=16777 (derived from the game's view-to-clip)
```

矩阵 `m[10] ≈ -1.19e-6`、`m[14]=0.02`，所以 `far = 0.02 / 1.19e-6 ≈ 16777`。
**这解释了为什么 `--fg-far-finite auto` 在 CP2077 上永远不生效**：它只在 far 为无限时替换。
OptiScaler 的 UE 75000 规则对这条标题不适用，ab_11 与 ab_10 实质是同一个配置。

### 9.5 发现并修复：`have_matrix` 每隔一帧翻转（投影/FOV/near-far 每两帧跳）

新增的 `near/far:` 日志暴露了一个**一直存在**的问题：

```text
zn=0.020 zf=16777 (derived ...) (camera=0 matrix=1 ...)
zn=10.000 zf=10000               (camera=0 matrix=0 ...)
zn=0.020 zf=16777 (derived ...) (camera=0 matrix=1 ...)
zn=10.000 zf=10000               (camera=0 matrix=0 ...)
```

严格 1:1 交替（ab_10/11/12/15 各 ~50%）。原因在 producer 日志里能对上：

```text
sl: publish calls=1800 ... staged token=3591
sl: present fallback publish #1800 ...
```

**每个游戏帧发布两条记录**（evaluate 时刻 + present 回退），只有其中一条带
sl::Constants；consumer 交替消费到两种记录，于是 `have_matrix` 每帧翻转。
后果：FOV 在 51.01° ↔ 60°、near/far 在 `0.02/16777` ↔ `10/10000`、
矩阵在"真实"↔"合成"之间**每两帧跳一次**。如果库读投影（model 15 有这个可能），
这本身就会造成半帧率周期的抖动/边缘错位；即使不读，它也让 ab_11/ab_15 的对比失效。

修复（`140_frame.inc`）：加一个 2 s 的投影缓存

```text
have_matrix          -> 记录 matrix / fov / aspect / near/far，更新时间戳
没有 matrix 且 <2 s  -> 继续用缓存（日志 matrix=2 cached）
超过 2 s             -> 才退回 --fg-fov/--fg-zn 常数
--fg-use-matrix      -> 缓存存在时也传缓存的矩阵
```

已重新构建并部署（备份 `lock_frame.exe.bak_projcache_*`）。修复后
`near/far:` 日志不应再出现 `matrix=0` + `zn=10.000 zf=10000`。

### 9.6 性能（3x，port）

gpu p90 6.9–10.6 ms、total p90 17.2–17.7 ms、load p90 64–68%，与 keep2x 轮次
（7.3–11.1 / 17.4–18.5）基本一致；ab_14 的 16.9/23.2/92% 更像场景更重，不归因于 jitter 取反。

### 9.7 视觉结论（第二遍，投影缓存修复后）

| 对比 | 结果 |
|---|---|
| ab_10（有深度） vs ab_12（`--no-depth`，dummy 深度） | **有深度更好**：边缘伪影更轻。说明 **model 15 确实消费深度**（model 18 不是这样） |
| ab_10 vs ab_14（`--fg-jitter-flip`） | 边缘基本一致 → jitter 符号不是边缘伪影来源 |
| ab_10 vs ab_15（`--fg-use-matrix`） | 边缘基本一致 → model 15 也不读我们喂的投影矩阵（与 `FINAL_CONFIG` §5.16 一致） |

第二遍日志（04:01–04:02，新 exe）也确认修复生效：每条日志只有 **1 条**
`near/far:`，外加一次 `note: record without a view-to-clip - keeping the last derived
projection`；`matrix=0` + `zn=10/10000` 不再出现。性能：gpu p90 7.2–12.4 ms、
total p90 17.8–19.6 ms、load 69–76%、presented=3、0 error。

### 9.8 由此确定的优先级

深度是 model 15 的**真实输入**，而且现在已经是 1:1 无损（producer 日志：

```text
fr: codec capability: ... -> MV CODEC(1:1), depth CODEC(1:1)
fr: transport plan: mv=codec 1920x1080 depth=codec 1920x1080 colour=off
                    (MvCodec=1 DepthCodec=1 StreamPercent mv/depth=100/100)
```

consumer 侧 `fr: decoded DEPTH -> 1920x1080 (codec 1:1)`），所以**分辨率/精度不是剩下的杠杆**。
下一步只剩三件事：

1. **model 15 vs model 18 的同场景直接对比**（3x，都带深度）。这是决定 3x/4x 默认走哪条路的唯一问题；
2. **深度的时序契约**：`--depth-lag 1`（上一帧深度）。sm86 的引擎自管"上一帧深度"，model 15 可能期望它；
3. **重建 pass 是否改变 MV/depth**：`--no-upsample`。

投影与 jitter 两条线（包括 `--fg-far-finite`、`--fg-use-matrix`、`--fg-jitter-flip`）
到此可以收束：都没有可见收益，保留为 A/B 开关即可。

### 9.9 第三轮：model 15 vs 18 同场景（2026-11-22 04:08–04:12）

| 日志 | 配置 | 窗口 | presented | gpu p90 | total p90 | load p90 | 视觉 |
|---|---|---|---|---|---|---|---|
| ab_20 | 3x keep2x（model 18） | 17 | 3 | 10.9 ms | 19.4 ms | 73% | 参考 |
| ab_21 | 3x port（model 15） | 61 | 3 | **6.3 ms** | **13.8 ms** | **51%** | **略好于 ab_20** |
| ab_22 | 3x port + `--depth-lag 1` | 36 | 3 | 8.7 ms | 18.6 ms | 73% | **比 ab_21 差** |
| ab_23 | 3x port + `--no-upsample` | 19 | 3 | 5.1 ms | 14.9 ms | 53% | 与 ab_21 接近，**21 略好** |

全部 `presented=3`、0 error；port 三条 `shader_supplied=28`，keep2x 为 0。

结论：

1. **3x 上 model 15 优于 model 18**，而且是"更好看且更便宜"（一次光流 + 两次合成 vs 跑两遍 2x 模型）。
   注意 ab_21 跑了 61 个窗口、跨的可能是不同区域，gpu 数字不能完全当同场景比较，但方向明确。
2. **`--depth-lag 1` 更差** ⇒ model 15 期望**当前帧**深度，历史由库自己维护（sm86 的引擎是自管上一帧，
   但移植到 XeFG 后不需要我们喂旧深度）。
3. **`--no-upsample` 中性偏负** ⇒ 重建 pass 留着（默认 ON）。

产品含义：**有 port 资产时，3x 应该走 `--mfg-model=port`**；没有资产时仍回退 keep2x（默认不变，
因为 54 个程序是 Intel 派生资产，发布包不带）。

### 9.10 第四轮：4x（待跑）

`ab_24_keep2x_4x`（model 18，锁 30）vs `ab_25_port_4x`（model 15，锁 30）。
4x 是 model 15 最可能赢的地方：一次光流摊到 3 张合成，而 keep2x 要跑 3 遍 2x 模型。
（11-21 那次"true model 4x 更差"的结论是在投影每帧翻转、且没验证深度的条件下得到的，需要用现在的版本重测。）

### 9.11 第四轮结果：4x 上 keep2x 明显更好 —— 按倍率选模型

| 日志 | 配置 | 窗口 | presented | gpu p90 | total p90 | load p90 | 视觉 |
|---|---|---|---|---|---|---|---|
| ab_24 | 4x keep2x（model 18） | 10 | 4 | 19.4 ms | 29.2 ms | 85% | **明显更好** |
| ab_25 | 4x port（model 15） | 22 | 4 | 19.5 ms | 28.4 ms | 85% | 明显更差 |

两条都 `presented=4`、0 error；port 的 `shader_supplied=28`。

结论：

1. **同一份 port 在 count=2（3x）略胜、count=3（4x）明显败。**
   最可疑的是多出来的那一个合成 index（t=1/4 或 3/4）——port 只替换了 5 个 shader，
   并且对 `i18/i19 -> i17`、`i33/i34 -> i32` 做了元数据翻译（`model_port.h:160-161`），
   这套翻译在两合成时够用、三合成时可能不再精确。
2. **性能没有区别**（gpu 19.4 vs 19.5、total 29.2 vs 28.4），所以 4x 用 port 没有任何算力收益，
   只有画质损失。
3. 由此得到**按倍率选模型**的矩阵（CP2077 + 这一版 port）：

| 倍率 | 模型 | 依据 |
|---|---|---|
| 2x | 待测（ab_26/27） | model 15 的 t=1/2 帧 vs model 18 |
| **3x** | **port（model 15）** | 3x 略好，且更便宜（gpu p90 6.3 vs 10.9） |
| **4x** | **keep2x（model 18）** | 4x 明显更好；port 无性能收益 |

注意这是这条标题 + 这份 port 的实测结论，不要外推到其它游戏/其它 libxess_fg 版本；
默认值仍保持 keep2x（没有 port 资产时自动回退），3x 想要 port 需显式 `--mfg-model=port`。

### 9.12 第五轮：2x 的 model 15 vs 18（待跑）

model 15 在 2x 时用的是**它自己的 t=1/2 帧**（`host_unlock.h:21`），与 model 18 的目标相位相同，
但模型不同。为了在不动 init 上限的前提下让它跑 2x，新增了一个开关：

```text
--interp 2 --interp-start 1
capacity = 2 (3x, 解锁按这个上限打补丁)
init     = capacity 2
SetEnabled 之后 apply 一次 count=1 -> 2x
```

实现在 `030_devices_capture_fg.inc` 的 `fg_apply_interp()`（一次性消费），
`live-count` 补丁（0x21B86B）保证这次切换不触发重建。
对比脚本：`ab_26_keep2x_2x`（model 18）vs `ab_27_port_2x`（model 15，容量 3x、起始 2x），锁 60 fps。

### 9.13 "4x 上 port 更差"是不是 port/解锁错了？—— 逐项核查（2026-11-22）

先给结论：**现有证据不支持"port 没启用"或"解锁错了"**；port 在 4x 确实走了另一条路径，
只是它的输出在这条 AMD 移植上更"真实"也更"挑输入"。核查项：

| 检查 | 结果 |
|---|---|
| 3x vs 4x 请求的程序集 | **27 个 key 完全相同**（`comm` 双向 diff 为空）；`shader_native_lookup`=28、`shader_supplied`=28 → 没有"未命中回退原生" |
| port 构造与 MFG-Unlocker 一致？ | 一致：`ShaderPort(fg, Self(), root, vendor)` → 校验 16 个 reconstruction key → `ModelPort(fg, true, true)` → 解锁 **不含** keep2x（`xess_mod.cpp:1087-1105` vs `xefg_mfg_port.h:147-162`） |
| 4x 的算力签名 | 与 keep2x 同场（04:17/04:18）：port `gpu=16.1-16.4 ms`（非常平），keep2x `17.4-17.7 ms` → **port 更便宜**；3x 同样 port 更低（≈5.0 vs 6.2）。若 port 实际在跑 model 18，不会更便宜 |
| 生成张数/错误 | 两条 4x 都 `presented=4`、`xN=4.00`、0 error、0 MISMATCH |
| 结构性断言 | `model_port_applied edits=167`、graph 46 节点、`port_graph_length` 未触发 → 模型目录/图/输出描述符的翻译自洽 |

所以剩下的解释只有两个方向：

1. **model 15 对"非 0.5 相位"的输入更敏感**（t=1/4、3/4 需要更准的 MV/深度/jitter）。
   3x 的相位是 1/3、2/3（离 0.5 更近）→ 略胜；4x 扩到 1/4、3/4 → 输入误差被放大。
   keep2x 的 model 18 在同样相位下更"糊"，把误差抹掉了（"UI 拖影"就是这个特征的副作用）。
   —— 这是一条**可验证**的假设：去掉 jitter 常量（`--fg-jitter-zero`）看 4x 是否改善。
2. **port 的数值在 3 次合成时退化**（只替换 5 个 shader、保留 2 个 Intel U-Net、`i18/i19→i17` 翻译）。
   —— 验证方式：用 XeFG 自己的调试视图只看生成帧，确认 3 张是**三个不同相位**还是实际重复。

### 9.14 第六轮：两个判别实验（已部署）

消费端此前只解析了 `--fg-debug-*` 却没有调用 `EnableDebugFeature`，这次补上了（`040_cadence.inc`）：

| 脚本 | 内容 | 判据 |
|---|---|---|
| `ab_30_port_4x_jitter0.bat` | 4x port + `--fg-jitter-zero` | 若边缘明显改善 ⇒ 相位相关的 jitter 是主因（输入侧） |
| `ab_31_port_4x_onlygen.bat` | 4x port + 只看生成帧 + 打标签 | 三张生成帧**不同**⇒真多帧；**重复**⇒port/调度问题；并指出哪一张最差 |
| `ab_32_keep2x_4x_jitter0.bat` | 4x keep2x + jitter0（对照） | keep2x 也应变化；对比两者对 jitter 的依赖程度 |

`ab_31` 的日志会出现 `[lock] debug views: tag-interp=1 (0) only-interp=1 (0)`；
画面只剩生成帧，用四角标签区分 index——这能直接回答"到底是模型不行，还是输入不行"。

### 9.15 第六轮结果：port 的插值会被"输入合法性"打掉（关键）

| 日志 | 配置 | gpu（逐窗） | total | presented | show |
|---|---|---|---|---|---|
| ab_30 | 4x port + `--fg-jitter-zero` | **0.3–4.5 ms** | ~10 ms | 4 | 120 |
| ab_31 | 4x port + 只看生成帧 | 1.1–3.2 ms | ~1.9 ms | 3 | **30** |
| ab_32 | 4x keep2x + `--fg-jitter-zero` | 17.4–17.8 ms | ~28.7 ms | 4 | 120 |

ab_30 是最有信息量的一条：**jitter 置 0 后，port 的合成几乎不再执行**
（gpu 从 16.2 ms 掉到 0.3–4.5 ms），但 `presented` 仍是 4、`show` 仍是 120 ——
也就是说库把**当前后缓冲重复上屏 4 次**，并没有报错（`result=SUCCESS`）。
同样的 jitter=0 对 keep2x 毫无影响（17.6 ms）。这说明：

- port 的插值结果会被库**静默判失败并退回后缓冲**，而且触发条件很宽（jitter=0 就足够）；
- `onlygen` 里看到的"原始帧"与 SDK 的说明一致：
  `SHOW_ONLY_INTERPOLATION` 在插值失败时退回当前后缓冲，且每批只上屏 1 张（`show=30`），
  所以**光凭它不能证明失败**——需要更硬的判据。

### 9.16 用"黑屏"做二值判定（已部署）

SDK 的第三个调试特性：`PRESENT_FAILED_INTERPOLATION` —— 插值失败时显示黑图而不是退回后缓冲。
消费端已接上 `--fg-debug-fail-black`：

| 脚本 | 内容 | 判据 |
|---|---|---|
| `ab_35_keep2x_4x_failblack.bat` | 4x keep2x + fail-black | 对照：应**无黑**（证明特性生效） |
| `ab_37_port_3x_failblack.bat` | 3x port + fail-black | 对照：若 3x 正常，应**无黑** |
| `ab_34_port_4x_failblack.bat` | 4x port + fail-black | **出现黑/闪烁 ⇒ 第 3 次合成失败**（就是 4x 更差的原因） |

结果分支：

- **34 黑、35/37 不黑** ⇒ 定位为"3 次合成路径失败"（port 的图/输出描述符在 count=3 时不被接受）。
  下一步：试 capacity/count 组合（如 `--interp 4 --interp-start 3`）、换掉 AMD 上保留的
  两个 Intel U-Net、或核对 3 合成的输出描述符。
- **34 不黑、但画质仍差** ⇒ 合成成功但数值差，属"移植数值/输入精确度"，方向转为
  jitter/MV 的精确传递（注意：jitter=0 已被证明更糟且会直接打死合成，不能用）。

### 9.17 第七轮结果：合成是"成功"的 —— 问题在移植数值，不在启用/解锁

35（keep2x）、37（3x port）、34（4x port）**都没有黑屏**。按 SDK 语义，
`PRESENT_FAILED_INTERPOLATION` 只在插值失败时出黑图，因此：

- 4x port 的每一张生成帧都被库判为**成功合成**；
- 4x 的"画质更差"不是"失败退回后缓冲"，而是**移植后的 model 15 在 t=1/4、3/4 的输出本身更差**；
- 结合 §9.13 的核查（程序集一致、构造与 MFG-Unlocker 一致、算力签名不同），
  "port 未启用 / 解锁错误"可以正式排除。

另外补一条独立的坑（§9.15 已记录）：`--fg-jitter-zero` 会让 port 的合成**静默不执行**
（gpu 16.2 → 0.3–4.5 ms，presented 仍是 4），但那次没有开 fail-black，
所以它是否被库算作 "failed interpolation" 还未验证；无论如何 **jitter0 不能用于 port**。

### 9.18 第八轮：两个针对"移植数值/调度"的实验（已部署）

| 脚本 | 内容 | 若改善说明 |
|---|---|---|
| `ab_38_port_4x_replace_all.bat` | `--mfg-replace-all`：AMD 上也替换全部 7 个 shader（不再保留两个 Intel U-Net） | 保留的两个 Intel U-Net 在 3 次合成下数值不对 |
| `ab_39_port_4x_capacity4.bat` | `--interp 4 --interp-start 3`：capacity 4、count 3 | 3 合成路径需要更大的 capacity 流水线（容量/计数耦合问题） |

`--mfg-replace-all` 已用 `--fg-probe` 预验证：`kept_intel":[]`、`replacements":7`。
两个变量分开跑，便于归因；`ab_26/27`（2x，model 15 的 t=1/2）可作基础合成质量的对照。

---

## 10. 最终结论与决策（2026-11-22）

**所有倍率保持 keep2x（model 18），不使用 ported model 15。**

依据（全部为同一台机、CP2077 4K、120 Hz 的实测）：

| 维度 | 结论 |
|---|---|
| port 是否真的在跑 | **是**：`model_port_applied 15->16 edits=167`、`shader_port_applied 54/5`、运行期 `shader_supplied=28`；3x/4x 请求的程序集完全相同 |
| 解锁是否错误 | **不是**：构造与 MFG-Unlocker 的 `PublishHostPort` 逐行一致；fail-black 三条都不黑 ⇒ 4x 的合成被库判为**成功** |
| 3x 画质 | port **略好**（差距很小）；成本 ≈5.0 vs 6.2 ms |
| 4x 画质 | port **明显更差**；成本仅便宜 ~1.5 ms（16.1 vs 17.6），无收益 |
| 两条移植修正 | `--mfg-replace-all`（不保留两个 Intel U-Net）与 capacity 4 / count 3，**都没有改善** |
| 其他 | `--depth-lag 1` 更差；`--no-upsample` 中性偏负；`--fg-jittered-mv` 打死生成；`--fg-jitter-zero` 让 port 合成静默消失 |

因此：**model 15 的多帧路径在这条 AMD 移植上没有带来可用收益**——它在离 0.5 更近的相位（3x）
略好，在极端相位（4x）明显更差，且两条最可疑的移植参数都不是原因。

保持的行为与默认值：

- `g_mfg_model` 默认 **2（keep2x）**；UI 对所有倍率显式追加 `--mfg-model=keep2x`；
- `--mfg-model=port` 保留为**显式实验入口**（需要 `xefg_programs` 资产，缺资产自动回退 keep2x），
  供将来换 libxess_fg 版本或 Intel 副卡时再评估；
- `--fg-gate`（默认 0）、`--interp-start`（默认 0）、`--mfg-replace-all`（默认关）、
  `--fg-debug-*`（默认关）都不改变默认行为。

顺带保留的两项**与模型无关的修正**（keep2x 下也生效、且都是修 bug）：

1. `have_matrix` 每帧翻转 → 2 s 投影缓存（FOV/near-far/矩阵不再每两帧跳一次）；
2. near/far 从游戏自己的 view-to-clip 派生（CP2077 实际为 `zn=0.02 / zf=16777`），
   而不是硬编码 `10/10000`；`--fg-far-finite` 仅在 far 为无限时生效（CP2077 不适用）。

如果将来要重开这条线，先决条件与已知失败点都在 §9.13–§9.18：程序集/构造/解锁已排除，
剩余只有"port 的极端相位数值"本身，需要换资产集（比如 sm86 的 model 15 内核）或换库版本。

---

## 11. 剩余两个入口：颜色匹配 与 输入质量（2026-11-22 起）

在"XeFG 执行 / 无损传输 / 配对算法都没问题，模型固定 keep2x"之后，只剩两个入口。

### 11.1 已经被日志排除的部分

| 检查 | 实测（ab_02/20/24） | 含义 |
|---|---|---|
| 颜色↔MV/depth 内容配对 | `pair exact=40`、`dup=0`、`ms_fail=0`、`lag(avg 0.03–0.05, max 1)` | marker 保证喂给 XeFG 的颜色与 MV/depth 是**同一内容帧** |
| 输入帧丢失 | `arr_gaps=0`（偶发 1/12 窗）、`qdrop` 0 | 不会跨帧插值 |
| WGC 时延 | `age=4.9–13.3 ms`（< 1 个输入间隔） | 是**延迟**，不是错配 |
| 颜色拷贝开销 | `colour=0.2 ms` | 转换/拷贝不是瓶颈 |
| 尺寸 | `bb=3840x2160` = 窗口 1:1 | 没有缩放重采样 |
| MV 语义 | `pixels / prev-cur / 1920x1080`，`mvscale=(1,1)` | 声明值与像素单位自洽 |

### 11.2 入口 A：WGC 的颜色

分两件事，别混：

1. **内容/时间匹配**：上面已排除（marker 精确配对，lag ≤1 槽）。
2. **颜色保真度**：WGC 拿到的是**合成后的窗口图像**，可能经过 DWM 采样/色彩处理；
   而 XeFG 的合成输入是这张图。检验方法只有一个干净的：**换成游戏自己的后缓冲**
   （`ColourStream=1` + 消费端 `--fg-colour shared`），其它全部不变。
   这条路径同时移除 WGC 的时延与合成，因此 A 若有问题，它会一起消失。
   代价：颜色要过 PCIe（渲染卡约 +6%），所以它只作 A/B，不作为默认。

另外用 `--tearing` 单独检验"呈现路径"（DWM 合成 vs 直接 flip）对**观感边缘**的影响。

### 11.3 入口 B：输入质量

- **MV 是唯一"未测量"的输入**：协议里有 `kSemMvMeasured / kSemSignMeasured /
  kSemQualityKnown / kSemMvUsable`（残差测试的结论位），但 **producer 侧从未实现**，
  所以日志一直打 `quality=unknown (sign test has not run yet)`。
  目前 MV 的正确性只是"声明 + 画面看起来没错"。
- 可立即做的 A/B：**MV 车道来源**——`FgInput=DLSSG`（当前）vs `FgInput=Upscaler`
  （DLSS-SR 参数块，render 分辨率、另一套卷积输出）。两条车道都是"游戏原生 MV"，谁更干净是实测问题。
- **深度**：`FINAL_CONFIG` §5.16 与 port 轮的 `--no-depth` 都指向"model 18 不读深度"。
  在 keep2x 4x 上再确认一次（`--no-depth`）：若画质不变，**7.9 MB/frame + 一次 tag/upsample 可以永久省掉**。
- 深水区（暂不做）：MV 的 jitter 逐相位修正（sm86 自己编了 `jt_` 内核）；
  以及自建 **MV 残差探针**（用上一帧颜色 warp 当前帧，算残差 → 直接给出 MV 质量分，
  同时能自动判定 sign/unit 是否声明错）。

### 11.4 本轮脚本（4x keep2x，其余全部不变）

| 脚本 | 变量 | 需要 |
|---|---|---|
| `ab_40_keep2x_4x_tearing.bat` | 呈现路径 ALLOW_TEARING | 无 |
| `ab_43_keep2x_4x_nodepth.bat` | 去掉深度流 | 无 |
| `ab_41_keep2x_4x_colour_shared.bat` | 颜色换成游戏后缓冲 | 游戏目录 `use_colour_v30.bat` + 重启 |
| `ab_42_keep2x_4x_upscaler_mv.bat` | MV 换 Upscaler 车道 | 游戏目录 `use_upscaler_mv.bat` + 重启 |

还原：`use_wgc_v30.bat` / `use_dlssg_mv.bat` + 重启。四个切换器都会自动备份 `fakerFG.ini`。

### 11.5 如果两个入口都不是

那剩下的就是 **"model 18 在同一组 MV/颜色上跑 4 个相位"的模型上限**（4x 的本质），
不再是链路问题。此时产品层的最优解就是文档 §7.4 的结论：**优先 2x@60，4x 只用于输入上不去、
又必须贴满刷新率的场景**；而 3x 是 2x 与 4x 之间的折中。

## 12. 附：前几轮 A/B 的其它健康指标

- `pair exact=40/40`（每窗），`dup=0`、`ms_fail=0`：颜色↔MV 配对没有问题；
- `initFlags=0x1`（inverted_depth=1，jittered_mv=0，ndc_velocity=0），
  `producer semantics: mv=pixels(declared) sign=prev-cur`，`mvscale=(1,1)` —— 与 pixels 单位自洽；
- `frame constants ... fovY=51.01deg`（来自矩阵），`--fg-fov 60` 已退役（日志有 note）；
- 3x/4x 的 `total p90` 距预算（25/33.3 ms）还有 6–8 ms，`skip` 总计 6–10 次/轮；
- 4x `load p90` 72–74%，比 11-21 那次的 77–87% 更松（这次场景更轻或库版本变了）。



