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

### (d) v11：`FrameShared.semantics`（生产端**实测**的输入语义）

`ProducerInfo.input_flags` 里放的是**游戏/API 声明**的东西；`semantics` 放的是生产端
**量出来**的东西（add-on 的在线探针，每 ~2 s 一次）。为什么要分开：实测已经证明声明会错
（PBZ 对 FSR 送 UV、对 DLSS/XeSS 送像素；`cameraFOV` 是水平 FOV；XeSS 段根本没有深度）。

| 位 | 含义 |
|---|---|
| `0..1` | MV 单位：`0` 未知、`1` 像素、`2` "小量级"（UV 或 NDC —— 单靠幅值分不开，只有声明能区分） |
| `2..3` | MV 方向：`0` 未知、`1` `prev−cur`、`2` `cur−prev` |
| `4..5` | 深度方向：`0` 未知、`1` 反向 Z、`2` 正向 Z |
| `8..15` | MV 质量分 `0..255`（= `1 − 残差比`，A-v2/A-v3 用） |
| `16` | MV 单位是**实测**的（否则是声明/未知） |
| `17` | 方向是**实测**的 |
| `18` | 深度方向是**实测**的 |
| `19` | MV 可用（质量分 > 0.15）；为 0 时消费端应退回自算光流 |

`valid_w/h` 是 MV/深度纹理里的**有效子矩形**（游戏可能申请得比渲染尺寸大，PBZ 深度
2564×1444 vs 渲染 2562×1441）。

现状（v12 起）：消费端**已开始用**它——`semantics` 里的单位/符号/深度方向会与声明合并后驱动
initFlags（NDC velocity 就是靠它），而 `--mv-flip` / `--fg-mv-scale` / `--fg-depth-inv` 保留为
**覆盖**（显式开关永远优先）。

`19` 号位（`kSemMvUsable`）自 A-v3 起**真的有人消费**了：为 0 且 `kSemQualityKnown` 置位时，
bridge 这一帧改用自己算的光流（`--no-of-fallback` 可关）。它与 `--fg-of` 的区别是**不取消**
`kRequestMv`，所以生产端一说"可用"就立刻切回来。

## 4.6 v12：真实投影矩阵（`view_to_clip`）

| 字段 | 类型 | 含义 |
|---|---|---|
| `view_to_clip[16]` | `float` | 游戏自己的 view→clip，**行主序**，与 `sl::Constants::cameraViewToClip` 同布局 |
| `matrix_tick_ms` | `uint64_t` | 生产端**采集**它的时刻（0 = 没有） |

- 来源（**两条，互为备份**）：
  1. Streamline `sl::Constants.cameraViewToClip`（游戏用 SL 出 FG 时；官档注明不含 TAA jitter）；
  2. **XeFG `xefg_swapchain_frame_constant_data_t.projectionMatrix`**（游戏自己经 XeSS-FG 出 FG 时；
     jitter 走单独的 `jitterOffsetX/Y`，投影本身干净）。PBZ 在 unlocker 下的实测就属于这种：
     `04:00` 的会话里一条 `sl.const` 都没有，只有 `xefg.const`。
  两条都写进同一个槽，先到先得（同一帧内容一致）。
- **没有相机的情况**：游戏既不用 SL 也不开 FG（纯 DLSS-SR）时，我们拿不到任何矩阵，
  `--fg-fov` 继续生效 —— 数据不存在，不是没读。
- 只有在 `ProducerInfo.input_flags` 带 `kInputFlagHasMatrix`（`1<<7`）时才有效。
- **为什么需要**：DLSS-SR 不导出相机，于是所有 NGX 标题过去都靠 `--fg-fov` 猜投影
  （默认 60°，和 PBZ 真实的 46.7° 垂直 FOV 差 13°）。有了这条，`--fg-fov` 对这些标题直接退役。
- **过期即失效**：`matrix_tick_ms` 超过 500 ms 或未来时间戳，消费端就当没有，退回合成投影 ——
  游戏切走 Streamline 之后矩阵不会僵在那里。
- **交接方式**：命名共享内存 `Local\mgpu_fg_camera_v1`（结构 = `seq + tick + matrix + near/far/vfov`）。
  不能用进程内 `static`：PBZ 启动时重建 swapchain ⇒ **ReShade 重载 add-on**（一次启动装载 5 次），
  旧实例装的钩子往旧实例的全局里写，新实例的 present 路径读到 0（实测 `raw_tick=0`、写者确实执行）。
  见 `docs/findings/PBZ_NOTES.md` §5.6。
- 生产端只在**源自己没有相机**时才填（FSR 3.1 声明了 near/far/vfov，那条路径不受影响）。

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
