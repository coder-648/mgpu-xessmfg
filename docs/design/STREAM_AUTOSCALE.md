# StreamPercent 自动计算：以 WGC 窗口为预算的动态控制器（2026-11-13）

> 前提：生产/消费解耦（`DECOUPLED_IDEAL.md`）。目标是把“每帧搬多少 MV/深度”从**手动档位**
> 变成**按当前帧率的 WGC 窗口反算**：只要窗口够，就用最高画质；窗口不够，就自动降一档。
>
> 结论先说：**可以做，而且代码地基是现成的**（槽位是 placed 资源、内容矩形可变、消费端按
> 归一化 UV 采样）。但有三个硬约束必须先承认：
> 1. 预算要用 **`age` 的低分位**（WGC 早到才是风险，晚到只是多给余量）；
> 2. 生产端拷贝如果挪到 Present 之后，消费端窗口会缩成 **`age − T_copyP`**；
> 3. 改一次尺寸有真实代价（清空 4 个发布槽 + 消费端重建本地拷贝），所以控制器必须**慢**。
>
> **状态（2026-11-13）**：**已落地**。`--stream-auto` / UI「自动 StreamPercent (A/B)」，
> 协议 v15 按流请求；控制器在 `bridge.cpp` `stream_controller_tick()`。
> **首次实机后修正**：年龄预算改用**中位数**（WGC 流里有重复/重组帧，p10 实测被压到 0.5–4.6 ms，
> 而中位数 ~12 ms）；生产端改用 `frame_time_ms` 头寸（见 §2.1）；档位取**第一个满足**的
> （最低 percent = 最高画质），全不满足才回退 400。参数：阶梯
> `{100,125,150,175,200,250,300,400}`、64 帧中位数、余量 1.0 ms、带宽上限 5.5 GB/s、
> 2/4 窗确认、2 s 驻留。真机 A/B 计划见 `PLAN_2026-11-13.md` 组 C。

## 1. 先厘清两个都叫 `age` 的量（否则预算算错）

| 量 | 代码位置 | 定义 | 数值（4K/30 实测） | 用途 |
|---|---|---|---|---|
| `g_ms_age`（`LATENCY` 行的 `age`） | `bridge.cpp` `note_arrival()`：`qpc_ms() − WGC SystemRelativeTime` | **合成器盖戳 → 我们取到手**（DWM hop） | p50 11.8 / p90 17.3 | **消费端拷贝的可用窗口** |
| 配对日志里的 `age` | `bridge.cpp` 7041：`g_wgc_ts_ms − frames[slot].frame_qpc_ms` | **游戏 Evaluate → WGC 盖戳**（帧尾 + Present + 合成） | 随帧尾变，4K/40 量级接近一个输入间隔 | 只用于配对合理性，**不能当拷贝预算** |

`frame_qpc_ms` 在 `provider.cpp` 的 harvest 里写的是 `GetTickCount64()`，即 **Evaluate 时刻**，
不是 Present。所以“配对 age”包含整个帧尾，不能用来判断“拷贝能不能藏进 WGC 等待”。

消费端真正能用的窗口：从**发布（Present）**到**取到手**，约等于
`(Present → 合成盖戳) + g_ms_age`，保守取 `g_ms_age`。这就是下面的 `A`。

## 2. 控制律

记 `B(percent)` = 每帧字节（4K 1:1 = 63.3 MB，`B ∝ percent⁻²`），
`T_P` = 生产端写、`T_C` = 消费端读（同一条共享内存，**两者串行**），
`A_lo` = 最近 N 帧 `g_ms_age` 的低分位（p10），`m` = 安全余量（1–2 ms）。

```text
选择最大的 percent ∈ {100,125,150,175,200,250,300,400}，满足：

(1) 覆盖：   T_P(B) + T_C(B) ≤ A_lo − m          ← 解耦后的生产端（拷贝在 Present 之后）
             或  T_C(B)     ≤ A_lo − m          ← 当前实现（生产端拷贝在帧内，代价付在它自己身上）
(2) 带宽：   B × fps ≤ B_budget                   B_budget ≈ 0.6 × 9.19 GB/s ≈ 5.5 GB/s
(3) 不许跳档：一次只动一格；下降要连续 2 个窗口满足，上升要连续 4 个窗口满足
```

为什么用 `A_lo` 而不是均值：**WGC 晚到只会给拷贝更多时间，WGC 早到才会让拷贝没跑完。**
用 p50 会在“合成器忽然变快”的那几帧破防；用 p10 意味着最多 10% 的帧低于这个窗口，
余量 `m` 再吃掉其中一部分。

`T_P + T_C` 的串行性来自数据面：消费端读的就是生产端刚写的那块内存。
4K/40 的实测/推算：

| percent | 每帧字节 | `T_P` | `T_C` | 合计 |
|---|---|---|---|---|
| 100 | 63.3 MB | 2.5–4.9 | 3.6 | **6.1–8.5 ms** |
| 125 | 40.5 MB | 1.6–3.1 | 2.3 | 3.9–5.4 ms |
| 150 | 28.1 MB | 1.2–2.2 | 1.6 | **2.7–3.8 ms** |
| 200 | 15.8 MB | 0.6–1.2 | 0.9 | 1.5–2.1 ms |
| 400 | 4.0 MB | 0.2–0.3 | 0.23 | 0.4–0.6 ms |

### 2.1 两条约束（首次实机后定稿）

```text
消费端：T_C(percent) ≤ age_med − m            // age_med = 64 帧中位数，m = 1.0 ms
带宽：  bytes(percent) × fps ≤ 5.5 GB/s
源生命周期：T_P(percent) ≤ 10 ms               // DMA 拷贝在 present 之后，不占帧关键路径
质量反馈：gpu+present > T_in − 1.0 ms → 至少降一档（提高 percent）
          gpu+present < T_in − 1.5 ms 才允许升档（降低 percent，4 窗确认）
```

生产端的 `frame_time_ms` 只用于日志/诊断：拷贝移交给 DMA/COPY 队列后，它不再包含拷贝，
所以**不能**再用“帧时间 − 拷贝”去算生产端余量（首次实机 C 组就是这样被压到 400%）。
真正决定掉帧的是 `gpu + present`（副卡），控制器现在直接用它做反馈。

两者都满足时取**第一个**（最低 percent、最高画质）；一个都不满足才落到 400。CP2077/4K/40
实测 `frame≈25.5 ms`、`T_in=25.0`、当前 150 的 `T_P≈2.2 ms`，底 ≈23.3 ms ⇒ 只剩 ~0.7 ms
给拷贝，控制器会落在 **300 左右**，而不是之前表里算的 150——因为生产端（内联拷贝）才是
4K/40 的约束，消费端窗口反而很宽。

## 3. 4K 各帧率的稳态解（推算）

> **首次实机后**：下表只按消费端窗口推算，已被 §2.1 的生产端头寸取代——4K/40 真正紧的是
> `frame_ms`，实机 C 组会落到 300 左右。保留此表是为了记录最初的模型及它与实测的差异。

`A_lo` 取“4K/30 实测 p50 11.8、p90 17.3”沿帧率外推的低分位（帧率越高，DWM hop 越接近 1 个合成周期）：

| 输入 | 预算 `A_lo−m` | 带宽（@100） | 覆盖条件选出的档 | 说明 |
|---|---|---|---|---|
| 30 fps | ≈ 5.5 ms | 1.9 GB/s | **150**（125 贴边） | 100 的 6.1–8.5 超预算 |
| **40 fps** | **≈ 4.5 ms** | **2.5 GB/s** | **150** | 与 `PERF_BASELINE.md` 推荐一致（63.3→28.1 MB） |
| 60 fps | ≈ 3.0 ms | 3.8 GB/s | **200** | 150 的 3.8 ms 已经贴边 |
| 90 fps | ≈ 2.4 ms | 5.7 GB/s | **200–300** | 带宽项也开始吃紧 |
| 120 fps | ≈ 2.0 ms | 7.6 GB/s | **200–300** | 100% 的 7.6 GB/s 已到 9.19 GB/s 的 83%，不可用 |

**所以“任意帧数下都能覆盖”成立，代价是帧率越高、档位越低**：120 fps 会稳定在 200（半分辨率）
甚至 300，而不是 100。能一路降到 400（4 MB/帧），覆盖永远能解出来——下限是画质，不是算法。

> 若生产端改成 COPY 队列，`frame_time_ms` 不再包含拷贝，增量公式要改成
> `T_P + T_C ≤ age`（或 `T_P ≤ T_in − base`，base 不含拷贝）；届时要重新标定一次。

## 4. 为什么控制器必须“慢”：改一次尺寸的真实代价

代码里已经有动态改尺寸的路径，但它设计给“DLSS 档位切换”，不是给逐帧调节：

| 位置 | 行为 | 后果 |
|---|---|---|
| `producer.cpp` `update_content()` | `memset(c->frames, 0, …)`、`g_next_slot = 0`、`generation += 1` | 4 个发布槽全部作废，消费者这一拍没有可配对的帧 |
| `bridge.cpp` `service_shared_layout()` | generation 变化 → `release_local_inputs()` | 本地 MV/深度 ring 全部销毁，下一帧按需重建（分配 + 清零） |
| 同上 | `refresh_input_geo()` 重读几何 | 正常，但会触发一次日志与状态刷新 |
| `producer.cpp` `ensure_shared()` | `g_slot_w = cap_dim(g_stream_w)` | **容量 = 初始流尺寸**，不是渲染尺寸 |
| `update_content()` | 超出 `g_slot_w/h` 直接拒绝（“content size kept”） | 从 200 想升回 100 **不可能**，除非重开映射 |

所以：

1. **自动模式必须把容量按 100% 预留**（`cap_dim(render_w/h)`），否则控制器只能单调下降；
   4K 下这意味着固定占用 **≈280 MB** 共享内存（实测 `shared ready … used=270.1 MB`），
   即使当前跑在 200（66 MB）也一样。这是 auto 模式的固定代价。
2. **每次改档 ≈ 1–3 帧的 1X / 停顿**（消费者丢一拍配对 + 重建本地拷贝）。40 fps 下就是 25–75 ms。
   因此控制器必须：一次一格、最小驻留 2 s、下降 2 窗确认、上升 4 窗确认，
   并且优先在加载/菜单（`g_mv_gate_off` 已经会关 FG）时改。

这也解释了为什么现成的 `StreamPercent` 是**会话级**的：不是懒得做动态，而是动态的代价（清槽）
比它省下的那几毫秒大。控制器只在“窗口真的不够”时付这笔钱。

## 5. 代码落点

### 5.1 协议（`src/mgpu_shared.h`，v15，字段追加在末尾）

```c
uint32_t stream_percent_request;   // consumer -> producer: 0 = 不干预（沿用 ini）
uint32_t stream_percent_active;    // producer -> consumer: 当前实际生效的档位
uint32_t producer_caps;            // bit0 = copy queue（决定控制器用哪条覆盖式）
uint32_t _pad_v15;
inline constexpr uint32_t kSharedVersion = 15;
```

- 语义与 v7/v14 一致：**单向提示**，生产端可以忽略（老 producer 读到 0 就按 ini 跑），
  永远不阻塞、不回问。
- `producer_caps` 是控制器选择“`T_C ≤ A`”还是“`T_P + T_C ≤ A`”的依据。

### 5.2 生产端（`xess_mfg_producer/producer.cpp`）

```text
on_record()（ensure_shared 之后每帧）：
  p = ctl->stream_percent_request
  if (p && p != g_percent) { g_percent = clamp(p, 100, ini_max); g_ds_w = 0;
                             update_content(g_render_w, g_render_h); }
  ctl->stream_percent_active = g_percent
```

配套改动只有三处：

| 位置 | 改动 |
|---|---|
| `read_percent()` | 保留，作为**上限/初始值**；`StreamAuto=1` 时把它当 cap |
| `compute_target()` | 去掉 `s_percent_read` 的单次读取，`g_percent` 变为可写；`g_scaled` 每次重算 |
| `ensure_shared()` | auto 时 `g_slot_w/h = cap_dim(render_w/h)`（预留 100%），否则维持现状 |

`update_content()` 已经会 bump generation、清帧、重置轮转，消费端也已经按 generation 重建，
**不需要新的握手**。缩放的 compute pass 每帧都在跑（`scale_dispatch`），改尺寸只是换目标尺寸，
`ensure_scale_target()` 会按新尺寸重建目标纹理。

### 5.3 消费端（`bridge/bridge.cpp`）

```text
每帧：记录 age（已有 g_ms_age）；记录本次 copy 的完成时间（submit → fence 完成）
每统计窗口（--report 的 40 帧）：
  A_lo   = 最近 60 个 age 的 p10
  bytes  = ctl->stream_bytes[1] + [2]                     // 生产端回显的精确字节
  bw     = EMA(bytes / copy_ms)，下限 8 GB/s                // 保守
  fps    = 1000 / g_arrival_ms_ema                         // 不要用墙钟（§3.7 的坑）
  for p in ladder:  T_P(bytes(p)) + T_C(bytes(p)) ≤ A_lo − m
                    且 bytes(p) × fps ≤ 5.5 GB/s  → 取最大的 p
  按滞回/驻留决定是否写 ctl->stream_percent_request
  日志：autoscale: age_p10 6.2 copy 3.6 bw 17.6 fps 40.0 -> 150 (was 200, switches=3, gen=5)
```

`copy_ms` 的测法：`input_copy_submit()` 之后记 `t0`，`input_copy_wait()` 之前记 `t1`、之后得到 `wait`；
`copy_ms ≈ (t1 − t0) + wait`（上界）。`--prefetch-inputs` 命中时用 `prefetch_wait()` 的对应时间。
不要求毫秒级精确——控制器有滞回和余量。

## 6. 失败模式与边界

| 情况 | 会发生什么 | 为什么可接受 |
|---|---|---|
| `A_lo` 估高了（WGC 突然变早） | 拷贝赶不上，`input_copy_wait` 变长 → `gpu`/`present` 上升、这一拍变慢 | 正确性由 copy fence 保证，**只会慢，不会读到半写数据** |
| 带宽塌了（PCIe 降速、系统内存争用） | `copy_ms` 实测变大 → 下一窗口自动降档 | 这正是闭环存在的意义 |
| 控制器抖动 | 每次改档清 4 槽 + 重建本地拷贝 → 空刷新上升 | 滞回 + 驻留 + 一次一格；日志有 `switches` 可查 |
| `request` 与生产端 ini 冲突 | producer 取 `min(request, ini)`（ini 视作画质上限） | 单向提示，消费端不会把生产端拖进等待 |
| 4K auto 的内存 | 固定 ≈280 MB | 与手动 100% 相同；跑 200 时多占 ~200 MB |
| XeFG 的尺寸假设 | MV/depth 必须同尺寸（已强制）；per-frame `resourceSize` 已经按流尺寸传 | 与 CP2077 切 DLSS 档位走同一条 `update_content` 路径 |

## 7. 验证

固定 100 / 固定 150 / auto 三组，在 30 / 40 / 60 / 120 fps 输入下各跑 ≥60 s，A→B→A 三轮：

| 指标 | 通过条件 |
|---|---|
| `skip` / `slow` | auto ≤ 固定 100 的那组（同一帧率） |
| `gpu` | auto 的 p50 不超过 `A_lo + 1 vblank` |
| `bus:` | auto ≤ 5.5 GB/s（带宽式 (2)） |
| `arr(vb)` | 不再出现 2/3/4 混桶（配合生产端 COPY 队列） |
| `gen` / `switches` | 稳定场景 60 s 内 ≤ 2 次；换场景才允许动 |
| 画质 | 以 auto 选中的固定档位做 A/B：两者应无可测差异 |

最后一条是验收集的关键：**自动的价值必须体现在“同画质下不丢帧”，而不是“偷偷降画质换帧率”。**
如果 auto 在 4K/40 稳定选 150，它就应该和手动 150 一样好看、一样稳；否则控制器只是把问题藏起来。
