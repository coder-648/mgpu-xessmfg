# XeFG 输入对齐（相机 / view / 投影 / rect / extent）—— 实机验证记录

日期：2026-11-25　环境：CP2077 4K **DLAA**（render == output == 3840×2160）、
DLSS Frame Generation ON、无边框、120 Hz；消费卡 RX 6750 GRE。
构建：`fakerFG` API **v3**（record 640 bytes），协议 v34。

## 1. 本轮改了什么

只动“喂给 XeFG 的帧常量/资源描述”，不改调度、模型、UI、marker：

| 数据 | 之前 | 现在 |
|---|---|---|
| `viewMatrix` | 恒为单位阵 | 用 `sl::Constants.cameraRight/Up/Fwd/Pos` 构造（OptiScaler `XeFG_Dx12.cpp:843-860`），带 2 s 缓存 |
| near/far/FOV | producer 只在“不可信”时反推；CP2077 的矩阵会被 `e<0` 分支拒绝 | producer 按 OptiScaler 规则对非 UE **一律**从 `cameraViewToClip` 反算，并发布（`m[10]=0 → far=∞`；CP2077 实际 `zn=0.020 / zf=16777`）；FOV 度→弧度 |
| `projectionMatrix` | 合成的右手矩阵（`m[11]=-1`） | 默认游戏的 `view_to_clip`；缺矩阵时退回 `XMMatrixPerspectiveFovLH` 形式；`--fg-proj-rz` 保留旧近似，`--fg-no-proj` 为 identity |
| 插值矩形 | 消费端不 tag `BACKBUFFER` | 每帧补 `BACKBUFFER`（`{0,0,draw_w,draw_h}`），与 OptiScaler 同形 |
| HUDless extent | clean stream 4 也用链尺寸 | 用 stream 4 的真实尺寸；`mode 5` 仅在 HUDless/UI/backbuffer 同尺寸时启用 |
| present-staged 车道的 `mvecScale` | 原始 `(1,1)` | 补上 `× MV extent`（与 Evaluate 车道一致） |

消费端新增一行诊断：`[lock] xefg-in: ...`（随 `frame constants` 各打印一次）。

## 2. 实机日志（07:43 会话，节选）

```text
api ready (version 3, record 640 bytes)
producer: subscribed to the fakerFG record (api v3, record 72 bytes)

[lock] initFlags=0x1 (inverted_depth=1 jittered_mv=0 ndc_velocity=0)
[lock] near/far: zn=0.020 zf=16777 (camera=1 matrix=1 farFinite=0)
[lock] frame constants: jitter=(0.4309, -0.2092) proj=game view-to-clip view=camera
        camera=producer fov=51.0deg zn=0.02 zf=16777 aspect=1.77778
        (3840x2160) mvscale=(3840.0000, 2160.0000) game_dt=33.37ms
[lock] xefg-in: mv=3840x2160 depth=3840x2160 hudless=0x0 rect=3840x2160+0+0
        proj=game view-to-clip view=camera zn=0.020 zf=16777
        fov=51.01deg mvscale=(3840.000, 2160.000)
```

启动段确认尺寸一致：

```text
capture item 3840x2160
capture size taken from the window: 3840x2160
FG swap chain: 3840x2160
present 3840x2160 == window client 3840x2160 (1:1, no stretch)
```

> `xefg-in` 的 `hudless=0x0` 是每个会话**首帧**打印的（patch 尚未到达）；
> 稳态 HUDless/UI 与之前一致。

## 3. A/B 结论（同一位置、同样转速）

| 对比 | 唯一变量 | 结果 |
|---|---|---|
| `ab_50`（默认：游戏矩阵 + camera view） vs `ab_51`（`--fg-proj-rz`） | 投影来源 | **无可见差异** ⇒ model 18 不消费投影（与 `QUALITY_VARIABLES.md` 一致） |
| 本轮构建 vs 上一版（identity view + 合成投影） | 补齐的契约字段 | **明显改善** ⇒ 收益来自 **`viewMatrix`**（两者都 `view=camera`） |
| `ab_52`（`--fg-no-proj`，同时 view=identity） | 负对照 | 仅作对照，不用于归因 |

性能/节奏未受影响：4x `xN=4.00 / show≈120`、3x `xN=3.00 / show=120`、`xNmiss=0`；
4x 偶发 HITCH/skip 属于既有的 4x 预算问题。

## 4. 结论与保留项

- **保留新默认**：游戏 `view_to_clip` + camera `viewMatrix`（SDK 原生、零成本）；
  `--fg-proj-rz` / `--fg-no-proj` 仅作为 A/B。
- 仍存在、但本轮有意未做的项：
  - `initFlags` 持久化（防启动顺序导致 `INVERTED_DEPTH` 未生效）；
  - `kInputFlagMvDilated → MV tag 尺寸` 映射（非 1:1 标题才需要）；
  - `frameRenderTime` 取值策略（属于帧节奏域）；
  - UI 平面传输（双卡设计取舍）。
- 若之后仍与 OptiScaler 有差距，优先看 **颜色来源**（WGC/DWM vs 游戏后缓冲）
  或 **GPU/runtime 差异**（同一模型在不同厂商内核上的数值差异），不再是输入契约。

## 5. 复现

游戏目录（或 `extras/cp2077_ab` 拷到 `lock_frame.exe` 同级）运行：

```text
ab_50_camvm_default_4x.bat   # 默认
ab_51_camvm_rz_4x.bat        # 旧投影
ab_52_camvm_noproj_4x.bat    # identity 对照
ab_53_camvm_default_3x.bat   # 3x
ab_54_camvm_default_2x.bat   # 2x
ab_55_camvm_rz_2x.bat        # 2x 旧投影
ab_99_camvm_check.bat        # 日志/二进制核对（游戏可关）
```

## 6. UE5 / PBZ 后续实测（2026-11-25）与结论更正

PBZ（`PBZ.exe`，UE5）用同一构建 3x 实测：

- **屏幕边缘伪影基本消失**，与 CP2077 一致 ⇒ `viewMatrix` 的收益跨引擎成立；
- **人物模型边缘仍有伪影**；显式 `--fg-of`（AMD 光流）后**表现相同**。

因此：

1. **撤回** `docs/findings/UE5_MV_OPTISCALER.md` §7.4/§8 的“PBZ 的 MV 流不可用”结论
   （2026-11-10 的数据是旧提取路径/旧链路的结论）。PBZ 的游戏 MV 可用，**MV 来源不是
   人物边缘残留的原因**。
2. 剩余的候选按优先级：
   - **keep2x 在 t≠0.5**（3x 的 1/3、2/3；4x 的 1/4、3/4）——最先用 **2x（t=0.5）** 对照；
   - **MV/depth 的 tag 网格**：PBZ 日志显示 render 2562×1441 / depth 1704×960 被联合重建到
     **3840×2160** 后 tag（`--upsample`）。OptiScaler 是直接在 render/MV 原生网格上 tag；
     `DepthCodecPercent=100 + --no-upsample` 可以零改码复现原生网格；
   - **jitter 符号 / MV 符号**（`--fg-jitter-zero`、`--mv-flip`）——UE5 的低成本对照；
   - **port model 15**（`--mfg-model=port --unlock-mfg`）：CP2077 3x 上 port 略好，PBZ 可复测。
3. 兼容性保留：`--fg-of` 仍是 MV 来源的 A/B 开关，但**不再作为 PBZ 的默认修复**。

## 7. 默认值调整（1.3.2-local.camvm）—— **已回退，不采用**

> 2026-11-25 实机对比后回退到 1.3.1：100/100 让 depth 从 1704×960 回到 2562×1441，
> 码流约 +8 MB/帧、depth 编码约 +1.3 ms，而人物边缘没有可见改善。以下内容仅作记录。
> 当前默认：`StreamPercent*=150 / DepthCodecPercent=150`，消费端联合重建到输入画面尺寸。

- `CodecStreamPercent / MvCodecPercent / DepthCodecPercent` 与
  `StreamPercent(Mv/Depth)` 默认全部 **100**（1:1，比特精确）；
- 消费端 **默认关闭** joint 重建：100/100 时直接按游戏 render/MV 网格 tag，
  不再把 MV/depth 升到 3840×2160。期望日志：
  `--upsample: OFF (default)`；
  `xefg-in: mv=2562x1441 depth=2562x1441 ...`（PBZ，render 尺寸）；
  没有 `--upsample: MV/depth rebuilt ... to 3840x2160`；
- `--upsample` 仅作显式 A/B；若旧 ini 的两条流尺寸不同，消费端自动把 depth 升到
  **render 网格**并打一行 `MV/depth stream sizes differ ...`；
- UI 同步：传输比例/深度码流默认 100%，文案改为“默认 1:1（不上采样）”，
  版本显示 `1.3.2-local.camvm`。

这样做的目的：把“无损流又被消费端重采样”这条特殊情况彻底去掉；人物模型边缘的残留
已归到 XeFG 模型/库侧，不再靠输入侧重建来掩盖。
