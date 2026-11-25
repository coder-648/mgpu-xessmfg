# lock_frame 1.3.1-local.camvm 发布说明

> **本地验证版，不作为公开发布。** 基于 1.3.0（协议 v34）；
> `fakerFG` 记录 API 升到 **v3**，`fakerFG.dll` 与 `xess_mfg_producer.dll`
> **必须一起替换**（旧 producer 会拒绝 attach，不会错读布局）。

## 1. 本版做了什么

只补 XeFG 的输入契约，不改调度、模型、UI、marker：

- **`viewMatrix`**：用 `sl::Constants.cameraRight/Up/Fwd/Pos` 构造（原来恒为单位阵），
  带 2 s 缓存，记录没有 constants 的帧不会掉回单位阵。
- **near/far/FOV**：producer 对非 Unreal 标题一律从 `cameraViewToClip` 反算
  （OptiScaler 规则），并发布到共享块；`cameraFOV>3` 视为角度转弧度。
  修复旧 `projection_camera()` 的 `e<0` 分支会拒绝 CP2077 真实矩阵的问题。
- **投影**：默认使用游戏自己的 `view_to_clip`（v12 已在传）；没有新鲜矩阵时退回
  `XMMatrixPerspectiveFovLH(near,far,fov,aspect)`，再退回旧近似。
  `--fg-proj-rz` 保留旧 reversed-Z 近似，`--fg-no-proj` 为 identity；
  `--fg-use-matrix` 成为历史别名（矩阵已是默认）。
- **插值矩形**：消费端每帧补 `BACKBUFFER`（`{0,0,draw_w,draw_h}`），与 OptiScaler 同形。
- **HUDless extent**：clean stream 4 使用真实流尺寸；`mode 5` 仅在
  HUDless/UI/backbuffer 同尺寸时启用。
- **present-staged 车道**：补上 `mvecScale × MV extent`（与 Evaluate 车道一致）。
- 诊断：`[lock] xefg-in: ...` 每人会话一行，配合 `frame constants: proj=/view=` 使用。

## 2. 实机验证（CP2077 4K DLAA，3x / 4x）

```text
api ready (version 3, record 640 bytes)
producer: subscribed to the fakerFG record (api v3, record 72 bytes)
initFlags=0x1 (inverted_depth=1 jittered_mv=0 ndc_velocity=0)
near/far: zn=0.020 zf=16777 (camera=1 matrix=1 farFinite=0)
frame constants: proj=game view-to-clip view=camera
                 fov=51.0deg zn=0.02 zf=16777 aspect=1.77778
                 (3840x2160) mvscale=(3840.0000, 2160.0000)
xefg-in: mv=3840x2160 depth=3840x2160 rect=3840x2160+0+0
```

- 4x `xN=4.00 / show≈120`，3x `xN=3.00 / show=120`，`xNmiss=0`；
- `ab_50`（游戏矩阵）vs `ab_51`（`--fg-proj-rz`）**无可见差异** ⇒ model 18 不消费投影；
- 相对上一版（identity view）**明显改善** ⇒ 收益来自 **`viewMatrix`**；
- `xefg-in` 首帧 `hudless=0x0` 是首帧 patch 未到达，稳态不变。

详见仓库 `docs/design/INPUT_ALIGNMENT_CAMVM.md`（也会随包放到 `docs/`）。

## 3. 包内容

```text
lock_frame.exe / lock_frame_ui.exe   消费端与 UI（版本 1.3.1-local.camvm）
fakerFG.dll                          v3 代理（部署时复制为 dxgi.dll / nvngx_dlssg.dll）
xess_mfg_producer.dll                v3 producer
fakerFG.ini / fakerFG.full.ini       游戏侧配置模板
libxess_fg.dll / libxell.dll         Intel 运行时（含许可）
docs/INPUT_ALIGNMENT_CAMVM.md        本轮验证记录
extras/cp2077_ab/                    本轮 A/B 脚本 + ab_99 检查脚本 + 说明
RELEASE_NOTES.md / manifest.json / SHA256SUMS.txt
```

## 4. 安装（升级现有部署）

1. 完全关闭游戏、`lock_frame.exe`、`lock_frame_ui.exe`。
2. 备份游戏 exe 目录里的 `fakerFG.dll`、`dxgi.dll`、`nvngx_dlssg.dll`、
   `xess_mfg_producer.dll`、`lock_frame.exe`、`lock_frame_ui.exe`。
3. **同一次**把本包的这 6 个文件一起覆盖（三份 fakerFG 必须是同一二进制）；
   `fakerFG.ini`、`fakerFG/` 运行时、`nvngx_dlssg_real.dll` 不要动。
4. 启动游戏，进画面后运行 `lock_frame_ui.exe`。

启动后确认：`<游戏>_fakerFG.log` 出现
`api ready (version 3, record 640 bytes)`，`<游戏>_mfg_producer.log` 出现
`producer: subscribed ... (api v3 ...)`。若仍是 v2/592，说明加载了旧 DLL。

## 5. 测试脚本

把 `extras/cp2077_ab/` 里的文件拷到游戏 exe 目录（与 `lock_frame.exe` 同级），
按 `AB_README_CAMVM.txt` 的顺序运行；结束后 `ab_99_camvm_check.bat` 会核对
二进制一致性、API v3、`proj/view/near-far/rect/mvscale/initFlags`。

## 6. 回滚

把第 2 步的备份按原名恢复即可。`--fg-proj-rz`（旧投影）与 `--fg-no-proj`
（identity）可在运行时做 A/B，无需换包。

## 7. 已知限制（未改）

- `initFlags` 仍不持久化：若 consumer 在游戏声明之前启动，可能整会话缺少
  `INVERTED_DEPTH`；启动顺序异常时先看 `initFlags=0x1`。
- `frameRenderTime` 仍用消费端的估计策略（帧节奏域）。
- `kInputFlagMvDilated → MV tag 尺寸` 映射未做（DLAA/1:1 下无影响）。
- UI 平面传输仍是双卡取舍（默认 patch + mode 4）。
