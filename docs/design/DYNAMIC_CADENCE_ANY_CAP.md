# 动态逐帧在任意上限下的设计（consumer / 双卡 FG）

> 状态：**设计 + 可运行参考核心**（`src/cadence_core.h`，仿真
> `tests/cadence_core/`）。Windows 侧尚未接线，接线点见 §7。
>
> 适用：`xess_mfg_consumer.exe --interp-cadence`（动态逐帧），
> 渲染卡 = 游戏，插帧卡 = 消费端（双卡 FG）。
> 目标上限 `J = min(--interp, DLL maxSupportedInterpolations) + 1`，
> 即 J 可以是 2x..9x 任意值，**也包括"输入慢到 J 张也填不满面板"**的区间。

---

## 0. TL;DR

1. 现有 `--interp-cadence` 实际有三套取 k 的逻辑（`governor` / `mean+phase` /
   `tick`），只有 governor 被验证为稳定默认；它们在 `want > J`（上限不够）
   时**没有定义行为**，只是把 k 静默夹到 J，`frameRenderTime` 仍按输入间隔喂，
   输出间距落成 1.3~1.5 vblank 的来回抖。这就是"上限一变就不顺"的根因。
2. 新控制律把问题拆成**两个上限、三个 regime**：
   - 上限：面板 vblank 数 `want`、DLL/用户倍率 `J`、**FG 卡单批成本**（双卡特有）；
   - regime：`fillable`（填得满）、`cap-limited`（填不满，只能常量 k）、
     `input-limited`（输入比面板还快）。
3. `fillable` 下默认 **even 策略**：一个 epoch 内 k 不变，向上立即、向下需持续
   10 帧，避免"每次切换丢一张输出"被打成 12~15% 的顿挫；
   `cap-limited` 下按 **残差 + 容差**挑一个常量 k（残差 ≤0.25 vblank 的候选里
   取最大的 k），保证"同样整齐时输出更高"，并**不切换**。
4. 切换成本被实测学习（`NoteSwitchLoss`）：一旦确认"每次切换丢输出"，
   自动加长驻留、把逐帧 `fill` 降级为 `even`。
5. 记账改为**只减真正生效的 k**（`Commit(appliedK)`）。旧实现减的是"想给的 k"，
   drain 延迟/DLL 拒绝时长期均值会悄悄漂，这是另一个"越跑越不顺"的来源。
6. 纯计算核心已用确定性输入仿真：J=2..8、锁定/抖动/突发/阶跃/慢输入、
   切换成本 0/1/2、运行中改上限、冷启动与会话复位全覆盖，22 项结构检查全过（§9）。

---

## 1. 现状：代码路径与缺陷

### 1.1 数据流（现在）

```
WGC 捕获到达
  └─ frame.cpp:1302  if (g_interp_cadence && !g_reuse_frame)
       ├─ g_cad_input_ts_ms = 游戏自己的 frame_qpc_ms（优先）或 WGC 时间
       ├─ cadence_next_slots()        // cadence.cpp:405-650，三选一取 k
       ├─ cad_want = slots - 1
       ├─ gpu_throttle()              // frame.cpp:1329，队列深度 1
       ├─ ack_previous_slot()
       └─ fg_set_interp(cad_want)     // frame.cpp:1337
            └─ devices_capture_fg.cpp:438  SetNumInterpolatedFrames
  └─ fg_render_time_ms()              // cadence.cpp:766，喂 pacer
  └─ fg_input_pace(g_cad_last_k)      // frame.cpp:45-69，提交对齐 k*vblank
```

三个取 k 源（`--cadence-source`）：

| 源 | 代码 | 行为 | 问题 |
|---|---|---|---|
| `governor`（默认） | `cadence.cpp:410-457` | `k = ceil(0.96·out_hz·T_in/1000)`，升档立即、降档连续 24 帧 | 稳定，但 `want > J` 时无定义；`k*vblank` 与 `T_in` 的差被面板丢弃 |
| `mean` | `cadence.cpp:464-650` | 16 样本均值 + Bresenham 相位，`phase -= k` | 每帧都可能切；drain/DLL 拒绝时减的是"想给的 k" |
| `tick` | `cadence.cpp:337-386` | 到达时间量化到 vblank，取 tick 差 | 对 WGC ±1 vblank 抖动敏感，实测最差 |

### 1.2 缺陷（按影响排序）

1. **上限不够时没有策略。** `cadence.cpp:602-607` 把 `k` 夹到 `kmax`，
   但 `want` 仍是 5.7 vblank，`fg_render_time_ms()`（`cadence.cpp:766-810`）
   返回 `t_in`，于是 J 张输出被摊在 5.7 个 vblank 上：
   `spacing = want/J` 既不是整 vblank 也不是常量。
   仿真里 `slow21 + J=4` 的现状是 **84.1 Hz、42.7% 的间距不齐**（§9）。
2. **相位记账与生效值脱钩。** `cadence.cpp:641` 在 apply 之前就
   `phase -= k`；而 apply 可能被 `cadence_present_drained()`（`cadence.cpp:275-281`）
   延迟或 `fg_set_interp` 拒绝（`frame.cpp:1339`）。误差没有回到 phase。
3. **切换成本没有进入控制律。** 注释（`cadence.cpp:191-195`）已写明"一次切换
   约等于丢一张输出"，但 `mean`/`tick` 路径仍按相位逐帧切；`--cadence-hold`
   被标为"退役"（`cadence.cpp:66-69`）后没有替代机制。
4. **双卡成本没有上限。** `k` 张输出都要 FG 卡出图，4K 下每张可能是 3~8 ms；
   `J` 只由 DLL 与面板决定，6x/8x 会把消费卡压满，反过来拉长真实帧间隔
   （`docs/findings/FINAL_CONFIG.md` 5.17.1 的 84–90% load 就是前兆）。
5. **没有"是否整 vblank"的显式状态。** `--fg-out-hz 60` + 40 fps 输入是
   1.5 张/输入，任何整数 k 都不可能在 120 Hz 上均匀；现在会静默输出
   80 Hz、50% 间距不齐，日志里只有一行 `NOT-EXACT`。

---

## 2. 为什么"任意上限"需要新的控制律

设面板刷新 `R`，vblank `V = 1000/R`，平滑后的输入间隔 `T`，
`slots = T/V`（每个真实帧跨几个 vblank），`J` 是每真实帧最多输出几张。

- **fillable**：`slots ≤ J`。理论上可以让每个 vblank 一张，但 `slots` 是小数，
  常量 `k` 必然留下 `frac(slots)` 的空拍或重复。要么切 k（有成本），
  要么接受 ≤frac 的微顿挫 —— 这是算术，不是调参。
- **cap-limited**：`slots > J`。**无论怎么选都填不满**，能选的只有
  "常量 k + 间距 `slots/k` 尽量接近整数 vblank"，且必须停止按
  "每 vblank 一张"来要求 pacer。当前实现没有这个状态。
- **input-limited**：`slots < 1`。输入比面板快，只能 1:1，应提示关 FG。

双卡还多一条：`k` 张输出的 GPU 成本 ≈ `k × cost`，若 `k×cost > T`，
消费卡自己就是瓶颈。于是有效上限：

```
J_eff = min(J_user_or_dll, floor((T - overhead) / cost_per_output))
```

**结论**：控制律必须先判 regime，再在 regime 内选常量 k 并限制切换，
而不是继续用"每帧算一个 k 直接 apply"。

---

## 3. 目标与不变量

**目标**
1. 任意 `J ∈ [2, 9]`（含由 `--interp` 给出的容量上限）都能进入确定状态，
   不出现 `k` 来回抖、不出现"上限不够却假装填满"。
2. 视觉优先：**单位时间内"1 帧被显示两遍"的次数最少**；
   在残差允许的范围内尽量提高输出率。
3. 上限变化（用户改 `--interp`、DLL 上限不同、GPU 成本上升）时状态迁移可解释、
   可回退、不产生新的长顿挫。

**不变量（实现/验收都按它）**
- I1：`1 ≤ k ≤ J_eff`，`k = 1` 只出现在 input-limited；否则 `k ≥ 2`。
- I2：一个 epoch 内 `k` 只在**排空后**改变，且默认一次 ±1；
  升档可以越过驻留，降档必须满足驻留与持续条件。
- I3：`phase` 只按 `Commit(appliedK)` 结算，`|phase|` 有界。
- I4：cap-limited 下 `k` 不随 jitter 改变（只在残差改善超过阈值时换），
  且 `frameRenderTime = T`（绝不用 `k·V` 反压输入）。
- I5：`--fg-out-hz` 不能落成整 vblank 时，`Decision.exact == false`，
  日志必须说明"最近的可选速率"。
- I6：输入比面板快时不叠加生成（不允许把输出推过面板 >2%）。

---

## 4. 新控制律（`src/cadence_core.h`）

### 4.1 输入估计

```
ring[16] ← 原始到达间隔（>250 ms 的单帧丢给 hitch，不更新）
median  = ring 中位数
mean    = mean( clamp(sample, 3×median) )      // 右偏分布用均值，单帧尖峰被夹
T       = EMA(mean, alpha = 2/(smooth+1))      // smooth 默认 8（= --cadence-smooth）
```

- 不用单样本：WGC 的 ±1 vblank 量化会让 40 fps 读成 25/33/25 ms。
- 不用纯中位数：中位数系统性低估 12%（`cadence.cpp:218-240` 已记录），
  累加器会少要 12% 的帧、留下空拍。
- 复用/重复捕获帧（`Sample.reuse`）**不喂估计器、不推进相位**（与旧实现一致）。

### 4.2 双上限

```
targetHz = (--fg-out-hz > 1) ? min(outHz, R) : R
want     = slots × targetHz / R                 // 本帧"欠"几张（小数）
J        = min(--interp, DLL max) + 1
if (cost_per_output > 0)
    J = min(J, floor((T - overhead) / cost_per_output))
J_eff    = max(J, 2)
```

`cost_per_output` 由消费端统计（§7.4），默认 0 = 不使用成本上限。
DLL 上限只在 init 时读一次（`GetProperties.maxSupportedInterpolations`），
`--interp` 在此之上再压低，二者取小。

### 4.3 regime 状态机（带滞回）

```
if (slots < 1.0)                       regime = input-limited
else if (capLimited)                   regime = cap-limited
else                                   regime = fillable

capLimited 进入：want > J_eff + 0.35
capLimited 退出：want < J_eff - 0.35     // 0.35 vblank 的滞回防边界抖动
```

cap-limited 的**原因**单独记录（DLL/用户上限 vs 成本上限），日志与 OSD 用不同文案；
成本上限导致的 J 变化不改变面板侧的判断。

### 4.4 fillable：两种策略

**even（默认，视觉优先）**

```
kNeed = ceil(0.96 × targetHz × T / 1000)          // 旧 governor 的"不低于目标 96%"
kNeed = clamp(kNeed, 2, J_eff)
if (kNeed > k)          k = kNeed                 // 升档立即：绝不让输出塌下去
else if (kNeed < k)     k 仅当连续 downBatches(=10) 帧后才 -1
```

- 0.96 的 ceil 保证"输出略过量、由面板丢弃"，而不是留空拍；
- 升档立即、降档慢：宁可多要一张，也不要画面先塌一下；
- 一个 epoch 内不切换 → 不会每帧触发 `SetNumInterpolatedFrames`。

**fill（A/B，仅在切换成本被实测为 0 时启用）**

```
acc = phase + min(want, J_eff)
kRound = clamp(round(acc), 2, J_eff)
if (kRound != k && |acc - k| > deadband)   k += sign(kRound - k)
phase = acc - k        // Commit 时按 applied 结算，误差有界
```

死区把"每帧切换"变成"长跑"：λ=3.4 的输入产出 3,4,4,4…；死区 1.8 时
仿真里切换从 6.4/s 降到 2.1/s（§9 表 5）。

### 4.5 cap-limited：常量 k 的选法

```
候选 k ∈ [max(2, J_eff-2), J_eff]
res(k) = |slots/k - round(slots/k)|          // 间距离整 vblank 多远（vblank）
第一优先：res ≤ evenTolerance(0.25) 的候选里取最大的 k   // 够整齐就选更高输出
兜底：    res 最小（并列取更大的 k）
if (--fg-out-hz) k 还需落在 slots/(R/outHz) 附近
```

迁移规则：

```
if (best > k)   // 升档：新 k 仍"够整齐"就换（best_res ≤ tol 且不超过当前 0.15）
else            // 降档：必须 best_res + 0.10 < 当前 res
一次只走一格；进入 cap-limited 的那一帧允许立即跳（不受驻留限制）
```

例子（120 Hz，输入 21 fps，slots=5.71）：

| J | 选中 k | 输出 | 间距 | 节奏不齐（仿真） |
|---|---|---|---|---|
| 3 | 3 | 63.1 Hz | 1.90 vb | 11.1% |
| 4 | 3 | 63.1 Hz | 1.90 vb | 11.1% |
| 5 | 5 | 104.9 Hz | 1.14 vb | 14.4% |
| 6 | 6（fillable） | 119.3 Hz | 1.01 vb | 0.6% |

J=5 选择 5 而不是 3：两者残差都 ≤0.25，取更大的 k（输出率几乎翻倍）。
J=6 时 `want=5.71 < 6`，回到 fillable，even 直接给 6 张近满帧。

### 4.6 切换门限与记账

```
决策：d = Update(sample)            // 只算，不 apply
排空：canSwitch = cadence_present_drained() && 上一批呈现已追平
apply：仅当 canSwitch 且 (framesSinceSwitch ≥ dwell || 升档 urgent)
       fg_set_interp(d.k - 1)
记账：Commit(applied_k, ok)          // applied_k 可能与 d.k 不同
      phase += lam - applied_k       // 只减真正生效的
```

- `dwell = max(--cadence-hold, learnedDwell)`；默认 4，学习后可到 60。
- 升档越过驻留：塌下去比多一次切换更显眼；降档必须等驻留 + `downBatches`。
- 排空失败：`k` 保持 applied，误差留在 phase，下一帧继续尝试；
  `deferred` 计数进日志。
- `Commit` 的 `ok=false`（DLL 拒绝）同样不丢账。

### 4.7 预算（`frameRenderTime`）与输入 pacer

```
renderTime = T                                   // 默认：k 张要覆盖的真实间隔
if (feedWholeVblanks && regime == fillable)
    renderTime = k × V                           // A/B：2026-11-09 实测会正反馈，不默认
```

- cap-limited 与 input-limited **必须**用 `T`。用 `k×V` 会让输入 pacer
  （`frame.cpp:45-69`）按 `k×V` 反压输入，而输入间隔又在下一帧到达时才测量，
  形成"预算越大 → 量到的间隔越大 → k 越大"的正反馈（历史实测 xN 3.00→3.38）。
- 输入 pacer 本身改成按 ms 预算（`fg_input_pace_ms(renderTime)`）：
  cap-limited 下等价于"帧到了就提交"，不再把 `late` 计数打满。

### 4.8 切换成本学习（`NoteSwitchLoss`）

消费端已有 `g_cad_expected_out`（`cadence.cpp:274`）与
`GetLastPresentCount`，每次切换后比较"应出张数"与"实际呈现"：

```
lost = expected_out - presented              // 一次切换窗口内的差
core.NoteSwitchLoss(lost)                    // 连续 8 次的均值 > 0.5
  → learnedDwell += 1（上限 60）
  → fill 自动降级为 even
```

### 4.9 双卡成本上限（可选，默认关）

- 统计：`cost_per_output ≈ Δ(g_ms_fg) / Δ(presents)` 的 EMA，
  或直接用 `g_ms_fg / (interp+1)` 的窗口均值；
- 只在"消费卡是瓶颈"时启用：本帧 `g_ms_throttle + g_ms_present` 已接近 `T`，
  且降 k 后输出反而更接近目标（与 `--dyn-mfg` 的 30% 规则同源）。
- 启用后 `J_eff` 下降，走同一条 cap-limited 路径，**不新增模式**。

---

## 5. 参数

| 字段 | 默认 | 含义 | 对应用户开关 |
|---|---|---|---|
| `refreshHz` | 探测值 | 面板刷新率（`overlay_display_mode`） | — |
| `maxOutputFrames` | DLL max+1 | `J` 的硬上限 | `--interp N`（N=J-1） |
| `outHz` | 0 | 目标输出率；0=面板 | `--fg-out-hz` |
| `policy` | even | 视觉优先 / 逐帧补满（A/B） | `--cad-policy even\|fill` |
| `smooth` | 8 | 输入间隔 EMA 窗口 | `--cadence-smooth` |
| `deadband` | 0.60 | fill 的相位死区（帧） | `--cad-deadband`（新，A/B） |
| `minDwell` | 4 | 计数切换最小间隔（真实帧） | `--cadence-hold`（语义改为"最小"） |
| `downBatches` | 10 | even 降档需要的持续帧数 | 内部 |
| `capEnterHyst/ExitHyst` | 0.35 | cap-limited 滞回（vblank） | 内部 |
| `evenTolerance` | 0.25 | "够整齐"的残差上限（vblank） | 内部 |
| `evenImprove` | 0.10 | 降档必须改善的残差（vblank） | 内部 |
| `floorShare` | 0.96 | 输出不低于目标的比例 | 内部（旧 governor 语义） |
| `costPerOutputMs` | 0 | FG 卡单张输出成本；0=关 | `--cad-cost-cap`（新，A/B） |
| `feedWholeVblanks` | false | 预算给 k×V（A/B） | `--fg-cad-rt` / `--cad-batch-rt` |

`--cadence-source tick|mean` 在 P5 之前仍走旧路径（作为 A/B 保留）；新核心先只实现
`mean + EMA` 估计器（默认）。`governor` 与新核心 `even` 语义接近（降档窗口
24 帧 vs 10 帧，且新增 cap-limited 与成本上限），可以直接映射过去。

---

## 6. 任意上限下的操作流程

**启动（冷路径）**

1. `init_fg()` 读 DLL 上限与 `--interp`，得到 `J`；cadence 模式从 `k=2` 开始
   （避免旧实现"首帧 6x"的涌帧）。
2. 按 §4.2 取 `J_eff`；若 `J_eff = 2`，日志明确写"2x 上限"。
3. 首帧 `frameRenderTime = T`，不做估计器预热之外的任何切换。

**fillable（输入够快）**

4. 每帧算 `kNeed`；升档立即（一次可跨多档，但只在排空后），降档等 10 帧 + 驻留。
5. 稳定后 `switches/s ≈ 0`；日志 5 s 一行给出 `out Hz / spacing / uneven%`。

**进入 cap-limited（输入变慢或 J 被成本压低）**

6. `want > J_eff + 0.35` → 进入；当帧允许跳过驻留直接到目标 k。
7. 选常量 k（§4.5）；`frameRenderTime = T`；输入 pacer 按 ms 预算，不反压输入。
8. 日志（一次性 + 每 5 s）：
   `dyn-cad: cap-limited (want 5.71 > J 4) -> 3x, 63.1 Hz even, spacing 1.90 vb;`
   `raise --interp / lower the input rate / --cad-policy fill for 84 Hz`.
9. 输入恢复（`want < J_eff - 0.35`）→ 退出，回到 even；退出只允许 +1 或直接到 kNeed。

**任意上限取值的边界**

| 情况 | 行为 |
|---|---|
| `J=2`（2x，DLL 只允许 1 张） | cap-limited 很常见；只选 k=2，`spacing=slots/2`，不切换 |
| `J=3..8` | 按 §4.5；`k` 一定 ≤ J；不允许"假装填满" |
| `J` 大于实际需要 | fillable，even 直接给 `kNeed`，不会为了用满上限而多生成 |
| 输入 > 面板 | input-limited：`k=1`，一次性提示 `--no-fg` |
| `--fg-out-hz` 无法整除 | `exact=false`，输出最近可达速率并提示（如 40 fps→60 Hz 需 1.5x） |

**运行中改配置**

10. 用户改 `--interp`（UI 重启 consumer）→ 新 `J` 重新 `Configure`，从当前 k 夹到新上限；
    不重置学习值（与 `--dyn-mfg` 的 `Restart` 语义一致）。
11. 切换成本被学习到 >0.5 张/次 → 驻留加长、fill→even；日志说明原因。

**运行中改上限的平滑规则（关键）**

| 事件 | 处理 |
|---|---|
| `J` 变大（例如 `--interp 3 → 5`） | 调用 `Reconfigure`（保留学习值）；保持当前 k；若因此退出 cap-limited，则按 fillable 规则升档（+1/驻留，最多一次跨到 `kNeed`）；不重置相位与学习值 |
| `J` 变小（5 → 3） | `Reconfigure` 内部把 k 夹到 `min(k, J)`；同时在 `Update` 里 `overCap` 允许**排空后立即切换**（不等驻留，否则会向 DLL 要超过上限的 k）；夹完仍 cap-limited 时按 §4.5 立刻重选一次 |
| DLL 上限变化（换 `--fg-lib`） | 与 `J` 变化同路径；日志写 `J: 6 -> 4 (dll)` |
| 成本上限生效/解除 | 与 `J` 变化同路径；日志写 `J: 6 -> 3 (cost 4.2 ms/frame)`，且 `reason=cost` 与面板上限区分 |
| 暂停/加载画面（producer 停发） | `reuse` 帧不推进相位；`want` 不更新；恢复后按新 `T` 重新决策，不补发 |

“立即夹”是唯一允许跳过驻留的降档：向 DLL 申请超过它上限的 k 会直接失败，
比一次模型切换更糟。

---

## 7. 与 consumer 的接线（逐文件）

### 7.1 包含与状态

- `src/xess_mfg_consumer/consumer_internal.h:51` 旁边加
  `#include "cadence_core.h"`（与 `xefg_dynamic_count.h` 同一 include 根）。
- `src/xess_mfg_consumer/modules/state_overlay.cpp:344-356` 之后加：

```cpp
static mgpu_cad::CadenceCore g_cad_core;
static bool  g_cad_core_ready = false;
static UINT  g_cad_last_expected = 0;   // 切换成本统计用
```

### 7.2 初始化（`cadence.cpp:1034-1130` 内）

```cpp
mgpu_cad::Config cc;
cc.refreshHz       = static_cast<double>(g_refresh_hz);
cc.maxOutputFrames = static_cast<int>(g_cad_kmax ? g_cad_kmax : g_fg_interp_max) + 1;
cc.outHz           = g_fg_out_hz;
cc.policy          = g_cad_policy;        // --cad-policy，默认 even
cc.smooth          = g_cad_smooth;
cc.minDwell        = g_cad_hold_frames;   // --cadence-hold，默认 4
cc.deadband        = g_cad_deadband;
g_cad_core.Configure(cc);
g_cad_core_ready = true;
```

`--interp-cadence` 的起始 k 仍是 2（现有 `want_interp = 1`，见 `cadence.cpp:1094`）。

### 7.3 每帧（`frame.cpp:1302-1342`）

```cpp
if (g_interp_cadence && g_refresh_hz > 0 && !g_reuse_frame) {
    g_cad_input_ts_ms = ...;                       // 保持现有游戏时钟优先逻辑
    mgpu_cad::Sample s;
    s.nowMs     = t_r0;
    s.inputMs   = (g_arr_last_raw > 0.5) ? g_arr_last_raw : arr_mean_ms();
    s.waitMs    = g_ms_present;
    s.canSwitch = cadence_present_drained();
    const mgpu_cad::Decision d = g_cad_core.Update(s);
    cad_want  = static_cast<UINT>(std::max(d.k, 1));
    cad_apply = true;
}
...
if (cad_apply) {
    const UINT before = g_fg_interp_actual;
    if (cad_want <= 1) {
        // k=1 只有 input-limited 会给出：等价于 1X，走 fg_dyn_mfg_apply(0) 的路径
        // 或保持 2x 并一次性提示 --no-fg（推荐后者，避免开关 FG 的代价）。
    } else {
        fg_set_interp(cad_want - 1, "interp-cadence", /*quiet=*/true);
    }
    const bool ok = (g_fg_interp_actual + 1 == static_cast<int>(cad_want));
    g_cad_core.Commit(static_cast<int>(g_fg_interp_actual) + 1, ok);
    if (g_fg_interp_actual != before) { ++g_cad_switches; }
    g_cad_last_k = g_fg_interp_actual + 1;
}
```

**顺序不能变**：`Update` 在 `gpu_throttle()` 之前（与本帧 tag 同一决策），
`Commit` 必须在 `fg_set_interp()` 之后，否则学不到 DLL 的真实生效值。

### 7.4 切换成本与成本上限

- `g_cad_expected_out` 已存在（`cadence.cpp:274`）。在每次切换后的第 4 帧：

```cpp
UINT last = 0;
if (SUCCEEDED(g_swapchain->GetLastPresentCount(&last)) && g_cad_last_expected > 0) {
    const int lost = static_cast<int>(g_cad_last_expected) - static_cast<int>(last);
    g_cad_core.NoteSwitchLoss(lost);       // 内部做 8 次平均与阈值判断
}
g_cad_last_expected = static_cast<UINT>(g_cad_expected_out);
```

- 成本：窗口结束时 `cost = g_ms_fg / max(presents, 1)`；
  仅当 `g_ms_throttle + g_ms_present > 0.8 × T` 时 `SetCostPerOutputMs(cost)`，
  否则 `SetCostPerOutputMs(0)`（关闭成本上限）。

### 7.5 预算与 pacer

- `fg_render_time_ms()`（`cadence.cpp:766`）在 cadence 分支直接返回
  `d.renderTimeMs`（缓存到 `g_cad_render_ms`），不再走 `g_cad_last_tin`/`k×V` 分支。
- `fg_input_pace()`（`frame.cpp:45-69`）增加 ms 版本：

```cpp
static void fg_input_pace_ms(double budget_ms);   // 内部同样是 vblank 槽位 + 自旋
...
if (g_interp_cadence) { fg_input_pace_ms(g_cad_render_ms); }
```

  旧的 `fg_input_pace(k)` 保留给 A/B。
- `--fg-cad-rt` / `--cad-batch-rt` 映射到 `Config.feedWholeVblanks`（默认 false）。

### 7.6 日志与 OSD

`frame.cpp:2498-2567` 的 cadence 行改为打印核心状态，并新增一行：

```
cadence: J=4 even cap-limited k=3 | want 5.71 slots 5.71 | out 63.1 Hz (1.90 vb)
         uneven 11.1%  switches 0.02/s  deferred 0  cost 0.0 ms  exact=no
```

`unexact`/`cap-limited` 各只提示一次，避免刷屏；`exact=no` 时给出
"nearest: 84 Hz (k=4, 1.43 vb)"。

---

## 8. 观测、判读与验收

### 8.1 必须进日志/OSD 的字段

| 字段 | 定义 | 健康值 |
|---|---|---|
| `out Hz` | 窗口内实际呈现帧率 | fillable 时 ≈ 目标；cap-limited 时 ≈ `k/T` |
| `spacing` | 相邻输出的 vblank 间隔均值 | 1.00（满帧）或 `slots/k` |
| `uneven %` | 间距与 `round(mean)` 相差 ≥1 vblank 的比例 | 稳态 <10%；锁定输入 <1% |
| `dbl %` | 同一 vblank 被塞两张而丢弃的比例 | 稳态 <10% |
| `switches/s` | `SetNumInterpolatedFrames` 实际切换频率 | <0.2/s（even）；fill 视成本 |
| `deferred` | 因排空/拒绝被推迟的切换次数 | 不持续增长 |
| `lost/switch` | 每次切换丢的输出张数（学习值） | 0 才允许 fill |
| `cap %` | cap-limited 时间占比 | 与输入帧率、J 一致即可 |
| `exact` | 间距是否整 vblank | `--fg-out-hz` 时尤其要看 |

### 8.2 验收（双卡、真实游戏）

1. **锁定输入 40 fps / 120 Hz / J=4**：`uneven <1%`、`dbl <1%`、`switches ≈0`；
   与现状持平（这是基线，不能退步）。
2. **波动输入 25–40 ms / J=4**：`uneven ≤ 现状 governor`（仿真 6.5% vs 6.35%），
   `switches <0.2/s`，10 分钟 `cap%` 无单调漂移。
3. **任意上限**：同一段录像分别用 J=2,3,4,5,6 跑，`k ≤ J` 恒成立；
   每个 J 都必须在 30 s 内进入稳定状态（`switches/s` 收敛，`k` 不再变化）。
4. **cap-limited**：21 fps / J=4 时必须是 63 Hz、`spacing≈1.90`、`switches≈0`；
   现状是 84 Hz、42.7% 不齐（仿真），实机看 `HITCH` 与 `SLOW frame` 应下降。
5. **成本上限**：J=8 + 消费卡接近满载时，`J_eff` 自动下降且 `cap%` 上升，
   `gpu/present` 不出现持续 2 倍于窗口均值的尖峰。
6. **回归**：`xefg` 的 `drop/not in slots`、`HITCH` 不超过基线 +10%；
   `--no-fg`、固定 `--interp` 路径完全不受影响（新核心只在 cadence 分支）。

---

## 9. 仿真结果（`tests/cadence_core/sim.cpp`）

模型：批次把 k 张均匀铺在 `renderTime` 上；每张向上取整到 vblank，
同 vblank 第二张丢弃（dbl）；切换时该批少出一张（loss=1）。
`uneven` = 间距与均值取整相差 ≥1 vblank 的比例。

### 9.1 J=4，切换成本 1（现状 pacer 的实测行为）

| 场景 | 控制器 | out Hz | uneven | dbl | sw/s |
|---|---|---|---|---|---|
| locked 40（3.00 vb） | gov / bres / even / fill | 120.0 | 0.00% | 0.00% | ≈0 |
| 35 fps（3.43 vb） | gov | 120.0 | 0.00% | 14.28% | 0.03 |
|  | bres | 90.1 | 33.29% | 0.00% | **31.84** |
|  | even | 120.0 | 0.00% | 14.28% | 0.03 |
|  | fill | 106.0 | 12.38% | 7.00% | 6.40 |
| 波动 25–40 ms | gov | 112.9 | 6.35% | 8.69% | 0.02 |
|  | bres | 92.9 | 20.98% | 7.38% | **15.34** |
|  | even | 112.7 | 6.50% | 8.83% | 0.02 |
|  | fill | 105.0 | 11.93% | 7.68% | 6.56 |
| 突发长帧 | gov | 96.2 | 9.90% | 14.52% | 0.02 |
|  | **even** | 99.9 | **4.39%** | 11.17% | 0.02 |
| 21 fps（5.71 vb，cap-limited） | gov | 84.1 | **42.68%** | 0.00% | 0.02 |
|  | **even** | 63.1 | **11.06%** | 0.00% | 0.02 |
| 15 fps（8.00 vb，cap-limited） | gov | 60.1 | 5.00% | 0.00% | 0.01 |
|  | even | 60.1 | 4.85% | 0.00% | 0.02 |

结论：

- `bres`（每帧相位）在切换成本为 1 时**全面退化**（31.8 次/秒切换、33% 不齐），
  这正是它不能做默认的原因；新 `fill` 用死区把切换压到 2~6 次/秒后可用，
  但仍不如 `even`。
- `even` 在 cap-limited 下把现状的 42.7% 不齐降到 11.1%；
  代价是输出从 84 Hz 降到 63 Hz —— 换来的是"每帧都按时"，而不是"多送几张但错拍"。
- 35 fps 的 14.28% `dbl` 是整数 k 的算术下界（slots=3.43 → k=4 过量 0.43/帧）；
  要真正 1:1 只能让切换免费（见 9.3）或锁输入（`--lock-auto`）。

### 9.2 上限扫描（switchLoss=1）

| 输入 | J | out Hz | spacing | uneven | cap% |
|---|---|---|---|---|---|
| 21 fps | 3 | 63.1 | 1.90 | 11.06% | 100 |
| 21 fps | 4 | 63.1 | 1.90 | 11.06% | 100 |
| 21 fps | 5 | 104.9 | 1.14 | 14.40% | 100 |
| 21 fps | 6 | 119.3 | 1.01 | 0.63% | 0 |
| 15 fps | 4 | 60.1 | 2.00 | 4.85% | 100 |
| 15 fps | 6 | 60.1 | 2.00 | 4.85% | 100 |
| 15 fps | 8 | 118.6 | 1.01 | 1.21% | 0 |

J=5 时选 5 而不是 3：两者残差都 ≤0.25 vblank，取更大的 k（输出率 ×1.66）。
J=6 时 `want=5.71<6`，自动回到 fillable，输出 119 Hz。

### 9.3 切换成本敏感性

| 场景 | 控制器 | loss=0 | loss=1 | loss=2 |
|---|---|---|---|---|
| 35 fps | even | 120.0 / dbl 14.28% | 120.0 / dbl 14.28% | 120.0 / dbl 14.28% |
| 35 fps | fill | 112.0 / uneven 7.15% | 106.0 / 12.38% | 100.0 / 12.15% |
| 波动 | fill | 109.7 / 9.33% | 105.0 / 11.93% | 99.0 / 12.77% |

即：**只有 loss ≈ 0 时 fill 才比 even 好**。这正是 `NoteSwitchLoss` 自动降级的依据。

### 9.4 死区扫描（fill，J=4，loss=1）

| 输入 | deadband | out Hz | uneven | sw/s | lost |
|---|---|---|---|---|---|
| 波动 25–40 | 0.5 | 104.8 | 12.13% | 6.70 | 286 |
|  | 0.8 | 105.5 | 11.50% | 5.90 | 252 |
|  | 1.2 | 107.6 | 10.51% | 3.56 | 152 |
|  | 1.8 | **108.3** | **10.01%** | **2.08** | **89** |

死区是"切换次数 ↔ 平均误差"的直接旋钮；默认 0.6（保守），A/B 时可放大到 1.8。

### 9.5 结构检查（全部通过）

```
[PASS] 任意 J：输出帧率不超过面板（<= 102%）
[PASS] slow15 + J=2/3/4/5/6 识别为 cap-limited（capPct 100%）
[PASS] locked 40 + J=4：计数切换 < 1 次/秒
[PASS] locked 40 + J=4：节奏不齐与重复帧均 < 1%
[PASS] fill：相位有界（|phase| < 6 帧）
[PASS] 输入 200fps + J=4：不会把输出抬过面板
[PASS] --fg-out-hz 60 + 40 fps：标记为无整 vblank 节奏（exact=false）
[PASS] 成本上限：J=8 + 20 ms/张 -> 自动降到 3 张（输出 < 70 Hz）
[PASS] 运行中 J 6->3：之后不再出现 k>3（立即夹住）
[PASS] 运行中 J 6->3：输出回落到 cap-limited 速率
[PASS] 冷启动 400ms 首帧：估计在第 2 个样本回到真实量级
[PASS] reset 15fps->60fps：重新起锚（out >100 Hz，p95 <=2 vb）
```

运行：

```bash
tests/cadence_core/run.sh          # Linux / WSL
tests\cadence_core\build.bat       # Windows（VS 开发者命令行）
```

---

## 10. 分阶段落地与回滚

| 阶段 | 内容 | 风险 | 回滚 |
|---|---|---|---|
| P0（已完成） | `cadence_core.h` + 仿真 + 本文档 | 无（未接线） | — |
| P1 | 接线但**影子模式**：每帧 `Update`，只打日志不 apply | 极低 | 删除调用 |
| P2 | `--cadence-source core`（opt-in），默认仍是 governor | 低 | 切回 governor |
| P3 | `--interp-cadence` 默认 core，governor 作为 A/B | 中 | `--cadence-source governor` |
| P4 | 打开切换成本学习 + 成本上限；退役 `--cadence-hold` 语义 | 中 | `--cad-cost-cap 0`、`--cad-policy even` |
| P5 | 删除 `cadence_tick_slots()` / mean 相位分支，收敛到单一控制律 | 低 | 保留 tag |

每个阶段都用 §8.2 的 1/2/4 三条验收，任一退步即回滚上一阶段。
P1 的影子日志尤其重要：它能回答"实机上 `uneven` 与 `dbl` 到底是多少"，
而不改变任何画面。

---

## 11. 已知边界与非目标

- **MFG 开启时不使用 VRR**。NVIDIA 的 MFG 指引与本仓实测都指向"关掉 VRR、
  自己控制全部节奏"：XeFG/XeLL 的内部 pacer 假设面板是固定 vblank 网格，
  VRR 会让刷新率变成第二个控制器（详见 §12.6）。因此本文把 `R` 当作固定值；
  VRR 只作为"FG 关闭 / 1X 原生路径"的选项，不进入动态逐帧的判决。
- **不合并 `--dyn-mfg`**。`--dyn-mfg` 是"追呈现帧率目标"的控制器
  （`docs/design/FG_CLAIM.md`、`cadence.cpp:855-954`），与"按输入节奏补帧"
  目标不同；二者共用 `SetNumInterpolatedFrames` 与切换成本学习即可。
- **不解决 MV 闪烁**。3x/4x 复用同一 MV 集导致的时间分数不一致
  （`docs/findings/FINAL_CONFIG.md` 5.17.1）是画质问题，控制器只能减少切换次数。
- **切换成本是学出来的**，不是常数；若某版 pacer 让切换免费，`fill` 会自动变优，
  无需改代码。
- **k=1** 只用于 input-limited 的提示；不在这里做 1X 的实时开关
  （XeFG 在 1X↔2X 之间会重新初始化，代价远大于一次计数切换）。

> 相邻参考：Magpie 的 XeSS-FG pacing 实现（过期时隙不追加、启动估计
> min-of-two、只归因自己的等待、逐输出 p50/p95/p99）见
> [`MAGPIE_FG_PACING_REFERENCE.md`](MAGPIE_FG_PACING_REFERENCE.md)。
> 本文负责"选多少"，那份文档负责"怎么等"。

---

## 12. 理论最优形态（综合结论）

### 12.1 把问题写成整数规划

固定刷新 `R`（用 double，兼容 59.94）、`V = 1000/R`、平滑后的输入间隔 `T`。
每帧输出的**显示保持时长只能是整数个 vblank**，所以真正的自由度只有两个整数：

- `m ≥ 1`：输出间距 = `m` 个 vblank，输出帧率 = `R/m`；
- `k ∈ [1, J_eff]`：每个真实输入帧输出几张。

理想关系是 `T = k·m·V`。偏离量 `δ = T/V − k·m`（vblank）就是"每批多/少出来的
显示时隙"，它以"某张被多显示一个 vblank"的形式出现。定义**长保持率**

```
q = |δ| / (k·m)          // 被多保持一个 vblank 的输出占比
```

`q` 是平滑度的直接度量（每秒长保持次数 ≈ `q · R/m`）。于是问题可以写成：

```
minimize   W_s · q
         + W_l · (k−1) · T/2          // 插帧带来的延迟（越小越好）
         + W_a · (F_in − R/(k·m))₊    // 输入被节流/丢弃的比例（只在主动 pace 输入时）
         + W_f · (F_target − R/m)₊    // 达不到目标帧率的惩罚
subject to 1 ≤ k ≤ J_eff(T)，m ∈ ℕ，R/m ≤ R
          J_eff = min(--interp+1, DLL max+1, floor((T−o)/c))
```

`c` = FG 卡单张输出成本，`o` = 每批固定开销（双卡特有）。`W_*` 是用户偏好权重；
默认取"平滑 > 帧率 > 延迟"。

**三条支配性事实**（决定了"没有单一最优点，只有 Pareto 前沿"）：

1. 固定刷新下，只要 `T/V` 不能被 `m` 整除，任何**常量** `(m,k)` 都有 `q>0`；
   这是整数格点的算术下界，不是控制律能消除的。
2. 让 `q=0` 只有两种可用途径（**MFG 开启时必须关掉 VRR**，见 §12.6）：
   - **零成本切换 k**：按 Bresenham 在 `k, k+1` 间切，`q=0`，
     但每次切换丢 `L` 张输出；只有实测 `L≈0` 才占优；
   - **主动 pace 输入到整数格**：把输入压到 `B = R/(k·m)`，`q=0`，
     代价是最多 `1/B` 的输入延迟 + 丢弃 `F_in − B` 的源帧。
3. 由 1、2 可得：**"满帧率 + 任意输入 + 完美均匀"三者不可兼得**。
   任意方案都是在 `q`、延迟、输入保真、实现成本之间选一个点。
   因此"理论最优"= 在上述整数规划下、按权重取最小代价的那个点。
4. **VRR 不是第三条路，而是第二个控制器**：它让 `R` 随我们的提交间隔变化，
   而 XeFG/XeLL 的 pacer 又按固定 vblank 排批；两个闭环互相追，实测比
   "固定刷新 + 自己控制"更差（§12.6）。所以 MFG 路径把 VRR 排除在决策之外，
   只在 FG 关闭时保留。

### 12.2 最优解的结构：一个三层控制器

```
L0 显示模式（MFG 开启时）
   VRR 关闭，R 固定为面板最高刷新率；整数格 (m, k) 强制
   （FG 关闭 / 1X 原生路径才允许 VRR pass-through）

L1 输入层（唯一的"延迟预算"旋钮 Λ）
   机会式量化：若把本帧等到下一个 vblank 边界的等待 <= Λ，就等；
   否则立即提交。只有这一层能改变输入节奏，且从不强制。

L2 计数层（epoch 常量 + 自适应死区）
   每个 epoch 选一对 (m, k)，只在排空后的 batch 边界改变；
   切换代价 L 实测学习：L>0 时加长驻留并退回常量 k；
   L≈0 时才允许 Bresenham 逐帧切 k。

L3 上限层
   J_eff = min(DLL, --interp, 成本上限)；k > J_eff 时提高 m（降输出率）
   而不是继续压 k，把"上限不够"变成"均匀的低帧率"。
```

`m` 的解析解（**平滑优先**分支：在上限内取"能装下的最小间距"，
代价是可能牺牲输出率）：

```
m = max(1, ceil( (T/V) / J_eff ))         // 最小的 m，使 k 仍 <= J_eff
k = clamp(round( (T/V) / m ), 1, J_eff)
```

若用户把帧率权重提高，则枚举候选集
`m ∈ [1, max(1, ceil(slots))]`、`k ∈ {floor, round, ceil}(slots/m)`，
按 §12.1 的代价函数选。例如 21 fps/J=5：平滑优先选 `m=2,k=3`（60 Hz，q≈5%），
帧率优先选 `m=1,k=5`（105 Hz，q≈14%）。

这就是"任意上限"的统一答案：

| 输入 | 面板 | J | m | k | 输出 | q | 说明 |
|---|---|---|---|---|---|---|---|
| 30 fps | 240 Hz | 4 | 2 | 4 | 120 Hz | **0** | 8 vblank/输入，4×4 = 16 vblank |
| 40 fps | 120 Hz | 4 | 1 | 3 | 120 Hz | **0** | 3.00 vblank 整除 |
| 35 fps | 120 Hz | 4 | 1 | 3 或 4 | 120 Hz | 14.3% | 3.43 vblank，整数格无解 |
| 21 fps | 120 Hz | 4 | 2 | 3 | 60 Hz | **9.8%** | 5.71 vblank；比 m=1,k=4 的 42.7% 好得多 |
| 21 fps | 120 Hz | 5 | 1 | 5 | 105 Hz | 14.2% | 上限够时选更高帧率 |
| 21 fps | 120 Hz | 6 | 1 | 6 | 120 Hz | 4.8% | 5.71→6，残差小 |
| 15 fps | 120 Hz | 4 | 2 | 4 | 60 Hz | **0** | 8.00 vblank 整除 |
| 100 fps | 120 Hz | 4 | 1 | 1 | 100 Hz | 0 | 输入快于上限：1:1，建议 `--no-fg` |

注意 21 fps/J=4 这一行：`m=1,k=4` 给出 84 Hz 但 `q=42.7%`（约 36 次/秒长保持），
`m=2,k=3` 给出 60 Hz 且 `q≈9.8%`（约 6 次/秒）。**降低输出率反而更流畅**，
这是 `(m,k)` 二维优化相对"只调 k"的核心收益（当前 `cadence_core.h` 只隐式地
用残差容差选 k，没有把 `m` 显式化）。表中的 `q` 按平滑后的 `T` 计算；
实机输入抖动会让观测到的"不齐比例"再高 3~6 个百分点（仿真里 21 fps 是 11%）。

### 12.3 为什么"死区自适应 = 最优"

把"常量 k 的长保持"与"切换 k 的丢帧"写成每秒代价：

```
C_const(θ) ≈ (R/m) · |slots − k m| / (k m)      // 不切换，残差转成长保持
C_switch   ≈ L · S(θ)                           // S ≈ |frac|·F_in / θ
```

对 `θ` 求极小：`θ* ∝ sqrt(L · |frac| · F_in · k m / (R/m))`。
结论：**死区应当随"实测切换成本 L"增长**，而不是固定 0.6 帧。
`cadence_core.h` 的 `NoteSwitchLoss → learnedDwell` 与 `deadband`
就是这个最优点估计器的雏形；剩下的工作只是把 `θ` 也接到 `L` 上。

### 12.4 默认推荐落点（"理论最优"的工程近似）

1. **MFG 开启时：关闭 VRR，固定刷新 + 自己控制全部节奏**。
   面板跑最高固定刷新率，`(m,k)` 整数格是本方案的骨架；
   不要再让显示器/驱动当第二个控制器（§12.6）。
2. **可接受 ≤Λ 延迟**：`Λ` 默认 **1~2 ms**（约 0.1~0.25 vblank），
   机会式量化输入；`m = max(1, ceil(slots/J_eff))`，`k = round(slots/m)`；
   `q` 按 §12.2 的表落在 0~11%。
3. **最低延迟**：`Λ = 0`，不量化；`m=1`，`k` 取常量 epoch，
   接受 `q = |slots−k|/k`（典型 10~15%），并把这部分记为"长保持率"而不是"卡顿"。
4. **上限不够**（`slots > J_eff`，含成本上限）：按 §12.2 提高 `m`，
   **不要**用 `m=1` 硬压 `k=J` 去换那点帧率。
5. **输入快于面板**：`k=1`，提示 `--no-fg`；不叠加生成。
6. **任意用户目标 F**：先吸附到最近的 `R/m`（`m = round(R/F)`），
   再算 `k`；若 `F > J_eff/T` 则如实报告"最高只能 J_eff·F_in"。
7. **VRR 只在 FG 关闭 / 1X 时可用**：此时没有内部 pacer 与它竞争，
   VRR 才是净收益；一旦启用 MFG，必须回到固定刷新。

### 12.6 为什么 MFG 下不开 VRR

NVIDIA 的 MFG 指引推荐使用 MFG 时不开 VRR；本仓实测也一致：开 VRR 明显不如
"自己控制所有节奏"。机制上这不是玄学，而是**两个闭环互相追**：

1. **pacer 假设固定 vblank**：XeFG 的 pacer 把一批 `k` 张按固定步长铺进
   vblank 网格；VRR 让 vblank 长度随提交时间变化，"一步 = 一个 vblank"
   的映射不再成立，pacer 只能用更多 hold/duplicate 去吸收。
2. **刷新率成为第二个控制器**：我们的 `m/k` 决策基于 `R`；VRR 下 `R` 反过来
   由我们的提交间隔决定 → 提交早一点，刷新率升高 → 下一批的 `T/V` 变小 →
   `k` 变小 → 输出变慢 → 刷新率下降。和 Magpie 修掉的那条
   "等待变长 → 采样更稀 → 估计更大"正反馈同构。
3. **LFC/倍帧边界**：MFG 的输出可能落到 VRR 下限附近，驱动开始倍帧；
   倍帧与 FG 的插值帧叠加，画面出现"节奏不均"，而固定刷新没有这个模式。
4. **测量口径被破坏**：VRR 下"显示事件间隔"随刷新率变，`p50/p95/p99`
   不再能区分"我们的调度问题"和"面板在跟"，排障成本显著上升。
5. **延迟不可重复**：VRR 的 present 排队行为随刷新率变化，
   `click→photons` 的方差变大；固定刷新下的延迟才是可比较、可优化的。

所以本方案的最终排序是：

```
固定刷新 + 单一节奏所有者（XeFG/XeLL + 我们的 (m,k) 格）  >  VRR + MFG
```

只有在 **FG 关闭（1X / 原生）** 时，VRR 才重新成为"跟随输入"的净收益选项。
如果用户坚持 VRR + MFG，至少要做到：面板刷新范围固定在上限附近
（例如 120 Hz 面板锁 100–120）、关闭驱动的 LFC 倍帧、只用 `m=1` 且让
输出略低于刷新率（避免越过上限），并接受 `q` 变大——这属于"降级可用"，
不是最优路径。

### 12.5 还缺的三块（按收益排序）

1. **把 `m` 变成显式决策**（`cadence_core.h` 目前只有 `k`）：
   `Decision` 增加 `m`，`renderTime = k·m·V`（机会式）或 `T`（不量化），
   `frameRenderTime`/pacer 预算随之统一。
2. **机会式 pacer**：`fg_input_pace` 从"按 k·vblank 硬等"改为
   "在 Λ 预算内等到下一个 vblank 边界，否则立即提交；过期不补"（Magpie B1），
   并把 `Λ` 做成 UI/参数。
3. **切换成本与 GPU 成本的闭环**：`NoteSwitchLoss` 已有；补
   `SetCostPerOutputMs` 的窗口统计，使 `J_eff` 真正随 `T` 与 `c` 变化。
   （观测用 p50/p95/p99 + `q` + `cap%`，见 §8。）
4. **VRR 状态检查**：启动时读显示器/交换链是否处于 VRR（或用户是否开了
   Adaptive-Sync），MFG 启用时明确告警"本方案按固定刷新工作"；
   不自动改系统设置，只在日志/UI 里给出可操作建议。

---

## 13. 落地记录（2026-11-14，consumer + CP2077 部署）

### 13.1 代码

- `src/cadence_core.h`：新增 `kPolicyOptimal`，`Decision` 增加 `m/paced/q/outputHz`，
  实现 §12.1 的候选枚举与代价函数；`Commit` 按真正生效的 k 采纳 `(m,paced)`；
  `Sample.reset` 与会话复位；bootstrap 两个样本取 min。
- `src/xess_mfg_consumer/`：
  - `consumer_internal.h` 引入 `cadence_core.h`；
  - `state_overlay.cpp` 增加控制器与 CLI 状态（`g_cad_policy/…`）；
  - `main.cpp` 新增 `--cad-policy optimal|even|fill`、`--cad-latency-budget N`、
    `--cad-allow-pacing off`、`--cad-cost-cap N`；
  - `cadence.cpp`：`cad_core_configure()` 在 `init_fg()` 末尾配置；
    `fg_render_time_ms()` 直接返回决策的 `renderTime`（paced = `k·m·V`，否则 `T`）；
  - `frame.cpp`：每真实帧调用 `Update`/`Commit`；`fg_input_pace_ms()` 实现
    机会式量化（等待 ≤ Λ 才等、过期不补、不追赶）；日志增加
    `cadence core: k/m/paced/q/out/render/policy` 一行。
- 默认策略 `optimal`，Λ=2 ms，cost cap 关闭（需要时用 `--cad-cost-cap`）。

### 13.2 构建与部署

- 构建：`./scripts/build_consumer.sh`（WSL → `cl`，含 4 个 HLSL 入口校验）。
  产物 `build/xess_mfg_consumer.exe`，SHA-256
  `ff990c2eef51d515de73b35e5cdcc45668054452b416c3bb7835546f77b79f0d`。
- 部署目标：`E:\SteamLibrary\steamapps\common\Cyberpunk 2077\bin\x64`
  - 旧 exe 备份：`xess_mfg_consumer.exe.bak_20261114_optimal`
    （SHA-256 `4bd791e5…5ddd68`）；
  - 新入口：`run_consumer_optimal.bat`（默认，pub-fifo）、
    `run_consumer_optimal_oldpath.bat`（关闭 FIFO/预拷）、
    `run_consumer_optimal_lowlat.bat`（Λ=0、不量化）；
  - 测试说明已追加到游戏目录的 `README_DYNAMIC_CADENCE_TEST.md`。
- 本机 `fakerFG\libxess_fg.dll` + `--unlock-mfg` 实测 `maxSupportedInterpolations = 3`，
  即 **J=4（4x）**；`--interp 5` 会被夹到 3。测试 bat 已改为 `--interp 3`。
- 启动时日志会打印：
  `cadence core: policy=optimal J=… panel=…Hz outHz=… latency-budget=2.0ms pacing=1 …`
  以及一次 `MFG assumes a FIXED refresh - disable VRR/Adaptive-Sync`。

### 13.3 验收（实机）

| 输入 | 期望决策 | 期望现象 |
|---|---|---|
| 40 fps / 120 Hz / J=4 | `m=1,k=3,paced=0`（q≈0） | 120 Hz，`switches` 很小 |
| 30 fps / 240 Hz / J=4 | `m=2,k=4`（q≈0） | 120 Hz，每张保持 2 vblank |
| 21 fps / 120 Hz / J=4 | `m=2,k=3`（q≈10%） | ~60 Hz，无 84 Hz 的 43% 长保持 |
| 15 fps / 120 Hz / J=4 | `m=2,k=4`（q≈0） | 60 Hz，均匀 |
| 35 fps / 120 Hz / J=4 | `m=1,k=4`（q≈14%） | 120 Hz；整数格无解的下界 |
| 输入 > 面板 | 提示 `--no-fg` | 不叠加生成 |
| Λ=6 ms / 35 fps | `paced=1,m=1,k=4` | 120 Hz、q=0（丢 14% 源帧） |

判读以日志中的 `q / switches / late / cap%` 为准；"是否卡顿"仍以
PresentMon/ETW 的显示事件为最终口径（provider 提交间隔不是显示事件）。

### 13.4 第一轮四组日志分析（2026-11-14 23:51–23:54）

| 日志 | 决策 | 实际 show | load | 结论 |
|---|---|---|---|---|
| `mgpu_consumer_optimal.log` | 全程 `k=2,m=1`，q≈0.38 | 82（40–102） | 76% / 110% | 输入约 44 fps（2.7 vblank），本应 k=3→120 Hz；被钉在 2x |
| `mgpu_consumer_optimal_oldpath.log` | 同上 | 85（40–102） | 43% / 91% | 传输路径不是瓶颈，控制器同样停在 2x |
| `mgpu_consumer_optimal_lowlat.log` | `k=4`，paced=0 | 112（40–121） | 111% / 128% | 4x 请求成功但 XeFG 大量 `ERR_OTHER`、`presented=1`，`presents=124/160` → 4K 4x 不可实现 |
| `mgpu_consumer.log`（固定 3x 基线） | 固定 interp=2 | 114（40–120） | 50% / 105% | **3x 是可行甜点** |

由此得到三个修复：

1. **可实现性反馈**：`NoteOutputShortfall()` 连续 3 次 `presented < k` 就把
   learned k 上限降一档（4→3→2）；连续 400 次满档才试探升回。
2. **排空判断**：`GetLastPresentCount` 计的是 Present 调用，不是生成张数；
   改用 `g_present_id` 比较（旧的“按 k 累加”会让闸门永远不开）。
3. **诊断字段**：`j / learnedJ / reason / can / reuse / slots / T` 进入每窗口一行，
   用于定位“为什么停在 2x”。

新构建 SHA-256 `c125f16713d5…19cc6d59` 已部署；仿真新增 shortfall 两项检查（22 项全过）。

---


### 13.5 第二轮结果（新 exe `c125f167…`）

- 73 个窗口全部 `k=3,m=1,paced=0`，`j=4/learnedJ=4`，`can=1 reuse=0`，
  `switches` 合计 1（启动 2→3），`q` 中位 0.002 / p95 0.073，`out≈119 Hz`，`show≈116.8`；
- `load` 均值 86% / 峰值 97%；5 次 `HITCH`（gap 52–62 ms，输入侧长帧）；
- 启动窗口 1 次 `OUTPUT FRAMES MISSING`（116/120）与 1 次 `ERR_OTHER`，之后消失；
- 4x 未被请求：25 ms = 3.0 vblank，k=3 已 q=0；k=4 会过订，因此 shortfall 未触发。

结论：**排空闸门的口径修正（Present 调用数）是“2x 卡死”的直接修复**；
控制器已稳定在 3x/120 Hz。剩余 `load` 86–97% 属传输/GPU 余量，
`HITCH` 的 50–60 ms 长帧来自输入侧。


### 13.6 第三轮：同场景 oldpath 对照 + 提高游戏负载

**同场景（游戏 ~40 fps，J=4）**

| 路径 | k/m | q 中位/p95 | out | show | load 均值/峰 | HITCH | switches |
|---|---|---|---|---|---|---|---|
| pub-fifo（13.5） | 3/1 | 0.002 / 0.073 | 119 | 116.8 | 86% / 97% | 5 | 1 |
| **oldpath**（无 FIFO/预拷） | 3/1 | 0.002 / 0.040 | 119.3 | 116.4 | 84.6% / 98% | **0** | 1 |

两者基本等价：传输路径不是瓶颈，3x 的 116/120 差距来自 Present/拷贝余量，
不是控制器。oldpath 的 q 更稳、HITCH 更少，可作为长跑 A/B 的候选默认。

**提高游戏负载（游戏 ~24 fps）**

- `game(vb)=5x`（41.7 ms），`arr(vb)` 3–7 vblank；`slots=4..6`，J=4；
- 决策 24 窗口 `k=4`、5 窗口 `k=3,m=2`、1 窗口 `k=3,m=1`，`switches=9`；
- `q` 均值 0.177 / p95 0.314，`out≈98`，`show≈92`，`load` 均值 30% / 峰 77%；
- 这是**算术上限**：24 fps × 4 = 96 Hz < 120，`q ≈ (slots−4)/4`；
  任何 m 更大的组合都更差（m=2,k=4 → 间距 2.5，q=0.5）。
- `MISSING` 窗口部分是旧 `expected = frames×(interp+1)` 在换档窗口的误报；
  已改为“本窗口实际应用的 k 之和”（新 exe `26cbbc00…`）。`xN≈3.75–4.00`
  说明 XeFG 实际出了接近 4 张，不是故障。

结论：负载升高时控制器正确地进入 cap-limited 并保持 k=4；要回到 120 Hz，
只能让游戏 ≥30 fps（降画质/分辨率）或提高可实现倍率（本机 DLL 上限 4x）。


### 13.7 第四轮：42.7 fps 场景的残余 q（卡顿来源）

| 指标 | optimal（pub-fifo） | optimal_oldpath |
|---|---|---|
| 游戏帧时间 | 均值 23.4 ms（37.1–50.8 fps，slots≈2.8） | 23.5 ms（相同） |
| 决策 | 全程 `k=3,m=1,paced=0` | 相同 |
| q 均值 / p95 | 0.079 / 0.201 | 0.085 / 0.221 |
| show | 115.3 | 112.8 |
| presents / vblanks | 113.5 / 114.5（每窗少 3–5 张） | 110.9 / 113.0 |
| load 均值/峰 | 73.6% / 106% | 71.4% / 109% |
| HITCH | 1 | 2 |

两条路径几乎相同，**卡顿来自小数 vblank**：

- 输入在 2.4–3.2 vblank 间波动，`k=3` 的 q = |slots−3|/3，均值 8%、
  50 fps 窗口到 20%（每 5 张就有 1 张多保持一个 vblank）；
- `k=2` 在同一区间 q 也在 20% 左右，但输出只有 ~100 Hz；常量的 (m,k)
  在这个输入上**没有 q=0 的解**；
- 另有每窗 3–5 张（3–4%）的 Present/拷贝缺帧，load 峰值 106–109%。

两种把 q 压到 0 的办法（都已提供测试脚本）：

1. **`--lock 40`**：把接受的捕获帧压到 25 ms 网格（丢早到的帧，不等待），
   `k=3` → 120 Hz、q=0；在 42.7 fps 输入下丢约 6% 源帧，**不增加延迟**。
   新 exe 同时修掉了 `--lock` 丢帧时重置 XeFG history 的问题（每丢一帧重置
   会制造 8 次/秒的 hitch）。入口 `run_consumer_optimal_lock40.bat`。
2. **`--cad-latency-budget 8`**：允许把输入量化到 3 vblank（25 ms），
   `k=3` → 120 Hz、q=0；代价是最多 8 ms 等待 + 丢同类源帧。
   入口 `run_consumer_optimal_pace8.bat`。

若游戏掉到 24 fps（5 vblank），4x 只能 96 Hz，属算术上限（§13.6）。


## 14. 缺失的里程碑 M6：输出时钟（output-driven pacing）

### 14.1 第五轮数据（lock40 / pace8）

| 指标 | lock40 | pace8 (Λ=8ms) |
|---|---|---|
| 决策 | 全程 k=3，paced=0 | k=3，paced=1 仅 7/27 窗 |
| q 均值/p95 | 0.078 / 0.208 | 0.039 / 0.150 |
| show | 113.2 | 111.9 |
| presents / 120 | 116.2 | 112.1 / 114.3 |
| HITCH | **20** | 2 |
| load 均值/峰 | 71.5% / 97% | 61.7% / 101% |
| 其他 | `arr(vb)` 仍 2x/3x/4x 混合 | learnedJ 4→3（4x 被学习为不可实现） |

两个补丁都没有把 q 压到 0：

- `--lock 40` 不是时钟，只是“早到就丢”的过滤器：`GetTickCount64` 1 ms 粒度、
  丢弃后相位继续漂移，接受间隔仍出现 16.7/25/33.3 ms；而且没有 coast，
  输入慢一拍就产生空 vblank，HITCH 反而升高到 20。
- `pace8` 只在“本帧等 ≤8 ms”时量化；输入抖动跨过阈值时又回到不量化，
  且每帧独立决策，没有固定输出节拍。

结论：**问题不在选 k，而在“Present 由捕获驱动”这件事本身。**

### 14.2 单卡 FG 为什么能做到

单卡 FG 的呈现路径就是游戏自己的 Present：

1. 游戏/Present 的节拍由 Reflex/XeLL + FG 内部队列控制；渲染抖动被**队列**吸收，
   不是直接把每个捕获间隔映射成输出间隔；
2. 固定倍率，不做逐帧 k 变更；
3. 显示端每 vblank 翻一张，偶尔重复/丢弃一张，频率由队列深度与 cap 决定，
   而不是每次捕获抖动都变成一次 hold。

我们的 consumer 是“捕获驱动”：每个 WGC 帧 → 拷贝 → 一次 Present 批次。
捕获线程的突发、跨卡拷贝、GPU 排队都会直接进入输出时序（日志里的
`bursts / qdrop / copy-still-in-flight / presents<120` 就是证据）。

### 14.3 M6 规格（建议实现为 `--out-clock`，默认关）

1. **固定输出节拍**：`P = k × vblank`（k 由“秒级平均输入”决定，例如 3 → 25 ms），
   由 QPC 定时器驱动；每 P 必须发生一次批次提交，与捕获到达无关。
2. **有界输入队列**：深度 2；每 tick 取“最新且不晚于 tick”的接受帧；
   积压 >2 丢最旧（延迟有界）；没有新帧则 **coast** 上一帧（复用 colour/MV/depth，
   不 reset history），最多连续 N 次（防暂停）。
3. **固定 k、固定预算**：`frameRenderTime = k×vblank`（不是 T），
   不再逐帧切换 `SetNumInterpolatedFrames`；k 只在秒级窗口变化。
4. **观测点改在 Present**：记录 tick 间隔 p50/p95/p99、coast 次数、队列丢弃数、
   `presents/120`；验收：tick p99 < 0.2 ms、show 119–121、coast <1%、drop <5%。
5. **失败回退**：若队列长期空/超时，退回当前的输入驱动路径。

这等价于“把 single-GPU FG 的 present/queue 语义搬到 consumer”，而不是继续
调 k、Λ 或 lock。


### 14.4 M6 已实现（`--out-clock`，默认关）

- `main.cpp`：QPC 输出时钟 `oc_tick()`；2 深输入队列（只保留最新 pending）；
  `coast<=2`（复用上一帧 colour/MV/depth，不 reset history）；
  等待用高分辨率 waitable timer（100 ns）+ QPC spin，**不用 ms 定时器**。
- `cadence.cpp`：`frameRenderTime = P`（`P=k*vblank` 或 `--out-clock N` 的基率）；
  启动日志打印 `out-clock: P=…ms (… vblank), k=…` 并校验与 `k*vblank` 一致。
- `frame.cpp`：每窗口打印 `out-clock: ticks/period/tick mean,max/coast/drops/skips`。
- 入口：`run_consumer_outclock.bat`（`--interp 2 --out-clock 40`，3x/120 Hz）。
- 验收：tick max 接近 P（无 ms 抖动）、`show 119–121`、`coast` 只在输入迟到时出现、
  `drops` 约为“输入 fps − 40”的比例；若 tick 明显超 P，说明 Present/拷贝本身超预算。


### 14.5 首次 out-clock 运行（P=25 ms，3x）

日志（`mgpu_consumer_outclock.log`）：

- `out-clock: ticks=40 period=25.000ms tick mean/max=25.0/28–64ms coast=3–8 drops=0`
- `presents=116/120`，`xN=2.90`，`xNmiss=2`，`load=86%`
- `breakdown: present=5.4ms gpu=16.9ms total=23.4ms, game=26.0ms (38.5 fps)`

说明：

1. **时钟本身工作**：tick 均值 25.0 ms，不是 26/33 ms；没有 ms 定时器抖动。
2. **但 P=25 ms 超出本场景可持续吞吐**：`run_frame` 的均值 23.4 ms、最坏 34.6 ms，
   只要超过 25 ms 就会漏掉一个 tick（presents 116/120、xN=2.90）。
3. 输入本身只有 38.5 fps < 40 base，所以还会出现 coast（3–8/40）。
4. 瓶颈是 FG 卡的 GPU 时间（`gpu=16.9ms`），不是拷贝（`copy=0`、`mvs=0`）。

即：**输出时钟把“输入抖动”问题解决了，但解决不了“GPU 没有余量”**。固定倍率要平滑，
必须让 `P ≥ p99(run_frame)`。本场景 4K/3x 需要约 30–35 ms，而不是 25 ms。

由此提供两个可持续档位（新增脚本）：

| 脚本 | 参数 | P | 输出 | 预期 |
|---|---|---|---|---|
| `run_consumer_outclock90.bat` | `--interp 2 --out-clock 30` | 33.3 ms | 90 Hz | tick max <33ms，coast=0（输入 38>30） |
| `run_consumer_outclock2x80.bat` | `--interp 1 --out-clock 40` | 25 ms | 80 Hz | FG 只生成 1 张，GPU 余量大，最稳 |


### 14.6 锁 40 / 锁 30x4 日志分析：不是“输入波动”，是 out-clock 路径漏了状态

| 指标 | 锁 40 / 3x | 锁 30 / 4x |
|---|---|---|
| tick mean / max | 25.000 / 26.9–66.6 ms | 33.3 / 43–58 ms |
| coast（稳态） | 0（仅启动 5） | 0（仅启动 5–6） |
| show 稳态 | 120 | 120 |
| presents 稳态 | 120/120 | 160/160 |
| **reset-history** | **8 次** | **8 次** |
| `arr(vb)` | **n=0** | **n=0** |
| tick max 的来源 | `present` 峰值 81ms、`total` 34.7ms | `present` 74ms、`total` 34.2ms |

两个关键发现：

1. **out-clock 路径漏掉了 `apply_arrival_values()`**：`arr(vb) n=0`、`g_wgc_ts_ms` 不更新，
   `pair_slot_for_wgc` 用旧时间戳配对，marker 识别失败 → `reset-history` 8 次；
   每次 reset 都是一次可见 hitch（日志里的 `HITCH gap=66.7ms xN=1`）。
2. **跳过/丢弃的帧仍会触发 reset**：`marker_slot < 0` 无条件 reset，等于把
   “输入少一帧”放大成“XeFG 历史清空”。

修复（新 exe `4a828a7a…`）：

- `oc_tick()` 在 `run_frame` 前调用 `apply_arrival_values(...)`（恢复 WGC 时间戳/arr 统计）；
- `--out-clock` 下 `marker_slot < 0` 不再 `reset-history`，改为复用上一组 MV/depth
  （`g_reuse_frame`），即“少一帧”只丢一帧画质，不清历史。

这两条正是“吸收波动”的正确位置：**时序由输出时钟吸收，配对失败由复用吸收，
而不是让 XeFG 重初始化**。重跑锁 40 / 锁 30x4 应看到：`arr(vb) n>0`、
`reset-history` 0、稳态 coast 0、tick max 接近 P（偶发 GPU 尖峰另算）。


### 14.7 为什么 out-clock 30x4 反而比固定 4x 差（对照 `mgpu_consumer_fixed4x.log`）

| 指标 | 固定 4x（17:52，用户确认平滑） | out-clock 30x4（01:08） |
|---|---|---|
| capture（队列等待） | **0 ms** | **78–87 ms** |
| prefetch served / missed | **40 / 0** | **13–17 / 40 missed** |
| gpu 等待 | 3–24 ms | **20–22 ms** |
| total | 12–33 ms | 29–31 ms |
| load | 33–92%（均值 65） | **87–95%** |
| xN | 4.00 | 4.00 |
| show | 115（稳态 120） | 113.6 |

原因不是 4x 本身，而是 **out-clock 的输入队列把捕获延迟了 2–3 个 tick**：

- 捕获帧在队列里等待 → `prefetch_publications()` 面对的是旧 publication，
  命中率从 40/40 掉到 13–17/40；
- 未命中就走“帧内补拷”，`capture` 78–87 ms、`gpu` 20 ms，
  把 4x 从 65% load 推到 89–95%，于是卡顿；
- 40x3 也同时出现了 34–87 ms 的 capture 等待，这就是“流畅但延迟大”的来源。

**结论：out-clock 的“缓存捕获帧 + 输出节拍”与 producer/prefetch 管线不兼容。**
固定倍率、输入驱动的老路径（capture=0、pf 40/40）才是正确的吸收方式：
输入抖动由 WGC/producer 队列和 XeFG pacer 吸收，而不是由我们延迟捕获。

### 14.8 回到固定倍率（下一步）

保留的新 exe 修复：`apply_arrival`、out-clock 下不 reset、marker 缺口不 reset；
默认不使用 out-clock。请用原有的：

- `run_fixed3x_den1.bat`（interp 2，锁 40）
- `run_fixed4x_den1.bat`（interp 3，锁 30）

若固定 4x 恢复旧日志的 `pf 40/40、capture≈0、xN=4、load≈65%`，则：

1. 固定倍率 + 输入驱动就是“波动输入仍平滑”的基线；
2. 只在其上加非侵入的吸收：`--colour-queue 2`（吸收突发）与“溢出不 reset”；
3. 需要更低延迟/更高帧率时，再单独讨论 4x 的 `--latency 2` 与 out-clock 的替代实现
   （例如不延迟捕获、只在 Present 侧排队，但那需要重写呈现路径）。


### 14.9 固定倍率 + 队列 2：最终结论（2026-11-15 01:13–01:15）

| 指标 | 固定 3x 旧（queue1） | 固定 4x 旧（queue1） | 固定 3x cq2 | 固定 4x cq2 |
|---|---|---|---|---|
| show 均值 / 峰 | 117.0 / 122 | 115.2 / 120 | 117.6 / 121 | 116.1 / 120 |
| load 均值 / 峰 | 28.9 / 95 | 65.4 / 123 | 92.0 / 98 | 93.6 / 101 |
| capture 均值 | 0.1 ms | 3.4 ms | 25.1 ms | 19.8 ms |
| pf served/missed | 2233 / 2 | 2106 / 3 | 2639 / 1 | 1995 / 3 |
| HITCH | 0 | 3 | 0 | **1** |
| reset-history | 3 | 8 | 2 | 4 |

- queue2 的 `load` 变高不是变慢：原来停在 capture 等帧（不计入 load），现在帧已在队列里，
  等待转移到 `gpu`（计入 load）；`total` 基本不变。
- 实际观感：3x/4x 都基本平滑；4x cq2 只有一次 251 ms（`colour=209.9ms`）的拷贝停顿，
  不是 cadence 问题。
- 代价：queue2 让捕获帧最多多等一个输入周期（25/33 ms）→ 延迟 +1 帧；queue1 延迟最低。

**最终建议**：

| 场景 | 脚本 | 理由 |
|---|---|---|
| 要最低延迟 | `run_fixed3x_den1.bat`（queue1，锁 40） | HITCH 0、load 29%、无队列延迟 |
| 要 4x / 更强抗突发 | `run_fixed4x_cq2.bat`（queue2，锁 30） | 4x 下 HITCH 3→1；代价 +1 帧延迟 |

**M6 结论**：固定倍率 + 输入驱动 + 小队列（1/2）就是“波动输入仍平滑”的正确形态；
不要用 out-clock 去缓存/延迟捕获帧（会破坏 prefetch，见 §14.7），
也不要逐帧改 k。动态逐帧/输出时钟保留为实验路径。


## 15. 目标修正：动态倍率是“秒级 epoch”，不是逐帧

原始目标是“输入帧率波动 + 指定输出上限下仍然丝滑”。实验证明，问题出在
**动态的粒度**，不是目标本身：

- 每次 `SetNumInterpolatedFrames` 切换在这份 pacer 上约等于丢 1 张输出；
  40 fps 下 1 次/秒 ≈ 2.5% 丢帧，30 次/秒（逐帧）≈ 30% —— 不可能丝滑。
- 固定倍率 + 输入驱动的日志（3x/4x）证明：只要 k 不变、捕获不延迟、prefetch 对齐，
  当前双卡管线本身是平滑的。
- out-clock 延迟捕获帧会把 prefetch 命中从 40/40 打到 13–17/40，4x 直接超预算
  （§14.7），所以“缓存捕获帧”不是吸收抖动的地方。

### 15.1 正确形态：epoch 动态倍率

```
每 1 s（或 0.5 s）:
    fps = 最近 2 s 输入间隔的中位数（忽略 >250 ms 的 hitch）
    k   = clamp(floor(target_out / fps), 2, J_eff)   // floor：绝不超额
    只有 |target_out / k - fps| 连续 2 个窗口超过 ~10% 才允许换 k
    换 k 只在“已排空 + 无 marker 缺口”的 batch 边界执行

epoch 内：
    k 固定；frameRenderTime = 输入中位数（输入驱动）
    colour 队列 1–2；prefetch/配对不变；不 reset XeFG history（缺帧用复用吸收）
```

`target_out = min(refresh, --fg-out-hz)`；`J_eff = min(DLL/--interp, 学到的可实现 k)`。
floor 的好处：输出 `k × fps ≤ target_out`，由面板/ pacer 自然吸收小数，
不会出现 3x@50fps = 150 帧被面板丢 20% 的情况。

| 输入（中位） | target 120 | k | 输出 | 说明 |
|---|---|---|---|---|
| 50 fps | 120 | 2 | 100 | 不超额；丝滑优先（nearest 会给 3x→150 被丢） |
| 40 fps | 120 | 3 | 120 | 正好 |
| 35 fps | 120 | 3 | 105 | 不超额；比 4x→140 稳 |
| 30 fps | 120 | 4 | 120 | 正好 |
| 21 fps | 120 | 4 (cap) | 84 | cap-limited，算术上限 |

### 15.2 与已有代码的关系

- 复用 `cadence_core` 的估计器、J_eff、切换成本/可实现性学习；
- 新增两个约束：`rounding=floor`、`epochMs≈1000`（epoch 内禁止换 k）；
- **不启用** out-clock、m/paced、逐帧 cadence；pacing 仍由输入驱动 + XeFG pacer；
- 这是“动态逐帧”的可用定义：倍率随负载在秒级自适应，画面节奏不被逐帧改动打散。


### 15.3 已实现：`--cad-epoch`（默认关）

- `cadence_core`：`Config.epochMs`、`Config.rounding`；even 路径在 epoch 模式下
  `k = floor(target_out / fps + 0.02)`（0.02 容忍锁定 30/40 的测量误差），
  epoch 内禁止换 k；换 k 仍需排空 + 驻留。
- `consumer`：`--cad-epoch N`（ms）、`--cad-round 0|1|2`；`--cad-epoch` 隐含
  `--interp-cadence`，policy 固定 even（不启用 optimal 的 m/paced/out-clock）。
- 入口：`run_consumer_epoch.bat`：`--interp 3 --interp-cadence --cad-policy even
  --cad-epoch 1000 --cad-round 0 --colour-queue 2`，游戏**不锁帧**。
- 预期：原生 50 fps → k=2（约 100 Hz）；30 fps → k=4（120 Hz）；40 fps → k=3（120 Hz）；
  切换最多约 1 次/秒，通常几秒一次；输出永不超额（floor）。


### 15.4 关键修正：仅动态 k 不能稳定显示帧率，必须同时锁定输入基率

第一版 `--cad-epoch`（只按中位数换 k）在 45–50 fps 输入上会在 2x/3x 间反复：
`show` 均值 89.7，19 个窗口 8 次切换，用户直接看到 100↔120 的跳变。

原因（也是“丝滑”的定义）：显示帧率 = k × 输入帧率；输入在 40 附近波动时，
floor(120/fps) 必然在 2 和 3 之间跳，输出就会 100↔120。**只改 k 不可能稳定。**

修正后的 epoch 算法：

```
选 k（秒级、带滞回）:
  k 初始 = ceil(target / median_fps)
  仅当 fps < (target/k)*0.92      -> k+1
  仅当 fps > (target/(k-1))*1.08  -> k-1
锁定输入基率:
  base = target / k        （120/3=40, 120/4=30, 120/2=60）
  --lock 用 QPC 把接受帧限制在 base（丢早到帧，不等待、不 reset history）
输出:
  k × base = target       （显示帧率恒定 120，间隔 = 1 vblank）
```

滞回带宽很宽（例如 k=3 在 36.8–64.8 fps 内都不动），所以不会在 40 附近抖动；
k 只在负载**持续**跨档时变化（30↔40↔60），且切换时同时改 base。

代价与单卡 FG 相同：为了让输出稳定在 target，基率会被限制到 target/k，
高于它的输入帧被丢弃（例如原生 50 fps、3x/120 时接受 40 fps，丢 20%）。
这是“显示帧率稳定 + 间隔平滑”的必要条件。


### 15.5 原始 bridge 的平衡点：只在整除时丢帧，否则不要锁

`src/bridge/modules/030_devices_capture_fg.inc:503-541`（`fg_solve_cadence`）
写得很清楚：

```
// We can only *drop* captured frames, never hold or re-time them, so a dropped
// frame turns one interval of T into two intervals of 2T. That is only invisible
// when T is an exact multiple of the lock period, i.e. when cap / lock is a
// whole number (dropping 1 of every 2 / 3 / 4 ...).
// Anything else (26 -> 24 fps = drop 1 in 13) would show up as a 2T gap every
// 13th frame - worse than the judder we are trying to remove.
```

所以 `--lock-auto` 先要求 `lock <= cap*0.97`，再要求 `cap/lock` 是整数；
不满足就**回退到不锁**。`020_state_overlay.inc:296-305` 的 `--slack-auto` 也是
“吸收一帧而不是丢掉”。

本次 epoch 锁 40 的问题正是：原生 50 / 锁 40 = 1.25，不是整数，每 5 帧丢 1 帧，
内容时间出现 20/20/20/20/40 ms 的不均匀步进 → 用户看到的“跳变”。

修正：**epoch 不再锁输入、不丢帧**；k 用输出带宽滞回保持稳定：

```
out = k × median_fps
step down 仅当 out > target*1.06
step up   仅当 (k+1)*fps <= target*1.00
```

| 原生 | k | 输出 | 丢帧 |
|---|---|---|---|
| 50 | 2 | 100 | 0 |
| 40 | 3 | 120 | 0 |
| 35 | 3 | 105 | 0 |
| 30 | 4 | 120 | 0 |
| 25 | 4 | 100 | 0 |

如果一定要 50 fps 下精确 120：只能锁 40（1.25 不整除，必然跳变）或使用 VRR；
固定刷新 + 整数倍率下，“120 且不丢帧”不存在。


### 15.6 bridge 的进位累加器，以及为什么我们只把它作为 A/B

`src/bridge/modules/040_cadence.inc:270-433` 的实现：

```
phase += T_in / vblank          // want = 本帧应出几张（小数）
k      = round(phase)
k      = min(k, ceil(want))     // 不超过实际经过的 vblank
phase -= k                      // 余数进位
clamp phase to ±0.5             // 下一次 k 最多走一步
--cadence-hold N (15)           // 保持 N 帧，再 ±1
```

它保证**长期平均**等于目标（26 fps 在 120 Hz 上 5,5,5,4…），但代价是
k 会周期性变化。bridge 的日志也记录了 `24–32 switches / s`，并靠 `hold 15` 压到
可接受范围。

我们的实测：每次 `SetNumInterpolatedFrames` 在这份 pacer 上约丢 1 张输出；
fill/bres 路径 15–31 switches/s 时画面明显更差（§9 表 1）。所以 carry 不能直接做默认。

本次同时提供两条 A/B：

| 入口 | 机制 | 特点 |
|---|---|---|
| `run_consumer_epoch.bat` | 宽滞回档位（不丢帧） | 50 fps→100、40→120、30→120；档位稳定，不切换 |
| `run_consumer_carry.bat` | `--cad-policy fill --cadence-hold 8` | bridge 式相位进位：k 在 2/3 间交替，平均≈120 |

宽滞回已改为：只有 `out > target×1.10` 才降档，只有 `(k+1)×fps ≤ target×0.92`
才升档——40–44 fps 区间不再出现 2↔3 的来回。


### 15.7 A/B 结果：carry 胜出，epoch 档位被淘汰

| 指标 | epoch 档位 | carry（hold 8） |
|---|---|---|
| k 分布 | 2×32 窗，3×7 窗 | 2×17，3×19，4×3 |
| out 均值 | 96.9 Hz | 约 120 Hz |
| show 均值 | **87.5** | **112.5** |
| switches | 3 / 39 窗 | 197 / 39 窗（≈5/窗） |
| HITCH | **16** | **2** |

epoch 长时间停在 k=2（100 Hz），遇到负载上升才整档跳到 3，HITCH 反而更多；
carry 虽然切换多，但平均一直贴住 120，画面节奏没有被拉低。

结论：**动态路径采用 bridge 式相位进位**，不再使用秒级档位。补充两项：

1. **over-refresh guard**：`kRound = min(kRound, ceil(want))`，绝不请求超过本帧
   实际经过的 vblank 数（bridge `040_cadence.inc:407-415` 的同款保护）；
2. `--cadence-hold` 作为唯一平滑旋钮：保持 k 若干帧再 ±1。

提供 `run_consumer_carry8/12/15.bat`（hold 8/12/15）做 A/B：hold 越大切换越少，
但平均跟随略慢；建议从 **hold 12** 开始。

2026-11-15 纯模型补测（`tests/cadence_core/sim.cpp` 表 5b，J=4、switchLoss=1）：

| 输入 | hold | out | uneven | switches | lost |
|---|---|---|---|---|---|
| lock35 | 4 / 8 / 12 / 15 | 106.0 / **108.3** / 106.3 / 107.0 | 12.4 / **9.5** / 10.6 / 10.3 % | 6.4 / **3.6** / 5.3 / 4.3 /s | 241 / **137** / 200 / 161 |
| fluct25-40 | 4 / 8 / 12 / 15 | 104.8 / 106.6 / 107.0 / **107.6** | 12.0 / 10.9 / 10.7 / **10.5 %** | 6.7 / 4.6 / 3.7 / **3.2 /s** | 286 / 196 / 156 / **135** |

`hold` 同时决定切换门限和相位夹取上限（`lim = deadband + 1 + 2×dwell`），
所以不是单调的：`lock35` 的甜点是 8，波动大的 `fluct25-40` 偏向 15。
实机仍以 `show` 与主观顺滑度为准。

同轮代码修正：`fill`/`even` 路径以前不填 `Decision.outputHz`（日志恒为 `out=0.0Hz`），
现在填“k 张/真实帧、受面板约束”的诚实值，cap-limited 时明确报低于目标。


## 附：新增文件

| 文件 | 作用 |
|---|---|
| `src/cadence_core.h` | 控制核心（纯 C++17，header-only，~580 行） |
| `tests/cadence_core/sim.cpp` | 确定性仿真 + 22 项结构检查 |
| `tests/cadence_core/run.sh` / `build.bat` | 构建与运行 |
| `docs/design/DYNAMIC_CADENCE_ANY_CAP.md` | 本文 |
