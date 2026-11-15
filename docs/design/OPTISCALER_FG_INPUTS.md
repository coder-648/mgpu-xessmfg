# OptiScaler 的 FG 输入端设计（可吸收的部分）

源码位置：`temp/optiscaler_src/OptiScaler-master/OptiScaler/inputs/FG/`

```text
Streamline_Inputs.{h,cpp}    DLSS-G（CP2077 走这条）—— 我们刚打通的同一条路
FSR3_Dx12_FG.{h,cpp}         FSR3 的 ffxDispatch 输入
FfxApi_Dx12_FG.{h,cpp}       ffx-api
XeFG_Inputs_Dx12.{h,cpp}     XeSS-FG 的 tag 输入
Upscaler_Inputs.{h,cpp}      公共部分：相机 near/far/vfov、FG_Constants
```

## 1. 三个枚举撑起整个模型

```cpp
enum FG_ResourceType { Depth, Velocity, HudlessColor, UIColor, Distortion, Color /*Hudless+UI*/ };

enum class FG_ResourceValidity { ValidNow, UntilPresent, ValidButMakeCopy, UntilPresentFromDispatch };

enum class FG_Flags { Async, DisplayResolutionMVs, JitteredMVs, InvertedDepth, InfiniteDepth, Hdr };
```

- **ResourceType**：把"tag 类型"提升成"语义角色"。`Color` 就是"hudless+UI 已合成"的兜底角色 ——
  正是我们讨论过的"没有 hudless 时用呈现图"。
- **Validity**：每个资源带生命周期；`ValidNow` 的含义是**"必须在给你的这条命令缓冲里立刻拷"**。
- **Flags**：传给 FG 运行时的语义（显示分辨率 MV / 抖动过的 MV / 反向深度 / 无限远 / HDR），
  与我们 `FrameShared.semantics` + `producer.input_flags` 是对应关系。

## 2. tag → 有效性的映射（Streamline_Inputs::reportResource）

```cpp
validity = (tag.lifecycle == sl::eValidUntilPresent) ? UntilPresent : ValidNow;

// 没有命令缓冲、又不是 UntilPresent -> 只能"YOLO"成 UntilPresent，晚点再拷
if (!cmdBuffer && validity != UntilPresent) validity = UntilPresent;

// 逐标题修正：某些游戏的 tag 生命周期是错的
if (FGVelocityValidNow / FGDepthValidNow / FGHudlessValidNow) validity = ValidNow;

if (tag.lifecycle == eValidUntilEvaluate && gameQuirks[IgnoreValidUntilEvaluateForFG]) return;
```

关键点：**`ValidNow` 不是"不能拷"，而是"必须现在就拷"** —— 因为命令缓冲就在手上。
我们之前的实现把 `lc=0(onlyValidNow)` 当成"不要拷"，方向正好相反。

## 3. 帧索引与每帧数组

```cpp
float _jitterX[BUFFER_COUNT], _mvScaleX[BUFFER_COUNT], _cameraNear[BUFFER_COUNT] …

IndexForFrameId(frameId)            // 用 frame id 精确配对
GetIndexWillBeDispatched()          // 还没 dispatch 时用"将被 dispatch 的 index"
HasResource(type, frameIndex)       // 同一帧重复上报时避免覆盖
```

好处：多帧在飞时不串帧。我们现在是"每帧锁一个槽位"，等效但更粗糙。

## 4. UI 可能走另一条命令缓冲

```cpp
cmdList = fg->GetUICommandList(index);     // XeFG_Inputs_Dx12.cpp:252
```

即 UI 的拷贝不保证和 MV/depth 同一条 list。我们的 UI 流需要允许"两条 list 分别记录"。

## 5. 我们该吸收什么（按性价比排序）

| # | 吸收项 | 我们要做的事 | 价值 |
|---|---|---|---|
| 1 | **Validity 语义**：`ValidNow` = 立刻在给到的 cmd 上拷 | 把 colour 路径的 `lc==0 → 跳过` 改成 `lc==0 → 现在拷` | 高：hudless/UI 才拿得到 |
| 2 | **逐标题 quirk 覆盖**（`FGVelocityValidNow` 等） | 加三个 ini 开关，默认跟随，必要时强制 | 中：跨游戏兼容 |
| 3 | **frame-id 索引的每帧数组** | 我们的 FrameShared 已有 frame_id；把"槽位锁"换成 frame_id → index 映射 | 中：多帧在飞不串 |
| 4 | **缺 list 的 YOLO 兜底** | 没有 cmd 时标记"下一帧补拷"，而不是丢弃 | 中 |
| 5 | **Color 角色（hudless+UI 合成）** | 我们已有 present 路径的残骸，可按这个角色重新定义 | 高（后期） |
| 6 | **UI 独立命令缓冲** | UI 流允许单独 list | 中（做 UI 时需要） |

## 6. 与我们已经实现的部分对照

```text
✓ tag + cmdBuffer 一起记录（reportResource 模型）        ← 已实现，今天打通
✓ 用 tag 声明当 barrier 依据（ValidNow 的等价物）        ← 已实现
✓ 语义位（DisplayResolutionMVs / InvertedDepth / Hdr）   ← 已在 publisher 里，按 OptiScaler 的命名核对一次
✗ ValidNow 即拷贝（我们目前对 hudless 仍然跳过）          ← 第 1 项待做
✗ 逐标题 quirk 开关                                      ← 第 2 项待做
✗ frame-id → index 的每帧数组                            ← 第 3 项待做
```
