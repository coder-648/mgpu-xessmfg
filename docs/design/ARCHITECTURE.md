# 架构（当前链路，2026-11-14）

> 本文描述**现在运行的代码**。历史版本（ReShade add-on + `bridge.exe`）已归档到
> [`archive/ARCHITECTURE_reshade_addon.md`](archive/ARCHITECTURE_reshade_addon.md)。
>
> 更细的专题：
> - fakerFG 链路与开关：[`XEFG_CHAIN_STANDARD.md`](XEFG_CHAIN_STANDARD.md)
> - producer：[`XESS_MFG_PRODUCER.md`](XESS_MFG_PRODUCER.md)
> - consumer：[`CONSUMER_ARCH.md`](CONSUMER_ARCH.md)
> - 共享内存协议：[`PROTOCOL.md`](PROTOCOL.md)
> - 全仓状态与优化清单：[`../PROJECT_STATUS_2026-11-14.md`](../PROJECT_STATUS_2026-11-14.md)

## 1. 一句话

游戏用自己的卡渲染 + 超分；游戏进程里的 `fakerFG.dll` 拥有整条 FG 管线，
把这一帧的完整输入整理成记录；`xess_mfg_producer.dll` 只负责把 MV/深度搬过 PCIe；
消费卡上的 `xess_mfg_consumer.exe` 用 WGC 取颜色、用 marker 精确配对、用 XeSS-FG 插帧，
再通过 DirectComposition 覆盖窗口上屏。

```text
┌────────────── 渲染卡（游戏进程） ───────────────────────────────────────┐
│ 游戏                                                                   │
│  └─ dxgi.dll = fakerFG.dll（同一份二进制，另有 nvngx_dlssg.dll 副本）  │
│       ├─ DXGI / NGX / Streamline / 加载器代理                          │
│       ├─ DLSS-G provider：回答契约 + harvest 参数块                    │
│       ├─ XeFG backend：建链、tag、Present；local 模式自己出帧          │
│       ├─ marker：Present 前在链缓冲四角画 32-bit 短雪花码              │
│       └─ LoadLibrary("xess_mfg_producer.dll")                          │
│              └─ sink：降采样 + barrier + CopyTextureRegion             │
│                 第二队列：等游戏队列提交后拷贝（DMA/DIRECT）            │
├──────────── Local\mgpu_mailbox_v1（协议 v17，4 槽） ────────────────────┤
│ MV / depth 纹理 + FrameShared 元数据 + fwd/back fence + consumed[]      │
└────────────── 消费卡（独立进程） ───────────────────────────────────────┘
   xess_mfg_consumer.exe
     ├─ WGC 抓游戏窗口（颜色只走 WGC，不跨卡）
     ├─ 颜色 → 双缓冲共享 sink → D3D12 转换（scRGB → BT.2020 PQ）
     ├─ 解码四角 marker → 用 frame_id 精确配对 publication 槽
     ├─ 输入预拷 / --upsample / --stream-auto
     └─ libxess_fg 插帧 → DirectComposition 覆盖窗口 → 显示器
          ↑ 启动/参数/热键/状态
       mgpu_ui.exe
```

## 2. 为什么是“游戏内 + 独立进程”两段

| 决策 | 原因 |
|---|---|
| FG 管线必须放进游戏进程 | DLSS-G 契约、Streamline 的输入 tag、swapchain 的 Present 都发生在游戏进程；只有成为 provider 才能“不生成但画面正确” |
| 传输与插帧放到独立进程 | 在游戏进程里创建另一家厂商的 D3D12 设备会死锁（历史实测）；独立进程也让副卡崩溃不带走游戏 |
| 颜色走 WGC，不走跨卡拷贝 | DWM 本来就要把画面搬到输出卡；WGC 抓的是合成后的窗口，等于白拿一份颜色。跨卡拷 4K 颜色会把渲染卡从 ~80% 推到 ~95% |
| 槽位一次性按最大尺寸放好 | 尺寸变化只改 `StreamWidth/Height`，不做 magic/ack 握手，避免渲染线程上的停顿 |
| 生产端只写、消费端只读 | 跨适配器 fence 在消费节点读不出来；同步只靠 header 里的 `published/consumed` 值 |

## 3. 三个运行单元

| 单元 | 进程 | 负责 | 不负责 |
|---|---|---|---|
| `fakerFG.dll` | 游戏 | FG 契约、输入 harvest、XeFG 链、Present、marker | 跨卡传输、共享槽布局 |
| `xess_mfg_producer.dll` | 游戏（fakerFG 加载） | 订阅记录、降采样、拷贝、发布 fence、读 ACK | 任何 FG 决策、任何钩子 |
| `xess_mfg_consumer.exe` | 独立 | WGC 颜色、配对、XeFG 插帧、上屏、诊断 | 不进入游戏进程 |
| `mgpu_ui.exe` | 独立 | 启动/停止 consumer、参数回显、状态栏 | 不参与渲染 |

`bridge.exe` 是 consumer 的旧 fork，保留为 A/B 基线，不进入当前发布包。

## 4. 一帧的生命周期

1. 游戏渲染并调用 DLSS-SR / FG 的 Evaluate；
2. `fakerFG` harvest 参数块（MV/深度/矩阵/jitter/reset/相机），在标题仍打开的命令表上：
   - `MvResample`（如需）把 MV 缩到 depth 尺寸；
   - 发布 `MgpuFgFrame` 记录给 producer；
3. producer 在同一个 sink 里：
   - 按 `StreamPercent` 降采样 MV/深度（MV 与 depth 必须同尺寸）；
   - 录 `barrier → CopyTextureRegion → barrier`，提交到第二队列；
4. 游戏 Present：`fakerFG` 分配 `present_id`、画四角 marker、写共享元数据、
   Signal `fwd[slot]`；producer 的第二队列等游戏队列提交后完成拷贝并 Signal；
5. consumer：
   - WGC 交付颜色（自带合成器时间戳）；
   - 从颜色四角解码 marker → 得到 `frame_id` → 找到对应 publication 槽；
   - 预拷命中则直接用本地 MV/深度副本；未命中才补拷；
   - 可选 `--upsample` 重建到渲染尺寸；
   - tag MV/depth/colour → XeFG → `Present` → ACK 槽位；
6. 覆盖窗口把最终画面呈现在消费卡驱动的显示器上。

## 5. 数据面与同步面

| 面 | 内容 |
|---|---|
| 数据面 | 命名 file mapping + `OpenExistingHeapFromFileMapping` + placed resource；MV/depth 为跨适配器共享纹理 |
| 同步面 | 每槽 `fwd`（生产→消费）、`back`（消费→生产）、`FrameShared.published`、`SharedControl.consumed[]` 信用窗口 |
| 元数据 | `frame_id` / `frame_key` / `frame_qpc_ms` / jitter / MV scale / 相机 / 矩阵 / 语义位 |
| 配对主键 | 颜色里的 marker → `frame_id`；时间戳/present-ring 仅兜底 |
| 版本 | `kSharedVersion = 17`，不匹配硬拒绝 |

## 6. 模式

`fakerFG.ini` 的 `Mode=` 决定谁出帧：

| Mode | 游戏自己的 DLSS-G | 谁生成 | 用途 |
|---|---|---|---|
| `off` / `observe` | 允许（转发真运行时） | 游戏自己的 FG | A/B、调查 |
| `idle` | 拒绝 | 没人（1X） | 验证“不出帧也不黑屏” |
| `local` | 拒绝 | fakerFG（XeFG，本卡） | 自检平台 |
| `consume`（默认） | 拒绝 | 消费卡 consumer | 双卡出货 |

`consume` 下游戏侧只出原生帧，补帧全部由副卡完成；`local` 下整条 FG 在渲染卡上跑完，
用于不接副卡时验证链路。

## 7. 已知边界

- 颜色只有 WGC 一条路；跨卡只有 MV/depth（+ 保留的 scar 槽，不再使用）。
- XeFG 插帧要求游戏窗口无边框；独占全屏抓不到。
- marker 与配对要求窗口/分辨率 1:1（缩放会让标记错位）。
- 3x/4x 依赖 `--unlock-mfg` 内存补丁，仅供个人研究；2x 是原生能力。
- 架构与性能结论来自单机（RTX 3080 Ti + RX 6750 GRE + 120 Hz），换硬件需重测。
