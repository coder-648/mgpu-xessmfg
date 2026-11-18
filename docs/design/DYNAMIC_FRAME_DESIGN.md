# dynamic_frame 设计文档（自由渲染 + 固定倍率 + 输出封顶）

> 定位：**新版本**，与 `lock_frame`（`LOCK_FRAME_IMPROVEMENTS.md`）并列。
> 代码基线选 `src/xess_mfg_consumer`（v22 颜色入槽），**不是** `lock_frame`——
> 原因见 §2。producer / fakerFG 完全不改。
> 需求：主卡不限帧（游戏跑满）、倍率固定（2x/3x/4x）、输出**不得超过**面板刷新率。
>
> 一句话：**把"跟住变化的输入帧率"换成"开固定数量的输入槽"**，
> 于是输出上限由结构保证，输入的两侧摆动（41/35）被"同槽丢弃 / 空槽等待"吸收。

---

## 1. 需求与不变量

```
R = 面板刷新（固定，120 Hz）
C = min(R, --df-cap)          // 输出硬上限
m = --df-mode 2/3/4           // 固定倍率 = 每张真帧出 m 张
f = 游戏帧率（GPU 决定，自由摆动）

目标：presented ≤ C 恒成立，且尽可能接近 C
```

| 不变式 | 内容 | 由什么保证 |
|---|---|---|
| **D-INV-1** | 任意窗口 `presented ≤ C·Δt + 1` | **槽位数量**（每槽最多交一张），不依赖速率估计 |
| **D-INV-2** | `C = R` 且 `m·f ≥ C` 时 `out_step` 恒为 1 vb | 槽间隔 `= m·vb` + 固定 m |
| **D-INV-3** | 只有 `m·f < C` 才出现重复/空槽，且不连续 | 相位浮动 + 有界债务 |
| **D-INV-4** | 队列深度 ≤ 1，延迟不随 f 增长 | 发布到达即折成"最新一张" |
| **D-INV-5** | 丢掉的发布立刻 ACK（`colour_seq` 单调水位） | 抽帧门在拷贝前 |

`q = C/(m·f)` 只在**诊断**里出现（丢帧率、浪费率），不参与决策。

---

## 2. 为什么基线是 consumer 而不是 lock_frame

| 维度 | lock_frame | dynamic_frame（本设计） |
|---|---|---|
| 颜色路径 | WGC（DWM 拷贝，scRGB FP16） | **共享内存 stream 0**（v22，颜色入槽） |
| 丢帧代价 | WGC 已经把颜色搬过来了，丢了只是不做后续 | **可以在跨卡拷贝前丢**，直接省掉拷贝 + 转换 + FG |
| 身份 | 四角 marker + `seq12/ms12` | 发布槽 + `frame_id`（v22 consumer 不再解 marker） |
| 主循环 | WGC 池轮询（可选 `--pace` tick） | **槽驱动**（定时等待 + 同槽只留最新） |
| 适用 | 游戏能稳定锁帧、带宽紧张 | 游戏不限帧、丢帧比例可能很高 |

自由渲染意味着丢帧率可以达到 40–60%（f=70、C=120、m=3），
**丢在拷贝前**是这条路线唯一能保住消费卡预算的做法，所以必须用颜色入槽。

---

## 3. 架构

```
游戏（NVIDIA）                        dynamic_frame.exe（AMD）
 fakerFG（记录 MV/depth、画 marker）    ┌──────────────────────────────┐
 xess_mfg_producer ──发布 32 槽──▶      │ 1. 定时等待（发布事件 or 槽） │
   colour + MV + depth（150%）          │ 2. drain_newest()：折成最新一张│
                                       │    其余 ack + drop（同槽丢弃） │
                                       │ 3. 到槽：交接（tag + Present） │
                                       │ 4. 回到 1                     │
                                       └──────────────────────────────┘
                                               │
                                     XeFG（代理交换链）+ XeLL 标记
```

线程模型沿用 consumer：主循环 + 拷贝队列 + 预拷线程；**新增**一个"槽定时器"
（可等待定时器或 `MsgWaitForMultipleObjects` 超时）。

---

## 4. 核心算法

### 4.1 槽

```cpp
slot_ms = m * 1000.0 / C;      // C=R 时 = m·vb；C=100、m=3 时 = 30 ms
next_slot = 0.0;

for (;;) {
    wait_for(publication_event, timeout = next_slot - now);
    drain_newest();                        // 队列里所有新发布 → stash（最新一张），其余 drop+ack

    if (now + 0.5 >= next_slot) {          // 到槽（半毫秒容差）
        if (stash) {
            handoff(stash);                // tag + Present（m 张输出）
            stash = nullptr;
            next_slot = std::max(now, next_slot) + slot_ms;   // 迟到浮动
            ++slots_served;
        } else {
            ++underfill;                   // 空槽：不推进 next_slot
        }
    }
}
```

三个关键点：

1. **`next_slot = max(now, next_slot) + slot_ms`**：迟到时相位浮动（不吸附整周期），
   亏损被摊成偶发 `m+1` vb 步，而不是"整个 3 vb 的定格"。
2. **空槽不推进**：宁可让相位落后，也不"跳过一个槽"（跳槽 = 丢 m 张输出）。
3. **`stash` 只保留最新一张**：这是 D-INV-1 和 D-INV-4 的唯一保证点。

### 4.2 `frameRenderTime` 必须喂 `slot_ms`

XeFG 用这个值决定把 m 张摊多开。喂 producer 的 `1/f` 会让它挤在 `1/f` 里 → 输出超上限；
喂"槽到槽"的间隔才能得到 `slot_ms/m = 1000/C` 的输出间距。

> 同一个原则也适用于 MV：**`frameRenderTime` 与 MV 描述的时间跨度必须一致**（§4.4）。

### 4.3 抽帧与 ACK

```
drain_newest():
  while (publication available) {
      if (stash) { ack(stash.id); ++drops; }   // 同槽的旧帧：ACK 后丢弃，不拷贝
      stash = newest;
  }
```

- **ACK 必须在丢弃时发出**：`colour_seq` 是单调水位，producer 靠它回收槽位；
  不 ACK 会让 producer 的信用窗口卡死（`mgpu_shared.h:534-537`）。
- **只能按序丢**（丢最旧）：水位不能倒退。
- **预拷（`--prefetch-inputs`）在抽帧门之后**：否则省下的带宽又被预拷吃回去。

### 4.4 MV：三种来源与默认策略

丢帧后保留对是 `(n−1, n+k)`，而游戏 MV 只描述相邻两帧 → 位移被低估，插值位置错。
默认策略：

| 丢帧率 `1−q` | MV 来源 | 说明 |
|---|---|---|
| < 5% | **游戏 MV**（`--df-mv game`） | 几乎不丢，语义基本正确；最省 |
| ≥ 5% | **光流**（`--df-mv of`，FidelityFX OF，约 0.3 ms） | 在保留对之间重算，语义正确；`--df-mv auto` 的默认门限 |
| 任意 | producer 合并 MV（P2） | `MV(n→n+1)+MV(n+k−1→n+k)` 相加；省消费卡，遮挡区会错；需要 producer 侧改动 |

`--df-mv auto` 用**上一窗口的 `drop%`** 决定（带 2% 迟滞），切换时打一行日志。
前置条件：OF 需要 depth + 分辨率一致（`--fg-of` 已有的约束，§`lock_frame/README.md`
"无 MV 标题"一节）。

### 4.5 C 与 m 的选择

```
C = min(R, --df-cap)                 // 默认 0 → C = R
m = --df-mode 2/3/4                 // 用户固定；默认按 f_p1 给出建议值
m = ceil(C / f_p1) 且 m·f_p1 ≥ 1.02C   // --df-auto-m 打开时
```

- `C` 尽量取 `R/n`（120/60/40/30）——只有整除时 `out_step` 恒 1 vb；
  116 这种"略低于刷新率"也可以，Bresenham 会摊成每 30 vb 一次 `m+1`，几乎不可见。
- m 按 **p1** 选（而不是均值）：宁可丢，不可欠供。
- **`--df-eco`**（可选，§6）：在 fakerFG 侧加绝对网格限帧到 `≈C/m + 3%`，
  把丢帧率压到 ±5% 内；这是"省电档"，与"自由渲染"互斥，默认关。

---

## 5. 状态机与观测

```
Init → Running ⇄ Underfill(短) → Stalled(20 个槽无发布) → Resuming → Running
```

- `Stalled`：产出停（菜单/加载）→ 不推进 `next_slot`，不丢弃；
- `Resuming`：第一张发布到达 → `next_slot = now + slot_ms`，清 `stash`，**不清 applied m**；
- 连续 3 个槽按时 → 回 `Running`。

OSD/报告新增字段（每窗口）：

```
slots:  served / underfill / drops
steps:  in_step(vb) 直方图（期望 ≈ m·R/C）    out_step(vb) {1: %, 2: %}   repeats/min
out:    presents / vblanks（应 ≤ C/R）
pace:   slot_err p50/p95 (ms)   phase_err (vb)
mv:     game / of   of=ms
q:      C/(m·f)   waste=1−q
```

> 两个 step 不是一回事：`in_step` 是**被接受输入**之间的间隔（槽间隔，≈ `m·R/C` vb）；
> `out_step` 是**每张输出**在面板上占的 vblank 数（C=R 时恒 1）。

验收：

| 指标 | 通过标准 |
|---|---|
| `presents/vblanks` | ≤ `C/R + 0.001`（C=R 时 ∈ [0.999, 1.001]） |
| `out_step` | C=R 且 m·f≥C 时 `{1: 100%}`；C<R 时 `2 vb` 占比 ≈ `1−C/R`，且均匀（无连续 2 次） |
| `in_step` | ≈ `m·R/C`；欠供时出现 `2×m·R/C`，无连续 |
| `repeats/min` | ≈ `(C−m·f)/m` |
| `age` | 不随运行时长增长（无 backlog） |

---

## 6. 参数与 CLI

| 参数 | 默认 | 说明 |
|---|---|---|
| `--df-mode 2x/3x/4x` | 3x | 固定倍率；设置 `g_fg_interp` 与 `--unlock-mfg` 目标（同 lock_frame 的教训：必须同时设 `interp_set`） |
| `--df-cap N` | 0（= R） | 输出上限；建议 `R/n` |
| `--df-mv auto/game/of` | auto | §4.4 |
| `--df-auto-m` | off | 按 `f_p1` 自动选 m（只升不降，迟滞 10 s） |
| `--df-float 0/1` | 1 | 迟到相位浮动（0 = 吸附到下一个 vblank，仅 A/B 用） |
| `--df-eco` | off | 渲染侧绝对网格限帧（省电档，见 §4.5） |
| `--df-max-wait` | 2×slot_ms | 空槽最多等多久；超时则记 `underfill` 并继续等（不跳槽） |
| `--df-dump-steps` | off | 把最近 512 个 `in_step`/`out_step` 写成 CSV，供离线分析 |

---

## 7. 里程碑

| 阶段 | 内容 | 验收 |
|---|---|---|
| **M0 能力** | 从 consumer 复制目录，改名 `dynamic_frame`；保持 v22 颜色路径；把主循环换成槽驱动（先不做抽帧） | 固定 C=R、m=2，输入锁定 60 时输出稳定 120；`presents/vblanks ≈ 1.0` |
| **M1 抽帧** | `drain_newest()` + 丢帧 ACK + 预拷顺序；`--df-cap` 生效 | f=42、C=120、m=3 → `drops ≈ 5%`、`out_step` 恒 `{1}`、无 backlog |
| **M2 欠供** | 空槽计数、相位浮动、有界债务、`Stalled/Resuming` | f=35 时 `underfill` 可见、`repeats/min` 与理论值一致、无连续重复 |
| **M3 MV** | `--df-mv auto` + OF 切换；`frameRenderTime = slot_ms` | 丢帧 30% 时 OF 生效、插帧主观质量与 locked 3x@40 相当 |
| **M4 产品化** | UI 增加模式（性能档/省电档）、打包、`--df-eco`、诊断收集脚本 | 与 lock_frame 相同的发布流程；两版共用 producer/fakerFG |

---

## 8. 实验矩阵

| # | 改动 | 对照 | 通过标准 |
|---|---|---|---|
| **F1** | M0 槽驱动（不抽帧），C=R，m=2，游戏锁 60 | 现 consumer | `presents/vblanks ≈ 1.0`；`slot_err p95 < 1 ms` |
| **F2** | M1 抽帧（f=42，m=3） | 不抽帧 | 输出 ≤ R；`drops ≈ 5%`；`age` 不增长 |
| **F3** | F2 + `--df-cap 100` | C=R | `presents/vblanks ≈ 0.833`；`out_step` 2 vb 占比 ≈ 16.7% 且均匀 |
| **F4** | M2 欠供（f=35，m=3） | 吸附式 | `repeats/min` ↓；无连续 2 次重复 |
| **F5** | M3 MV auto（丢 30%） | 强制 game MV | 快速平移无"滑动/重影"；`of= ≤ 0.5 ms` |
| **F6** | `--df-auto-m` | 固定 m | `underfill ≈ 0`；10 s 内不来回切 |
| **F7** | 自由渲染（不锁游戏，f=50–70，m=3，C=120） | 游戏锁 40 + 3x | `out_step` 恒 `{1}`、`age` 更低（内容更新鲜）、功耗更高 |

每组 2 分钟同场景；报告见 §5。

---

## 9. 风险与不采纳

| 风险 | 说明 | 对策 |
|---|---|---|
| MV 语义断裂 | 丢帧后 MV 只覆盖 1 帧 | §4.4 的 auto 门限 + OF；这是本设计唯一的硬风险 |
| 丢帧率过高浪费 | f=70、m=3、C=120 → 丢 43% | 用 `--df-eco`（省电档）或降 m；报告里给出 waste |
| 槽定时器精度 | 定时等待要准 | `CreateWaitableTimerExW(HIGH_RESOLUTION)` + 最后 0.3 ms 自旋 |
| 相位浮动漂移 | `next_slot` 可能偏离 vblank | 限制 ±1 vb；超出强制重锚并计数 |
| 空槽等待拖延迟 | 欠供时相位落后 | `--df-max-wait` 只是诊断上限，不跳槽（跳槽会丢 m 张） |

**不采纳**：

| 候选 | 原因 |
|---|---|
| 在 consumer 里按 fps 限帧（Reflex/FrameLimit 等价物） | 位置错误；槽位数量已经给上限 |
| 给 XeLL 设 `minimumIntervalUs` | 与 XeFG 内部 pacing 冲突；A 卡睡眠为空 |
| 动态倍率（DynamicCount/DynamicMFG） | 与"固定倍率"需求冲突；只保留 `--df-auto-m` 的单向安全阀（默认关） |
| VRR | MFG pacing 假设固定刷新；本仓未验证 |
| WGC 颜色 | 丢帧无法省带宽（见 §2） |

---

## 10. 与 lock_frame 的关系

| 场景 | 用哪个 |
|---|---|
| 游戏能稳定锁帧、想省跨卡带宽（MV/depth 150%，颜色走 DWM） | **lock_frame** |
| 游戏不限帧、想要最低延迟、接受丢帧与 `--df-eco` 的取舍 | **dynamic_frame** |
| 两者共存 | 是的：同一 producer/fakerFG，UI 里选模式；`lock_frame` 是省带宽档，`dynamic_frame` 是性能档 |

两版共享的东西：`mgpu_shared.h` 协议、marker/fakerFG、producer、XeFG 封装、
XeLL 标记序列、OSD 面板。差异只在**颜色来源**与**主循环节奏**。

---

## 11. 一句话

dynamic_frame 把"限帧"从游戏侧搬到了消费侧的**槽位数量**上：
`slot_ms = m·1000/C`，每槽只交最新一张，于是"输出不超过面板"由结构保证，
41/35 的两侧摆动自动被同槽丢弃/空槽等待吸收。
它唯一的硬成本是 **MV 与 `frameRenderTime` 的时间跨度必须重建**（默认光流），
这也是 M3 成为里程碑而不是可选项的原因。
