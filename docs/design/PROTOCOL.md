# 共享内存协议（v10）

定义文件：`src/mgpu_shared.h`（两端共用）。版本号 `kSharedVersion = 10`，
**不匹配时两端拒绝通信**（旧版配新版会静默读垃圾，所以故意做成硬拒绝）。

## 1. 载体

| 项 | 值 |
|---|---|
| 命名映射 | `Local\mgpu_mailbox_v1`（`Local\` 保证只在当前用户会话内） |
| 控制块 | 映射偏移 0 起，`SharedControl` |
| 纹理区 | 偏移 `kDataStart = 65536` 开始，64 KB 对齐 |
| 纹理放置 | 两端各自 `ID3D12Device3::OpenExistingHeapFromFileMapping` + `CreatePlacedResource`，**零拷贝** |
| 槽位容量 | 每流 `kStreamMaxW × kStreamMaxH = 3840 × 2160`，`kMaxSlots = 4` |
| fence | NT 句柄值写在控制块里（生产者进程内有效），消费端用 `DuplicateHandle` 复制 |

## 2. 结构

```
SharedControl
├── magic / version / producer_pid / slots / stream_count / mapping_size
├── frame_published            诊断用
├── streams[4]   StreamShared  width/height/format/offset/slot_size/slot_w/slot_h/seq
├── fence_forward[4]           生产 → 消费（NT 句柄值）
├── fence_back[4]              消费 → 生产
├── frames[4]    FrameShared   frame_id/jitter/mv_scale/ready/相机/frame_time_ms/published
├── producer     ProducerInfo  input_api/input_flags/render_w,h/output_w,h/device_luid
├── consumer_request           消费端说"要不要 MV"（v7，单向提示）
├── generation                 内容尺寸变了（信息性）
├── consumed[4]                信用窗口：消费端已读完的 fence 值（v10）
└── producer_overrun / producer_waits / slot_policy / _pad（v10 诊断）
```

`StreamShared.slot_w/slot_h` 是**放置尺寸**，`width/height` 是**内容尺寸**。
消费端必须按 `slot_w × slot_h` 放置（行 pitch 由它决定），只拷 `width × height`。

## 3. 版本演进

| 版本 | 内容 | 为什么 |
|---|---|---|
| v4 | `kMaxSlots` 2 → 4 | 桥可以持有 1–2 帧（`--slack`）而不覆盖还没配对的 MV/深度槽 |
| v5 | `FrameShared.ready` | 跨卡 fence 在消费端不可读，消费端没有别的办法知道"GPU 真的写完了" |
| **v6** | `ProducerInfo`（LUID / api / flags / 尺寸）+ 每帧相机（near/far/vfov/frame_time） | 消费者不用再猜"另一块卡是哪块"，也能用真相机构建投影 |
| **v7** | `consumer_request` + `kInputFlagMvActive` | 消费端说"我不 tag MV 就别做 14 MB/帧 的转换与拷贝" |
| v8 | `generation` + 重建布局（尺寸变化不重启游戏） | **已被 v9 取代**：重建需要 magic/ack 握手 + 渲染线程上的有界停顿 |
| **v9** | **解耦**：槽位按 3840×2160 一次性放好，尺寸变化只改两个整数 | 零握手、零停顿。生产端只做"拷 + 发信号"，消费端只做"读" |
| **v10** | (a) `consumed[]` per-slot ACK 信用窗口 (b) `FrameShared.published` (c) `StreamShared.seq` seqlock (d) 生产端诊断计数 | 槽位覆盖从"4 槽碰运气"变成**有保证**；元数据从"读两次碰运气 + clamp"变成 seqlock |

## 4. v10 的三条语义

### (a) 信用窗口（`consumed[]`）

生产端复用槽位 k 之前必须满足：

```
consumed[k] >= 它上次发布在该槽位的 published
```

`consumed[k] == 0` 表示"还没任何消费端 ACK 过"（新起的消费端，或者根本没起）。
这种情况下生产端在**宽限期（30 帧）**后回退到旧的轮转行为，否则"没有消费端"
会变成永远没有槽位。

> 踩过的坑：判据曾经写成"有没有 ACK 过"（`any_ack == 0`）而不是"ACK 有没有
> **推进**"，结果桥一退出就永久丢 39% 的帧；另一版把粒度做到只 ACK 一个槽，
> 切档后 10 秒内丢 25–65%。见 `docs/findings/FINAL_CONFIG.md` §5.13。

### (b) `FrameShared.published`

消费端在拷贝完成后把 `published` 写回 `consumed[k]`。**消费端不能读跨卡 fence**，
所以 `published` 是它唯一能"命名我读完了哪一次发布"的途径。

### (c) `StreamShared.seq`（seqlock）

生产端写 `width/height` 时先把 `seq` 变奇数，写完变偶数。消费端读到奇数就重试，
所以永远不会把旧 `width` 和新 `height` 拼在一起。

## 5. 颜色与输入流的配对语义

**结论先说**：`MV ↔ 深度` 严格同帧；`WGC 颜色 ↔ MV/深度` **不是**帧号级对应，
而是"落后一帧 + 轮转近似"。

### 5.1 MV ↔ 深度：严格同帧（构造保证）

- 生产端：同一次超分调用拿到的两个指针，写进**同一个槽 `k`**，同一条 command list、
  同一次 fence 发布（`game_capture.cpp` 颜色/MV/深度三处都用 `g_cur_slot`）。
- 消费端：一帧只选一个 `slot`；`--depth-lag 0`（默认）时深度走 1 元环，
  等价于刚拷的那一份；宽高用同一份 `g_geo` 快照，所以两者不可能尺寸不一致。

### 5.2 颜色 ↔ 输入：一帧延迟 + 轮转近似

WGC 只在画面**被合成上屏之后**才交付，所以颜色天然晚一帧（实测 10–70 ms ≈
0.3–2 帧 @30 fps）。消费端按轮转取槽：

```
slot = g_mv_slot % slots        // 每呈现一帧 +1（bridge.cpp）
```

生产端也从 `g_slot_index` 起轮转，但要求该槽已被 ACK（v10 信用窗口，见 §4a）。
**`FrameShared.frame_id` 不参与配对**，只用于诊断与 `--dump`；v5 的
`frames[].ready` 与 `--mv-newest` 已被删除（跨卡查 fence 会把游戏的渲染线程
从 33 ms 拖到 60 ms）。

由此产生三点不确定性：

| 来源 | 影响 |
|---|---|
| WGC 延迟浮动（10–70 ms） | 名义"一帧"实际会在亚帧~1 帧之间抖 |
| 生产端丢发布会（`producer_overrun`） | 消费端轮转不跟着跳 ⇒ 配对偏 1 帧并**保持偏**（不累积漂移） |
| 无重同步机制 | 信用窗口只保证"不并发读写"，**不保证内容对得上** |

### 5.3 为什么现在可以接受

- `--depth-lag 1/2/3` 实测**全部更差**（默认 0）—— 反向印证深度已在对的位置；
- 深度有无、投影/FOV 对画质无可测影响（`docs/findings/CONCLUSIONS.md` §1）；
- **推荐配方（3x + `--fg-of`）里 MV 是消费端从当前颜色帧现算的**
  ⇒ 真正逐帧敏感的那路输入已经天然同帧，剩下的深度差一帧看不出来。

想把 §5.2 的不确定性变成可测量：见 `ROADMAP.md` §5（`frame_id` ↔ WGC
`SystemRelativeTime`）。想彻底消除它：见 `ROADMAP.md` §6（颜色也走共享槽，
代价是渲染卡 80%→95%）。

## 6. 修改协议的规矩

1. `kSharedVersion` 必须 `+1`，两端的版本检查是硬拒绝，不要绕过；
2. **字段只追加到结构体末尾**（现有偏移不能动）—— v6 就是这么做的；
3. 新增字段必须在 `mgpu_shared.h` 顶部注释里写清"为什么需要它"，
   并同步更新本文；
4. add-on 与 `bridge.exe` 必须**同一次提交构建**。版本不符会拒绝通信
   （这是特性，不是 bug）。
