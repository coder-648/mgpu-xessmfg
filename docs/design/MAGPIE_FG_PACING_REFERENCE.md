# Magpie 的 XeSS-FG pacing 实现：可借鉴点与我们 consumer 的落点

> 审阅对象：`third_party/Magpie-experimental`（`src/Magpie.Core/XeSSFGPacing.h`、
> `XeSSFGTiming.h`、`XeSSFGPresenter.cpp`、`FramePresentationTiming.h`、
> `FramePacingWait.h`、`StepTimer.cpp`，以及
> `docs/experimental/design/20260914-xessfg-startup-pacing-investigation.md`）。
>
> 结论先行：**它最有价值的不是"怎么改 k"，而是"怎么不把自己的等待喂回节奏"**。
> 我们动态逐帧里几个反复踩到的坑（`k*V` 正反馈、每帧切 k、缺少输出间隔观测），
> Magpie 都给出了有实测证据的答案；反过来，它没有解决"任意上限选 k"，
> 那部分仍以 `docs/design/DYNAMIC_CADENCE_ANY_CAP.md` 为准。

---

## 1. Magpie 的 pacing 栈长什么样

```
游戏/源（外部捕获）
  │  SetSourceTiming(frameId, sequence, generation, timestamp100ns)
  ▼
Renderer / StepTimer              ← 基础输入闸门：min/max interval，整数倍吸附
  │   FrontEdgeSyncClock          ← 仅在非 FG 路径使用
  ▼
XeSSFGPresenter
  ├─ SetBaseFrameRateLimit()      ← XeLL minimumIntervalUs（FG 路径唯一的限帧者）
  ├─ XeSSFGTiming::Submit()       ← frameRenderTime 估计（源节奏 / 归因提交间隔）
  ├─ xellSleep + markers          ← 低延迟
  └─ 代理 Present（一次调用出 2~4 张）
        │
        ▼
   libxess_fg 内部
        ├─ 原生 scheduler（可用时优先，直接放行）
        ├─ TsDetour：按源周期重排 burst 内每张的 deadline
        ├─ PaceFrame：scheduler 不可用时的兜底等待
        └─ PresentationIntervalQueue：逐输出提交间隔 p50/p95/p99
```

四个关键立场：

1. **一层只允许一个节奏所有者**：XeSS FG 路径把前端限帧交给 XeLL
   （`ResolveFrameSyncBackend` 对 `xessFG` 直接返回 `XeLL`），
   `FrontEdgeSyncClock`、Reflex、`StepTimer` 的限帧都退出，
   避免同一帧被多套等待叠加。
2. **原生 scheduler 优先**：`TryPace` 先看 `SchedulerUsable(ctx)`，
   可用就只转发/计数，不自己等；`ProviderPacesLastFrame` 再挡掉
   "provider 已经给最后一张排过等待"的重复等待。
3. **过期时隙不追加**：deadline 只在同一 burst 内按固定步长推进；
   已经过去的时隙交给 GPU/Present 消费，最多 3 张可追赶，
   下一个 burst 重新取锚点，不携带时间债务。
4. **观测与推断分离**：SDK 计数（`framesPresented`）、provider 调用、
   提交间隔分位、显示事件是四个口径；他们只把显示事件当最终验收，
   日志里明确标注 "not display events"。

---

## 2. 最值得借鉴的十条

### B1 过期时隙不追加等待（最重要）

- 位置：`XeSSFGPacing.h:208-246`（`PaceFrame`）、`:363-427`（`TsDetour`）。
- 旧实现：一张输出迟到时 `nextDeadline = anchor + elapsed + burstStep`，
  即"已经过去的等待 + 一个完整步长"，把未来的等待也推后。
- 实测（`20260914-xessfg-startup-pacing-investigation.md` §3/§7）：
  仅跳过这一个分支，4× 冷启动前 120 次提交从 **10.176 / 11.917 s 降到
  2.980 / 3.158 s**，周期估计峰值从 **186.7 ms 降到 37.3 ms**；
  形成的是正反馈：**等待变长 → WGC 采样更稀 → 估计周期更大 → 等待更长**。
- 正式修复：固定 burst 内步长、过期不补、下一 burst 重锚、最多 3 张追赶
  （`:285-367` 注释写得很清楚："do not map a future deadline to QPC now
  or roll each expired slot forward"）。
- **我们的镜像问题**：`frame.cpp:45-69` 的 `fg_input_pace(k)` 与
  `cadence.cpp:766-810` 的 `frameRenderTime`。我们已经在文档里禁止
  cap-limited 下用 `k×V` 作预算（`DYNAMIC_CADENCE_ANY_CAP.md` §4.7），
  但缺少"迟到后不再补"的显式规则与计数。
  建议：`fg_input_pace` 的 `next` 改为
  `due + k*V`，若 `now > due + k*V` 则 `now + k*V`（不追赶），
  并限制单次等待不超过 `k*V`；`late` 超过 1 个 vblank 时重锚并记一次事件。

### B2 启动估计：前两个样本取较短值，之后中位数，条件式 reset

- 位置：`XeSSFGTiming.h:17-57`。
- 规则：
  - 只在 `frameId == previous+1` 且时间戳有效单调时，用**捕获时间戳差**
    作为源周期；否则用“提交间隔 − 我们自己的额外等待”；
  - reset 条件：sequence/generation 变化、frameId 不前进、提交间隔 ≥500 ms、
    时间戳倒退或重复；
  - bootstrap：只有两个样本时取 `min`；三个样本起用 9 样本滚动中位数；
  - 明确写："连续 frame ID 不证明 WGC 没跳帧"，`captured` 只是
    "采用了捕获时间戳"的标记。
- **我们已有的更好条件**：`frame.cpp:1307-1319` 优先用 producer 的
  `frame_qpc_ms`（游戏自己的时钟），比 WGC 时间戳更接近源节奏。
  但我们**没有**：
  - 两个样本取短值的 bootstrap（首帧 400 ms 会把估计拖住几帧）；
  - 与会话/配对/换代绑定的 reset（现在只有 >500 ms 的隐式重锚）。
- **已落地**：`src/cadence_core.h` 增加 `Sample.reset` 与
  `min(first two)` bootstrap，仿真覆盖 400 ms 冷启动与 15→60 fps 换代
  （`tests/cadence_core/sim.cpp` table 6，全部检查通过）。

### B3 只归因"我们自己的等待"，绝不扣整段 Present

- 位置：`XeSSFGTiming.h:35-39`、`FramePresentationTiming.h:88-133`
  （`CaptureFrameCadence`："Estimate pacing without feeding downstream
  queue waits back into the next interval"）。
- 他们的做法：`sample = submitMs - clamp(previousExtraWaitMs, ...)`，
  `extraWaitMs` 只统计 pacing hook 自己 `WaitUntil` 的时间；
  XeLL sleep 与 Present 阻塞分开记，不参与源周期估计。
- **我们的镜像**：`g_arr_last_raw` 是 WGC 到达间隔，理论上不含我们的等待，
  但 WGC 池"排空取最新"会在我们变慢时把多个源帧压成一个大间隔
  （他们 §4.1 用 60/100/180 ms 的例子证明）。我们的 `arr_mean_ms()`
  用 3×median 夹单样本，只能挡住孤立尖峰，挡不住"持续背压造成的稀疏"。
  建议：在 `--interp-cadence` 下把"本帧 `gpu_throttle + Present` 的时间"
  记进日志，并在 `T` 突然变长时优先怀疑自己的队列，而不是直接降 k
  （我们的 cost cap 也是同一思路）。

### B4 输出间隔必须单独量，且分位数是验收口径

- 位置：`XeSSFGPacing.h:60-116`（512 条有界记录 + `ReadOutputStats`）、
  `PresentationIntervals.h`（异步、丢样即断开的 `PresentationIntervalQueue`）、
  `XeSSFGPresenter.cpp:1099-1112`（日志 p50/p95/p99）。
- 要点：
  - 只在 **成功的、序号连续的** provider 输出调用之间形成样本；
    丢一次回调就断开，绝不跨洞拼一个间隔；
  - 异步（`try_to_lock` / 无锁 try），hook 内不格式化日志；
  - 明确不是显示事件，不能用它宣称"画面均匀"。
- **我们的现状**：只有 `g_presented_sum / presented_n` 的均值与 `HITCH`，
  没有逐输出间隔分布；`cadence.cpp:2548-2560` 的 hist 是"用了哪些 k"，
  不是"输出间隔长什么样"。
- 建议：即使不做 hook，也可以在我们自己的 `Present` 调用点记录
  `begin/end` 与 `framesPresented`，得到"每批提交间隔"的 p50/p95/p99；
  要和 Magpie 的逐张口径一致才需要 hook（见 §3 的适用性）。

### B5 固定 burst 计划 + 下一 burst 重锚（有界追赶）

- 位置：`TsDetour` 的 `fresh`/非 fresh 分支；`g_burstStepNs` 固定，
  `index` 跳跃时按 `(index - lastIndex) * step` 补齐，但不把过期映射到 now。
- 这个结构可以直接映射到我们的 `fg_input_pace`：
  一个 batch 的 k 张输出共享一个 step；`index` 只用于本批；
  下一批重新取 `now` 作锚，不累计上一批的迟到。
- 我们的 `fg_input_pace` 已经是"每批重锚"（`next = now + k*vb`），
  但缺"本批内部固定 step"的显式表达——一旦我们将来做逐张提交
  （例如 UI 叠加或 split present），需要复用这个结构。

### B6 区间取整：最小/最大帧率、输出率都要落在整数倍上

- `StepTimer.cpp:10-31`：设定最小帧率时，把最大帧间隔吸附到
  **最小帧间隔的整数倍**，注释直说"这能使帧间隔保持稳定，代价是实际
  最小帧率可能比要求的稍高一点"。
- `FramePacingOptions.h::ResolvePresentationFrameRate`：
  未指定目标时用 `refresh / multiplier`，再与已有上限取小。
- **我们的镜像**：`cadence.cpp:425-433` 的 `ceil(0.96 × out_hz × T)`
  与 `--fg-out-hz` 的 `m` 概念。建议 `--fg-out-hz` 显式实现为
  `realized = refresh / round(refresh / outHz)`，日志写实际速率，
  与我们的 `exact` 标记一致（`cadence_core.h` 已输出 `exact`）。

### B7 空闲/暂停：先关 FG，再关限速；恢复反过来；清历史与计时

- 位置：`XeSSFGPresenter.cpp:796-822`。
- 顺序：
  - 进入 idle：`xefgSwapChainSetEnabled(false)` → `SetBaseFrameRateLimit(0/禁用)`
    → `resetHistory`、`timing.Reset()`、`compatibility.Reset()`；
  - 恢复：先 `SetBaseFrameRateLimit`（XeLL）→ 再 `SetEnabled(true)`；
  - 只在进入/离开 idle 时切换，不对每个缓存帧切换。
- **我们的镜像**：`dyn-mfg` 的 1X 路径已经先 `SetEnabled(0)`；
  但 `--interp-cadence` 没有 idle 状态，producer 停发时 `fg_input_pace`
  仍可能按旧格等待，`cadence_core` 的估计器也不会复位。
  建议：producer stall 超过 N 帧 → 暂停 pacer、`Sample.reset`；
  恢复后从 k=2 重新起锚（我们的 core 已支持 `reset`）。

### B8 高分辨率定时器 + 消息感知等待

- `FramePacingWait.h:10-31`：`CreateWaitableTimerExW(..., HIGH_RESOLUTION)`
  优先，失败回退普通定时器，再回退 `MsgWaitForMultipleObjectsEx`；
  等待期间 `QS_ALLINPUT` + `MWMO_INPUTAVAILABLE`，不阻塞消息泵。
- **我们的镜像**：`frame.cpp:56-61` 是 `Sleep(1)` + `YieldProcessor()` 自旋。
  双卡消费者不是 UI 线程，但仍可借鉴：用高分辨率定时器睡到 deadline 前
  ~200 µs，再自旋；避免 1 ms 粒度把 jitter 引入输入间隔测量。
  这也是他们能把 `extraWait` 控制在微秒级的前提。

### B9 版本/哈希门控 + 事务式补丁 + 进程级租约

- `XeSSFGCompatibility.h`：文件大小 + SHA-256 + 八处原始字节 + 模块边界
  联合校验；八处补丁作为事务安装，任一失败回滚并核验；恢复失败把模块
  标记为 poisoned，本进程不再创建 XeFG 会话；租约覆盖**所有**上下文。
- **我们的现状**：`--unlock-mfg` 也改内存，但没有这么严格的版本门控与
  生命周期管理。若我们要继续用社区解锁，建议照抄这套"宁可拒绝，不猜版本"
  的治理方式（我们已有 `docs/ops/LEGAL.md` 的许可说明）。

### B10 观测口径分离（SDK 计数 / provider 调用 / 提交间隔 / 显示事件）

- 他们每次日志都写 "not display events"，并且明确"SDK `framesPresented`
  不能当作显示事件"。
- 我们的日志（`frame.cpp:2483-2494`）把 `presented`、`fg`、`show`
  混在一行；建议至少分三列：`sdkFrames`、`providerCalls`、`presented`。
  验收"无卡顿"仍然要 PresentMon/ETW，不能只用 `show`。

---

## 3. 不要照搬的部分

| Magpie 的做法 | 为什么不适用于我们 |
|---|---|
| Hook libxess_fg 内部三个 RVA（Present/Scheduler/timestamp） | 绑定 `1.3.1.78` 单版本哈希；我们走公开 SDK + 自有代理交换链，跨版本升级会立刻失效 |
| 固定 2×/3×/4×，只改 deadline | 我们明确要"任意上限动态逐帧"；固定 k 恰好是我们要解决的问题，不是答案 |
| 用捕获时间戳当源周期 | 我们有更好的 producer `frame_qpc_ms`；WGC 时间戳是合成器量化的（`cadence.cpp:1303-1306` 已记录） |
| `framesPresented` 当输出计数 | 它是一次 Present 的汇总，无法定位单张；Magpie 自己也是靠 hook provider Present 才拿到逐输出间隔 |
| 把 XeLL 当作唯一限帧者 | 他们的捕获/呈现/限帧在同一进程；我们的 consumer 是独立进程，输入 pacer 还在我们手里，需要 A/B 验证能否全部交给 XeLL |
| 关闭 RTSS/Reflex 以避免叠加 | 我们本来就不注入 Reflex；但 `--lock`/`fg_input_pace`/XeLL 三者同样只能留一个 owner |
| 把 VRR 当成 MFG 的兜底 | NVIDIA MFG 指引与本仓实测都不建议 MFG + VRR：XeFG/XeLL 的 pacer 假设固定 vblank，VRR 会让刷新率变成第二个控制器（见 `DYNAMIC_CADENCE_ANY_CAP.md` §12.6） |
| 用 provider 间隔验收平滑度 | 他们自己反复强调不是显示事件；我们的 `SLOW frame`/`HITCH` 也只能作为过程指标 |

## 4. 落到我们代码的清单

### 4.1 已经落地（本次）

| 改动 | 文件 | 依据 |
|---|---|---|
| `Sample.reset`：会话/换代/配对变化时清估计器、相位与 cap 状态，保留学习值 | `src/cadence_core.h` | Magpie `XeSSFGTiming` 的 reset 条件 |
| bootstrap 两个样本取 `min`，三个起才用均值+EMA | 同上 | `XeSSFGTiming` bootstrap |
| 仿真新增输出间隔 p50/p95/p99，以及冷启动/复位用例 | `tests/cadence_core/sim.cpp` | `PresentationIntervalQueue` + 调查 §7.2 |

仿真实测（table 6）：

```
cold-start 400ms->16.7ms even | out 119.9 Hz  uneven 0.17%  p50 1 p95 1 p99 1 vb  est 16.70 ms
reset@700 15fps->60fps even  | out 119.5 Hz  uneven 0.50%  p50 1 p95 1 p99 1 vb  est 16.70 ms
```

### 4.2 待做（按优先级）

1. **P0 影子观测**（不改画面）：在 `frame.cpp:2498-2567` 增加
   `feedMs / observedSpacing / p50 / p95 / p99 / late / extraWait` 六个字段；
   `observedSpacing` 由 `GetLastPresentCount` 的差分与 `g_cad_expected_out` 估算，
   明确标注 "provider submissions, not display events"。
2. **P1 pacer 无债规则**：`fg_input_pace` 改为
   `due + k*V` 计划、过期不补、单次最多等 `k*V`、迟到 >1 vblank 重锚并计数；
   同时把 `Sleep(1)` 自旋换成高分辨率定时器（`FramePacingWait` 的做法）。
3. **P2 单一限帧者 A/B**：在 `--interp-cadence` 下试
   （a）现状 `fg_input_pace` + XeLL 低延迟；
   （b）关闭 `fg_input_pace`，把基础目标交给 `xellSetSleepMode.minimumIntervalUs`
   （按 `base × k` 的目标换算），XeLL 作为唯一 owner。
   对比 `uneven / p95 / late / HITCH`，决定哪一个进默认。
4. **P3 clamp 检测**：若 P0 发现 `mean(observedSpacing) < T/k × 0.97`
   持续存在，说明 pacer 内部把我们的预算压短了（Magpie 的 `nativeUnit`
   最小钳位）。此时先尝试把 `renderTime` 抬到 `k*V` 与 `T` 的较大者，
   再考虑降 k；不要直接增加 k 去"补洞"。
5. **P4 idle 状态机**：producer stall / 暂停 / 加载 → 暂停 pacer、
   `Sample.reset`、必要时 `SetEnabled(0)`；恢复顺序
   "限速/预算 → FG → 清历史"。

---

## 5. 一句话总结

Magpie 用"**只在一个地方等待 + 过期不追加 + 估计只归因自己的等待 + 逐输出分位数**"
把 XeSS-FG 的节奏问题收敛住了；我们动态逐帧的三条主线
（regime 化选 k、切换成本学习、双卡成本上限）与它是互补的：
**选 k 用我们的设计，pacing 的自反馈与观测方法借它的经验。**
另外，MFG 下"单一节奏所有者"在显示层同样成立：**VRR 关闭、刷新率固定**，
否则 VRR 只是第三个控制器（§12.6 / NVIDIA MFG 指引 / 本仓实测）。
