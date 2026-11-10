# 输入语义：怎么做到"每个游戏都能用"（而不是逐游戏打补丁）

> 日期：2026-11-10　状态：**C 已实现、A-v1 已实现**（add-on only，未动协议）
> 代码：`src/game_capture/game_capture.cpp`（`ds_run` / `probe_*`）、`temp/ds_shader.hlsl` → `src/mgpu_ds_shaders.h`
> 背景数据：`docs/findings/FG_SNIFF.md` §9/§10（PBZ 四轮实测）

## 1. "每个游戏不一样"其实是四类，只有一类真的需要特例

PBZ 四轮实测把四类全演了一遍：

| 类别 | PBZ 实例 | 通用做法 |
|---|---|---|
| ① 有声明，只是没读 | `DepthInverted`、`MVLowRes`、`MotionVectorsJittered`、`mvecScale`、`motionVectorsInvalidValue`、SL 的 `Extent` | 读声明，零特例 |
| ② 声明缺了或错了 | `cameraFOV=75.01°` 其实是**水平** FOV；`cameraFar=75000` 而矩阵是无限远；jitter 被烤进投影矩阵；NGX 报 `MV.Scale=1.0` 而 SL 报 `1/2562`（两张不同纹理） | **量**，不能信 |
| ③ 结构差异 | FG 可能由游戏外产生（unlocker / 驱动），此时没有任何 API 可读 | **不依赖它**：SR 路径才是主粮 |
| ④ 只能降级 | 深度纹理 2564×1444 但有效区是 2562×1441；有的游戏直接不给深度/相机 | 降级阶梯 + 说明理由 |

⇒ 通用方案 = **声明优先，缺了才量，量不出来才降级**，并且把量出来的结果当数据缓存。
不是"每款游戏一组开关"：查过 `temp/optiscaler_src` 之后可以确定，**OptiScaler 的 106 条游戏表里
一条 MV/深度语义都没有**（详见 §1.5），他们同样靠声明。

## 1.5 决策顺序：声明 > 覆盖表 > 测量（2026-11-10 查证 OptiScaler 后修正）

| 层 | 来源 | 何时用 |
|---|---|---|
| 1 | **API 声明** | 永远优先。NGX 的 `MV.Scale`/`DepthInverted`/`MVJittered`、XeSS 的速度空间、FSR 的 near/far/vfov |
| 2 | 用户覆盖（ini / 命令行） | 声明缺失，或已知某个标题说谎（`MvNormalize=0/1/2`、`MvFlip=1`、`--fg-fov`） |
| 3 | **在线测量**（`probe:`） | 只剩这一层时才用，且只补空，不覆盖 |

**为什么不是"抄一张游戏表"**（实测证据，都在同仓 `temp/optiscaler_src/OptiScaler-master/`）：

1. `misc/GameConfigs.cpp` 共 106 条，可设置的键只有 `DxgiSpoofing` / `FGAllowedFrameAhead` /
   `ColorResourceBarrier` / `RestoreComputeSignature` / `SkipFirstFrames` 这类**管道**开关
   （见该文件 `GAME_SETTING(...)` 列表）—— **没有** `MotionVector`、`DepthInverted`、`Jitter`。
2. 单位他们根本不判：`NVSDK_NGX_Parameter_MV_Scale_X/Y` 直接抄进 FSR/XeSS 的
   `motionVectorScale`（`upscalers/fsr2_212/FSR2Feature_Dx12_212.cpp:255-266`），
   深度方向用 init flag（`IFeature::DepthInverted()`）。
3. **按 exe 的表对 PBZ 会是错的**：同一个 `PBZ.exe`，FSR 会话声明 `MV.Scale≈2880`（实测 uv），
   DLSS 会话声明 `MV.Scale=1.0`（实测 pixels）。表无法表达"随 API/档位变"。

⇒ 我们能复用的是他们的**结论（信声明）**，不是他们的表。加上 §0 那条关键读法：
`像素 = 纹素值 × MV.Scale`，于是 `MV.Scale=1.0` **不是"不可知"，而是"已经是像素"**。

## 2. 已实现

先语义层（① ② ③：把三家的声明翻成同一套词），再数据层（C 有效区裁切、A-v1 单位探针）。

### ① 归一化字段（`inputs.h`）

`FrameInputs` 新增：

```cpp
enum class MvUnit       { unknown, pixels, uv, ndc };
enum class MvSign       { unknown, prev_minus_cur, cur_minus_prev };
enum class CameraSource { none, declared, matrix };

MvUnit mv_unit; MvSign mv_sign; CameraSource camera_source;
float invalid_marker; uint32_t valid_w, valid_h;
```

规矩：**源知道就填，不知道就 `unknown`**。`unknown` 不是失败，是"等探针量"。
消费端今后只读这些字段，永远不写 `if (vendor == ...)`。（暂不进共享协议，见 §4 的 B 步。）

### ② 三个源的声明映射（纯查表，零实测成本）

| 源 | mv_unit | camera | 依据 |
|---|---|---|---|
| **NGX** | `scale≈1` → `pixels`；`≈渲染宽` → `uv`；`≈宽/2` → `ndc` | `none` | `像素 = 纹素 × MV.Scale`（DLSS 就这么消费，OptiScaler 也原样转发）⇒ 声明直接给单位 |
| **XeSS** | `USE_NDC_VELOCITY ? ndc : uv` | `none` | SDK 直接说了速度空间，这是三家唯一"明说"的 |
| **FSR 2.2/3.x** | `uv` | `declared`（有 `kInputFlagHasCamera` 时） | FSR 导出真实 near/far/vfov，`motionVectorScale` 是像素口径 ⇒ 纹素 UV |
| 全部 | `mv_sign = prev_minus_cur` | | 所有超分 API 都定义成"上一帧−当前帧"；反转会弄坏游戏自己的 DLSS。`MvFlip=1` 留给说谎的标题 |

日志（每变化一次一行）：
```
semantics: mv=pixels sign=prev-cur camera=declared depth=reversed valid=2562x1441
```
`mv=unknown` 现在只在 API 真的什么都没说时出现（例如只拿到深度、没有超分路径）—— 那才是探针的活。

### A-v2. 符号（±mv 深度配对胜负）

之前 `mv_sign` 只能写 `unknown`，因为没有任何 API 声明方向。现在用**两帧连拍**量出来：

- 探针每轮连拍 **两帧**（gen0 = 上一帧，gen1 = 当前帧），MV 与深度**在同样 4 个位置**各取一块
  （128×128，4 个位置覆盖上中/下中/左中/右中），全部塞进同一对 READBACK 缓冲，仍然一次 fence、
  一次 Map、不阻塞；
- CPU 上做**配对比较**：把上一帧深度沿 **+mv** 和 **−mv** 分别双线性重采样到当前帧的位置，
  逐纹素问"哪边更接近当前帧的深度"，数**胜率**；
  `+mv` 过半 ⇒ 约定是 `prev − cur`（XeFG/FSR 要的那种），`−mv` 过半 ⇒ `cur − prev`（游戏向量反了）；
- **为什么是胜率而不是残差求和**：求和会被少数去遮挡/远平面的巨大误差点主导。PBZ 实测
  `plus=431.7979 minus=539.9221`（22,206 个采样，均值 0.0195 vs 0.0243）—— 真差 20%，
  但除以样本数后只剩 1.9%，被 0.85 的阈值判成 `ambiguous`。胜率是投票制：
  离 50% 越远越可信，`z > 5`（p < 1e-6）在 22k 票上已经是不可能由噪声产生的偏差。
- **判据是 z 分数 + 残差差，不是胜率本身**。第一版按 `|胜率−0.5|` 排序，结果 446 票的
  `uv` 把 22,206 票的 `pixels` 挤掉了（03:21 实测）。现在：
  1. 每个候选必须 `used ≥ 2000`（4×126×126 = 63,504 个位置的 3%）；
  2. 排序用二项 z 分数 `(wins − n/2)/√(n/4)`；
  3. 胜出方向还要 `mean|d|` 至少差 **2%**，否则写
     `unknown (votes lean one way but both offsets fit equally well)` —— 投票多数但残差只差
     0.3%（实测 `plus=0.00323 minus=0.00322`）说明两边根本没区别。
- `quality = 2 × |胜率 − 0.5|`（0..1）：0.5/0.5 的平局 = 0，一边倒 = 1。

日志：
```
probe: sign scale=uv used=41230 static=1877 big=0 wins=79.4% z=119.3 mean|d| plus=0.0612 minus=0.1487 -> prev-cur (+mv aligns)  quality=0.59
probe: sign scale=pixels used=38877 static=910 big=0 wins=20.6% z=116.0 mean|d| plus=0.1502 minus=0.0598 -> cur-prev (-mv aligns)  (game vectors are flipped)
probe: sign scale=uv used=446 static=1 big=61000 wins=40.4% z=4.1 mean|d| plus=0.00323 minus=0.00322 -> unknown (votes lean one way but both offsets fit equally well)  quality=0.19
```

`scale=` 这一项是**顺带量出的 MV 单位**：三种候选换算（pixels / uv / ndc）各算一遍胜率，
胜出的那个就是单位，会覆盖 `kSemMvUnit*`。所以一行日志同时给出**单位 + 符号 + 质量**。

**前提**：需要 (a) 有深度、(b) 场景在动、(c) 两帧都被拷到。所以 **XeSS 段不会有这行**（PBZ 的 XeSS
不给深度），静止画面也会是 `unknown` —— 这些都是如实报告，不是失败。

**必须记住的坑（2026-11-10 实测踩到）**：

1. **偏移必须换算到 patch 纹素**。流里的值已经是"像素"（`ds_run` 乘过 `mvScale`），但很多游戏的
   源是 UV（`mean|v|≈0.009`）。第一版直接把它当像素偏移用 ⇒ 四舍五入成 **0** ⇒ `+mv` 和 `−mv`
   采样同一个纹素 ⇒ `plus`/`minus` **完全相同**（实测 `2110.6384 / 2110.6384`）。
   现在的做法：三种换算都试，让胜率决定。单一路程的经验值：`pixels` 用 `stream/render` 比，
   `uv` 用 `stream` 宽，`ndc` 用 `stream/2`。
2. **深度采样必须双线性**。UV 源在 128 的 patch 上只有 ~0.45 纹素偏移，最近邻采不出来；
   双线性才能反映亚像素差异。
3. **小于 1/4 纹素的运动不能参与投票** —— 两侧偏移落在同一批纹素里，只有插值噪声，
   会把胜率拉回 50%。这些点单独计入 `static=`。
4. **两个极点都要排除**（反向 Z 的天空 = 0，正向 Z 的天空 = 1）：那里的相对深度误差没有意义，
   矢量通常也是无效值。规则写成两端都排除，与方向无关。
5. 声明会**随会话/档位变**：同一个 PBZ，有时报 `MV.Scale=1.0`（⇒ `unknown`），有时报
   `≈2880`（⇒ `uv`，且实测同意）。**不要把声明按游戏缓存**，每帧都以当前源为准。
6. **patch 必须比运动量大**。PBZ 正常转视角是 **33~56 像素/帧**，128 的 patch + 16 纹素上限
   直接扔掉了 73% 的 texel（`big=46222/63504`），于是正确尺度没票、错尺度赢。
   现在 `kProbeDim = 256`、上限 `kProbeDim/4 = 64`，并要求 warp 采样点落在 patch 内（`out=`）。
7. **平坦区域不投票**。`slope_per_texel × |offset| < 2% × depth` 的 texel 不参与：那里 ±mv
   采样结果相同，投票只是舍入噪声（实测 `plus=0.00323 minus=0.00322`）。计入 `flat=`。
   这一条同时解释了为什么远景贴墙的 patch 上永远测不出符号。



`probe:` 现在会带上声明值并给出一致/矛盾判断：

```
probe: mv mean|v|=(3.421,0.113) invalid=0.4% large=2.1% -> unit=pixels
       [...]  declared=unknown  sign=unknown (needs previous-frame depth)
probe: … -> unit=UV  declared=uv  (agrees with the declaration)
probe: … -> unit=pixels  declared=uv  (DISAGREES with the declaration - trust the measurement)
```

**深度方向也一起量**（同一套 readback，+16 KB）：反向 Z 的远平面是 0（天空=0），正向 Z 的远平面是 1，
所以 64×64 的一块就够判：

```
probe: depth min=0.0000 mean=0.0721 max=1.0000 far0=71% near1=3% bad=0.0%
       -> reversed-Z (far=0)  declared=reversed  (agrees with the declaration)
```

这条是**实测逼出来的**：同一局里 DLSS/FSR 声明 `depth=reversed`，XeSS 却声明 `depth=forward`
（`semantics:` 三行见 §2），而 XeSS 拿到的极可能就是同一张深度缓冲。有了这条，谁在说谎一眼可见。



### C. 有效区裁切（`crop:`）

问题：PBZ 的深度纹理是 **2564×1444**，而渲染分辨率（也是 SL 声明的 tag extent）是
**2562×1441**。`ds_run` 原来按整张纹理归一化采样 ⇒ 深度场被拉伸 0.08%，越靠右/下越偏
（约 1–2 像素），而这正好落在"运动物体边缘轻微拖影"的观感上。

做法：
- `temp/ds_shader.hlsl` 的常量块从 `{float2 mvScale; float2 pad;}` 改成
  `{float2 mvScale; float2 uvMax;}`，采样变成 `src.SampleLevel(smp, i.uv * uvMax, 0)`；
- `ds_run` 用 **NGX 上报的 render 尺寸**当"有效区"，只有当纹理比它大 **且超出 ≤16 像素**时
  才裁（`uvMax = valid/tex`）；超出更多（动态分辨率子矩形、行填充）保持旧行为并**打一行警告**；
- `mvScale` 和 `uvMax` 共用原来就有的 4 个 32-bit 常量槽，根签名/PSO 都没动。

日志（每变化一次打一行）：
```
crop: depth tex 2564x1444 -> valid 2562x1441 (uvMax=0.99922,0.99792)
crop: mv    tex 2562x1441 -> valid 2562x1441            ← 这张不需要裁，不打
```
代价：0（同一个 draw，多两次乘加）。

### A-v1. 在线量 MV 单位（`probe:`）

问题：MV 是"像素"还是"UV"决定了 `motionVectorScale` 该给 1 还是 1/w，以前只能靠离线
`--dump` + `analyse_dump.py` 拟合。

做法（**只读、非阻塞**）：
- 每 **120 帧**（约 2 s）把 MV 流左上 **64×64** 拷进一个 READBACK 缓冲：拷贝正好发生在
  `ds_run` 里 MV 目标已经是 `COPY_SOURCE` 的那一刻，零额外状态转换；
- 用**自己的 fence**（在 present 回调里从游戏队列 Signal，1 帧滞后）确认 GPU 完成后才 `Map`，
  永不阻塞；16 KB 的读回，比一次 dump 便宜 4 个数量级；
- CPU 侧统计：有效纹理数、`invalid`（NaN/inf/FLT_MAX 饱和）、`large`（|v|>1000）、平均 |v|；
- 先除以 `mvScale` 还原成**游戏原样**的场，再判单位：
  `mean|v| > 0.5 → pixels` / `< 0.02 → UV` / 中间 → `ambiguous`（场景几乎静止，下次再测）。

日志：
```
probe: mv unit detection ON (64x64 patch, every 120 frames)
probe: mv mean|v|=(3.421,0.113) invalid=0.4% large=2.1% -> unit=pixels
       [as pixels 3.4 px | as UV 3.421 screens]  sign=unknown (needs previous-frame depth)
```

**为什么不做符号**：符号必须有"上一帧"的地面真值（把 prev-depth 沿 ±mv 重采样，比残差）。
那需要一个 ping-pong 深度 RT，本轮不动 —— 与其猜，不如在日志里写 `sign=unknown`。

**前提（要知道）**：探针测的就是 MV 流，而 MV 流只有在**消费端请求**时才产出（`[capture] SkipMv=0`
的含义是"听 bridge 的 `kRequestMv` 位"）。所以要么跑 bridge、要么在 UI 里打开 MV，才会出现
`probe:`。没被请求时日志会打一行：
`[mv] the consumer did not ask for motion vectors, so 'probe:' stays silent - …`

## 3. 这次测试要看的三件事

1. `crop: depth tex 2564x1444 -> valid 2562x1441` 出现一次（PBZ 必现）；
2. `probe: ... unit=pixels`（或 `UV`）在**有运动的场景**里稳定出现，且
   `invalid` 比例与之前 dump 的结论（0.4–0.5%）对得上；
3. `perf: frames=… begin=…` 与上一版持平（探针不该带来可测量的开销）。

如果 `probe:` 一直 `ambiguous`，说明采样窗口正好落在静止画面 —— 边跑边转视角即可。

## 4. 还没做（下一步，按风险从低到高）

| 步 | 内容 | 收益 |
|---|---|---|
| ③ | 模块发现改成"按导出入口 + 是否在被调用"（现在还是按文件名匹配 `nvngx`/`libxess`/`amd_fidelityfx`） | 覆盖插件目录、改名代理、游戏自带运行时 |
| A-v3 | 用 A-v2 的 `quality`（残差比）做**质量分**：低于阈值自动降级到 `--fg-of` | 干掉"这游戏 MV 是垃圾"的人工判断 |
| D | `profiles/<exe>.json` 缓存测量结果（单位/符号/FOV/深度约定） | 首次 2 秒标定，之后零配置 |
| B | 协议 v11：把归一化字段 + `cameraViewToClip`（jitter 置零）发给 bridge | 干掉 `--fg-fov`/`--fg-zn`/`--fg-zf`/`--fg-mv-scale` |

## 5. 三条反模式（踩过的坑）

1. **逐游戏硬编码开关表** —— 永远落后于新游戏；要"量"不要"记"。
2. **钩引擎内部结构（UE 对象偏移 / RVA 字节补丁）** —— 版本一变就废（SM86 解锁器就是范例）。
3. **架构依赖"游戏自己的 FG 开着"** —— 实测 PBZ 的 FG 可以在游戏外产生，完全没有 API 可读；
   颜色/深度/MV/相机从 SR 路径拿就够，FG 那条路只用来**交叉验证**。
