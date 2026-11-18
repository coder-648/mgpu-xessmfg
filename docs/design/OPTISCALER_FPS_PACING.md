# OptiScaler 的固定帧率节奏控制：设计与实现

> 审阅对象：`third_party/OptiScaler-master/OptiScaler/`
> （`misc/FrameLimit.{h,cpp}`、`low_latency/low_latency_tech/ll_fps_limiter.cpp`、
> `ll_reflex.cpp`、`ll_reflex_vk.cpp`、`ll_xell.cpp`、`ll_antilag2.cpp`、`ll_latencyflex.cpp`、
> `low_latency/input/input_common.cpp`、`input_xell.cpp`、`hooks/Reflex_Hooks.cpp`、
> `hooks/FG_Hooks.cpp`、`wrapped/wrapped_swapchain.cpp`、`framegen/xefg/XeFG_DynamicMFG.cpp`、
> `XeFG_DynamicCap.h`，以及 `Util.cpp` 的时间基准、`OptiScaler.ini` §Framerate/§FSRFG）。
>
> 范围：**目标帧率恒定的那一类限帧**——用户设的 `FramerateLimit`，以及 Dynamic MFG 在
> "Use target as cap" 下给出的 target。动态倍率选择（选 2X 还是 4X）不在本文；
> 那是 `XeFG_DynamicCount` / `XeFG_DynamicCap` 的职责，本文只讲它们与限帧器的接口。
>
> 结论先行：OptiScaler 把"节奏"拆成两层——**一个数字（`minimum_interval_us`）** 和
> **一个唯一执行者**。它自己的两个软件 pacer（`FrameLimit`、`FpsLimiter`）是兜底，
> 绝大多数情况下真正的等待发生在 Reflex / XeLL / AntiLag 2 的驱动或运行时里。
> 真正值得抄的是三件事：**绝对时隙网格（FpsLimiter）**、**FG 倍率只乘一次**、
> **零值表示"不接管"**。

---

## 1. 全景：谁能限帧，怎么选

### 1.1 一个目标，多条执行路径

```
Config.FramerateLimit (float, 0 = 关)
        │
        ├─ FrameLimit::targetFps()  ← 唯一取值入口（含 Dynamic MFG cap 覆盖）
        ▼
InputCommon::update()             ← 每次 Present 调用一次
        │  interval_us = round(1e6 / fps) × (k+1)
        ▼
set_sleep_mode(tech, interval_us) ← 只在数字变化时下发
        │
        ├─ Reflex        → NV_SET_SLEEP_MODE_PARAMS.minimumIntervalUs（驱动睡）
        ├─ ReflexVk      → VkLatencySleepModeInfoNV.minimumIntervalUs
        ├─ XeLL          → xellSetSleepMode(minimumIntervalUs) + xellSleep
        ├─ AntiLag 2     → AMD::AntiLag2DX12::Update(ctx, enabled, max_fps)
        ├─ LatencyFlex   → ctx->target_frame_time = 1000 × interval_us
        └─ FpsLimiter    → 自己的 deadline 网格（本文件内唯一的"纯软件"执行者）
```

另有第六条路：**游戏自己的原生 XeLL**（`InputXeLL::IsNative()`）。此时 OptiScaler
把其他 output 全部 deinit，只把 interval 写进游戏那个真实的 XeLL 上下文
（`input_xell.cpp:73-82` `SetRealSleepMode`、`:131-143` `LimitFps`），
返回值是"游戏的 XeLL 低延迟是否开着"，决定 `can_limit_fps()`。

真正的软件兜底只有一处：`FrameLimit::sleep()`，在 **present 路径**上，
且仅在"没有低延迟 output 能限帧"时才会走到：

```cpp
// FG_Hooks.cpp:1580-1585
if (willPresent && !state.reflexLimitsFps && state.activeFgOutput != FGOutput::NoFG &&
    !IdentifyGpu::getPrimaryGpu().dxgiIsDxvk && !InputCommon::can_limit_fps())
    FrameLimit::sleep(fg != nullptr ? fg->IsActive() && !fg->IsPaused() : false);

// wrapped_swapchain.cpp:990-994 / :1305-1309（无 FG 的普通 Present / Present1）
if (isCurrent && !reflexLimitsFps && activeFgOutput == FGOutput::NoFG &&
    !dxgiIsDxvk && !InputCommon::can_limit_fps())
    FrameLimit::sleep(false);
```

这些 `!... && !...` 条件就是"单一节奏所有者"的落地方式：**只要有一个 output 在限，
`FrameLimit` 立刻让位**。

### 1.2 目标值本身

```cpp
// misc/FrameLimit.cpp:57-70
float FrameLimit::targetFps()
{
    auto fpsLimit = Config::Instance()->FramerateLimit.value_or_default();   // 默认 0.0

    if (auto fg = state.currentFG; fg && fg->IsActive() && !fg->IsPaused() &&
        state.activeFgOutput == FGOutput::XeFG && state.xefgDynamicCapFps > 0.0f &&
        (fpsLimit <= 0.0f || state.xefgDynamicCapFps < fpsLimit))
        return state.xefgDynamicCapFps;      // 取更小的那个

    return fpsLimit;
}
```

三点设计意图：

1. **0 = 不限**，且**不会覆盖游戏自己的目标**。Reflex 的 hook 只在自己非 0 时改写
   `minimumIntervalUs`，否则把游戏原来的值原样传下去（`Reflex_Hooks.cpp:19-34`）；
   `targetFps()` 返回 0 时 `sleep()` 整个函数体不执行。
   这保证了"OptiScaler 不插手时行为与游戏一致"。
2. **Dynamic MFG 的 target 只是"更紧的上限"**，不会把用户的 `FramerateLimit` 抬高。
3. 单位统一走 `µs` 的整数（驱动/运行时 API 的公共单位），浮点 fps 只活在配置与日志里。

UI 侧的两个细节：

- 滑块范围 `0 .. max(200, refreshRate)`（`menu_common.cpp:6797-6798`），
  即允许超过刷新率（关 V-Sync 时有用）。
- "VRR Frame Cap Calculator"（`menu_common.cpp:6814-6841`）：
  `cap = round(10000 / (1000/refresh + 0.3ms)) / 10`。这条 0.3 ms 的余量就是为
  驱动限帧器的**过冲**留的；AntiLag 2 只吃整数 fps，所以再 `round()` 一次。
  这是全项目唯一显式承认"限帧器会 overshoot"的地方。

---

## 2. 从 fps 到 interval：三处换算并不一致

| 位置 | 代码 | 取整 | 60 fps 时 |
|---|---|---|---|
| `InputCommon::update()` | `round(1'000'000 / fps)` | 四舍五入 | 16667 µs → 59.9988 fps |
| `ReflexHooks::setFPSLimit()` | `round(1'000'000 / fps)` | 四舍五入 | 16667 µs |
| `FrameLimit::sleep()` | `(uint64_t)(1'000'000 / fpsCap)` | **截断** | 16666 µs → 60.0024 fps |
| `AntiLag2::al2_sleep()` | `round(1e6 / interval_us)` 再交给 SDK | 往返一次 | 整数 fps |

`FrameLimit` 那处截断是唯一会让实际速率**略高于**请求值的地方，而且只影响"软件兜底"
这一条路。不是 bug 级别，但移植时建议统一成 `round` 语义，并在日志里打实际间隔——
OptiScaler 自己在 `InputCommon::update()` 就是这么做的
（`LOG_INFO("FPS limit through {}: {} us", ...)`，`input_common.cpp:1485-1491`）。

`AntiLag2` 特意保留区间而不转 fps：

```cpp
// ll_antilag2.cpp:152-161
minimum_interval_us = sleep_mode->minimum_interval_us; // don't convert to fps due to fg fps limit fallback using intervals
```

原因见 §3：FG 倍率是在 interval 域里乘的，先转 fps 再转会引入第二次取整误差。

---

## 3. FG 下最关键的一步：倍率只乘一次

限帧的语义是"**呈现帧率**"，而游戏每批次只产出 1 张真帧。因此
`interval_for_real_frame = interval_presented × (k+1)`。谁乘、乘几次，
是这套实现里最容易错的地方。

### 3.1 主路径（`InputCommon::update()`）

```cpp
// input_common.cpp:1448-1481（节选）
uint32_t interval = 0;
if (active && fpsLimit > 0.0f)
{
    interval = static_cast<uint32_t>(std::round(1'000'000 / fpsLimit));

    auto fg = state.CurrentFG();
    bool fgActive = fg != nullptr && fg->IsActive() && !fg->IsPaused();
    auto vkFg = state.currentVkFG;
    bool driverCountsFg = fgActive && activeOutput == LowLatencyMode::Reflex &&
                          output_api == API::Vulkan && vkFg != nullptr &&
                          vkFg->OwnsSwapchain(VulkanHooks::LowLatencySwapchain());

    if (xefg_paced() || driverCountsFg)
    {
        // XeLL already counts XeFG's generated frames, the driver DLSSG's
    }
    else if (fgActive)
        interval *= fg->GetInterpolatedFrameCount() + 1;
    else if (state.dlssgDetectedInterpolationCount > 0)
        interval *= state.dlssgDetectedInterpolationCount + 1;
    else if (activeInput == LowLatencyInput::AntiLag2 && reports_frame_generation(LowLatencyInput::AntiLag2))
        interval *= 2;   // TODO: 游戏的 FSR-FG 按 1 张生成帧算
}
```

四个分支对应四种"谁知道生成了几张"：

| 情形 | 谁知道 k | 为什么不再乘 |
|---|---|---|
| `xefg_paced()`：output=XeLL 且 FG=XeFG | XeLL 自己 | XeFG 的 present 与 `xellSleep` 在同一上下文，XeLL 统计的就是**含插值帧的输出** |
| Vulkan Reflex + 自己的 swapchain + DLSSG | 驱动（`VK_NV_low_latency2`） | 该扩展限的是"提交到 swapchain 的所有 present"，含生成的帧 |
| Reflex Sync（`output_reports_fg_multiplier()`） | 驱动 | OptiScaler 用 `NV_SET_REFLEX_SYNC_PARAMS.fgMultiplier` 告诉驱动倍率，驱动自己把 interval 除回去 |
| 其余（DLSSG / FSR-FG 未上报） | OptiScaler | 只能按自己观测到的 k 乘 |

`xefg_paced()` 的定义只有一行，但它决定了整条 XeFG 路径的归属：

```cpp
// input_common.cpp:289-293
bool InputCommon::xefg_paced()
{
    // OptiScaler's XeFG needs XeLL in low latency mode and knows its generated frames
    return activeOutput == LowLatencyMode::XeLL && State::Instance().activeFgOutput == FGOutput::XeFG;
}
```

### 3.2 Reflex 那条平行的实现

Reflex 不走 `set_sleep_mode`，而是由 hook 直接改 `minimumIntervalUs`。
`Reflex_Hooks::update(fgActive, isVulkan)` 里有一份同样的乘/除逻辑：

```cpp
// Reflex_Hooks.cpp:557-589（节选）
float currentFps = FrameLimit::targetFps();

if (fgActive && State::Instance().activeFgOutput == FGOutput::FSRFG)
{
    bool driverCountsFg = reflexOutput && InputCommon::output_reports_fg_multiplier();
    if (!driverCountsFg)
        currentFps /= 2;          // 注意这里是 fps 域
}
if (currentFps != lastFps) { setFPSLimit(currentFps); lastFps = currentFps; }
```

同一件事在 interval 域乘、在 fps 域除，语义等价但**两处的取整与假设不同**：

- 主路径用 `×(k+1)`（`k` 来自 `GetInterpolatedFrameCount()`），支持任意倍率；
- Reflex 路径对 FSR-FG 写死 `÷2`（假设只有 1 张生成帧），并且只有在驱动通过
  `Reflex Sync` 知道倍率时才跳过这个 `÷2`；
- `÷2` 之后再 `round(1e6 / fps)`，与主路径的 `round(1e6/fps) × 2` 不保证逐位相同
  （30 fps + 2X：主路径 `33333 × 2 = 66666 µs`，Reflex 路径 `round(1e6/15) = 66667 µs`，
  1 µs 的差来自取整顺序；量级无意义，但它说明两条路不能互相校验）。

Reflex 这条路还叠了三个"是否真的在限"的判定：

```cpp
// Reflex_Hooks.cpp:512-541
bool reflexOutput = InputCommon::active_output() == LowLatencyMode::Reflex;
if (_updatesWithoutMarker > 20 || !_inited)      { reflexLimitsFps = false; return; }
if (isVulkan && _lastVkSleepDev)                 reflexLimitsFps = _lastVkSleepParams.bLowLatencyMode;
else if (!reflexOutput)                          reflexLimitsFps = false;   // 别的 output 自己在限
else if (_lastSleepDev)                          reflexLimitsFps = !fgActive || _lastSleepParams.bLowLatencyMode;
```

即：**Reflex 的限帧只在 `bLowLatencyMode=1` 时生效**（它本身不是独立限帧器），
且连续 20 次 update 没有 marker 就认为游戏不再走 Reflex，主动退回软件兜底。
`reflexShowWarning` 还标出 FSR-FG + 低延迟同时开的组合（会带来明显输入延迟）。

### 3.3 软件兜底的那一份

```cpp
// FrameLimit.cpp:72-100
void FrameLimit::sleep(bool fgActive)
{
    if (auto fpsCap = targetFps(); fpsCap > 0.0f)
    {
        uint64_t min_interval_us = std::clamp((uint64_t)(1'000'000 / fpsCap), 0ULL, 100'000'000ULL);
        if (fgActive)
        {
            auto fg = State::Instance().CurrentFG();
            min_interval_us *= fg != nullptr ? fg->GetInterpolatedFrameCount() + 1 : 2;
        }

        static uint64_t previous_frame_time = 0;          // ← 函数内静态，跨 FG 开关存活
        uint64_t current_time = Util::GetTimestamp();
        uint64_t frame_time = current_time - previous_frame_time;
        if (frame_time < 1000 * min_interval_us)
            combined_sleep(min_interval_us * 1000 - frame_time);
        previous_frame_time = Util::GetTimestamp();
    }
}
```

`100'000'000` µs（100 s）的上限是防御性夹紧：fps 配成极小值时不至于让
`µs × 1000` 溢出。

`FrameLimit` 的 FG 判定里没有 XeLL/Reflex Sync 的例外，因为它只在
`!can_limit_fps() && !reflexLimitsFps` 时被调用——那时已经不存在会自己数帧的 owner 了。

---

## 4. 两个软件 pacer 的算法（真正的"帧节奏"）

### 4.1 `FpsLimiter`：绝对时隙网格（推荐照抄的那一个）

`FpsLimiter` 是一个可被用户直接选作 output 的"纯限帧器"（菜单里叫 "FPS Limiter /
Only limits the FPS"），也是整套实现里唯一有**相位**概念的 pacer。

```cpp
// ll_fps_limiter.cpp:4-26
void FpsLimiter::limit()
{
    uint64_t interval_ns = 1000ULL * minimum_interval_us;
    uint64_t now = Util::GetTimestamp();

    if (interval_ns == 0 || !is_limiting())
    {
        next_frame_timestamp = 0;
        return;
    }

    // Behind schedule (or way ahead after a change), start pacing from now instead of catching up
    if (next_frame_timestamp <= now || next_frame_timestamp - now > interval_ns)
    {
        next_frame_timestamp = now + interval_ns;
        return;
    }

    if (auto res = eepy(next_frame_timestamp - now); res)
        LOG_ERROR("Sleep command failed: {}", res);

    next_frame_timestamp += interval_ns;
}
```

这段 20 行里包含了四个决策，每个都值得注意：

1. **时隙是绝对的**：`next_frame_timestamp += interval`，不是 `now + interval`。
   因此每次睡眠的过冲（overshoot）会在下一帧被**自动吸收**（下一帧少睡同样多），
   误差不累积。这是与 `FrameLimit` 最本质的区别。
2. **过期不补（no catch-up）**：`next <= now` 时**直接返回、本帧不睡**，
   并把锚点重置到 `now + interval`。丢掉一个时隙，而不是连睡两倍去追。
   这条规则正好是 Magpie 调查里"过期时隙不追加"的同款思路
   （见 `MAGPIE_FG_PACING_REFERENCE.md` §B1）。
3. **超前也重锚**：`next - now > interval` 覆盖了三种情况——限帧值变小（interval 缩短）、
   系统时钟前跳、或刚从"不限"切回"限"。没有这个判断，一次改配置会让下一帧等一个
   旧的大间隔。
4. **`next = 0` 是"未初始化"哨兵**：`limit()` 的第一条路径同时处理"不限帧"和"刚被禁用"。

调用点：游戏调 `Sleep()` 时走 `limit()`；游戏**不**调 `Sleep()`（只用 marker）时，
靠 `SIMULATION_START` 计数兜底：

```cpp
// ll_fps_limiter.cpp:47-57
void FpsLimiter::set_marker(IUnknown* pDevice, const MarkerParams& marker_params)
{
    if (marker_params.marker_type != MarkerType::SIMULATION_START) return;
    simulation_framecount++;
    // Only when the game doesn't send sleep calls
    if (last_sleep_framecount + sleep_call_timeout < simulation_framecount)   // timeout = 20
        limit();
}
```

`sleep_call_timeout = 20` 与 Reflex 那边的 `_updatesWithoutMarker > 20` 是同一个经验值：
20 帧（60 fps 下约 0.33 s）足够判断"这个游戏不走这条 API"，同时不会因为偶发丢帧误判。

### 4.2 `FrameLimit`：相对间隔的"上一帧到现在"

```cpp
frame_time = now - previous_sleep_end;            // 相对量
if (frame_time < interval) sleep(interval - frame_time);
previous_frame_time = now_after_sleep;
```

特征与代价：

- **没有绝对相位**。它保证的是"相邻两次调用之间的间隔 ≥ interval"，而不是
  "落在某条 16.67 ms 网格上"。对固定刷新率、固定帧率的目标这已经够了；
  但如果游戏本身有 0.5 ms 的提交抖动，这个抖动会**原样保留**在输出里（不重排相位）。
- **过冲会自我修正**：上一帧睡多了，本帧 `frame_time` 就大，睡得少。等价于一个
  只有比例项、没有积分项的控制器。
- **不追赶**：`frame_time > interval` 时只跳过本帧的睡眠，不会把差值带到以后。
- **一次调用 = 一批帧**：`fgActive` 时 `interval × (k+1)`，所以它天然适合
  "一次 Present 出 k+1 张"的 FG 输出路径。
- **静态变量跨会话存活**：`previous_frame_time` 是函数内 `static`，切 FG、切限帧值、
  暂停恢复都不会重置。因为算法本身是相对的，这不会造成错误，只会让"恢复后的第一帧"
  少睡一次（`frame_time` 是暂停时长，通常远大于 interval）。

---

## 5. 等待原语与时间基准

### 5.1 混合睡眠：定时器 + 2 ms 自旋

两份几乎相同的实现：

```cpp
// low_latency/ll_util.h:90-106（FpsLimiter / AntiLag2 / LatencyFlex 用）
inline int eepy(int64_t ns)
{
    constexpr int64_t busywait_threshold = 2'000'000;   // 2 ms
    auto current_time = Util::GetTimestamp();
    if (ns <= busywait_threshold) status = busywait_sleep(ns);
    else                          status = timer_sleep((ns - busywait_threshold) / 100);  // ns → 100ns

    if (int64_t sleep_deviation = ns - (Util::GetTimestamp() - current_time);
        sleep_deviation > 0 && !status)
        status = busywait_sleep(sleep_deviation);
    return status;
}
```

```cpp
// misc/FrameLimit.cpp:40-55（present 兜底用）
inline int FrameLimit::combined_sleep(int64_t ns)
{
    constexpr int64_t busywait_threshold = 2'000'000; // 2ms
    if (ns <= busywait_threshold) status = busywait_sleep(ns);
    else                          status = timer_sleep((ns - busywait_threshold) / 100);

    // Busy wait also covers a failed timer
    if (int64_t sleep_deviation = ns - (Util::GetTimestamp() - current_time); sleep_deviation > 0)
        busywait_sleep(sleep_deviation);
    return status;
}
```

设计要点：

- **>2 ms 才用定时器**。`CreateWaitableTimerExW(..., CREATE_WAITABLE_TIMER_HIGH_RESOLUTION)`
  （`ll_util.h:60-77`、`FrameLimit.cpp:10-27`）在支持的系统上把唤醒精度拉到亚毫秒
  （典型 0.5–1 ms 过冲），远好于 `Sleep()` 的默认粒度；2 ms 以下直接自旋，
  避免"睡 1 ms 醒 2 ms"。
- **定时器只睡 `ns - 2ms`**，剩下 2 ms 交给自旋做精细对齐。这是把
  "定时器的低抖动"和"自旋的高精度"分开用。
- **失败时行为不一致**：`eepy` 在定时器失败（`status != 0`）时**完全不自旋**
  ——等于这一帧不等待，靠下一帧的绝对网格自然恢复（`FpsLimiter` 恰好能恢复）；
  `combined_sleep` 则**无论成败都自旋**补齐差值（相对网格必须补，否则节奏直接短一截）。
  这个差异是刻意的，但也意味着：在没有高分辨率定时器的系统上，
  `FpsLimiter`/`AntiLag2`/`LatencyFlex` 会静默退化成"不等待"。
- `FrameLimit::timer_sleep` 用 `WaitForSingleObject(timer, 1000)`，`ll_util` 那份用
  `INFINITE`——前者防止线程永久挂死（它在 present 线程上）。

### 5.2 时间基准：一个容易被忽略的坑

```cpp
// Util.cpp:1125-1133
uint64_t Util::GetTimestamp()
{
    FILETIME fileTime;
    GetSystemTimePreciseAsFileTime(&fileTime);        // 100 ns 单位
    uint64_t time = (hi << 32) | lo;
    return time * 100;                                // → ns
}
```

**这是墙钟，不是 QPC**。`GetSystemTimePreciseAsFileTime` 在 Win8+ 内部由 QPC 插值，
分辨率足够（~0.1–1 µs），但它**会被时间服务调整**（NTP slew / step）。
所有 pacer（`FrameLimit`、`FpsLimiter`、`LatencyFlex`、`AntiLag2`）都用它算 deadline。

更微妙的是项目里还有第二个时钟：

```cpp
// Util.cpp:348-362
double Util::MillisecondsNow()   // QueryPerformanceCounter
```

`XeFG_DynamicMFG` / `XeFG_DynamicCap` 的窗口、hold、退避全用 `MillisecondsNow()`（QPC），
而它们要减去的 `sleepMs` 来自 `steady_clock`（`input_common.cpp:632-637`）。
三种时钟混在一个控制环里，这是**移植时最该改掉的地方**：统一用 QPC（`steady_clock`
在 MSVC 上就是 QPC），并给墙钟跳变加一个"单帧偏差 > 阈值就重锚"的保护。

`FpsLimiter` 的 `next - now > interval` 那一条恰好也能吸收墙钟跳变（前跳→重锚，
后跳→`next <= now` 也重锚），算是无意的防御；`FrameLimit` 则靠 `frame_time`
的无符号回绕（后跳）或超大值（前跳）自然落入"不睡"分支。两者都不会崩，但都会
**丢一个时隙**。

---

## 6. 驱动/运行时限帧器的接入方式

这一层 OptiScaler 只做"传数字 + 保状态"，但每个 API 都有各自的脾气。

### 6.1 Reflex（D3D + Vulkan）

```cpp
// Reflex_Hooks.cpp:19-34
NvAPI_Status ReflexHooks::hkNvAPI_D3D_SetSleepMode(IUnknown* pDev, NV_SET_SLEEP_MODE_PARAMS* p)
{
    memcpy(&_lastSleepParams, p, sizeof(NV_SET_SLEEP_MODE_PARAMS));  // 存一份，之后能改
    _lastSleepDev = pDev;
    if (_minimumIntervalUs != 0)
        p->minimumIntervalUs = _minimumIntervalUs;                   // 0 = 保留游戏自己的
    return o_NvAPI_D3D_SetSleepMode(pDev, p);
}
```

- **hook 层面**改参数，不改调用序列：游戏的 marker/sleep 流完全不动。
- `setFPSLimit()` 在运行中改值时，用存的 `_lastSleepParams` 复制一份、只换
  `minimumIntervalUs`，再调一次 `SetSleepMode`（`Reflex_Hooks.cpp:592-622`）；
  Vulkan 那份同样处理 `NV_VULKAN_SET_SLEEP_MODE_PARAMS`。
- **Reflex Sync**：`NvAPI_D3D_SetReflexSync` 被拦下，当 OptiScaler 自己的 FSR-FG/XeFG
  是当前输出时，由 `Reflex::set_reflex_sync(frameMultiplier)` 代发
  （`ll_reflex.cpp:157-174`），因为游戏的 Streamline 并不知道这些倍率。
  这就是 §3.1 里 `driverCountsFg` 的来源。
- 去重：`Reflex::set_sleep_mode` 里
  `if (sleep_mode_known && is_enabled() == applied_enabled && *sleep_mode == this->sleep_mode) return;`
  （`ll_reflex.cpp:111-121`）——很多游戏每帧都发同样的 SetSleepMode，去重后
  只有真正变化才进驱动。

### 6.2 XeLL（Intel）

```cpp
// ll_xell.cpp:80-110（节选）
xell_sleep_params_t p {};
p.bLowLatencyMode = forced_mode ? true : is_enabled();
p.minimumIntervalUs = sleep_mode->minimum_interval_us;
p.bLowLatencyBoost = sleep_mode->low_latency_boost;

// With ForceXeLL we have FG enabled but not actually working
// but their FPS limit thinks that the FG is working
if (Config::Instance()->ForceXeLL.value_or_default())
    p.minimumIntervalUs /= 2;

if (changed) o_xellSetSleepMode(xell_context, &p);   // 同样只在变化时下发
```

- `ForceXeLL` 的 `/= 2` 是**兼容性补丁**：此时 FG 只是"被启用"而没有真的插帧，
  但 XeLL 的限帧逻辑认为 FG 在工作（每个真帧对应 2 张输出），会按"呈现帧"来限，
  于是 OptiScaler 把区间再除 2 抵消它的假设。属于"对齐别人错误前提"的典型 hack，
  不宜作为设计参考。
- `xellSleep(frame_id)` 必须带 frame id；游戏不发 sleep 时，
  `SIMULATION_START` 之后 10 帧仍未 sleep 就补一次（`ll_xell.cpp:125-131`）。
- 原生 XeLL 路径（游戏自带）里，OptiScaler 只在**非 local context** 上覆写
  `minimumIntervalUs`（`input_xell.cpp:73-82`），避免污染自己创建的上下文。

### 6.3 AntiLag 2（AMD）

```cpp
// ll_antilag2.cpp:6-41（节选）
max_fps = minimum_interval_us > 0 ? (int) std::round(1000000.0f / minimum_interval_us) : 0;
result = AMD::AntiLag2DX12::Update(&dx12_ctx, is_enabled(), max_fps);
```

- AL2 的 API 只吃**整数 fps**，所以这里是 interval → fps 的往返；源码注释明确说
  不把内部状态转成 fps 就是为了避免二次取整（§2）。
- 调用点有两个：游戏调 `Sleep()` 时，或 `SIMULATION_START` 时
  （`current_call_spot` 在两者间切换，`ll_antilag2.cpp:163-195`）。
  它用"最近 N 帧有没有 sleep"来决定该用哪个点，避免同一帧停两次。
- `set_fg_type(interpolated, frame_id)` 通过 `OUT_OF_BAND_PRESENT_START` 的
  frame id 是否重复来判断"这张是生成的帧"（`ll_antilag2.cpp:197-206`），
  这是它唯一能拿到 FG 帧类型的方式。

### 6.4 LatencyFlex

```cpp
// ll_latencyflex.cpp:32-71（节选）
ctx->target_frame_time = 1000 * minimum_interval_us;        // µs → ns
target = ctx->GetWaitTarget(local_frame_id);
if (target > current_timestamp)
{
    uint64_t timeout_timestamp = current_timestamp + 50'000'000ULL;   // 50 ms 上限
    if (target > timeout_timestamp) { timestamp = timeout_timestamp; needs_reset = ++timeout_events > 5; }
    else                            { timestamp = target; timeout_events = 0; }
    eepy(timestamp - current_timestamp);
}
```

- 它自带等待目标（`GetWaitTarget`），不是简单的"每帧等 interval"；
  OptiScaler 只给 `target_frame_time`。
- **50 ms 上限 + 连续 5 次超时后 reset** 是防止 LFX 内部模型发散的保护
  （reset 时还会先 `eepy(200ms)` 让队列排空）。这是所有 pacer 里唯一显式处理
  "pacer 自己坏了"的实现。

---

## 7. 控制环：什么时候重新下发

```cpp
// input_common.cpp:1483-1491
limits_fps = active;

if (fps_limit_interval_us.exchange(interval) != interval)
{
    LOG_INFO("FPS limit through {}: {} us", magic_enum::enum_name(activeOutput), interval);
    if (current_tech != nullptr)
        apply_sleep_mode(current_tech.get());
}
```

`apply_sleep_mode()` 把"游戏最后一次给的 SleepMode 副本"复制出来，**只覆盖
`minimum_interval_us`**，再交给 tech：

```cpp
// input_common.cpp:1351-1359
void InputCommon::apply_sleep_mode(LowLatencyTech* tech)
{
    auto sleepMode = get_sleep_copy(activeInput);      // 每个 input 各存一份
    if (auto interval = fps_limit_interval_us.load(); interval != 0)
        sleepMode.minimum_interval_us = interval;
    tech->set_sleep_mode(&sleepMode);
}
```

这条"**保留游戏原值、只替换一个字段**"的做法很关键：

- 游戏的 `bLowLatencyMode` / `bLowLatencyBoost` / `bUseMarkersToOptimize` 不被破坏；
- OptiScaler 的限帧值为 0 时，`sleepMode` 原样透传（等于完全不接管）；
- 每个 input 存一份副本，切换 input（Reflex→XeLL 等）不会串味。

除了 interval 变化，还有两处会触发重新下发：

1. `xefgPaced` 状态翻转（XeFG 启停）或 `Force State` 改变
   （`input_common.cpp:1406-1419`）——因为 `bLowLatencyMode` 的语义变了。
2. 多帧生成启动时的降级：

   ```cpp
   // input_common.cpp:1391-1404
   bool multiFrame = state.dlssgLastSetMode == sl::DLSSGMode::eDynamic ||
                     (state.dlssgDetectedInterpolationCount > 1 && activeOutput != LowLatencyMode::AntiLag2);
   if (!std::exchange(lastMultiFrame, multiFrame) && multiFrame &&
       activeOutput != LowLatencyMode::Reflex && activeOutput != LowLatencyMode::FpsLimiter &&
       !xefgPaced && Config::Instance()->FN_ForceReflex.value_or_default() != ForceReflex::ForceEnable)
   {
       LOG_INFO("Multi frame generation, disabling {}", magic_enum::enum_name(activeOutput));
       Config::Instance()->FN_ForceReflex.set_volatile_value(ForceReflex::ForceDisable);
   }
   ```

即：**LatencyFlex / XeLL（非 XeFG）遇到 >1 张生成帧会主动关掉自己**，
因为它们内部的等待模型按 2 帧（1 张生成帧）假设。`Reflex` 与 `FpsLimiter` 不受影响。

还有一个"活性"保护：output 只在游戏的低延迟调用仍在驱动它时才睡
（`input_common.cpp:1422-1446`），条件包括 `presents_without_input <= 20`。
游戏切到菜单、停止发 marker/sleep 后，20 帧内退出限帧，避免用旧节奏把 UI 拖慢。

---

## 8. 与 Dynamic MFG 的耦合：把"选倍率"变成"可行性测试"

`FGXeFGTargetAsCapDMFG`（"Use target as cap"）打开时，限帧器成了控制器的一部分：

```cpp
// XeFG_DynamicMFG.cpp:36-60（节选）
const auto target = TargetFps(now, window);
// Use target as cap: every OptiScaler limiter holds the presented frame rate at the target
// (FrameLimit::targetFps), the target is at most the FPS limit. Without 1X the cap needs 3X to choose from.
const bool cap = config->FGXeFGTargetAsCapDMFG.value_or_default() && target > 0.0f && targetCount > 1;
state.xefgDynamicCapFps = cap ? target : 0.0f;
count = _cap.Update({ now, state.lastFGFrameTime, sleepMs, target, targetCount, count1 });
```

于是 `FrameLimit::targetFps()` 返回这个 target，**所有** 限帧路径（Reflex / XeLL /
AL2 / LatencyFlex / FrameLimit / FpsLimiter / 原生 XeLL）都会把**呈现帧率**钉在 target 上。
倍率控制器要做的就只剩一件事：**判断"当前 k 够不够"**。

`XeFG_DynamicCap` 的三条判据都建立在"限帧器已经把呈现帧率压到 target"这个前提上：

| 判据 | 阈值 | 含义 |
|---|---|---|
| 上台阶 | `presented < target × 0.97` 持续 0.3 s | 限帧器都拉不到 target → k 不够，必然要加 |
| 下台阶（有证据） | `reachable × k ≥ target × 1.04` 持续 1.5 s | `reachable = 1000 / (frameMs − sleepMs)`，即"扣掉限帧睡眠后游戏能跑多快"；k 减 1 仍够 |
| 下台阶（探测） | 呈现帧率稳定在 `target × 0.97..1.03` 且无睡眠可测 | 被 V-Sync 或外部限帧钉住时没有证据，定期试降一次 |

`frameMs` 来自 FG presenter（`state.lastFGFrameTime`），`sleepMs` 是
`InputCommon::sleep()` 用 `steady_clock` 量到的**真实等待时长**
（`input_common.cpp:632-637`）。这条"扣掉自己等待再算能力"的归因方式，与 Magpie 的
`submitMs − extraWaitMs` 同源（见 `MAGPIE_FG_PACING_REFERENCE.md` §B3）。
下台阶失败会 `Undo` 并按 5 s → 60 s 指数退避，或在 `reachable` 涨了 10% 后提前解禁。

注意 target 的默认值有三套互不相同的余量公式：

```cpp
// XeFG_DynamicMFG.cpp:184-198
if (target > 0)  return limit > 0 ? min(target, limit) : target;   // 用户显式值，且不超过 FPS Limit
if (limit > 0)   return limit;
return floor(refresh - refresh * refresh / 3600.0f);               // 120 Hz → 116；60 Hz → 59
```

而 §1.2 的 VRR 计算器是 `10000/(1000/refresh + 0.3ms)`（120 Hz → 115.8）。
**两处 margin 不是同一个公式**，只是都在"略低于刷新率"附近。移植时要选一个，
不要两个都用。

---

## 9. 特性与坑一览

### 9.1 执行者对比

| 执行者 | 节奏模型 | 相位 | FG 感知 | 失效/退出条件 |
|---|---|---|---|---|
| Reflex（驱动） | 驱动内部，按 marker + `minimumIntervalUs` | 驱动决定 | `Reflex Sync` 上报倍率；否则由 OptiScaler 乘 | `bLowLatencyMode=0`；20 次无 marker |
| Reflex Vk（驱动） | 同上，限 swapchain 全部 present | 驱动决定 | 驱动自己数（含生成帧） | 同上 + 需要 swapchain 所有权 |
| XeLL（运行时） | 运行时内部，按 frame id 与 interval | 运行时决定 | XeFG 时自己数；否则 OptiScaler 乘 | 低延迟关；>1 生成帧时被关 |
| AntiLag 2（驱动） | 整数 `max_fps` + 帧类型标记 | 驱动决定 | 靠 out-of-band present 的重复 frame id | 低延迟关；非 AMD |
| LatencyFlex | 自带等待目标（`GetWaitTarget`） | LFX 模型 | 无（遇到 >1 生成帧被关） | 50 ms 超时 ×5 → reset；>1 生成帧 |
| `FpsLimiter` | **绝对时隙网格** | 有 | 只按乘好的 interval | ForceDisable |
| `FrameLimit` | 相对间隔（上次睡醒 → 现在） | 无 | 乘 `(k+1)` | 任何其他 owner 在限时让位 |

### 9.2 已知的坑（移植时逐条对照）

1. **墙钟 deadline**（§5.2）。NTP step 会让 `FpsLimiter` 丢掉一个时隙；
   `FrameLimit` 靠无符号回绕兜住。长时间连续运行的分支应换成 QPC。
2. **`eepy` 定时器失败 = 不等待**（§5.1）。没有高分辨率定时器的系统上，
   `FpsLimiter` / `AL2` / `LFX` 会静默变成"完全不限帧"，只有一条 `LOG_ERROR`。
   移植时至少要让失败路径退化成 `Sleep(1)` 循环。
3. **三处 fps↔interval 取整不一致**（§2）。`FrameLimit` 用截断，会比目标快约 0.004%。
4. **FG 倍率的"乘法"与"除法"混用**：主路径 `×(k+1)`，Reflex 路径对 FSR-FG `÷2`。
   后者只对 1 张生成帧成立；如果 FSR-FG 将来支持 3 帧，这条会静默错 1 倍。
5. **`ForceXeLL` 的 `/2`**（§6.2）是纯兼容 hack，与真实倍率无关。
6. **`FrameLimit::sleep` 的 `previous_frame_time` 是函数内 static**，跨会话/跨 FG
   存活；切限帧值后第一帧的等待可能偏差一个 interval（有界，不累积）。
7. **多帧生成的降级是"一次性"的**：`multiFrame` 用 `std::exchange` 只在**进入**
   多帧时触发一次；如果用户随后手动把 `Force State` 打开，不会再关。
8. **`bUseMarkersToOptimize` / `bLowLatencyBoost` 由游戏决定**，OptiScaler 不碰。
   也就是说限帧的"等待发生在哪个点"（simulation start 还是 present）仍归游戏。
9. **`minimum_interval_us = 0` 在两层含义不同**：`SleepMode` 里它表示"不限"，
   `Reflex` hook 里它表示"不改写、透传游戏值"。两者恰好一致，但语义是分开实现的。
10. **`FrameLimit` 在 DXVK 下被跳过**（`dxgiIsDxvk` 判断）：那条路上的 present 会先
    转到 Vulkan，由 Vulkan 侧的限帧器负责，避免双等待。
11. **没有 `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` 的旧系统**：代码没做能力检测，
    失败就落到第 2 条。

---

## 10. 文件索引

| 文件 | 职责 | 关键行 |
|---|---|---|
| `misc/FrameLimit.{h,cpp}` | 目标值 + present 兜底 pacer | `57-70` targetFps，`40-55` combined_sleep，`72-100` sleep |
| `low_latency/low_latency_tech/ll_fps_limiter.cpp` | 绝对时隙网格 pacer | `4-26` limit，`28-39` sleep status，`47-57` marker 兜底 |
| `low_latency/low_latency_tech/ll_reflex.cpp` | Reflex 输出（去重、Reflex Sync） | `71-89`，`111-121`，`157-174` |
| `low_latency/low_latency_tech/ll_reflex_vk.cpp` | Vulkan Reflex（swapchain 绑定） | 初始化 `5-58` |
| `low_latency/low_latency_tech/ll_xell.cpp` | XeLL 输出 | `80-110` set_sleep_mode，`125-131` sleep 兜底 |
| `low_latency/low_latency_tech/ll_antilag2.cpp` | AL2（整数 fps） | `6-41`，`152-161`，`163-195` |
| `low_latency/low_latency_tech/ll_latencyflex.cpp` | LFX（自带目标 + 超时保护） | `32-71` |
| `low_latency/low_latency_tech/low_latency_tech.h` | `SleepMode` / `SleepParams` / `MarkerType` | `18-64` |
| `low_latency/ll_util.h` | `eepy` / timer / busywait | `60-106` |
| `low_latency/input/input_common.cpp` | 控制环：目标→interval→FG 倍率→下发 | `1351-1359`，`1361-1492`，`622-644` |
| `low_latency/input/input_xell.cpp` | 原生 XeLL 的 interval 注入 | `73-82`，`131-143` |
| `hooks/Reflex_Hooks.cpp` | Reflex hook：改写参数、算 `reflexLimitsFps` | `19-34`，`504-590`，`592-622` |
| `hooks/FG_Hooks.cpp` | FG present 路径的兜底调用 | `1580-1585` |
| `wrapped/wrapped_swapchain.cpp` | 无 FG present 的兜底调用 + `update()` | `460`，`990-994`，`1305-1309` |
| `hooks/Vulkan_Hooks.cpp` | Vulkan present 的兜底调用 + `update()` | `932`，`970-975` |
| `framegen/xefg/XeFG_DynamicMFG.cpp` | target / cap 的开关 | `36-60`，`184-198` |
| `framegen/xefg/XeFG_DynamicCap.h` | "target 作为上限"时的倍率控制 | `41-187` |
| `framegen/ffx/FSRFG_Dx12.cpp` | FSR-FG 自己的 pacing tuning | `278-305` |
| `Util.cpp` | `GetTimestamp`（墙钟）/ `MillisecondsNow`（QPC） | `348-362`，`1125-1133` |
| `OptiScaler.ini` | `[Framerate]`、`[FSRFG] FramePacingTuning` | `181-197`，`555-562` |

### 10.1 一个容易混淆的近邻：FSR-FG 的 Frame Pacing Tuning

`[FSRFG] FramePacingTuning`（`OptiScaler.ini:181-197`）**不是**本文说的限帧，
它是 FidelityFX 交换链内部的呈现调度参数，通过
`ffxConfigureDescFrameGenerationSwapChainKeyValueDX12`（key = 2）下发：

```cpp
// FSRFG_Dx12.cpp:290-302
fpt.allowHybridSpin = ...;            // pacing 自旋是否允许睡眠
fpt.hybridSpinTime = ...;             // 自旋时长（timer resolution 单位，<2 会频繁过冲）
fpt.allowWaitForSingleObjectOnFence = ...;
fpt.safetyMarginInMs = ...;           // 默认 0.1
fpt.varianceFactor = ...;             // 默认 0.1
```

FFX 文档给出的目标帧时间公式（`third_party/ffx/api/include/ffx_api/ffx_api_types.h:197-200`）：

```
target frametime delta = average Frametime − (variance × varianceFactor) − safetyMarginInMs
```

即"按平均帧时间减去方差的一部分再减固定余量"来定下一个目标时间戳——
这是**方差自适应 + 安全余量**的思路，与 OptiScaler 自己的固定 interval 是两套东西，
而且它在 FG 提供方内部，OptiScaler 只是透传参数。若要在自家 FG 输出里实现
"抗抖动的固定帧率"，这条公式比固定 interval 更值得参考：抖动大的场景自动多留余量
（FFX 注释里的 Example #1 也说明：余量太大会在重负载回落后"卡在"旧的长帧时间上，
所以 `varianceFactor` / `safetyMargin` 是"抗抖"与"恢复速度"之间的取舍）。

---

## 11. 对我们的落点（`lock_frame` / consumer）

本仓固定倍率链路（`lock_frame/README.md`、`src/bridge/modules/040_cadence.inc`）
的限帧是"输入 = 刷新率 / 倍率"，由游戏侧锁帧 + `--input-ms` 合成间隔，
消费端默认 `g_no_xell = true`（`src/bridge/modules/020_state_overlay.inc:248`），
即**默认不用 XeLL 限帧**。对照这份实现，可以直接借的四条：

1. **绝对时隙而不是相对间隔**。如果以后把 `--input-ms` 的合成节拍做成真正的
   pacer，`FpsLimiter::limit()` 的 `next += interval` + 双侧重锚 + 不追赶
   是最小且完整的实现（约 20 行），比"每帧睡固定值"稳得多。
2. **FG 倍率只在 interval 域乘一次，并明确"谁在数帧"**。我们 3x/4x 下
   `interval × (k+1)` 必须在唯一一处完成；一旦同时开 XeLL 限帧，必须像
   `xefg_paced()` 那样让 XeLL 成为 owner，否则会双重限帧（输出掉到 target/(k+1)）。
   `README` 里"2x 锁 60 / 3x 锁 40 / 4x 锁 30"对应的就是这个乘法。
3. **0 表示不接管、只改一个字段**。要给 XeLL/Reflex 设 interval 时，
   应复制当前 sleep mode、只覆盖 `minimumIntervalUs`，其余字段透传；
   我们自己的 `--input-ms` 关掉时必须完全回到透传状态。
4. **给驱动/运行时限帧器留 ~0.3 ms 余量**，并且只用一个 margin 公式
   （OptiScaler 自己就有 VRR 计算器与 DMFG target 两套，见 §8）。

两条"不要抄"：

- `FrameLimit::sleep` 的相对间隔 + 函数内 static：简单，但相位会跟着游戏的抖动走，
  且切配置后的第一帧行为不直观。
- `eepy` 的"定时器失败就不等"：在我们的多进程链路里会表现为"限帧偶发完全失效"，
  很难在日志里定位。

---

## 12. 一句话总结

OptiScaler 的固定帧率限帧是"**一个目标值 + 一个唯一 owner**"：
目标值由 `FrameLimit::targetFps()` 统一给出（含 Dynamic MFG 的 cap），
`InputCommon::update()` 每 present 把它换算成 `interval_us = round(1e6/fps) × (k+1)`，
只在数字变化时下发；执行者优先是驱动/运行时（Reflex、XeLL、AntiLag 2、LatencyFlex），
它们自己数生成帧的时候 OptiScaler 就不乘倍率。
它自己的两个软件 pacer 里，`FpsLimiter` 的**绝对时隙网格 + 过期不补 + 双侧重锚**
是干净的参考实现，`FrameLimit` 的**相对间隔**只是最后兜底。
时间基准用墙钟、定时器失败不自旋、三处 fps↔interval 取整不一致，是这套代码里
最该在移植时修掉的三件事。
