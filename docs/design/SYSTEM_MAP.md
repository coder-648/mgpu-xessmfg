# 系统地图：三个组件的职责、协议与时序

> **v22（2026-11-15）**：协议 v22 把颜色变成 producer 的 stream 0，与 MV/depth 同一槽、
> 同一 fence 发布；consumer 不再抓屏，配对就是槽号。本文 §3/§4 中 WGC、marker、
> present-ring 的描述是 v21 及以前的结构，当前结构见
> `docs/design/COLOUR_TRANSPORT_V22.md`。

最后更新：2026-11-15。本文是 `DYNAMIC_MF_ANALYSIS.md` 的"结构底座"——
那份文档讲**算法与理论**，本文讲**系统由什么组成、数据怎么流、谁在等谁**。

---

## 1. 进程与适配器拓扑

```
┌─────────────────── 游戏进程（NVIDIA RTX 3080 Ti）───────────────────┐
│  Cyberpunk2077.exe                                                  │
│    ├─ dxgi.dll        = fakerFG.dll （DXGI 代理：接管 swap chain）   │
│    ├─ nvngx_dlssg.dll = fakerFG.dll （NGX 代理：回答 DLSS-G）        │
│    └─ xess_mfg_producer.dll（transport：MV/depth 跨卡复制 + 发布）   │
└──────────────────────────────┬──────────────────────────────────────┘
                               │ 共享内存 Local\mgpu_mailbox_v1
                               │ 双方各自 OpenExistingHeapFromFileMapping
┌──────────────────────────────┴──────────────────────────────────────┐
│  xess_mfg_consumer.exe（AMD RX 6750 GRE，驱动 overlay 显示器）       │
│    ├─ WGC 捕获颜色                                                  │
│    ├─ 配对：present-ring seq / marker / producer 时间戳              │
│    ├─ 本地拷贝 MV/depth、convert 到 HDR10、tag 给 libxess_fg         │
│    └─ Present（XeFG 代理 swap chain）→ DWM → 面板                    │
└─────────────────────────────────────────────────────────────────────┘
```

**为什么三个组件**：D3D12 钩子是进程级的，在游戏进程里创建第二厂商设备会死锁
（`docs/findings/milestones/M3.2_RESULTS.md`）。拆开还保证 AMD 侧崩溃不带走游戏。

---

## 2. 职责边界

| 组件 | 只负责 | 不负责 |
|---|---|---|
| **fakerFG.dll** | 决定"谁生成帧"、回答 DLSS-G、Harvest 出 `MgpuFgFrame`、画 marker、Present 时通知 transport | 不复制纹理、不写共享内存、不控制 pacing |
| **xess_mfg_producer.dll** | MV/depth 跨卡复制、StreamPercent 缩放、发布 fence、信用窗口 | 不决定 FG、不改时序 |
| **xess_mfg_consumer.exe** | 捕获、配对、转换、tag、pacing、cadence 决策、OSD/日志 | 不参与游戏渲染 |

---

## 3. 共享内存协议（`src/mgpu_shared.h`，v17）

### 3.1 布局

```
offset 0      SharedControl（magic/version/slots/streams/fences/frames/request/generation）
offset 64KB   kDataStart：纹理槽区，容量按 kStreamMaxW×kStreamMaxH = 3840×2160 预置
```

- 槽位容量固定；分辨率变化只改 `StreamShared.width/height` + bump `generation`（v9 解耦）。

### 3.2 关键字段

| 字段 | 写方 | 含义 |
|---|---|---|
| `streams[i].{width,height,format,offset,slot_w,slot_h,seq}` | producer | 内容尺寸（唯一会变）、槽容量、seqlock |
| `frames[s].frame_id` | producer | = fakerFG 的 `f.seq`（唯一帧号） |
| `frames[s].frame_qpc_ms` | producer | 游戏 present 的 QPC（配对 / 相位） |
| `frames[s].frame_time_ms` | producer | **每呈现帧一次**的间隔 EMA（速率源） |
| `frames[s].published` | producer | 该槽最近一次 forward-fence 值（消费者据此 ACK） |
| `frames[s].ready` | producer | CPU 确认该槽 GPU 复制完成（跨卡 fence 消费者读不出） |
| `frames[s].{jitter,render_*,mv_scale,near/far/vfov}` | producer | 帧常量与相机 |
| `frames[s].{semantics,valid_*}` | producer | 线上探测的 MV 单位/符号/深度方向 |
| `frames[s].view_to_clip` / `clip_to_prev_clip` | producer | Streamline 真投影（v12/v13） |
| `frames[s].fg_flags` | producer | resetHistory / dilated / 相机运动… |
| `consumed[s]` | consumer | "槽 s 的第 N 次发布我读完了" |
| `fence_forward[s]` / `fence_back[s]` | producer / consumer | NT handle（消费者需 DuplicateHandle） |
| `request_mask` / `active_mask` | consumer / producer | 需要哪些流 |
| `requested_percent_*` / `active_percent_*` | consumer / producer | 每流 StreamPercent（v15） |
| `generation` | producer | 内容尺寸换代 |

### 3.3 槽位复用（信用窗口）

复用槽 `k` 前必须 `consumed[k] >= 上次发布给 k 的值`；否则覆盖"发布值最小"的槽并计入
`producer_overrun`。`slot_policy` / `producer_waits` 是**保留字段**（生产者固定写 0，
wait 策略从未实现），不要用它们判断阻塞。

### 3.4 present-ring（配对桥）

`src/mgpu_present_ring.h`：fakerFG 每次 present 写一条 `(qpc, seq)`；
消费者用 WGC 的 `SystemRelativeTime` 查环得到 seq，即"颜色 ↔ MV/depth"精确配对依据。

---

## 4. 一帧的完整时序（bridge 模式）

```
 游戏渲染
   ├─(1) 游戏调用 DLSS-G Evaluate ─► fakerFG provider 回答 + harvest
   │        record：颜色/深度/MV 资源、jitter、相机、fg_flags、seq
   │        （只在本次 Evaluate 内有效；ONLY_NOW 资源必须当场用）
   │
   ├─(2) 游戏 Present ─► fakerFG 的 DXGI 代理接管
   │        (a) 画 4 角 marker（把 seq 编进像素）
   │        (b) 写 present-ring 条目
   │        (c) 通过 MgpuFgFrameSink 同步交给 transport（游戏线程上）
   │
   ├─(3) producer：game queue 信号 g_frame_fence → 自己的 copy/DMA 队列等待
   │        → 按 StreamPercent 缩放复制 MV/depth 到槽 s
   │        → 信号 fence_forward[s]，写 published/frame_id/qpc/jitter/camera/semantics
   │        → CPU 观察到复制完成后置 ready
   │
   ▼  —— 以上都在游戏进程，AMD 侧未动 ——
   │
   ├─(4) consumer 捕获：WGC 交颜色（通常晚一帧）+ SystemRelativeTime
   │        配对优先级：present-ring seq → marker → 时间戳邻近
   │
   ├─(5) consumer 输入：预拷命中用本地缓存，否则当场拷（占关键路径）
   │        跨卡 fence 不可读 → 用 published + 显式 expect
   │
   ├─(6) consumer 色彩：WGC 纹理 → 共享 sink（3 张轮转 + keyed mutex）
   │        → D3D12 convert（scRGB→BT.2020 PQ）
   │
   ├─(7) consumer cadence：算 k / 预算 → fg_set_interp(k-1)
   │        → TagFrameConstants(frameRenderTime) + TagFrameResource(MV/depth)
   │        → 可选 pacer 等到 vblank 槽
   │
   ├─(8) Present(0) ─► XeFG 生成 k 张、按 vblank 铺开、上屏
   │
   └─(9) consumer 统计：presents/vblanks、vphase（DWM）、xN、switches、HITCH…
```

### 4.1 必须保持的顺序不变式

1. `fg_set_interp` 必须在上一批 GPU 工作退休之后（`gpu_throttle` / `ack_previous_slot` 之后）；
2. `frameRenderTime` 必须与本帧 k 匹配（预算规则见 `DYNAMIC_MF_ANALYSIS.md` §3）；
3. 颜色 sink 只在"确定要拷贝"时轮转（否则复用帧读到旧 sink）；
4. `consumed[]` 只能在该槽 GPU 工作退休后写；
5. resetHistory 只允许来自真实配对失败或游戏声明，不允许来自队列溢出。

---

## 5. 版本演进（影响理解的关键几版）

| 版本 | 变化 | 为什么重要 |
|---|---|---|
| v5/v9 | 槽位预置 + generation 解耦 | 分辨率变化不重建映射，消费者无需重启 |
| v7 | `consumer_request` | 可关掉 MV 流省带宽 |
| v10 | `consumed[]` + `published` + seqlock | 真正的队列语义，可算真实深度 |
| v11/v12/v13 | 语义探测 / 真投影 / FG 声明 | 消费者不再"猜" MV 单位与投影 |
| v14/v15 | 流开关板 + 每流 StreamPercent | 在线调带宽，不改 ini |

---

## 6. 相关文档

| 主题 | 文档 |
|---|---|
| 算法与理论（不可能三角、标量函数、代码层缺陷） | `docs/design/DYNAMIC_MF_ANALYSIS.md` |
| fakerFG 内部 | `docs/design/COMPONENT_FAKERFG.md` |
| producer 内部 | `docs/design/COMPONENT_PRODUCER.md` |
| consumer 内部 | `docs/design/COMPONENT_CONSUMER.md` |
| 输入语义 | `docs/findings/INPUT_SEMANTICS.md` |
| colour 停顿与根治 | `docs/design/COLOUR_STALL.md` |
| consumer 审计（12 个已修 bug） | `docs/design/CONSUMER_AUDIT.md` |

ring 由两个进程各自写自己的条目，所以 seq 不能靠低 12 位反推（代码有实测注释）。
