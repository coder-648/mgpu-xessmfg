# fakerFG × OptiScaler —— 逐机制对照（当前状态）

> 原则：**fakerFG 的 FG 链路与 OptiScaler 一一对应，不做自己的发明。**
> 任何"为什么不这么做"的问题，先在这里找到 OptiScaler 的原文；找不到原文就不写代码。
> 左侧路径相对 `temp/optiscaler_src/OptiScaler-master/OptiScaler/`，右侧是我们的 `src/faker_fg/`。

## 1. 我们的链路（一页）

```
游戏启动
 ├ dxgi.dll = 我们: DxgiFactory 槽 15 -> 抓到 CreateSwapChainForHwnd            (2.1)
 ├ loader 线程: nvapi 架构门 -> Streamline 六个出口 -> ReShade -> 上采样器导出钩子
 └ 首次建链: XeFG D3D12CreateContext -> XeLL -> InitFromSwapChainDesc
              -> GetSwapChainPtr -> 包成 FgSwapChain 交给游戏                    (2.2/2.3)

每帧
 游戏 tag   -> slSetTag / slSetConstants 记录到 g_stage                          (3.1a)
 上采样器   -> NVSDK_NGX_D3D12_EvaluateFeature（导出钩子）-> sr_harvest           (3.1b)
 DLSS-G    -> provider_evaluate: harvest(参数块) -> 按 FgInput 取唯一车道 -> xefg_tag (3.2)
 游戏 Present -> FgSwapChain::Present -> before_present:
               TagFrameConstants(frameId) -> SetPresentId(frameId) -> 真 Present  (3.3)
```

## 2. 入口与时序

| # | OptiScaler | 我们 | 备注 |
|---|---|---|---|
| 2.1 | `hooks/DxgiFactory_Hooks.cpp:300/327` Detour 挂 **`CreateSwapChain`（槽 10）** 与 **`CreateSwapChainForHwnd`（槽 15）**；旧 DESC 形式转成 DESC1 再走同一条路 | `proxy_dxgi.cpp`：改共享 vtable 的槽 **10 与 15**，`hook_create_swapchain()` 把 `DXGI_SWAP_CHAIN_DESC` 转 `DESC1`（`Scaling` 映射、`AlphaMode=IGNORE`）后交给 ForHwnd 那条路 | 槽位算错一次 = 钩子一次不触发（曾把 15 算成 16） |
| 2.2 | `Util::IsOverlaySwapchain`（<100px 放行） | 同判据 | |
| 2.3 | `wrapped/wrapped_swapchain.cpp`：`WrappedIDXGISwapChain4`，`LocalPresent` 里先 FG 再转发 | `fg_swapchain.h` 的 `FgSwapChain`（完整 `IDXGISwapChain4`） | 直接改后端链 vtable 且截断拷贝 → 越界崩溃 |
| 2.4 | `XeFG_Dx12::CreateSwapchain` 的 **FGPreserveSwapChain**：同窗口再建链 → `ResizeBuffers` + `AddRef` 返回已有链 | `xefg_owned_chain()` | CP2077 启动会建两次链 |
| 2.5 | `LocalPresent()` 每帧 `GetDesc(&currentSwapchainDesc)`，失败告警 | `before_present` 每帧读一次，变化时更新 `g_width/g_height` | 改分辨率后 tag 尺寸跟随 |
| 2.6 | `wrapped_swapchain.cpp:1212`：`DXGI_ERROR_DEVICE_REMOVED -> Util::GetDeviceRemovedReason` | `PresentHooks::present_failed` → `present_failed()` | 标题只拿到 HRESULT，这里才能记下原因 |
| 2.7 | `ResizeBuffers` 只转发给真链（不重建 XeFG） | 同 | |
| 2.8 | `WrappedIDXGISwapChain4::Present`：`DXGI_PRESENT_TEST` 不做 FG 工作 | 同 | |

## 3. XeFG 输入契约

### 3.1 两条输入车道（`FgInput`，只有一个生效）

| # | OptiScaler | 我们 |
|---|---|---|
| 3.1a | `inputs/FG/Streamline_Inputs.cpp` `Sl_Inputs::reportResource`（`FGInput=DLSSG`）：`state = tag.resource->state`、`validity = lifecycle==eValidUntilPresent ? UntilPresent : ValidNow`、`cmdList = cmdBuffer`、尺寸取 `tag.extent` 或资源 desc | `proxy_streamline.cpp` 的 `g_stage` + `sl_stage_inputs()` |
| 3.1b | `inputs/FG/Upscaler_Inputs_Dx12.cpp` `UpscaleStart`（`FGInput=Upscaler`）：MV/depth/`Reset`/`MV_Scale_X,Y`/`Jitter_Offset_X,Y` 取自**上采样器**的参数块，`validity=ValidNow`，`state=MVResourceBarrier/DepthResourceBarrier`（默认 `NON_PIXEL_SHADER_RESOURCE`）；相机值来自 `UpscalerInputs::GetCameraValues`（配置 `FsrCameraNear/Far=0.1/100000`、`FsrVerticalFov=60`，`DepthInverted` 时交换 near/far） | `proxy_nvngx.cpp`：`sr_harvest()` + `sr_apply()`，钩的是上采样器模块 `EvaluateFeature` 的**导出**（`nvngx_scan_upscalers`） |
| 3.1c | 一个车道失败不会退回另一个车道 | `provider.cpp`：`have_inputs` 只由所选车道决定 |

### 3.2 资源 tag（`XeFG_Dx12::SetResource` / `GetResourceData`）

| # | OptiScaler | 我们 |
|---|---|---|
| 3.2a | `res.state`：DLSSG 车道 = `tag.resource->state`（未声明就是 `UINT_MAX`，原样传）；Upscaler 车道 = `MVResourceBarrier/DepthResourceBarrier.value_or(NON_PIXEL_SHADER_RESOURCE)` | `state_of()` 原样返回（DLSSG 车道）；`proxy_nvngx.cpp` 的 `mv_state()/depth_state()`（Upscaler 车道，`MvState/DepthState` 键，0 = 128） |
| 3.2b | `validity`：`ValidNow` / `UntilPresent` 由 tag 生命周期决定 | `until_present()` 映射 `MGPU_FG_RV_UNTIL_PRESENT` |
| 3.2c | tag 的 `pCommandList` 必须非空（`intel/xess#45`："the docs lie … it checks for it"） | 传游戏那张表；没有表时用 `(ID3D12CommandList*)1` |
| 3.2d | 深度/速度尺寸各自独立 | 同（逐流用各自 extent） |
| 3.2e | **backbuffer tag**（`Dispatch()`，仅当 `activeFgInput != Upscaler`）：`pResource=nullptr`、`resourceBase={left,top}`、`resourceSize=插值矩形`、`validity=UNTIL_NEXT_PRESENT`、`cmdList=(ID3D12CommandList*)1`；矩形可被 `FGRectLeft/Top/Width/Height` 覆盖 | `tag_backbuffer()` + `Rect*` 配置；Upscaler 车道跳过 |
| 3.2f | `ValidButMakeCopy`（拷贝到自己的纹理） | **不用**：两条车道都不需要拷贝（§5） |

### 3.3 `Dispatch()`：帧常数与 present id（`framegen/xefg/XeFG_Dx12.cpp:841-990`）

| # | OptiScaler | 我们 `tag_frame_constants()` |
|---|---|---|
| 3.3a | `viewMatrix` 由 `cameraRight/Up/Forward/Position` 构造（position 非 0 时） | 同，取自本帧 `sl::Constants` |
| 3.3b | `projectionMatrix = XMMatrixPerspectiveFovLH(fov, aspect, near, far)`，先 `if (DepthInverted) swap(near, far)`；**不用**标题的 `cameraViewToClip` | 同 |
| 3.3c | 无限远平面：`near!=0 && far==0` 标记，交换后把 `inf` 放到正确一侧 | 同 |
| 3.3d | `jitterOffsetX/Y`、`motionVectorScaleX/Y = mvecScale × mvsWidth/mvsHeight`（`multiplyByResolution=true`） | 同（乘 MV 尺寸） |
| 3.3e | `resetHistory` | 同 |
| 3.3f | `frameRenderTime`：`FTInput` 默认 `Input` → `State::lastFGFrameTime`，**只在没有 FG 时更新** | `g_frame_ms_no_fg`（`!g_generate` 才更新） |
| 3.3g | 顺序：`TagFrameConstants(frameId)` → `SetPresentId(frameId)`，都在 Present 路径 | `before_present` 里同样顺序 |

### 3.4 生成开关与就绪闸门

| # | OptiScaler | 我们 |
|---|---|---|
| 3.4a | `Activate()`：`SetEnabled(true)`，条件 `IsLowResMV() \|\| nativeAA \|\| ForceFGRenderSizeMVs \|\| IgnoreInitChecks` | `xefg_frame_feedback()`：`lowResMV \|\| nativeAA \|\| IgnoreInitChecks`（判据 = MV 宽 vs 输出宽） |
| 3.4b | `Dispatch()` 只在 **Depth 与 Velocity 都 ready** 时继续；否则跳过这一帧，不动 `SetEnabled` | 同（`xefg_tags_complete()`，库里 tag 返回 SUCCESS 才算 ready） |
| 3.4c | 1X = `SetEnabled(0)`，FG 对象仍 active、dispatch 继续 | 同 |
| 3.4d | `Present()`：`(_fgFramePresentId - _lastFGFramePresentId) > 3` → `Deactivate()` + `_waitingNewFrameData` | 同（同名计数器） |
| 3.4e | 被拒（如 `-12`）→ `fgChanged/UpdateTarget/Deactivate()` | `Deactivate()`（`fgChanged/UpdateTarget` 只重建它的 FG 对象，我们没有） |
| 3.4f | `SetNumInterpolatedFrames(targetCount)` 在 count 变化时应用 | 激活时应用一次（count 固定） |

### 3.5 初始化参数（`CreateSwapchain*Internal`）

| # | OptiScaler | 我们 |
|---|---|---|
| 3.5a | `params{}` 零初始化 → `creationNodeMask/visibleNodeMask = 0`、`uiMode = AUTO(0)` | 同 |
| 3.5b | `maxInterpolatedFrames = _maxInterpolationCount`，count 之后单独应用 | 同 |
| 3.5c | `initFlags`：`INVERTED_DEPTH` / `JITTERED_MV` / `HIGH_RES_MV`，`!FGUIPremultipliedAlpha → UITEXTURE_NOT_PREMUL_ALPHA` | 同（`UiPremultiplied` 默认 0 → 置位） |
| 3.5d | `desc` / `pFullscreenDesc` 原样传入 | 同 |
| 3.5e | Init 前先挂 XeLL（否则 `-15`） | 同 |

### 3.6 其它 XeFG 调用

| # | OptiScaler | 我们 |
|---|---|---|
| 3.6a | `SetSceneChangeThreshold(0.7)` | 同 |
| 3.6b | `EnableDebugFeature(TAG_INTERPOLATED_FRAMES)` / `(SHOW_ONLY_INTERPOLATION)` | `DebugView` / `DebugOnlyGenerated` |
| 3.6c | 每次 present 后读 `GetLastPresentStatus` —— **只**用于记账（`FG_Hooks.cpp:1541` 把它喂给 `InputCommon::xefg_present_status`），不据此开关生成 | 同（读 `framesPresented`，无策略动作） |
| 3.6d | 只有 `TagFrameConstants` / `SetPresentId` 返回失败时才 `Deactivate()` | 同 |

## 4. NGX / Streamline 门（菜单与入口）

| # | OptiScaler | 我们 |
|---|---|---|
| 4.1 | `proxies/NVNGX_Proxy.h`：按模块建真函数表，只接管 DLSS-G | `proxy_ngx.cpp` + `proxy_nvngx.cpp`（`GetProcAddress` 按模块 thunk，按 **handle** 分派） |
| 4.2 | `hooks/Streamline_Hooks.cpp:315`：只伪装 DLSS-G 的 supported/loaded/requirements/version，功能函数转发 | `proxy_streamline.cpp` 六个出口 |
| 4.3 | `Streamline_Hooks.cpp:287`：仅当 FG 输入/输出本身是 DLSSG 才从 `featuresToLoad` 摘插件 | `StripDlssGPlugin`（默认 0；CP2077 的插件就是我们的调用者） |
| 4.4 | `nvapi/NvApiHooks.cpp`：架构门 | `nvapi_arch.cpp`：只对 Streamline 侧回答 AD10x（**机制不同，见 §5**） |
| 4.8 | `hooks/LibraryLoad_Hooks.cpp:60`：请求 `nvngx.dll`/`_nvngx.dll` 时 **返回自己的模块**；这个答案要从**所有**解析模块的入口给出：`Kernel_Hooks.cpp:403`(LoadLibraryExW)、`:186`(GetModuleHandleA/W/ExA/ExW)、`Ntdll_Hooks.h:44`(LdrLoadDll)，真实核心只用完整路径加载作转发目标（`NVNGX_Proxy.h:549`） | `proxy_kernel.cpp`：MinHook **7 个**入口，基名是 NGX 核心就返回我们自己的模块（PIN 住）；真实核心用调用者给的完整路径加载并记住。`hook_get_proc` 对我们自己的模块先给我们的导出、再给真实核心的函数 | 只钩 `LoadLibraryExW` 不够——14:23 的日志就是核心经 `GetModuleHandle`/`LdrLoadDll` 被真模块应答了 |
| 4.9 | `Streamline_Hooks.cpp:2557 hookPlugin` / `:2407 slotDetour` / `:1583 hkdlss_slGetPluginFunction`：钩**每个 SL 插件模块**的 `slGetPluginFunction`（每模块一个槽），对 `slOnPluginLoad` 返回自己的钩子 | `proxy_kernel.cpp`：插件模块一加载就给 `slGetPluginFunction` 装 MinHook（8 槽模板 thunk），`slOnPluginLoad` 返回我们的 `sl_on_plugin_load<N>` | 这是插件的入口表，`slOnPluginLoad` 是架构门所在 |
| 4.10 | `Streamline_Hooks.cpp:925 spoofArch` + `Streamline_Hooks.h:13-41`（自己的 `SystemCaps` 结构）+ NVIDIA 的 `include/sl.param/parameters.h` | 同：`third_party/streamline/include/sl.param/parameters.h`（MIT 原文）+ 本地 `SlSystemCaps` 布局；`sl.param.common.gpuInfo` 里最高架构 < `AD100(0x170)` 时把各 adapter 的 `architecture` 写成 `0xFFFFFFFF`（= 它的 `maxArch`） | 这是它真正的架构门机制（`ArchSpoof` 只做开关；`nvapi_arch.cpp` 那条路是补充） |
| 4.11 | `Streamline_Hooks.cpp:962-965 / 1055-1076`：`spoofArch()` 之后 **`setArch(currentArch)` 还原** | `sl_on_plugin_load<N>`：改前逐 adapter 存 `saved[]`，调用完原插件的 `slOnPluginLoad` 后写回 | `sl.param.common.gpuInfo` 是**共享**块：不还原就把 `0xFFFFFFFF` 漏给后面加载的每个插件（实测 `sl_dlss_d` 读到 0xFFFFFFFF），DLSS 侧由此拿到不存在的架构模型 |
| 4.12 | `NvApiHooks` 只伪装 Streamline 需要的查询 | `nvapi_arch.cpp::caller_is_streamline_module()`：正名单（`sl.interposer/sl.common/sl.dlss*/sl_reflex*/sl_pcl*` 与 `NGX\models\sl_*_override_*`），其余一律照实 | 第一版"非 NVIDIA 模块就伪装"把**游戏本体**也回答成 AD10x（真实 GPU 是 Ampere `0x170`），CP2077 随即按 Ada 去要 DLSS 能力/模型，拿不到就把 DLSS 选项从设置菜单里删掉（实测） |
| 4.6 | `NVNGX_Parameter.cpp:801-825`：`FrameGeneration.Available/NeedsUpdatedDriver/FeatureInitResult` + `FrameInterpolation.Available/NeedsUpdatedDriver/FeatureInitResult` + `DLSSG.MultiFrameCountMax`（保留驱动表里的值） | 同（`answer_capability()` / `thunk_caps()` 两处，同一组键） |
| 4.7 | `NVNGX_Proxy.h:102/121/141`：`FeatureSupported = Supported`、`MinHWArchitecture = 0` | 同（原来是 0x170，已改 0） |
| 4.5 | `Config::IsFGEnabled()`（菜单里的总开关） | `Mode=local`（我们产品自己的开关，语义等价：只有它决定"要不要生成"；`FgInput/FgOutput` 决定"用哪条车道"） |

## 5. 与 OptiScaler 的差异（只有"抄不了"的三类）

| 类别 | 内容 | 为什么 |
|---|---|---|
| 需要发 DXIL | `DI_Dx12` / `DS_Dx12` / `RUI_Dx12` / `HC_Dx12` / `CM_Dx12` / Hudfix | 它们以预编译 blob 形式在 `shaders/**/precompiled`；我们没有 shader 编译链。注意 `DI` 是**反转数值**、`DS` 是**归一化**（`src / DepthScale`），都不是格式转换 |
| 需要它的限帧器/Reflex | Dynamic MFG（`XeFG_DynamicCount/Cap` + `FrameLimit` + `InputCommon::take_sleep_ms`） | 是子系统，不是单点机制 |
| 需要它的内部对象 | `Activate()` 里 `IsLowResMV()/nativeAA` 来自上采样器特征尺寸 | 同判据（MV 宽 vs 输出宽）等价实现 |
| **架构门机制** | `Streamline_Hooks.cpp:925` `spoofArch()`：在 `slOnPluginLoad` 里拿 `sl::param::SystemCaps`，只对 `kFeatureDLSS_G` 把 adapter 的 `architecture` 改成 `maxArch`（SL2 的 `sl::param` API） | 我们随仓库的 Streamline 头文件是 **1.x**，没有 `sl::param`/`slOnPluginLoad`，所以用 `nvapi_QueryInterface`→`NvAPI_GPU_GetArchInfo` 按调用模块过滤的方式回答（`ArchSpoof`）。**效果相同：只对 Streamline 侧回答 Ada，其他模块照实回答** | 唯一的机制差异，原因是 SDK；升级到 SL2 头文件后应改回 `spoofArch()` |

**OptiScaler 里不存在 D32→R32 的转换**：`Upscaler_Inputs_Dx12` 直接转发上采样器的 R32 深度、
`ValidButMakeCopy` 只是同格式 `CopyResource`、`DS/DI` 是数值处理。因此"标题的 D32 深度 + XeFG"
在两条车道里都不成立 —— CP2077 必须 `FgInput=Upscaler`。

## 6. 不要再做的事（每条都付出过代价）

| 不要 | 后果 |
|---|---|
| 自己 barrier / 转换标题的资源（猜 `StateBefore`） | GPU 挂起，present 从 13 ms 变 4-5 s/次；`COMMON` / `0x80` / `0xC0` 全都不行 |
| 把拷贝放到 present 期的自建命令表 | `-12`（拷贝与 tag 不在同一张表），且 present 期资源已不在那个状态 |
| 用自己的"深度格式转换"补 D32 | OptiScaler 没有这个东西，那是发明 |
| 在 Present 里安装/启停钩子 | `MH_EnableHook` 挂起所有线程 → 死锁或崩 |
| 用两个计数器索引同一张模块表 | 表项被覆盖 → 别人的 handle 进了别人的 Evaluate |
| 不检查 `MH_CreateHook` 返回值 | `MH_ERROR_ALREADY_CREATED` 时槽里留着已挂钩地址 → 自己调自己，栈溢出 |
| 用截断的 vtable 拷贝改后端对象 | 越界崩溃（device vtable 87 项、swapchain ~32 项） |
| 生成开关只看"参数块里有 MV/深度" | 真正要看的是**库里 tag 成功了没有** |
| 建链就 `SetEnabled(1)` | tag 一旦失败 = "不生成也不呈现" = 黑屏 |
| 用源的 heap flags 建自己的纹理 | CP2077 的深度堆只允许 RT/DS，`E_INVALIDARG`；OptiScaler 传 `D3D12_HEAP_FLAG_NONE` |

## 8. 本轮定论（CP2077：DLSS + XeFG 打通，2026-11-12）

按"症状 → 根因 → 依据"记，后续任何游戏遇到同类症状直接查这张表。

| # | 症状 | 根因 | 依据 / 位置 |
|---|---|---|---|
| 1 | 游戏启动崩溃（`ucrtbase+0xEDDF8`） | `GetModuleHandleExW` 在 `FROM_ADDRESS` 模式下参数是**代码地址**，被当字符串跑 `wcsrchr` | `Kernel_Hooks.cpp:239` 只在 `dwFlags == PIN` 时比较名字 |
| 2 | 同上 | `GetProcAddress` 的**序数**调用是小数，被 `strcmp` | `Kernel_Hooks.cpp:115` 先判 `< 0xF000` |
| 3 | 菜单里没有 DLSS | 冒充 NGX 核心 `_nvngx.dll`；OptiScaler 只做 `nvngx_dlssg.dll` | `NgxCoreProxy=0`；`fg_internal.h:65` 自己的注释就写着 breaks DLSS-SR |
| 4 | DLSS 选项消失（菜单） | NVAPI 架构只该骗 Streamline，不能骗游戏本体（真实是 Ampere `0x170`） | `nvapi_arch.cpp::caller_is_streamline_module()` 正名单 |
| 5 | 插出来的帧全黑 | `sl.param.common.gpuInfo` 改了架构**没还原**，漏给后续插件 | `Streamline_Hooks.cpp:962-965/1075` `setArch(currentArch)` |
| 6 | 帧率翻倍但黑屏（真凶） | `slDLSSGSetOptions`/`GetState` 转发给真插件 → 真 DLSS-G 与我们的 XeFG 抢同一窗口 | `Streamline_Hooks.cpp:437-457` 无条件返回 dummy；`FakeSlDlssg=1` |
| 7 | 完全不出帧 | XeFG 只能由 **present 路径**派发（`Dispatch()`），DLSS-G Evaluate 不是驱动源 | `FG_Hooks.cpp:1455` `fg->Present()`；本项目 `present_dispatch()` |
| 8 | present 永不执行（自锁） | 暂停时钟用了只在 tag 时前进的计数器 → `IsPaused()` 恒真 | `IFGFeature.cpp:154` `_frameCount`；改用 `g_present_counter` |
| 9 | `-12 MISMATCH_INPUT_RESOURCES` | 缺**插值区域**：rect 只随 hudless/backbuffer tag 走，本车道两者都没发 | `XeFG_Inputs_Dx12.cpp:549-557`（backbuffer tag 只记 rect）；`Upscaler_Inputs_Dx12.cpp:123` `SetInterpolationRect` |
| 10 | 加载页/过场黑屏 | 上一场景的 MV/depth 被继续使用（`g_sr.valid` 永不复位） | `Upscaler_Inputs_Dx12` 每帧当场 tag；改为 100 ms 新鲜度 + `sr_lane_ready()` |
| 11 | 同 10 | 没有 `UpdateTarget()+Deactivate()` 暂停机制 | `wrapped_swapchain.cpp:1099-1104` → `xefg_resize_notify()` |
| 12 | MV 缩放错 3840 倍 | 把另一条车道的 `multiplyByResolution` 抄进来了 | `XeFG_Dx12.cpp:890` 原样传入 |
| 13 | `Output` 当颜色导致黑 | CP2077 profile 是 `FGDisableHUDFix=true`，该车道不给颜色 | `misc/GameConfigs.cpp:321-328` |
| 14 | 代理链 desc 改 alpha 反而 -12 | 游戏的 desc 原样传入才对 | `backend_xefg.cpp` `xefg_start` 注释 |

**CP2077 已验证配置**：`Mode=local` / `FgInput=Upscaler` / `FgOutput=XeFG` / `FakeSlDlssg=1` /
`NgxCoreProxy=0` / `TagBackbuffer=1` / `TagHudless=0` / `DepthInverted=1` / `MvJittered=0` / `HighResMv=0`。

**判读三行**：`xefg: present dispatch id=N SetPresentId -> OK` 递增、`2X present status framesPresented=2
result=OK`（该字段是"本次提交几帧"，恒为 2 即正常）、`sr: upscaler lane is stale` 只应出现在场景切换处。

**已知未端口**：UI/HUD 合成（`FGDisableHUDFix` 下 OptiScaler 也不做）、每帧槽位多缓冲派发
（`GetDispatchIndex`/`_resourceReady`）、Dynamic MFG、Hudfix/ResTrack（需 DXIL）。


部署（游戏 `bin/x64`）：`dxgi.dll` 与 `nvngx_dlssg.dll` 是同一份 fakerFG，原 DLSS-G 改名
`nvngx_dlssg_real.dll`；**不能同时存在 `version.dll` / `dlssg_sm86`**。ini 关键项：

```ini
Mode=local
FgInput=Upscaler
FgOutput=XeFG
DepthInverted=1
```

判读（一次读到底）：

```bash
grep -aE 'config:|xefg: inputs|sr: upscaler|export hooked|SetEnabled|TagFrameConstants|rejected|present: |Deactivate|projectionMatrix' \
  Cyberpunk2077_fakerFG.log | tail -40
```

| 看到 | 含义 |
|---|---|
| `sr: upscaler lane … depth … fmt=41` | 拿到 R32 深度（Upscaler 车道成立） |
| `sr: … fmt=39` | 上采样器的深度也是 D32 → 两条车道都不成立，只能走 §5 的 DXIL 类手段 |
| `xefg: inputs mv 0x… depth 0x… declared=…` | 实际交给库的声明值（`declared=0` = 标题没声明） |
| `xefg: SetEnabled: true (lowResMV=… nativeAA=…)` | 生成已开 |
| `xefg: XeFG rejected the inputs (…) -> Deactivate()` | 输入集合被拒（`-12` = 格式/尺寸不符） |
| `present: xefg failed … device reason: DEVICE_HUNG` | 某一帧让设备挂了；看它前一条 tag |
| `xefg: can't calculate projectionMatrix` | 相机参数缺失（Upscaler 车道用配置值，正常不出现） |
| 只想确认"有没有插帧" | ini 置 `DebugOnlyGenerated=1`：屏幕上只剩生成的帧 |

## 9. 换到别的游戏时的顺序（照 OptiScaler 走，别自己加判断）

1. **先让 DLSS 菜单正常**：`NgxCoreProxy=0`；确认日志里 `arch: question from <exe> -> honest`、
   `sl: slIsFeatureSupported(feature=0) -> 0`。这一步不通过，后面全是空谈。
2. **确认上采样车道有货**：`sr: upscaler lane … mv fmt=… depth … fmt=…`。
   - `depth fmt=41`（R32）：最好的情况；
   - `depth fmt=39`（D32）：先用 `TagHudless=1` 试；仍不行就是 DXIL 类问题（§5）。
3. **确认插值区域**：`xefg: lane=… tags: … backbuffer yes`。没有它必然 `-12`。
4. **确认派发在 present**：`xefg: present dispatch id=N SetPresentId -> OK` 递增。
5. **确认 2X**：`2X present status framesPresented=2 result=OK`。
6. **场景切换**：`sr: … stale` → `presents without a dispatch - Deactivate()` → 恢复后自动 2X。
7. 输入标志（`DepthInverted/MvJittered/HighResMv`）不用猜：日志 `sr: upscaler create flags 0x…` 会算出并写回 ini，**下一局生效**（与它的 `Config::SaveXeFG()` 一致）。

## 10. 兼容层清单（OptiScaler 的“吃得开”来自这里）

逐项对照，✅ = 已抄，⬜ = 未抄（附原因）。

| 机制 | OptiScaler 位置 | 作用 | 状态 |
|---|---|---|---|
| GameQuirks | `misc/Quirks.h` | 按游戏特性开关行为 | ✅ `Quirk_*` + `game_quirks()` |
| GameConfigs（exe → 默认值/quirks） | `misc/GameConfigs.cpp` | 每个游戏一套预设 | ✅ 内置表（CP2077 + Unreal 引擎检测） |
| 引擎识别 | `Streamline_Hooks.cpp:168-171` | `slInit` 的 `pref.engine` 当权威 | ✅ `eUnreal → ForceUnrealEngine + NoFSRFGFirstSwapchain` |
| 首条交换链跳过 | `DxgiFactory_Hooks.cpp:829-856` | UE 的一次性链不接 | ✅ `NoFSRFGFirstSwapchain` |
| 覆盖层链识别 | `Util::IsOverlaySwapchain` | 小窗口链不接 | ✅ 100 px + 客户区回退 |
| 单窗口约束 | `FG_Hooks.cpp:177-186,312` | 第二个窗口用自己链 | ✅ `g_owner_hwnd` 判定（放行而非 E_NOINTERFACE） |
| DX12 flip 升级 | `FG_Hooks.cpp:485-494` | DISCARD/SEQUENTIAL → FLIP | ✅ |
| 自备厂商 DLL 目录 | `Streamline_Proxy.h:88-105` | 与游戏布局解耦 | ✅ `fakerFG\streamline\` + `fakerFG\libxess_fg/libxell` |
| 全局钩子单实例 | 代理实例持有 | 防两实例互相打钩 | ✅ `g_exports_only` |
| 批量安装（事务） | `DetourTransaction*` | 少挂起线程 | ✅ `MH_EnableHook(MH_ALL_HOOKS)` |
| 加载即挂钩 | `LibraryLoad_Hooks` | 不轮询 | ✅ 模块钩子置事件 + 装载时挂 SR 导出 |
| SL 门（supported/loaded/req/version） | `Streamline_Hooks.cpp:315-354` | 菜单可见性 | ✅ |
| DLSS-G 选项接管 | `Streamline_Hooks.cpp:437-457` | 真插件不启动 | ✅ `FakeSlDlssg=1` |
| 架构门 + 还原 | `:925/:962-965/:1055-1076` | 让 DLSS-G 可选 | ✅ |
| XeFG 参数/desc/flags | `XeFG_Dx12.cpp:296-390`、`IFeature.cpp:109-116` | 初始化对齐 | ✅（flags 自动学习并写回 ini） |
| 输入 tag / Dispatch / present 顺序 | `Upscaler_Inputs_Dx12`、`XeFG_Dx12::Dispatch` | 正确喂数据 | ✅ |
| 输入 tag 的位置（Evaluate 内、列表 OPEN → `ValidNow`） | `Upscaler_Inputs_Dx12.cpp` `UpscaleStart` / `UpscaleEnd` | 合成才有内容（present 期只能 `UntilPresent`） | ✅ `TagAtEvaluate`（默认关，见 §11） |
| 输入就绪按帧判定 | `Present()` 的 `>3` | 不误关生成 | ✅ 帧计数 + 3 帧容差 |
| resize 暂停/重激活 | `wrapped_swapchain.cpp:1099`、`IFGFeature.cpp:154,300` | 场景切换不黑 | ✅ |
| 空 resize 不转给库 | `WrappedIDXGISwapChain4::ResizeBuffers` 的 `WaitForGPUIdle` 语义 | 少一次 4K 重建 | ✅ |
| 退出不清理 | `dllmain.cpp:2010-2045` | 避免退出崩溃 | ✅ |
| 签名 DLL 替换（`CreateFileW`/`GetFileAttributesW` 重定向 `nvngx.dll`） | `Kernel_Hooks.cpp:284-330` | 游戏缺 NGX 时补上 | ⬜ 需要一份签名 DLL，且只在游戏找不到时触发 |
| DXGI/NVAPI 适配器欺骗（`DxgiSpoofing`、`fakenvapi`） | `DxgiFactory_Hooks`、`NvApiHooks` | 让非 N 卡/非支持卡也能开菜单 | ⬜ 我们的目标机是 N 卡；架构门只对 Streamline 做已足够 |
| `FGPreserveSwapChain` 全量 / `ReleaseUnusableFGSwapchain` | `FG_Hooks.cpp:188-212,509-529` | 换链时等 GPU 空闲并释放旧链 | ⬜ 当前仅在“同窗口重建”时 ResizeBuffers 复用 |
| 3 槽环形缓冲（`_resourceReady[fIndex]`/`GetDispatchIndex`） | `IFGFeature.cpp:110-145` | 多帧提前提交的流水线 | ⬜ 单槽在 CP2077/Wuchang 已验证可用 |
| Hudfix / ResTrack / RenderUI / CameraMotion | `hudfix/`、`resource_tracking/`、`shaders/` | HUD 合成与捕获 | ⬜ 需要 DXIL 预编译着色器 |
| ForceHDR / OverrideVsync 的 desc 改写 | `DxgiFactory_Hooks.cpp:487-497,917-935` | HDR/垂直同步兼容 | ⬜ 默认关闭，未启用 |

## 11. tag 的位置：搬进上采样器的 Evaluate（`TagAtEvaluate`，默认关）

**症状**：`2X framesPresented=2 fgEnabled=1 result=OK srFresh=1` + 全黑 = 合成没有内容。

**根因**：时序位置。OptiScaler 在**上采样器 Evaluate 内部**打 tag，present 期只发
`TagFrameConstants + SetPresentId`：

```cpp
// inputs/FG/Upscaler_Inputs_Dx12.cpp  UpscaleStart
res.cmdList  = InCmdList;                      // 此刻列表是 OPEN 的
res.validity = FG_ResourceValidity::ValidNow;
fg->SetResource(&res);
```

我们在 present 期打 tag，UE5 已经 close/execute 过那张列表：传列表 → 闪退（实测）；
不传 → 只能 `UntilPresent` → 合成没有内容 → 黑屏。

**实现（`TagAtEvaluate=1`）**：

```
thunk_sr_evaluate / thunk_evaluate
  └─ forward_upscaler_evaluate()                 proxy_nvngx.cpp
       ├─ sr_note_command_list(list)             列表还开着
       ├─ sr_harvest(params)                     MV/深度/颜色/jitter/MV scale
       ├─ xefg_tag_at_evaluate(list, Inputs)     ← UpscaleStart：MV/深度(+rect/backbuffer)
       ├─ 转发真 Evaluate
       └─ xefg_tag_at_evaluate(list, Color)      ← UpscaleEnd：hudless/UI
present_dispatch()
  └─ 只发 tag_frame_constants + SetPresentId（同一个 id，不再 tag）
```

要点（括号里是实现位置，`src/faker_fg/`）：

1. 帧 id 在 evaluate 侧生成（`g_eval_tag_counter` → `g_eval_frame_id`），present 原样复用；
2. `>3` 计数在 evaluate 侧刷新（`tag_begin()`，与 `xefg_tag` 共用）；present 只消费
   `g_eval_tag_fresh`，一个 present 间隔最多一个 tag，UE5 多 view 不会给 XeFG 两个 id；
3. **颜色必须在上采样器写完之后 tag**：`TagHudless=1` 时颜色就是
   `NVSDK_NGX_Parameter_Output`，`ValidNow` 会在列表当前位置拷贝它——转发前 tag 拷到的是
   上一帧。所以上采样器输出走 `UpscaleEnd`（`tag_color()`），MV/深度走 `UpscaleStart`；
4. resize/pause 清掉未消费的 tag（`xefg_pause()`），避免暂停后把旧资源的帧配给新的 present；
5. 只对 `FgInput=Upscaler` 生效（该车道的 Evaluate 才带 MV/深度），`DLSSG` 仍走 present 路径；
   默认 `0`：已验证的 present 路径完全不变（`xefg_tag()` = `tag_begin + tag_inputs + tag_color`）。

**怎么判读（一轮就能定位）**：

| 日志 | 含义 |
|---|---|
| `config: … TagAtEvaluate=1` | 开关生效 |
| `xefg: evaluate-side tag id=N list=0x… valid-now mv=1 depth=1` 递增 | 列表开着时已 tag（`mv=0/depth=0` = 那一类 tag 被库拒） |
| `xefg: present dispatch id=N SetPresentId -> OK [tagged at evaluate]` 递增 | present 复用了同一个 id |
| 仍然黑且 `present dispatch` 不递增 | evaluate 侧 `sr_apply()` 为假（tag 未发生 / 输入不新鲜） |
| `-12` / `-14` | 输入集合仍不完整：先看 `xefg: lane=… tags: … backbuffer`（rect 随 backbuffer/hudless 走） |

若回填后出现 `-12`/`-14`，下一步不是改 present，而是确认 hudless/rect 与 MV/深度在同一
位置发出（本实现里 `tag_color()` 与 `tag_inputs()` 只差“转发前/后”，同在一个 Evaluate）。

## 12. Wuchang 实测：全屏、尺寸、颜色（2026-11-13）

实机日志（`Project_Plague-Win64-Shipping_fakerFG.log`），按发现顺序：

### 12.1 独占全屏 = XeFG 拒绝插帧

```
[00:40:52.231] xefg[debug] SetFullscreenState called to set fullscreen mode.
                          Interpolation is not supported in fullscreen mode.
[00:40:53.994] xefg[error] Interpolation is not supported in fullscreen mode.
                          Interpolation won't be enabled.
```

处理：`ForceBorderless=1`（`fg_swapchain.h` 吞掉 `SetFullscreenState(TRUE)`，下一个 present 前
改成无边框，`GetFullscreenState` 仍回 TRUE）。实测生效：`xefg: borderless emulation applied
(3840x2160 at 0,0)`，之后不再出现该错误。

### 12.2 输入声明：无条件照抄 OptiScaler

| 位置 | OptiScaler | 本实现 |
|---|---|---|
| velocity 尺寸 | `LowResMV ? RenderWidth/Height : TargetWidth/Height`（`Upscaler_Inputs_Dx12.cpp:167-177`） | `high_res_mv == 0 ? render : target`（`tag_inputs`） |
| depth 尺寸 | `RenderWidth/Height`（`:219-221`） | `render` |
| 尺寸来源 | NGX 参数块 `Width/Height/OutWidth/OutHeight`，render=min、target=max（`IFeature.cpp:120-165`） | `sr_harvest()` 同算法，日志 `sr: upscaler lane render=… target=…` |
| 颜色 | 只有 `FGHUDFix` 打开才在 UpscaleEnd 用 `NVSDK_NGX_Parameter_Output`（默认 false，`Config.h:671`） | 上采样车道**不发颜色**（`tag_color` 直接返回） |
| backbuffer tag | 只在 `activeFgInput != Upscaler` 时发（`XeFG_Dx12.cpp:945`） | 上采样车道**不发** |
| tag 位置 | `SetResource` 内部、上采样器 Evaluate 的列表上，`ValidNow` | `xefg_tag_at_evaluate(EvalTag_Inputs)` |
| **启用门控** | `Activate()`：`!_isActive && (IsLowResMV() \|\| nativeAA \|\| ForceFGRenderSizeMVs \|\| IgnoreInitChecks)`，`nativeAA = RenderWidth()==DisplayWidth()`，`IsLowResMV = !DisplayResolutionMVs = LowResMV`（`XeFG_Dx12.cpp:583-610`、`IFGFeature.cpp:158`、`Upscaler_Inputs.cpp:66`） | `xefg_frame_feedback()`：`low_res_mv = (HighResMv==0)`、`native_aa = (render_w==target_w)` |
| 打 tag 的前置门 | 只有 `fg->IsActive()` 才喂资源（`Upscaler_Inputs_Dx12.cpp:129`） | `xefg_tag_at_evaluate` 先 `xefg_frame_feedback(true)`，`!g_generate` 直接返回 |
| 失败处理 | `UpdateTarget(); Deactivate();`（`:931-940`、`:1639-1647`） | `xefg_pause(10)` |

### 12.3 硬限制：XeFG 1.3.1 没有高分辨率 MV

```c
/** Deprecated. Setting this flag has no effect. */
XEFG_SWAPCHAIN_INIT_FLAG_HIGH_RES_MV = 1 << 2,   // xefg_swapchain.h:91
```

Wuchang 在 1440p 渲染 / 4K 输出时给 DLSS-SR 的是 **MV 3840x2160 + depth 2560x1440**
（create flags `0x69`，`MVLowRes` 未置位）。无条件照抄后库必然回：

```
xefg[error] XeFG: Invalid argument. motion vector and depth resource resolutions must match.
2X present status framesPresented=1 fgEnabled=1 result=MISMATCH_INPUT_RESOURCES (-12)
```

这不是实现 bug：OptiScaler 在同一输入下给的是同一对尺寸，`Dispatch()` 会
`UpdateTarget + Deactivate` —— **它在这条路径上也插不了帧**（Wuchang 在它的 `GameConfigs.cpp:418-422`
只有 `SkipFirstFrames = 10` 的崩溃规避，没有可用 FG 配方）。

能工作的组合只有 **render == display**（DLAA / 原生）：此时声明尺寸 = 资源尺寸 = 同一分辨率。
若要支持升频档，需要自己做一次 MV 或 depth 的重采样（OptiScaler 没有这一步）——
仓库里已有可复用的降采样 DXBC（`src/mgpu_ds_shaders.h`，`temp/ds_shader.hlsl` 可重生成）。

### 12.4 启用门控（本轮修正的黑屏原因）

`XeFG_Dx12::Activate()` 的判据**不是**“MV 尺寸 == 输出尺寸”：

```cpp
bool nativeAA = currentFeature->RenderWidth() == currentFeature->DisplayWidth();
if (!_isActive && (IsLowResMV() || nativeAA || ForceFGRenderSizeMVs || IgnoreInitChecks))
    SetEnabled(true);
// IsLowResMV() = !_constants.flags[DisplayResolutionMVs] = feature->LowResMV()
```

Wuchang 升频档：`HighResMv=1` → `IsLowResMV()=false`；`RenderWidth(2558) != DisplayWidth(3840)`
→ `nativeAA=false` → **OptiScaler 从不调用 `SetEnabled(true)`**，FG 一直 1X，画面正常。

我们之前用自己发明的 `mv.width == output.width`（4K == 4K）判成 nativeAA，把 XeFG 打开了——
输入随后被库以 `-4/-12` 拒绝，而 XeFG 已接管后缓冲合成 → **整屏黑 + 游戏内仍显示 2X**。
这就是一直复现的“黑屏 + 帧数 x2”的直接原因。

同一处还有第二个门：`Upscaler_Inputs_Dx12.cpp:129` 的 tag 块有 `fg->IsActive()` 前置，
未激活时不喂任何资源；我们补上了（`xefg_tag_at_evaluate` 先做 Activate 判定，`!g_generate` 直接返回）。

游戏自己的 FG 开关（`slDLSSGSetOptions`）：OptiScaler **默认不跟随**，只有 `FGFollowsGame=1` 时才
置 `state.fgChanged`（→ Deactivate + UpdateTarget）。我们也只记录（`g_stage.game_enabled`）不启用。


### 12.5 HDR：Wuchang 自己的输出路径（本轮定位）

现象：`Present -> 0x0`、`GetBuffer` 正常、`SetColorSpace1(12)` 成功、后缓冲内容读回是
“清屏色 + 少量 UI 像素”，屏幕全黑；但 **Win+Tab 的任务视图缩略图有画面**。

配置证据（`%LOCALAPPDATA%\Project_Plague\Saved\Config\Windows\GameUserSettings.ini`）：

```ini
bUseHDRDisplayOutput=False      ; 引擎设置是关
HDRDisplayOutputNits=1000
```

但交换链是 `fmt=24`（R10G10B10A2）、`SetColorSpace1(12)`（RGB_FULL_G2084_NONE_P2020 = PQ/HDR10）、
DLSS create flags 里 `HDR=1` —— **游戏用自己的 HDR 实现，绕过了引擎开关**。
在**独占全屏**下正常；一旦走 **DWM 合成**（XeFG 只能用窗口/无边框），实机扫描输出就是黑的，
而 DWM 缩略图（SDR 转换）仍有画面。

OptiScaler 的 `ForceHDR` **不适用**：`UseForceHDR()` 要求 `currentFGSwapchain == nullptr`，
它做的是把 **SDR 游戏**强制成 HDR 输出（`R10G10B10A2`/PQ 或 `R16G16B16A16`/scRGB）。

结论/处理：**开 FG 时需要在游戏内关闭 HDR**（SDR 链 + SDR 色彩空间，DWM 合成正常）。
反向 ForceHDR（把链强制成 8-bit SDR）试做后已按要求移除，不进入默认路径。

### 12.6 交换链创建：OptiScaler 的 `pApplicationSwapChain` 路线（**本 DLL 不适用**）

`XeFG_Inputs_Dx12.cpp:370-392` 的做法是**由应用侧按游戏 desc 建链**，再交给库：

```cpp
factory->CreateSwapChainForHwnd(queue, hwnd, desc, fullscreenDesc, nullptr, &swapChain);
params.pApplicationSwapChain = swapChain;
InitFromSwapChain(handle, queue, &params);
```

本实现照此改过，实测（libxess_fg 1.3.1）**行不通**：

- `InitFromSwapChain` 期间库**又建了一条自己的链**（日志里的嵌套 `count=3`），
  `D3D12GetSwapChainPtr` 返回的是它自己的对象（`(app chain …, different)`）；
- 我们传进去的那条链随后被库**释放**：把 app 链交给游戏后立刻
  `0xC0000005`，`rip = <app chain 地址>`；把它 Release（成功路径）同样会崩
  （`rip=0`，库仍在用）。
- OptiScaler 能这么做，是因为它的 XeFG 代理自己实现了 `InitFromSwapChain`
  （`_swapChain = params->pApplicationSwapChain`，`GetSwapChainPtr` 返回它），
  并不把该链转交给真实的 libxess。

结论：**保持 `D3D12InitFromSwapChainDesc`（库自己建链）**，这是该 DLL 唯一支持的路线；
不要再把应用链交给游戏，也不要在成功路径释放它。

### 12.7 判读

| 日志 | 含义 |
|---|---|
| `xefg[debug] SetFullscreenState called …` | 标题要独占全屏；`ForceBorderless=0` 必然被库拒绝 |
| `xefg: borderless emulation applied (WxH at X,Y)` | 无边框伪装生效 |
| `sr: upscaler lane render=2560x1440 target=3840x2160 …` | 参数块给出的 render/target（OptiScaler 同源） |
| `xefg: FG stays off - high-res MV (HighResMv=1) and render … != display …` | **门控生效**：与 OptiScaler 一致，不启用 FG（升频档的正常结果） |
| `xefg: SetEnabled: true (lowResMV=… nativeAA=…)` | 只有低分辨率 MV 或 render==display 才会出现这一行 |
| `xefg: tag sizes Upscaler: mv … depth …` | 声明值（升频档若仍被激活 → 库报 mismatch） |
| `result=MISMATCH_INPUT_RESOURCES (-12)` + `xefg: paused …` | 与 OptiScaler 一致：放弃插帧，保持 1X |
| `xefg: GetBuffer(0) -> hr=0x0 res=0x…` | 标题确实渲染进 XeFG 的链（没有这行 = 1X 也会黑，与 FG 输入无关） |

## 13. 与 OptiScaler 的接口对齐审计（2026-11-13）

逐项核对 `src/faker_fg` 实际调用的 XeFG API 与 `XeFG_Dx12.cpp` / `XeFG_Inputs_Dx12.cpp`：

| 我们调用 | OptiScaler | 备注 |
|---|---|---|
| `D3D12CreateContext` / `Destroy` | ✅ | — |
| `D3D12InitFromSwapChainDesc` / `D3D12GetSwapChainPtr` | ✅ | `pApplicationSwapChain` 在本 DLL 不可用（§12.6） |
| `GetProperties` / `GetVersion` / `SetLoggingCallback` | ✅ | — |
| `SetLatencyReduction`（XeLL，Init 前） | ✅ | 缺它会 `-15` |
| `TagFrameConstants` → `SetPresentId` | ✅ | 顺序一致，均在 present 期 |
| `D3D12TagFrameResource`（MV、depth） | ✅ | 在 Evaluate 内、列表 OPEN、`ValidNow` |
| `SetEnabled` / `SetNumInterpolatedFrames` | ✅ | Activate 时应用 count |
| `SetSceneChangeThreshold` / `EnableDebugFeature` | ✅ | 初始化一次 |
| `GetLastPresentStatus` | ✅ | 仅诊断 |
| — | `SetUiCompositionState` | 仅 hudfix/UI 合成需要；本车道不打颜色，不需要 |

**未端口、且不影响本标题判定的内部机制**（理由）：

| 机制 | 位置 | 理由 |
|---|---|---|
| 3 槽环形缓冲 `GetDispatchIndex`/`_resourceReady` | `IFGFeature_Dx12.cpp:110-145` | 单槽语义等价；CP2077 已验证 |
| `StartNewFrame()` 的 `_frameCount` 推进 | `IFGFeature.cpp:11-36` | 我们的 id 在打 tag 时推进、present 复用 |
| Hudfix / RenderUI / UI unblend | `hudfix/`、`shaders/`（DXIL） | CP2077 profile 同为 `FGDisableHUDFix=true` |
| `ValidButMakeCopy`、depth 翻转/R32 转换 | `XeFG_Dx12::SetResource` | 本车道按 title 状态原样 tag（与 CP2077 一致） |
| `FGUseMutexForSwapchain` | `FGUseMutexForSwapchain` | 单线程 present 已稳定 |

结论：**API 面已完全对齐**；Wuchang 的黑屏不是接口差异，而是该标题的内容没有落到库建的后缓冲里
（同一进程、同一窗口，游戏自己的链有画面，库建的链只有清屏色 + 少量 UI）。




## 14. OptiScaler 全局架构对照（2026-11-13）

对照组：`Ownswapchain=0`（游戏自己的链）**有画面**，`=1`（XeFG 的链）**黑屏**。
两组的 NGX 拦截、DLSS-SR 转发、tag 逻辑、色彩空间、Present 返回码完全相同，
**唯一变量是链的身份**。因此“输入/兼容/钩子”层的缺口都已被这个对照排除。

| 层 | OptiScaler | 本实现 | 与 Wuchang 黑屏 |
|---|---|---|---|
| 代理与模块扫描 `proxies/` `scanner/` `library/` | dxgi/nvngx/streamline/kernel | ✅ | 无 |
| DXGI 工厂钩子 `hooks/DxgiFactory_Hooks.cpp` | 建链 → 包 `WrappedIDXGISwapChain4` → FG 链 | ✅ 工厂钩子 + 自有 facade | 无（等价） |
| 交换链包装 `wrapped/wrapped_swapchain.cpp` | ~1700 行：Present/Present1、Resize 过滤、HDR、tearing、最大帧延迟、缓冲捕获 | ⚠️ 只做透明转发 | **唯一仍未排除的层** |
| FG present 调度 `hooks/FG_Hooks.cpp`（LocalPresent） | `fg->Present()` + mutex + 帧时间 | ✅ `before_present` 等价 | 低 |
| D3D12 命令列表钩子 `hooks/D3D12_Hooks.cpp` | `Reset/Close/SetPipelineState/SetDescriptorHeaps/Set*Root*` 早/晚钩子、`HookToCommandListLate`、根签名恢复 | ❌ | 逻辑上已排除（对照组同一条 NGX 路径） |
| NGX 输入钩子 `inputs/NVNGX_DLSS_Dx12.cpp` | `SkipFirstFrames`（Wuchang=10）、恢复签名 | ⚠️ 仅转发 | 逻辑上已排除（同上） |
| 资源追踪 `resource_tracking/` | 状态/尺寸表（配 hudfix） | ❌ | 低（本车道原样 tag） |
| Hudfix `hudfix/`、着色器 `shaders/` | HUD 合成、depth/format transfer（DXIL） | ❌ | 低（CP2077 同 profile） |
| IFGFeature 环形缓冲 | 3 槽 | ❌ 单槽 | 低（CP2077 已验证） |
| `gpu_time/` `low_latency/` | 动态 MFG / Reflex | ⚠️ 只用 XeLL（Init 必需） | 低 |
| 欺骗 `spoofing/` `nvapi/` | 适配器/架构 | ✅ 仅 Streamline 架构门 | 无 |
| 菜单 `menu/` | ImGui 覆盖层 | ❌ | 无 |

**结论**：能解释“只有链不同就黑”的只剩 `wrapped_swapchain.cpp` 里与链生命周期/呈现语义相关的行为，
以及真实 libxess 自建链与 DXGI 自建链在驱动/DWM 层的差异（这部分 OptiScaler 源码里没有答案，
因为它自己的链也是走 `CreateSwapChainForHwnd` 建的）。

**若要继续，优先按顺序试**：
1. `wrapped_swapchain` 的细节对齐：`Present1` 的 dirty rect、`ResizeBuffers` 的 `BufferCount/Format/Flags` 过滤、
   `SetMaximumFrameLatency`、`CheckForHdrOutput`/`SetHDRMetaData`；
2. `FGDontUseSwapchainBuffers=1`（OptiScaler 的“不使用交换链缓冲”模式）；
3. `SkipFirstFrames`（Wuchang profile 的 10 帧）作为低成本对照；
4. 换一份 libxess_fg，验证 `pApplicationSwapChain` 是否真被采用。

### 12.8 已知稳定性问题：上采样车道首帧的 `cfg()` 栈溢出

在 `OwnSwapchain=1` 下（窗口模式与全屏模式各一次）出现 `0xC00000FD`（STACK_OVERFLOW）：

```
crash: first-chance code=0xC00000FD rip = dxgi.dll+0xA905
map:   ?cfg@fakerfg@@YAAEAUConfig@1@XZ @ 0x180009ad0 -> cfg+0xE35
objdump: cfg+0xE35: call __security_check_cookie   （栈已耗尽，压返回地址撞保护页）
栈里反复出现 cfg 的返回地址（0x…A90A） -> 递归
```

触发时机固定：**上采样车道第一次 harvest**（`sr_harvest` → `cfg_save_upscaler_flags` →
`cfg()` → 末尾 `logf`）。同一个 build 在 CP2077 上不出现。

**未解决**，因此 Wuchang 的 `OwnSwapchain=1` 既黑屏也不稳定；当前交付状态是
`OwnSwapchain=0`（可玩、原生帧数、无 FG）。下一步若要继续，需要带调试器复现（不是日志能定位的）。

## 15. `ForceBorderless` 逐条对照（2026-11-13，本轮补漏）

OptiScaler 的这条开关不是一处，而是**四层**，缺任何一层都会让 XeFG 自己的链被切成全屏，
而 libxess 一旦认为自己是全屏就**永久拒绝插帧**：

```
xefg[debug] SetFullscreenState called to set fullscreen mode. Interpolation is not supported in fullscreen mode.
xefg[error] Interpolation is not supported in fullscreen mode. Interpolation won't be enabled.
xefg: 2X present status framesPresented=1 (was 1) fgEnabled=0 result=OK
```

`fgEnabled=0` 而标题按 2X 呈现 ⇒ 隔帧没有内容 ⇒ **黑屏 + 2X**。这就是 Wuchang 这一幕。

四层与我们的落点：

| OptiScaler | 位置 | 迁移状态 |
|---|---|---|
| 创建期把 desc 改成 windowed（`Windowed=true`、去掉 `ALLOW_MODE_SWITCH`、`Scaling=STRETCH`），并记下 `SCExclusiveFullscreenRequested = !localDesc.Windowed` | `DxgiFactory_Hooks.cpp:487-500 / 905-930` | **漏了两处**：desc 改写了，但「标题在创建时就要求全屏」没记录 ⇒ 不调 `SetFullscreenState` 的标题永远不会进入模拟 |
| `SetFGSwapchain`：`SCExclusiveFullscreen = UseBorderless && Requested`，`_borderlessPending = UseBorderless()` | `FG_Hooks.cpp:572-573` | **漏**：模拟只在 `SetFullscreenState(TRUE)` 时武装 |
| 门面覆盖：`SetFullscreenState` / `GetFullscreenState` / `GetFullscreenDesc`（强制 `Windowed=false`）/ `ResizeTarget`（模拟期间直接 `S_OK`，不转发）/ `ResizeBuffers`（成功后 re-arm） | `FG_Hooks.cpp:700-760 / 787-812 / 1013-1030` | **漏三处**：`GetFullscreenDesc` 原样转发（标题看到 windowed）、`ResizeTarget` 转发后再摆窗口（把无边框矩形打掉）、`ResizeBuffers` 不 re-arm（按 §12 的 5fps 抖动结论保留） |
| 窗口动作：`ApplyBorderless(target)` 用**请求的那个 output 的显示器**、`SetWindowPos(..., SWP_FRAMECHANGED \| SWP_SHOWWINDOW)`；`RestoreWindow` 无保存矩形时用 `WS_OVERLAPPEDWINDOW + AdjustWindowRectEx` 重建 | `FG_Hooks.cpp:222-300` | **漏三处**：只用 `MonitorFromWindow`、缺 `SWP_SHOWWINDOW`（标题切模式时隐藏过窗口就再也显不出来）、缺无保存矩形时的重建 |

本轮已全部按原文补齐（`fg_swapchain.h` / `proxy_dxgi.cpp` / `xefg_start` 增加 `title_fullscreen` 入参），
并额外加了一条**兜底**：`apply_borderless()` 时若真实链 `GetFullscreenState()` 仍报全屏，
直接 `SetFullscreenState(FALSE)` 把它按回窗口模式——libxess 的「全屏」判断是粘性的，
标题若绕过门面动过真实链，只有这一句能救回来。

新增诊断行（只在模式切换时打印，不刷屏）：

```
xefg: facade created (borderless=1, title asked fullscreen=0)
xefg: SetFullscreenState(1, target=…) borderless=1 requested=0
xefg: backend chain was in fullscreen - forcing windowed for XeFG
xefg: borderless emulation applied (3840x2160 at 0,0)
```

若日志里出现 `SetFullscreenState(1, …)` 但没有紧跟 `borderless emulation applied`，
说明是 `IsIconic()`（最小化）把它推迟了；若根本没有 `SetFullscreenState(1, …)` 却有
libxess 的 `SetFullscreenState called…`，说明调用压根没经过门面（标题持有别的链对象）。

## 16. 黑屏的第二种形态：把画面交给了「不会生成的 provider」（2026-11-13）

Wuchang + DLSS（render 2558x1439 → 3840x2160）这一轮的日志：

```
03:23:59.307 provider: handle slot 0, mode=local interpolated=1 - CreateFeature is ours
03:23:59.543 sr: upscaler create flags 0x69 ... HighResMv=1
03:23:59.543 xefg: FG stays off - high-res MV ... Activate() needs low-res MV or native AA
03:24:00.120 xefg: 1X present status framesPresented=1 fgEnabled=0 result=OK
```

**零错误、`result=OK`、真 1X 呈递，画面还是黑的**——所以黑屏与全屏、与 ForceBorderless、
与 XeFG 的 `Present` 都无关。原因是上一层契约：我们把 NGX 的
`FrameGeneration.Available` 无条件改成 1（`proxy_nvngx.cpp` thunk_caps），标题因此进入
「帧生成已启用」状态——在 FG 模式下标题只把 clear + UI 写进交换链，把场景
downsample 后的 hudless + UI 交给 provider 合成。provider 是 XeFG，而 XeFG 在这里被
`Activate()` 的同一道门拒绝（高分辨率 MV + 缩放），于是没人合成 → 只剩 UI。

处置（`provider.cpp` / `proxy_nvngx.cpp` / `backend_xefg.cpp`）：

1. 门拒绝满 30 帧后调用 `provider_block_fg()`，一次性、不可逆；
2. `nvngx_withdraw_frame_generation()` 把真实参数块上的 `FrameGeneration.Available` /
   `FrameInterpolation.Available` / `FeatureInitResult` 改回 0；
3. `provider_owns_output()` / `provider_answers()` 转为 false ⇒ 后续 DLSS-G `CreateFeature`
   落到真实运行时（非 RTX 上必然失败），`provider_evaluate()` 返回失败 ⇒ 标题关掉自己的 FG；
4. 标题回到正常合成 → 1X 真实画面（与 `OwnSwapchain=0` 的观感一致）。

判读：下一轮日志应出现

```
provider: frame generation withdrawn (high-res MV with render != display) - the title keeps its own picture at 1X
nvngx: capability block withdrawn - FrameGeneration.Available=0
```

若之后标题仍报 x2 且仍黑，说明它把「FG 可用」在启动时缓存了，必须从
`thunk_requirements` / `thunk_caps` 的第一次回答就为 0（即按标题/分辨率在启动时判定）。

## 17. MV 到底是不是读错了：全分辨率 MV 与 OptiScaler 的「光流」核查（2026-11-13）

### 17.1 事实（用 `dxgiformat.h` 与 NGX 头核对过）

Wuchang（`03:23:59.543`）：

```
sr: upscaler create flags 0x69 -> DepthInverted=1 MvJittered=0 HighResMv=1 (HDR=1)
sr: upscaler lane render=2558x1439 target=3840x2160 mv 3840x2160 fmt=34 depth 2560x1440 fmt=19
```

| 项 | 值 | 结论 |
|---|---|---|
| `NVSDK_NGX_DLSS_Feature_Flags` | `IsHDR=0x1 MVLowRes=0x2 MVJittered=0x4 DepthInverted=0x8 Reserved_0=0x10 DoSharpening=0x20 AutoExposure=0x40` | `0x69 = IsHDR\|DepthInverted\|DoSharpening\|AutoExposure`，**`MVLowRes` 未置位** ⇒ 标题自己声明「MV 是全分辨率」 |
| MV 资源 | `DXGI_FORMAT_R16G16_FLOAT = 34`，**3840x2160 = 输出分辨率** | 与上一条一致，不是读错纹理 |
| depth | `R32G8X24_TYPELESS = 19`，2560x1440（`render 2558x1439` 的 8 像素对齐） | 渲染分辨率 |
| 对照 CP2077 | `flags 0x4B` **含 `MVLowRes(0x2)`**，`render==target==3840x2160`（DLAA） | 低分辨率 MV 的正面样本 |

所以 MV 不是可疑对象：它确实是全分辨率像素位移（单位 = 输出像素）。XeFG 1.3.1 要求
MV 与 depth 同尺寸，而 `HIGH_RES_MV` 初始化标志已被移除 —— 与 OptiScaler 遇到的限制完全相同。

### 17.2 OptiScaler 里没有「MV 缺失就用光流补」的机制

只有三处相邻的东西，逐条排除：

1. **`GameQuirk::XeFGCameraMotionFill`（唯一 XeFG 相关项，The Witcher 3 专用 quirk）**
   `inputs/FG/XeFG_Inputs_Dx12.cpp:232-292 FillVelocity()` → `shaders/camera_motion/CM_Dx12.cpp`。
   它做的是「用 depth + 上一帧 view-projection 重投影出相机运动，写进 velocity 里
   **alpha==0（纯相机位移）的像素**」，输出缓冲由 `CreateBufferResource(..., InVelocity, ...,
   DXGI_FORMAT_R16G16_FLOAT)` 从输入 velocity 复制 desc ⇒ **输出尺寸 = 输入尺寸**。
   `depth.resource == nullptr || velocity.resource == nullptr` 时直接 `return false`
   （`:237-239`），即 **没有 velocity 就没有任何补救**。
2. **`XEFG_SWAPCHAIN_INIT_FLAG_USE_NDC_VELOCITY`**：只是单位换算（NDC → 像素，
   `mvScale *= size * 0.5`），不是补偿。
3. **FSR3.1 SDK 的 `FfxOpticalFlow`**（`include/fsr31/ffx_opticalflow.h`、
   `ffx_frameinterpolation.h` 的 `opticalFlowVector` / `opticalFlowSceneChangeDetection`）：
   属于 FSR-FG 自己的插帧通路，OptiScaler 从不用它给 XeFG 供数据。

结论：**OptiScaler 对 Wuchang 这种全分辨率 MV + 缩放的情况只会拒绝激活**（`Activate()` 的
`IsLowResMV() || nativeAA` 不成立），我们之前的行为与它一致。

### 17.3 我们的补充：`MvFromTarget`

OptiScaler 不会做的事可以自己做：XeFG 只校验「MV 与 depth 同尺寸」，而采样是按 UV 归一化的。
于是把 MV **按 depth 的尺寸声明**、向量值乘 `render/output`（Wuchang ≈ 0.6667），并按低分辨率 MV
走 `Activate()` 门（`low_res_mv = HighResMv==0 || MvFromTarget`，同时不再置
`XEFG_SWAPCHAIN_INIT_FLAG_HIGH_RES_MV`）。资源本身不动。

开关：`MvFromTarget=0/1`（默认 0）。日志：

```
sr: MvFromTarget - MV 3840x2160 declared as 2560x1440, scale 0.6667x0.6667
xefg: tag sizes Upscaler: mv 2558x1439 (resource 3840x2160) depth 2558x1439 target 3840x2160
xefg: SetEnabled: true (lowResMV=1 nativeAA=0)
```

若库按整数像素坐标采样（`Load()`）而不是归一化 UV，则声明变小只会读到左上角区域，
画面会出现整体错位/拖影 —— 那就是必须补一个真正的重采样 compute pass（把 3840x2160 →
2560x1440 并缩放向量值）才能成立。

### 17.4 双链假设被证伪（2026-11-13 晚，第二轮实测）

`create#` 日志显示同一 hwnd 出现两次（`03:44:53.519` / `03:44:53.643`），但**第二条不是标题的第二个视口**：
它是 libxess 在 `D3D12InitFromSwapChainDesc` 内部自己调的 `CreateSwapChainForHwnd`，撞上 `g_starting`
是正常且必须 pass-through 的（它才是真正建链的那一次）。证据：当时给线程 B 加的「等待第一个链」
把这次内部创建挡住了 20 s，`D3D12InitFromSwapChainDesc` 从 2984 ms 变成 **22140 ms**，
`waited 21875 ms ... passing through` —— 等待的是它自己，纯死等。

因此：

* **只有一条链**，双链假设作废，相关的等待代码已回退为 `return false`（并在日志里写明是嵌套创建）；
* `g_starting`/`g_chain_ready` 保留 `std::atomic<bool>`（跨线程读取，无副作用）；
* 标题确实在使用我们的链：`GetBuffer(0)/(1)` 每秒都有，`real chain 00000080D19292F0, facade 00000080EE050050`。

下一步改为**直接测量**：`ProbeFrame=1` 在 present 前把后缓冲拷到 readback 堆、采样 32x32 求平均亮度
（`backend_xefg.cpp probe_presented_frame()`，10 次/s 上限）。判读：

* 平均/峰值都接近 0 ⇒ 标题没把场景画进我们的链（它画在别处）；
* 平均/峰值正常 ⇒ 内容在链里，是 XeFG 的呈递把它丢了。

## 18. Wuchang 画面回来了：`DLSSG.Backbuffer` 回拷（2026-11-13 深夜）

### 18.1 定位链

1. `ProbeFrame=1` 读回呈递前的后缓冲：`mean RGB 2.4/2.4/2.4 max 90`、`6.5/6.5/6.5 max 255`，
   **R=G=B 完全相等** ⇒ 只有黑底 + 白色 UI，场景不在链里；
2. 新诊断对比指针：`provider: eval #2 DLSSG block: backbuffer=000000813519E8E0 3840x2160 ...`
   与 `xefg: GetBuffer(0) -> 000000813519B910 3840x2160` —— **两块不同的缓冲**；
3. 结论：标题把合成好的整帧放在 `DLSSG.Backbuffer` 里，往交换链只画了 UI，等 provider 去填。
   OptiScaler 的 `HudCopy_Dx12` 走的是同一个假设（它写 hudless 进 `DLSSG.Backbuffer`）。

### 18.2 实现

`BlitDlssgBackbuffer=1`：present 之前把最近一次 DLSS-G Evaluate 声明的 backbuffer
`CopyResource` 到当前链缓冲（两者不同才做）。

实测（首次成功）：

```
blit: DLSS-G backbuffer 000000813519E8E0 -> chain[0] 000000813519B910 (3840x2160)
xefg: 2X present status framesPresented=2 fgEnabled=1 result=OK (0)
```

**画面出现、补帧生效**。三个工程细节：

* 拷贝走**独立的 4 槽 allocator/list/fence 环**，提交后不等（队列本身保证顺序）；早先版本
  每帧 `WaitForSingleObject` 是整条队列同步，会拖慢整帧；
* `ProbeFrame` 只用于定位，已关（每秒一次 GPU 等待）；
* dlssg backbuffer 的 `state` 在参数块里是 `0xFFFFFFFF`（未知），回拷时按 `COMMON → COPY_SOURCE → COMMON`，
  目的链缓冲 `PRESENT → COPY_DEST → PRESENT`。

### 18.3 遗留：补帧有拖影

两个候选，都是「速度场/时序对不上」的形状：

1. **回拷晚一帧**：标题在 3 个 DLSS-G backbuffer 之间轮转，我们用的是「最后一次 Evaluate
   捕获到的那块」。为此 `xefg_set_dlssg_backbuffer()` 现在同时记录 Evaluate 序号，present 路径
   统计 `fresh/stale`（`blit: #N seq=… fresh=x stale=y`）。若 stale 持续增长，就是晚一帧。
2. **MV 声明技巧**：`MvFromTarget` 把 3840x2160 的 MV 声明成合 2560x1440 并把值乘 0.6667。
   若库按整数像素 `Load()` 采样而不是归一化 UV，读到的就是左上角区域 → 速度场错位 → 拖影。
   判定方法：真实重采样一遍（compute pass 把 MV 缩到 2560x1440 并缩放向量值），MVScale 归 1。

### 18.4 改用 OptiScaler 的接入点（在 Evaluate 里、用标题的命令列表）

OptiScaler 的 HudCopy 不是在自己提交的命令列表上做的，而是：

```cpp
// Nvngx_FG.cpp:419-459, inside D3D12_EvaluateFeature
InParameters->Get("DLSSG.Backbuffer", &presentWithHud);
_hudCopyDx12->Dispatch(InCmdList, hudlessResource, presentWithHud, ...);   // InCmdList = 标题的
```

我们把回拷搬到同一位置（`xefg_copy_dlssg_backbuffer(list, src, seq)`，从 `provider_evaluate`
用 DLSS-G Evaluate 的 `list` 调用）：没有额外提交、没有 CPU/GPU 同步，拷贝属于**声明它的那一帧**，
这正是「晚一帧 → 拖影」的消除办法。present 路径的 4 槽拷贝保留，仅作为**该帧没有命令列表**时的兜底
（`g_last_copied_seq` 已记录过就不再重复）。

语义差异（有意保留）：OptiScaler 的 `HudCopy` 着色器是 `hudless + (present - hudless) * uiMask`，
用于修正「present 里没有 HUD / HDR 色调差」的画面；Wuchang 的情况恰好相反（`DLSSG.Backbuffer`
本身就是合成好的整帧），所以这里用 `CopyResource`，不引入它们的阈值检测。

### 18.5 回退到稳定态（2026-11-14）

评估点拷贝（18.4）在 Wuchang 上表现为**闪烁** —— 标题在 DLSS-G Evaluate 之后仍会写链缓冲，
我们的拷贝被覆盖/交错，逐帧时好时坏。据此定下的稳定配置：

| 项 | 值 | 原因 |
|---|---|---|
| `BlitDlssgBackbuffer` | **1** | 唯一让画面成立的机制（present 前把 `DLSSG.Backbuffer` 拷进链缓冲） |
| `BlitAtEvaluate` | **0** | 评估点拷贝会闪烁，保留为可选，默认走 present 路径 |
| `MvFromTarget` | **0** | 全分辨率 MV 的声明技巧能让 XeFG 激活（`fgEnabled=1 presented=2`），但画面拖影/闪烁，先撤 |
| `HighResMv` | 1 | 标题自己声明的（DLSS create flags 无 `MVLowRes`） |
| `ProbeFrame` | 0 | 定位用，已关 |

另外一处必要联动：`BlitDlssgBackbuffer=1` 时**不撤回**「FG 可用」（`provider_block_fg`）。
撤回会让标题停止评估 DLSS-G，我们也就拿不到 `DLSSG.Backbuffer`，画面又会全黑。
此时 XeFG 保持 `fgEnabled=0` 呈递真实帧（1X），标题仍以为 FG 在跑并持续把整帧交给我们。

**当前交付状态：Wuchang 1X 正常画面、无闪烁；插帧的 MV/时序问题未解决。**

### 18.6 移除 MV 处理、恢复 OptiScaler 原版门控（2026-11-14）

结论先写：**Wuchang 的黑屏根因只有 `DLSSG.Backbuffer`，与 MV 无关**；`MvFromTarget`
那套「声明成 render 尺寸 + 向量值缩放」虽然能让 `Activate()` 通过（`fgEnabled=1 presented=2`），
但画面拖影，且掩盖了门控的真实判定。已**整体删除**：

* `Config::mv_from_target` 字段与 `MvFromTarget` ini 键；
* `sr_apply()` 里的尺寸声明 + `mv_scale` 缩放；
* `initFlags` 里 `HIGH_RES_MV` 的联动条件；
* `tag_inputs()` 里的 `mv_at_render`；
* `xefg_frame_feedback()` 里 `low_res_mv` 的那一项。

恢复为 OptiScaler 原文（`Upscaler_Inputs_Dx12.cpp:167-177`、`XeFG_Dx12.cpp:583-610`）：

```
velocity: LowResMV() ? RenderWidth/Height : TargetWidth/Height
activate: IsLowResMV() || nativeAA || ForceFGRenderSizeMVs || IgnoreInitChecks
```

即 Wuchang（`MVLowRes` 未置位、render 2558x1439 ≠ display 3840x2160）**按 OptiScaler 的判定
就是不激活**，日志会打出 `FG stays off - high-res MV …`。

保留的两项（与 MV 无关，属于 backbuffer 修复）：

| 项 | 值 | 作用 |
|---|---|---|
| `BlitDlssgBackbuffer` | 1 | present 前把 `DLSSG.Backbuffer` 拷进链缓冲 —— 画面成立的唯一原因 |
| `BlitAtEvaluate` | 0 | 评估点拷贝会闪烁，默认关闭 |

配套：`BlitDlssgBackbuffer=1` 时不撤回「FG 可用」（否则标题停止评估 DLSS-G，画面又黑）。
当前预期状态：**1X 正常画面 + `fgEnabled=0`**，标题菜单仍显示 x2（它以为 FG 在跑）。

### 18.7 ResizeBuffers 的 flags 保护（我们漏抄的 OptiScaler 行为）

实测崩溃（UE5 `D3D12Viewport.cpp:384` fatal）：

```
SwapChain1->ResizeBuffers(Num=3, Size=(3072,1728), PF=18, Format=0x18, Flags=0x802) failed
```

链创建时是 `Flags=0x842`（`FRAME_LATENCY_WAITABLE_OBJECT(0x40)` + `ALLOW_TEARING(0x800)` +
`ALLOW_MODE_SWITCH(0x2)`），这次 resize 去掉 `0x40` —— DXGI 不允许对 flip 链增删这些标志，
`ResizeBuffers` 直接失败。

OptiScaler 的保护（`FG_Hooks.cpp:844-873`）已逐行搬进 `FgSwapChain::keep_flags()`：

```cpp
if (activeFgOutput == XeFG) {
    if (FGXeFGForceBorderless) SwapChainFlags &= ~DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    if (SCLastFlags != SwapChainFlags) { LOG_WARN("Preventing flag change for XeFG!"); SwapChainFlags = SCLastFlags; }
}
```

`ResizeBuffers` 与 `ResizeBuffers1` 都走它；`SCLastFlags` 用 `m_real->GetDesc().Flags` 的当前值代替。

另外两个同轮修掉的自身缺陷：

* `xefg_cache_chain_buffers()` 原本 `AddRef` 持有链缓冲 → `ResizeBuffers` 因未释放引用失败；
  现在只记指针值、立刻 `Release`；
* 该诊断每秒刷新一次（`ResizeBuffers` 会换掉所有缓冲指针）。

## 19. Wuchang 交付基线（2026-11-14，实测通过）

游戏菜单里**关闭帧生成**、DLSS 保持开启时：`1X present status framesPresented=1 fgEnabled=0`，
画面正常、帧率回到原生、无闪烁无顿挫。日志证据：

```
xefg: chain buffer[0] = 0000008133FB89C0 3840x2160 fmt=24
xefg: chain buffer[1] = 0000008133FB8030 3840x2160 fmt=24
blit: #1868 seq=608 bb=0000008133FBB990(chain[-1]) -> chain[1] ... changed=604 same=1264
```

三件已被测量确认的事：

1. **标题的整帧不在我们的链里**（`chain[-1]`，我们是 2 缓冲，它是另一块 3840x2160 R10G10B10A2）。
   所以 `BlitDlssgBackbuffer` 是**必要**的，不是重复拷贝；OptiScaler 默认路径（它假设标题渲染进
   交给它的链）在 Wuchang 上不成立。
2. **关闭帧生成后 `seq` 不再增长**（`same` 一直涨）但画面仍是活的 —— 标题停止评估 DLSS-G，
   却继续渲染进那块缓冲，所以回拷必须**一直**做，不能只在新评估时做。
3. `ResizeBuffers` 的 flags 保护（§18.7）修掉了 `D3D12Viewport.cpp:384` fatal：
   `preventing ResizeBuffers flag change 0x842 -> 0x802 (keeping 0x842)`，之后的 3072x1728/3840x2160
   两次 resize 都成功。

### 仍未解决：真正的插帧

帧生成开着时标题进入它自己的 FG 契约：**只往链里画 UI**、整帧走 `DLSSG.Backbuffer`、
并且**每评估一次 Present 两次**。因此：

* 关掉我们的插帧 → 同一帧显示两次（顿挫，已复现）；
* 打开 XeFG 插帧 → 它的 `Activate()` 对「全分辨率 MV + 缩放」本来就拒绝（OptiScaler 同判），
  强行用声明技巧通过则出现拖影/闪烁（§18.4-18.6）。

要做成，需要同时满足：正确的 MV（render 尺寸重采样）+ 与标题双 Present 齐平的呈现节奏。
这是独立的一步，不影响当前 1X 基线。

## 20. CP2077 回归（2026-11-14，通过）

`scripts/check_fakerfg_regression.py` 只取日志最后一个 session（最后一条 `dxgi: real module` 之后），
八项全绿：

| 检查 | 旧构建基线 | 新构建 |
|---|---|---|
| 交换链接管 | PASS | PASS |
| 只交出一条链（`could not take` ≤ 1） | PASS | PASS |
| `SetEnabled: true` | PASS | PASS（`lowResMV=1 nativeAA=1`） |
| `2X ... fgEnabled=1` | PASS | PASS |
| `framesPresented=2 result=OK` | PASS | PASS |
| **不出现 `blit: #`** | PASS | PASS（0 次） |
| 无 `resolutions must match` | PASS | PASS |
| 无 `ResizeBuffers failed` | PASS | PASS |

其它：`xefg[error]` 0 行、`crash:` 0 行、flags 保护 0 次触发（CP2077 不改 flags）、
present 平均 41.44 ms → 42.10 ms（噪声内）。

结论：本轮为 Wuchang 加的三样东西（backbuffer 回拷、`keep_flags`、链缓冲诊断）在 CP2077 上
是**零影响路径** —— 回拷没开启、flags 保护未触发、门控与插帧行为与基线一致。

## 21. MvResample：真的产出 render 尺寸的 MV（2026-11-14）

Wuchang 的 MV 是全分辨率（`MVLowRes` 未置位、资源 = 输出尺寸），depth 是 render 尺寸，
XeFG 1.3.1 要求两者同尺寸、且 `HIGH_RES_MV` 已废弃。之前的「声明成 render 尺寸 + 缩放向量值」
是假的（库按声明尺寸采样，速度场错位 → 拖影），已删除。

现在的做法是**真的生成一份低分辨率 MV**：

* `src/faker_fg/shaders/mv_resample.hlsl`：`DstMV[id] = SrcMV.Load(nearest) * (dst/src)`，
  即按深度尺寸最近邻取样、并把向量从输出像素换算成 render 像素；
* 构建期由 SDK 的 `fxc` 编译成 CSO，`scripts/embed_cso.py` 生成为字节数组
  （DLL 运行时不依赖 d3dcompiler）；
* `src/faker_fg/mv_resample.cpp`：root signature（SRV t0 + UAV u0，无采样器/常量）、
  compute PSO、2 槽输出纹理（R16G16_FLOAT，UAV → NON_PIXEL_SHADER_RESOURCE）；
* 调用点与 OptiScaler 的 shader pass 相同：`xefg_tag_at_evaluate()` 里、用标题的
  Evaluate 命令列表、在 tag 之前；
* 之后 `LowResMV` 就是**事实**：门控用 `(HighResMv==0) || MvResample`，tag 尺寸用 render，
  `HIGH_RES_MV` 初始化标志不再设置（`MvResample` 打开时）。

开关 `MvResample=0/1`（Wuchang=1）。日志：

```
mvr: resample pipeline ready (992 bytes of CS)
mvr: output 2560x1440 R16G16_FLOAT created
mvr: motion vectors resampled 3840x2160 -> 2560x1440 (values x0.6667 x0.6667)
```

### 21.1 实测：MV 重采样生效，但拖影/“双层画面”来自呈现节奏（2026-11-14）

`MvResample=1` + 游戏内帧生成开启的实测日志：

```
mvr: motion vectors resampled 3840x2160 -> 2560x1440 (values x0.6667 x0.6667)
xefg: SetEnabled: true (lowResMV=1 nativeAA=0)
xefg: 2X present status framesPresented=2 (was 1) fgEnabled=1 result=OK (0)
xefg: 2X present status framesPresented=1 (was 2) fgEnabled=1 result=MISMATCH_INPUT_RESOURCES (-12)
xefg: 2X present status framesPresented=2 (was 1) fgEnabled=1 result=OK (0)
```

MV 这条链路是通的（尺寸 2560x1440、向量 x0.6667），但 present 在 **OK(2 帧) 与 MISMATCH(1 帧)
之间交替**，画面上就是「FG 帧叠在原始帧上」的双层观感。原因：

* 标题每次 DLSS-G Evaluate 会 **Present 两次**（`blit` 的 changed/same ≈ 1:1 早已测到）；
* 只有其中一次带**新鲜的** MV/depth tag（我们的 `g_eval_tag_fresh` 一次一帧）；
* 另一次 XeFG 仍会尝试生成 —— 它在同内容上按 MV 做形变，于是得到「两层叠加」，库则报
  `MISMATCH_INPUT_RESOURCES`。

另：Win+Tab 触发 `PresentInternal ... 80004004 (E_ABORT)` fatal，只在**插帧开启**时出现
（我们的回拷 + XeFG 自己的 present 在同一队列上叠加，窗口状态变化时更容易撞上）。

**结论：剩下的唯一缺口是呈现节奏** —— 要让 XeFG 每个真实帧只生成一次，且第二次 Present
不携带新的 present id / 不触发插值（相当于 DLSSG 运行库自己做的节拍）。这是独立的一步，
在此之前 Wuchang 保持 1X（`MvResample=0`，门控自然拒绝）。

### 21.2 Win+Tab 崩溃：DLSS-G 后缓冲指针在 ResizeBuffers 后失效（2026-11-14）

日志顺序（`04:54:17`-`18`）：

```
provider: handle slot 0 released            ← 标题释放 DLSS-G 特征
xefg: SetFullscreenState(0, ...)            ← 窗口状态变化（Win+Tab）
xefg[debug] ResizeBuffers called            ← 缓冲重建
xefg: chain buffer[0/1] = <新指针>
crash: 0xC0000005 rip=00007FFB499AA0A7 = nvwgf2umx.dll+0x76A0A7
```

原因：`g_dlssg_backbuffer` 是我们从参数块里取的**裸指针**（不持有引用）。窗口状态变化导致
`ResizeBuffers` 之后，那块纹理可能已经被销毁，而我们下一个 Present 仍然拿它 `CopyResource`
→ 驱动访问已释放资源。

处置：`xefg_resize_notify()`（present 钩子 `after_resize`）里清空该指针，下一帧起不再回拷。
**未知点**：`provider_release` 时*不*清空——实测标题释放特征后仍继续渲染进同一块缓冲
（画面正常），说明纹理寿命长于 handle。

代价（需实测确认）：若标题在 ResizeBuffers 之后不再评估 DLSS-G（游戏内帧生成关闭的状态），
画面会保持黑色，需要重新开关一次帧生成才会恢复。这是「不崩溃」换来的代价，
若不可接受，下一步改为在捕获时 `AddRef`、替换时 `Release`（要另行验证它不会让标题自己的
ResizeBuffers 失败）。

## 22. OptiScaler 三个合成 pass 的原样移植（2026-11-14）

按「不做发明、一比一照搬」的要求，把 OptiScaler 的 present / evaluate 侧 shader pass 全部搬入：

| OptiScaler | 我们 | 触发开关（同名同默认） | 调用点 |
|---|---|---|---|
| `shaders/render_ui/{render_ui.hlsl, render_ui_pm.hlsl}` + `RUI_Dx12::Dispatch` | `shaders/render_ui*.hlsl` + `pass_render_ui()`（`passes.cpp`） | `FGDrawUIOverFG=0`、`FGUIPremultipliedAlpha=1` | present 前（`XeFG_Dx12.cpp:1265-1300` 位置） |
| `shaders/hud_copy/HudCopy.hlsl` + `HudCopy_Dx12::Dispatch` | `shaders/hud_copy.hlsl` + `pass_hud_copy()` | `FGHudCutoff=0`（/10 → 阈值） | DLSS-G Evaluate 内、标题命令列表上（`Nvngx_FG.cpp:405-459` 位置） |
| `shaders/hudless_compare/hudless_compare.hlsl` + `HC_Dx12` | 已编译内嵌（`kHudlessComparePS`） | `FGHudlessCompare=0` | 诊断，尚未接线 |

基础设施：

* `scripts/build_faker_fg.sh` 增加 shader 规格表（file:profile:entry:symbol），用 SDK `fxc` 编译、
  `scripts/embed_cso.py` 生成为字节数组 —— 运行时不依赖 d3dcompiler；
* `passes.cpp` 提供两套 root signature（graphics：SRV t0/t1 + CBV b0 + 静态采样器 s0；
  compute：SRV t0/t1 + UAV u0 + CBV b0）、按 (chain format, variant) 缓存的 graphics PSO、
  compute PSO、3 个内部拷贝纹理（对应 OptiScaler 的 `HC_NUM_OF_HEAPS`）。

**默认全部关闭，所以这几个 pass 目前是惰性代码**：Wuchang 继续用 `BlitDlssgBackbuffer=1`。
按 §21.2 的分析，`HudCopy` 要求 present 里已含画面、`RUI` 只是在已有画面之上叠 UI，
所以它们不能替代回拷 —— 开启与否请在拿到 `OutputReal` / `OutputInterpolated` 的实测数据后再定。

## 23. 把「门面对象」换成 OptiScaler 的做法：真链 + vtable 钩子（2026-11-14）

### 23.1 差异（为什么值得改）

| | OptiScaler | 我们（改前） |
|---|---|---|
| 建链 | `D3D12InitFromSwapChainDesc` | 相同 |
| 交给游戏的对象 | **libxess 的链对象本身**（`DxgiFactory_Hooks.cpp:788-791`，QI 到 `IDXGISwapChain4` 就原样交） | 我们自己 new 的 `FgSwapChain` 门面 |
| 拦截 | 在对象 **vtable** 上挂钩（`FGHooks::HookFGSwapchain`，`FG_Hooks.cpp:617-680`） | 门面逐方法转发 |

对象身份不同可能让标题/中间件（Streamline 的 `sl::Swapchain`、ReShade 的代理检测）走不同分支 ——
这正是 Wuchang「整帧渲染到别处、只把 UI 画进我们这条链」最可疑的来源。

### 23.2 实现（`src/faker_fg/fg_swapchain_hooks.cpp`）

槽位与 OptiScaler 完全一致：`Release 2`、`Present 8`、`SetFullscreenState 10`、
`GetFullscreenState 11`、`ResizeBuffers 13`、`ResizeTarget 14`、`GetFullscreenDesc 19`、
`Present1 22`、`GetFrameLatencyWaitableObject 33`、`ResizeBuffers1 39`。

* 用 MinHook 在该对象的 vtable 槽位上 `MH_CreateHook/MH_EnableHook`；
* 每个 detour 先判 `This == g_hooked_chain`，不是我们的链就**原样转发**（同一 vtable 也服务别的链）；
* 未被 detour 的方法（`GetBuffer`/`QueryInterface`/`GetDesc`/颜色空间/HDR 元数据…）**就是库自己的实现**；
* 未变的逻辑原样搬过来：borderless 模拟（`SetFullscreenState/GetFullscreenState/GetFullscreenDesc/ResizeTarget`）、
  `ResizeBuffers` 的 flags 保护、`SimulateWaitableObject` 信号量、Present 前后的 `before_present`/`present_failed`。

开关：**`RealSwapchainObject`（默认 0 = 现状门面）**。为 1 时 `xefg_start()` 直接返回
`chain` 并调用 `fg_hook_swapchain(chain)`，日志：

```
xefg: hooked swap chain Present (enabled)
xefg: hooked swap chain Present1 (enabled)
...
xefg: swap chain vtable hooked (OptiScaler shape, object 00000080D18B7340)
xefg: the title keeps the library's swap chain object 00000080D18B7340 (hooked)
```

后端其余部分改为通过统一的 `g_chain_api`（门面或真链）访问缓冲/下标，两条路径共用同一套逻辑。

### 23.3 首个 bug：trampoline 没接住（`0xC00000FD`）

启用 `RealSwapchainObject=1` 后立即栈溢出：

```
xefg: hooked swap chain GetFullscreenDesc (enabled)
xefg: swap chain vtable hooked (OptiScaler shape, object 00000080D2D2E060)
crash: first-chance code=0xC00000FD rip 00007FFB7AE7B4EA = dxgi.dll+0xB4EA
crash:   stack 00007FFB7AE7B4EF = dxgi.dll+0xB4EF   ← 同一地址反复出现
```

原因：`MH_CreateHook(target, detour, nullptr)` 的第三个参数是 **out 参数**（MinHook 把 trampoline
写回它）。传 `nullptr` 时 `o_present` 仍指向**已打补丁的入口**，detour 调 `o_present` 又进 detour，
直到栈耗尽。

修法（与 OptiScaler 的 `DetourAttach(&(PVOID&) o_FGSCPresent, hkFGPresent)` 等价）：
把 original 变量的地址交给 MinHook，由它写回 trampoline —— 见 `fg_swapchain_hooks.cpp`
里的 `hook(...)` lambda。

## 24. 与 OptiScaler 的关键分歧：我们伪造了 DLSS-G 特征（2026-11-14）

### 24.1 OptiScaler 的原文

```cpp
// Nvngx_FG::D3D12_CreateFeature, Nvngx_FG.cpp:312-344
auto* provider = getProvider();
if (!provider) return NVSDK_NGX_Result_Fail;                       // 没有 provider 就失败
result = provider->D3D12_CreateFeature(..., &proxyHandle->nativeHandle);   // 从不自己造 handle
if (result != NVSDK_NGX_Result_Success) { delete proxyHandle; return result; }
```

`getProvider()`（`:19-51`）只会给出 FFX / Nukems / Arturs / Combo / Custom —— **真正会写
`OutputReal` / `OutputInterpolated` 的实现**；`None` 时返回 nullptr。也就是说：**OptiScaler 从不
声称“DLSS-G 特征可用”**，没有 provider 就让标题的 `CreateFeature` 失败。

### 24.2 我们错在哪

`provider_create()` 自己造一个 handle 并返回成功 → 标题进入 FG 模式 → 把整帧交给 DLSSG 契约、
只往链里画 UI → **黑屏 + 菜单里的 x2 计数**。用户实测：Win+Tab 切出去后画面回来且是原生帧数 ——
正是因为那时标题**释放**了 FG 特征（日志 `provider: handle slot 0 released`），恢复自己渲染整帧。

### 24.3 处置

新增 `FakeDlssgFeature`（默认 1 = 保持旧行为，CP2077 依赖它）；**Wuchang 设为 0**：
`provider_create()` 直接拒绝，`thunk_create` 转发给真实运行时，失败即失败 → 标题自己关掉帧生成、
自己渲染整帧。日志：

```
provider: Declining DLSS-G CreateFeature (FakeDlssgFeature=0) - the title keeps its own frame generation off
```

配合 `RealSwapchainObject=1`（真链 + vtable 钩子）与 `BlitDlssgBackbuffer=0`（无需回拷），
这条路径就是 OptiScaler 在“没有可用的 DLSSG provider”时的实际行为：**画面正确、原生帧率、没有 x2、没有黑屏**。

## 25. 结论性架构判断：这类标题的 FG 组件是 provider，不是交换链（2026-11-14）

### 25.1 两条车道，Wuchang 走的是 NGX 那条

| 车道 | 前提 | 谁出图 |
|---|---|---|
| Streamline（`slSetTag` / `slEvaluateFeature(DLSS_G)`） | 标题调用 `slSetTag` | provider 或 XeFG（`FgInput=DLSSG` 时 OptiScaler 用它 tag hudless/UI/MV/backbuffer） |
| **NGX DLSS-G 契约**（`DLSSG.*` 参数块 + `OutputReal`/`OutputInterpolated`） | 标题调用 `NVSDK_NGX_D3D12_EvaluateFeature` | **provider 自己写这两个输出**，标题呈递它们 |

实测：Wuchang 日志 **没有任何 `sl: SetTag`**，而 `provider: eval` 每帧都在 —— 它走 NGX 契约，
且 `DLSSG.Backbuffer` / `OutputReal` / `OutputInterpolated` 全是标题自己的纹理（`chain[-1]`）。

### 25.2 因此

对这类标题，**交换链不归 FG 组件**：OptiScaler 在这里出帧靠的是真实 DLSS-G provider
（`Nvngx_Arturs` → `dlss-enabler-headless.dll`，见 §25 前文分析），它写 `OutputReal/OutputInterpolated`。
我们的目标是自己的 FG 组件（双卡 XeSS-FG），所以应当做同一件事：

1. `OwnSwapchain=0`（不接管交换链，`RealSwapchainObject` / 回拷 / vtable 钩子全部不参与）；
2. `FakeDlssgFeature=1`（让引擎持续吐出全部输入：`DLSSG.MVecs/Depth/HUDLess/UI/Backbuffer`、矩阵、jitter、命令表）；
3. **`WriteOutputReal=1`**：在 Evaluate 里、标题的命令列表上，把 `DLSSG.Backbuffer` 拷进
   `OutputReal` —— 这是 provider 的契约义务，真实帧由此成立；
4. `OutputInterpolated` 留给我们的生成器（双卡 XeSS-FG），在它出现之前标题呈现的就是真实帧（1X）。

日志：`provider: OutputReal <- DLSSG.Backbuffer (3840x2160), contract satisfied`。
这条路径下没有任何交换链干预，输入与状态全部由我们捕获，正好是双卡方案需要的基础。

## 26. Wuchang XeFG 链路验证通过（2026-11-14，参考实现）

### 26.1 已实测通过：画面正常、2X 平滑、无重影、无回拷

```
xefg: lane=Upscaler tags: mv 2560x1440 fmt=34 state=0x40, depth 2560x1440 fmt=19 state=0x40, colour none, backbuffer no
xefg: SetEnabled: true (lowResMV=1 nativeAA=0)
xefg: 2X present status framesPresented=2 (was 2) fgEnabled=1 result=OK (0)   ← 连续 49 次，05:59:57 → 06:00:24
```

### 26.2 配置（四个条件缺一不可）

```ini
OwnSwapchain=1            ; 接管交换链
RealSwapchainObject=1     ; 交还 libxess 真链 + vtable 钩子（OptiScaler 形态，FGHooks::HookFGSwapchain 槽位）
FakeDlssgFeature=0        ; 不进 DLSS-G 契约：标题把整帧渲染进我们的链，只有 XeFG 一个呈递者
BlitDlssgBackbuffer=0     ; 不需要任何回拷
MvResample=1              ; 全分辨率 MV -> render 尺寸（2560x1440），向量换算成 render 像素
ForceBorderless=1         ; 否则 libxess 判定全屏并拒绝插帧（SetEnabled 仍返回 SUCCESS）
WriteOutputReal=0
```

### 26.3 每个条件的因果（都踩过）

| 条件 | 不满足时会发生什么 |
|---|---|
| `RealSwapchainObject=1` | 门面对象让标题渲染到别处，链里只有 UI → 黑屏；只能靠回拷补，且带双层/双 Present 错配 |
| `FakeDlssgFeature=0` | 标题进入 DLSS-G 契约（只画 UI，整帧写 `DLSSG.Backbuffer`/`OutputReal`）→ 黑屏 + 菜单 x2 但无帧 |
| `MvResample=1` | `Activate()` 因 `HighResMv` 且 `render != display` 拒绝（与 OptiScaler 同判），`fgEnabled` 永远 0 |
| `ForceBorderless=1` | `xefg[error] Interpolation is not supported in fullscreen mode` —— `SetEnabled` 返回 SUCCESS，但下一帧 `fgEnabled=0` |

### 26.4 这条链路的定位

* **输入校验台**：`xefg_tag_at_evaluate()` 就是 XeFG 收输入的时点，XeFG 接受并生成
  （`presented=2 result=OK`）即证明 MV/depth/jitter/矩阵/尺寸全部正确；
* **双卡参考实现**：本地跑通的这条链（输入 + `SetPresentId` + 生成）就是远端 GPU 要复现的目标；
* **provider 模式（`WriteOutputReal=1` + `OwnSwapchain=0`）** 保留为"无生成器时"的保底与输入捕获。

## 27. 收尾：只保留有效设置与代码（2026-11-14）

### 27.1 删除的（都已被实测证否，属于我们自己的发明）

| 删除项 | 为什么删 |
|---|---|
| `BlitDlssgBackbuffer` / `blit_dlssg_backbuffer()` | 真链 + 标题自己渲染之后不再需要；它曾带来双层与双 Present 错配 |
| `BlitAtEvaluate` / `xefg_copy_dlssg_backbuffer()` | 评估点拷贝在 Wuchang 上闪烁 |
| `MvFromTarget`（声明技巧） | 库按声明尺寸采样导致速度场错位；被真实的 `MvResample` 取代 |
| `ProbeFrame` / `probe_presented_frame()` | 定位用的一次性读回探针 |
| 链缓冲诊断（`chain buffer[...]`、`chain_buffer_index`） | 只为判断"帧在不在我们的链"，问题已定性 |

### 27.2 保留的（全部有实测依据）

| 开关 | 取值 | 作用 |
|---|---|---|
| `OwnSwapchain` | 1 | 接管交换链 |
| `RealSwapchainObject` | 1 | 交还 libxess 真链 + vtable 钩子（OptiScaler `HookFGSwapchain` 槽位） |
| `FakeDlssgFeature` | 0 | 不进 DLSS-G 契约 ⇒ 标题渲染整帧、单一呈递者（1 则为输入捕获模式） |
| `WriteOutputReal` | 0 | provider 模式才需要（把 `DLSSG.Backbuffer` 写进契约的 `OutputReal`） |
| `MvResample` | 1 | 全分辨率 MV → render 尺寸，向量换算成 render 像素（`shaders/mv_resample.hlsl`） |
| `ForceBorderless` | 1 | 否则 libxess 判全屏并静默拒绝插帧 |
| `FgInput/FgOutput` | Upscaler / XeFG | 输入取自 DLSS-SR 的参数块，输出由 XeFG 呈递 |
| 代码侧 | — | `keep_flags()` 的 ResizeBuffers 保护、`FakeDlssgFeature`/`WriteOutputReal` 实现、`MvResample`、OptiScaler 三个 pass（默认关闭）、真链 vtable 钩子 |

### 27.3 两种配方（换游戏时先选一种）

**A. 链路模式（有生成、可当参考实现）**
```ini
OwnSwapchain=1
RealSwapchainObject=1
FakeDlssgFeature=0
WriteOutputReal=0
MvResample=1        ; 仅当标题给的是全分辨率 MV（DLSS 旗标无 MVLowRes）
ForceBorderless=1
FgInput=Upscaler
FgOutput=XeFG
```
判据：`SetEnabled: true (lowResMV=1 …)` + `2X present status … fgEnabled=1 result=OK` 持续出现。

**B. provider 模式（无生成时的保底 + 输入捕获）**
```ini
OwnSwapchain=0
FakeDlssgFeature=1
WriteOutputReal=1
```
判据：`provider: OutputReal <- DLSSG.Backbuffer … contract satisfied`，画面正确、原生帧率。

新开关在 CP2077 上均为默认值（`RealSwapchainObject=0`、`FakeDlssgFeature=1`、`MvResample=0`、
`WriteOutputReal=0`），行为与回归基线一致。
