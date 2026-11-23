# DLSS-FG 输入链路的 HUDless 从哪来（OptiScaler 对照）

> 依据：`third_party/OptiScaler-master/OptiScaler/`（下称 OS）源码，以及本仓
> `docs/findings/FG_INPUT_V13_*.md` 的三款实测。
> 只讨论「喂给帧生成（DLSSG / XeFG / FX/FfxFG）的 HUDless 颜色」，
> 不讨论 UI 流的完整合成（见 `UI_UNBLEND_MODE4.md` / `XEFG_UI_INPUTS.md`）。

结论先行：

1. **HUDless 的获取位置由 `FgInput` 决定，不是由游戏决定**。OptiScaler 有 6 条输入车道
   （`NoFG` 除外），每条车道的 hudless 是不同 API 面的产物（SL tag / NGX 参数块 /
   FSR3 结构体 / XeFG swapchain tag / 自己抓）。
2. 只有 `FgInput=Upscaler`（自家超分当输入源）才会启用 **Hudfix 资源跟踪**去「造」hudless；
   其余车道都是直接消费游戏交出来的 hudless（或 present 兜底）。
3. 对 DLSSG 输出而言，最稳的顺序是：
   **游戏 HUDLESS tag → SCALING_OUTPUT → present/backbuffer → 无**；
   Hudfix 只覆盖“游戏根本没交 hudless”的超分车道，HudCutoff 只是在
   present 与 hudless 之间做一致性修正，不能凭空造出无 UI 画面。

---

## 1. 输入车道与 HUDless 来源

`OS/State.h:34-45`：

```cpp
enum class FGInput { NoFG, Upscaler /*OptiFG*/, DLSSG /*Streamline*/, NvngxFG, FSRFG, FSRFG30, XeFG, ForceXeLL };
```

| `FgInput` | HUDless 来源（API 面） | 代码入口 | 官方注释 |
|---|---|---|---|
| `dlssg` | 游戏的 Streamline tag `sl::kBufferTypeHUDLessColor` | `hooks/Streamline_Hooks.cpp:537-544` → `inputs/FG/Streamline_Inputs.cpp:295-296,304-447` | “Supports Hudless out of the box”（`OptiScaler.ini` FrameGen 段） |
| `nvngxfg` | 游戏 NGX 参数块 `DLSSG.HUDLess`（+ `DLSSG.Backbuffer`） | `framegen/nvngx/Nvngx_FG.cpp:406-462` | 同上 |
| `fsrfg`（FSR3 inputs） | `FfxFrameInterpolationDispatchDescription::currentBackBuffer_HUDLess`，缺失时用 `currentBackBuffer` | `inputs/FG/FSR3_Dx12_FG.cpp:635-690` | 同上 |
| `fsrfg30`（ffx-api） | `ffxDispatchDescFrameGeneration::presentColor`（当 hudless） | `inputs/FG/FfxApi_Dx12_FG.cpp:1026-1056` | 同上 |
| `xefg` | 游戏 XeFG swapchain tag `XEFG_SWAPCHAIN_RES_HUDLESS_COLOR`（BACKBUFFER 只提供矩形，不接受资源） | `inputs/FG/XeFG_Inputs_Dx12.cpp:539-600` | 同上 |
| `upscaler` | **Hudfix 资源跟踪捕获**，或游戏经 NGX 参数 `Opti.Hudless` 显式交付 | `hooks/D3D12_Hooks.cpp:2472-2478`；`hudfix/Hudfix_Dx12.cpp:547-955`；`inputs/FG/Upscaler_Inputs_Dx12.cpp:13-57` | “To prevent UI glitching, Hudfix is required” |

这个「车道决定来源」的结构很关键：我们在 CP2077 上看到「HUDless tag 只在游戏 FG 打开时出现」，
是因为 `dlssg` 车道消费的就是 Streamline FG 契约；FG 关着时游戏不 tag，就没有 hudless。

## 2. 各车道的具体获取点

### 2.1 DLSSG（Streamline tag）——最常用、最完整

入口有三个，都只在 `activeFgInput == FGInput::DLSSG` 时才把 tag 转给 `slFGInputs`：

- `hkslSetTag`（`Streamline_Hooks.cpp:537-544`）
- `hkslSetTagForFrame`（`:616-630`）
- `hkslEvaluateFeature`（`:643-659`，处理以输入结构体形式进来的 tag）

`Sl_Inputs::reportResource`（`Streamline_Inputs.cpp:304-447`）做的事：

| 处理 | 代码 | 说明 |
|---|---|---|
| 类型映射 | `:287-302` | Depth/HiRes/Linear → Depth；MV → Velocity；`kBufferTypeHUDLessColor` → HudlessColor；`kBufferTypeUIColorAndAlpha` → UIColor |
| 生命周期 | `:344-359` | `eValidUntilPresent` → `UntilPresent`；其余（含 `eOnlyValidNow`）→ `ValidNow`；缺 cmdList 时 “YOLO” 成 `UntilPresent` |
| 逐标题强制 | `:354-359` | `FGDepthValidNow` / `FGVelocityValidNow` / **`FGHudlessValidNow`** 把 validity 改成 `ValidNow` |
| 帧配对 | `:361-379` | frameId → index；没有 frameId 时用 `GetIndexWillBeDispatched()` |
| 插值矩形 | `:398-401` | **HUDless 的 extent 就是插值矩形**（不是 depth/MV 的尺寸） |
| 交付 | `:433-443` | `Dx12Resource{type, resource, cmdList, state, validity, frameIndex}` → `fg->SetResource()` |

同一条车道的 SL1（老 Streamline）版本：`inputs/FG/Streamline_Inputs_Sl1_Dx12.cpp:320-343`。

带游戏修正的两个 quirk：

- `CyberpunkHudlessState`（`misc/Quirks.h:11`）：当 CP2077 的 HUDless state 是
  `NON_PIXEL|PIXEL_SHADER_RESOURCE` 时改成 `UNORDERED_ACCESS`（`Streamline_Hooks.cpp:528-535`）。
- `IgnoreTagsWithoutHudlessForFG`（`Quirks.h:29`）：同一批 tag 里只有 Depth+MV、没有
  HUDless 时整批跳过（`Streamline_Hooks.cpp:487-515` / `:576-604`）。
  **这是 Unity 题的保护**：ZZZ、Arknights: Endfield 会把超分链的 depth/MV 当成 FG 输入，
  MV/深度是翻转的，必须丢弃（`GameConfigs.cpp:216,237`）。

### 2.2 NvngxFG（NGX 参数块）

`Nvngx_FG::D3D12_EvaluateFeature`（`framegen/nvngx/Nvngx_FG.cpp:386-462`）：

- `DLSSG.HUDLess` 是游戏交的无 UI 帧；`DLSSG.Backbuffer` 是带 UI 的帧；
- `FGHudCutoff > 0` 或 `FSRFGHudlessMismatchFixup` 时，在 `frameIndex == 1` 上跑
  `HudCopy` 把 present 与 hudless 对齐（阈值默认 0.03，CP2077 且非 FSRFG 输出时 0.01）；
- `NvngxFGDisableHudless`（`Config.h:757`）直接把 `DLSSG.HUDLess` 置空，交给下游兜底；
- UI 侧同名参数：`DLSSG.UI` / `DLSSG.UIAlpha`（`:771-790`）。

### 2.3 FSRFG / FSRFG30

- FSR3 inputs：`currentBackBuffer_HUDLess` 存在就作为 HudlessColor（`FSR3_Dx12_FG.cpp:635-661`）；
  不存在时若有 present callback，则把 `currentBackBuffer` 当 hudless（`:663-690`）——
  这是“present 兜底”，HUD 会被烘进插值底图。
- FfxApi：`ffxDispatchDescFrameGeneration::presentColor` 直接当 HudlessColor
  （`FfxApi_Dx12_FG.cpp:1026-1056`），可用 `FSRFGSkipDispatchForHudless` 关掉；
  两条 callback 路径会把 backbuffer 拷进 `_hudless[fIndex]` 再发布（`:1330-1385`、`:1434-1484`），
  这是为了在 present / callback 时机拿到一帧稳定的颜色副本。

### 2.4 XeFG（游戏自己的 XeFG swapchain）

`XeFGInputs::TagFrameResource`（`XeFG_Inputs_Dx12.cpp:536-600`）：

- `RES_HUDLESS_COLOR` / `RES_BACKBUFFER` 都只用来定 `SetInterpolationRect/Pos`；
  **BACKBUFFER 不作为资源接收**（proxy swapchain 隐式提供）；
- `RES_HUDLESS_COLOR` → `FG_ResourceType::HudlessColor`；
- `RV_ONLY_NOW` 且给到 cmdList → `ValidNow`，否则 `UntilPresent`；
- UI 拷贝可能走另一条命令缓冲：`fg->GetUICommandList(index)`（`:252`）。


### 2.5 Upscaler 车道：Hudfix（唯一会“自己造 hudless”的车道）

启用条件很硬：**ResTrack 的设备 hook 只在 `activeFgInput == Upscaler` 且
`FGDisableHUDFix == false` 时安装**（`hooks/D3D12_Hooks.cpp:2472-2478`），
Vulkan 侧同样只看 `Upscaler`（`hudfix/Hudfix_Vk.cpp:178`）。其余车道没有 Hudfix。

捕获点不是“某个固定 pass”，而是**资源跟踪到所有可能成为“最终颜色”的写入点**：

| 触发 | 代码 | 说明 |
|---|---|---|
| RTV/SRV/UAV 创建、OMSetRenderTargets、根描述符、Draw/Dispatch | `resource_tracking/ResTrack_dx12.cpp`（hook 安装与筛选）；`Config.h:680-688` | 每个来源都能单独关（`HudfixDisableRTV/SRV/UAV/OM/SCR/SGR/DI/DII/Dispatch`）；`ResTrack_dx12.cpp:523` 也重复校验 `HUDFix && FGEnabled` |
| 基本筛选 | `:299-407` | 尺寸≈swapchain（`FGHUDFixExtended` / `FGRelaxedResolutionCheck` ±5%）、格式同组（`:22-95`）、不允许 depth/RTAS/encode 等 flag |
| 每帧配额 | `:238-259` | `FGHUDLimit`（默认 1）：第 N 个候选才抓，延迟越大越容易避开 HUD pass |
| 实际拷贝 | `:734-808` | `CopyResource` / `CopyTextureRegion`（尺寸不同时居中裁切）到 `_captureBuffer[fIndex]` |
| 格式转换 | `:813-892` | 格式不同则 `FT_Dx12` 转到 swapchain 格式后再交 |
| 交付 | `:894-916` | `SetResource{type=HudlessColor, state, validity=UntilPresent, frameIndex=GetIndexWillBeDispatched()}` |
| 生命周期/黑名单 | `:590-717` | 候选资源会被记住、重试、禁用，避免拿错 buffer 抖动 |

另外 Vulkan 有两条专用路径（`OptiScaler.ini` `[OptiFG]`），且候选集是“超分之后、与 swapchain 同尺寸的被使用图像”，
第 `FGHUDLimit` 个候选被拷成 hudless（`hudfix/Hudfix_Vk.h:7`；配额在 `Hudfix_Vk.cpp:271`）：

- `HUDFixUITexture`：用游戏最后一个 compute pass 里 blend 的 UI 纹理（`Hudfix_Vk.cpp:620`）；
- `HUDFixUIUnblend`：从帧里把这块 UI 去掉重建 hudless；**FSR-FG 只能靠这条路拿 UI；
  DLSSG 直接吃 UI 纹理**。

外部的显式交付通道：游戏/插件把纹理写进 NGX 参数 `Opti.Hudless` + `Opti.Hudless.State`，
Upscaler 车道优先用它（`inputs/FG/Upscaler_Inputs_Dx12.cpp:13-57`；D3D11 侧
`Upscaler_Inputs_Dx11wDx12.cpp:271-279` → `Hudfix_Dx11::SetGameHudless/PublishGameHudless`
`:758-817`）。没有 `State` 会被忽略。

---

## 3. 不同游戏/引擎实际落在哪

| 游戏/引擎 | 游戏自身 FG | HUDless 获取位置 | 相关配置/修正 |
|---|---|---|---|
| CP2077（RED4） | DLSSG | SL tag `HUDLESS_COLOR` + `SCALING_OUTPUT`；FG 关时都没有 | `CyberpunkHudlessState`（R10 state 修正）、`FSRFGHudlessMismatchFixup`、upscaler 车道 `FGDisableHUDFix`（`GameConfigs.cpp:320-328`） |
| Khazan（UE4.27） | DLSSG | 每帧 `HUDLESS_COLOR`，3840×2160，`state=0x400`，`lc=onlyValidNow` | 见 `docs/findings/FG_INPUT_V13_KHAZAN.md:65,214` |
| PRAGMATA（RE Engine） | DLSSG | FG 关：无 HUDLESS，只有 `SCALING_INPUT/OUTPUT`+`EXPOSURE`；FG 开：`HUDLESS_COLOR` + `UI_COLOR_AND_ALPHA` 才出现 | `docs/findings/FG_INPUT_V13_PRAGMATA.md:93,176-183,281-285` |
| DD2 / MH Wilds / RE9 / Onimusha | DLSSG | 同上（依赖游戏 FG 契约） | **Hudfix incompatible**：upscaler 车道直接 `FGDisableHUDFix`（`GameConfigs.cpp:283-315`） |
| Nixxes 移植（Spider-Man 1/2、Ratchet、Horizon、TLOU、GoT） | DLSSG | SL tag `HUDLESS_COLOR` | Spider-Man Remastered / DS2 带 `FSRFGHudlessMismatchFixup`；多数 upscaler 车道 `FGDisableHUDFix`（`:470-510`） |
| UE 系（Wuthering Waves、FF7 Rebirth、Stellar Blade…） | DLSSG/无 | SL tag `HUDLESS_COLOR`（输出分辨率）；FF7RB/Stellar Blade 的 upscaler 车道不用 Hudfix | `GameConfigs.cpp:225-230,760-770,805-810` |
| Unity（ZZZ、Arknights: Endfield） | DLSSG | SL tag，但同批 tag 可能没有 hudless | `IgnoreTagsWithoutHudlessForFG`（`:216,237`） |
| 只有 FSR3 FG 的题 | FSRFG | `currentBackBuffer_HUDLess`；没有则 present | `FSR3_Dx12_FG.cpp:635-690` |
| 无 FG、只有超分 | 无 | **Hudfix 捕获**（或 `Opti.Hudless`） | 需要 `HUDFix=true`；Hudfix 不兼容的题必须放弃或换输入源 |
| No Man's Sky（Vulkan） | DLSSG/FSRFG | SL tag / ffx presentColor | `FSRFGHudlessMismatchFixup` + `VulkanDLSSBarrierFixup`（`:820-830`） |

> 备注：`GameConfigs.cpp` 里的 `WithUpscalerFGInput(FGDisableHUDFix = true)` 表示
> **“这题只在 upscaler 车道禁 Hudfix”**，不是全局关闭；换 `FgInput=dlssg` 时仍吃游戏 hudless。


---

## 4. 可用的兜底阶梯（按优先级）

| 级别 | 兜底 | 落点 | 代价 / 风险 | 何时启用 |
|---|---|---|---|---|
| L0 | 游戏 HUDLESS（SL tag / `DLSSG.HUDLess` / `currentBackBuffer_HUDLess` / XeFG tag） | 各输入车道 | 最干净；但 lifetime 可能是 `onlyValidNow`，晚拷 = 读回收内存 | 默认 |
| L1 | `SCALING_OUTPUT` | **OptiScaler 没有这一层**（SL 车道的 `GetResourceType` 只认 HUDless/Depth/MV/UI，`Streamline_Inputs.cpp:287-302`）；本仓补在 `lock_frame/faker_fg/proxy_streamline.cpp:451-464,1303-1320` | 是「已超分、可能仍带 UI / 或 FG 关时为空」的图；CP2077 FG 关时实测全黑 | 有 tag 但 HUDLESS 快照不可用 |
| L2 | present / backbuffer 当 hudless | `FSR3_Dx12_FG.cpp:663-690`；`FfxApi_Dx12_FG.cpp:1026-1056` | HUD 烘进底图 → 插值帧 HUD 拖影；只能靠 HudCutoff 缓解 | 输入源只给了 present |
| L3 | Hudfix 资源跟踪捕获 | `Hudfix_Dx12.cpp:547-955` / `Hudfix_Vk.cpp` | 只对 upscaler 车道；有 CPU/GPU 成本、可能抓错 buffer、**有崩溃风险**（`HUDFix` 注释明说） | 游戏不交 hudless、又必须出帧 |
| L4 | HudCutoff（HudCopy）修正 present | `FSRFG_Dx12.cpp:970-1032`、`Nvngx_FG.cpp:406-458`、shader `shaders/hud_copy/precompile/HudCopy.hlsl` | 不造 hudless，只把 present 与 hudless 的差异压到 UI 上；阈值敏感 | present/hudless 有色差/噪声（CP2077、DS2、NMS） |
| L5 | 放弃 hudless | `FGDisableHudless`（`OptiScaler.ini` FrameGen）、`NvngxFGDisableHudless`、`IgnoreTagsWithoutHudlessForFG` | FG 质量下降或直接不出帧；后者用于保护 SR 的 depth/MV 不被误当 FG 输入 | Hudfix 不兼容 / 有 HUD 拖影不可接受 |

补充的“可用性闸门”（不是兜底，但决定兜底会不会生效）：

- **validity**：`FGHudlessValidNow` 强制 `ValidNow`；`FGOnlyAcceptFirstHudless`
  只收每帧第一个 hudless（`framegen/dlssg/DLSSG_Dx12.cpp:609-628`）。
- **格式**：Hudfix 允许同组格式（`Hudfix_Dx12.cpp:22-95`）；DLSSG 输出侧格式一变就
  `fgChanged` 重建（`DLSSG_Dx12.cpp:724-742`）。XeFG 侧要求 HUDless/UI/backbuffer
  同格式同尺寸（见 `XEFG_UI_INPUTS.md` §1）。
- **尺寸**：默认必须等于 swapchain；`HUDFixRelaxedResolutionCheck` 允许 ±5%
  （Witcher 3 的黑边）；`HUDFixDontUseSwapchainBuffers` 禁止把交换链缓冲当 hudless。
- **没有 cmdList**：`Streamline_Inputs.cpp:347-352` 会把 `ValidNow` “YOLO” 成
  `UntilPresent`；这正是本仓 CP2077 踩过的坑（`CHANGELOG.md:2715`：`onlyValidNow`
  晚一帧拷到回收内存 → `0x887a0001` GPU hang）。

---

## 5. 与本仓实现的对照

| 点 | OptiScaler | 本仓 | 结论 |
|---|---|---|---|
| HUDless 优先级 | 各车道单一来源；Hudfix 仅 upscaler | `HUDLESS stable snapshot` → `SCALING_OUTPUT` → 空（`proxy_streamline.cpp:1303-1323`） | 我们多一层 SCALING_OUTPUT，方向正确 |
| `onlyValidNow` | `ValidNow` = 立刻在给到的 list 上拷 | 在 tag 时拷成稳定快照（`snapshot_hudless`） | 等价，且避开了生命周期坑 |
| 逐标题强制 | `FGHudlessValidNow` / `FGOnlyAcceptFirstHudless` | `cfg().fg_hudless_valid_now`（`proxy_streamline.cpp:322-323`） | 已对齐 |
| present 兜底 | FSRFG/ffx 车道用 presentColor | 本仓颜色走 WGC/共享缓冲，另有 `SCALING_OUTPUT` 兜底 | 不需要 present 兜底时不要引入（HUD 拖影） |
| HudCutoff | 只在 FSRFG / NvngxFG 输出侧对 present 做对齐 | 未实现；我们的 `unblend_ui.hlsl` 是另一条路（从 P、H 差分出 UI） | 若要复用，注意它是“改 present”，不是“造 hudless” |

一句话总结给 DLSS-FG 输入链路：

> **HUDless 的“家”在游戏的 FG 契约里（SL tag / NGX 参数 / FSR3 结构体 / XeFG tag）；
> OptiScaler 只在没有契约可用时用 Hudfix 自己抓；抓不到就退到 SCALING_OUTPUT/present，
> 再不行就关掉 hudless。HudCutoff 只是修正 present 与 hudless 的一致性，不能替代 hudless。**

---

## 6. 证据索引

| 内容 | 位置 |
|---|---|
| 输入车道枚举 | `OS/State.h:34-45`；`OptiScaler.ini` `[FrameGen] FGInput` 注释 |
| SL tag → Hudless 映射 / validity / frame index | `OS/inputs/FG/Streamline_Inputs.cpp:287-302,344-359,361-379,398-401,433-443` |
| SL1 tag 版本 | `OS/inputs/FG/Streamline_Inputs_Sl1_Dx12.cpp:320-343` |
| tag hook（仅 DLSSG 车道） | `OS/hooks/Streamline_Hooks.cpp:537-544,616-630,643-659` |
| `IgnoreTagsWithoutHudlessForFG` | `OS/hooks/Streamline_Hooks.cpp:487-515,576-604`；`OS/misc/Quirks.h:29` |
| `CyberpunkHudlessState` | `OS/hooks/Streamline_Hooks.cpp:528-535`；`OS/misc/Quirks.h:11` |
| NGX `DLSSG.HUDLess` / HudCutoff / DisableHudless | `OS/framegen/nvngx/Nvngx_FG.cpp:406-462,771-790` |
| FSR3 `currentBackBuffer_HUDLess` + present 兜底 | `OS/inputs/FG/FSR3_Dx12_FG.cpp:635-690` |
| ffx `presentColor` 当 hudless | `OS/inputs/FG/FfxApi_Dx12_FG.cpp:1026-1056,1330-1385,1434-1484` |
| XeFG `RES_HUDLESS_COLOR` | `OS/inputs/FG/XeFG_Inputs_Dx12.cpp:536-600` |
| Hudfix 启用条件（仅 upscaler） | `OS/hooks/D3D12_Hooks.cpp:2472-2478`；`OS/hudfix/Hudfix_Vk.cpp:178` |
| Hudfix 筛选/抓取/交付/配额 | `OS/hudfix/Hudfix_Dx12.cpp:22-95,238-259,299-407,547-955` |
| `Opti.Hudless` 外部交付 | `OS/inputs/FG/Upscaler_Inputs_Dx12.cpp:13-57`；`OS/hudfix/Hudfix_Dx11.cpp:758-817` |
| HudCutoff shader | `OS/shaders/hud_copy/precompile/HudCopy.hlsl`；`OS/framegen/ffx/FSRFG_Dx12.cpp:970-1032` |
| 逐游戏配置 | `OS/misc/GameConfigs.cpp`（`Hudfix incompatible` / `FGDisableHUDFix` 注释） |
| 本仓实测 | `docs/findings/FG_INPUT_V13_2077.md`、`..._KHAZAN.md`、`..._PRAGMATA.md` |
| 本仓 hudless 优先级 | `lock_frame/faker_fg/proxy_streamline.cpp:451-464,1303-1323`；`CHANGELOG.md:2715,2722-2724` |

