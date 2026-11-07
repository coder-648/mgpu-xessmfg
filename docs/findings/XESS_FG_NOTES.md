# XeSS-FG 约束与卡间传输载荷（从 SDK 3.0.2 提炼）

来源：`third_party/xess/doc/xess_fg_developer_guide_english.md`（v1.3 指南）、`inc/xess_fg/*.h`。
这些是写代码前必须钉死的事实，直接影响架构。

---

## 1. 硬性约束（会改变 PLAN 的目标值）

| 约束 | 原文/位置 | 对本项目的影响 |
|---|---|---|
| **非 Intel GPU 默认只能生成 1 帧** | Requirements：`On non-Intel GPUs the maximum number of generated frames is one.` | 副卡是 RX 6750 GRE（非 Intel）→ 官方 `libxess_fg.dll` 默认把 `maxSupportedInterpolations` 夹到 **1**（2x）。**但这个限制是 DLL 里几个字节的检查，可以解锁**：`--unlock-mfg` 之后实测上报 3（4x），见 `docs/findings/MFG.md` |
| XeSS-FG 强制依赖 XeLL 1.3.0+ | Requirements + `xefgSwapChainSetLatencyReduction` | 必须同时链 `libxell.dll`，每帧 `xellSleep` + `xellAddMarkerData` |
| 非 Intel 卡的 pacing 由库内高优先级线程做 | Programming Guide | 不用我们自己做 present 节流；但这也意味着 present 时序由库掌控 |
| 需要 VC++ 运行库 14.40.33810+ | Requirements | `msvcp140/vcruntime140/vcruntime140_1.dll` |
| 系统 Win10/11 x64 ≥ 19043/22000 | Requirements | 本机 Win11 29695 ✅ |
| 非 Intel 卡要求 **SM 6.4** | Requirements | 6750 GRE（RDNA2）FL 12_2 ✅ |

## 1.5 运行时二进制已与先例包对齐（MD5 校验）

以下 `md5sum` 结果一致：

| 文件 | MD5 | SDK v3.0.2 | 先例包 `玄学双显卡1.7版` | 文件内 FileVersion |
|---|---|---|---|---|
| `libxess_fg.dll` | `e5da7e91e3af8a8340b3fef922912e6e` | ✅ | ✅ 同一份 | 1.3.1.78 |
| `libxell.dll` | `831a165eb6832a5c1ec46eee2a33de30` | ✅ | ✅ 同一份 | 1.3.2.10 |

含义：
1. XeSS SDK 的**外部版本号是 3.0.2，内部 FG 组件版本仍是 1.3.x** —— 所以先例文档里写的 "libxess_fg 1.3.1.78" 并不是旧版，就是现在这版。之前 `docs/findings/ENV_REPORT.md` §2 里"现存是 XeSS2 时代旧运行时"的怀疑**不成立，已修正**。
2. 先例项目已在**非 Intel GPU（RTX 5050）**上实际跑通过这套 DLL → `xefgSwapChainD3D12CreateContext` 在非 Intel 设备上能成功的把握很大（仍要 M0 在 6750 GRE 上实测）。
3. 部署时直接拷 `third_party/xess/bin/` 里的 DLL 即可，不需要另找版本。

## 2. 架构形态：代理交换链（不是"算一张图还给你"）

XeSS-FG 不是 `Execute(buffers) -> image` 的函数式 API，而是**接管 present**：

```
xefgSwapChainD3D12CreateContext(device, &ctx)        // 检查设备能力，10-50ms
xefgSwapChainD3D12GetProperties(ctx, &props)         // 拿所需描述符数量
xefgSwapChainD3D12InitFromSwapChainDesc(ctx, hwnd, &scDesc, &fsDesc, queue, dxgiFactory, &initParams)
xefgSwapChainD3D12GetSwapChainPtr(ctx, IID_PPV_ARGS(&swapChain))   // 之后所有 present 都走它
xefgSwapChainSetEnabled(ctx, true)
每帧：
  xefgSwapChainD3D12TagFrameResource(ctx, cmdList, presentId, &resData)   // MV + Depth
  xefgSwapChainTagFrameConstants(ctx, presentId, &constData)              // View/Proj/抖动
  xefgSwapChainSetPresentId(ctx, presentId)
  swapChain->Present(...)
```

**这对 PLAN 是利好**：`InitFromSwapChainDesc` 会**自己为该窗口创建交换链**，所以副卡（6750 GRE）上的 bridge 窗口 + 交换链由 XeSS-FG 直接接管，不需要我们手搓 present 循环或自己插帧。先例项目里那些 `MidFrameQueue`/`MaxFrameLatency`/`RenderFlush` 的自研调参，在这一版可以大幅简化。

## 3. 必须跨卡传输的输入（每帧）

| 资源 | 格式 | 尺寸规则 | 必填 |
|---|---|---|---|
| 运动矢量 `XEFG_SWAPCHAIN_RES_MOTION_VECTOR` | `R16G16_FLOAT` 或类似；可用 NDC 或像素空间；可带抖动 | 低分辨率（与 DLSS/TAAU 输入同尺寸）**或** 高分辨率（目标分辨率） | ✅ |
| 深度 `XEFG_SWAPCHAIN_RES_DEPTH` | 任意深度格式（D32_FLOAT / D24_UNORM） | **必须与 MV buffer 同尺寸**（指南原文） | ✅ |
| HUD-less 颜色 | 与 backbuffer 同格式 | 与 backbuffer 同尺寸 | ❌ 仅在自定义 UI 合成模式需要 |
| UI 纹理 | 与 backbuffer 同格式 | 同上 | ❌ 默认模式不需要 |
| 帧常量 `xefg_swapchain_frame_constant_data_t` | viewMatrix / projectionMatrix（行主序、无抖动）、jitterOffsetX/Y、motionVectorScaleX/Y、resetHistory、frameRenderTime | — | ✅ |

默认 UI 模式是 **UI 被插值**（`uiMode = XEFG_SWAPCHAIN_UI_MODE_AUTO`），所以**第一版可以不传 HUD-less 和 UI 纹理**，省 2 份 4K 流量。代价是 HUD 会被当作画面一起插值（可能的 HUD 拖影，先例项目也踩过这类坑，后续再加 UI 检测）。

### 传输载荷预算（4K 3840×2160，MV 取渲染分辨率 2560×1440）

| 项 | 尺寸 | 每帧 |
|---|---|---|
| 颜色（RGBA8，4K） | 8.29 Mpx × 4 B | **33.2 MB** |
| 深度（R32F，1440p，与 MV 同尺寸） | 3.69 Mpx × 4 B | **14.7 MB** |
| 运动矢量（RG16F，1440p） | 3.69 Mpx × 4 B | **14.7 MB** |
| 合计 | | **≈ 62.6 MB/帧** |

- 60 渲染帧/秒 → **≈ 3.8 GB/s**
- 120 渲染帧/秒 → **≈ 7.5 GB/s**（2x 后上屏 240）
- 若 MV/深度改用高分辨率（4K），载荷涨到 ≈ 99.6 MB/帧 → 60fps 需 6.0 GB/s

→ 结论：**先用低分辨率 MV + 低分辨率深度**（顺带省掉深度上采样 pass），把 4K60 作为第一目标；4K120 是否可行取决于实测链路（见 `docs/findings/ENV_REPORT.md` §3 的带宽风险）。

## 4. 初始化标志位（`xefg_swapchain_init_flags_t`）

| 标志 | 何时用 |
|---|---|
| `XEFG_SWAPCHAIN_INIT_FLAG_NONE` | 默认 |
| `XEFG_SWAPCHAIN_INIT_FLAG_INVERTED_DEPTH` | 逆向 Z（现代游戏多半是，需实测判断） |
| `XEFG_SWAPCHAIN_INIT_FLAG_EXTERNAL_DESCRIPTOR_HEAP` | 与 ReShade 共用描述符堆时 |
| `XEFG_SWAPCHAIN_INIT_FLAG_HIGH_RES_MV` | **已废弃**，设了没效果 |
| `XEFG_SWAPCHAIN_INIT_FLAG_USE_NDC_VELOCITY` | MV 是 NDC 而非像素空间 |
| `XEFG_SWAPCHAIN_INIT_FLAG_JITTERED_MV` | MV 未去抖动 |

## 5. 资源有效期语义（影响 ring buffer 深度设计）

- `XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT` —— 活到下次 present，库可以延后/省掉拷贝
- `XEFG_SWAPCHAIN_RV_ONLY_NOW` —— 只在当前有效，库会立刻在传入的 command list 上记录拷贝

→ 我们用跨卡 ring buffer 喂数据时，应优先 `RV_UNTIL_NEXT_PRESENT`，让 ring 槽位在下一个 present 前不被覆写（即 ring 深度 ≥ 2）。

## 6. 与 PLAN 模块的对应

| PLAN 模块 | 依赖到的 API |
|---|---|
| 模块 0 能力验证 | `xefgSwapChainD3D12CreateContext`（在 AMD 设备上）+ `xellD3D12CreateContext` + 跨卡 shared texture 互拷 |
| 模块 1 卡间传输 | D3D12 `CreateSharedHandle` / `OpenSharedHandle` + `CopyResource` + fence |
| 模块 2 MV 截取 | `nvngx_dlss.dll` hook（拿游戏 DLSS 的 MV + jitter + 矩阵） |
| 模块 3 深度采集 | ReShade `DEPTH` 语义（先例 `mgpu_depth_tap.fx` 已趟过坑） |
| 模块 4 副卡 FG + 上屏 | `xefgSwapChainD3D12InitFromSwapChainDesc` + `TagFrameResource` + `TagFrameConstants` |
| 模块 5 合成 addon | ReShade `reshade.hpp` 事件 + `reshade_overlay.hpp` 面板 |
