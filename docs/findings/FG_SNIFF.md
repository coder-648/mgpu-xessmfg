# 只读地抓游戏自己的帧生成（`[capture] FgSniff=1`）

> 日期：2026-11-10　状态：**已实现，待实测**（只改 add-on，不碰 bridge / 协议）
> 代码：`src/game_capture/in_fg.cpp`　开关：`game_capture.ini` 的 `[capture] FgSniff=1` + `Log=1`
> 相关：`docs/findings/UE5_MV_OPTISCALER.md`（UE5 的 MV 实测）、`docs/design/INPUT_SOURCES.md`（现有的三个输入源）

## 1. 它解决什么

`in_ngx.cpp` 钩的是**超分**（DLSS-SR），那条路**不导出相机**，所以 bridge 只能用
`--fg-fov` / `--fg-zn` 猜投影（`src/bridge/bridge.cpp:6502`）。而**帧生成 API 必须拿到
完整相机**才能重投影自己 —— 我们一直在猜的那些数，游戏早就一帧一帧地声明好了。

所以这个文件不产出任何输入，只把游戏交给它自己 FG 的**声明值**写进 add-on 日志：

| 想要的东西 | SR 那条路 | FG 这条路 |
|---|---|---|
| 投影矩阵 | ❌ 没有 | ✅ `cameraViewToClip` / `projectionMatrix` |
| 当前→上一帧裁剪空间 | ❌ | ✅ `clipToPrevClip` / `prevClipToClip` |
| MV 的尺度语义 | ⚠️ 游戏自报，含义不统一 | ✅ 归一化到 `[-1,1]`（SL）/ 像素（XeFG），可对照 |
| 深度方向 / 相机运动是否已在 MV 里 | ⚠️ 只从 CreateFlags 推断 | ✅ `depthInverted` / `cameraMotionIncluded` 显式给 |
| MV / 深度**是哪张纹理、什么分辨率** | 有 | 有（且是**present 那一帧**的） |
| HUDless 颜色 / UI+alpha | ❌ | ✅ 一起 tag 进来 |

## 2. 挂什么（三条路径，按游戏实际用哪个）

| 路径 | 模块 | 钩子 | 拿到 |
|---|---|---|---|
| Streamline（UE5.2+ 的 Nvidia 插件、RE Engine） | `sl.interposer.dll` | `slSetConstants`、`slSetTagForFrame`、`slSetTag`、`slEvaluateFeature`、`slDLSSGGetState` | `sl::Constants` 全套 + `sl::ResourceTag` 的原生 `ID3D12Resource` |
| 原生 NGX DLSS-G | `nvngx_dlssg.dll`（导出名带前缀 `DLSSG_NVSDK_NGX_D3D12_*`） | `CreateFeature`、`EvaluateFeature` | `DLSSG.CameraViewToClip` / `ClipToPrevClip` / `MVecs` / `Depth` / `HUDLess` / `MvecScaleX/Y` / `DepthInverted` / `MultiFrameCount` |
| 集成式 XeSS-FG | `libxess_fg.dll` | `xefgSwapChainTagFrameConstants`、`xefgSwapChainD3D12TagFrameResource`、两个 `InitFrom*` | `viewMatrix` + `projectionMatrix` + jitter + `motionVectorScale` + `resetHistory`，以及 initFlags |
| **同一条 NGX 入口**（既没有 Streamline、也没有集成式 XeSS-FG 的游戏 —— **PBZ 就是**） | 已经被 `in_ngx.cpp` 钩住的 `_nvngx.dll`（驱动 shim） | 借用 `in_ngx.cpp` 的 `NVSDK_NGX_D3D12_EvaluateFeature` 钩子（同一地址不能钩两次） | 参数块里带 `DLSSG.MVecs` 时即为帧生成调用 ⇒ 上面那套 `DLSSG.*`。日志里 `where` 是 `ngx` |

**SM86 / MFG 解锁补丁的影响：没有。** 补丁改的是 runtime 内部（`MultiFrameCountMax`、2x 模型复用），
入口没变 ⇒ MinHook 照样挂得上。反过来说，日志里
`slDLSSGGetState: ... framesToGenerateMax=3` 就是"挂到了打过补丁的那份"的最快证据
（原版报 1）。同理 `dlssg.eval` 行的 `mf=1/3` 说明游戏真的在要 3 个中间帧。

## 3. 日志长什么样

一次完整的常量块（首 3 次 / 声明变化时 / 每 1200 次各打一次）：

```
fg sniffer: ON ([capture] FgSniff=1) - read-only, never publishes
fg sniffer: Streamline  C:\...\sl.interposer.dll
fg: Streamline hooks active (5 installed, 0 not exported).
fg: sl.const #1 vp=0 ver=2 jit=(+0.250,+0.250)px mvs=(0.6667,0.3333) zn=0.1000 zf=inf fovY=1.0240rad(58.67deg) aspect=1.7778 dpi=1 cmi=1 mv3d=0 reset=0 ortho=0 mvj=0 mvd=0 mvBad=65504
  cameraViewToClip [ 0.8438 0.0000 0.0000 0.0000 | 0.0000 1.5000 0.0000 0.0000 | 0.0000 0.0000 0.0000 0.1000 | 0.0000 0.0000 -1.0000 0.0000 ]
  clipToCameraView [ ... ]
  clipToPrevClip   [ ... ]
  prevClipToClip   [ ... ]
  cameraPos=(...) up=(...) right=(...) fwd=(...) lensIdentity=1 pinhole=(0.0000,0.0000) minDepthSep=40.00
```

之后每秒一行（矩阵只在变化/每 10 秒时展开）：

```
fg: sl.const #121 jit=(-0.125,+0.375)px mvs=(0.6667,0.3333) zn=0.1000 zf=inf ...
fg: sl.tags #118 vp=0 n=4
  DEPTH             ( 0) 0x000002A1... 3840x2160 state=0x0 lc=1 extent=(0,0 3840x2160)
  MOTION_VECTORS    ( 1) 0x000002A1... 2560x1440 state=0x0 lc=1 extent=(0,0 2560x1440)
  HUDLESS_COLOR     ( 2) 0x000002A1... 3840x2160 state=0x0 lc=1
  UI_COLOR_AND_ALPHA(23) 0x000002A1... 3840x2160 state=0x0 lc=1
fg: sl.eval #118 feature=DLSS_G inputs=3 [Constants v2] [ResourceTag v1] [ResourceTag v1]
fg: slDLSSGGetState: status=1 minDim=64 presented=118 framesToGenerateMax=1  (2x only: stock runtime or a driver cap)
```

DLSS-G 原生路径：

```
fg sniffer: DLSS-G       C:\...\nvngx_dlssg.dll
fg: DLSSG hooks active (2 installed, 2 not exported).
fg: dlssg.eval#1 internal=2560x1440 mf=1/1(max=0) jit=(+0.250,+0.250) mvecScale=(0.0004,0.0007) zn=0.1000 zf=0.0 fovY=0.0000rad(0.00deg) aspect=0.0000 dpi=1 cmi=1 reset=0 hdr=1
  mv          0x... 2560x1440 fmt=10
  depth       0x... 2560x1440 fmt=40
  hudless     0x... 3840x2160 fmt=10
  ...
  cameraViewToClip [ ... ]        <- 这一行不再出现时，说明游戏**没有**给矩阵
```

XeSS-FG：

```
fg: xefg init: initFlags=0x1 invertedDepth=1 externalHeap=0 highResMV=0 ndcVelocity=0 jitteredMV=0 uiNotPremul=0 maxInterpolatedFrames=1 uiMode=0
fg: xefg.const #1 presentId=42 jit=(+0.250,+0.250)px mvs=(2560.000,1440.000) reset=0 frameTime=16.667ms
  viewMatrix       [ ... ]
  projectionMatrix [ ... ]
fg: xefg.res #1 *FIRST* presentId=42 DEPTH         (1) 0x... base=(0,0) size=(2560,1440) incomingState=0x4 validity=0
```

借道 NGX 入口（PBZ 这种没有 Streamline 的游戏，`where` = `ngx`）：

```
fg: dlssg.ngx#1 internal=2560x1440 mf=1/1(max=0) jit=(+0.250,+0.250) mvecScale=(0.0004,0.0007) zn=0.1000 zf=inf fovY=1.0200rad(58.44deg) aspect=1.7778 dpi=1 cmi=1 reset=0 hdr=1
  mv          0x... 2560x1440 fmt=10
  depth       0x... 2560x1440 fmt=40
  hudless     0x... 3840x2160 fmt=10
  ...
  cameraViewToClip [ ... ]
  clipToPrevClip   [ ... ]
```

## 4. 怎么读（三条最值钱的判据）

1. **`mvs=` 与 `mvj=` 一起看**：SL 的 `mvecScale` 语义是"把 MV 归一化到 [-1,1]"，
   XeFG 的是"screen-space 像素"。两边同时有数时，UE5 那个"游戏报 `MV.Scale=1.0`"
   （`UE5_MV_OPTISCALER.md` §7.3）就能一次定死，不用拟合。
2. **`dpi` / `mvj` / `mvd`**：游戏自己说深度反不反、MV 里带不带 jitter、MV 是否已经
   dilated。这三条正是我们靠 DLSS CreateFlags 猜的东西（`in_ngx.cpp:309`），现在有
   第二份独立证据。
3. **tag 里的分辨率**：`MOTION_VECTORS 2560x1440` vs `DEPTH 3840x2160` 这种组合一眼
   就能看出"MV 在渲染分辨率、深度在输出分辨率"，不用猜 `[capture] StreamHeight`。

## 5. 能立刻回答的三个悬案

| 悬案 | 怎么判 |
|---|---|
| §7.4 说的"PBZ 的 MV 是垃圾"，坏的是 SR 那张还是 FG 那张？ | 同时开着 DLSS 和 DLSS-FG，比 `in_ngx.cpp` 报的 MV 指针 与这里 tag 的 MV 指针/尺寸 |
| `MV.Scale=(1,1)` 到底什么意思？ | 见上第 1 条，用 SL 的 `mvecScale` + XeFG 的 `motionVectorScale` 夹逼 |
| 我们自己合成的反向 Z 投影够不够用？ | 把日志里的 `cameraViewToClip` 与 `approx_projection_rz()`（`bridge.cpp:778`）逐元素比；差在远平面就印证 OptiScaler 那个 75000 |

## 6. 限制与安全边界

- **只读**：每个钩子都把原调用原样转发，参数一个字节都不改；`is_input()==false` + `poll()`
  恒返回 `false` ⇒ 宿主的"输入源"循环根本不会碰它（不会被 `Input=auto` 钉住，也不会出现在
  `waiting for ngx, ffx, xess` 那行里），更不会往共享块里写任何东西（协议版本没动）。
- 游戏**没开 FG** 时没有数据（FG 只在启用时被求值）。测量阶段临时打开，测完关掉。
- 只有 TSR / 只有 DLSS-SR 的题目没有 FG ⇒ 拿不到矩阵，继续用 `--fg-fov`。
- Streamline 的镜像结构体按 **2.x** 写（`sl::Constants` 456 字节，`sl::ResourceTag` 64 字节），
  编译期 `static_assert`、运行期校验 GUID；对不上就打原始字节，**不解释**。
  换 SL 大版本时先看日志第一行是 `sl.const` 还是 `unexpected struct GUID`。
- 每个挂点在日志里都有"装了几个 / 有几个没导出"，第一次调用一定打印（`#1`）。
  看到"hooks active"却**没有 `#1` 行**，就是那条路这个游戏根本没走 —— 这是刻意设计的
  判据，别把"没日志"当成"没钩上"。
- 联机 / 反作弊游戏不要用（同 `docs/findings/MFG.md` §249 的态度）。

## 7. 收数据的最小流程

1. `game_capture.ini`：`Log=1`、`FgSniff=1`；游戏里把**自带**的帧生成打开，跑 30 s 慢速平移；
2. 拿 add-on 旁边的 `game_capture.log`，`grep '\] fg'` 抽出来；
3. 若要判 MV 单位，把 dump 一起收（`scripts/analyse_dump.py` 的 units fit 段已经在等这份数据）。

## 8. 下一步（不在本次改动里）

- 把日志里的 `clipToPrevClip` 通过协议 v11 传给 bridge（`FrameShared` 加 16 个 float +
  `kInputFlagHasProjection`），让 `--fg-fov` 在 UE5 上退役；
- 用 `presentId` / `sl::FrameToken` 当配对键，替掉现在"槽位轮转近似配对"（`ROADMAP.md` §5）。

## 9. PBZ 实测环境摸底（2026-11-10，未开跑）

渲染进程是 `F:\steam\steamapps\common\PBZ\platform\steam\PBZ.exe` —— 两条独立证据：
`ReShade.log` 的 `Initializing ... into ...\platform\steam\PBZ.exe`，以及我们 add-on 自己打的
`config: ... (path F:\steam\...\platform\steam\game_capture.ini)`。

| 事实 | 证据 | 对实验的意义 |
|---|---|---|
| 进程里**没有 Streamline** | 整个游戏目录 `find -iname 'sl*.dll'` 只有 `SLLogger.dll`（且在非活目录） | `sl.interposer` 那几个钩子会打 `0 installed` / 没有 `#1` —— 这是**正常**，不是钩失败 |
| NGX 来自驱动 shim `_nvngx.dll` | `game_capture.log`：`NGX provider: C:\WINDOWS\System32\DriverStore\...\_nvngx.dll` | 超分与帧生成**共用**这一个入口 ⇒ 走"借道 NGX"那条路（`fg: dlssg.ngx#`） |
| 游戏声明 `flags 0x4B` | 日志 `dlss feature flags 0x4B: MVJittered=0 MVLowRes=1 DepthInverted=1` | 与 `UE5_MV_OPTISCALER.md` §7.1 一致 |
| 没有集成式 XeSS-FG | 无 `libxess_fg.dll` 被加载 | `xefg*` 钩子同样会 `0 installed` |
| `dlssg_sm86` 解锁器**当前不生效** | `dlssg_sm86.ini` 是 `Enabled=1 / Engine=XeSS / MaxGeneratedFrames=3`，但它要靠 `platform\steam\VERSION.dll` 当代理，而**那个文件已经不在了**（`dlssg_sm86/logs/*.log` 最后一条会话 11-06，`proxy` 字段写的正是 `...\platform\steam\VERSION.dll`） | 想用它得把随包的 `version.dll` 放回 `platform\steam\`；否则走官方 DLSS-G（3080 Ti 只有 2x） |
| 解锁器在该作有 3 次 `fg_gate_crash` | `dlssg_sm86/logs/dlssg_sm86.log` | 不稳，先短测 |
| OptiScaler 装在**另一个目录** | `ProjectPBZ\Binaries\Win64\`：`OptiScaler.ini`（今天 01:01）、25 MB 的 `dxgi.dll`、`D3D12_Optiscaler\`、一堆 `*.original`；而活目录 `platform\steam\` 的 `dxgi.dll` 是 ReShade 6.8（43 处 reshade 字符串） | **OptiScaler 不会被加载**（Windows 先从 exe 目录找 `dxgi.dll`），也从未产生过 `OptiScaler.log`。要用必须装进 `platform\steam\`，并且按它的共存方式：OptiScaler 当 `dxgi.dll`、ReShade 改名 `ReShade64.dll`、`LoadReshade=true` |
| `hid.dll` 不是解锁器代理 | 它转发真 `hid.dll` 导出并跑 "PBZ Mods Menu" 的 JS 运行时 | 与本次实验无关 |

**结论：对 PBZ，"用 OptiScaler 注入 XeFG 抓数据"答的不是"游戏自己怎么处理"** —— OptiScaler 会
接管游戏的 DLSS 调用，抓到的是**它自己**的 XeFG 输入（那是"一份跑得通的参考实现"，是另一个
有用的问题）。要拿游戏的真值，必须走**游戏自己的 FG 调用链**。

两套跑法：

| 想回答 | 怎么跑 | 期望日志 |
|---|---|---|
| **游戏自己交给帧生成什么**（矩阵 / MV 尺度 / 真实分辨率） | 保留 ReShade(`dxgi.dll`) + 本 add-on；`Log=1` + `FgSniff=1`；游戏内开 DLSS 帧生成 | `fg sniffer: ON ... DLSS-G is also read through the NGX evaluate hook` → `fg: dlssg.ngx#1 ...`；`sl.interposer` / `xefg` 那两行 `0 installed` 属正常 |
| 同一件事，但插值帧由 **XeSS-FG 模型**出 | 上面那套 + 把解锁器代理 `version.dll` 放回 `platform\steam\`（`Engine=XeSS`） | 同 `fg: dlssg.ngx#`（**游戏自己的调用链不变**，只是运行库换成 XeSS-FG）—— 这就是"游戏真参数 + 我们同款模型"的那组对照 |
| 一份能跑通的 XeFG 输入长什么样（对照我们的 bridge） | 另装一套 OptiScaler 到 `platform\steam\`（OptiScaler=`dxgi.dll` + `ReShade64.dll`），游戏内选 XeSS/FSR + 它的 FG | `fg: xefg.const #1 ...` + `fg: xefg.res #1 *FIRST* DEPTH ...` |

## 10. 崩溃复盘：`slGetFeatureFunction` 的签名（2026-11-10，已修）

第一次带 `slGetFeatureFunction` 钩子的构建让 PBZ **启动即崩**（01:29:54，swapchain 创建那一步）。
UE 的崩溃目录 `%LOCALAPPDATA%\ProjectPBZ\Saved\Crashes\UECC-*\CrashContext.runtime-xml` 直接给出：

```
<ErrorMessage>Unhandled Exception: EXCEPTION_ACCESS_VIOLATION reading address 0x0000000000000408
<PCallStack>
game_capture  0x00007ffc82650000 + 6d6f
PBZ           0x0000000140000000 + 47effdd
```

反汇编 `+0x6d6f` 是 `mov 0x20(%r15),%edx`，而 `r15 = 0x3E8`；`0x3E8 + 0x20 = 0x408` —— 正是崩溃地址。

**根因**：`PFun_slGetFeatureFunction = Result(sl::Feature feature, const char* functionName,
void*& function)`（`sl_core_api.h:69`）。第一个参数是 **`sl::Feature`**，不是 `ViewportHandle&`；
我按后者读了 `->value`（+0x20），把 `1000`（`kFeatureDLSS_G`）当指针用了。

**修法**：① 参数类型改成 `uint32_t feature`，只当日志里的整数打；② 新增 `viewport_value()`
守卫 —— 指针小于 `0x10000` 直接当"不是 viewport"，绝不 deref。这条守卫对**任何**签名假设都成立，
所以留着。

**顺带确认**：游戏确实用 `slGetFeatureFunction(feature=1000, ...)` 去拿 DLSS-G 的函数，说明它走的是
**逐 feature 的插件函数表** —— `slDLSSGSetOptions` / `slDLSSGGetState` 只在这里能拿到（这也解释了
为什么 interposer 不导出 `slDLSSGGetState`）。

**教训**：钩第三方 C API 之前签名必须从头文件抄，不能从"另一个同名/相邻函数"推断；
对"看起来像指针"的参数先判范围再 deref。


