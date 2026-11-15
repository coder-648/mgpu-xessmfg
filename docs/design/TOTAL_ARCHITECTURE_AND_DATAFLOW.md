# 顶层视野：完整架构图 · 数据流图 · 逐环节算法设计

> **v22（2026-11-15）**：颜色入槽。producer 的 stream 0 = 游戏 backbuffer 的整帧
> 颜色（源格式原样），与 MV/depth 同槽、同一 fence 发布；consumer 删除 WGC / marker /
> present-ring / 颜色队列，主循环改为 publication-driven。本文中 P2 颜色平面、§2、§5
> 的 WGC/marker 描述是 v21 的结构，当前以 `COLOUR_TRANSPORT_V22.md` 为准。

最后更新：2026-11-15。本文是**代码层抽象**的最终产物：把
`fakerFG.dll → xess_mfg_producer.dll → xess_mfg_consumer.exe` 这条现行链路
按"图 + 算法"重新组织，供后续任何问题分析当第一张地图用。

**与现有文档的关系（不重复、不冲突）**

| 本文负责 | 交给谁 |
|---|---|
| 顶层拓扑、三条路径、逐环节算法、状态机、不变式 | — |
| 理论（不可能三角、标量函数的边界、L0–L6 分层） | `DYNAMIC_MF_ANALYSIS.md`（结论以它为准） |
| 组件内部职责清单 | `COMPONENT_FAKERFG.md` / `COMPONENT_PRODUCER.md` / `COMPONENT_CONSUMER.md` |
| 协议字段的版本演进 | `PROTOCOL.md`；本文只给"谁写、谁读、算法含义" |
| 过程与实测数据 | `PROGRESS_2026-11-15.md` |

**读法（三层）**

1. **L-TOP（本文 §1–§7）**：先建立"谁在等谁"的直觉；
2. **L-COMP（本文 §2–§5 + `COMPONENT_*`）**：每个环节的输入/输出/状态/失败模式；
3. **L-CODE（本文的 `file:line` 锚点）**：所有结论都能落到一行代码。

> 约定：`file:line` 是 2026-11-15 工作区快照。行号会漂，函数名不会；
> 找不到行号时按函数名搜。凡是"已实现"的判断，本文只认代码，不认文档。
> 本轮审计对 `producer.cpp` / `main.cpp` / `frame_io.cpp` / `mgpu_shared.h` 做过
> 注释级编辑，这些文件的行号整体偏移 1–3 行；因此关键位置已改用**函数名**引用。

---

## 0. 五个平面：用它来分诊，而不是只用一个"帧率"

任何现象先落到下面某一个平面，再去看对应章节。混平面是这轮调试最贵的错误。

| 平面 | 内容 | 时间尺度 | 主文档 |
|---|---|---|---|
| **P1 内容平面** | 游戏渲染、上采样、MV/depth 的语义 | 帧（16–40 ms） | §3 |
| **P2 数据平面（颜色）** | WGC 捕获 → sink → 转换 → backbuffer | vblank（8.33 ms） | §2 |
| **P3 数据平面（运动）** | 共享内存槽、跨卡拷贝、本地缓存 | 帧 + 队列深度 | §3 |
| **P4 控制平面** | cadence 决策、pacer、换档、门控 | 秒级（估计）+ 帧级（判决） | §4 |
| **P5 测量平面** | QPC / DWM vblank / WGC 时间戳 / presents | 微秒–秒 | §1.3、§10 |

一条判据：**P4 只能改变"误差落在哪里"，不能消掉误差**（`DYNAMIC_MF_ANALYSIS §6`）。
如果现象是"间距不齐/内容跳/空洞"，先确认 `f ∤ R` 的算术结论，再谈控制器。

### 0.1 2026-11-15 已落地：精确配对（**覆盖 §3/§5 中的旧描述**）

本轮把配对从"上一帧 id + 1"改成"当前帧精确 id + ms 校验"，并修正了 ACK 语义。
完整设计、改动清单与实测手册见 **`PAIRING_100.md`**。

| 旧描述（下文） | 现在 |
|---|---|
| §3.6/§3.7：ACK 在该槽 GPU 工作退休后写 | 只 ACK **本地副本 fence 已退休**的版本（`mgpu_pair_core::ack_value`） |
| §5.2：staging 读天生滞后一帧 | 读当前帧（阻塞 Map 144 px；仍失败才 stale，且不参与 tag） |
| §5.3：优先 `seq12+1`、其次 `seq12` | 只做 `(frame_id&0xFFF)==seq12` 且 ms12 在 ±40 ms 内 |
| §5.4：#2 按后继恢复 | 删除；无法验证 → 该帧 1X |
| §7.2/§3.4：4 槽 | `kMaxSlots=4`、协议 v19（`mgpu_shared.h`；先试 8，lag max=1 且启动变慢，回到 4） |

日志新增 `pair:` 行（exact/ms_fail/no_slot/ambiguous/unreadable/stale/lost/mismatch/lag），
用 `scripts/pair_report.py` 判定；纯算法仿真 `tests/pair_core/run.sh`。

---


## 1. 全景架构

### 1.1 物理拓扑与执行实体

```
┌──────────────────── 游戏进程  Cyberpunk2077.exe（NVIDIA RTX 3080 Ti）────────────────────┐
│                                                                                          │
│  渲染线程                                     Present 线程                               │
│  ├ 渲染 + 上采样(DLSS/FSR/XeSS)               ├ DLSS-G / NGX Evaluate ──► fakerFG       │
│  │                                            │    harvest → g_record(MgpuFgFrame)      │
│  │                                            │    dispatch_sink → producer.on_record() │
│  │                                            │    [cmd_list 仍然打开，只录命令]        │
│  │                                            ├ Present ──► fakerFG 的 DXGI 代理        │
│  │                                            │    pass_marker：四角 6×6 编码          │
│  │                                            │    present_ring_commit(qpc, seq)       │
│  │                                            └ producer.present(queue)                 │
│  │                                                   ├ 游戏队列 Signal(g_frame_fence)  │
│  │                                                   └ 复制队列 Wait→Execute→Signal(fwd)│
│  ▼                                                                                       │
│  GPU-A: 游戏 DIRECT 队列            GPU-A: producer DIRECT(g_copy_q) / COPY(g_dma_q)     │
└───────────────┬──────────────────────────────────────────────────────────────────────────┘
                │ Local\mgpu_mailbox_v1   （命名文件映射；双方各开 heap，无 CPU 拷贝）
                │   offset 0: SharedControl（64 KB 内）
                │   offset 64 KB: 5 条流记录 × 8 槽；**只放置 1=MV / 2=depth（槽 3840×2160）
                │   与 4=scar（16×16）**；0=colour / 3=UI 未放置（§12.4）
                │ Local\mgpu_present_ring_v1（64 条 present + 128 条 marker 历史）
┌───────────────┴────────────── 消费进程 xess_mfg_consumer.exe（AMD RX 6750 GRE）──────────┐
│                                                                                          │
│  capture 线程（ABOVE_NORMAL）                presenter 线程（NORMAL，25 ms deadline）     │
│  ├ WGC 帧池 TryGetNextFrame                  ├ run_frame_ex()：17 步（§6）               │
│  ├ SystemRelativeTime / age / 到达间隔       ├ 输入：pub FIFO / prefetch / 帧内现拷      │
│  └ ColourItem 队列（深度可配）               ├ cadence core → fg_set_interp(k−1)         │
│                                              ├ 颜色：sink → convert → backbuffer         │
│                                              ├ tag MV / depth / UI                      │
│                                              └ Present(0) → XeFG 生成 k 张 → DWM        │
│  GPU-B: g_queue(DIRECT: convert) + g_copy_queue(COPY: MV/depth 本地化) + XeFG 内部       │
└───────────────┬──────────────────────────────────────────────────────────────────────────┘
                │ IDXGISwapChain（FLIP_DISCARD，buffers=2，Present(0)）
        DWM 合成（DwmGetCompositionTimingInfo：qpcRefreshPeriod / qpcVBlank）
                │
             面板：实测 120.002 Hz（8.3332 ms）
```

**为什么必须三个组件**（`mgpu_shared.h:5-8`）：D3D12 钩子是进程级的，在游戏进程里
创建第二厂商设备会死锁；拆开还保证 AMD 侧崩溃不带走游戏。

### 1.2 四个执行实体与各自的"死线"

| 实体 | 代码入口 | 节拍来源 | 超时的后果 |
|---|---|---|---|
| 游戏渲染 + Evaluate | `faker_fg/provider.cpp` | 游戏 | 游戏掉帧（与 FG 无关） |
| 游戏 Present → 发布 | `publish_record()`（`faker_fg.cpp:681`）→ `on_record()`（`producer.cpp`，`on_record`）→ `publish_frame()`（`producer.cpp`，`publish_frame`） | 游戏 present | sink 内阻塞会**直接拖慢游戏**（`COMPONENT_PRODUCER §10`） |
| consumer capture | `colour_capture_thread`（`main.cpp:286`） | WGC FrameArrived | 只是队列积压/丢帧（`qdrop`） |
| consumer presenter | `run_frame_ex`（`frame.cpp:1037`） | 输入到达（无 pacer）或 vblank 网格（paced） | 错过 vblank → 间距误差、`HITCH`、`vphase` 漂移 |
| producer GPU | `g_copy_q` / `g_dma_q` | 游戏队列 fence | 发布迟到（消费者读旧槽），**不影响游戏 present** |
| consumer GPU | `g_queue` / `g_copy_queue` | 提交 | `colour` 阻塞、`pf` 溢出、`total` p95 升高 |

### 1.3 时钟域（混用是"看起来对但一直漂"的根因）

| 时钟 | 起点 | 分辨率/量化 | 合法用途 | 禁止用途 |
|---|---|---|---|---|
| `QPC`（`qpc_ms()`） | 本机 | ~0.1 µs | 一切本地耗时、pacer、相位 | — |
| 游戏 present 时间 `frames[s].frame_qpc_ms` | 游戏 Evaluate 时刻 | 一帧多次 Evaluate（CP2077 ≈3.3×） | 计时诊断（`pq`）、相位锁定实验 | **帧间隔**（会读出 ~7.5 ms） |
| `frames[s].frame_time_ms` | producer 的 **publish 间隔** EMA | 每呈现帧一次 | **决策输入 `T`**（唯一合法信源） | — |
| WGC `SystemRelativeTime` | 合成器 | 被量化到 2/3/4 vblank | 配对（ring 查询）、`age` | 决策（会跟着合成器跳） |
| DWM `qpcRefreshPeriod/qpcVBlank` | 合成器 | 实测周期 | `V`、`vphase` | 当扫描输出真值（见 L1 盲点） |
| `frameRenderTime`（喂 XeFG） | 我们的估计 | ms | 告诉 pacer 本批覆盖多久 | **语义未文档化**（`DYNAMIC_MF_ANALYSIS §8.3`） |

> 一条铁律（本轮三次踩坑的总结）：**任何"输入速率"必须以 per-presentation 为单位**；
> 任何混入我们自己的处理间隔、丢弃、节流的估计都不可用（`DYNAMIC_MF_ANALYSIS §1.3`）。

### 1.4 身份链：颜色、运动数据、标记三者怎么对上

```
游戏一帧
  │ fakerFG harvest：seq = 每次 Evaluate 递增; present_id; frame_qpc_ms
  ├─► MgpuFgFrame.seq ──► producer frames[slot].frame_id        （运动数据的身份）
  │                       producer c->frame_published = seq      （最新发布号）
  ├─► marker payload24 = seq12 | low12(ms) + CRC-8              （颜色里的身份）
  ├─► present_ring entry = (present_qpc, eval_qpc, seq, frame_key)
  └─► 消费者：WGC 颜色 → 解码 seq12 → 找 frames[s].frame_id ∈ {seq12, seq12+1}
                                          └ 命中即精确配对（滞后一帧是构造使然）
```

**为什么不能只靠 present-ring**：ring 由两个进程各自写各自的条目（游戏侧 fakerFG 与
consumer 自己的代理实例），计数器不同，`ring_seq & 0xFFF` 与 marker 的 seq 不可靠相等；
实测"ring 反推 seq"120 帧里只对上 7 帧（`frame.cpp:1169-1177` 的注释）。
所以 **marker 是主键，ring 是回退**，`pair_slot_for_wgc()` 的顺序是
marker seq → ring seq → 时间戳邻近（`pair_dump.cpp:75-138`）。

---


## 2. 数据平面 A：颜色（WGC → sink → convert → backbuffer）

### 2.1 路径图（每一跳都标"谁持有、什么格式、多大"）

```
游戏 backbuffer（scRGB FP16，3840×2160×8B = 66.4 MB）
   │ DWM 合成
   ▼
WGC 帧池（IGraphicsCaptureFramePool，free-threaded，池深 = --pool，默认 2）
   │ FrameArrived 事件（MinUpdateInterval 默认 1 个合成周期 → 最多 ~60 fps 交付）
   ▼
capture 线程：TryGetNextFrame → ID3D11Texture2D
   │ 打点：SystemRelativeTime(ts_ms)、dt_arr（到达间隔）、age = qpc − ts
   ▼
ColourItem 队列（g_colour_queue，capture → presenter 的唯一通道）
   │
   ▼ run_frame_ex 取出一帧
D3D11 共享 sink[g_sink]（3 张轮转，keyed mutex(0, 20 ms)）
   │ CopyResource：66.4 MB，全帧
   │ 止损：三先兆任一成立就跳过（复用上一张）
   ▼
D3D12 g_queue（DIRECT）：convert 全屏三角
   │  scRGB FP16 → BT.2020 PQ R10G10B10A2（33.2 MB 写入）
   │  同一 pass 内联修复四角 marker（取内侧邻居）
   ▼
XeFG swap chain 的 backbuffer（R10G10B10A2）
   │ Present(0)（sync interval 0，XeFG 自己做 pacing）
   ▼
DWM → 面板
```

### 2.2 算法 2.1：WGC 捕获线程（`main.cpp:286-351`）

输入：WGC 帧池。输出：`ColourItem{frame, tex, wgc_ts, dt_arr, age, wall_ms}`。

```
loop:
  Wait(M2: frame_event | retire_event, timeout=100ms / M1: 1ms)
  drain_retired_frames()                     // 归还上一批 IDirect3D11CaptureFrame
  while TryGetNextFrame(&f):
      tex = capture_frame_texture(f)          // IDirect3DSurface → ID3D11Texture2D
      ts  = f.SystemRelativeTime              // 合成器时间戳（100ns）
      dt  = ts - prev_ts     (0<dt<1000ms)    // 到达间隔（仅诊断）
      age = qpc_ms() - ts    (0<age<500ms)    // WGC 排队时间
      colour_push({f, tex, ts, dt, age, now})
```

要点：
- 线程优先级 `ABOVE_NORMAL`（`main.cpp:374`）；**presenter 线程是 NORMAL**（`--cpu-low` 才会降到 BELOW，`main.cpp:671`）。
- WGC 交付被 `IGraphicsCaptureSession5::MinUpdateInterval` 限制：默认 1 个合成周期
  （~16.6 ms → 最多 ~60 帧/s），`--wgc-minint 0` 可关掉（`devices_capture_fg.cpp:385-412`）。
  这是"游戏 100 fps、WGC 只给 60"这类现象的第一嫌疑。
- `dt_arr` 是"**我们**的到达间隔"，会受 pacer/队列影响，不能当游戏帧率（`DYNAMIC_MF_ANALYSIS §2.2`）。
- 池深 `--pool` 越大，能吸收的突发越多，但捕获延迟越高（代码默认 2；历史 bat 覆盖成 4）。
- `--legacy-poll` 会退回 Sleep(1) 轮询（无 FrameArrived / publication 事件）。

### 2.3 算法 2.2：颜色 sink 轮转 + 止损（`frame.cpp:1547-1636`）

三个先兆任一成立 → 本帧不拷贝、`g_reuse_frame = true`（复用上一次写过的 sink）：

| 先兆 | 判据 | 代码 |
|---|---|---|
| 上次拷贝本身阻塞 | `g_last_colour_ms > kColourStallMs`（40 ms） | `frame.cpp:1549` |
| D3D11 侧 ≥2 张在飞 | `fence11 - completed >= 2` | `frame.cpp:1552` |
| D3D12 侧 3 帧未退 | `frame_fence pending >= kColourSinks` | `frame.cpp:1557` |

顺序细节（易错）：**先判定，再换 sink**（`frame.cpp:1580`）。复用帧必须继续读
"上一次写过的那一张"，否则 convert 会读到 2–3 帧前的旧内容（画面回跳）。
连续跳过上限 `kColourSkipMax = 4`（`frame_io.cpp:120`）：GPU 持续慢时仍然给新颜色。

拷贝前后：`AcquireSync(0, 20)` → `CopyResource` → `ReleaseSync(0)`；
拿不到锁也不再硬拷（旧代码忽略返回值，是 200+ ms 阻塞的来源，`frame.cpp:1583-1596`）。

**为什么是 66 MB 的单点**：驱动会把"写一个仍被上一帧 convert 读的资源"串行化，
实测 207–222 ms；跳过拷贝的代价是一帧重复颜色，远小于 250 ms 冻结
（`COLOUR_STALL.md`、`CONSUMER_AUDIT` 第 12 项）。

### 2.4 算法 2.3：convert pass（`frame.cpp:1904-2035`）

- 全屏三角，输出到 `g_swapchain->GetCurrentBackBufferIndex()` 的 backbuffer；
- 常量 `conv_consts[12]`（`convert_warp.cpp:14-50`，逐项）：
  `[0]=80 nits`（scRGB 1.0）、`[1]=colour_mode`、`[2]=sharpen`、`[3]=down`、
  `[4]=down_support`、`[5]=taps`、`[6..7]=dst_w/h`、`[8..10]=0`、`[11]=marker 修复开关`。
  **没有色彩矩阵**：scRGB→PQ 是着色器里的固定变换，1:1 时 `down=0/support=0/taps=1`（恒等拷贝）；
- `conv_consts[11] == 1` 时，像素着色器把四个角块（marker 为 6×6）替换为**内侧邻居**——
  这就是现行 marker 修复（不是协议里的 scar 流，见 §12.5）；
- 输出格式 `DXGI_FORMAT_R10G10B10A2_UNORM`；backbuffer 尺寸可能与捕获尺寸不同，
  viewport 用真实 backbuffer 尺寸（`frame.cpp:1948-1950`）。

### 2.5 算法 2.4：提交与 Present

```
g_list->Close()
g_queue->ExecuteCommandLists({g_list})
[cq] g_queue->Wait(copy_fence, pf/copy value)   // 输入拷贝与本 list 并行
g_api.TagFrameConstants(g_fg, present_id, &cd)
g_api.SetPresentId(g_fg, present_id)
t_present = qpc_ms()
g_swapchain->Present(0, flags)
vphase = (t_present - t0) mod period            // 仅统计，不改行为
g_queue->Signal(g_frame_fence, ++value)         // 覆盖 XeFG 自己的提交
```

`Present(0)` 不用 sync interval 1：`Present(1,0)` 实测阻塞 ~31 ms，把循环钉在 ~31 fps
（`frame.cpp:2629-2632`）。XeFG 自己按 vblank 铺 k 张。
`flags` 由 `g_present_flags()` 给出：`--tearing` 时加 `DXGI_PRESENT_ALLOW_TEARING`
（`state_overlay.cpp:339`），否则 0。它是 §11 里检验"合成层是否在改节奏"的开关。

### 2.6 颜色路径的失败模式（按出现频率）

| 现象 | 机制 | 可观测 |
|---|---|---|
| 屏幕周期性顿一下 | sink 拷贝阻塞 200 ms → WGC 溢出 → 丢帧 → reset | `SLOW frame colour=…`、`cskip=`、`reset-history: forced` |
| 画面回跳 1–2 帧 | 复用帧时 sink 仍轮转（已修） | `colour: … reusing` 与 sink 序号对不上（历史 bug） |
| 全黑但计数健康 | CopyResource 格式/尺寸不匹配（失败静默） | 首帧 `colour copy: … <== MISMATCH` |
| 颜色与 MV 差一帧 | WGC 晚一帧 + marker 只识别到 seq12+1 | `pair: … one frame behind`、`age=` |
| 交付帧率虚高 | 用 loop_fps × xN 当 show（已修） | `out: presents/vblanks` |

---


## 3. 数据平面 B：运动数据（MV / depth / UI 的跨卡传输）

### 3.1 路径图

```
游戏 GPU 资源（MV R16G16 @ render 2560×1440 = 14.7 MB；depth R32 = 14.7 MB）
   │ fakerFG harvest：资源指针 + 当前 state + validity + mv_scale + jitter + 相机 + 矩阵
   ▼  dispatch_sink(g_record 快照)  —— 在游戏 Present 线程、cmd_list 仍打开
producer.on_record(f, cmd_list)：
   ├ claim_slot()                         // 信用窗口：consumed[s] >= published
   ├ update_content()                     // StreamPercent → 目标尺寸；变化才 bump generation
   ├ [可选] scale_dispatch()              // compute 同尺寸缩放到 g_ds_tex
   └ copy_stream(list, src, stream)       // 录命令：box = min(源, 槽, 目标)
       │  list 归 g_copy_q(DIRECT) 或 g_dma_q(COPY) 或游戏 list
       ▼
producer.present(queue)（游戏提交之后）：
   ├ 游戏队列 Signal(g_frame_fence, fv)
   ├ 复制队列 Wait(g_frame_fence, fv) → Execute(copy_list)
   ├ 复制队列 Signal(g_copy_done, ++v)  // allocator 复用
   └ 复制队列 Signal(fence_forward[slot], value)
        + 先写 frames[slot] 全部元数据（published 必须最后且先于信号）
   ▼
共享内存槽 slot ∈ [0,4)：streams[1]=MV, [2]=depth, [3]=UI, [4]=scar；每槽容量 3840×2160
   ▼ 消费侧
capture 线程空闲时：prefetch_publications()（copy-on-publication，默认开）
   ├ 扫描 8 个槽：published 变了且双流都有 → 按发布值升序排序
   ├ copy_queue->Wait(fwd[s], published)   // GPU 侧等发布
   ├ copy_slot_streams(s)：MV + depth 都成功才入缓存（半份会毒化历史）
   └ g_local_pub[s]=pub; g_local_fence[s]=value; pub_q_push(...)（上限 4，同槽新发布顶掉旧条目）
   ▼
presenter：pub FIFO 按 frame_id 精确取条目 → 只排队等本地 fence → tag
```

### 3.2 算法 3.1：harvest（fakerFG 侧，`mgpu_fg_api.h` v2）

`MgpuFgFrame` 是**快照**，不是活引用（`faker_fg.cpp:177` 拷贝后派发）。字段分四类：

| 类 | 字段 | 消费者怎么用 |
|---|---|---|
| 身份 | `seq`、`present_id`、`frame_qpc_ms`、`frame_key` | 配对主键；`seq` 写进 `frames[s].frame_id` |
| 资源 | `stream[k].{resource,format,state,validity}` | producer 只按声明的 `state` 插 barrier，不猜 |
| 定时 | `jitter_x/y`、`render_w/h`、`output_w/h`、`camera_*` | `frameRenderTime`、投影、jitter |
| 语义 | `flags`（`MGPU_FG_FRAME_*`）、`view_to_clip`、`clip_to_prev_clip` | `fg_flags`、resetHistory、MV 单位 |

**绑定契约（违反会花屏/鬼影）**：`validity = ONLY_NOW` 的资源只能在 sink 窗口内拷完；
`publish_record()` 必须在 `cmd_list` 打开期间派发；同一 `frame_qpc_ms` 不重复发布
（`publish_frame()` 用 `g_last_publish_frame_qpc` 去重）。

### 3.3 算法 3.2：StreamPercent 与内容尺寸（`read_percent` / `target_dim` / `compute_target` / `update_content`）

```
每帧 on_record → update_content()：
  apply_requests()：
      percent[s] = (requested_percent ∈ [100,400]) ? requested : ini_percent
                   // 0 或越界 = **回退 ini**（不是"保持上一次请求"）
  compute_target(rw, rh):
      scaled[s] = (percent[s] > 100)                 // 只有降采样才需要 compute pass
      stream_w/h[s] = scaled[s] ? target_dim(render, percent[s]) : render
      若缩放后仍 ≥ render：退回 1:1（没有放大分支）
  只有尺寸真的变了才：写 seqlock → generation++ → memset(frames[]) → g_next_slot = 0
```

- **`StreamPercent` 是降采样旋钮**：`200` 把每个维度减半（字节数 1/4），成本按面积平方下降；
  没有"放大"路径（`target_dim` 只会变小，且 `>= render` 时回退）。两个流允许不同百分比。
- 槽容量固定 3840×2160，**内容只占左上角**；两边必须按 `slot_w/slot_h` 建视图，
  否则行距不同会读出花屏（v9 核心约定）；
- 消费者回读 `active_percent_*`，不是 `requested_*`（后者只是请求）。

### 3.4 算法 3.3：槽位分配（信用窗口，`update_credit` / `slot_free` / `claim_slot`）

```
update_credit():
    ack_sum = Σ consumed[0..3]
    if ack_sum 变化: ack_idle = 0
    else: ++ack_idle
    credit_on = (ack_idle < 30 次发布)       // 消费者 30 次发布不 ACK → 退回轮转

slot_free(s):
    if !credit_on: return true               // 旧行为
    if published[s] == 0: return true
    return consumed[s] >= published[s]       // 消费端已读完这一版

claim_slot():
    for i in 0..3: s = (g_next_slot + i) % 4
        if slot_free(s): g_cur_slot = s; return true
    // 都不空闲：覆盖"发布值最小"的槽，producer_overrun++
    g_cur_slot = argmin(g_fwd_value[s]); ++c->producer_overrun
```

设计意图：消费者按 marker/ring 精确配对，所以"最旧的那一版"最不可能正在被读。
注意：**消费者并不按 `published` 选槽**（那是旧注释的说法）；这条覆盖路径只在
信用窗口关闭（消费者 30 帧不 ACK）或所有槽都未 ACK 时才会走到，即消费者已经停滞。
注意：**`slot_policy`/`producer_waits` 是未实现的旧设计**——生产者固定写 0（drop），
没有任何 wait 分支，`producer_waits` 从不递增；`producer_overrun` 才是真实计数。

---

### 3.5 算法 3.4：录复制（`copy_stream` / `on_record`）

```
copy_stream(list, src, s, declared_state):
    if src 尺寸 > placed slot: 跳过该流（只打一次日志，整会话生效）
    if 源格式族 != 槽格式族: 跳过该流（只打一次日志）
    box = (min(src_w, slot_w, target_w), min(src_h, slot_h, target_h))
    before = declared_state ?: (MV: PSR|NPSR, depth: DEPTH_READ)   // 0 时的兜底
    can_barrier = (list->GetType() != COPY)
    if can_barrier && before != COPY_SOURCE: barrier(before → COPY_SOURCE)
    CopyTextureRegion(...)                     // COPY 队列上不插 barrier
    if can_barrier && before != COPY_SOURCE: barrier(COPY_SOURCE → before)
```

`can_barrier` 很重要：COPY 队列不能做状态转换，所以 `use_dma` 只在"两条流都已是
COPY_SOURCE（或本来就要缩放）"时成立；否则退回 DIRECT 队列。

队列选择（`on_record` 内的 DMA/DIRECT 选择）：

```
use_dma = g_dma_enabled
          && (MV 不需要缩放 or 已是 COPY_SOURCE)
          && (depth 不需要缩放 or 已是 COPY_SOURCE)
copy_list = use_dma ? g_dma_list : (g_copy_enabled ? g_copy_list : 游戏 list)
```

- **DIRECT 队列**（`g_copy_q`）：需要 barrier / compute 缩放；
- **COPY 队列**（`g_dma_q`）：纯 1:1，DMA 引擎，无 barrier；
- 缩放必须留在**游戏的 list**（`Dispatch` 在 COPY 上非法，源纹理也只在那里有效）；
  失败就整帧跳过，不允许"一条流缩放、另一条全尺寸"（XeFG 拒绝尺寸不一致）。

### 3.6 算法 3.5：发布顺序（`publish_frame`）

严格顺序（错一步就是"读到更新但未完成"或"ACK 错版本"）：

```
1. 去重：pub_fq == last_publish_frame_qpc → return
2. 更新 frame_time_ms = publish 间隔的 EMA(0.9/0.1)     // 每呈现帧一次
3. frames[slot].ready = 0                               // 先失效
4. 写 frame_id / jitter / render / mv_scale / near,far,vfov / frame_time_ms
5. 写 semantics：unit ← mv_unit_bits(); sign=1; depth=1|2
6. 写 view_to_clip + matrix_tick_ms / clip_to_prev / fg_flags / frame_key / frame_qpc_ms
7. active_mask / stream_bytes / frame_published = seq
8. producer.input_api = kInputApiFaker; input_flags |= kInputFlagMvActive（若真拷了 MV）
9. MemoryBarrier
10. 游戏队列 Signal(g_frame_fence, fv)
11. 复制队列 Wait(g_frame_fence, fv) → Execute → Signal(g_copy_done) → Signal(fwd[slot], value)
12. frames[slot].published = value 必须在第 11 步的 Signal 之前写（消费者用它命名 ACK）
```

> 注意：`FrameShared.ready` 在本版本**从未被置 1**（跨卡 fence 的 `GetCompletedValue`
> 返回 `UINT64_MAX`，生产者轮询会把游戏拖慢到 33→60 ms，`HANDOFF 8.6`）。
> 消费者因此不读 `ready`，只用 `published` + 显式 expect。

**MV 语义判定**（`mv_unit_bits`）：`MV.Scale` 绝对值 ≤1.5 → pixels；
≈ render_w → UV；≈ render_w/2 → NDC。显式 `MGPU_FG_FRAME_MV_NDC` 优先。
这个判定写进 `semantics[0:2]`，消费者只在 API 声明缺失时才用（`frame.cpp:2219-2267`）。

### 3.7 算法 3.6：消费者本地缓存（三条策略 + 命中判定）

**为什么必须本地副本**：XeFG 不能用跨卡共享堆资源（`Failed to allocate a copy of DEPTH`），
所以 `g_mv_local[s] / g_depth_local[s]` 是 DEFAULT 堆（`frame_io.cpp:337-343`）。

| 策略 | 开关 | 录制时机 | 命中判定 | 代码 |
|---|---|---|---|---|
| copy-on-publication（默认） | `--prefetch-publications` + `--pub-fifo` | capture 线程空闲时，每个新 `published` | pub FIFO 条目 + 本地 fence 完成 | `input_cache.cpp:496-564`、`frame_io.cpp:385-432` |
| 单槽预取 | `--prefetch-inputs` | 本帧 Present **之后**，为下一槽 | `pf_slot == slot && pf_gen == generation` | `input_cache.cpp:427-454` |
| 帧内现拷（兜底） | 命中失败 | `run_frame_ex` 内，占关键路径 | — | `frame_io.cpp:634` |

**发布队列（pub FIFO）算法**（`input_cache.cpp:496-564`）：

```
for s in 0..3:
    if published[s] == local_pub[s] or !fwd[s] or !mv[s] or !depth[s]: skip
按 published 升序排序（生产者是轮转发布，0..3 扫描会得到 100,101,98,99）
copy_queue->Wait(fwd[s], published[s])
ok[s] = 双流都录成功
提交一次；只有 ok[s] 才写 local_pub/local_fence 并入队
溢出的真实机制：每个槽只有一份本地副本，同一槽的**新发布**会顶掉仍排队的旧条目
              （`pub_q_push` 先把同槽条目全部删掉并 ++g_fifo_drops）
              总长上限 4（= kMaxSlots）只是安全阀
```

**命中判定的细节**（`frame.cpp:1790-1869`）：
- 有 `g_pair_seq`（ring/marker 精确 seq）→ `pub_q_pop_seq(seq)` 精确取；
- 没有 → 退回队头（旧 fakerFG 才需要）；
- 条目在但 fence 未完成 → **不等待**，`++g_pf_stale`，本帧走现拷
  （等待正是 220–280 ms 冻结的来源）；
- 完全没条目 → `++g_pf_misses`；若 marker 也没识别出这一帧，才 `resetHistory`。

### 3.8 算法 3.7：跨适配器 fence 与 ACK

```
生产者：fence_forward[s] 是 NT handle（本进程有效）
消费者：OpenProcess → DuplicateHandle → ID3D12Fence
        GetCompletedValue() 恒为 UINT64_MAX（跨节点不可读）
        ⇒ 只能 SetEventOnCompletion(expected, event) + Wait
        ⇒ expected 由 frames[s].published 推算，维护在 g_fwd_expect[s]
ACK：  该槽 GPU 工作退休后写 consumed[s] = frames[s].published
        绝不写"正在读"和"下一帧要读"的槽
```

`ack_previous_slot()`（`input_cache.cpp:58`）在 `gpu_throttle()` 之后调用——
顺序不变式 4（§8）。`wait_slot()`（`frame_io.cpp:295`）每次 CreateEventW 一次，
因为复用事件需要 `GetCompletedValue` 复检，而这里读不到（`frame_io.cpp:299-303`）。

### 3.9 带宽账（4K / render 2560×1440）

| 项 | 公式 | 100% | 备注 |
|---|---|---|---|
| MV 跨卡 | 2560·1440·4 B | 14.7 MB/帧 | R16G16 |
| depth 跨卡 | 2560·1440·4 B | 14.7 MB/帧 | R32 |
| 合计（默认双流） | — | **29.4 MB/帧**；40 fps ≈ 1.18 GB/s | 这是"唯一离开渲染卡"的流量 |
| 颜色 sink 拷贝（卡内） | 3840·2160·8 B | 66.4 MB/帧 | 不是 PCIe，但占 AMD 显存带宽 |
| convert 写 | 3840·2160·4 B | 33.2 MB/帧 | R10G10B10A2 |

`StreamPercent > 100`（降采样）按面积平方缩小字节数；`--no-depth` 直接省一半。
`stream_auto.cpp` 的控制器按 WGC `age` 与 `total` 的预算在阶梯上调整
（`stream_auto.cpp:54-176`，升降档分别需要 4/2 个窗口的滞回）。

---


## 4. 控制平面：速率 → 分配 → 预算 → 提交

> **先确认现在跑的是哪条控制路径**：默认（不加 `--interp-cadence`）是**固定倍率**——
> `--interp N` 直接决定 `g_fg_interp_actual`，`cadence_core` 根本不运行，也没有 pacer；
> `--interp-cadence` 才会启用 `cadence_core`（默认 `policy=optimal`）。
> `--dyn-mfg`（OptiScaler 的控制器）与 `--cad-epoch`（秒级档位）、`--cad-source tick`
> 是另外三条**互斥的**路径，都只在显式开关下走。读日志先看 `cadence core:` 行是否存在。

### 4.1 形式化（把"调参"变成"解一个问题"）

```
状态  x_t = (T̂_t, V, J, k_{t−1}, m_{t−1}, paced_{t−1}, φ_t, regime, dwell, J_learned)
观测  o_t = (frame_time_ms 的 16 样本中位数, DWM V, 上一批 presented, 上一帧 GPU 退休)
决策  u_t = (k_t, m_t, paced_t, B_t)      k ∈ [2, J_eff], m ∈ [1, 8], B = frameRenderTime
约束  (a) 一批 k 张正好占 k·m 个 vblank（不能压缩/超铺）
      (b) paced 时等待 B − T̂ ≤ Λ（--cad-latency，默认 2 ms），且不允许加速输入
      (c) k ≤ J_eff = min(--interp+1, DLL max, learnedJ, 成本上限)
      (d) 换档必须排空上一批（GetLastPresentCount）且满足驻留 dwell
目标  min  wSmooth·qEff + wFps·|out−target|/target + wLatency·lat + wOver·overEff
      + wAlias·alias + wSwitch·换档惩罚
```

这解释了为什么"再调一个 hold"不会消失伪影：**约束 (a) 是物理的**，
目标函数只能在可行域里选点（`DYNAMIC_MF_ANALYSIS §6`）。

### 4.2 L1：速率估计的三级流水（代码全是串联的，这就是 0.4–0.5 s 阶跃滞后的来源）

```
级 1  producer.publish_frame()：publish 间隔 EMA(0.9/0.1)
        → FrameShared.frame_time_ms（每呈现帧一次；不是 Evaluate 间隔）
   ▼
级 2  consumer frame.cpp:2280-2345：最近的 16 个 frame_time_ms 取中位数
        → g_prod_interval_ms（决策用的 T）
   ▼
级 3  cadence_core.h:685-735：
        ring[16] 中位数 → 单样本夹到 3×中位数 → 均值 → EMA(α = 2/(smooth+1))
        启动特例：1 个样本直接用；2 个样本取较短者（防首帧 400 ms 拖偏）
   ▼
过滤  raw ≤ 0 / >1000 → kHitch（不改 k）
      raw > 250 ms → kHitch
      raw > 2.5×EMA 且连续 <2 次 → kHitch；连续 2 次才接受为真实阶跃
```

**为什么是三级串联**：`A6` 实测 43→30 fps 阶跃时，`optimal` 需要 13 帧（433 ms）
才把 k 从 3 切到 4，`locked` 16 帧（533 ms）。每一级都有自己的窗口，
级联后总滞后 ≈ 8 + 12 帧（`DYNAMIC_MF_ANALYSIS §11 A6`）。

### 4.3 预算：`frameRenderTime` 到底喂什么（`frame.cpp:2550`、`cadence.cpp:807-900`）

优先级从高到低：

```
1. --fg-rt 0                         → 0（实验）
2. --out-clock                       → P = k·V（固定 tick）
3. cadence core 的 d.renderTimeMs（**仅 --interp-cadence**）：
     optimal 非 paced  → T（实测输入间隔）
     optimal paced     → k·m·V
     locked            → k·V
     fill              → T；--cad-budget vblank 时 → k·V
4. 非 cadence 模式（**默认**）：rt_quant（16 样本中位数吸附整 vblank），带单帧尖峰处理
```

尖峰处理（`cadence.cpp:741-789`）：单帧 `arr_last > kSpikeRatio·plan` 时，
把这一帧的 `frameRenderTime` 延伸到真实的 vblank 数（≤3×plan），避免
"一个迟到输入 → 一个空刷新周期"（`out: vblanks > presents`）。
cadence 模式下关闭该分支（批次由控制器决定，改预算反而制造间距错配）。

### 4.4 四种 policy 的算法（`cadence_core.h`，仅 `--interp-cadence`）

> 不开 `--interp-cadence` 时这一节全部不执行：`--interp N` 固定，k = N+1。

#### 4.4.1 `even`（恒定整数倍率，默认之外的保守档）

```
kNeed = ceil(0.96 · targetHz · T / 1000)        // 输出不低于目标的 96%
kNeed = clamp(kNeed, 2, J_eff)
若 kNeed > k: 立即升（绝不让输出塌下去）
若 kNeed < k: 连续 downBatches(10) 帧才降一档
预算 = T
```

#### 4.4.2 `fill`（相位 + 死区）

```
lam   = min(want, J_eff)                  want = (T/V)·target/R
acc   = φ + lam
kRound= clamp(round(acc), 2, jEff)
kCap  = clamp(ceil(want), 2, jEff)        // over-refresh guard：不许超过本帧经过的 vblank
errKeep = |acc − k_last|                  // 与"当前生效的 k"比，不是与 kRound 比
若 kRound ≠ k_last 且 errKeep > deadband(0.60):
    k ← clamp(k_last ± 1, 2, kCap)        // 一次只走一格
否则 k ← k_last
Commit: φ += lam − appliedK;  φ = clamp(φ, ±max(deadband, 1))
```

死区的意义：λ=3.4 时产生 3,4,4,4… 的"长跑"，而不是每帧在 3/4 之间翻
（每次换档 ≈ 丢一张输出）。

#### 4.4.3 `locked`（策略 v2：算术决定 k，预算恒 k·V）

```
wantK = target · T / 1000
kLo = floor(wantK), kHi = ceil(wantK)     // 都夹到 [2, J_eff]
underFrac = (R − kLo·f) / R
kNear = (underFrac ≤ 5%) ? kLo : kHi      // 宁可丢帧，不要 >5% 空洞
换档：err(k) = |k·f/R − 1|
      仅当 err(current) − err(kNear) > 3% 且连续 3 帧指向同一个 kNear
预算 = k·V；φ ≡ 0（不做平均修正）
```

`k=3 @35fps` 会得到 105 Hz（12.5% 空洞），所以规则改成"空洞 ≤5% 才用 floor"，
35 fps 应为 4x（14% 丢帧）——仿真 table 5c。

#### 4.4.4 `optimal`（默认：在 (m,k,paced) 网格上按代价选）

候选枚举（`cadence_core.h:744-903`）：

```
paced:   m ∈ [1, ceil(slots)+2]（≤8），k = ceil(slots/m)
         要求 paceWait = k·m·V − T ∈ [0, Λ]
unpaced: k ∈ [2, min(J_eff, ceil(slots)+1)]，m = round(slots/k)
```

代价（`costOf`，`cadence_core.h:770-808`）：

```
unpaced:
  q      = |slots − k·m| / k                 // 长保持率（每张输出被多保持的 vblank 占比）
  rawOut = k·f
  out    = min(R, rawOut)
  over   = max(0, rawOut − R)/R              // 超额供给 → 丢输入帧
  qEff   = max(0, q − 0.05)                  // 感知死区：≤5% 不可辨
  overEff= max(0, over − 0.05)
  lat    = 0.5·(k−1)·T
  cost   = 1.00·qEff + 0.50·|out−target|/target + 0.01·lat/16.7 + 0.50·overEff

paced:
  batch = k·m·V;  out = R/m
  alias = max(0, f − 1/batch)/f
  lat   = 0.5·(k−1)·T + 0.5·batch
  cost  = 0.50·|out−target|/target + 0.01·lat/16.7 + 0.30·alias
```

切换：只有 `bestCost + wSwitch(0.05) + switchMargin(0.02) < currentCost` 才换；
换档还要 `s.canSwitch`（排空）且 `framesSinceSwitch ≥ dwell`（升档可越过驻留）。

#### 4.4.5 三处算法修正（已落地，`DYNAMIC_MF_ANALYSIS §11`）

| # | 修正 | 等价说法 |
|---|---|---|
| A1 | 显式 `over` 项 | `min(R, k·f)` 让 7% 丢帧在代价里"看不见"，必须单独计价 |
| A2 | `qEff/overEff` 的 5% 死区 | 3% 空洞主观可接受，按线性计价会过度惩罚欠供 |
| A3 | `locked` 改成 under≤5% 取 floor | `round` 在 35 fps 选 3x → 105 Hz（12.5% 空洞） |

---

### 4.5 换档门限与"只按生效 k 记账"（`cadence_core.h:527-570`）

**四道门**（任何一道不满足，`k` 不变，`reason=deferred`）：

| 门 | 判据 | 代码 |
|---|---|---|
| 上限 | `k ≤ J_eff` | `frame.cpp:1463-1465` |
| 排空 | 现行：`GetLastPresentCount + 2 ≥ g_present_id`（计**调用**，故闸门几乎恒真）；legacy hold 路径用 `g_cad_expected_out` | `frame.cpp:1440`、`cadence.cpp:295` |
| 驻留 | `framesSinceSwitch ≥ LearnedDwell()`；升档可紧急越过 | `cadence_core.h:463-484` |
| 成本/上限变化 | `overCap` 时允许立刻降 | 同上 |

**Commit 的记账**（这是"长期均值不漂"的关键）：

```cpp
if (ok && appliedK == wantK) { ... } else { _deferred++; }
_phase += lam - appliedK;          // 只减真正生效的 k
_phase = clamp(_phase, -max(deadband,1), +max(deadband,1));
_lastK = appliedK;
```

旧实现减的是"想给的 k"：被拒绝/被延迟时，账面上已经出了 k 张，实际没有 →
平均值悄悄漂。相位夹取也从 `deadband+1+2·dwell`（允许 ±25 帧欠账）改成 ±1 帧：
那笔欠账在 `k ≤ ceil(want)` 下永远还不掉，只会把 k 顶在档位上端（table 5b）。

**两道学习**（默认只测量，不自动改策略）：

| 学习 | 触发 | 动作 | 开关 |
|---|---|---|---|
| 切换成本 | `NoteSwitchLoss(lost)` ≥8 样本 | `autoTuneDwell` 打开时加/减驻留；`fill` 明显亏本就退回 `even` | `--cad-switch-loss auto`（默认 measure-only） |
| 可实现上限 | 连续 3 次 `presented < k` | `learnedMaxK--`；连续 400 次满档再试探 +1 | 自动，但 MV 门控期间 `ForgetShortfall()` |

### 4.6 pacer 与 vblank 网格

**pacer 只在以下情况存在**：`--interp-cadence` 下的 `fill/even`（policy≠optimal）、
`optimal` 的 paced 分支，以及 `--out-clock`。
默认（固定倍率）与 `--interp-cadence` 的 optimal 非 paced **都没有 pacer**——
提交完全自由跑进 2 缓冲队列（`DYNAMIC_MF_ANALYSIS §10 F5`）。

两个 pacer 的语义不同，不能混为一谈：

```
fg_input_pace(k)        // fill/even 的旧路径（frame.cpp:45-75）
  due = g_fgi_next_ms                      // 上一批排定的起点
  if now < due:  等到 due
  else:          ++late; due += (floor((now−due)/vb)+1)·vb   // 迟到只跳整 vblank
  g_fgi_next_ms = due + k·vb               // 保持 mod vb 的相位，绝不以 now 起锚

fg_input_pace_ms(B, Λ)  // optimal 的 paced 分支（frame.cpp:79-100）
  due = g_fgi_next_ms
  if due > now && (due−now) <= Λ: 等到 due; next = due + B
  else:                           立即提交; next = now + B   // 过期不补，无债务
```

前者是**相位锁定网格**：F1 修掉的是它（旧实现 `next = now + k·vb` 把每次迟到永久
编进相位 → `late` 只增不减、`vphase` 游走 0.2→6.1→3.9→7.3 ms）。
后者有意"过期即重锚"，因为它的目标只是"有机会就量化到整 vblank"，不承担长期相位；
`late` 在它这里计的是真实迟到次数。`--out-clock` 走 `oc_tick()`，是第三套独立网格。

**vblank 校准**（`cadence.cpp:948-1017`）：

```
优先 DwmGetCompositionTimingInfo:
     period = qpcRefreshPeriod / QPC_freq（本机 8.3332 ms = 120.002 Hz）
     t0     = qpcVBlank
     合法区间 (2 ms, 40 ms)
回退：对 overlay 显示器 IDXGIOutput->WaitForVBlank 取 120 个 vblank 的长基线平均
     （9 次中位数会给出 119.674/119.890/120.291 —— 噪声与要消除的漂移同量级）
phase(qpc) = (qpc − t0) mod period
```

校准现在是 `init_fg()` 末尾无条件调用（`cadence.cpp:1772`），固定倍率基线也有
`period/vphase`（F2 已修）。
注意：`DwmGetCompositionTimingInfo(nullptr, …)` 取的是**主显示器**的合成时钟；
overlay 放到副屏时会用错周期（回退路径反而按 overlay 显示器枚举）。见 §12.6 #12。

### 4.7 门控与保护（每一条都对应一类"看起来像 bug"的现象）

| 机制 | 触发条件 | 动作 | 为什么必须这样 |
|---|---|---|---|
| **MV gate** | `active_mask` 无 MV 或 `pub_stall_frames ≥ 8` | `SetEnabled(0)`；恢复时 `SetEnabled(1)` | 菜单/加载时 MV 是旧数据，硬生成会花屏；门控期间**不推进 cadence、不计 shortfall** |
| **tag 判空** | MV/depth 资源未建好 | 跳过该 tag | 空指针让 XeFG 返回 `ERR_OTHER` |
| **colour stall guard** | 见 §2.3 | 跳过 sink 拷贝、复用上一张 | 200 ms 冻结 > 一帧重复 |
| **keyed mutex 超时** | `AcquireSync(0,20)` ≠ S_OK | 复用、计数 | 旧代码忽略返回值，照样拷 → 阻塞 |
| **`--lock N`** | 可选 | 丢早到的捕获帧，把输入压到 N | 只能"规则化"，不能消除非整除失真 |
| **`--no-throttle`** | 可选 | 去掉深度 1 的 GPU 等待 | +1 帧松弛，但 copy queue 可能回退 |
| **`--buffers 3`** | 未默认 | 合成器多一张松弛 | 从未实测（P1 #3） |
| **`--cpu-low`** | 可选 | 整个进程 + 主线程降到 BELOW_NORMAL | 与游戏抢 CPU 时用；默认 presenter 是 NORMAL |

### 4.8 控制器的天花板（一句话）

`f ∤ R` 时 `e = |k·f/R − 1|` **必须**出现在间距、内容或空洞之一，四层（L1 速率 / L2 窗口分配 /
L3 落点选择 / L4 闭环）只能决定它落在哪里、以及不被非算术因素（估计误差、单帧超时、
3x/4x 复用 MV）放大。**整除点已做到 0.1% 间距 / 99.9% 填充（锁 40 + optimal + 实测 vblank）；
非整除点没有第四种选择。**（完整证明见 `DYNAMIC_MF_ANALYSIS §6`）

---


## 5. 配对：颜色里的 24 位身份 → 运动数据槽

### 5.1 编码（fakerFG，`mgpu_shared.h` 的 marker 段、`passes.cpp:281-314`）

```
payload24 = seq12 | (low12(ms) << 12)          // 短雪花：不要 41 位时间戳
code32    = payload24 | (CRC8(payload24) << 24)
每块 8 个数据位（XOR 0x5A）+ 1 个参考单元（数据位全白时为黑，保证 min/max 可判）
四角各 6×6 px（3×3 个 2×2 px 单元），margin = w/32、h/32（4 字节对齐）
一次 DrawInstanced(3) + 全屏 scissor，PS 自己丢到四角（不做 4 次 scissor draw）
```

为什么是 24 位：marker 只有 144 px/行，完整 64 位雪花的 ms/worker 位没用；
完整 frame id 由 `frames[s].frame_id` 解析，24 位不需要全局唯一。

### 5.2 解码（consumer，`marker_decode_all` / `mgpu_pair_core.h`）

```
1. 四角各做一个小 staging copy；读的是**本帧刚填的那一格**
2. 先 Map(DO_NOT_WAIT)；WAS_STILL_DRAWING 时再做阻塞 Map（144 px）
   仍失败才退回上一帧的 staging，并置 stale=true（该 id 永不参与 tag）
3. 每块 min/max 判位（参考单元保证块内总有两级）
4. 只有 4/4 块可读才继续；否则返回 0xFFFFFFFF
5. 拼 code32 → payload + CRC8；CRC 不对时尝试翻转 1 位修正
6. 返回 payload24
```

> 2026-11-15 改动：旧实现总是读**上一帧**的 staging（非阻塞 Map），因此 id 滞后一帧、
> 靠 `+1` 补偿；丢帧/重复帧会静默错配。现在读当前帧，`+1` 规则已删除。

### 5.3 精确配对：id + 时间双条件

`mgpu_pair::find_exact()`：

```
候选 slot 满足：
    (frames[s].frame_id & 0xFFF) == marker.seq12
且  circular_distance(frames[s].frame_qpc_ms & 0xFFF, marker.ms12) <= 40 ms
取 published 最大的候选；两个候选（4096 帧回绕）→ ambiguous，不 tag
```

- `ms12` 用于拒绝过期发布、消解回绕；
- `no_slot`（发布已被覆盖）与 `ms_fail`（同 seq12、不同时间）都走 1X；
- 选槽后、tag 前再与**本地副本**的 `frame_id` 比对（`g_local_id`），
  不一致则不 tag。

`find_exact()` 只接受 `(fid & 0xFFF) == s12` 且 ms12 在 ±40 ms 内的槽；
命中后 `g_marker_seq = frames[slot].frame_id`，**在状态机之前**设置。
不再有"找不到就试 s12+1"的分支。

### 5.4 配对状态机（`run_frame_ex`）

输入：`marker_slot`（精确解析结果）、`pub_fresh`（生产者是否还在发布）、
`ring_ok/ring_seq`、`g_marker_last_seq/slot`、`g_reuse_streak`。

| # | 条件 | 动作 | resetHistory | 备注 |
|---|---|---|---|---|
| 1 | marker 精确匹配（id + ms） | 精确配对并 tag | 否 | `g_pair_verified=true` |
| 2 | marker 不可读 / 没有槽 / ms 失配 | **该帧 1X**（不 tag） | 恢复时一次 | `g_pair_lost++` |
| 3 | marker 是旧 code（比 last 小且差 <1000） | 视为重复 present，复用上一帧 | 否 | 仅当 id 与本地副本一致才 verified |
| 4 | 同一 seq（游戏一帧 present 两次） | 复用上一帧（同一 slot/MV） | 否 | 重复交付的正常路径 |
| 5 | 无 marker，ring 落后且生产者仍在发布 | 复用（复用连击上限 6） | 否 | 只复用颜色，不 tag |
| 6 | 无 marker，ring 恰好 +1 | 记录为 fallback，**不 tag** | 否 | 旧版会当精确；已改 |
| 7 | 生产者停摆（菜单/加载/暂停） | 原样呈现；MV 门控关 FG | 否 | 不视为丢配对 |
| 8 | 无法识别且生产者仍在发布 | 复用 + 计数；**不再强制 reset** | 否 | 由 1X + 恢复时 reset 取代 |

两条硬约束：
- **复用连击 ≤6**：生产者停摆时 ring 会一直返回最后 seq，不加限就永久冻屏；
- **颜色队列溢出不再 reset**：与"配对失败"无关；只计数。

### 5.5 reset 归因（日志能直接分开三类）

```
reset-history: forced for frame seq=N (pair-gap a, colour-drop b, producer c so far)
```

| 来源 | 计数器 | 真实含义 |
|---|---|---|
| `pair-gap` | `g_reset_pair_gap_count` | marker 无法识别且生产者仍在发布（真断点） |
| `colour-drop` | `g_reset_colour_drop_count` | 颜色队列丢帧（**已不触发 reset**，只计数） |
| `producer` | `g_reset_producer_count` | 游戏自己声明 `kInputFlagResetHistory`（场景切换/传送） |

另有一个开关 `--out-clock` 会整体关掉"强制 reset"路径（`!g_out_clock` 判断），
因为 out-clock 自己负责节奏。

---


## 6. 一帧的 17 步（按代码真实顺序，`run_frame_ex`，`frame.cpp:1037-2744`）

> `COMPONENT_CONSUMER.md §3` 是同一流程的摘要版；两处顺序有差异时**以本节为准**，
> 因为本节逐条对过行号。

| # | 步骤 | 关键动作 | 失败模式 | 代码 |
|---|---|---|---|---|
| 1 | **marker 解码** | 四角 staging read → min/max → CRC8（可 1 位修正） | 不可读 → 0xFFFFFFFF（随后按后继恢复） | 1052-1062 |
| 2 | **marker→slot 解析** | 找 `frame_id == seq12+1`（优先）或 `== seq12` | 都不在 → `marker_slot=-1` | 1072-1099 |
| 3 | **发布新鲜度** | 是否有 `published` 变化；`pub_stall_frames` | 停摆 → 后续走菜单路径 | 1104-1119 |
| 4 | **配对状态机** | §5.4 的 8 种情形 | 无法识别 → 复用 + reset | 1129-1247 |
| 5 | **世代/退休** | `service_shared_layout()`；`collect_retired_inputs()`（延迟 16 presents 销毁） | 世代变化 → 释放本地副本与 pub 队列 | 1263-1265 |
| 6 | **流请求板** | `consumer_request`（v7）+ `request_mask`（v14） | 写错 → 生产者白传或该流传不到 | 1287-1317 |
| 7 | **控制判决** | 四选一：`--dyn-mfg` / `--cad-epoch` / `--interp-cadence`(core) / 固定倍率（默认，不动 k）；core 走 `Update(s)` | `inputMs` 无效 → `kHitch`，k 不变 | 1321-1477 |
| 8 | **GPU 深度 1** | `gpu_throttle()` 等上一帧 `g_frame_fence` | 等太久 → `gpu=` 升高、错过 vblank | 1480、`osd.cpp:296` |
| 9 | **归还信用** | `ack_previous_slot()` 写 `consumed[]` | 写太早 → 生产者覆盖正在读的槽 | 1483 |
| 10 | **换档 + 记账** | `fg_set_interp(k−1)`；`Commit(appliedK, ok)` | DLL 拒绝 → `refused`，相位不丢账 | 1486-1500 |
| 11 | **XeLL + 输入间隔** | `xellSleep`；`g_frame_ms_ema` | XeLL 睡眠计入 `xell=` | 1505-1521 |
| 12 | **颜色落 sink** | 三先兆止损 → sink 轮转 → `AcquireSync` → `CopyResource` → `fence11` | 阻塞 → `cskip=`、复用上一张 | 1547-1649 |
| 13 | **选槽** | 复用帧用 `g_last_slot`；否则 `pair_slot_for_wgc`；marker 有最终否决权 | 选错 → 用过期 MV（鬼影） | 1674-1776 |
| 14 | **输入命中** | pub FIFO / 单槽预取 / 现拷；`pf_hit / pf_stale / pf_misses` | fence 未完成 → **不等待**，本帧现拷 | 1790-1881 |
| 15 | **颜色转换** | backbuffer barrier；convert 全屏三角（含 marker 内联修复）；`marker_probe_overlay` | 格式/尺寸不匹配 → 黑屏但计数健康 | 1885-2035 |
| 16 | **输入拷贝 + tag** | `copy_inputs_local` 或 COPY 队列；MV gate；tag MV/depth/UI（`tag_list=nullptr` 表示活到下次 Present） | 资源为空 → 跳过 tag（否则 `ERR_OTHER`） | 2052-2189 |
| 17 | **提交 + 收尾** | `Close/Execute`；`frame constants`（投影/jitter/MV scale/resetHistory/renderTime）；pacer；`TagFrameConstants`；`Present(0)`；`Signal`；`GetLastPresentStatus`；统计 | Present 阻塞 → `present=`；`framesPresented<k` → learnedJ 降档 | 2191-2744 |

### 6.1 单帧时间轴（40 fps、25 ms 预算、4K，典型值）

```
t (ms)  capture 线程（异步）        presenter 线程（g_queue 提交）              GPU-B
0.0     WGC 交付帧 N ──► 队列       marker 解码 0.1–0.5
0.3                                 配对 + layout 0.1
0.5                                 cadence Update ~10 µs
0.6                                 gpu_throttle：等第 N−1 批退休（5–20，常为最大项）
  ~                                  fg_set_interp + Commit（µs）
  ~                                  colour：mutex + 66 MB CopyResource（正常 2–6）
  ~                                  fence11 信号 → fence12 Wait（GPU 侧）
  ~                                  convert draw 录制（CPU 0.2）──────────────► 执行
  ~                                  输入：命中 0 / 未命中 1.8 + 拷贝 ──────────► COPY 队列
  ~                                  MV gate + tag（µs）
  ~                                  Close/Execute ────────────────────────────► 排队
  ~                                  frame constants（µs）
  ~                                  pacer（仅 paced，等待 ≤Λ）
25−ε                                TagFrameConstants + Present(0) ───────────► XeFG 铺 k 张
                                                                              （k 个 vblank）
```

判读：**`gpu`/`present` 与 `colour` 是三个会真正吃掉 25 ms 的量**；其余都是微秒级。
`total` 的 p95 才是"能否踩上 vblank 预算"的指标（`DYNAMIC_MF_ANALYSIS §2.4`）。

### 6.2 生产者一帧的并行时间轴（与上面不共享线程，但共享 GPU 时间）

```
游戏 Present 线程：on_record 录命令（CPU ~0.1 ms/流） ──► publish（元数据 + fence）
游戏 GPU：渲染/上采样 ──► [g_frame_fence]
producer GPU：Wait(g_frame_fence) → 复制 29.4 MB → Signal(fwd[slot])
consumer capture 线程：看到 published 变化 → Wait(fwd) → 再拷 29.4 MB 到本地 DEFAULT 堆
```

同一条数据要搬两次（游戏卡 → 跨卡槽 → AMD 本地），第三次是 XeFG 自己的内部拷贝：
它拒绝直接使用跨卡共享堆资源。去掉"本地副本"这一跳在当前 DLL 下不可行；
去掉"跨卡槽"这一跳等于改成单卡（LOCAL 模式）。

---


## 7. 生命周期与状态机（总览）

### 7.1 生产端槽位（slot）

```
        claim_slot()                    copy_stream()                 publish_frame()
   ┌──────────────┐  录完命令   ┌────────────────┐  复制队列完成  ┌──────────────┐
   │   FREE       │───────────►│   RECORDING    │──────────────►│  PUBLISHED   │
   │ consumed≥pub │            │ g_copied_mask  │  Signal(fwd)   │ published=v  │
   └──────────────┘            └────────────────┘                └──────┬───────┘
          ▲                                                             │
          │                     consumed[s] = published（消费者 ACK）   │
          └─────────────────────────────────────────────────────────────┘
```

- `g_slot_locked` 保证一帧只 claim 一次（同一帧多次 Evaluate 只录一次复制）；
- `producer_overrun` 是"没有 ACK 空闲槽"的次数；`producer_waits` 只在 `slot_policy=wait` 出现。

### 7.2 消费者本地缓存（per slot）

```
   EMPTY ──(prefetch_publications 录完双流)──► COPYING ──(copy fence 完成)──► READY(pub=P)
     ▲                                             │                              │
     └──────── generation 变化 / 布局重建 ──────────┴──────────────────────────────┘
                                          │
                                    被本帧取用后仍保留（按 pub 值匹配）；只有新一轮
                                    published 才覆盖
```

命中判定：pub FIFO 模式看"条目 + copy fence 完成"；非 FIFO 模式看
`local_pub[s] == frames[s].published && fence_done`。两者都不是"slot 被读过"。
每个槽只有一份本地副本：同一槽的新发布会顶掉仍排队的旧条目（`g_fifo_drops`），
这就是 `pf: overflow 10–20%` 的来源——**丢的是"颜色还没取走的那一版"**，
不是生产者 overrun（P1 #5）。

### 7.3 XeFG 批次

```
IDLE ──SetEnabled(1)──► ENABLED ──Present──► BATCH(k 张待铺) ──vblank×k──► RETIRED
  ▲                        │                                                  │
  └──SetEnabled(0)─────────┘（MV gate：菜单/加载/生产者停摆）                    │
                                    GetLastPresentCount 追上 + 驻留满足 ◄────────┘
                                    ⇒ 才允许 SetNumInterpolatedFrames
```

三个约束叠在一起：`SetNumInterpolatedFrames` 必须在上一批退休后；`frameRenderTime`
必须与本批 k 匹配；`GetLastPresentCount` 计的是**调用**不是张数（所以闸门用 `+2` 松弛）。

### 7.4 cadence regime（带滞回）

```
                  want > J_eff + 0.35
   FILLABLE ─────────────────────────────► CAP_LIMITED
       ▲                                      │
       └────── want < J_eff − 0.35 ───────────┘

   slots < 1.0（输入快于面板） ⇒ INPUT_LIMITED（建议 --no-fg，运行时不切 1X）
```

`CAP_LIMITED` 会改变输出率的**含义**：日志里的 `out=` 必须报 `k·f`（可能低于 target），
不能继续声称目标已达到（`cadence_core.h:503-508`）。

### 7.5 世代（generation）

```
游戏切换 DLSS/FSR 预设 → render 尺寸变 → producer update_content()
    → streams[i].{width,height} 改写（seqlock）
    → bump generation → **memset(frames[])** → g_next_slot = 0
消费者 service_shared_layout()：释放本地副本 / pub 队列 / 视图，按同一 mapping 重建
    → cadence core 收到 s.reset=true（清估计器、相位、cap 状态；保留学习值）
```

分辨率变化**不重建映射**、不重启任何进程（v9 解耦）。

---

## 8. 不变式（违反后的现象都见过，所以这里写"后果"）

| # | 不变式 | 违反后果 | 代码/文档 |
|---|---|---|---|
| 1 | `fg_set_interp` 必须在上一批 GPU 工作退休后 | 丢一张输出（`swloss` 升高） | `frame.cpp:1480-1493` |
| 2 | `frameRenderTime` 与本帧 k 匹配 | 间距错配（k 张塞进 k−1 vblank 或留洞） | `§4.3` |
| 3 | 颜色 sink 只在"确定要拷贝"时轮转 | 复用帧读到 2–3 帧前的旧内容（画面回跳） | `frame.cpp:1580` |
| 4 | `consumed[]` 只能在该槽 GPU 退休后写 | 生产者覆盖正在读的槽 → 花屏/MV 错配 | `input_cache.cpp:58-86` |
| 5 | resetHistory 只来自真实配对失败或游戏声明 | 每次溢出都 reset → `ERR_OTHER`、画面坏 | `frame.cpp:2503-2519` |
| 6 | 门控期间不推进 cadence、不计 shortfall | `learnedMaxK` 永久降到 2（120 Hz 回不去） | `frame.cpp:2724-2743` |
| 7 | 任何"输入速率"必须 per-presentation | 估小 9% → k 掉档 → 输出掉 20% | `DYNAMIC_MF_ANALYSIS §1.3` |
| 8 | 生产者 sink 回调内不得 CPU 等待 | 直接拖慢游戏 present（它跑在游戏线程） | `COMPONENT_PRODUCER §10` |
| 9 | `frames[s].published` 必须先于 fence 信号写 | 消费者 ACK 不到正确版本 | `publish_frame()`（写 `published` → Signal） |
| 10 | 三份同名 fakerFG DLL 必须同一二进制 | 行为分裂（dxgi 与 nvngx 走不同代码） | `COMPONENT_FAKERFG §9` |
| 11 | marker 先画、scar/in-shader 修复后贴 | marker 出现在成片四角 | `passes.cpp`、`frame.cpp:1998` |
| 12 | 复制必须排在游戏队列 fence 之后 | 读到未完成的渲染（撕裂/旧帧） | `publish_frame()`（游戏队列 Signal → 复制队列 Wait） |

---


## 9. 共享内存数据字典（只列影响算法的字段）

> 协议全文见 `PROTOCOL.md`；这里给"谁写、谁读、算法含义、误读方式"。
> 结构定义：`mgpu_shared.h`（StreamShared/FrameShared/SharedControl），版本 `kSharedVersion = 17`（`mgpu_shared.h:79`）。

### 9.1 `StreamShared`（每流一份，共 5：0=colour(未放置) / 1=MV / 2=depth / 3=UI(未放置) / 4=scar）

| 字段 | 写方 | 读方 | 算法含义 | 误读 |
|---|---|---|---|---|
| `width/height` | producer | consumer | **内容**尺寸（会变） | 当成槽容量 → 行距错、花屏 |
| `slot_w/slot_h` | producer | consumer | 放置容量（固定 3840×2160） | 建视图必须用它 |
| `offset/slot_size` | producer | consumer | 64 KB 对齐的分槽 | 自己算 offset 会错 |
| `format` | producer | consumer | 源格式族（R16G16 / R32） | 与槽不一致时该流被静默跳过 |
| `seq` | producer | consumer | `(width,height)` 的 seqlock（奇=写入中） | 不重试会混用旧宽新高的尺寸 |
| `generation`（SharedControl） | producer | consumer | 内容尺寸换代 | 只"信息性"；但消费者必须重建本地副本 |

### 9.2 `FrameShared`（每槽一份）

| 字段 | 写方 | 算法含义 | 陷阱 |
|---|---|---|---|
| `frame_id` | producer | = fakerFG `seq`，配对主键 | 低 12 位才进 marker |
| `frame_qpc_ms` | producer | 游戏 **Evaluate** 时刻 | 一帧多次；**不能当帧间隔** |
| `frame_time_ms` | producer | **publish 间隔** EMA = 真帧间隔 | 决策唯一合法信源 |
| `published` | producer | 该槽这一版的 fence 值 | 消费者 ACK 与 pub FIFO 都用它 |
| `ready` | （本版本从不置 1） | 原本是"GPU 完成" | 读了只会永远 false |
| `jitter_x/y` | producer | 游戏 jitter | 我们抓的是后上采样图，jitter 已解；`--fg-jitter-zero` 可 A/B |
| `mv_scale_x/y` | producer | NGX `MV.Scale` | 决定 XeFG 怎么解释向量 |
| `near/far/vfov` | producer | 相机（FSR/XeFG 有，DLSS 常无） | 只在 `kInputFlagHasCamera` 时有效 |
| `semantics` | producer | 单位/符号/深度方向的**实测或声明** | `kSemQualityKnown` 才代表测过；缺失 ≠ 质量 0 |
| `valid_w/h` | producer | 有效子矩形 | 0 = 整张 |
| `view_to_clip` / `matrix_tick_ms` | producer | 游戏真投影（Streamline） | 形状+新鲜度检查后才用（`frame.cpp:2371-2397`） |
| `clip_to_prev_clip` / `prev_clip_to_clip` | producer | 历史投影（诊断/重投影） | 不直接喂 XeFG |
| `fg_flags` | producer | resetHistory / dilated / 3D MV / 相机运动 | 位定义在 `mgpu_shared.h`（`kInputFlag*`） |
| `present_id` / `frame_key` | producer | 配对键（与 ring `frame_key` 对应） | 仅作键，不是时间 |

### 9.3 `SharedControl` 控制字段

| 字段 | 写方 | 算法含义 | 陷阱 |
|---|---|---|---|
| `request_mask` / `consumer_request` | consumer | 要哪些流（v14 + v7） | 生产者把两者 OR；都不请求就完全空闲 |
| `active_mask` | producer | 上一帧**实际**写了哪些流 | 消费者据此判断 MV 是否可用（门控输入） |
| `stream_bytes[]` | producer | 实际传输字节 | 用来解释 `mvs` |
| `requested_percent_*` / `active_percent_*` | consumer / producer | StreamPercent 请求/回显 | 控制器读 `active_*` |
| `consumed[4]` | consumer | 每个槽读完的版本 | 只在 GPU 退休后写 |
| `producer_overrun` | producer | 无空闲槽、覆盖最旧的次数 | ≠ 消费者 `pf overflow`（两个队列） |
| `producer_waits` | producer | `slot_policy=wait` 自旋 | **恒为 0**：wait 策略从未实现 |
| `slot_policy` | producer（镜像 ini） | 0=drop / 1=wait | 生产用 drop |
| `fence_forward[]` / `fence_back[]` | producer / consumer | NT handle | 消费者必须 DuplicateHandle |
| `generation` | producer | 内容尺寸换代 | 只 bump 在真的变化时 |
| `frame_published` | producer | 最新发布的 seq | 用于"是否还有新内容"判断（slack） |

### 9.4 present-ring（`mgpu_present_ring.h`，v4）

| 字段 | 写方 | 算法含义 | 陷阱 |
|---|---|---|---|
| `write`（LONG64） | fakerFG（Present 线程） | 已提交条目数（单调） | 只读映射，不能 `InterlockedCompareExchange64`（会 0xC0000005） |
| `entries[64].{present_qpc_100ns, eval_qpc_100ns, seq, frame_key}` | fakerFG | 颜色时间 → frame_id | 两个进程各写各的计数器，seq 不可靠反推 |
| `marker[]`（128 条、每条 16×16 tile） | 历史/兼容 | 旧 scar 通道 | 现行 marker 是 32-bit code，不读 tile |

---


## 10. 观测层：每个日志字段对应哪条算法

> 完整数据字典（含"易误读"）在 `DYNAMIC_MF_ANALYSIS §2`。这里只做"字段 → 算法位置"的索引，
> 便于从现象反查代码。

| 字段/事件 | 由哪段算法产生 | 能回答 | 不能回答 |
|---|---|---|---|
| `pin=` | producer publish EMA → consumer 16 中位数（`frame.cpp:2280-2345`） | 游戏真实帧间隔 | 游戏内部 GPU 时间 |
| `arr=` | capture 线程到达间隔（`main.cpp:323-339`） | 我们的消费节拍（受 pacer 限制） | 游戏帧率 |
| `pq=` | `frame_qpc_ms` 按 frame_id 归一（`frame.cpp:2302-2332`） | Evaluate 节奏诊断 | 帧间隔 |
| `age=` | `qpc − WGC SystemRelativeTime` | 捕获排队时间 | 处理时间 |
| `qdrop` / `arr_gaps` | 颜色队列溢出/丢帧 | 输入被丢 | 输出填充 |
| `pf: served/missed/stale/overflow` | pub FIFO + 预取（`frame.cpp:1790-1869`） | 本地缓存命中率 | 生产者是否 overrun |
| `cskip=` | colour stall guard（`frame.cpp:1547-1579`） | 止损次数 | 面板是否真丢帧 |
| `show=` / `out:` | `GetLastPresentCount` 窗口增量 ÷ 时长（`frame.cpp:3135-3190`） | 交换链交付帧率（平均） | 间距是否规则 |
| `presents/vblanks` | 上屏数 ÷ (窗口 × 实测刷新) | 面板填充率 | 内容是否在跳 |
| `vphase mean/std` | `(t_present − t0) mod V`（`cadence.cpp:1021-1027`） | 提交在 vblank 周期的位置 | 显示落点（合成器还会再采样） |
| `rhythm` | `abs(k − slots)` / `abs(T/(k·V) − 1)` | 决策与输入的错配 | kV 预算下它测的是**速度**不是间距 |
| `swloss` | `Σ(expected − presents)/switches` | 每换档少上屏几张 | 含在途帧与丢帧，偏高 |
| `late` | pacer 迟到计数（`frame.cpp:63-71/97`） | 网格是否在漂（F1 修复后才是有效指标；旧实现会单调增长） | 不能区分"输入晚"与"我们晚"（配合 `wait`/`late` 绝对值看） |
| `xN=` | `GetLastPresentStatus().framesPresented`（`frame.cpp:2713-2716`） | XeFG 每批实际出几张 | 是否真的上屏（无显示端真值） |
| `breakdown:` | 各阶段 EMA（`frame_io.cpp:97-130`） | 谁吃了 25 ms 预算 | 平均值掩盖尖峰——要看 p95/max |
| `HITCH gap=` | 到达间隔 > 1.45×/2× 期望（`frame.cpp:2783-2802`） | 输入侧长间隔 | 显示端卡顿 |
| `SLOW frame:` | `total > 55 ms`（`frame.cpp:2648-2662`） | 单帧分解（decode/layout/colour/inputs/present） | 频率（有速率限制） |

### 10.1 三条"必须并排看"的曲线

1. `pin` / `arr` / `pq`：三个估计谁在撒谎；
2. `total p95` / `gpu` / `present` / `colour`：预算被谁吃掉；
3. `presents` / `vblanks` / `switches` / `holes`：交付、填充、换档、欠供四件事分开。

---

## 11. 分层定位表（现象 → 层 → 代码 → 廉价检验）

| 层 | 典型现象 | 代码位置 | 5 分钟检验 |
|---|---|---|---|
| **L0 测量** | 指标互相矛盾（show 119 但观感差） | `frame.cpp` 统计、`cadence.cpp:948` | 同时记录 `WaitForVBlank` 与 DWM `qpcVBlank` 的 QPC 差 |
| **L1 合成/扫描** | 相位推断总偏 1 vblank；`vblanks > presents` | overlay + DWM（无代码可改） | `--tearing` 跑一轮对比 presents/vblanks |
| **L2 XeFG pacer 语义** | k 恒定、预算 kV 仍有残余间距误差 | `frameRenderTime`（`frame.cpp:2550`） | 固定 k=2，扫 renderTime 0.8T–1.2T，画 presents/vblanks |
| **L3 输入结构** | 输入双峰（16.7/33.3）被平均成不存在的 f | `frame.cpp:2280`、`cadence_core.h:721` | 记录 `(submit − game_qpc) mod T` 的分布 |
| **L4 内容** | 同样 7% 误差，有的场景刺眼有的无感 | 无（未实现） | 同段输入下"最小跳丢帧" vs 随机丢帧 |
| **L5 控制器** | `optimal` 以为在闭环，实际用错信号 | `cadence_core.h:744-903` | 打印它的 out 与真实 presents/s 的差 |
| **工程 P0** | p95 超时 → 相位不稳 | presenter 无优先级（`main.cpp:671`） | 加 ABOVE_NORMAL/MMCSS 后看 p95 |
| **队列** | `pf overflow` 10–20% → 帧内现拷上关键路径 | `pub_q_push`（`frame_io.cpp:394-431`） | 把 overflow 与 `total p95`/`HITCH` 做相关 |
| **带宽** | `mvs` 尖峰 | `copy_inputs_local` | `--no-depth` / StreamPercent 200 对比 |

**使用顺序**：先 L0/L1（否则相位结论无意义）→ 再队列/工程（把 p95 压到预算下）→
最后 L2/L3/L4（算法层）。跳过前两层直接调 hold/deadband，这轮已验证无效
（`DYNAMIC_MF_ANALYSIS §3.4`）。

---


## 12. 模块地图与"文档 ≠ 代码"清单

### 12.1 fakerFG.dll（unity：`src/faker_fg/unity.cpp`，13 个实现文件）

| 文件 | 在链路中的位置 |
|---|---|
| `faker_fg.cpp` | 模式、`g_record`、`MgpuFgGetApi`、`publish_record`、sink 派发 |
| `producer_link.cpp` | `LoadLibrary(xess_mfg_producer.dll)` + `MgpuFgSetSink` |
| `nvapi_arch.cpp` | 对 NGX 伪造架构（让本卡能建 feature） |
| `proxy_streamline.cpp` | `sl::Constants` / `sl::ResourceTag` 采集（真投影、jitter、reset） |
| `backend_xefg.cpp` | XeFG 后端：init/tag/present、marker 绘制时机、present-ring 提交 |
| `mv_resample.cpp` | MV 重采样到输出分辨率（`MvResample`） |
| `passes.cpp` | marker PS、UI 合成、scar（历史） |
| `fg_swapchain_hooks.cpp` | swap chain 的 Present/ResizeBuffers 钩子 |
| `provider.cpp` | 回答 DLSS-G Create/Evaluate/Release；决定"是否接管" |
| `proxy_nvngx.cpp` / `proxy_ngx.cpp` | NGX 入口（DLSS-SR / DLSS-G） |
| `proxy_kernel.cpp` | LoadLibrary/GetProcAddress 代理（注入顺序） |
| `proxy_dxgi.cpp` | Factory/SwapChain 代理，把游戏的链换成我们的 |

### 12.2 xess_mfg_producer.dll（单文件 `producer.cpp`，约 1.4k 行）

| 区块 | 函数 |
|---|---|
| 共享块/布局 | `layout_streams` / `place_slots` / `ensure_shared` |
| 尺寸/带宽 | `read_percent` / `target_dim` / `compute_target` / `update_content` |
| 缩放 | `ensure_scale_objects` / `ensure_scale_target` / `scale_dispatch` |
| 槽位 | `update_credit` / `slot_free` / `claim_slot` |
| 复制 | `copy_stream` / `copy_begin_frame` |
| 发布 | `on_record` → `publish_frame`（元数据 + fence） |
| 导出 | `MgpuProducerStart/Present/Shutdown/GetApi` |

### 12.3 consumer（unity：`src/xess_mfg_consumer/unity.cpp`，16 个模块，依赖顺序固定）

| # | 模块 | 职责 |
|---|---|---|
| 1 | `platform.cpp` | 日志、退出、控制台 |
| 2 | `state_overlay.cpp` | 所有跨模块静态状态 + OSD 状态 |
| 3 | `devices_capture_fg.cpp` | 适配器/设备、WGC、XeFG init、fence、`fg_set_interp` |
| 4 | `cadence.cpp` | 速率估计、vblank、`cad_core_configure`、pacer、旧 tick/next_slots 路径 |
| 5 | `shaders.cpp` | convert/prepare HLSL（运行时编译） |
| 6 | `frame_io.cpp` | 共享视图、槽、retire、copy 队列、pub 队列、sink、统计 |
| 7 | `osd.cpp` | 屏幕字/图、`gpu_throttle` |
| 8 | `warp_upsample.cpp` | `--upsample` 联合重建 |
| 9 | `stream_auto.cpp` | StreamPercent 控制器 |
| 10 | `convert_warp.cpp` | 转换常量、marker 修复参数、`--fg-warp` |
| 11 | `pair_dump.cpp` | `qpc_ms`、`pair_slot_for_wgc`、`--dump` |
| 12 | `dump_of.cpp` | 光流 dump/readback |
| 13 | `input_cache.cpp` | 本地缓存、ACK、prefetch、copy-on-publication |
| 14 | `frame.cpp` | `run_frame_ex` + 报告 |
| 15 | `probe_mfg.cpp` | `--fg-probe` |
| 16 | `main.cpp` | 参数、初始化、事件循环、热键 |

### 12.4 另一条颜色路径：`--fg-colour shared`（**当前不可用**，已加保护）

设计意图（代码仍保留分支）：

```
游戏进程：fakerFG 把颜色（PQ 编码）写进 streams[0]（跨卡 copy，HDR10 R10G10B10A2）
consumer：不启动 WGC，g_fg_colour_shared = true
          marker 配对关闭（frame.cpp:1179），由"发布"驱动帧循环
          convert 用 mode 3（不再做 scRGB→PQ）
```

**审计结论（2026-11-15）**：当前 producer 的 `layout_streams()` 只放置
stream 1/2/4（MV、depth、scar；见 `layout_streams()`），`on_record()` 也只复制 MV/depth；
**stream 0（colour）与 stream 3（UI）既不放置也不发布**。因此：

- `--fg-colour shared` 会读到未放置的视图 → 黑屏；
- UI 流的 tag 条件（`frame.cpp:2178`：`streams[3].width >= 64`）永远不成立 → UI 从未参与。

处理：解析到该开关时打印告警并回退 WGC（`main.cpp:949-960`），不再静默黑屏。
若将来要真正实现，需要 producer 增加 stream 0 的复制与 PQ 转换（4K 约 +33.2 MB/帧）。

### 12.5 文档 ≠ 代码（按"会影响判断"排序）

| # | 文档说法 | 代码事实 | 影响 |
|---|---|---|---|
| 1 | v17 有 scar 流，"画 marker 前把 16×16 复制到同槽" | producer **布局了** scar 槽（`layout_streams()` 的 scar 槽），但 `publish_frame` 写 `marker_valid=0` 且不拷；修复由 convert PS 用内侧邻居完成（`frame.cpp:1998`） | 不要按 scar 流排查花屏；看 `conv_consts[11]`/`marker_drawn_recently()` |
| 2 | `FrameShared.ready` = "CPU 确认 GPU 复制完成" | **从未置 1**（跨卡 fence 不可读，轮询会拖慢游戏） | 看到 ready=0 是正常的，不是 bug |
| 3 | `COMPONENT_CONSUMER §3` 的 17 步：layout→retire→取色→配对… | 代码先 marker 解码/配对，再 layout；cadence 判决在 `gpu_throttle` 之前（源文档已加"以 §6 为准"标注） | 用本文 §6 的顺序 |
| 4 | 所有 bat 用 `--pool 4` | 代码默认 `--pool 2`；4 多约两帧捕获延迟 | P0 待改 |
| 5 | "cadence 有三套取 k 逻辑" | 默认**不启用** cadence core（固定 `--interp N`）；`--interp-cadence` 才走 core（默认 policy=optimal）；epoch / tick / next_slots 是另外的显式开关路径 | 先看日志有没有 `cadence core:` 行，再决定用哪套指标 |
| 6 | `GetLastPresentCount` 能判断排空 | 它计 **Present 调用**，不是生成张数；闸门用 `+2` 松弛，实际恒真 | 换档可能切在批次中途（P0 #4） |
| 7 | `--dyn-mfg` 与 `--interp-cadence` 可叠加 | 互斥：`if (g_dyn_mfg && !g_interp_cadence)`（`frame.cpp:1321`） | 同时开会静默只跑一个 |
| 8 | `fill + k·V` 能得到"完美间距 + 不丢帧" | 实测证伪（S+D 那格是空的，`DYNAMIC_MF_ANALYSIS §6.7`） | 不要把它当默认 |
| 9 | `COMPONENT_PRODUCER §6`：`150%` = 放大（MV 质量实验） | `target_dim = v·100/pct`，**>100 是降采样**（200 → 各维减半，字节 1/4）；没有放大分支（源文档已更正） | 读 `active_percent_*` 时别把 200 当成"更多数据" |
| 10 | producer 注释："消费者按 published 选最新槽，所以覆盖最旧" | 消费者按 **marker/ring** 配对（`pair_slot_for_wgc`）；"覆盖最旧"只是启发式，且只在信用窗口关闭/全部未 ACK 时发生（注释已更正） | 不要据此推断配对行为 |
| 11 | frame_io 注释："pub 队列上限 `kMaxSlots-1`，溢出丢最旧" | 上限是 `kMaxSlots`；真正的溢出是"**同槽的新发布**顶掉仍排队的旧条目"（`pub_q_push`，注释已更正） | `pf overflow` 是按槽覆盖，不是均匀丢帧 |
| 12 | `--fg-colour shared` / UI 流的文档描述像可用功能 | stream 0（colour）与 3（UI）**从未被放置或发布**；该模式会黑屏（§12.4） | 已改为告警并回退 WGC；UI 从未参与 |
| 13 | `SYSTEM_MAP §3.3` / `COMPONENT_PRODUCER §8`：`slot_policy=wait` 会自旋并计入 `producer_waits` | 生产者固定 `slot_policy=0`（drop），**没有 wait 分支**，`producer_waits` 从不递增 | 不要把 `producer_waits=0` 当"从没等过"的证据 |

### 12.6 代码级"落后设计"清单（审计结论与处理状态）

> 本轮只做**不改变正常路径**的处理：文档纠正、错误注释更正、失效开关的显式回退。
> 行为级改动需要 Windows 构建 + 实机验收，按 `HANDOVER §5` 的优先级单独做。

| # | 位置 | 问题 | 状态 |
|---|---|---|---|
| 1 | `main.cpp`（presenter 主线程） | 只有 capture 线程 `ABOVE_NORMAL`；presenter 是 NORMAL、无 MMCSS | **未改**（3 行；预期降低 p95 超时） |
| 2 | bat 脚本 | 覆盖成 `--pool 4`（代码默认 2）→ 多约 2 帧捕获延迟 | **未改**（配置层） |
| 3 | `producer.cpp` 信用窗口注释 / `frame_io.cpp` pub 队列注释 | 两处注释与真实行为不符（§12.5 #10/#11） | **已改注释** |
| 4 | `mgpu_shared.h` `kStreamScar` | 注释把保留槽写成"正在使用" | **已改注释** |
| 5 | `main.cpp --fg-colour shared` | 未实现的路径会黑屏 | **已改**：告警 + 回退 WGC |
| 6 | `frame.cpp` UI tag | `streams[3]` 永不放置 → 条件恒假（死代码） | **未改**（保留给将来的 UI 流；文档已标） |
| 7 | `GetLastPresentCount + 2` 闸门 | 计调用而非张数 → 排空判断恒真 | **未改**（需用 `appliedK` 累加 `expected_out`，P0 #4） |
| 8 | 速率估计串联 | producer EMA → 16 中位数 → 16 中位数 + EMA = 13–16 帧滞后 | **未改**（参数层：中位数 16→8、smooth 8→4，需 table 8 验证） |
| 9 | `FrameShared.ready` | 死字段（从不置 1） | **保留**（协议兼容；文档已标） |
| 10 | `--fg-colour shared` 的真正实现 | 需要 producer 增加 stream 0 复制 + PQ（+33.2 MB/帧） | **未做**（产品决策，不是 bug） |
| 11 | `slot_policy` / `producer_waits` | 协议字段保留，wait 策略从未实现（`producer_waits` 恒 0） | **保留字段**（文档/注释已标） |
| 12 | `cadence.cpp vblank_init` | `DwmGetCompositionTimingInfo(nullptr)` 取主显示器周期；overlay 在副屏时会用错网格 | **未改**（多屏场景需按 `g_overlay_monitor` 取 DWM 时钟或强制走 WaitForVBlank 回退） |
| 13 | `input_cache.cpp ack_previous_slot` | 旧 ACK 把**所有槽**的当前 `published` 写入 `consumed[]`（over-ACK），并可能在拷贝队列仍在飞行时放行覆盖 | **已改**：只 ACK 本地副本 fence 已退休的精确版本（§PAIRING_100 2.4） |
| 14 | `frame.cpp` marker 解码 | 解码滞后一帧 + 硬编码 `+1`；WGC 丢帧/DWM 重复帧会静默错配 | **已改**：当前帧解码、id+ms 双条件、验证失败走 1X（§PAIRING_100 2.1-2.3） |
| 15 | `mgpu_shared.h kMaxSlots` | 4 槽小于颜色 p99 滞后，颜色到达时发布常已被覆盖 | **已改**：8 槽、协议 v18（容量见 §2.5） |

**已核对并更正的源文档**：`COMPONENT_PRODUCER §6`（StreamPercent 方向 + 范围/回退语义）、
`COMPONENT_CONSUMER §3`（步骤顺序标注）、`COMPONENT_CONSUMER §5`（默认输入路径是
copy-on-publication，单槽预取被短路）。本文 §12.5 是统一登记处，后续引用以本文与代码为准。

---


## 13. 验证工具与"结论 → 数字"的对应


### 13.1 纯计算仿真（控制器）

```bash
bash tests/cadence_core/run.sh        # g++ 编译 sim.cpp + 24 项检查
```

| 表 | 覆盖的算法 | 回答的问题 |
|---|---|---|
| table 1 | J=4、每次换档丢 1 张 | 各 policy 的稳定输出率 |
| table 2 | J = 2..8 扫描 | 上限变大值不值 |
| table 3 | 切换成本敏感性 | 换档成本对策略排序的影响 |
| table 4 | `--fg-out-hz 60` | 目标低于面板时 |
| table 5 / 5b | fill 死区 / 驻留扫描 | deadband 与 hold 的交换 |
| table 5c | locked 的 k 规则 | 35 fps 该 3x 还是 4x |
| table 7 | optimal 的 (m,k,paced) | 默认策略的选择 |
| table 8 | 43→30 fps 阶跃 | **滞后帧数**（optimal 13 / fill 9 / locked 16） |

### 13.2 日志分析

```bash
python3 scripts/cadence_report.py <log...>
```

内置字段：`show / presents / rhythm / vphase / k 分布 / switches / swloss / qdrop /
cskip / HITCH / resets / pf`。任何一次实验先用它出一张表，再决定要不要看原始行。

其它：`scripts/producer_report.py`（发布/槽位/带宽）、`scripts/logstats.py`、
`scripts/verify_pipeline.py`（链路自检）、`scripts/fg_ratio.py`（输出比）。

### 13.3 每类改动的验收口径

| 改动层 | 必看指标 | 通过线 |
|---|---|---|
| 速率估计 | `pin` 与 `pq` 并排稳定性；k 不因尖峰掉档 | 阶跃滞后 ≤ table 8 的一半 |
| 决策 | 整除点 `spacing err`；非整除点 `qdrop` 或 `holes` 只出现一种 | 整除 ≤1%，非整除符合所选代价 |
| pacer | `late` 不再单调、`vphase std` | std < 0.5 ms |
| 输入队列 | `pf overflow` 与 `total p95` 的相关性 | overflow < 3%，p95 < 预算 − 1 ms |
| 颜色 | `cskip`、`SLOW frame colour=` | cskip 只在 GPU 落后时出现 |
| 工程 | `gpu`/`present` p95、presenter 优先级 | p95 下降且 `switches` 不变差 |

---

## 14. 一页速查（贴在显示器旁边的那种）

```
⓪ 先看日志有没有 `cadence core:`：没有 = 固定倍率（--interp），控制器与 pacer 都不在。
① 先问：f 与 R 整不整？ 整除 → 锁 k，预算 k·V，结束。
② 非整除 → 只有三种代价：间距 / 丢输入 / 空洞。选一个，别让它们随机混合。
③ 任何"间距"结论先确认：V 是 DWM 实测，T 是 frame_time_ms（不是 frame_qpc_ms）。
④ 颜色与 MV 对不上 → 看 marker seq12 与 frames[].frame_id，不要看 ring 的 seq。
⑤ 卡 200 ms → 看 colour（sink 拷贝），不是 XeFG。
⑥ 输出少一张 → 看 SetNumInterpolatedFrames 是否切在批次中途（GetLastPresentCount 闸门）。
⑦ 菜单/加载花屏 → MV gate；门控期间不要计入 shortfall。
⑧ pf overflow 高 → 同槽新发布顶掉旧条目；不是 producer_overrun。
⑨ 改控制器之前，先把 total p95 压到预算以下；否则相位结论都不可信。
⑩ 所有"已实现"以代码为准；本文 §12.5 列了 15 处文档与代码不一致。
```

---

## 15. 维护约定（与 `HANDOVER_2026-11-15.md §6` 一致）

1. **结论只写进三类**：理论/方案进 `DYNAMIC_MF_ANALYSIS.md`，顶层结构/数据流/算法进
   `TOTAL_ARCHITECTURE_AND_DATAFLOW.md`，组件细节与不变式进 `COMPONENT_*`；
   过程与原始数据留 `PROGRESS_2026-11-15.md`。
2. 任何"已实现"必须附 **`file:line` + 验证方式**（仿真表号或日志字段）。
3. 新增/修改一条算法时，同步改三处：本文对应小节、§6 的 17 步表（若顺序变了）、
   §10 的字段索引（若新增/改名了日志字段）。
4. 被推翻的结论**不删**，标 `已证伪 + 依据`；本文 §12.5 是"文档 ≠ 代码"的登记处。
5. 新文档在 `docs/README.md` 建索引，并在 `HANDOVER` 第 1/2/3 节归类。

---

### 附：本文核对过的代码清单（2026-11-15）

```
src/mgpu_shared.h                    src/mgpu_present_ring.h
src/mgpu_producer_api.h              src/cadence_core.h
src/faker_fg/{faker_fg.cpp, passes.cpp, backend_xefg.cpp, unity.cpp}
src/xess_mfg_producer/producer.cpp
src/xess_mfg_consumer/unity.cpp
src/xess_mfg_consumer/modules/{frame.cpp, cadence.cpp, input_cache.cpp,
    frame_io.cpp, pair_dump.cpp, devices_capture_fg.cpp, osd.cpp,
    main.cpp, stream_auto.cpp}
tests/cadence_core/{run.sh, sim.cpp}
```

