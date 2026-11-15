# 输入预拷：从 ~90% 命中到 ~100%（设计，2026-11-14）

> **状态（2026-11-14 晨）**：已实现，但**默认关闭**。A 组第一次实机复测暴露两个问题：
> `pf` 仍只有 3–9/40 命中（`prefetch_publications()` 原来用整批 `g_copy_count` 判成功，
> 任一槽失败就整批不写 `local_pub`；已改成槽级判定），以及 **4K 光流每帧约 2–3 ms**
> （`gpu 21.2→23.55`、`load 96%→106.5%`、空 vblank 4→17）。
>
> 现在的 A/B 开关：
> - `--prefetch-publications`：启用本文的 copy-on-publication（默认 = 单槽轮转预拷）；
> - `--of-miss-fallback`：miss 时用 FX 光流（默认 = 帧内补拷；4K 代价高，见 §5）；
> - `--no-prefetch-inputs`：回到 WGC 之后的旧顺序。
>
> 默认路径仍是上一版 A 的 `gpu 21.2 / load 96% / skip 1 / 空 4`。

> 现状（A 组）：`pf` 每 40 帧命中 32–37、miss 3–8。miss 的代价不是丢帧，而是那 3–8 帧
> 要在 WGC 到达后补拷（copy 队列 3.6 ms），偶尔把 `cq` 顶到 4–9 ms，`skip`/空 vblank 跟着抖。
>
> 目标：正常游戏内**命中率 100%**，只有启动/分辨率切换/生产端重启这种瞬时例外。

## 1. 为什么现在不是 100%

```text
producer: 每帧 publish 到 slot[k]（4 槽轮转），Signal(fwd[k])
consumer: 上一帧 Present 之后，按 g_mv_slot % 4 预拷“下一槽”
WGC:      到达后用时间戳查表，可能选中另一个槽
```

三个失配源：

1. **猜槽**：预拷的槽是轮转猜的，不是“这次发布实际写入的槽”；
2. **WGC 抖动**：`arr(vb)` 是 2x/3x/4x 混合（RTSS/DWM 抖动），一帧晚到就会选到相邻槽；
3. **生产端重发/跳帧**：WGC 的重复帧、生产端漏发，让轮转和真实 publication 错位。

命中与否是**二值**的：猜中就不拷，猜不中就补拷。要提高命中率，不能改“猜”，只能改
“**不猜**”：把每一个新 publication 都拷进本地缓存。

## 2. 设计：copy-on-publication（capture-loop 预拷 + 每槽本地缓存）

```text
┌─ producer（渲染卡）────────────────────────────────────────────┐
│ on_record: 降采样/录制 → 第二队列（DMA）                         │
│ present:   写 frames[k].published = v；Signal(fwd[k], v)        │
└─────────────────────────────────────────────────────────────────┘
                 │ 共享堆：frames[4].published + fwd[4]
                 ▼
┌─ consumer（消费卡）────────────────────────────────────────────┐
│ capture-loop 预拷（无额外线程；复用 input copy 队列 + fence）       │
│   每轮循环（WGC 空闲 Sleep(1) 期间）扫 frames[s].published：      │
│     if (pub > localPub[s]) {                                    │
│         copyQueue.Wait(fwd[s], pub);        // GPU 侧等发布完成  │
│         Copy MV/depth → g_mv_local[s] / g_depth_local[s]        │
│         localPub[s] = pub;  localFence[s] = ++fenceValue        │
│     }                                                           │
│   提交；Signal(prefetchFence, fenceValue)                        │
│                                                                 │
│ 主线程（WGC 到达驱动）                                           │
│   pair_slot_for_wgc → s（时间戳查表）                            │
│   if (localPub[s] == frames[s].published) → 命中，本地缓存直接用 │
│   else → 立即补拷这一槽（fallback，记 miss）                     │
│   录 list 前：directQueue.Wait(prefetchFence, localFence[s])     │
│   tag 本地 MV/depth（--upsample 也读这里）→ XeFG → Present        │
└─────────────────────────────────────────────────────────────────┘
```

关键点：

1. **预拷由 publication 驱动，不由 WGC 驱动**。生产端一 publish，1 ms 内就开始拷；
   WGC 还要 `age`（实测 5.7–8.6 ms）才到，而拷贝只要 3.6 ms（28.1 MB @150）——**必然先到**。
2. **每个生产槽一份本地缓存**（4 槽）。`g_mv_local[s]` 已存在；depth 改成
   `g_depth_local[s]`（per-slot，不再用 ring）。4K/150 时约 4×(14.1+14.1)=113 MB 显存；
   4K/100 时约 264 MB，12 GB 卡没问题。
3. **命中判定是“本地缓存是不是这一版 publication”**：`localPub[s] == frames[s].published`。
   命中就不再有任何拷贝；WGC 到达只是选槽 + 等一个已经完成的 fence。
4. **不猜槽**：预拷轮询扫所有 4 个槽的 `published`，谁变拷谁；同一槽的新 publication 覆盖旧缓存
   （4 槽 × 25 ms = 100 ms 生命周期，WGC 的 age < 25 ms，不会在用到之前被覆盖）。
5. **预拷在主循环的 `Sleep(1)` 轮询里跑**：生产者发布落在帧间隙，1 ms 轮询足够；
   copy 队列与 direct 队列并行，CPU 不占主线程。
6. **fallback 只处理异常**：分辨率切换（generation 变化）、生产端重启、WGC 重复帧。
   这些场景本来就要重建本地拷贝，miss 是瞬时的。

## 3. 时间线（4K/40，150）

```text
t0      producer present，publish slot k，Signal(fwd[k])
t0+1ms  capture loop 看到 published 变化 → copy queue Wait(fwd[k])
t0+~2ms 拷贝开始（DMA，28.1MB）
t0+~6ms 拷贝完成；localPub[k]=v；Signal(prefetchFence)
t0+8ms  WGC 颜色到达（age 5.7–8.6）→ pair 到 k
        命中：本地已有；direct 队列 Wait(prefetchFence) 已完成（0 开销）
t0+…    tag → XeFG → Present
```

命中窗口 = `age − T_copy` ≈ 8 − 3.6 ≈ 4.4 ms，余量足够；1 ms 轮询再吃掉 1 ms。

> **命中 ≠ 已就绪**：本地缓存属于正确的 publication，但写→读是串行的：
> 生产端 DMA 写（1.2–2.2 ms @150）+ 消费端跨卡读（3.6 ms @150）≈ **4.8–5.8 ms**，
> 而 `age` 的低端是 5.7 ms。也就是说 150 档“能命中，但偶尔要等 0–1 ms”；
> 200 档只有 15.8 MB（1.2+0.9≈2.1 ms），余量 ~4 ms。要做到**零等待**，
> 自动档位应把 `T_P + T_C ≤ age_med − 1` 也作为条件（现在只用了 `T_C`），
> 4K/40 下会落在 **175–200** 而不是 150。A 组 150 的 `skip 1–4` 里就有一部分是这 0–1 ms。

## 4. 同步与数据结构

| 数据 | 位置 | 同步 |
|---|---|---|
| `frames[s].published` | 共享内存 | 生产端写；预拷轮询读 |
| `fwd[s]` | 跨卡 fence | **只在 GPU 队列上 Wait**（消费端读不到值，HANDOFF 8.6） |
| `g_mv_local[s]` / `g_depth_local[s]` | 消费端显存 | 预拷写；主线程在 `prefetchFence` 之后读/tag |
| `localPub[s]` | 消费端 | 预拷轮询写；主线程做命中判定 |
| `localFence[s]` | 消费端 | 预拷提交后写；主线程在录 list 前 Wait |
| `prefetchFence` | 消费端 fence | 预拷 Signal；主线程 direct 队列 Wait |

注意：**不要**在主线程用 `GetCompletedValue()` 读跨卡 `fwd`——它在消费节点恒为 `UINT64_MAX`
（`HANDOFF.md` 8.6）。预拷轮询只把它交给 copy 队列做 GPU Wait。

## 5. 边界与失败模式

| 情况 | 行为 |
|---|---|
| generation 变化（DLSS 档位/分辨率） | `release_local_inputs()` 清 `localPub[]`；预拷轮询重建本地拷贝；瞬时 miss |
| 生产端重启（magic/pid 变） | 同上；主线程暂停一帧 |
| WGC 重复/重组帧 | 时间戳会选到同一 publication；本地缓存命中，只是多一次消费 |
| 生产端跳帧 | 选最近的 publication；本地缓存存在（拷贝线程已拷） |
| 预拷轮询落后 > age | 主线程等 `prefetchFence`（GPU 侧），这一帧变慢；不会读错数据 |
| 输入帧率 > 消费能力 | 4 槽 ring + 覆盖最旧；本地缓存跟随最新，miss 只在跨代时 |

## 6. 实现步骤（bridge）

1. **per-slot depth**：`copy_slot_to_local(s)` 创建 `g_depth_local[s]`（当前 depth 走 ring），
   tag/upsample 在预拷模式下读 `g_depth_local[slot]`；`--depth-lag > 0` 与预拷互斥（log）。
2. **预拷轮询**：capture loop 的 `Sleep(1)` 空闲轮询里扫 `frames[s].published`，对每个新
   publication 录 `Wait(fwd[s],pub) + Copy` 到 input copy 队列，Submit + Signal；更新
   `localPub[s]`/`localFence[s]`（无额外线程）。
3. **主线程**：`pair_slot_for_wgc` 之后检查 `localPub[s]`；命中不再调 `copy_inputs_local`；
   录 list 之前 `g_queue->Wait(g_prefetch_fence, localFence[s])`；tag 用 `g_mv_local[s]`/
   `g_depth_local[s]`；`--upsample` 的 pass 同样读它们。
4. **fallback**：不命中时在 main copy 队列上补拷这一槽（现有 `copy_inputs_local`），
   并把 `localPub[s]` 更新；日志单独计数。
5. **日志**：`pf: publication copies=N, hits=…, misses=…, local lag=…`；命中率进 `--report`。
6. **默认**：`--prefetch-publications`（新名字）默认开；`--no-prefetch-inputs` 回到旧行为。

## 7. 预期

| 指标 | 现在（单槽预拷） | 新设计 |
|---|---|---|
| `pf` 命中 | 32–37 / 40（80–92%） | **40/40（100%）**，除 generation 变化 |
| miss 补拷 | 3–8 / 40 | ~0 |
| `cq` 尖峰 | 4–9 ms | 消除（不再等上一份拷贝） |
| `gpu` | 16.9–20.9 | 不变（拷贝本来就不在 3D 关键路径上） |
| `skip` / 空 vblank | 1–4 / 2–12 | 只剩 XeFG 自身的 jitter |
| 渲染卡 | 90% | 不变（DMA） |
| 显存 | 4 槽 MV+depth | 4 槽 MV+depth（113 MB @150） |

**代价**：每帧一次拷贝（1.1 GB/s @150/40fps，COPY 引擎），以及一个后台线程 +
每槽一份 depth 缓存。收益是 WGC 到达后**永远不再拷贝**，`skip` 的最后一个随机源消失。

## 8. 未命中：不补拷，用 AMD 光流替代（2026-11-14 定案）

命中率做到 ~100% 之后，miss 只剩启动、分辨率切换、生产端重启这些瞬时场景。
这些帧**不再补拷**，直接切到 bridge 已集成的 FFX optical flow：

```text
miss 帧：
  1. 跳过 copy_inputs_local（不产生任何跨卡拷贝）
  2. of_record() 用当前 WGC 颜色生成 UV 场（g_of_mv）
  3. tag：MV = g_of_mv（尺寸 = depth 流尺寸），depth = 本地最后一帧 depth
  4. 本帧照常 XeFG → Present；下一帧恢复 producer MV
```

为什么可行：

- FFX 光流已经在 bridge 里（`g_of_active` / `g_of_fresh` / `of_record` / `g_of_mv`），
  `--fg-of` 路径就是它；单帧成本 0.3 ms（4K，`PERF_BASELINE`），只用于 miss 帧的**消费**；
- tag 尺寸天然匹配：`g_of_out_w/h` 就是 depth 流尺寸，XeFG 要求 MV/depth 同尺寸；
  depth 用本地 ring 里最后一帧（旧 1 帧，比 dummy 好）；
- `--upsample` 的 miss 帧跳过重建 pass：flow MV 与最后一帧 depth 已经是流尺寸，直接 tag，
  不会出现“MV 4K / depth 1440p”那种 `ERR_INVALID_ARGUMENT`；
- 如果 flow 不可用（初始化失败/`--no-of`），退回“上一帧本地 MV”（stale 1 帧），**仍然不拷**。

代价与约束：

| 项 | 值 |
|---|---|
| 光流历史 | FFX 内部维护，必须**每帧**喂颜色（只在 miss 帧跑会用旧历史）；4K 约 0.3 ms |
| 降成本 | 把 flow context 建在 1/4 分辨率（输出仍升到 depth 尺寸），约 <0.1 ms；miss 时才用，画质足够 |
| 视觉 | miss 的那一张生成帧用光流 MV，慢/中速运动几乎不可见；快速转视角可能有一帧轻微拖影 |
| 频率 | 命中 ~100% 时，miss 只在启动/切分辨率/生产端重启；稳态 0 次 |
| 收益 | **WGC 到达后不再有任何补拷**，`cq 4–9 ms` 尖峰消失，`skip` 只剩 XeFG 自身 jitter |

日志/计数：`of: miss frame N (flow used)`，并统计 `pf misses / of fallbacks`；
`--no-prefetch-inputs` 仍然回到旧顺序（miss 补拷），方便 A/B。

> 结论：**copy-on-publication 负责“命中”，光流负责“命中不了的极少数帧”**。
> 两条加起来，WGC 到达后的数据路径只有“查表 + 等一个已完成的 fence”，没有拷贝分支。

## 9. 已落地（2026-11-14）：miss → 光流，不补拷

`bridge.cpp` 已实现上面的 miss 分支：

- 预拷命中判定提前到 OF/颜色 list 之前；
- `g_of_track`：每帧跑 FFX 光流（维持内部历史），命中帧的输出**不使用**；
- miss 帧：`g_of_miss=1`，跳过 `copy_inputs_local`（不补拷），tag `g_of_mv` + 环形 depth 的
  最后一帧；`--upsample` 的 miss 帧跳过重建 pass（两者都已经是流尺寸）；
- 无 depth 历史时（启动/切分辨率）用 flat dummy 并绑给 flow；
- `--no-of-fallback` / `--no-prefetch-inputs` 回到旧补拷路径，便于 A/B；
- 日志：`of: tracking every frame for prefetch misses`（启动）、
  `of: prefetch miss -> optical flow for this frame (no copy)`（前 3 次）。

预期：`pf` 的 miss 数字仍然存在（选槽没中），但 **miss 帧不再产生任何拷贝**，
`cq 4–9 ms` 尖峰消失；代价是光流每帧的 GPU 时间。**4K 实机不是 0.3 ms**：A 组
`gpu` 从 21.2 涨到 23.55 ms、`load 96%→106.5%`、空 vblank 4→17，即约 **2–3 ms**。
因此这条路径改成 `--of-miss-fallback` 显式打开；默认 miss 仍然补拷。若要默认启用，
先把 flow context 建在 1/4 分辨率（`--fg-of` 的 field 尺寸）再实测。

**100% 命中本身也已落地（同日）**：`--prefetch-publications` 在 capture loop 的
`Sleep(1)` 轮询里按 `frames[s].published` 把**每一个新 publication** 拷进对应槽的本地缓存
（`g_mv_local[s]` / `g_depth_local[s]`），主线程只用 `local_pub[s] == frames[s].published`
判定命中；命中后 `g_queue->Wait(g_copy_fence, local_fence[s])` 只等一个已提交的拷贝。
## 10. 队列化：`--pub-fifo`（2026-11-14）

A2 的槽级修复后 `pf` 仍只有 1–7/40，说明问题不在 key 比较，而是大量槽的预拷**本身失败**
（已加 `pf: slot N copy FAILED (...)` 诊断看 mv/depth 指针、尺寸、格式）。同时实测
`age 10.6 ms > copy 3.6 ms`，所以不必再“猜槽/比对”，按顺序排队即可：

```text
prefetch（copy-on-publication）
  for each new publication, in increasing publication value:
      copy MV+depth into g_mv_local[slot]/g_depth_local[slot]
      push {pub, slot, copyFence, generation}

main（每个 WGC 帧）
  if queue non-empty: pop head -> slot/fence -> directQueue.Wait(copyFence, fence)
  else: miss（空队列才发生）
```

- 队列上限 `kMaxSlots`（4）：生产端复用某个槽时，它的旧 publication 不能再被队列引用，
  入队前先丢掉引用该槽的 entry（`dropped by overflow`），对应“消费侧跳过了一个颜色帧”；
- 队列空/溢出**不是**配对算法问题，而是消费侧吞吐低于 40 fps 的直接表现（看 `total`）。

**A5 实测（`--pub-fifo --den 2`）**：`pf` 35–40/40 命中、`missed 0–5`、`overflow 0`、
`queue 0`，且 A2/A4/A5 都没有 `copy FAILED`——说明每槽拷贝本身成功，A2 的低命中是
key 比对失配，FIFO 绕过了它。A4（den 1）的 `cq` 中位数 17.5 ms 来自 `input_copy_begin()`
在 CPU 侧等上一批 28 MB 拷贝；已改成 **8 组 allocator/list 轮转**，正常情况 CPU 不等。
`gpu`/`present` 的分配会随 vblank pacing 互换，判断是否正常看 `total`/`load`/`skip`/空 vblank。

实现开关 `--pub-fifo`（隐含 `--prefetch-publications`，默认关）。在 `--den 2` / `--scale 0.75`
把 `total` 压回 25 ms 以内后，A5 用来单独判断 FIFO 的配对质量。
