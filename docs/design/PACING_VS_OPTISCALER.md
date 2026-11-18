# 我们的帧节奏控制 vs OptiScaler / Reflex / XeLL

> 对照对象：本仓 `src/xess_mfg_consumer/`（v22/v23 消费端）、`lock_frame/`（固定倍率版）、
> `src/bridge/`（legacy），与 `third_party/OptiScaler-master/`。
> 前者的实现细节见 `docs/design/MAGPIE_FG_PACING_REFERENCE.md`、
> `docs/design/CADENCE_STRATEGY_V2.md`、`docs/design/DYNAMIC_CADENCE_ANY_CAP.md`；
> 后者的拆解见 `docs/design/OPTISCALER_FPS_PACING.md`（下称"OS 文档"）。
>
> 结论先行：**两边解决的不是同一个问题。** 本仓没有"限帧器"——我们从不改变游戏跑多快，
> 只在"已经到达的帧"之间排相位、决定每帧出几张、并（可选）让 XeLL 降延迟。
> OptiScaler 的每条路径都是**限帧器**（Reflex、XeLL、AntiLag 2、LatencyFlex 四个外部执行者，
> 加上它自己的 `FpsLimiter` 与 `FrameLimit` 两个软件兜底），共同作用是让游戏线程**少跑几帧**；
> 差别只在"睡在谁家、锚在什么时钟上、谁的数字"。

---

## 1. 先把"节奏"拆成三件事，否则没法比

| 层 | 问题 | 本仓的做法 | OptiScaler 的做法 |
|---|---|---|---|
| **A. 输入节拍** | 游戏每秒跑多少真帧？ | **不由我们决定**。游戏侧锁帧（2x→60 / 3x→40 / 4x→30 @120Hz），`--input-ms` 只改"我们认为的"间隔 | `FramerateLimit` → `minimum_interval_us` 交给 Reflex / XeLL / AL2 / LFX / `FrameLimit` / `FpsLimiter`，**真的限住** |
| **B. 输出节拍** | 每个真帧什么时候交给 FG、出几张？ | `fg_input_pace(_ms)` 把提交对齐到 vblank 网格；`cadence core` 选 k 与每批预算 | 不由它决定：交给 XeFG / DLSSG / FSR-FG 的 provider，它只负责把**呈现帧率**钉在 target（含 `TargetAsCap`） |
| **C. 延迟** | 什么时候开始干活？ | XeLL 全套标记 + `xellSleep`（A 卡上睡眠是空的，标记才是 XeFG 的时钟）；`gpu_throttle` 把 GPU 深度压到 1 | Reflex / XeLL / AL2 的 sleep 位置与 marker 语义（`bUseMarkersToOptimize` 由游戏给） |

**A 层是最根本的差异**：我们的限帧发生在**别人进程**（游戏自己的帧率限制、RTSS、
或游戏内菜单），OptiScaler 的限帧发生在**游戏进程内**。这也解释了为什么在
consumer 里做"限帧"只能得到丢帧或加延迟，而它做"限帧"能真的省下 GPU 时间。

---

## 2. 我们的节奏栈：8 段代码

### P1 发布驱动的主循环（consumer）

```cpp
// modules/main.cpp:1076-1080（注释）
// One publication = one frame. Pick the OLDEST unprocessed publication (not the
// newest) ...
```

一发布一帧，`wait_slot()` 是唯一的同步点。**没有任何 ratemaking**：输入多快，
循环就多快；没有帧就等。`--pace` 在 v22 之后被明确忽略：

```cpp
// modules/main.cpp:1066-1068
if (g_pace_ms > 1.0 && !g_fg_warp) {
    logf_("[consumer] note: --pace is ignored by the publication-driven loop\n");
}
```

### P2 `gpu_throttle()`：深度 1 的背压，不是限帧

```cpp
// modules/osd.cpp:296-312（节选）
// Keeps at most one frame in flight on the GPU. Without this the queue builds up and
// every Present waits for work submitted several frames earlier ...
if (g_frame_fence->GetCompletedValue() >= target) { return; }
wait_fence_value(g_frame_fence.Get(), target, 200);
```

语义是"上一帧的 GPU 活干完再开始这一帧"——**上限是 1 帧在飞**，不产生节拍。
它同时是 `gpu=` 与 `load` 的测量源。`--no-throttle` 关掉它，队列就会堆积
（换来吞吐、付出延迟）。

### P3 `fg_input_pace(k)`：绝对 vblank 网格（even/fill 路径）

```cpp
// modules/frame.cpp:70-100（节选）
double due = g_fgi_next_ms;
if (due > now) {
    if (wait > 1.5) { Sleep(static_cast<DWORD>(wait - 1.0)); }
    while ((now = qpc_ms()) < due) { YieldProcessor(); }
} else {
    ++g_fgi_late;
    // 按整 vblank 跳过（保持 mod vblank 的相位，而不是保持 mod k*vblank）。
    due += (std::floor((now - due) / vb) + 1.0) * vb;
}
// End-to-end: this batch occupies exactly k vblanks from its scheduled start.
g_fgi_next_ms = due + static_cast<double>(k) * vb;
```

四点特征，正好和 OS 文档里 `FpsLimiter` 的规则一一对应：

- **绝对网格**：`next = due + k·vb`，不是 `now + k·vb`；
- **过期不补**：迟到时按**整 vblank** 跳过（保持 `mod vblank` 的相位），不把迟到累加进下一批
  （注释记录了旧实现 `now + k*vb` 导致 vphase 游走、late 单调增长的实测）；
- **重锚只给 stall**：`now - next > 100 ms` 才重置（加载、暂停、窗口切换）；
- **睡眠方式**：粗睡 `Sleep(wait-1)` + 最后 1 ms 自旋（QPC 判据）。

`k` 来自 cadence 控制器（P5），pacer 只负责"这一批占 k 个 vblank"。

> **生效范围**：P3/P4 只在 `--interp-cadence` 下运行（`g_interp_cadence` 默认 `false`，
> 见 `state_overlay.cpp:339`、`main.cpp:365`）。固定倍率链路不经过它们：
> 那条路的节拍由「游戏锁帧 + XeFG 内部 pacing」决定，我们只负责按时把帧交出去。

### P4 `fg_input_pace_ms(budget, Λ)`：机会式量化（optimal 的 paced 分支）

```cpp
// modules/frame.cpp:104-125（节选）
if (due > now && latency_budget_ms > 0.05 && (due - now) <= latency_budget_ms) {
    ... 等到 due ...
    g_fgi_next_ms = due + budget_ms;
} else {
    // 超过 Λ 或已经过期：立即提交，不把迟到补进下一批（无债务）。
    if (due <= now) { ++g_fgi_late; }
    g_fgi_next_ms = now + budget_ms;
}
```

与 P3 的区别是**允许放弃对齐**：只有"等得起"（≤ 延迟预算 Λ）才等，否则立刻交，
下一批从 `now` 重新起锚。这是"节奏 vs 延迟"的显式取舍；OptiScaler 没有对应物
（它把等待位置交给驱动/运行时的 sleep 模型，自己不做这个决策）。

### P5 `cadence core`（`src/cadence_core.h`）：选 k 与每批预算

| 维度 | 取值 | 含义 |
|---|---|---|
| policy | `even` / `fill` / `optimal`(默认) / `locked` | 少切换 / 每帧补满 / 代价函数选 (m,k,paced) / 锁死整数 k |
| regime | `fillable` / `cap-limited` / `input-limited` | `want ≤ J` / 上限不够只能常量 k / 输入快于面板（建议关 FG） |
| 预算 | `input-T`（默认）或 `k·vblank`（`--cad-budget vblank`） | 后者让输出间距严格 1 vblank，代价是丢弃多余输入帧 |
| 约束 | `costPerOutputMs`（FG 卡单张成本上限） | 防止 6x/8x 把消费卡压满、反过来拉长真实帧间隔 |
| 切换 | `minDwell` + `deadband 0.60` + `NoteSwitchLoss/autoTuneDwell` | 切换有成本（约丢一张输出），所以学习"切换一次丢多少" |

`locked` 的算术来自 `CADENCE_STRATEGY_V2.md`：`k_ceil = ceil(R/f)`，预算恒 `k·vb`，
**输出间距严格 1 vblank**，非整除输入多出的帧由面板丢掉。这与 OptiScaler 的
`Dynamic MFG + TargetAsCap` 是**相反方向**：它用限帧器把呈现帧率钉在 target 上、再调 k 去够 target；
我们是先锁死间距、再接受丢输入帧。

### P6 legacy `--pace`：固定输出率的 tick（bridge / lock_frame）

```cpp
// lock_frame/modules/160_main.inc:1258-1268（节选）
if (g_pace_next == 0.0) { g_pace_next = now + g_pace_ms; }
if (now < g_pace_next) { if (g_pace_next - now > 2.0) { Sleep(1); } continue; }
if (now - g_pace_next > g_pace_ms) { g_pace_next = now; }  // fell behind: resync
g_pace_next += g_pace_ms;
```

这是本仓唯一"真的按固定帧率出帧"的东西，而且它比 OptiScaler 多一个能力：
**没新帧就重复上一帧**（注释："if none arrived by the tick the previous frame is presented
again, so the output rate never wobbles"）。`--pace auto` 让 tick 跟随到达 EMA。

三个边界：consumer 已不用它（P1）；`lock_frame` 默认 `g_pace_ms = 0`（到达驱动）；
`src/bridge` 里有一行明确警告：`--pace` 与 XeSS-FG 自身的 pacing 会互相串行、抬高 `load`。

### P7 XeLL：在 A 卡上只买到"契约 + 标记"，买不到睡眠

```cpp
// modules/cadence.cpp:1428-1433
xell_sleep_params_t sp{};
sp.minimumIntervalUs = 0;              // ← 从不设限帧
sp.bLowLatencyMode = g_xell_mode;      // --xell-mode 0/1，默认 1
xellSetSleepMode(g_xell, &sp);
```

之后每帧按"全套 6 个 required 标记 + `xellSleep`"发：

| 位置 | 标记 |
|---|---|
| `frame.cpp:1427 / 1435` | `XELL_SIMULATION_START` → `xellSleep(fid)` → `XELL_SIMULATION_END` |
| `frame.cpp:1807 / 1827` | `XELL_RENDERSUBMIT_START` / `_END` |
| `frame.cpp:2243 / 2290` | `XELL_PRESENT_START` / `_END` |

`fid == present_id`，与 `xell.h` 的 "required" 列表完全对得上（`INPUT_SAMPLE` 可选，我们没发）。

注意 `--xell` / `--no-xell` 是**半成品开关**：`g_no_xell` 只在启动日志里被引用
（`main.cpp:825`），并不出现在任何调用点；所以"默认 no-sleep"只是日志文字，
真实行为是**标记与 `xellSleep` 每帧都发**。A 卡上的实际效果见 §4.4。

### P8 `--input-ms` 与 `frameRenderTime`：认知，不是控制

`input_interval_ms()`（`cadence.cpp:738-748`）优先用 `--input-ms` 覆盖值，其次用
producer 每帧的发布间隔，最后才回退到（v22 后已冻结的）WGC 到达 EMA；
`fg_render_time_ms()` 再按 `--fg-rt-mode ema|raw|median|quant` 选喂给 `frameRenderTime` 的值。
源码注释把边界写得很直白：

> It does NOT create GPU time - if the work genuinely needs 27 ms in a 25 ms slot the
> frames still arrive late, they are just no longer reported as "slow".

OptiScaler 没有等价物：它把游戏的原始 `FrameTimeMs()` 直接喂进去，只做秒/毫秒量纲检测。

---

## 3. 逐项对比

| 机制 | 谁在等 | 锚（相位） | 过期怎么办 | FG 倍率 | 时间基准 | 粒度 |
|---|---|---|---|---|---|---|
| **P3 `fg_input_pace`** | consumer 的**提交线程**（交帧给 XeFG 前） | 绝对 vblank 网格，`due += k·vb` | 按整 vblank 跳，不追赶 | 由调用方传 `k`（不含 FG 倍率的隐式乘法） | **QPC** + 实测 vblank | 微秒级；粗睡 + 1 ms 自旋 |
| **P4 `fg_input_pace_ms`** | 同上 | `due`，但**只等 ≤ Λ** | 立刻交，`next = now + budget` | 同上 | QPC | 同上 |
| **P6 `--pace`** | consumer/bridge 主循环 | 绝对 tick，`next += pace_ms` | 落后 > 1 tick 就 `next = now` 重同步 | 无（固定输出率，重复上一帧补齐） | QPC | 1 ms 睡眠 + 轮询 |
| **OS `FrameLimit`** | present 线程（仅当无其他 owner） | **无相位**，`上次睡醒 → now` | 只跳过本帧 | `×(k+1)` | **墙钟** | 定时器 + 2 ms 自旋 |
| **OS `FpsLimiter`** | 自己的 `limit()`（sleep 调用或 marker） | 绝对时隙，`next += interval` | `next <= now` 或超前 > interval → 重锚 | 只按已乘好的 interval | **墙钟** | 同上 |
| **Reflex（驱动）** | 驱动内部的 sleep | 驱动自己的模型（marker 驱动） | 驱动决定 | 需要 `Reflex Sync` 上报倍率，否则外部乘 | 驱动内部 QPC | 微秒整数 interval；实际过冲 ~0.5–1 ms |
| **XeLL（运行时）** | 运行时内部的 sleep | 运行时模型 + frame id | 运行时决定 | XeFG 时自己数；否则外部乘 | 运行时内部 | 微秒整数 interval |
| **AL2（驱动）** | 驱动 | 整数 `max_fps` | 驱动决定 | 靠 out-of-band present 的重复 frame id | 驱动内部 | **整数 fps**（1 fps 粒度） |
| **LFX** | LFX 模型（自带 wait target） | LFX 内部 | 50 ms 上限，5 次超时 reset | 无 | 墙钟（OptiScaler 侧） | 微秒 |

### 3.1 同一目标下的数值对照（120 Hz、3x、输入 40 fps）

```
OptiScaler：interval = round(1e6/120) = 8333 µs → ×(k+1)=3 → 24999 µs
             → xellSetSleepMode(minimumIntervalUs=24999) / Reflex 同理
             → 由驱动/运行时决定"什么时候醒"

本仓 P3/P4：due += k·vb = 3 × 8.333 ms = 24.999 ms   （vb = WaitForVBlank 实测值）
             → 由我们在提交前睡到 due
```

**数字完全一样（24999 µs = 3 × 8.333 ms），语义完全不同**：他们限制的是**生产**
（游戏线程早醒/晚醒），我们排列的是**交付**（同一张输入帧交给 FG 的时刻）。因此：

- 他们的 `interval` 到了驱动手里，**相位由驱动的唤醒精度决定**，我们无法观测或纠正；
- 我们的 `due` 每一批都自己算、自己等，**相位可观测**（`vblank_phase_ms()` 的
  mean/std 就是验收指标），但代价是消费端线程要自己承担 `Sleep`/自旋的成本。

### 3.2 与 Reflex / XeLL / AL2 的四个本质区别

1. **等待的位置不同**
   Reflex/XeLL 睡在**游戏线程**里（`Sleep()` / `xellSleep` 的调用点），所以它能省 GPU 时间、
   也能降延迟；我们睡在**消费进程的提交线程**里，只影响"这张什么时候进 FG 队列"，
   对游戏跑多快毫无影响。这也是为什么在 consumer 加"限帧"是错的：
   输入已经采到了，晚了只能丢，不能省。

2. **语义不同：`minimumIntervalUs` 不是"帧网格"**
   它是"两次 sleep 调用之间的最小间隔"，**相位完全取决于游戏在哪里调用**。
   OptiScaler 自己也承认这一点：`bUseMarkersToOptimize` 由游戏决定，
   所以它干脆不碰，只改数字。我们的 `due` 是"第 n 批必须在第 n·k·vb 个 vblank 上"，
   是真正的网格；代价是必须自己知道 vblank 周期（我们没有用"假设 8.333 ms"，
   而是 `WaitForVBlank` 校准 120 次，因为实测误差会让相位每秒漂 2.7 ms）。

3. **时间基准不同**
   本仓全部用 **QPC**（`qpc_ms()`，`pair_dump.cpp:11-18`）；OptiScaler 的两个软件 pacer 用
   **墙钟**（`GetSystemTimePreciseAsFileTime`，OS 文档 §5.2），只有 Dynamic MFG 用 QPC。
   驱动/运行时内部用什么我们看不到，但可以确定它们**不会被 NTP 调整影响**——
   这反而说明"把 interval 交给它们"在长时间运行上比 OptiScaler 的 `FrameLimit` 更稳。

4. **能不能"发明"帧**
   OptiScaler 的所有限帧器都只**推迟**，绝不产出；我们的 `--pace` 在 tick 到了但没有新帧时
   **重复上一帧**。这在固定倍率下是有用的（输出速率恒定），但在 FG 链路上会让
   `frame_id` 与内容脱钩，所以 consumer 已经不用它了。

---

## 4. 我们缺什么、多什么

### 4.1 缺（且大概率不该在 consumer 补）

| 缺的东西 | 为什么不在 consumer 做 | 应该在哪 |
|---|---|---|
| 进程内限帧（`FrameLimit` 等价物） | 输入已采到，限帧只能丢帧/加延迟 | `fakerFG`（游戏进程）或游戏自身/RTSS |
| 每帧 k 的"官方"倍率上报 | 我们本身就是 FG 输出方，`k` 是已知量，不需要探测 | —（这是我们比 OptiScaler 简单的地方） |
| 驱动级的 sleep 精度 | 我们没有在游戏线程里 sleep 的权限 | 不需要：我们只需要排交付相位 |
| `Reflex Sync` 那套 `fgMultiplier` 协商 | 只在"别人的 FG + 别人的限帧"时才需要 | — |

### 4.2 多（OptiScaler 没有的）

- **真实 vblank 校准**：`vblank_init()` 先用 DWM 的 `qpcRefreshPeriod`，否则
  `WaitForVBlank` 采 120 次求平均；`refresh_ms()` 返回实测值。
  OptiScaler 的 `FrameLimit`/`FpsLimiter` 完全不涉及 vblank（它只认 interval）。
- **显示端相位统计**：`vblank_phase_ms(t_present)` → mean/std/min/max，日志里
  `vblank: period=... phase mean=... std=...`。这是"间距是否真齐"的验收口径，
  OptiScaler 只有"呈现帧率"。
- **无债网格**比 OptiScaler 的 `FrameLimit` 严格：迟到按整 vblank 跳、绝不重锚到 now
  （`FpsLimiter` 是重锚到 `now + interval`，会改变相位；我们刻意保持 `mod vblank`）。
- **机会式量化**（P4）：有延迟预算 Λ 时才对齐，否则立刻交。OptiScaler 是"要么限要么不限"。
- **`k·vblank` 预算**（P5 的 `locked`/`--cad-budget vblank`）：把"间距 1 vblank"作为
  硬约束，代价显式标成"丢输入帧"。

### 4.3 A 卡（非 Intel）上 XeLL 到底生效到哪一步

先把"生效"拆成三层，三层结论不同：

| 层 | A 卡（RX 6750 GRE）上的实测 | 证据 |
|---|---|---|
| **① API 契约**（context / sleep mode / SetLatencyReduction） | ✅ 全部 SUCCESS | `docs/findings/milestones/M0_RESULTS.md:10-20`：`xellD3D12CreateContext` SUCCESS、`xefgSwapChainSetLatencyReduction` SUCCESS（RTX 3080 Ti 与 6750 GRE 两列都是 SUCCESS） |
| **② 标记**（6 个 required marker） | ✅ 生效，而且**必须发** | `M4_WGC_RESULTS.md` #8：不发标记 → "节奏抖动"；`FINAL_CONFIG.md` §5.1：XeFG 用标记推断节奏，缺了只能退回最保守的呈现行为 |
| **③ 睡眠**（`xellSleep` 的延迟收益） | ❌ 实测为 0（空转） | `FINAL_CONFIG.md:575`："`xellSleep()` 在 AMD 上是**空实现**（`libxell.dll` 里明写 `No sleep algorithm available`）"；本仓日志里 `xell=` 只出现过 `0.0ms`（`FINAL_CONFIG.md:920`） |

`libxell.dll` 1.3.2.10 的字符串说明了为什么是"分层"的：

```
Latency reduction enabled - Cross Vendor.        ← 非 Intel 也有低延迟路径
Latency reduction enabled.
DisplayTelemtrySleep requires properly initialized Intel Extensions
Failed to get display telemetry.
No sleep algorithm available.                    ← 两条路都不可用时的兜底
[Sleep] Slept for (latency reduction):
Slept for: skipping, sleep too big:
Sleep() called after markers.                    ← 时序告警
Sleep initialized after frame:
```

即：Intel 卡走 `_INTC_D3D12_GetDisplayTelemetry`（需要 Intel Extensions + 驱动）；
非 Intel 有一条 "Cross Vendor" 路径；两条都拿不到时才是 `No sleep algorithm available.`。
我们在 A 卡上量到 0.0 ms，说明这条 cross-vendor 路径在**当前调用方式下**没有产生等待。

**所以在本仓的 A 卡配置里，XeLL 的实际价值只有两点**：
满足 XeSS-FG 的硬依赖（否则 `SetEnabled` 直接 `-15`），以及提供 XeFG 的时钟标记。
它**不提供**延迟收益，也不参与限帧（`minimumIntervalUs = 0`）。

三个值得实验的点（都不改链路结构）：

1. **调用顺序**：我们在 `SIMULATION_START` 之后 `xellSleep`（`frame.cpp:1427-1435`），
   而 OptiScaler 是在**之前**（`ll_xell.cpp:125-132`：先 `xell_sleep(frame_id)` 再 `add_marker`）。
   DLL 里有 `Sleep() called after markers.` 这类告警串，如果它把"START 之后"判成迟到，
   就会直接跳过。A/B：把 sleep 移到 marker 前，看 `xell=` 是否变成非 0。
2. **`XELL_INPUT_SAMPLE`（=6）没发**。头文件里它不是 required，但 XeLL 的延迟模型需要
   "输入采样 → 呈现"的距离才能算该睡多久；官方 low-latency sample 会发。
3. **`minimumIntervalUs=0` + FG 已启用**：`libxess_fg.dll` 自己会加载并调用
   `xellSetFgEnabled` / `xellSetGeneratedFramesCount` / `xellD3D12SetAppQueue`
   （这些在 `libxess_fg.dll` 的字符串里能直接看到，它通过 `GetProcAddress` 从 libxell 取），
   即 XeFG 明确告诉 XeLL"FG 在工作、生成了几张"。此时 XeLL 很可能认为节奏归 XeFG，
   自己不再做 latency sleep —— 与 OptiScaler 的 `markers_only` 上下文是同一现象。

**怎么验证**（不改代码也能做）：看日志的 `xell=` 字段是否恒为 0.0；
若要更细，接 `xellSetLoggingCallback` 就能直接看到上面那些字符串，
或调 `xellGetFramesReports` 看它是否真的在统计帧（我们目前都没接）。

### 4.4 两边都不做的事

- **VRR**：XeFG/MFG 的 pacing 假设固定刷新，本仓与 OptiScaler 都在日志里明确要求关掉。
- **在同一个链路上开两个节奏 owner**：OptiScaler 用 `!can_limit_fps() && !reflexLimitsFps`
  让 `FrameLimit` 让位；我们用 `g_no_xell` + "consumer 不做限帧"达到同样效果。

---

## 5. 场景 → 该用哪一种

| 场景 | 用哪个 | 为什么 | 不要用 |
|---|---|---|---|
| 固定倍率 2x/3x/4x，游戏能锁帧（推荐主路径） | 游戏侧锁帧 + P3 网格 | 输入节拍由别人保证；我们只排交付相位 | 在 consumer 限帧 |
| 输入与刷新率**非整除**（43 fps @120Hz） | P5 `locked` 或 `--cad-budget vblank` | 间距严格 1 vblank，代价是丢输入帧 | `fill`（每帧补满 → 间距 1.3~1.5 vb 抖动） |
| 输入抖动但均值稳定（22–28 ms） | P3 网格 + P5 `even`/`optimal` | 网格吸收抖动，k 少切 | `--input-ms` 当"修抖动"用（它只改上报） |
| 输入瞬间变化（转场、加载） | P4（`paced` 分支）+ 100 ms stall 重置 | 等不起就不等，下一批重锚 | 硬等网格（会把延迟拉长到 Λ 以外） |
| 需要更低延迟 | `--xell`（只开 `bLowLatencyMode`） | 买延迟，不买节拍 | 给 XeLL 设 `minimumIntervalUs`（会与 XeFG 内部 pacing 双重限帧） |
| 输入稀疏、要求输出率绝对恒定（固定倍率） | `lock_frame --pace N` | 没新帧就重复上一帧 | 在 FG 链路上用（`frame_id` 与内容脱钩） |
| 输入快于面板 | `1X` / `--no-fg` | `input-limited` regime | 加 k（没有可插的帧） |
| 任何情况 | 关 VRR | MFG pacing 假设固定刷新 | 把 VRR 当兜底 |

---

## 6. 一句话总结

- **OptiScaler 的所有路径都在解决"游戏跑太快"**：把 `minimum_interval_us` 交给
  Reflex / XeLL / AL2 / LFX，或用自己的 `FpsLimiter`（绝对网格）/ `FrameLimit`（相对间隔）
  兜底；它们睡在游戏线程里，所以能省 GPU 时间，但**相位不可控**。
- **我们的 8 段代码都在解决"帧已经到了，怎么排"**：`fg_input_pace` 把交付锚在
  实测 vblank 网格上（绝对、无债、过期整周期跳），`cadence core` 决定每批出几张与预算，
  `gpu_throttle` 把在飞帧压到 1，XeLL 只用来降延迟。
- **所以"我们的限帧"和"Reflex/XeLL 的限帧"不是同一层**：他们限的是**生产**，
  我们排的是**交付**；两边的数字能对上（24999 µs = 3 × 8.333 ms）只是同一套算术
  的两种用法，真正的差别在"谁在等、等的相位从哪来、过期了要不要还债"。
