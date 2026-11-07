# 架构

## 1. 一句话

游戏照常用自己的卡渲染 + 超分（DLSS / FSR / XeSS），ReShade add-on 把
**运动矢量 / 深度 / 相机** 写进共享显存，另一个进程 `bridge.exe` 在**另一块卡**上
用 **XeSS-FG** 插帧，再通过覆盖窗口上屏。

```
┌───────────────────────── 游戏进程 ─────────────────────────┐
│  游戏  ──►  超分（DLSS / FSR / XeSS）                       │
│              │                                             │
│              ▼                                             │
│  ReShade 6.8（dxgi.dll）                                   │
│      └─ game_capture.addon64                              │
│           · 钩超分 API（观察者，不改参数、不替换实现）        │
│           · 每个 fenced 环写入 MV / depth 到跨卡堆            │
│           · 写 header（协议 v10，见 PROTOCOL.md）             │
└───────────────────────┬───────────────────────────────────┘
                        │  Local\mgpu_mailbox_v1
                        │  命名文件映射 = 跨卡堆（OpenExistingHeapFromFileMapping）
                        │  + 循环 fence（NT 句柄，按槽位）
                        ▼
┌───────────────────────── bridge.exe（另一块卡）─────────────┐
│  WGC 抓屏 ──► 颜色                                          │
│  共享槽     ──► MV / 深度（或自己算光流）                     │
│        ▼                                                   │
│  libxess_fg.dll（XeFG）+ libxell.dll（XeLL，低延迟）          │
│        ▼                                                   │
│  DirectComposition 覆盖窗口 ──► 屏幕上屏                     │
└────────────────────────────────────────────────────────────┘
                        ▲
                        │ 参数 / 热键 / OSD
                 mgpu_ui.exe（可选前端）
```

## 2. 为什么必须是两个进程

ReShade 的 D3D12 钩子是**进程全局**的：在游戏进程里再创建一块**另一家厂商**的
设备会死锁（实测，见 `docs/findings/milestones/M3.2_RESULTS.md`）。拆成两个进程
还有两个副作用，都是好事：

- A 卡侧的崩溃永远不会带走游戏；
- 帧生成卡可以是任意一块（A 卡 + N 卡混插也能跑），见
  `docs/design/VENDOR_COMPAT_PLAN.md`。

代价是**所有数据必须跨进程、跨适配器传递**，而这正好是整个项目最贵的部分。

## 3. 三个模块

| 模块 | 进程 | 职责 | 关键文件 |
|---|---|---|---|
| `game_capture.addon64` | 游戏 | 钩超分 API → 产出 MV/depth/相机 → 写共享槽 + fence | `src/game_capture/game_capture.cpp`（宿主）、`inputs.h`（源抽象）、`in_ngx/in_ffx/in_xess.cpp`（三个输入源）、`shared_layout.h`（槽位放置） |
| `bridge.exe` | 独立 | 消费槽位 + WGC 颜色 → XeFG 插帧 → 覆盖窗口上屏 | `src/bridge/bridge.cpp`（~6.9k 行，含 OSD、XeFG 接线、诊断工具）、`osd_panel.h` |
| `mgpu_ui.exe` | 独立 | 选窗口 / 参数面板 / 热键 / 拉起并监控 `bridge.exe` | `src/mgpu_ui/mgpu_ui.cpp`（Win32 + GDI，无第三方库） |

共享的内存布局与常量在同名头文件里：`src/mgpu_shared.h`（两端共用，**不含**
`windows.h`），协议版本 `kSharedVersion = 10`。

## 4. 一帧的生命周期（v10）

1. 游戏调用超分 API。add-on 的钩子读取参数（**只读**），拿到 MV / depth 指针、
   jitter、渲染尺寸、相机；
2. add-on 把 MV/depth 拷进**预放置**的跨卡槽位（按 3840×2160 容量一次性放好，
   实际内容是左上角 `width × height` 的矩形），写 `FrameShared`（含
   `published` = 本次发布的 fence 值）；
3. add-on 向自己的前向 fence 发信号（该槽位）；
4. `bridge.exe` 等到 fence → 从槽位拷出 MV/depth → 回 ACK
   （`SharedControl.consumed[k] = published`）→ 同时用 WGC 拿到颜色；
5. bridge 调 XeFG 生成中间帧，XeLL 负责低延迟节拍，DirectComposition 上屏；
6. 生产端复用槽位前必须看到 `consumed[k] >= 它上次发布的值`（信用窗口）。

**没人等别人。** 生产端永远不阻塞消费端，消费端永远不要求生产端怎么做 ——
这是 v9 解耦 + v10 消息队列语义的全部要点。

## 5. 分辨率变化

槽位**从不移动**。游戏切换超分预设（渲染分辨率变了）时：

- 生产端改写 `StreamShared.width/height`（用 `seq` seqlock 保证成对一致），
  并 `generation++`；
- 消费端下一次读到时按新矩形拷贝。

没有握手、没有重新放槽、没有停顿 —— 这就是"尺寸变化不重启游戏"的实现。

## 6. 关键设计取舍

| 取舍 | 为什么 |
|---|---|
| 颜色走 **WGC 抓屏**，不走跨卡拷贝 | 见 §6.1：那一跳 DWM 本来就在做，抓合成结果等于白拿；`Color=1` 是在它之上再叠一份 33–56 MB/帧，实测把渲染卡从 **80% 推到 95%** |
| 槽位**一次性按最大容量**放好 | 重建 slot 需要 magic/ack 握手，会在渲染线程上产生有界停顿（v8 的教训） |
| 消费端**不读**跨卡 fence | 跨适配器 fence 在消费节点上返回 `UINT64_MAX`，读它等于空转。同步只能靠 header 里发布的值 |
| add-on 是**观察者** | 不改游戏参数、不替换超分实现 ⇒ 兼容性好、崩了也不影响游戏；代价是拿不到 NGX 的相机 |

### 6.1 为什么颜色走 WGC 而不是后台缓冲拷贝（实测记录）

这是 M4.2a 实验的结论，原始记录在 `tests/wgc_probe/wgc_probe.cpp` 头部与
`src/game_capture/game_capture.cpp` 的颜色拷贝分支里。

**当时的机器**：游戏窗口显示在**帧生成卡（AMD）接的 5K 屏**上，游戏渲染在另一块卡上。

推论链：

1. 画面要出现在 AMD 卡的屏上，**DWM 每帧本来就得跨总线搬一次**（这是上屏的必要条件）；
2. **WGC 抓的是"合成后的窗口"** ⇒ 它复用的正是这一跳，我们等于白拿一份颜色；
3. `Color=1`（从游戏后台缓冲 CopyResource 进跨卡共享堆）则是在这一跳**之上**，
   再从**游戏自己的 command list** 追加一份 **5K = 56.25 MiB/帧**（4K = 33.2 MB/帧）的 PCIe 写。

**实测代价**：游戏 GPU 占用 **~80% → ~95%**。

注意这是**占用率（occupancy）**，不是帧时间：含义是余量从 20% 掉到 5%，
峰值场景会直接掉帧，而不是线性慢 15%。

另外三条附带的坑（同样在代码里踩过）：

| 坑 | 说明 |
|---|---|
| 格式必须精确匹配 | `copy_compatible()` 要求后台缓冲就是 `R10G10B10A2`，SDR 游戏（BGRA8）直接 `[color] skipped` |
| 失败会连坐 | 颜色拷贝失败 → `ok = false` → `g_copied_this_frame` 为假 → **整帧不发布**，MV/深度也一起没了（深度分支专门做了"只警告不阻止发布"，颜色分支没有） |
| 时机不明确 | 拷贝点在 `on_begin_effects`，此刻后台缓冲里是否已有本帧 UI 取决于引擎；`--fg-ui none` 的前提可能被打破 |

> 补充：**当前的 `bridge.exe` 根本不读颜色流**。它只 tag `MOTION_VECTOR` 和 `DEPTH`
> 给 XeFG，`streams[0]` 只被读宽高当作画布尺寸。所以现在开 `Color=1` 是纯成本、
> 零收益；要真正利用它必须先在消费端加一条"从共享槽读颜色"的通路。
> 相关取舍见 `docs/findings/ARCHIVED_EXPERIMENTS.md` 与 `ROADMAP.md`。

## 7. 想看更多

- 协议字段与版本演进：`docs/design/PROTOCOL.md`
- 三个输入源的能力差异：`docs/design/INPUT_SOURCES.md`
- 为什么长这样（历史）：`docs/findings/milestones/M3.2_RESULTS.md`、
  `docs/findings/HANDOFF.md`
