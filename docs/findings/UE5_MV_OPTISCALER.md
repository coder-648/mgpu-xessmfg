# OptiScaler 是怎么处理 UE5 的（源码走读，2026-11-09）

> 要回答的问题：**影之刃零 / 黑神话 / 明末 这些 UE5 游戏在 OptiScaler 里没有专门的
> "MV 修补"，为什么它那边很少听到模型运动伪影？** 以及我们能抄什么。
>
> 证据全部来自本机完整源码 `temp/optiscaler_src/OptiScaler-master/`
> （上游 master 快照，2026-11-06，GPL-3.0；抽取版见 `third_party/optiscaler/`）。
> 我们自己的实现用 `src/...` 的 file:line 对照。
> **本文只有结论与实验设计，没有改代码。**

---

## 1. MV 只有一个来源：游戏自己的 upscaler 调用

OptiScaler **不抓屏、不算光流**。MV / 深度 / jitter / 相机只来自它劫持的那次
upscaler dispatch：

| 输入源 | 位置 |
|---|---|
| DLSS（NGX） | `inputs/NVNGX_DLSS_Dx12.cpp`、`inputs/NVNGX_DLSS_Dx11.cpp`、`inputs/NVNGX_DLSS_Vk.cpp` |
| FSR 2/3 + ffx-api | `inputs/FSR3_Dx12.cpp`、`inputs/FfxApi_Dx12.cpp` |
| XeSS | `inputs/XeSS_Dx12.cpp` |
| 游戏自己就在用 XeFG | `inputs/FG/XeFG_Inputs_Dx12.cpp` |

关键点：**MV 的"性质"是游戏自己声明的，不是猜出来的。** DLSS 在 CreateFeature 时会给出
feature flags，OptiScaler 把它们一对一映射成自己的 init flags
（`upscalers/IFeature.cpp:42-72`）：

| 游戏给的 DLSS flag（`third_party/dlss/include/nvsdk_ngx_defs.h:286-300`） | OptiScaler 字段 | 用户覆盖项 |
|---|---|---|
| `NVSDK_NGX_DLSS_Feature_Flags_DepthInverted` | `_initFlags.DepthInverted` | `[DLSS] DepthInverted` |
| `NVSDK_NGX_DLSS_Feature_Flags_MVJittered` | `_initFlags.JitteredMV` | `[DLSS] JitterCancellation` |
| `NVSDK_NGX_DLSS_Feature_Flags_MVLowRes` | `_initFlags.LowResMV` | `[DLSS] DisplayResolution` |

这三个再变成 XeFG 的初始化标志（`IFeature.cpp:109-118` → `framegen/xefg/XeFG_Dx12.cpp:359-382`）：

```
DepthInverted  -> XEFG_SWAPCHAIN_INIT_FLAG_INVERTED_DEPTH
JitteredMV     -> XEFG_SWAPCHAIN_INIT_FLAG_JITTERED_MV
!LowResMV      -> XEFG_SWAPCHAIN_INIT_FLAG_HIGH_RES_MV   // 而且这个 flag 已废弃
```

MV 的量纲同理，"照抄游戏"（`inputs/FG/XeFG_Inputs_Dx12.cpp:510-525`）：

```cpp
float mvScaleX = data->motionVectorScaleX;      // 游戏自己报的值
float mvScaleY = data->motionVectorScaleY;
if (gameFlags & XEFG_SWAPCHAIN_INIT_FLAG_USE_NDC_VELOCITY) {  // 只有 NDC 才换算
    mvScaleX *= _mvSize.x * 0.5f;               // _mvSize = 我们 tag 的那张 MV 的尺寸
    mvScaleY *= _mvSize.y * -0.5f;
}
fg->SetMVScale(mvScaleX, mvScaleY, index);
```

像素空间约定写死在相机运动 shader 里（与 `OPTISCALER_NOTES.md` §1 一致）：
**像素单位 + (previous − current)**，见 `shaders/camera_motion/CM_Common.h`。

⇒ **OptiScaler 里不存在"UE5 的 MV 单位/方向不对，我们翻一下"这种修补。**
它的正确性来自"游戏给 DLSS 什么，就原样给 FG 什么"，而不是逐游戏硬编码。

---

## 2. 唯一硬门槛：MV 必须是 **render 分辨率**，不是 display 分辨率

两处同一判断（`menu/menu_common.cpp:5357-5358`、`framegen/xefg/XeFG_Dx12.cpp:592-595`）：

```cpp
const bool correctMVs = fgOutput->IsLowResMV() || nativeAA ||
                        (gameQuirks & GameQuirk::ForceFGRenderSizeMVs) || ignoreChecks;
// 不满足：菜单显示 "Requires disabling dilated motion vectors"，并且 FGEnabled=false
```

- DLSS 的 `MVLowRes` 表示"**MV 在（较低的）渲染分辨率上**，而不是显示分辨率"——这正是
  XeFG 要的东西（它与 XeSS 的 `XESS_INIT_FLAG_HIGH_RES_MV` 互为反面，见
  `IFeature.cpp:113` 的 `FGXeFGHighResMV = !LowResMV`）。
- 所以 OptiScaler 的逻辑是：**游戏报的是显示分辨率的 MV（flag 没置、也不是 nativeAA）
  → 直接不给开 FG**，菜单写着 "Requires disabling dilated motion vectors"。
  它宁可不开，也不喂错分辨率的 MV。
- 例外只有两类：`ForceFGRenderSizeMVs` quirk（个别游戏）和 **Unity 专属资源翻转**
  （`dllmain.cpp:1963-1966`：Unity + Upscaler 输入 → 强制 `FGResourceFlip=true`）。
  **没有任何 Unreal 专属的 MV 修正。**

对我们的含义：我们把 MV 按 `[capture] StreamHeight=1440` 降采样之后，tag 出去的
`resourceSize` 是 2560×1440，而 `motionVectorScale` 仍是游戏报的 4K 渲染尺寸
（`src/bridge/bridge.cpp:6135-6143`）。对 XeFG 来说这等于"MV 纹理只有 1440p，
但每个纹素要按 4K 的比例放大"——**是否被库按纹理自身尺寸归一化，必须实测**（见 §5 实验 2）。
OptiScaler 不做降采样，所以这个问题它根本不存在。

---

## 3. UE5 专属处理只有两条，都在"引擎识别"之后

引擎识别（`Util.cpp:150-296`）三条独立判据，任一命中即 `GameEngineType::Unreal`：

1. exe 名后缀 `-Win64-Shipping.exe` / `-WinGDK-Shipping.exe` / `-Windows-Shipping.exe` /
   `-Win64-Test.exe` 等；
2. 版本信息（ProductName / CompanyName / LegalCopyright / FileDescription）里出现
   `unreal` / `epic games`；
3. 目录结构 `.../Binaries/Win64|WinGDK/`。

判定为 Unreal 之后，**只有两件事发生**：

**(a) 无限远平面 → 75000**（`inputs/FG/XeFG_Inputs_Dx12.cpp:461-478`）

```cpp
bool infiniteDepth = cameraKnown && nearZ != 0.0f && farZ == 0.0f;
// UE5 的 XeFG 插件给的是引擎投影（far=∞），但 XeFG 不接无限远；
// UE 自己的 DLSSG 集成送的是 r.Streamline.CustomCameraFarPlane（一个有限的远平面）
if (infiniteDepth && (gameEngine == Unreal || NVNGX_Engine == UNREAL || ForceUnrealEngine)) {
    infiniteDepth = false;
    farZ = 75000.0f;
}
```

**(b) 让 UE 的 DLSS 菜单"认为驱动够新"**（`NVNGX_Parameter.cpp:711-717 / 787-798 / 825-836`）：
UE5 只认 `Major.Minor = 10.10` 以上，所以 Unreal 下把 NGX 能力探测的版本号伪装成 10.10。
**与画质无关**，只是让 DLSS / DLSS-FG 选项出现。

**没有 (c)**：Unreal 下没有 MV 翻转、没有相机运动填充、没有曝光补丁。

---

## 4. 逐游戏表：三个 UE5 标题都只为"能开 / 不崩"，一律不动 MV

`misc/GameConfigs.cpp`（`ueExes` 会自动匹配 `<name>-win64-shipping.exe` / `-wingdk-shipping.exe`，
见文件末尾的 `MatchesExe`）：

| 游戏 | 条目 | 配置 | 目的 | 动 MV 吗 |
|---|---|---|---|---|
| **黑神话：悟空** | `.ueExes={"b1"}` → `b1-Win64-Shipping.exe`（:442-447） | `SpoofRegistry=true` | 让菜单里出现 **DLSS-FG** | ❌ |
| **明末：渊虚之羽** | `project_plague-win64-shipping.exe` / `-deck-shipping.exe`（:418-423） | `SkipFirstFrames=10` | 绕开局崩溃（`inputs/NVNGX_DLSS_Dx12.cpp:933` 起 10 帧不跑替换） | ❌ |
| **影之刃零** | `pbz.exe`（:674-679） | quirk `ExtendedSearch` | 额外在 `Plugins/` 子树里找 upscaler DLL（PBZ 把它当 UE 插件装，`Util.cpp:701-731`） | ❌ |
| 赛博朋克 2077（对照） | `cyberpunk2077.exe`（:323-327） | `ReprojectionDepthCutoff=0.02`、`FGDisableHUDFix` | 相机运动重投影的深度截断 / HUD | 只影响**相机运动填充**那条路 |
| 巫师 3（对照） | :619-620 | quirk `XeFGCameraMotionFill` | velocity 的 alpha=0（只有相机在动）时补相机运动 | 唯一"改 MV"的 quirk，**只给巫师 3** |

`ExtendedSearch`、`SkipFirstFrames`、`SpoofRegistry` 都与画面无关：
前两个是"找得到 DLL / 不崩"，第三个是"菜单里能选"。

⇒ **结论：OptiScaler 对影之刃 / 黑猴 / 明末一视同仁。** 没有"影之刃要乘 2、黑猴要翻转"
这种表，也从来没有"UE5 就 `--no-mv`"这种规则——我们的 `--no-mv` 备注是**我们自己的实测**
（`docs/findings/HANDOFF.md` §8.6）逼出来的，不是一个通用规律。

---

## 5. 我们和它的差距（按可疑度排序）与对应实验

### 5.1 `JITTERED_MV` 从来没告诉过库 ← **最可疑，直接对应"模型运动伪影"**

`third_party/xess/inc/xess_fg/xefg_swapchain.h:95` 的定义是一句话：
`XEFG_SWAPCHAIN_INIT_FLAG_JITTERED_MV` = **"Remove jitter from input velocity."**
——置上它，库才会把 jitter 从进来的 velocity 里减掉。

- OptiScaler：`_initFlags.JitteredMV` → 这个 flag（`IFeature.cpp:112` → `XeFG_Dx12.cpp:362-363/378-379`）。
- **我们：`src/game_capture/in_ngx.cpp:230-238` 直接 `out.flags = 0`**，注释写着
  "DLSS-SR motion vectors are UV-space, **not jittered**"；bridge 侧
  `src/bridge/bridge.cpp:2310-2315` 也只设 `INVERTED_DEPTH`，`--fg-jittered-mv` 已被删掉
  （理由写的是"tagged MVs are a dead input"——而这条结论 2026-11-08 已被 `--mv-shadow`
  推翻，见 `CONCLUSIONS.md` §2）。
- 现状只把 jitter **数值**传进去（`bridge.cpp:6127-6134`），却没说 MV 里还带着抖动。
  于是库会把每帧 ±0.5 px 的 Halton 抖动当成**真实位移**去对齐历史 ⇒
  静止相机下是边缘细闪，**有运动物体时就是模型边缘糊/拖影**——与"只剩下 UE5 模型
  运动伪影"的描述吻合。
- **已实现（2026-11-09）**：
  - add-on `src/game_capture/in_ngx.cpp`：`hook_create` 读
    `NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags`（`nvsdk_ngx_defs.h:767`），日志打
    `dlss feature flags 0x...: MVJittered=1 MVLowRes=.. DepthInverted=..`，
    并把它作为 `kInputFlagMvJittered`（+ `kInputFlagHiResMv`）上报；**`DepthInverted`
    故意没有转发**，那会让 bridge 对每个反向 Z 游戏自动开 `INVERTED_DEPTH`，是另一轮独立 A/B
    （`HANDOFF.md` "伪影第四轮"）。
  - bridge `src/bridge/bridge.cpp`：`adopt_jittered_mv_hint()` 在 `peek_producer()` 与
    `open_shared()` 两处消费这个 hint，等价于自动加 `--fg-jittered-mv`；init flags 里
    `if (g_fg_jittered_mv) ip.initFlags |= XEFG_SWAPCHAIN_INIT_FLAG_JITTERED_MV;`
    并打印 `initFlags=0x10 (inverted_depth=0 jittered_mv=1)`。
  - **自动路径的范围**：只有"游戏 MV"那条路会被自动改动。`--fg-of`（我们自己算的光流）
    与 `--no-mv`（没有 MV）被 `if (g_no_mv || g_fg_of) return;` 明确排除，
    行为与改动前逐字节相同；想在这两条路上试就显式 `--fg-jittered-mv`。
  - 三个开关的组合就是一次干净的 A/B：① 不传（自动跟随游戏）② `--fg-no-jittered-mv`
    （只关这一个标志）③ `--no-mv`（完全不喂 MV）。脚本：
    `scripts/recipes/run_ue5_mv_ab.bat <game.exe> [auto|nojit|nomv]`。
  - 顺带一提：`in_ffx.cpp`（FSR 的 `JITTER_CANCELLATION` 未开）与 `in_xess.cpp`
    （`XESS_INIT_FLAG_JITTERED_MV`）**一直**在报这个标志，所以这次改动也会让那两类
    输入源真正开始置 `JITTERED_MV`——与 OptiScaler 的语义一致，但 A/B 时要知道这一点。
  - **仍需实测**：`docs/ops/CONFIG.md` §4 的开关表已写，结论待 UE5 三款游戏跑完再回填本文。

### 5.2 MV 降采样后 scale 没跟着改（`StreamHeight=1440`）

tag 的 MV 是 2560×1440，`motionVectorScale` 还是 3840/2160（`bridge.cpp:6138`）。
`game_capture.cpp:1098-1111` 的 `g_mv_scale_x/y` 只在 `MvNormalize=1|2` / `MvFlip=1`
时才非 1——降采样本身不改比例。

- 实验：同一 4K 渲染场景，`StreamHeight=2160` vs `1440` 各跑一次，看模型边缘；
  若 1440 更糊/更拖，就在 mv pass 里乘 `stream/render` 比例（`ds_run` 的常量槽
  `k[4]`，`game_capture.cpp:827-829`），一次改动、零额外带宽。

### 5.3 远平面：OptiScaler 对 Unreal 用 **75000**，我们给的是"无限远"

我们合成的反向 Z 投影是 `m[10]=0`（无限远，`bridge.cpp:706-716`），回退路径
`zf=10000`（:6111）。而 OptiScaler 明确写"XeFG 不接无限远，UE 自己送的是有限远平面"。

- 实验：加 `--fg-zf <f>`（现在只有 `--fg-fov` / `--fg-zn`，见 `bridge.cpp:6956-6960`），
  UE5 下试 75000，看深度参与的重投影/遮挡判定是否更稳（模型与背景交界处）。

### 5.4 "UE5 的 MV 是垃圾"这个旧结论该重测

`HANDOFF.md` §8.6.1 第 3 条：PBZ 的 MV 3% NaN、成片 ±40000、**y 中位 −56 且随行线性变化**。
但"y 随行线性变化"恰恰是**带旋转的相机**该有的形状；而当时的判据是"全屏 FFX 光流均值 +
k ∈ {1, 3840, 1/64, 1/256} 几档缩放"的相关性，**没有覆盖"UV × 纹理尺寸"这种组合**。

- 实验：用现在的 `--dump` + `scripts/analyse_dump.py`（会分别比 `i-1→i` 与 `i→i+1`，
  并给出 depth 自身的位移）在影之刃/明末上重跑一次；画面里要**有变速运动**。
- 若 MV 实际可用：UE5 的推荐配方应从 `--no-mv` 改成 **2x + 游戏 MV + `--fg-jittered-mv`**，
  `--no-mv` 降级为"3x/4x 更省"的建议（与 `CONCLUSIONS.md` §2 的分工表一致）。

### 5.5 结构差异：它天然同帧，我们是"槽位轮转配对"

OptiScaler 的 MV 就在同一帧那次 dispatch 的参数里，不存在配对问题；
我们的颜色来自 WGC、MV/深度来自共享槽，靠轮转近似配对（`ROADMAP.md` §5）。
这条短期只能靠 `frame_id ↔ SystemRelativeTime` 的校验来**量化**，别急着改架构。

---

## 6. 一句话总结

OptiScaler 对 UE5 的全部"特殊处理"只有三条：**认出 Unreal**、**把无限远平面换成 75000**、
**把驱动版本伪装成 10.10**；影之刃/黑猴/明末在它的表里分别只是"扩大搜索"、"跳过前 10 帧"、
"伪造注册表让菜单出现 DLSS-FG"。
**MV 的正确性它靠的是"照抄游戏声明的 DLSS flags"**（尤其 `MVJittered` / `MVLowRes` /
`DepthInverted`），而不是逐游戏调参。
我们缺的正是这一步：`in_ngx.cpp` 把 flags 硬编码成 0，bridge 也不再把
`JITTERED_MV` 传给 XeFG —— 这是"UE5 模型运动伪影"目前最值得先验证的一条。

---

## 7. 实测结果（PBZ / 影之刃，2026-11-09）—— 5.1 的假设被**证伪**

### 7.1 游戏自己怎么声明（add-on 日志，`Log=1`）

```
[07:25:03.882] dlss feature flags 0x4B: MVJittered=0 MVLowRes=1 DepthInverted=1 (feature 1)
```

`0x4B = 0x01 IsHDR | 0x02 MVLowRes | 0x08 DepthInverted | 0x40 AutoExposure`：

| 位 | 值 | 含义 | 我们该做什么 |
|---|---|---|---|
| `MVLowRes` | **1** | MV 在渲染分辨率（1280x720 / 2560x1440），不是 4K | ✅ XeFG 正好要这个 |
| `MVJittered` | **0** | **MV 已经去过抖动** | ❌ 不能置 `JITTERED_MV`；强行置是错的 |
| `DepthInverted` | **1** | 反向 Z（与 HANDOFF §8-38 的推测一致，现在有游戏自己的证据） | ✅ 应该置 `INVERTED_DEPTH` |
| `IsHDR` / `AutoExposure` | 1 / 1 | 与 FG 无关 | — |

⇒ **§5.1 的假设（"UE5 的 MV 带抖动，加 `JITTERED_MV` 就能修"）对 PBZ 不成立。**
`MVJittered=0` 是游戏自己的声明，强行打开只会让库去减一个不存在的抖动。

### 7.2 四档 A/B（2x @ 60 fps 锁帧，`input=16.7ms = 2 vblanks`，`cadence` 无 `NOT-EXACT`）

| 档 | 结果 |
|---|---|
| `auto`（不置 JITTERED_MV，实际按声明） | 伪影 + **闪烁** |
| `jit`（`--fg-jittered-mv` 强制置上） | 伪影 + 闪烁（与 auto 无改善） |
| `depthinv`（`--fg-depth-inv`） | 伪影 + 闪烁（与 auto 无改善） |
| `nomv`（`--no-mv`） | **不闪**，仍有伪影 |

两条结论：

1. **闪烁来自 MV 流本身**，与 `JITTERED_MV`、与深度方向都无关 —— 关掉 MV 就不闪了；
2. **伪影在不喂 MV 时依然存在** ⇒ 至少还有一个与 MV 无关的来源（相机/投影近似、
   历史对齐，或 UE5 本身的 TSR 残留）。

### 7.3 下一个测量点：MV 的单位（`MV.Scale`）

同一批日志里 `frame constants: ... mvscale=(1.0000, 1.0000)`，而 CP2077 报的是
`(2560, 1440)`。XeFG 文档写的是 **"screen-space motion in pixels"**
（`third_party/xess/doc/xess_fg_developer_guide_english.md:809`），所以"游戏报 1.0"到底
意味着"texel 已经是像素"还是"texel 是 UV"，必须量出来，不能猜。

- 工具已就绪：`scripts/analyse_dump.py` 新增 **units fit** 段，对 dump 出来的
  `fNN_mv.raw` 与相邻帧真实位移做最小二乘，拟合 `motion_px = mv_raw × k`，
  并给出"最接近哪种约定"（`k=1` 像素 / `k=宽高` UV / `k=宽高/2` NDC）；
- 复现：`4 - dump frames.bat PBZ.exe 5`（画面里要有**变速**运动）→
  `python3 scripts/analyse_dump.py <dump-dir>`；
- 若拟合出来是 UV ⇒ `--fg-mv-scale px` 就是正确设置（已实现）；
  若是像素 ⇒ 现在的 `raw` 本来就对，MV 的单位这条可以永久排除。

### 7.4 dump 的答案：**这条 MV 流本身不可用**（2026-11-09 07:38，PBZ 720p 渲染）

`7 - UE5 MV A-B.bat PBZ.exe dump 1` → 5 帧 `fNN_{colour,depth,mv}.raw` + `meta`，
用 `scripts/analyse_dump.py` 与本仓库的临时统计脚本分析：

| 观察 | 数值 |
|---|---|
| 非有限值 | **0.44–0.53%**（其中 NaN 0.03%，其余为 −inf） |
| `\|x\|` 分布 | 中位数 **0.000**、51% 落在 `\|x\|<1`、**17% 落在 `\|x\|>1000`**（最大 ±65504 = fp16 饱和） |
| `\|y\|` 分布 | 中位数 **+31.3**（而真实位移换算到渲染像素只有 ≈ −0.2） |
| 与真实位移的相关 | tile 级 `corr = −0.03 (x) / −0.045 (y)` —— **≈0** |
| `MV.Scale` | `(1, 1)`（游戏自己报的） |
| 深度 | 正常（`min 0 / max 0.028 / mean 0.010`，反向 Z，块匹配能解出位移） |

三种"单位"假设都解释不了这组数：真值在渲染像素下应≈`(+11.6, −0.2)`，UV 下≈`(+0.009, −0.0002)`，
而实测是"x≈0、y≈+31、17% 巨大值、0.5% NaN/inf"。

⇒ **结论：PBZ 交给 NGX 的那张 `MotionVectors` 资源里不是可用的速度场。**
这与 `HANDOFF.md` §8.6 在 2026-11-07 得出的旧结论一致（当时被"MV 是死输入"的争论盖过去了，
现在有了量化证据）；`--mv-shadow` 证明的是"XeFG 会消费 MV"，不代表内容有效。

**所以对 UE5 / PBZ，正确做法不是调 flag、也不是换算单位，而是换掉这条流：**

| 方案 | 结果（已实测/预期） |
|---|---|
| `--no-mv` | 不闪，仍有边角伪影（MV 为空，靠库自己估相机运动） |
| **`--fg-of`（我们的 FidelityFX 光流）** | 待测：MV 由我们自己从捕获颜色算（0.3 ms），时间上自洽、没有 NaN/饱和 |
| 游戏 MV（任何 flag/单位组合） | 伪影 + 闪烁 —— **不可用** |

`7 - UE5 MV A-B.bat PBZ.exe of 1` 就是下一档要跑的对照（2x @ 60，和 `nomv 1` 比）。

### 7.5 `--fg-of` 的第一版结果——**我们自己把 scale 用错了**（2026-11-09 07:44）

`of` 档实测：**不闪，伪影与 `nomv` 差不多**。原因写在它自己的日志里：

```
[bridge] --fg-of: FidelityFX optical flow at 3840x2160, field 480x270 -> tagged at 1278x720
[bridge] frame constants: ... mvscale=(1.0000, 1.0000)
```

我们的光流场是以 **screen UV** 写进 MV 纹理的（`uv = flow_px / capture_size`），
但 `motionVectorScale` 还在透传**游戏**报的 `MV.Scale`（PBZ = 1.0）。
于是库把 `0.009` 当成 **0.009 像素**，而不是 35 像素 ⇒ 运动信息等价于零 ⇒
`of` 表现等于 `nomv`。（同理，历史上"光流不闪"的结论其实也可能是"光流没生效"。）

**已修**：`own_mv_path()`（`--fg-of`）时把 `motionVectorScaleX/Y` 设成 **MV 纹理尺寸**
（=渲染尺寸，和游戏报可用 scale 时的口径一致，例如 CP2077 的 2560x1440），
日志会变成 `mvscale=(1278.0000, 720.0000)`。

同时新增 `ofsign` 变体（`--fg-of --fg-of-sign -1`）：以前 MV 近似为零，符号从来没被验证过，
现在第一次真正生效，需要用一档 A/B 确认 `(previous − current)` 的朝向。
另加 `offov` 变体（`--fg-of --fg-fov 58.7`）：`HANDOFF.md` §0.1 当年在 PBZ 上测出
**58.7° 干净、55/62 会重新出现人物边缘伪影**，而脚本默认用的是 60.0 —— 这一档就是把它也一起换掉。

### 7.6 `of` 真正生效之后：**伪影与 MV、与 FOV 都无关**（2026-11-09 07:48–07:50）

三档日志确认 scale 修好了（`mvscale=(1278.0000, 720.0000)`），而 `of` / `ofsign` / `offov`
（58.7°）与 `nomv` 的观感**仍然一样**。已经可以排除的：

| 变量 | 怎么排掉的 |
|---|---|
| MV 内容（游戏那条） | NaN/inf + 饱和 + 相关性 ≈0，任何 flag/单位组合都闪 |
| MV 来源（换成我们自己的光流） | `of` 生效后仍一样 |
| MV 符号 `(prev−cur)` | `ofsign` 取反后仍一样 |
| 相机 FOV（60 → 58.7） | `offov` 仍一样 |

⇒ 剩下的嫌疑集中在**两条路都共用的东西**上：

1. **历史对齐 / jitter**：我们的颜色是**上采样之后**的画面（DLSS 已经把 jitter 解掉了），
   却仍然告诉 XeFG "这一帧带了 j₀ 的抖动"。若库按 j₀ 去挪历史，就会得到**系统性亚像素错位**
   —— 表现为所有运动物体边缘的糊/双影，且换任何 MV 都不会变。
   开关：`--fg-jitter-zero`（2026-11-09 起重新生效，见脚本变体 `jz`）。
   注意：2026-11-08 "传真实 jitter 更好" 的结论是在 MV 是垃圾的前提下测的，需要重测。
2. **深度**（去遮挡判定）：`nodepth` 变体（`--fg-of --no-depth`）一出就能判定。
3. **游戏自己的 720p→4K**：本轮测试把 DLSS 换成了**性能档**（渲染 1278x720），
   游戏本身的放大伪影也可能就在其中 —— 需要关掉 FG 看一眼、或换回质量档对照。

为了能"直接看见"插值帧，脚本还加了 `only` 变体（`--fg-debug-only-interp`，只上生成的帧）。

---

## 8. 最终结论（2026-11-09 08:00）—— 伪影的主因是**放大倍率**，不是 MV

用户在同一台机器上做了最后一组对照：

| 配置 | 渲染 → 输出 | 伪影 |
|---|---|---|
| 游戏本身（**不插帧**） | 1278x720 → 3840x2160（DLSS 性能，**3 倍**） | **本身就有一点点** |
| `auto`/`jit`/`nomv`/`of` 都在这档位测 | 同上 | 都有伪影（换 MV 来源/符号/FOV 都不变） |
| **DLAA + `--fg-of`** | **3840x2160 → 3840x2160（1 倍）** | **干净** |

日志可以直接对上：`of` 那档结尾是
`frame constants: ... aspect=1.77778 (3840x2160)`（渲染=输出=4K），
`mvscale=(2560.0000, 1440.0000)`、`of=0.3ms`；前面几档是 `(1279x720)`。

### 为什么

我们的 FG **插的是上采样之后的画面**（WGC 抓的是 DLSS/TSR 的输出）。上采样倍率越大，
游戏自己那张图的时域不稳定（DLSS 自身的 history/denoise、锐化）就越明显，插帧会把它们
当成"真实内容"再叠一层 ⇒ 模型边缘糊/双影。放到 1:1（DLAA）时上游没有放大，源头干净，插帧也就干净。

这也解释了整轮实验里那些"怎么换都没用"的现象：**MV 来源、MV 符号、FOV、jitter 都不是主因**，
它们在 720p 那档被放大伪影完全淹没了。

### 因此，UE5 / 高放大倍率场景的推荐

1. **画质优先**：DLAA（或至少 DLSS 质量档）+ **`--fg-of`** —— 实测干净；
2. 必须用性能档时：加不加 FG 都有伪影，且伪影主要由**放大倍率**决定 ⇒ 期待值要按"放大后的画面"来定；
3. 游戏那条 MV 流（PBZ）不要用：NaN/inf + 饱和 + 相关性 ≈0（§7.4）。

### 仍然成立的附带结论（与 UE5 无关，对所有游戏有效）

- add-on 读 `DLSS.Feature.Create.Flags` 并转发 `MVJittered` / `MVLowRes` / `DepthInverted`；
  bridge 在创建 swap chain 前再读一次，迟到会 WARNING；
- `--fg-of` 的 `motionVectorScale` 必须用 **MV 纹理尺寸**（以前透传游戏的 1.0 ⇒ 光流等价于失效）；
- `--fg-mv-scale raw|px|<float>` 可用于验证"游戏 MV 的单位"；
- `scripts/analyse_dump.py` 的 **units fit** 段可以对任何 dump 直接判单位与配对。

---

## 9. 逐游戏记录：UE5 的运动矢量**不能一概而论**（2026-11-09 起）

§8 的结论是"PBZ/影之刃这条 MV 流不要用"，但**那只对那个游戏成立**。后续实测说明：
UE5 各版本 / 各游戏把 MV 交给 NGX 的方式（单位、符号、有没有抖动）不统一，**必须按游戏试**。

| 游戏 | 结论 | 依据 |
|---|---|---|
| **影之刃：零**（PBZ） | **AMD 光流**（`--fg-of`） | 游戏 MV 流是坏数据：0.5% NaN/inf、17% 饱和到 fp16 上限、与真实位移相关性 ≈0（§7.4） |
| **明末：渊虚之羽** | **游戏 MV（反向）**（`--mv-flip`） | MV 本身正确，只是符号与 XeFG 的 `previous-current` 约定相反 ⇒ 反向即干净，不必上光流 |
| 黑神话：悟空 | 待记录 | — |

**试的顺序**（从最省到最贵）：`游戏 MV` → `游戏 MV (反向)`(`--mv-flip`) → `AMD 光流`(`--fg-of`)
→ `关闭`(`--no-mv`)。判据：慢速平移看物体边缘有没有闪烁/拖影/鬼影；**符号反了的表现是
边缘沿运动方向撕裂**。

> 这一节是对 §8 的补充而不是推翻：§8 说"伪影主因是放大倍率"，那一半仍然成立；
> 这一节说"MV 的来源与符号要按游戏定"，别把影之刃的结论直接套到别的 UE5 游戏上。

