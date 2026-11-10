# OptiScaler 的 106 条游戏表，我们能复用哪一部分

> 日期：2026-11-10　对象：`temp/optiscaler_src/OptiScaler-master/`（GPL-3.0，与本仓库同许可）
> 相关：`docs/findings/INPUT_SEMANTICS.md` §1.5（为什么语义不用表）、`docs/findings/PBZ_NOTES.md`

## 0. 一句话结论

那张表是**"替换超分器"的补丁库**（DXGI 伪装、ffx-api 输入装配、FG swapchain 生命周期），
**语义知识（MV 单位/符号/深度方向）一条都没有** —— 那部分他们和我们一样靠声明。
真正能搬的是**与"钩子怎么见到资源"和"FG 输入怎么被消费"有关的约 8 条**，其中两条直接指出
我们现有代码的缺口（NDC velocity 的 Y 符号、MV 用显示分辨率时的处理）。

规模（`misc/GameConfigs.cpp`）：85 条 `.exes` + 20 条 `.ueExes`（覆盖 31 个 UE 项目）= **105 个条目**，
其中 92 条改默认值、30 条挂 quirk。

## 1. 全表分类（按"对我们是否适用"）

| 类别 | 代表性条目（出现次数） | 我们是否需要 |
|---|---|---|
| **A. 替换超分才需要的管道** | `DxgiSpoofing`(51)、`EnableFsr2Inputs`/`EnableFsr3Inputs`(各 8)、`AutoExposure`(8)、`RestoreComputeSignature`(4)、`CreateHeaps`、`BuildPipelines`、`DisableReactiveMask`、`ColorResourceBarrier`、`MVResourceBarrier`、`SpoofRegistry`、`UseFakenvapi`、Vulkan 系列 | **不需要**。我们不替换超分；显卡伪装由玩家的解锁器（`dlssg_sm86`/`version.dll`）负责 |
| **B. 他们自己的 FG swapchain** | `FGPreserveSwapChain`(4)、`FGSkipResizeBuffers`(2)、`NoFSRFGFirstSwapchain`、`FGAlwaysCaptureFSRFGSwapchain`(2)、`FSRFGHudlessMismatchFixup`(4)、`CyberpunkHudlessState`、`HideFsr4`、`SkipFsr3Method`、`FastFeatureReset`、`HitmanReflexHacks`、`PregmataFixDLSSModes`(3)、`SkipD3D11FeatureLevelElevation`(2) | **不需要**。bridge 的 swapchain 是我们自己建的，颜色走 WGC（不碰 hudless） |
| **C. 模块发现与钩子** | `ExtendedSearch`(PBZ!)、`KernelBaseHooks`(2)、`UseNtdllHooks`(3)、`LoadD3D12Manually`、`LoadVulkanManually`(2)、`CreateSLOnThe2ndDevice`、`ForceCreateD3D12Device`(2)、`CreateD3D12DeviceForLuma` | **部分需要**（§2） |
| **D. FG 输入的语义** | `ForceFGRenderSizeMVs`、`XeFGCameraMotionFill`、`ForceDepthD32S8`、`IgnoreTagsWithoutHudlessForFG`(2)、`IgnoreValidUntilEvaluateForFG`、`FixSlSimulationMarkers`、`DLSSGFrameLatencyWaitable` | **需要，且有两条已在本次修掉**（§3） |

## 2. C 类：值得对进降级阶梯的

### 2.1 `ExtendedSearch` —— **PBZ 就在这条上**

```cpp
// Phantom Blade Zero
// Extended Search for locating plugin binaries
{ .exes = { "pbz.exe" }, .quirks = { ExtendedSearch } },
```

他们的插件（proxy DLL / ini / nvngx 包装）要在 exe 附近**扩展搜索**才能找到。PBZ 的 exe 在
`Binaries\Win64\`，而我们放 ReShade/add-on 的目录是 `platform\steam\`（实际被加载的那个）。

- **对我们的印证**：add-on、`dxgi.dll`、`game_capture.ini`、共享段必须都放在**被加载的那个目录**。
  我们现在是这么放的 ✓（这也是为什么"改了 add-on 没生效"通常是放错了目录）。
- 我们已经有的等价降级：`[capture] LoaderHook=0`（不钩 `LoadLibrary*`，改每 120 帧扫模块表）。
  那是**发现方式**的降级，和"搜索路径"是两件事，两者都需要。

### 2.2 `CreateSLOnThe2ndDevice` / `ForceCreateD3D12Device`

他们注释的意思是：SL 必须建在**真正渲染的那个 device** 上，有些游戏先建一个"显示/解码"设备。
对本项目（核心就是双 GPU）直接相关：

- 我们按**调用点**挂钩（谁调 NGX/FFX/XeSS 就用谁的 device），天然拿到渲染设备 ✓；
- 风险点在 `[capture] OffloadQueue=1`：我们自己建的命令队列必须与资源所属 device 一致，
  否则就是设备移除（这也是它默认关的原因）。
- **建议**：offload 路径加一条设备一致性断言日志（拿 `resource->GetDevice()` 比对队列 device）。

### 2.3 `KernelBaseHooks` / `UseNtdllHooks`

有一批游戏的加载钩子必须走 `ntdll`，因为游戏自己钩了 `kernel32`。我们有完全相同的结构
（`LoaderHook=1` 钩 `kernel32!LoadLibrary*`；ReShade 自己也钩），但**已经有 120 帧模块表兜底** ✓ ——
唯一要保证的是兜底扫描不会被早退（`SkipMv`/`Input=depth` 等）跳过。

## 3. D 类：FG 输入语义（最有价值，已发现两个缺口）

### 3.1 `XeFGCameraMotionFill` 顺带给出了 XeFG 的**精确口径**（本轮最大收获）

```cpp
// inputs/FG/XeFG_Inputs_Dx12.cpp:514
if (CameraMotionFill())                                { mvScaleX = 1.0f;              mvScaleY = 1.0f; }
else if (initFlags & XEFG_SWAPCHAIN_INIT_FLAG_USE_NDC_VELOCITY)
                                                       { mvScaleX *= size.x * 0.5f;    mvScaleY *= size.y * -0.5f; }
```

两条结论：

1. **XeFG 的 `motionVectorScale` = "每个 velocity 单位等于多少像素"** —— 与 NGX 的 `MV.Scale`
   完全同一个口径。所以我们把 `MV.Scale` 原样转发是**对的** ✓（bridge `cd.motionVectorScaleX`）。
2. **NDC 时 Y 要取负**（NDC +Y 向上，UV/屏幕 +Y 向下）。

我们原来的缺口：`mv_unit == ndc` 时只乘了 2，**没有取负 Y**，而且 bridge 侧也没有
`USE_NDC_VELOCITY` flag。**已全部修掉**：

- add-on：`mv_unit == ndc` → `mv_scale_y *= -2`，并置 `kInputFlagMvNdc`；
- bridge：`producer.input_flags & kInputFlagMvNdc` → `ip.initFlags |= USE_NDC_VELOCITY`，
  initFlags 日志多一列 `ndc_velocity=`。

同时补掉一个更隐蔽的陷阱：`MvNormalize` 会改写流里的单位，而发布的 `mv_scale` 原来只是
**转发游戏声明**，两者一旦不一致，XeFG 拿到的速度就会差 ~2560 倍（画质静默劣化）。
现在发布的 `mv_scale` 会按流里实际单位折算。

### 3.2 `ForceFGRenderSizeMVs` —— 我们已经等价实现了（本轮核对后订正）

他们的用法：当游戏的 MV 是**显示分辨率**时，FG 仍要**渲染分辨率**的 MV
（`framegen/xefg/XeFG_Dx12.cpp:593`、`framegen/ffx/FSRFG_Dx12.cpp:1370`）。

**原先的判断是错的。** 2026-11-10 的 XeSS 实跑（MV 3840×2160，渲染 2562×1441）日志是：

```
crop: mv tex 3840x2160 vs reported 2562x1441 - excess too large, sampling the whole texture
```

`excess too large` 的意思是"差得太多，不做子矩形裁切"，**不是放弃**：`uvMax` 保持 1.0，`ds_run`
把整张 MV 贴图采样到渲染网格上 —— 这就是重采样，正好等价于他们的 `ForceFGRenderSizeMVs`。
唯一的问题是**措辞像失败**，已改成 `resampling the whole texture`。

⇒ 该项**无需改动**，只需把这条写进日志和文档（本轮已做）。

### 3.3 `ForceDepthD32S8` / `IgnoreTagsWithoutHudlessForFG` / `IgnoreValidUntilEvaluateForFG`

- **`ForceDepthD32S8`**：我们已有等价物（`ds_run` 统一转成 `R32_TYPELESS`），而且踩过"镜像
  `D32_S8` 格式导致 `CreatePlacedResource` 失败"的坑 ✓（`shared:` 日志那一段注释）。
- **hudless 相关的两条**：我们用 WGC 取颜色，不读 hudless tag ⇒ 天然免疫 ✓。
- **`IgnoreValidUntilEvaluateForFG`**：SL 的 `eValidUntilEvaluate` 表示"直到 `slEvaluateFeature`
  返回前 SL 还可能改写这个资源"，所以那种 tag 不是"本帧成品输入"
  （`external/streamline/sl_core_types.h:372`；生命周期数值 `0=onlyValidNow / 1=validUntilPresent /
  2=validUntilEvaluate`）。我们的深度/MV 取自 **evaluate 参数**而不是 tag，所以本来就没踩到，
  但日志里原来只打 `lc=2`，看不出危险。**已修**：`sl.tags` 现在打 `lc=2(validUntilEvaluate)`，
  并对 depth/MV（BufferType 0/1）追加 `[staged for Evaluate - not read as input]`。

### 3.4 `cameraViewToClip`：他们只用来**反算**，不直接喂（B-matrix 的设计依据）

`inputs/FG/Streamline_Inputs.cpp:150-210`：

```cpp
memcpy(projMatrix, (void*) &data.cameraViewToClip, sizeof(projMatrix));
...
data.cameraFOV = 2.0 * std::atan(1.0 / b);      // b = projMatrix[1][1]
...
// UE seems to not be passing the correct cameraViewToClip
if (engineType != sl::EngineType::eUnreal && !dontRecalc) loadCameraMatrix();
...
fgOutput->SetCameraValues(data.cameraNear, data.cameraFar, data.cameraFOV, aspect, 0.0f);
```

三条结论直接影响我们：

1. **不要**把 SL 的矩阵原样当投影交给 FG 后端 —— 他们拿到同一个矩阵也只用来算 near/far/FOV。
2. **UE 的矩阵不可信**（至少 near/far 那部分）。所以我们的 v12 只从矩阵取 **FOV 与 aspect**，
   near/far 仍用源声明/`--fg-near-z`。这就是 `--fg-use-matrix` 默认关的原因。
3. 反算公式可以直接抄：`fovY = 2·atan(1/m[5])`、`aspect = m[5]/m[0]`（他们只用 b，aspect 用
   `cameraAspectRatio`；我们从同一矩阵拿更准）。

⇒ 我们的 `bridge.cpp` 照此实现：`--fg-fov` 对这些标题退役，但投影形态仍走
`approx_projection_rz`（已验证的那条路径）。

## 4. 他们表里没有、但我们踩过的（对照用）

| 坑 | 他们的做法 | 我们的做法 |
|---|---|---|
| 同一个 exe 不同 API 声明不同的 MV 单位 | 不判，直接转发声明 | 同样转发，并按声明推导单位（`INPUT_SEMANTICS.md` §1.5） |
| 深度纹理比有效区大 2×3 | — | `crop` 按 SL extent 裁（`ds_run`） |
| MV 是 `FLT_MAX` 无效值 | — | 按 `motion_vectors_invalid_value` 处理 |
| FSR 自带 DLL 销毁时 double-free | — | 记档：PBZ 不用 FSR（`PBZ_NOTES.md` §2） |

## 5. 行动清单（按性价比）

| # | 动作 | 依据 | 成本 | 状态 |
|---|---|---|---|---|
| 1 | bridge：`mv_unit == ndc` 时置 `XEFG_SWAPCHAIN_INIT_FLAG_USE_NDC_VELOCITY` | §3.1 | 小 | ✅ 已做（add-on 置 `kInputFlagMvNdc`，bridge 据此设 flag） |
| 2 | `crop` 守卫在尺寸不等时重采样（`MvResample`） | §3.2 | — | ✅ 无需改动（早就是这么做的，只是日志措辞像失败，已改） |
| 3 | 发布的 `mv_scale` 与流里实际单位对齐 | §3.1 | 小 | ✅ 已做 |
| 4 | offload 队列与资源 device 一致性断言 | §2.2 | 小 | ✅ 已做（`offload_same_device`，不一致则回落到游戏队列并记日志） |
| 5 | SL tag 的 lifecycle 可读化 + `eValidUntilEvaluate` 标注 | §3.3 | 小 | ✅ 已做 |
| 6 | 不搬他们的表；语义继续按"声明优先" | §0、`INPUT_SEMANTICS.md` §1.5 | — | 已确定 |
| 7 | **B-matrix**：真实投影（协议 v12）——FgSniff 的 `cameraViewToClip` → 反算 FOV/aspect → `--fg-fov` 退役 | §3.4 | 中 | ✅ 已做（见 `PROTOCOL.md` §4.6 / `CHANGELOG`） |



