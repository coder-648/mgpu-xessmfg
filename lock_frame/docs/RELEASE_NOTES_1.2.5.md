# lock_frame 1.2.5 发布说明（本地版）

**1.2.5 是 1.2.2 → 1.2.5 的整理发布**：公开版上一个 tag 是 1.2.1，本包把传输层、
fakerFG 输入车道和 UI 合成的实验结论一次带齐。

- 传输：MV/depth 默认走**无损码流**（协议 **v30**），硬件/格式不支持时自动退回
  「跨卡直接拷贝 + 升降采样」。
- fakerFG：DLSSG / NVNGX / Upscaler 三条输入车道按 OptiScaler 的 Streamline 输入
  规则补齐；FSR 车道未实现。
- UI 合成：**结论是暂无通用解决方案，保持默认关闭**。本版只修掉实验路径里“代码齐全
  但验收不过”的确切 bug，并把开关收敛成显式的本地 A/B。
- 修复：`FgInput=NVNGX` + `FgOutput=Capture` 启动后窗口冻死（present 门控丢帧）。
- 多帧：XeFG 调用序列逐条审计过，运行时行为不变（动态多帧仍不开放）。

## 1. 无损码流传输（MV + depth，协议 v30）

- 渲染卡一次 compute pass 把 MV/depth 编码进跨卡槽（tile 32×8，每 tile ≤264 words），
  消费卡解码回**逐位一致**的原数据；两条流各自 1:1，不再统一降到某个 StreamPercent。
- 协议 **v29 → v30**：`StreamShared` 增加 `codec` 字段。**`fakerFG.dll` 与
  `lock_frame.exe` 必须同版本**；混合版本会直接拒绝启动传输，不会静默出错数据。
- `fakerFG.ini` 的 `MvCodec` / `DepthCodec` 三态：
  - `auto`（默认）：能跑就用，跑不了自动退回老路径（日志 `fr: codec capability: ...`）
  - `1` 强制开、`0` 强制关（等价旧版 1:1 拷贝路径）
  - 两条流互相独立，`MvCodec=auto + DepthCodec=0` 这类混合档合法
- 消费端：MV/depth 各一个 decode pass（分离描述符堆）；源尺寸不一致时自动打开联合
  重建 pass 归一（XeFG 要求同尺寸）；codec 开着且 `load` 连续 ≥85% 会打降级 WARNING。

实测（详见 `docs/design/LOSSLESS_TRANSPORT.md` §17.10）：CP2077 4K/3x 双流
103/107 窗口 `presents=120/120`、`load 68%`；PBZ 40x3（wide depth）67/69 窗口
120/120；Wuchang 40x3 only-depth，coded depth **7.0–7.6 MB/帧（raw 14.06，1.86–2.21x）**；
`--mv-verify` 三次采样 **BIT-EXACT**。

## 2. fakerFG 输入车道对齐 OptiScaler

范围：`lock_frame/faker_fg`（`build.sh faker` 构建的那份）。

**Streamline 车道（`FgInput=DLSSG`）**

- `HiResDepth` / `LinearDepth` 与普通 Depth 一样记为深度流。
- `sl::ResourceTag::extent` 优先于资源 desc（大纹理携带小有效区时尺寸不再被放大）。
- `sl::Constants` v1/v0 版本提升为 v2，避免小结构体被读越界字段。
- 相机 near/far/fov 不可用时从 `cameraViewToClip` 反推；字段本来就合理的标题不被覆盖。
- `mvecScale` 乘 MV 分辨率；`MvScaleMultiplyResolution=0` 可关（标题已给像素单位时）。
- 非 `eValidUntilPresent` 且无命令缓冲的 tag 提升为 UntilPresent；新增
  `FGDepthValidNow` / `FGVelocityValidNow` / `FGHudlessValidNow` 逐标题覆盖（默认 0）。
- 同一 Streamline frame token 只取一个 id：标题重复 evaluate 不会让 id 跳号（跳号会让
  XeFG 接受 `SetPresentId` 但不出帧）。

**本地 XeFG 输出（`FgOutput=XeFG`）**

- 在 Evaluate 里打 tag 的车道（DLSSG / NVNGX / FSR）现在由 present 真正派发：
  `fg_output_submit()` 记录被接受 tag 的 id，present 用同一个 id 调
  `TagFrameConstants` + `SetPresentId`，**一次 tag 一次 dispatch**；resize / pause 清 pending。
- `FgInput=NVNGX`（游戏自己的 DLSS-G 参数块）打通：MV/depth 同尺寸才启用，资源无声明
  状态时用 `MvState`/`DepthState`（缺省 `NON_PIXEL_SHADER_RESOURCE`）。
- `FgInput=FSR` 仍未实现（没有 ffx hook）；`fakerFG.ini` 注释已标明，写 FSR 等于 NoFG。

## 3. UI 合成：暂无通用解决方案（默认关闭）

**结论**：在“游戏只提供 present / HUDless / UI 三选一”的现实输入下，跨卡 UI 合成
没有能覆盖主流标题的通用方案。因此 `UiStream=0` / `UiSource=Off` 仍是发布默认值，
本轮不放开。

本版修掉了实验路径里一个确切的 bug，并补齐了诊断能力（**只对显式本地 A/B 生效**）：

- `unblend_ui.hlsl` 不透明分支（`a≈1`）原来保留**带 UI 的 present**，于是重建出的
  HUDless 里仍有 HUD，XeFG mode 4 插值底图带着 HUD → 拖影不消失。现在与 OptiScaler
  `UiUnblend.hlsl` 一致写 **0（黑）**；不透明 UI 由 mode 4 从 backbuffer 提取，不会上屏。
- `fakerFG.ini` 新增 `UiSynthesis=1` 作为本地 A/B 覆盖；`UiStream=1 + UiSource=Game|Hudless`
  本身也算显式 opt-in（`UiSource=Auto` 仍关闭）。否则这条路径完全惰性。
- 消费端：`--ui-unblend` / `--ui-source` 会自动把 `kRequestUi` 并进请求位（之前
  `--streams=mv,depth` 会把 UI 请求覆盖掉）；显式 `--ui-source` 不再被编译期门控吞掉。
- 失败路径补一次性 `ui-unblend: inactive - <reason>`，验收时能区分“没有 UI 平面 /
  不 fresh / stream 3 无效 / synth 失败”。
- HUDless 源优先用 `HUDLESS_COLOR` 的 stable snapshot，`SCALING_OUTPUT` 仅作 fallback：
  CP2077 在游戏自身 FG 关闭时该 target 可能是空纹理（实测 stream 3 全黑）。
- 已知限制：UI 流是 R10G10B10A2，alpha 只有 2 bit，半透明 UI 边缘会量化。
  后续方向见 `docs/design/UI_UNBLEND_MODE4.md` §11，`docs/UI_SYNTHESIS_GATE.md` 记录门控条件。

## 4. 修复

- **NVNGX + Capture 启动卡死**：`xefg_should_present()` 在没有新 native record 时会丢弃
  Present；切到 `FgInput=NVNGX` 后游戏不 evaluate DLSS-G，record 停供，窗口看起来冻死。
  现在超过 **500 ms** 没有 record 就释放 present 门控并继续上屏（打一条日志），之后再来
  record 会重新挂上。
- `init_fg()` / `SetUiCompositionState` 不再把“stream 3 已预留”当成 UI 存在的证据：
  生产者 `ui_source` 为 Off 时保持 UI composition DISABLED，并打一条说明日志。

## 5. 多帧 / XeFG 审计（运行时不变）

- 对照 XeSS-FG SDK 头文件、OptiScaler（`XeFG_Dx12.cpp`）与 MFG-Unlocker
  （`d3d12_host.cpp`）逐条核对调用序列、资源 tag、`SetPresentId`、UI composition、
  XeLL marker、帧常量：与上游一致，不是“调错 API”。
- `maxInterpolatedFrames` 是**上限**而非当前倍率，所以 `--dyn-mfg` / `--interp-auto`
  的动态升档会被库拒绝；**动态多帧本版不开放**，固定倍率下两种写法等价。
  将来启用时按 `docs/design/TRUE_MFG_OPTIONS.md` §8.3 落地。
- `--mfg-model=port` 请固定倍率：真多帧按 count 选模型，跨越时可能切模型并重编译着色器。
- `--interp N` 同时是解锁补丁容量，要摸到 6x 必须 `--interp 5` 启动。

## 6. 升级注意

- **整包替换**：`fakerFG.dll`、`xess_mfg_producer.dll`、`lock_frame.exe`、
  `lock_frame_ui.exe` 与 `fakerFG.ini` 一起换，协议 v30 不接受混合版本。
- UI 相关开关默认关闭；不需要任何操作，行为与 1.2.1 的插帧路径一致。
- 回退传输：把 `MvCodec` / `DepthCodec` 改成 `0` 重启游戏即可，无需换回旧 exe。
- 构建验证：`./lock_frame/build.sh all` 通过（MSVC 14.44.35207）；
  1.2.5 包由 `./lock_frame/release.sh` 生成并通过 `check_release.py` 校验。
