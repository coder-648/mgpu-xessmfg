# lock_frame 1.2.7 发布说明（2026-11-22）

> 协议仍为 **v30**，`MgpuFgFrame` 布局不变，`fakerFG.ini` 默认值不变。
> 本次是"输入侧正确性 + 逐游戏适配"版本，帧生成模型仍是 **keep2x（model 18，所有倍率）**。

## 1. 新功能：OptiScaler 逐游戏输入表（默认启用）

- 把 `OptiScaler/misc/GameConfigs.cpp` 里所有与 FG **输入**相关的行数据化移植进来
  （99 条里 49 条；生成器 `scripts/gen_input_quirks.py` → `lock_frame/modules/input_quirks_data.h`）。
- 启动时按 `--game` 匹配并打印命中的行；默认 **apply**（只套用安全项，命令行永远优先）：
  - `ForceUnrealEngine` → 无限远平面时用有限 far 75000；
  - `FGXeFGJitteredMV = true` → 置 JITTERED_MV；
  - `FGXeFGDepthInverted = true` → 置 INVERTED_DEPTH；
  - 其余（ForceFGRenderSizeMVs / IgnoreValidUntilEvaluateForFG / IgnoreTagsWithoutHudlessForFG /
    ForceDepthD32S8 / FGDisableHUDFix / FGPreserveSwapChain / FGSkipResizeBuffers /
    FGXeFGForceBorderless / ReprojectionDepthCutoff / CyberpunkHudlessState）要么已等价实现，要么只记录。
- 命令行：
  ```text
  lock_frame.exe --list-quirks                    # 打印 49 行全表
  --game-quirks apply | log | off                 # 默认 apply
  ```
- **UI**：「游戏侧配置」卡片新增 **游戏适配（OptiScaler 表）** 开关，默认开，
  关闭时命令行退化为 `--game-quirks off`；该开关按游戏保存在 `profiles\*.ini`（`game_quirks`）。

## 2. 修复：投影每帧翻转

生产端每个游戏帧会发布两条记录（evaluate + present 回退），只有一条带 `sl::Constants`，
导致消费端 `have_matrix` **每隔一帧翻转**：FOV 在 51.01°↔60°、near/far 在 `0.02/16777`↔`10/10000`、
投影矩阵在"真实"↔"合成"之间每两帧跳一次（第一轮就有，只是当时 near/far 相同、看不出来）。

现在：矩阵派生结果缓存 2 s，缺矩阵的记录沿用缓存（`near/far: ... matrix=2 cached`）。
keep2x 下帧常量因此稳定。

## 3. 修复：near/far 从游戏自己的 view-to-clip 派生

- 之前"没有相机"时固定 `zn=10 / zf=10000`；CP2077 实测真实值为 `zn=0.02 / zf=16777`。
- 现在 `derive_near_far_from_matrix()` 与符号/行列布局无关（reversed-Z 无限远也能识别）；
  `--fg-far-finite <Z|auto>` 只在 far 为无限时生效（CP2077 不适用）。

## 4. 新增（实验/诊断，默认关）

| 开关 | 用途 |
|---|---|
| `--interp-start M` | 初始化按 `--interp` 的容量（解锁按它打补丁），SetEnabled 后只落一次 M 张（容量/倍率解耦） |
| `--mfg-replace-all` | port 路线下 AMD 也替换全部 7 个 shader（不再保留两个 Intel U-Net） |
| `--fg-debug-tag-interp` / `--fg-debug-only-interp` / `--fg-debug-fail-black` | 真正接到 `xefgSwapChainEnableDebugFeature`（此前只解析未调用） |

## 5. 模型选择：所有倍率保持 keep2x

本轮对 ported model 15（`--mfg-model=port`）做了完整实机验证：它确实在跑
（`model_port_applied 15->16`、54/5 程序、运行期 `shader_supplied=28`），
fail-black 三条都不黑（合成被库判为成功），但：

- 3x 仅略好（差距很小）；
- **4x 明显更差**（gpu 16.1 vs 17.6 ms，成本几乎一样）；
- `--mfg-replace-all` 与 capacity 4/count 3 都没有改善；`--fg-jitter-zero` 会让 port 合成静默消失。

因此发布默认 **keep2x（model 18）不变**；`--mfg-model=port` 保留为显式实验入口。
详见 `docs/design/OPTISCALER_XEFG_MFG_SCHEDULING.md` §9–§10。

## 6. 已知限制

- UI 合成（HUDless/unblend）仍默认关闭（见 `docs/UI_SYNTHESIS_GATE.md`）。
- `XeFGCameraMotionFill`（Witcher 3 类标题的相机运动填充）**未移植**：需要带 alpha 的 velocity；
  已在 `docs/design/INPUT_QUIRKS_PORT.md` 记录条件与做法。
- 测试脚本（`cp2077_ab`）只用于本地 A/B，不随发布包分发。
