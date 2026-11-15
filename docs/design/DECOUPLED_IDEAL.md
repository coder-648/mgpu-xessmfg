# 生产/消费解耦：理想态的两条覆盖条件与当前差距（2026-11-13）

> 目标态（用户定义）：**消费端以自己抓到的 WGC 帧为锚点，数据搬运藏在 WGC 延迟里并行完成；
> 生产端以自己渲染出的帧为锚点，跨卡拷贝藏在“FG 输入 → 该帧上屏”的窗口里并行完成。
> 两端各自按自己的能力跑，互不拖慢。**
>
> 本文把这个说法写成可判定的不等式，逐条对照当前代码，再用 4K/40Hz 的实测数算一遍
> 理想态能拿回多少。结论先说：**模型成立，但“覆盖”必须靠独立队列；同队列内是“插入”，
> 会把窗口本身推长。** 当前实现两处都还是插入。

## 1. 把“理想态”写成四个条件

记：

- `T_in`：输入间隔（4K/40 = 25.0 ms）
- `T_copyP`：生产端跨卡写 MV+depth 的时间（4K/100% = 2.5–4.9 ms）
- `T_copyC`：消费端跨卡读 MV+depth 到本地 ring 的时间（实测增量 3.6 ms）
- `age`：合成器盖戳 → WGC 取到手（`g_ms_age`；4K p50 ≈ 8–12 ms）
- 注意：配对日志里的另一个 `age` 是“Evaluate → WGC 盖戳”（`frame_qpc_ms` 写的是 Evaluate 时刻），
  含整个帧尾，**不能当拷贝预算**；两者不要混用（见 `STREAM_AUTOSCALE.md` §1）
- `T_xefg`：XeFG 生成一批帧的 GPU 时间（4K 3x ≈ 16.6 ms，含真 MV）
- `T_colour`：颜色 hop（0.2–0.4 ms）

### 条件 P（生产端）：拷贝不得延长本帧，且必须在消费者需要之前发布

```text
P1  T_copyP 与“Evaluate 之后剩余的帧工作”并发      ← 不能排在游戏同一条队列里
P2  t_publish = t_present + T_copyP ≤ t_present + age   ⇔  T_copyP ≤ age
```

P2 在 4K/40 天然成立：`2.5–4.9 ms < 8–12 ms`。**唯一不成立的是 P1**：
拷贝目前录在游戏打开的那条 DIRECT 表上（`producer.cpp` `copy_stream(list, …)`，
`list = f->cmd_list`），GPU 按队列顺序执行，它后面的后处理/UI 都得等它。
所以它不是“藏进窗口”，而是“把窗口推长 `T_copyP`”。

> 注意：双卡 consume 模式下真正上屏的是 bridge 的覆盖窗口，不是游戏窗口。
> 因此“原生画面到屏幕”不是拷贝可用的预算；拷贝真正的截止时间是 **WGC 交付**
> （`t_present + age`），而 **游戏 Present 不能被推迟**——推迟 Present 就是推迟整条节拍。

### 条件 C（消费端）：数据搬运藏在 WGC 等待里，与抓帧并行

```text
C1  T_copyC 在“等 WGC 帧”的窗口内发起并完成      ← 必须在 WGC 交付之前知道槽位
C2a 现在（生产端拷贝仍在帧内）:  T_copyC ≤ age            （4K/40：3.6 ≤ 8–12 ✓）
C2b 生产端挪到 Present 之后:     T_copyP + T_copyC ≤ age   （同一块内存，写→读串行）
C3  选槽必须由 frame_key/时间戳决定，不能靠“最新发布”猜
```

> C2b 是解耦的隐藏代价：生产端不再推迟自己的 Present，但发布点后移 `T_copyP`，
> 消费端窗口从 `age` 缩到 `age − T_copyP`。4K/40 的 `T_P + T_C = 6.1–8.5 ms` 对
> `age 8–12` 已经贴边，所以自动降档（`StreamPercent=150`）不是可选项，而是前提。

C2a 天然成立；C2b 在解耦后会把窗口吃掉一截，需要靠自动降档补回来。缺的是 C1。
当前主路径是 `TryGetNextFrame`（阻塞，`capture`）→ 颜色拷贝 →
`copy_inputs_local(slot)`：**拷贝发生在抓帧之后**，而且默认和 convert draw 同一条 DIRECT 队列
（`--input-copy-queue` 关）。所以 `T_copyC` 落在 XeFG 之前的串行链上，没有被 `age` 覆盖。

### 条件 I（两端独立）

```text
I1  生产端的帧率 = 游戏自己的能力，不因消费者变慢而下降
I2  消费端的抓取节奏 = 自己的 GPU 能力；跟不上就丢，不反压生产端
```

I2 已经成立：4 槽 ring + `consumed[]` credit，没有逐帧往返；消费者落后超过 3 格时
`claim_slot()` 覆盖最旧的一格并 `producer_overrun++`，这是**单向弹性缓冲**，不是反压。
I1 **今天不成立**：只要 `request_mask` 还在请求，生产者每帧都要付 `T_copyP`（2.5–4.9 ms），
哪怕消费者已经卡住、槽位正在被覆盖。4K/40 下这一笔正好把 21.6 ms 顶到 24.1–26.5 ms。

## 2. 当前代码逐条对照

| 条件 | 现状 | 证据 |
|---|---|---|
| P1 拷贝与帧工作并发 | ❌ 录在游戏的 DIRECT list 上，顺序执行 | `producer.cpp` `on_record()`：`auto* list = f->cmd_list`；`copy_stream(list, …)` |
| P2 发布早于 WGC | ✅ `publish_frame(queue)` 在 Present 时 `Signal(fwd[slot])` | `producer.cpp` §publish |
| C1 拷贝先于 WGC 发起 | ⚠️ 只有 `--prefetch-inputs` 做了（默认关） | `bridge.cpp` `prefetch_next_inputs()`：上一帧 Present 之后录拷贝 |
| C1 拷贝与颜色并行 | ⚠️ 只有 `--input-copy-queue` 做了（默认关） | `input_copy_begin()/input_copy_wait()` |
| C2 拷贝 ≤ age | ✅（现在 3.6 ≤ 8–12）；解耦后变成 `T_P+T_C = 6.1–8.5`，贴边 | `PERF_BASELINE.md` §3.3 |
| C3 选槽正确 | ⚠️ 主路径按时间戳配对；prefetch 只按 round-robin 的 `next_slot`，**不校验 frame_key** | `pair_slot_for_wgc()`；`prefetch_next_inputs()` |
| I1 生产不随消费变慢 | ❌ 拷贝无条件进游戏队列 | 同上 |
| I2 消费不反压生产 | ✅ credit 单向，无往返 | `claim_slot()` / `ack_previous_slot()` |

也就是说：**理想态的四条里，两条结构条件（P1、C1）没有落地，C3 的健壮性还差一环。**
今天的链路是“半解耦”：credit 和发布/订阅已经单向，但两端的拷贝都还在各自的图形队列上。

## 3. 4K/40 的账：理想态能拿回多少

### 3.1 生产端

| 项 | 现在（同队列） | 理想态（COPY 队列） |
|---|---|---|
| 游戏渲染 1 帧 | 21.6 ms（实测，不锁帧） | 21.6 ms |
| 跨卡写 2×31.6 MB | **+2.5–4.9 ms，串在帧里** | **0（并发）** |
| 单帧占用 | **24.1–26.5 ms / 25.0** | **21.6 / 25.0（余 3.4 ms）** |
| 40 fps 锁定时 | 0–1.5 ms 超预算 → `arr(vb)` 抖成 2/3/4 | 稳定 3 vblank |
| 对消费者的依赖 | 只要 `request_mask` 在请求就付费 | 与消费者快慢无关（I1 成立） |

### 3.2 消费端

```text
现在： gpu = colour 0.3 + copy 3.6 + XeFG 16.6 = 20.6
理想： gpu = colour 0.3 +             XeFG 16.6 = 17.0     （copy 藏在 age 里）

预算： gpu + 1 vblank ≤ T_in
现在： 20.6 + 8.33 = 28.9  > 25.0   ❌ 超 3.9 ms（16%）→ 实测 106 fps、skip 5.3
理想： 17.0 + 8.33 = 25.3  ≈ 25.0   ⚠️ 超 0.3 ms（1.2%）→ 预计 117–120 fps
```

也就是说：**把 `T_copyC` 藏进 `age` 之后，4K 3x@40 从“结构性装不下”变成“刚好卡在线上”**。
剩下的 0.3 ms 落在本机 2–10 ms 的噪声里，能不能整拍 120 取决于 XeFG 的 p99（31.8 ms），
而不是我们的链路。若把真 MV 换成零矢量，XeFG 掉 7.4 ms（`0.3 + 9.2 + 8.33 = 17.8`），
余量立刻变成 7 ms —— **“最理想状态”下 4K/40 的可行性，本质上由 MV 是否值得那 7.4 ms 决定。**

### 3.3 端到端延迟

`T_copyC` 从关键路径上消失，`total` 少 3.6 ms：

```text
compose->photons = age 10 + total 21.8 + [0,8.33] = 31.8…40.1 ms   （现在 35.4…43.7，实测中值 39.9）
click->photons   = +T_in 25.0                      = 56.8…65.1 ms   （现在 60.4…68.7，实测 64.9）
```

生产端不再推迟 Present，还会让“游戏输入→合成”那一段略微变好（不可直接测，未计入）。
**理想态不会改变结构式 `2·T_in + age`，只是把它前面的“我们自己的开销”清零。**

### 3.4 目标态时序

```text
主卡  |-- 游戏渲染 21.6 ---------------|-- Present --|-- 下一帧渲染 -----------|
                          (COPY 队列)  | 等 present 的 fence
                                       └─ copy 2.5–4.9 ─► Signal(fwd)  ← 发布
                                                          │ age 8–12 ms
副卡  (copy 已发起，等 fwd) ─ copy 3.6 ─┐                 │
                                        │                 ▼
        TryGetNextFrame（阻塞，CPU 空闲）└──── WGC 交付 ──► colour 0.3 ─► XeFG 16.6 ─► Present
```

两段“等待”各自被另一件事填满：主卡等 Present 时拷贝在 COPY 队列上跑，副卡等 WGC 时
跨卡读在 COPY 队列上跑。CPU 侧仍然只有 1 ms 量级，**没有任何一环需要等另一环**。

## 4. 达成理想态的最小改动集（按收益/风险）

### 4.1 生产端：把拷贝挪到独立队列（P1）——**已实现（2026-11-14）**

实现不是 COPY 队列，而是**第二条 DIRECT 队列**：源资源需要 `-> COPY_SOURCE` 的状态转换，
COPY 表不能发 barrier。现在的时序：

```text
on_record:  缩放 pass 留在游戏队列；barrier + CopyTextureRegion 录进第二条队列的 list
present:    游戏队列 Signal(frameFence)  →  第二条队列 Wait
                                          →  Execute(copy list)
                                          →  Signal(copyDone) / Signal(fwd[slot])
```

- 游戏 Present **不再**等待拷贝；实测证据：150 档 `game=25.1 ms`、100 档 `27.2 ms`
  （拷贝差值 ≈2 ms 直接压在帧上）。
- 发布点后移 `T_copyP`（1.2–2.2 ms @150），仍早于 WGC 交付（`age≈12 ms`），满足 P2。
- 消费端窗口相应变成 `age − T_copyP`，4K/40 仍有余量（12−2≈10 ms > `T_C` 1.6 ms）。
- 源资源在下一帧 Evaluate 之前被覆写：4K/40 的窗口约 15–20 ms ≫ 2 ms；高输入帧率
  （120 fps）要重新核这条。

> 之前那段“D3D12 不能在命令表中间发 fence，所以只能帧提交后拷”的分析仍然成立；
> 只是第二条队列用 DIRECT 而不是 COPY。

D3D12 不能在命令表中间发 GPU fence，所以“Evaluate 当时就并发”做不到，正确的做法是
**帧提交之后立刻在 COPY 队列上跑**：

```text
present 时：游戏队列 Signal(fenceP)  →  COPY 队列 Wait(fenceP)
                                    →  barrier(COPY_SOURCE) + 2×CopyTextureRegion
                                    →  Signal(fwd[slot])          ← 发布点后移 T_copyP
```

此时生产端的真实条件是：

```text
P1'  T_copyP ≤ t_Evaluate(n+1) − t_present(n)     ← 下一帧 DLSS-SR 覆写源资源之前
P2   T_copyP ≤ age                                ← 消费者需要之前
4K/40：窗口 ≈ 15–20 ms（推算）≫ 2.5–4.9 ms ✓      8–12 ms ≫ 2.5–4.9 ms ✓
4K/120：窗口 ≈ 3–5 ms（8.3 减去 Evaluate→Present），2.5–4.9 ms 已经贴边 ⚠️
        （若游戏对 MV/depth 双缓冲，窗口是一整帧，另算；需要实测）
```

> 这条不等式是解耦模型的真正边界：**P1' 依赖“输入间隔 − 帧尾”，不是依赖队列本身。**
> 输入帧率越高，留给拷贝的窗口越窄；4K/40 很宽裕，4K/120 就要重新设计（例如把流降到
> 1440p，让 `T_copyP ≤ 1.2–2.2 ms`）。

代价与注意：

- 发布晚 `T_copyP`（2.5–4.9 ms），仍早于 WGC 交付，**不影响消费者看到帧的时刻**；
- 槽位复用必须等自己的 copy fence（现在只等 `consumed[]` credit），否则会覆写还在被读的槽；
- `StreamPercent>100` 的缩放 pass 仍要留在 DIRECT/COMPUTE 表上（COPY 表不能 Dispatch），
  应“先缩放到本地纹理，再在 COPY 队列上搬”，两段之间加一次队列 fence；
- 收益：主卡 21.6 → 不再 24.1–26.5，40 fps 的输入重新变整（这是 `arr(vb)` 2/3/4 抖动的根因之一）。

### 4.2 消费端：默认开启 prefetch + copy 队列（C1）

代码已经有了，缺的是**默认值**和**配对准校验**：

- `--input-copy-queue`：把 MV/depth 拷到独立 COPY 队列，与 convert draw 重叠；
- `--prefetch-inputs`：在上一帧 Present 之后就把下一槽拷好，copy 队列**在 GPU 侧等 fwd**
  （`prefetch_next_inputs()` 已有），于是抓 WGC 的那 8–12 ms 里拷贝已经跑完；
- 新增一步：`prefetch` 命中时用 WGC 帧的 `frame_key` 与该槽 `frames[slot].frame_key` 对一次，
  不一致就退回本帧再拷（当前只校验 `g_pf_gen`，不校验 frame_key）。这是 C3。

### 4.3 不建议为了“解耦”做的事

| 做法 | 为什么不做 |
|---|---|
| 消费者提速去追生产者 | 抓取节奏必须由自己的 GPU 决定，追只会把 `present` 拉长、`slow` 上升（§3.24 的教训） |
| 生产端按消费者的 ACK 决定“这帧要不要拷” | credit 已经是槽位级；再加逐帧请求就引入往返，正是要避免的 |
| 给发布加锁 / 队列 / 轮询 | 560 B 快照 + 22 ns 发布已经够；轮询会把延迟换成 CPU 空转 |
| 让生产端等消费者 ACK 再写 | 那就是反压（I2 被破坏）；正确行为是覆盖最旧（已实现） |

## 5. 理想态也拿不到的东西

| 项 | 为什么 | 4K/40 的量 |
|---|---|---|
| XeFG 4K 3x 的 16.6 ms | 闭源；其中 7.4 ms 是“喂真 MV”的数据相关工作 | 占 25 ms 预算的 **66%** |
| `2·T_in + age` 的延迟结构 | 插值帧必须先于真帧上屏（XeFG 只能在 prev→cur 之间插） | `click→photons ≥ 2×25 + 10 = 60 ms` |
| WGC/DWM 的 `age` | 系统合成器决定，只能拿来“藏拷贝”，不能用来降延迟 | 8–12 ms |
| ring 深度带来的积压 | 4 槽 = 100 ms 弹性缓冲；消费者持续更慢就会覆盖最旧、`overrun++` | 这是设计行为，不是缺陷 |

换句话说：**理想态把“我们自己的开销”从两条关键路径上清零，但清不掉闭源生成器的算力需求。**
4K/40 在这台机器上能不能整拍，最终由 `XeFG + 1 vblank` 与 25 ms 的比较决定；
解耦只负责把这比较里属于我们的 3.6 ms 拿掉。

## 6. 怎么验证“真的解耦了”

改动只有两三处，但必须用数据证明“两段等待被填满”，否则只是换了条队列：

| 指标 | 期望（理想态） | 说明 |
|---|---|---|
| 生产端 `arr(vb)` | 40 fps 锁定时稳定 `3x=40`，不再 `2x/3x/4x` 混桶 | 拷贝不再推长帧 |
| 生产端 `bus:` | 2.53 GB/s 不变 | 带宽没省，只是换了队列 |
| 消费端 `gpu` | 20.6 → **≈17.0**（−3.6） | 跨卡读离开关键路径 |
| 消费端 `capture` | 基本不变 | WGC 等待没被拉长 |
| 消费端 `load` | 101.6% → **≈87%**（17.0+4.8 / 25） | 预算重新有余量 |
| `skip` / `slow` | 5.3 → 接近 0 | 装得下的直接证据 |
| `pf: … hit/miss` | hit 占绝大多数，miss 只在 WGC 停顿后出现 | prefetch 配对有效 |
| `compose->photons` | 39.9 → **≈36**（−3.6） | 延迟账 |
| `producer_overrun` | 不应随时间线性增长 | 消费者跟不上时应是偶发，不是持续 |

复测方式沿用 `--report 40` + `scripts/latstats.py`，每档 ≥60 s、A→B→A 至少 3 轮；
本机噪声 2–10 ms，所以“省 3.6 ms”这种量级的结论必须看**配对的三轮**，不能看单次。

