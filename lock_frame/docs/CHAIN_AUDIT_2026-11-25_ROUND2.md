# DLSSG 输入 → XeFG 输出链路：第二轮审查（2026-11-25）

审查范围：`faker_fg`（截取/本地 XeFG/桥接）与 `lock_frame`（消费/生成）两段，
对照 `third_party/OptiScaler-master/OptiScaler/inputs/FG/` 与
`framegen/xefg/`。本轮只列**第一轮修复之后仍然存在**的项，以及风险/优化，
不重复已经落地的 P0/P1/P2。

## 0. 本轮落地的补充修正

1. **相机基向量按帧取用**（`faker_fg/backend_xefg.cpp` `tag_frame_constants`）
   - 之前本地 XeFG 的 viewMatrix 只读全局 `g_cam_*`；现在优先读
     `MgpuFgFrame.camera_right/up/forward/pos`，没有才回退 `g_cam_*`。
   - `provider.cpp` 增加了 `DLSSG.CameraRight/Up/Fwd/Pos` 的采集；NGX DLSSG
     输入通道不再只能拿到零基向量。
   - `proxy_streamline.cpp` 的 `sl_stage_inputs()` 现在同时把常量里的相机基向量
     写入记录，Capture 记录也带上了这些字段。
2. **`--no-upsample` 的尺寸不匹配改为“跳过 FG”**
   - `modules/140_frame.inc`：MV/depth 尺寸不同时不再继续发 constants/presentId，
     而是 `g_mv_gate_off = true`，直接 1X 输出；避免 XeFG 侧
     `MISMATCH_INPUT_RESOURCES` 和半帧状态。默认 `--upsample` 不受影响。
3. **游戏目录 `fakerFG.ini` 的 `SimulateWaitableObject=0` 改为注释**
   - 原文件的全量默认值会覆盖 per-title profile（CONTROL/KCD2 需要 1）。
     注释后由代码 profile 决定；CP2077 默认仍是 0，行为不变。
4. 重新构建并部署 `fakerFG.dll` / `lock_frame.exe`
   - 备份后缀 `*.bak_compat3_*`、`*.bak_compat4_*`、`*.bak_compat5_*`；
     `compat_build_info.txt` 已更新。

## 0.1 本轮实现状态（2026-11-25 第二轮）

| 项 | 状态 | 落地 |
|---|---|---|
| P1-A 每资源 command list | 局部修复 | `MgpuFgFrame.stream_cmd[stream]`（API v4，追加在 v3 字段之后）；本地 XeFG 的 MV/depth/HUDless/UI `ValidNow` 使用资源自己的列表。桥接 transport 仍以 `record.cmd_list` 为安全提交边界（跨列表提交没有 producer fence，不能盲用资源列表）。 |
| P1-B frame token | 修复 | `Stage.tag_token/constants_token`；Evaluate/PCL 与非零 token 不一致时拒绝发布，避免单 stage 混帧。 |
| P1-C 高分辨率 MV 激活 | 修复 | `xefg_frame_feedback` 读取记录的 `MGPU_FG_FRAME_HI_RES_MV`/`MGPU_FG_FRAME_MV_DILATED`，不再只看 `HighResMv`。 |
| P1-D HUDless 尺寸不一致 | 修复（patch/Unblend 优先） | 消费端 clean stream 尺寸不匹配时不再直接丢弃，而是走 `record_ui_unblend`（有 patch/UI 时重建为链尺寸）；最终还有同样的尺寸保护。 |
| P1-E 多 HUDless | 修复 | `FGOnlyAcceptFirstHudless=1` 时每帧保留第一张，发布后重置。默认 0 保持旧行为。 |
| P1-F UI premultiplied | 修复 | 消费端 init 读取 `streams[3].ui_flags & kUiFlagPremultiplied`；只有明确 straight alpha 才设置 `UITEXTURE_NOT_PREMUL_ALPHA`。patch-only 链不 tag UI，天然无影响。 |
| P2-A 延迟 init flags | 保留说明 | 可变的 uiMode 已由 `SetUiCompositionState` 处理；premultiplied/inverted/jittered 在链创建时冻结，重建会触发 P2-E 的旧对象问题。patch-only 不依赖晚到的 UI 标志。 |
| P2-B 1X 门槛 | 修复 | 输入不完整时只做 Dispatch early-out：不 tag、不发 constants/present id，也不再 `SetEnabled(0/1)` 翻转。 |
| P2-C NoFSRFGFirstSwapchain | 修复 | 不再对所有 UE 生效；只对数据/profile 中的 RDR2 生效，并保留 ini 覆盖。 |
| P2-D CyberpunkScalingHudless | 修复（删除死分支） | 不再自动启用 CP2077 的 ScalingOutputColor patch 源（历史上有绿块）；需要该来源时用 `HudlessSource=Scaling`，默认继续 snapshot。 |
| P2-E recreate 生命周期 | 修复 | `FGPreserveSwapChain=false` 不再立即 Destroy 旧 context；旧 context 进入 bounded retire 列表，进程退出时统一释放，避免旧 facade Present 的 UAF。 |
| P2-F 多 HWND | 部分修复 | 旧 owner 窗口销毁后允许新窗口接管；同时存在的第二窗口仍 pass-through（单 XeFG 上下文设计）。 |
| P2-G DLSSGFrameLatencyWaitable | 修复 | KCD2 profile 在 FG swapchain 描述中加 `DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT`，不再依赖模拟 semaphore。 |

**验证**：`build.sh faker`、`build.sh producer`、`build.sh consumer` 均通过（consumer 仅保留既有 `wcsicmp` C4996 警告）。部署到 CP2077 目录，备份后缀 `*.bak_compat13_*`，`compat_build_info.txt` 已更新。


## 1. 仍然存在的链路问题（历史记录；状态见 0.1）



### P1-A 一个 Frame 只保存一个 command list（真实兼容缺口）

`proxy_streamline.cpp` 的 `StageEntry` 不保存每个 tag 的 `cmd`；`Stage` 只有
`g_stage.cmd`，最后记录 `f.cmd_list = g_stage.cmd`（present-staged）或
Evaluate 的 `cmd`（Evaluate 路径）。OptiScaler 的 `Sl_Inputs::Resource` 是
**每个资源各自带 cmdList** 的。

影响：
- 标题在 A 命令列表 tag MV/depth、在 B 列表 Evaluate 时，本地 XeFG 的
  `ValidNow` 复制会落在 Evaluate 列表上，顺序/状态可能不是 tag 时的状态；
- Capture 桥接的 transport 也只按 `record.cmd_list` 记录复制。

建议：给 `MgpuFgResource` 增加 `cmd_list`（或按 tag 保存），本地 lane 用
资源自己的列表做 `ValidNow` 复制；桥接协议 v 升级时再加。CP2077 当前同列表，
所以日志没有暴露问题。

### P1-B `slSetTagForFrame` / `slSetConstants` 的帧令牌没有参与匹配

`Stage` 有 `frame_token`，但 `hk_set_tag_frame`/`hk_set_constants` 没有把
token 落到 stage，`hk_evaluate` 也不校验。OptiScaler 用
`Sl_Inputs::CheckForFrame(frameId)` 按帧缓存。多视图、延迟 tag、或先写
N+1 再 Evaluate N 的标题会把两帧输入混在一起。建议至少：
- `Stage` 保存最近 tag/constants 的 frame token；
- Evaluate token 非 0 且与 stage 不一致时拒绝发布并记一次日志；
- 或改成按 token 的小数组（2–3 帧）匹配。

### P1-C `motionVectorsDilated` / 高分辨率 MV 的激活门槛

`faker_fg` 的 `xefg_frame_feedback` 仍主要看 `cfg().high_res_mv`，而不是记录里的
`MGPU_FG_FRAME_HI_RES_MV`/`MV_DILATED`。第一轮已经用“按资源实际 extent 做
resample / 不一致即 1X”兜住了正确性，但**是否允许激活**仍可能与 OptiScaler 的
`Sl_Inputs::IsLowResMV()` 判定不同。对 render≠display 且声明 dilated MV 的标题，
可能过早 1X 或过早激活。建议把激活判断改成“记录声明的 MV 类型 + 实际 extent”
两者结合，并保留 `Quirk_ForceFGRenderSizeMVs` 覆盖。

### P1-D HUDless 尺寸不一致时直接放弃 UI 合成

消费端 `modules/140_frame.inc` 现在对“HUDless 小于/大于链”的 clean stream
直接跳过 HUDless tag（第一轮的设计选择：不猜 letterbox 子矩形）。这保证不会
错位，但会退化成无 UI 合成。优化方向：
- 有 UI/patch 流时，先用 `record_ui_unblend` 重建到链尺寸，再 tag；
- 或增加一个只放大 HUDless 的 compute pass（成本低于整帧 unblend）。
本地 `backend_xefg` 对 HUDless 尺寸不匹配也是跳过，同理。

### P1-E 多个 HUDless tag 的“先到先得”策略缺失

OptiScaler 有 `FGOnlyAcceptFirstHudless`；我们的 `Stage` 对同一帧的第二个
HUDless 直接覆盖。对会在一帧内重复 tag（UI 重建、反射/多视图）的标题，可能拿到
错误的那一张。建议加一个 per-frame `hudless_seen` 标志和配置项，默认保持
“最后一张”（当前行为），按标题开启“第一张”。

### P1-F UI premultiplied 初始化标志仍然缺失

消费端 `g_xapi.CreateContext` 目前没有设置 UI premultiplied 标志。XeFG 的 UI
合成如果按错误 alpha 约定初始化，会出现边缘发灰/光晕；目前 CP2077 用
patch/unblend 路径绕开了。需要从 UI tag 的 flags（或 per-title config）在
context 创建时传入；不能在运行中改。

### P2-A 延迟初始化标志仍未重建 context

`modules/030` 对 `GetInitParams` 的 `flags` 只记 warning（DLSS 输入 lane 要求
UI/HUDless 等），不会重建 XeFG context。部分标题第一帧才 tag UI，导致 UI
composition 被静默关闭。可以做一次“首帧后重建”，但必须处理旧 context 仍被
游戏引用的问题。

### P2-B 1X 门槛仍在反复 `SetEnabled`

`modules/140` 在 depth/MV 缺失时仍调用 `SetEnabled(false)` 再恢复；OptiScaler
是在 `Dispatch` 早期直接 return，不改 library 状态。反复 enable 可能触发
内部重建/清历史。建议把“本帧不生成”做成不调用 API 的 early-out，只有真正
切换 1X/生成时才改 enabled。

### P2-C `NoFSRFGFirstSwapchain` 的 UE 判定过宽

本地 `proxy_dxgi` 对所有 `sl_engine_unreal()` 的标题都不接管第一条 swapchain；
对非 FSR-FG 的 UE 标题这是不必要的 pass-through，可能影响 present 钩子/等待对象。
建议按标题/插件（FSR-FG）而不是引擎判断。

### P2-D `Quirk_CyberpunkScalingHudless`（已删除死分支；见 0.1）

`025` 有该 quirk 的描述，但 `faker_fg` 没有任何代码读取它。要么在 HUDless
来源选择里接上（CP2077 `ScalingOutputColor` 为空时的已知情况），要么删掉，
避免误以为已覆盖。

### P2-E `xefg_prepare_recreate` 的旧 facade 生命周期风险

`Wuthering Waves` 的 `FGPreserveSwapChain=false` 路径会 `Destroy` 旧 context/
chain 并置空 `g_facade`。如果标题仍持有旧 `FGFacade::SwapChain`（或旧链还有
引用），下一次 Present 可能访问已释放对象。需要在 Destroy 前确认引用计数/
标题已释放，或把旧 facade 延迟到进程结束再释放。

### P2-F `FGPreserveSwapChain` 只处理同一 HWND 的第二条链

不同 HWND/不同 adapter 的第二条链仍是 pass-through。若后续标题需要接管多窗口，
需要按 HWND 维护多个 hook/owner，而不是单例。

### P2-G `DLSSGFrameLatencyWaitable` 仍是近似实现

KCD2 的“精确 waitable、不消费信号”路径仍是模拟对象；如果标题依赖真实
`ID3D12Fence`/semaphore 的 signal count，可能出现等待超时或提前。需要按
OptiScaler 的 per-title waitable 语义补全（或至少加日志检测）。

## 2. 数据/兼容性缺口（按 OptiScaler 对照）

- `cameraMotionIncluded / cameraMotionFill`：仍不支持（Witcher 3 的
  camera-motion-only 输入需要 RGBA velocity 合成）。
- 单独的 `UIAlpha` tag：仍只记录、不参与合成（XeFG 需要 UIColorAndAlpha）；
  需要 per-title 的 alpha 合并。
- `FGHudCutoff` / `Hudfix`：本地 pass 有，消费/桥接没有完整 per-title 策略。
- `FGAllowedFrameAhead` / `FGAsync`：没有移植；对靠“提前一帧”提交的标题可能
  影响输入时效。
- `FGDisableUI` / `FGDisableHudless` / `FGOnlyAcceptFirstHudless` 的配置项
  仍未全部暴露；当前只有代码内的来源顺序。
- `ForceDepthD32S8` 仅在记录层支持，缺少按标题强制的开关。
- `SL1`（旧 Streamline）输入不在本构建范围；如遇到旧插件会走不到 tag 钩子。

## 3. 性能/维护优化项

1. **Consumer `tag_color`/`ui_fmt_ok` 每帧 `GetDesc()`**：HUDless/UI 的格式在
   生命周期内基本不变，可以按资源指针缓存格式/尺寸，减少每帧 COM 调用。
2. **`provider.cpp` 新增 12 次 `Get()`**：每次 Evaluate 多 12 次参数查询，
   可接受；如果想再省，可缓存“该参数是否存在”的一次性探测。
3. **`tag_frame_constants` 每帧构造 view**：成本极小；现在按记录/全局回退，
   保持即可。
4. **强制 MV resample**：`mv_resample_frame(force=true)` 只在 extent 不一致时
   触发；可以加一个每标题的“一次性日志 + 计数器”，避免用户误以为是每帧成本。
5. **per-title 表仍是两份**：`faker_fg` 的手写 profile 与
   `modules/input_quirks_data.h` 的生成表没有共用源；建议把
   `SimulateWaitableObject`、`IgnoreTags...`、`ForceFGRenderSizeMVs` 等也纳入
   一个生成器，减少“消费者修了、截取端没修”的漂移。
6. **`sl_publish_staged` 的 depth-only 兜底**：现在消费者会用光流；如果后续打开
   `--fg-mv-scale`/高分辨率 MV，建议在记录里显式标记“MV 由消费者生成”，避免
   下游把空 MV 当成低分辨率输入。

## 4. 建议的验证顺序

1. `compat_3x_default.bat`（主路径，3x 默认）→ 看
   `compat_logs/latest` 是否仍无 tag/尺寸/格式失败；
2. `compat_00_check.bat`（环境/版本）；
3. 若要复现高分辨率 MV/相机路径：`compat_3x_rtproducer.bat` 仅作对照；
4. 只有出现 HUDless 尺寸告警时，才需要做 P1-D 的 consumer 重建；
5. 若后续测 CONTROL/KCD2，先确认游戏目录 `fakerFG.ini` 的
   `SimulateWaitableObject` 仍是注释状态（本轮已改）。
