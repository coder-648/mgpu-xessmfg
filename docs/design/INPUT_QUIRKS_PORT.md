# OptiScaler 输入层移植审计（2026-11-22）

> 目的：回答"能不能直接抄 OptiScaler 的输入部分"。
> 结论先说：**结构我们已经有了大部分；它真正独有的只有一张 7 行的 FG 输入表和一个
> 相机运动填充 shader。抄它对 CP2077 的 4x 伪影没有帮助**（CP2077 在它的表里只有一条
> FSR-FG hudless 修正）。真正值得抄的是"以后加游戏时不用再一轮轮试"的那张表。

## 1. OptiScaler 输入层的五个部分，对照我们

| 层 | OptiScaler | 我们的对应 | 状态 |
|---|---|---|---|
| 资源语义 | `FG_ResourceType`（Depth/Velocity/Hudless/UI/Distortion/Color） | `MgpuFgStream` 六条流 + 角色位 | ✅ 已有 |
| 生命周期 | `FG_ResourceValidity`（ValidNow/UntilPresent/ValidButMakeCopy/UntilPresentFromDispatch）+ 逐标题 quirk | producer 声明状态；consumer `RV_ONLY_NOW` / `RV_UNTIL_NEXT_PRESENT`；`IgnoreValidUntilEvaluateForFG` 等价物已在 producer | ✅ 已有（缺少 ValidButMakeCopy 的通用化） |
| 每帧数组 | `IndexForFrameId` / `GetIndexWillBeDispatched` | 槽位环 + frame_id + `tag_begin` 记账 | ✅ 已有 |
| 相机/矩阵 | `FindProjection`（IsProjection/IsView）、near/far 反算、UE 无限远→75000、`UpdateReprojection`、`SetCameraValues` | `derive_near_far_from_matrix()` + 2 s 投影缓存 + `--fg-far-finite`；reprojection 矩阵已随 v13 传输 | ✅ 基本已有（仅 `--fg-use-matrix` 默认关） |
| 数值归一 | `FrameTimeMs()`（秒/毫秒自动判别）、NDC 速度 `mvScale *= size*0.5 / *-0.5`、CameraMotionFill | 我们自己测 ms；`USE_NDC_VELOCITY` + producer 发布匹配 scale | ✅ 已有；CameraMotionFill ❌ 未做 |

**唯一真正的缺口是 CameraMotionFill**，它只服务"MV 不含相机运动"的标题。

## 2. 现在已经整表移植（数据层）

`scripts/gen_input_quirks.py` 把 `misc/GameConfigs.cpp` 解析成
`lock_frame/modules/input_quirks_data.h`，保留**所有声明 `.ueExes` 或提到 FG 输入设置/quirk 的行**
（99 条里 49 条），其余约 50 条（DXGI 伪装、FSR 输入、颜色屏障…）与我们的架构无关，不搬。
生成器可重复运行，换 OptiScaler 版本时重跑即可。

每行的 `.defaults` / `.quirks` 原样保留（OptiScaler 的原文），`025_input_quirks.inc`
再按下面的映射消费：

| OptiScaler 的字段 | 我们的行为 |
|---|---|
| `ForceUnrealEngine` | `--game-quirks apply` 时 `--fg-far-finite 75000`（仅 far 为无限） |
| `ForceFGRenderSizeMVs` | `--upsample`（默认开，等价） |
| `IgnoreValidUntilEvaluateForFG` | producer 已忽略 `lc=2` 的 depth/MV tag |
| `IgnoreTagsWithoutHudlessForFG` | WGC 颜色路径免疫 |
| `ForceDepthD32S8` | depth 流接受 D32/R32 家族 |
| `FGXeFGJitteredMV = true` | 置 `g_fg_jittered_mv`（CLI 强制优先） |
| `FGXeFGDepthInverted = true` | 置 `g_fg_depth_inv` |
| `FGDisableHUDFix` | 我们默认不 tag UI（info） |
| `FGPreserveSwapChain` | `xefg_owned_chain()` 已保留（info） |
| `FGSkipResizeBuffers` | 我们在 resize 时暂停（info） |
| `FGXeFGForceBorderless` | 我们本来就强制无边框（info） |
| `ReprojectionDepthCutoff` | 仅他们的 reprojection 路径用（info） |
| `XeFGCameraMotionFill` | **未实现**（见 §4.1） |
| `CyberpunkHudlessState` | 仅 FSR-FG（无 XeFG 影响） |

默认 `--game-quirks log`：只打印命中的行，不改任何行为；`apply` 才会套用上面的"可套用项"，
`off` 关闭。CLI 永远优先（例如显式 `--fg-far-finite` 不会被覆盖）。

### 2.1 CP2077 在表里的真实内容（实测打印）

```text
[lock] input quirks: cyberpunk2077.exe
[lock]   quirks  : CyberpunkHudlessState, FSRFGHudlessMismatchFixup
[lock]   defaults: WithUpscalerFGInput(FGDisableHUDFix = true), DxgiSpoofing = false,
                   ReprojectionDepthCutoff = 0.02f
[lock]   ours    : FSR-FG hudless only (no XeFG effect); no HUD fix (we do not tag UI);
                   reprojection depth cutoff (info: their reprojection path)
```

两条值得记下的信息：

- `WithUpscalerFGInput(FGDisableHUDFix = true)`：OptiScaler 对 CP2077 的 **Upscaler 车道**
  也关掉了 HUD fix——和我们"Upscaler 车道不发颜色/backbuffer tag"的结论一致；
- `ReprojectionDepthCutoff = 0.02f`：他们的 reprojection（摄像机运动）路径会把深度里
  接近远平面的像素（reversed-Z 下 < 0.02）排除。我们不走那条路径，所以只记录。

## 2.2 其它 48 行的作用

绝大多数行只影响 DXGI 伪装 / FSR 输入 / 资源屏障（例如 `DxgiSpoofing=false`、
`EnableFsr3Inputs`、`ColorResourceBarrier`），对我们的 XeFG 输入没有任何作用；
它们作为数据保留，换游戏时 `--list-quirks` 能直接看到"这个 exe 有没有已知坑"。

## 3. 命令与验证

```text
lock_frame.exe --list-quirks                      # 49 行全表（exe/ue + 我们的映射 + 原始 quirks）
lock_frame.exe --game <exe> --game-quirks log      # 默认：只打印命中的行
lock_frame.exe --game <exe> --game-quirks apply    # 套用安全项（UE far 75000 / jitteredMV / depthInv）
lock_frame.exe --game <exe> --game-quirks off      # 关闭
```

实测（2026-11-22，已构建部署）：

| 命令 | 结果 |
|---|---|
| `--list-quirks` | `input quirks: 49 rows ported from OptiScaler` |
| `--game Cyberpunk2077.exe --game-quirks log` | 命中并打印上面那三行（不改行为） |
| `--game witcher3.exe --game-quirks apply` | `NOT PORTED: camera-motion fill`，far 不变 |
| `--game game.exe --game-quirks apply` | `applied: --fg-far-finite 75000 (Unreal far plane)` |

## 4. 没抄的部分，以及为什么

### 4.1 `XeFGCameraMotionFill`（P2，唯一值得抄的算法）

它的 HLSL（`shaders/camera_motion/CM_Common.h`）语义非常具体：

```hlsl
// 输入 velocity：UV 单位、current-previous，alpha==0 表示"游戏没写速度"
// 输出：像素单位、previous-current；alpha==0 的像素用 depth + Reprojection 填相机运动
if (velocity.a != 0) { Output = -velocity.xy * size; return; }
```

要移植需要三件事，我们目前缺两件：

1. **带 alpha 的 velocity**：我们传输的是 `RG16 pixels prev-cur`（fmt 34），**没有 mask**。
   要在 producer 侧改传 RGBA16（或单独一条 mask 流），属于协议/带宽改动；
2. **Reprojection 矩阵**：v13 已经传了 `clip_to_prev_clip` / `prev_clip_to_clip`，可直接用；
3. **depth 约定**：shader 假设 reversed-Z 的特定形式，需要按我们的 `INVERTED_DEPTH` 校准。

而且它只对"MV 不含相机运动"的标题有意义（表里只有 Witcher 3）。
**建议：等真的要支持这类标题时再做，不要为 CP2077 做。**

### 4.2 `ValidButMakeCopy` 的通用化（P2.5，小）

我们需要它的情况只有 hudless/UI 合成（当前默认关闭）。等 UI 合成重新打开时再补。

### 4.3 MV 残差质量探针（P3，我们自己补，OptiScaler 没有）

协议里已经预留 `kSemMvMeasured / kSemSignMeasured / kSemQualityKnown / kSemMvUsable`，
但 **producer 从未实现**，所以日志一直是 `quality=unknown (sign test has not run yet)`。
做法：用上一帧颜色按 MV warp 当前帧，算残差 → 直接给出 MV 质量分，并自动判定
sign/unit 是否声明错。成本：一个 1/8 分辨率 compute pass（消费端已有 cso 内嵌管线）。
这是"输入是否足够优质"唯一客观的仪器，比抄任何表都值。

## 5. 建议顺序

1. **现在**：跑完 `ab_40~43`（颜色来源 / MV 车道 / 深度），它们回答 CP2077 的两个入口；
2. **加新游戏时**：先看 `--list-quirks` 是否命中；命中就 `--game-quirks apply`；
   没命中的游戏按 `docs/design/XEFG_CHAIN_STANDARD.md` §6 的最小流程走一遍；
3. **要支持 Witcher 3 类标题时**：再做 P2（producer 传 RGBA velocity/mask + CM shader）；
4. **任何标题**：P3 的 MV 残差探针是下一步最有价值的基础设施。
