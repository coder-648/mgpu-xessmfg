# 组件文档：xess_mfg_producer.dll（游戏进程内的 transport）

代码：`src/xess_mfg_producer/producer.cpp`（1443 行）。最后更新：2026-11-15。

---

## 1. 角色

**只做传输**：把 fakerFG 记录里的 MV/depth 跨卡复制进共享槽，然后发布。
它不决定 FG、不改时机、不碰颜色。

```
fakerFG.dll ──(sink, 同步, 游戏线程, cmd_list 打开中)──► producer
                                                          │
                                        claim_slot → copy → publish
                                                          ▼
                                            共享块（fence + FrameShared）
```

## 2. 入口与线程

- 由 `fakerFG` 的 `producer_link.cpp` 在启动时 `LoadLibrary` + `MgpuFgSetSink`。
- sink 回调发生在**游戏的 Present 线程**、`publish_record()` 内部 ——
  所以它必须是"录命令"而不是"等 GPU"：所有等待都在 GPU 队列上，CPU 立即返回。

## 3. 队列结构

| 对象 | 类型 | 用途 |
|---|---|---|
| `g_copy_q` | DIRECT | 需要 barrier 的路径（缩放 dispatch、状态转换） |
| `g_dma_q` | COPY | 纯 1:1 复制（DMA 引擎，无 barrier） |
| `g_frame_fence` | D3D12Fence | 游戏队列 → 复制队列的"这一帧已提交" |
| `g_copy_done` | D3D12Fence | 复制队列完成（用于 allocator 复用） |
| `fence_forward[s]` | shared NT handle | 发布信号（消费者等它） |
| `fence_back[s]` | shared NT handle | 反向（消费者用完） |

> 关键设计：复制队列**等待游戏队列的 fence**，而不是等 CPU；因此
> **游戏的 Present 从不被传输延迟**（注释里实测 25.1ms @150 vs 27.2ms @100）。

## 4. 每帧流程

```
1. claim_slot()
     信用窗口 ON：找 consumed[s] >= published 的槽（轮转起点 g_next_slot）
     都不空闲：覆盖「最旧的一次发布」，producer_overrun++
     信用窗口 OFF（消费者 30 帧没 ACK）：退回轮转
2. ensure_shared()/compute_target()
     首次或 generation 变化时：layout_streams()（64KB 对齐分槽）+ place_slots()
     按 StreamPercent 决定内容尺寸：target_dim(v, percent)
3. copy_stream(mv) / copy_stream(depth)
     源状态用 record 里 fakerFG **声明**的 state；DIRECT 列表才插 barrier
     box 取 min(源, 槽, stream 目标尺寸)
     需要缩放时：scale_dispatch()（compute shader，输出到槽）
4. publish
     g_copy_q->Signal(fence_forward[s], ++v)
     frames[s].published = v
     写 frame_id / frame_qpc_ms / jitter / render / mv_scale / camera /
        semantics / view_to_clip / fg_flags / present_id …
     frame_published++（诊断）
5. CPU 侧观察到复制完成 → frames[s].ready = 1
```

## 5. 槽位与容量

- `kMaxSlots = 4`，每流 4 个槽，槽容量按 `kStreamMaxW×kStreamMaxH`（3840×2160）预置；
- 分辨率/预设变化**不重建映射**，只改 `StreamShared.width/height` 并 bump `generation`；
- `slot_w/slot_h` 是**放置容量**，内容只占左上角 —— 消费者必须按 `slot_w/h` 建视图，
  否则行距不同会读出花屏（v9 的核心约定）。

## 6. StreamPercent（带宽旋钮）

- `requested_percent_*`（消费者写）→ `apply_requests()` → `target_dim()` → compute 降采样；
- `active_percent_*`（生产者回显）—— 控制器与 UI 读这个；
- **100 = 原尺寸；>100 = 降采样**：`target_dim = (v·100/pct) & ~7`（下限 256），
  所以 200 让每个维度减半、字节数 1/4；范围 100..400，**没有放大分支**
  （缩放后 ≥ render 时回退 1:1）。`requested_* = 0` 或越界 = 回退 fakerFG.ini；
- 改变会重算目标尺寸，内容矩形变化 → bump generation（消费者重建本地拷贝），
  槽容量与映射不变。

## 7. 输入语义探测（v11）

生产者每 ~2 s 对 MV 做一次在线探测（单位/符号/深度方向），结果写
`FrameShared.semantics` + `valid_w/h`，并用 `kSem*Measured` 标明哪些是实测值。
消费者在 API 声明缺失或可疑时用它兜底。

## 8. 诊断字段

| 字段 | 含义 | 判读 |
|---|---|---|
| `producer_overrun` | 无空闲槽、覆盖最旧的次数 | >0 说明消费者跟不上（或队列不够） |
| `producer_waits` | **恒为 0**：`SlotPolicy=wait` 从未实现（生产者固定 drop） | 不要用它判断阻塞 |
| `stream_bytes[]` | 每流实际传输字节 | 用于解释 `mvs` 时间 |
| `frame_published` | 累计发布数 | 与消费者 `pf:` 的 pub 数对照 |

## 9. 已知问题（本轮回测发现）

- **消费者侧 `pf: ... overflow` 10–20%**：那不是生产者的 overrun，而是**消费者自己的
  pub 队列（4 项）溢出**；后果是那些帧退回"帧内现拷"，把 `mvs` 压到关键路径上。
  与本文的 `producer_overrun` 是两回事，判读时别混。
- 深度流的格式族必须与槽一致（`copy_formats_ok`），否则该流被静默跳过（只打一次日志）。

## 10. 不变式

1. sink 回调内**不得**做任何 CPU 等待（它运行在游戏线程）；
2. 复制必须排在游戏队列 fence 之后（否则读到未完成的渲染）；
3. `frames[s].published` 必须在 fence 信号**之前**写（消费者用它命名 ACK）；
4. 槽位复用必须遵守 `consumed[]`（信用窗口），否则消费者会读到"更新但未完成"的帧；
5. `generation` 只允许在"内容尺寸真的变了"时 bump。
