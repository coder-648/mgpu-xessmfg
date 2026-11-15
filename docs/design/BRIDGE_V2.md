# bridge v2：事件驱动的双卡 FG 消费端（设计，2026-11-14）

> 🧊 **legacy 设计稿**：其中 M1（事件循环）/ M2（capture 线程 + colour 队列）/
> M2.5（配对）已进入当前 `xess_mfg_consumer.exe`；M3/M4 仍待做。
> 当前形态见 [`CONSUMER_ARCH.md`](CONSUMER_ARCH.md)，
> 性能与优化见 [`../PROJECT_STATUS_2026-11-14.md`](../PROJECT_STATUS_2026-11-14.md)。

> 现状：`bridge.cpp` 已拆成 16 个模块（`BRIDGE_ARCH.md`），生产端
> `xess_mfg_producer` 由 fakerFG/引擎在 FG 节奏上 publish，**没有轮询点**；
> 消费端唯一的高频轮询是 WGC 的 `TryGetNextFrame` + `Sleep(1)`。
>
> 目标：把消费端改成**事件/fence 驱动的流水线**，让 CPU 在空闲时完全不唤醒，
> 输入拷贝、颜色捕获、XeFG 呈现各自有界、可背压；不再靠“猜槽 + 时间戳查表”。

## 1. 为什么不是小修小补

现在的主循环是单线程：

```text
for (;;) {
    prefetch_publications();      // 扫 4 个 published
    PeekMessage();
    TryGetNextFrame();            // WGC 非阻塞排空
    if (空) { Sleep(1); continue; }  // 1000 次/秒的轮询
    run_frame_ex();               // 录制 + 拷贝 + tag + XeFG + present
}
```

单线程意味着：

- WGC 的等待、输入拷贝的提交、XeFG 的 tag/present 互相串行；
- 任何一个阶段慢（`present` 等 vblank、XeFG 模型切换、WGC 抖动）都会推迟其它阶段；
- 空闲时 1000 Hz 唤醒，和游戏的渲染线程争 CPU；
- 输入 pairing 依赖时间戳/槽位猜测，WGC 抖动时命中率下降。

## 2. v2 拓扑

```text
┌─ 渲染卡（游戏进程）──────────────────────────────────────────────┐
│ engine → fakerFG → FG evaluate/present                            │
│   xess_mfg_producer: downsample MV/depth → slot[k] → publish      │
│   Signal(fwd[k], pub)；可选：Signal(pub_event)                    │
└──────────────────────────────────────────────────────────────────┘
                 │ FrameShared: mv/depth/meta + fwd + frame_key
                 ▼
┌─ 消费卡 ────────────────────────────────────────────────────────┐
│ input thread（等 publication 事件/fence，不轮询）                 │
│   新 pub → copy queue（8 allocators）→ local ring[s] → ready fence│
│   → SPSC input queue（low/high watermark）                        │
│                                                                  │
│ capture thread（WGC FrameArrived 事件）                           │
│   get frame → D3D11 copy → D3D12 colour ring → colour fence       │
│   → SPSC colour queue（cap 2）                                    │
│                                                                  │
│ matcher（事件驱动，或 presenter 内联）                             │
│   pop colour + 对应 input；按 sequence/time 配对 → FrameJob       │
│   → SPSC job queue（cap 2）                                       │
│                                                                  │
│ presenter（主线程）                                               │
│   wait {job queue, present timer/vblank, quit/hotkeys}            │
│   tag XeFG MV/depth/constants → Present → ack producer            │
└──────────────────────────────────────────────────────────────────┘
```

三条队列都是 **SPSC 有界环**，各自有 high/low watermark；没有 `Sleep(1)` 轮询。

## 3. 线程与等待原语

| 线程 | 等待 | 职责 |
|---|---|---|
| input | producer `pub` 事件或 `fwd` fence 的 `SetEventOnCompletion` | 拷贝 MV/depth，维护本地 ring + input queue |
| capture | WGC `FrameArrived` 事件 + 退出事件 | 取 colour，复制到 D3D12 ring，推 colour queue |
| presenter（main） | `MsgWaitForMultipleObjects`：job queue / present timer / WGC 退出 / hotkeys | tag、XeFG、present、OSD、控制 |
| producer | 游戏引擎/FG 节奏（已有） | publish + signal，不改 |

- **不再有 `TryGetNextFrame` 轮询**：`FrameArrived` 回调只做 `SetEvent`；
  所有 WGC 操作在 capture 线程，避免和 presenter 的 D3D12 队列互相阻塞。
- **不再有 publication 扫描**：producer 在 publish 后 signal 一个共享 event
  （协议 v16 的 `FrameShared.pub_event`，或对 `fwd` 用 `SetEventOnCompletion`）；
  input 线程被唤醒一次，拷贝一次。
- 所有 `WaitForMultipleObjects` 都带 timeout，用于退出/异常恢复，不用于正常节奏。

## 4. 帧身份与配对

不再“猜槽”，用**单调帧序号 + 时间戳校准**：

1. producer 每帧写 `frame_key`（v13 已有）+ `frame_qpc_ms`；
2. WGC colour 带 `SystemRelativeTime`；
3. 启动/分辨率切换时用最小二乘/PLL 估计 `colour_ts → producer frame_key` 的偏移；
4. 稳态按 **序号 FIFO** 消费：colour 的估计 key 与 input queue 的队头匹配；
5. 只有检测到 gap/duplicate（WGC 抖动或丢帧）时才用时间戳重同步：
   - gap：丢弃无 colour 的 input；
   - duplicate：丢弃重复 colour，不重复 tag。

这样正常路径是 **O(1) 的序号匹配**，时间戳只在异常时使用，比现在的
`pair_slot_for_wgc` 更稳，也更适合多线程。

## 5. 队列、水位与背压

| 队列 | 容量 | 低水位 | 高水位/溢出 |
|---|---:|---:|---|
| input（publication） | 4 | 2（继续预拷） | 4：丢最旧 + 计数 |
| colour（WGC） | 2 | — | 2：丢最旧（重复/过期） |
| job（待 present） | 2 | — | 2：丢最旧并记录 `OUTPUT FRAMES MISSING` |

- **input 是 eager copy**：WGC `age 8–13 ms` > 拷贝 3.6 ms，等消费后再拷会把
  拷贝放进关键路径；低水位只用于防止生产端突然变快时队列无界。
- **colour/job 用高水位**吸收 WGC 抖动；溢出丢最旧，不阻塞 producer。
- **背压到 producer**：input 队列持续高水位时，通过 v15 的 `requested_percent`
  降 StreamPercent，或直接丢最旧（当前 FIFO 的行为）。

## 6. 呈现与节奏

- presenter 只做“取 job → tag → present”，**不再录制拷贝**；
- 呈现节拍由 `Pacer` 统一：
  - 固定 `--lock`：按 `refresh/(interp+1)` 量化输入；
  - `--vrr`：按实际输入间隔生成 2–4 帧；
  - `--pace`：固定输出 tick；
  - jitter buffer（`--jbuf 1`）放在 colour/job 队列之前，而不是主循环里；
- 所有 XeFG 状态（interp 数、模型切换）只在 presenter 线程改，避免跨线程；
- 目标：正常场景 `total ≤ input_interval`、`OUTPUT FRAMES MISSING = 0`、
  `arr` 的 3x 占比 ≥ 95%。

## 7. CPU 预算

| 项 | 现状（A5/A9） | v2 目标 |
|---|---:|---:|
| bridge 进程 CPU | 5.9–6.3 ms/帧，23–25% 单核 | **≤ 3 ms/帧**（事件化 + 去轮询） |
| 空闲唤醒 | ~1000 次/秒 | **0**（等事件） |
| input 拷贝记录 | 0.2 ms | 不变（COPY 队列） |
| WGC/D3D 线程 | 大头 | 仍存在，但不再与 presenter 串行 |

## 8. 迁移步骤（不要 big-bang）

1. **阶段 1：WGC + producer 事件化（仍单线程）**
   - 用 `FrameArrived` + `g_frame_event`（现在创建了但从未 wait 的死代码）；
   - producer 侧加共享 event（协议 v16）或在共享 `fwd` 上 `SetEventOnCompletion`，
     主循环用 `MsgWaitForMultipleObjects` 同时等 {WGC, publication, quit/hotkeys}；
   - 去掉 `Sleep(1)`；publication 扫描仍保留一次/唤醒，拷贝仍 eager；
   - 加 `poll: N loops/s` 与 `proc:` 对照，目标是空闲唤醒为 0、CPU 下降。
2. **阶段 2：capture 线程 + colour 队列**
   - WGC 全移到 capture 线程；presenter 只拿 colour 队列；
   - 保留现有 `run_frame_ex` 作为 presenter 的实现，先不改 XeFG。
3. **阶段 3：input 线程 + input 队列**
   - producer publish 事件/`fwd` 唤醒 input 线程；
   - 把 `prefetch_publications` + COPY ring 移过去；presenter 只 pop。
4. **阶段 4：sequence matcher**
   - 用帧序号 FIFO 替代 `pair_slot_for_wgc`；时间戳只做 gap/duplicate 重同步。
5. **阶段 5：Pacer/jitter**
   - 把 `--lock/--vrr/--pace/--jbuf` 统一到 Pacer；移除 cadence+MV 的不稳定组合。
6. **阶段 6：清理**
   - 删除 `g_frame_event` 死代码、旧的 key-mode 预拷、`--fg warp` 等实验路径；
   - 模块按 v2 线程边界重组。

每个阶段都用 A5/A6/A10/A11 对比 `proc:`、`poll/s`、`total`、`OUTPUT FRAMES MISSING`、
`arr 3x%`，不允许行为回归。

## 9. 风险与约束

- **WGC `FrameArrived`**：当前用的是 ABI WinRT 接口，需要实现
  `TypedEventHandler`；事件回调线程与 D3D11 设备的线程模型要确认（WGC 的 frame
  pool 支持多线程，但 `get_Surface`/复制要在 capture 线程内完成）。
- **跨适配器事件**：producer 的 `fwd` 是共享 fence；`SetEventOnCompletion` 在
  共享 fence 上的可用性要在真机验证；不行就用协议 v16 的共享 event。
- **XeFG 线程约束**：tag/present 必须在同一 presenter 线程；capture/input 只准备
  资源，不碰 XeFG。
- **颜色仍是 WGC**：这条设计不改变拓扑（颜色不跨卡回传），所以不增加渲染卡
  的 +10–15% 成本；如果未来要彻底去掉 WGC，需要先解决颜色传输成本。
- **不要 big-bang**：现有 FIFO/预拷/cadence 是踩坑换来的，按阶段迁移，每步可回退。

## 10. 验收标准

- 空闲时 `poll/s = 0`，`proc:` 从 ~6 ms/帧降到 ≤3 ms/帧；
- A5 的 `pf` 仍 40/40、`cq` 0、`total` ≤ 24.5 ms；
- `OUTPUT FRAMES MISSING` 从 3–7 个窗口降到 0；
- `arr` 的 3x 占比 ≥95%，`cadence` 不再出现 `NOT-EXACT` 的常态；
- 主观：A6（固定 3x）或 A5 的平移镜头无周期性顿挫。

