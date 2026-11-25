# 逐游戏兼容性：本轮落地与剩余清单（2026-11-25）

审计依据：`third_party/OptiScaler-master/OptiScaler/misc/GameConfigs.cpp`、`misc/Quirks.h`、
`inputs/FG/Streamline_Inputs.cpp`、`inputs/FG/XeFG_Inputs_Dx12.cpp`、`hooks/Streamline_Hooks.cpp`、
`framegen/xefg/XeFG_Dx12.cpp`，以及 `third_party/xess/doc/xess_fg_developer_guide_english.md`
的输入约束（MV/depth 同尺寸；HUDless/UI/backbuffer 同尺寸；HUDless/UI 与 backbuffer 同格式）。

## 1. 本轮已落地

### 1.1 Streamline tag 过滤（fakerFG）

| 项 | 游戏 | 实现 |
|---|---|---|
| `IgnoreTagsWithoutHudlessForFG` | Arknights: Endfield、Zenless Zone Zero | `proxy_streamline.cpp::record_tags()`：DLSSG 车道上，一批 tag 含 depth+MV、不含 HUDless 时只转发给原始 `slSetTag`，不写入 FG stage。对应 OptiScaler `Streamline_Hooks.cpp:487-515/576-...`。 |
| `IgnoreValidUntilEvaluateForFG` | 007 First Light | `proxy_streamline.cpp::remember()`：DLSSG 车道丢弃 `sl::eValidUntilEvaluate` tag。对应 `Streamline_Inputs.cpp:316-323`。 |
| `CyberpunkHudlessState` | CP2077 | 只在 `Quirk_CyberpunkHudlessState` 下把 HUDless 的 `PIXEL|NON_PIXEL` 改成 `UNORDERED_ACCESS`；`remember()`（非 snapshot）和 `snapshot_hudless()`（snapshot）都覆盖。此前 quirk 设了但从未读取，且修正被无条件下发。 |
| `Quirk_ForceFGRenderSizeMVs` | Where Winds Meet | profile 已加；XeFG 的 MV/depth tag 改为一律使用各自资源 extent；两者不同时在打开的命令列表上强制 `mv_resample_frame()`（不再用 `HighResMv` 配置去猜尺寸）。 |

### 1.2 MV/depth 同尺寸契约（fakerFG + bridge）

- fakerFG `tag_inputs()`：MV 和 depth 分别按 `stream[].width/height` tag；不同就先 resample；
  resample 失败则 `g_tags_size_ok=false`，`xefg_tags_complete()` 为 false，保持 1X 并打日志。
- bridge `modules/140_frame.inc`：`--no-upsample` 且两条流尺寸不同时打一条明确的
  `MISMATCH_INPUT_RESOURCES` 风险警告。
- `incomingState` 的 `UINT_MAX` 哨兵不再原样交给 XeFG：`state_of()` 落到 `COMMON`。

### 1.3 HUDless/UI/backbuffer 同矩形、同格式

- fakerFG `tag_backbuffer()` 增加 rect 参数（配置 rect 或整链）；HUDless 尺寸与链/rect 不一致时
  **不 tag HUDless**（transfer 缩放不是 letterbox，居中裁剪会几何错误），UI 同理。
- fakerFG `tag_color()`：HUDless/UI 用各自 extent；尺寸必须等于链/配置 rect，格式必须等于链格式。
- bridge：
  - BACKBUFFER 保持整链（或配置 rect），不再从缩放的 HUDless 猜居中 rect；
  - HUDless 尺寸或格式 != 链时不 tag HUDless；
  - UI 格式 != 链格式时不参与 UI 合成；
  - UI-only fallback 要求 UI 与链同尺寸。
- `xefg-in` 日志现在打印真实 rect + 链尺寸。

### 1.4 UE 远平面 75000

- fakerFG `tag_frame_constants()`：`sl_engine_unreal()` 或 `Quirk_ForceUnrealEngine` 且无限远时，
  用有限远 75000 的 LH 投影（对应 `XeFG_Inputs_Dx12.cpp:464-478`）。
- bridge：`--game` 的 `*-Win64-Shipping.exe` 独立于 MV 策略识别 UE，并自动
  `--fg-far-finite 75000`（仅在 far 由相机/派生平面提供时生效；有新鲜游戏矩阵时仍优先
  游戏矩阵，A/B 显示 model 18 基本不消费投影）。PBZ 的 exe 是 `PBZ.exe`，
  `025` 的表匹配里显式标成 UE。

### 1.5 调用结果与 1X 形状

- bridge 所有 `D3D12TagFrameResource` / `TagFrameConstants` / `SetPresentId` 都检查返回值；
  失败时打一条日志并跳过本帧后续 FG tag（`fg_tag_result()`）。
- bridge 在 `g_mv_gate_off`（1X）时不再发 `TagFrameConstants`/`SetPresentId`，与 OptiScaler
  `Dispatch()` 的 early-out 一致。

### 1.6 init 容量 / 运行 count

- bridge 的 `maxInterpolatedFrames` 现在写 `g_fg_interp_max`（没有 MFG 导出时仍写当前值）；
  `SetNumInterpolatedFrames` 在 `SetEnabled` 之后落实际 count。
  这修掉了 `--dyn-mfg` 从 4x 升到 5x/6x 时 `INVALID_ARGUMENT` 的问题。

### 1.7 waitable / swapchain / vsync（fakerFG profile）

| 项 | 游戏 | 实现 |
|---|---|---|
| `SimulateWaitableObject` | CONTROL Resonant | `faker_fg.cpp` profile → `simulate_waitable=1`（现有 semaphore 机制）。 |
| `DLSSGFrameLatencyWaitable` | Kingdom Come: Deliverance II | `Quirk_DLSSGFrameLatencyWaitable`：FG chain 加 `FRAME_LATENCY_WAITABLE_OBJECT`，游戏拿到真实 waitable。 |
| `FGPreserveSwapChain=false` | Wuthering Waves (`client`)、NINJA GAIDEN 4、DoA6 LR、Talos 2、RDR2 | `GameProfile::recreate_swapchain` + `xefg_prepare_recreate()`：第二次同窗口建链时保留旧 XeFG context（延迟到退出释放）再新建，避免旧 facade UAF。 |
| `OverrideVsync` | Dead Space | `GameProfile::override_vsync`；`hk_present`/`hk_present1` 传 sync interval 0。 |

### 1.8 逐游戏表/生成器

- `scripts/gen_input_quirks.py`：输出路径改到 `lock_frame/modules/input_quirks_data.h`；
  token 补齐 `DLSSGFrameLatencyWaitable`、`SimulateWaitableObject`、`ExtendedSearch`、
  `FixSlSimulationMarkers`、`SkipFirstFrames`、`CreateSLOnThe2ndDevice`、`NoFSRFGFirstSwapchain`。
- 重新生成后补进：KCD2、CONTROL Resonant、PBZ、Wuchang (`project_plague`) 等行。

### 1.9 构建验证

- `bash lock_frame/build.sh consumer` → `build/lock_frame.exe`（只有既有的 `wcsicmp` C4996 警告）
- `bash lock_frame/build.sh faker` → `build/fakerFG.dll`
- `bash lock_frame/build.sh producer` → `build/xess_mfg_producer.dll`
- `python3 scripts/gen_input_quirks.py` → `modules/input_quirks_data.h`（54 行）
- 实机运行仍需在 Windows 游戏环境验证（见 §3）。

## 2. 仍需要做 / 明确不做的项

### P1（下一轮）

1. **CameraMotionFill（The Witcher 3）**：需要 RGBA velocity（alpha==0 作为“游戏没写速度”的 mask）
   + `clip_to_prev_clip`/`prev_clip_to_clip` + `CM_Dx12`。当前协议只有 RG16，**未实现**；
   在此之前不要宣称 Witcher 3 的 DLSSG 输入支持。
2. **`DLSSGFrameLatencyWaitable`**：KCD2 已按 OptiScaler `DLSSG_Dx12.cpp:54/123` 给 FG chain
   加真实 waitable flag；若仍冻结，再检查 SL 是否仍在模拟消费。
3. **late init flags**：`INVERTED_DEPTH` / `JITTERED_MV` 只在 init 生效。本轮已加两条 WARNING；
   要彻底解决需在 flag 0→1 时重建 XeFG context（consumer）或持久化（fakerFG DLSSG 车道）。
4. **bridge 的 1X 门控**：本轮采用“1X 不发 constants/presentId”；仍未完全改成 OptiScaler 的
   “保持 SetEnabled、Dispatch early-out”。`--fg-gate` 继续作为过渡开关。
5. **frameRenderTime**：默认仍是捕获节奏；已加 `--fg-rt-producer` 用
   `FrameShared.frame_time_ms` 中位数做 A/B（默认不改，避免回归已测结果）。
6. **UI premultiplied**：bridge 的 `UITEXTURE_NOT_PREMUL_ALPHA` 仍未从
   `kUiFlagPremultiplied` 推导；UI 流出现在 init 之后，需要 context 重建或首帧接受。
7. **`ForceFGRenderSizeMVs` 的完整语义**：现在靠“按真实 extent + 必要时 resample”实现；
   OptiScaler 在 XeFG 上只影响 Activate gate。WWM 需要实机确认 resample 是否比直接高分辨率 MV 更好。

### P2（记录，不在本轮）

- `FGAllowedFrameAhead=2`（TLOU I / PRAGMATA / HFW）：我们的 present 路径是一 tag 一次 dispatch，
  只有实测到节奏问题时才加“一个 tag 服务多个 present”的开关。
- `FixSlSimulationMarkers`（RoadCraft）：只在 present-staged 车道依赖 marker 时才有意义。
- `CyberpunkScalingHudless`：不再自动设置；CP2077 的 ScalingOutputColor 曾有绿块，默认
  继续 snapshot，需要时用 `HudlessSource=Scaling`。
- `NoFSRFGFirstSwapchain`：只对数据/profile 中的 RDR2 生效（OptiScaler 也只在那行使用）；
  不再对所有 UE 标题跳过第一条链。
- `CreateSLOnThe2ndDevice`（Witcher 3）：Streamline 在游戏进程，架构上不适用。
- `PregmataFixDLSSModes` / `ExtendedSearch` / `RestoreComputeSignature`：属于 upscaler 替换或
  DLL 查找，当前架构 N/A。
- `FSRFGHudlessMismatchFixup`：仅 FSR-FG；当前 FSR-FG 输入车道未实现，N/A。

## 3. 验证清单（实机）

| 游戏 | 检查 |
|---|---|
| CP2077 | `sl: tags hudless ... state 0x...`；HUDless 非黑；`xefg: HUDless format ...` 不出现 |
| PBZ / Wuchang | `near/far ... zf=75000`；`xefg-in: ... rect=...` 三者尺寸一致；`xefg: tag sizes ...` 无 mismatch |
| Endfield / ZZZ | `IgnoreTagsWithoutHudlessForFG - skipping ...` 在 upscaler tag 批次上出现 |
| 007 First Light | `IgnoreValidUntilEvaluateForFG - dropping ...` 出现 |
| Where Winds Meet | `xefg: tag sizes ... resampled` 或 MV/depth 同尺寸；不再出现 MISMATCH |
| CONTROL Resonant / KCD2 | waitable 日志；不再冻结 |
| Wuthering Waves / NG4 / DoA6 | `FGPreserveSwapChain=false - destroying ...`，第二次链尺寸/格式正确 |
| Dead Space | `OverrideVsync` 下 Present sync=0 |
| 任意 UE | `Unreal shipping build - --fg-far-finite 75000`（bridge）或 `xefg: Unreal far plane -> 75000`（local） |

## 4. 代码位置速查

| 文件 | 内容 |
|---|---|
| `lock_frame/faker_fg/fg_internal.h` | 新 Config 字段、GameQuirk 位、`xefg_prepare_recreate`、`sl_engine_unreal`、`mv_resample_frame(force)` |
| `lock_frame/faker_fg/faker_fg.cpp` | `GameProfile` 新字段与逐游戏行、ini 读取 |
| `lock_frame/faker_fg/proxy_streamline.cpp` | tag 过滤、HUDless state、`sl_engine_unreal()` |
| `lock_frame/faker_fg/backend_xefg.cpp` | rect/extent/format、UE 75000、结果日志、recreate |
| `lock_frame/faker_fg/mv_resample.cpp` | `force` 参数 |
| `lock_frame/faker_fg/proxy_dxgi.cpp` | FGPreserveSwapChain=false 分支 |
| `lock_frame/faker_fg/fg_swapchain_hooks.cpp` | OverrideVsync |
| `lock_frame/modules/140_frame.inc` | rect、tag result、1X constants |
| `lock_frame/modules/040_cadence.inc` | init 容量、实际 count 日志 |
| `lock_frame/modules/130_input_cache.inc` | late flag WARNING |
| `lock_frame/modules/160_main.inc` | UE 检测 + 75000 |
| `lock_frame/modules/025_input_quirks.inc` | 描述修正 |
| `scripts/gen_input_quirks.py` | 输出路径/token 修正 |
