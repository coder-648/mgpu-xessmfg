# 协议 v13 场景 3：PRAGMATA（RE Engine）

> 日期：2026-11-10　状态：**第一个真正走 `sl` 输入源的游戏；相机/配对/reset 全部命中**
> 目标：`E:\PRAGMATA\PRAGMATA.exe`
> 前两个场景：`docs/findings/FG_INPUT_V13_2077.md`、`docs/findings/FG_INPUT_V13_KHAZAN.md`

## 0. 一句话

PRAGMATA 把 v13 想要的东西**一次给全了**：Streamline 同时做 SR 和 FG ⇒ `sl` 是输入源；
`sl.const` 给真实投影（`fovY=58°`，`--fg-fov` 正式退役）；`slEvaluateFeature` 每帧都调 ⇒
`frame_key` ↔ WGC 配对首次在"正常游戏"上生效；场景切换时第一次抓到 `reset=1`。
代价是它的 `mvecScale=(1,1)` / `mvPx=1x1` 是占位值，且 `DEPTH` tag 是 `validUntilEvaluate`。

## 1. 部署（这个游戏有两个坑）

1. **add-on / ini 要从 `_storage_\` 读**（RE Engine 的进程级重定向）：
   两份都要放 —— `E:\PRAGMATA\` 与 `E:\PRAGMATA\_storage_\`；
2. `LoaderHook=0`：RE Engine 的加载器会被 `LoadLibrary` 钩子拖慢
   （`docs/findings/RE_ENGINE_NOTES.md` §6.1）。

```ini
Log=1
FgSniff=1
Input=auto
SlInput=1
SlFgOff=0
FgSuppressNgx=0
SkipMv=0
StreamlineCamera=1
Color=0
StreamHeight=1440
StreamPercent=100
LoaderHook=0
```

游戏内（`config.ini` 已确认）：`UpscalingAlgorithm=DLSS`、`DLSSQuality=DLAA`、
`FrameGeneration=Off`、`RayTracing=OFF`、HDR10 活跃、Reflex On。

## 2. 修掉的真 bug：`LoaderHook=0` 时 Streamline 钩子全装不上

第一次跑（12:19）拿到的是：

```text
fg sniffer: Streamline  e:\pragmata\_storage_\sl.interposer.dll
fg: hook slSetConstants FAILED
fg: hook slSetTagForFrame FAILED
fg: hook slSetTag FAILED
fg: hook slEvaluateFeature FAILED
fg: sl.interposer does not export slDLSSGGetState
fg: hook slGetFeatureFunction FAILED
fg: Streamline hooks active (0 installed, 1 not exported).
...
fg: hook NVSDK_NGX_D3D12_EvaluateFeature FAILED      (dlssg_sm86 运行时)
fg: hook xefgSwapChainD3D12InitFromSwapChain FAILED  (libxess_fg)
...
NGX provider: ...\_nvngx.dll
hooked NVSDK_NGX_D3D12_EvaluateFeature @ ...          ← 这个却成功
```

根因：**`in_fg.cpp` 从来没调用过 `mgpu::input::ensure_minhook()`**。
`LoaderHook=1` 时宿主在 AddonInit 里先初始化了 MinHook（所以 Khazan/CP2077 没暴露），
而 `LoaderHook=0` 走的是"启动时扫模块表"，那一刻 MinHook 还没初始化 ⇒
`MH_CreateHook` 对**所有** FG 相关导出返回 `MH_ERROR_NOT_INITIALIZED`，全部静默失败；
稍后 NGX 源自己初始化了 MinHook，所以只有 NGX 的钩子成功。

修法（`in_fg.cpp`，`consider_module()` 里，所有 SL / DLSS-G / XeSS-FG 安装路径都经过它）：

```cpp
if (!mgpu::input::ensure_minhook()) { return; }
```

顺带把安装失败的原因打出来（`fg: hook %s FAILED (MH status %d)`），
下次不用再猜。新 build md5 **`f9a7d9ca0982cacc90d410a112175c48`**。

修完之后同一个游戏：

```text
fg: Streamline hooks active (5 installed, 1 not exported).
NGX hooks active
dlss feature flags 0xB: MVJittered=0 MVLowRes=1 DepthInverted=1 (feature 1)
input source: sl (api 5) - pinned for this process        ← 本项目第一次 SL 当输入源
camera: using the input source's own view-to-clip (v13, sl)
shared: producer luid=00000000:00012F36 api=sl flags=0x3191 render=2560x1440 output=3840x2160
```

`flags=0x3191` 解码：`0x0001` InvertedDepth ｜ `0x0010` HasCamera ｜ `0x0080` HasMatrix ｜
`0x0100` HasPrevClip ｜ `0x1000` CameraMotion ｜ `0x2000` FgInput。

## 3. 采集 → 消费对照（本作特有）

| 数据 | 本作的表现 | 结论 |
|---|---|---|
| `sl.tags`（n=5） | `SCALING_INPUT`(3) / `SCALING_OUTPUT`(4) / `DEPTH`(0) / `MOTION_VECTORS`(1) / `EXPOSURE`(13)，每帧 | 比前两作都全（多了 SCALING_* 和 EXPOSURE） |
| `DEPTH` 的 lifecycle | **`lc=2(validUntilEvaluate)`** | 按设计**不读**（日志标 `[staged for Evaluate - not read as input]`）——和 OptiScaler 的 `IgnoreValidUntilEvaluateForFG` 同一个坑 |
| `MOTION_VECTORS` lifecycle | `lc=1(validUntilPresent)` | ✅ 可用 |
| `sl.const` 相机 | 进世界后 `zn=0.0100 zf=998.7 fovY=58.00deg`，矩阵是**真**透视 | bridge：`projection: from the game's own view-to-clip ... fovY=58.00deg (--fg-fov retired)` |
| `sl.const` 的 `mvecScale` | `(1.000000,1.000000)` + `mvPx=1x1` = **占位** | add-on 回退到 NGX 的 `MV.Scale=(3840,2160)` 得到正确尺度；但 **unit 标签仍是 `pixels`**（应为 `uv`）→ 待修 |
| `render` 尺寸 | `render=0x0`（`mvPx=1x1` 且 tag 没有 extent） | 流尺寸退回默认 2560x1440（`source: default`），能用但拿不到"按渲染尺寸"的信息 |
| `frame_key` | `slEvaluateFeature` 每帧调用 | ✅ bridge `frame-key pair: WGC ...-> slot 3 (age 164.6ms)`（日志只在第一次配对时打一行） |
| `resetHistory` | `sl.const ... reset=1` 在 **12:22:57 / 12:22:58** 各一次 | ✅ **第一次在真机抓到 reset 置 1**（进世界后的场景切换） |

## 4. 实测数字（4K DLAA，RTX 3080 Ti + RX 6750 GRE）

```text
cadence: rt=25.0ms (quant) input=22.4ms = 2.69 vblanks -> interp=0
         (avg 2.00x, 89.3 fps out = 1.34 vblank/frame NOT-EXACT)
breakdown: work=0.4 present=5.3 gpu=2.9 wait=0.0 colour=0.2 age=2.4 total=6.4
out: presents=80 expected=80   timeouts=0
```

- 游戏自身 ~44 fps（GPU 97%，4K DLAA 很重）⇒ 2x 只有 89 fps，非整档；
  要整档得 3x（`--interp 2 --unlock-mfg`，44×3=132 也非整档）或降超分档位；
- 我们的成本很小：`gpu` 2.9 ms、`present` 5.3 ms。

### 4.1 第二轮（12:23–12:28，用户自己点的 `run_pragmata_2x.bat`）

```text
(43.1 fps avg, 44.9 recent)          ← 游戏自身
cadence: input=21.9-22.4ms = 2.62-2.69 vblanks -> interp=0
         (avg 2.00x, 89-91 fps out = 1.31-1.34 vblank/frame NOT-EXACT)
breakdown: work=0.5 present=7.6-8.1 gpu=1.8-2.3 wait=0.0 colour=0.2 age=3.9-4.0 total=8.6-9.1
out: presents=80 expected=80   timeouts=0   drops=0
frame-key pair: WGC ...-> slot 3 (只在第一次配对时打一行)
sl.const ... reset=1 (12:22:57 / 12:22:58)
probe: depth ... -> reversed-Z (tentative: nothing near 1)  declared=reversed (agrees)
```

44 fps 输入在 120 Hz 上没有整档解（2x=89、3x=132），要整档只能动态 cadence
（`--interp 5 --interp-cadence`）或把游戏的超分档位降下来。

### 4.2 真问题：`sl` 源下 **MV 流是全 0**

```text
[12:24:23] probe: mv mean|v|=(0.000,0.000) invalid=0.0% large=0.0% -> unit=UV  magnitude=0.000  declared=unknown
[12:27:21] probe: mv mean|v|=(0.000,0.000) invalid=0.0% large=0.0% -> unit=UV  magnitude=0.000  declared=unknown
[12:28:02] probe: mv mean|v|=(0.003,0.003) invalid=0.0% large=0.0% -> unit=UV  magnitude=0.003  declared=unknown
```

同一时刻**深度是活的**（`min=0.0004 mean=0.0034 max=0.0092`、`far0=0%`），
所以不是"玩家人没动"，是 MV 那条流没拷到内容。

已定位到的直接原因：本作的 `sl.tags` **没有 extent**（`extent=(0,0)`），
加上 `sl::Constants.mvPx=1x1` 占位 ⇒ `FrameInputs.render_w/h = 0`
⇒ `-- frame ... render=0x0` ⇒ 下游按 0 尺寸算源矩形/尺度。

已做两处修改（build **`a9abfe48ffb06ce59f7dfd72d377b3db`**，待部署验证）：

1. `size_from_resource()`：tag 没有 extent 时，用 `ID3D12Resource::GetDesc()` 把
   MV/depth 的宽高补上（顺带让 `render` 不再是 0x0，`HiResMv` 也能被正确识别）；
2. `mv_unit`：当 `mvecScale==1`（占位）改用 NGX 的 `MV.Scale` 时，**同时**把
   unit 标成 `uv`，不再输出 `semantics: mv=pixels`（PRAGMATA 的 MV 实际是 UV）。

验证方法：重新进世界后看 `-- frame ... render=3840x2160`、`semantics: mv=uv`，
以及 `probe: mv` 在走动时给出非 0 的 `mean|v|`。

### 4.3 一个必须记住的现场教训


中途游戏掉到 **8 fps**、`input=100.5ms`，一度怀疑是 bridge 的叠加层。
`nvidia-smi` 里发现**上一款游戏（Khazan）的进程还活着**（窗口没了、7.9 GB 显存、
吃满 GPU）。`taskkill` 掉之后立刻回到 44 fps。
⇒ 换游戏前先 `tasklist | grep -i <exe>` + `nvidia-smi --query-compute-apps` 对一遍。

### 4.4 A 阶段（harvest 位图）实测 —— PRAGMATA，build `8a3b081a…`

三处修复全部生效：

```text
-- frame 6256 stage=2  ... render=3840x2160 mvscale=(3840.000000, 2160.000000)   ← 不再是 0x0
semantics: mv=uv sign=prev-cur camera=matrix depth=reversed valid=3840x2160      ← 不再是 pixels
```

`harvest:` 位图第一次给出统一数据面（和设计预测完全一致）：

```text
[12:51:11] harvest: mv=y depth=y camera=n clip=n token=y reset=n hudless=n ui=n scaling=io expose=y placeholder=y
[12:51:36] harvest: mv=y depth=y camera=y clip=y token=y reset=n hudless=n ui=n scaling=io expose=y placeholder=n
[12:51:52] harvest: mv=y depth=y camera=y clip=y token=y reset=y hudless=n ui=n scaling=io expose=y placeholder=n
```

- `placeholder=y → n`：**占位矩阵检测生效**（开局 90° 假投影被拦下，`semantics` 那时是 `camera=none`）；
- `scaling=io` / `expose=y`：RE Engine 给 SCALING_INPUT+OUTPUT 和 EXPOSURE，但没有 HUDLESS/UI
  ——这正是文档里预测的"颜色只能走 `SCALING_OUTPUT` 或 WGC"；
- `reset=y` 在 12:51:52 出现一次（**第二次在真机抓到 reset**）。

**MV 仍然是 0**，而且这次的证据指向"场景不动"，不是拷贝坏了：

```text
probe: mv mean|v|=(0.000,0.000) ... declared=uv (agrees with the declaration)
probe: depth min=0.0008 mean=0.0024 max=0.0088  （深度是活的）
fg: sl.const #6600 / #6720 / #6840 ... jit=(-0.125,-0.278)px   ← 6 秒内 jitter 完全冻结
```

jitter 冻结 = 这些帧的 `sl::Constants` 没有真正更新（角色站着不动/暂停），
所以 MV 为 0 是"没有运动"而不是"流没通"。**下次要结论就必须让角色走起来**
（`probe:` 每 120 帧一次，站立时永远只会给 `ambiguous/UV`）。

### 4.5 2026-11-10 13:03 启动失败：残留实例（Application Hang）

现象：双击游戏后无反应，Windows 事件日志 `Application Hang`（13:04:41），
`ReShade.log` 停在 13:03:27 的"Installing export hooks for dxgi.dll"——连 add-on 都还没加载。

排查：`breadcrumb.log` 里最后一次 `AddonInit` 是 12:51 的 **pid 27988**（那次跑完游戏一直挂在后台），
13:03 的新实例（pid 28368）**根本没有 AddonInit 记录** ⇒ 新实例在加载 add-on 之前就卡住了。

根因：**上一次的 PRAGMATA 还活着，又启动了第二个**（RE Engine 的 pak 文件锁 / 单实例），
13:04 被系统判定为无响应。杀掉后重新启动一次即正常（13:05:41，新 build `4800dc96…`）。

> 同样的坑当天出现过两次：早些时候 Khazan 的残留进程吃满 GPU 让 PRAGMATA 掉到 8 fps。
> **换游戏或重开之前先 `tasklist | grep -i <exe>` + `nvidia-smi --query-compute-apps`。**

### 4.6 `Claim=0`（FG 关闭）时 MV 是活的 —— 本作实测

```text
[12:52:39] probe: mv mean|v|=(0.000,0.000) ... magnitude=0.000     ← 游戏在后台/没出新帧
[13:06:54] probe: mv mean|v|=(0.034,0.004) ... magnitude=0.034     ← 正常出帧 + 有位移
```

同一会话里 `jitter` 也在变（`0.125,0.278` → `-0.375,-0.056`）；0.034（UV）≈ 130 px 的位移，
不可能来自静止画面。⇒ **本作在 FG 关闭时仍然产出有效 MV**，`claim` 不是拿到 MV 的前提。

这与 Khazan（FG 关、MV 实测 69 px）一致，也再次说明"MV 是 TAA/DLSS-SR 的输入，不是 FG 的产物"。
但按 §0.1 的方法论：这只证明**本机这次的机制正常**，不构成"对其它游戏成立"的保证 ——
能保证的是 `kInputFlagMvSuspect`（数据级判定）+ bridge 的一次性告警 + 建议 `--fg-of`。

### 4.7 `Claim=1` 的实测结论：对"菜单里关掉 FG"的游戏**无效**

```text
config: SlInput=1 SlFgOff=0 Claim=1 (...)
fg: sl.interposer does not export slDLSSGGetState
fg: DLSSG hooks active (2 installed, 2 not exported).
    A missing "dlssg.eval#1" line means the game is not generating frames through this module.

（此后：dlssg.options = 0 次，sl.eval = 0 次，dlssg.eval = 0 次）
```

游戏 `config.ini` 里 `FrameGeneration=Off`，所以 PRAGMATA **根本不跟 DLSS-G 说话**：
不查 GetState、不设 SetOptions、更不 evaluate。我们的两面假面（`slDLSSGGetState` /
`slDLSSGSetOptions`）**没有调用点可以拦**，claim 自然什么也没发生。

**结论（可外推的那条）**：claim 只能影响"已经在跟 DLSS-G 对话"的集成；
一款在菜单里把 FG 关掉的游戏，FG 管线压根没初始化，claim 无从下手。
这不是实现问题，是接口位置问题 —— 想驱动它，必须让它自己打开 FG。

### 4.8 由此得到的更优路线：**游戏内 FG 开 + `SlFgOff=1`**

Khazan 已经示范过这个状态：FG 管线**初始化了**，但 SL 的 mode 是 off ——
于是游戏每帧照常 `slSetConstants` / `slSetTag`，我们白拿完整输入，而它不出帧。

所以对 PRAGMATA 这类题：

1. 游戏内把「帧生成 / Frame Generation」设成 **开**（让 DLSS-G 初始化、开始调用 API）；
2. `game_capture.ini` 设 `SlFgOff=1`（把 `slDLSSGSetOptions.mode` 改写成 `eOff`）；
3. 结果应当是：**输入完整 + 不出双份帧 + 画面正常**，日志里出现
   `fg: SlFgOff=1 - forcing slDLSSGSetOptions mode=eOff`。

判据（一次就能分辨）：
- 出现 `dlssg.options mode=0 ... (frame generation OFF)` ⇒ SlFgOff 生效，成功；
- 出现 `dlssg.options mode=1` 且游戏帧率翻倍 ⇒ SlFgOff 没拦住（CP2077 那种），
  此时必须靠 `FgSuppressNgx`（不行）或先退回"游戏内关 FG"。


### 4.9 ✅ 可用配置（2026-11-10 最终结论）：游戏内 FG 开 + `SlFgOff=1`

```ini
SlInput=1      ; Streamline 是输入源
SlFgOff=1      ; 我们改写 slDLSSGSetOptions.mode -> eOff
Claim=0
```

游戏内：**DLSS-SR 开（DLAA）+ 帧生成 开**。

实测（pid 27528，13:12–13:14）：

```text
config: SlInput=1 SlFgOff=1 Claim=0 (...)
fg: dlssg.options mode=0 framesToGenerate=1 (=2x) ... (frame generation OFF)
fg: SlFgOff=1 - forcing slDLSSGSetOptions mode=eOff (game frame generation disabled;
    add-on reads the declarations)

harvest: mv=y depth=y camera=y clip=y token=y reset=y hudless=y ui=y scaling=io expose=y placeholder=n
```

对照：**同一款游戏、FG 关着**时是 `hudless=n ui=n`。把游戏的 FG 打开（管线初始化）
之后，`HUDLESS_COLOR` 和 `UI_COLOR_AND_ALPHA` 两个 tag 才出现 —— **FG 契约确实携带
SR 路径拿不到的东西**，这就是"在 FG 流上抓数据"的实证。

同时：

- 漏拦 `mode=1` **0 次**（每一次 SetOptions 都被改写成 off）；
- 游戏帧率 **~52 fps 正常**（没有翻倍 ⇒ 游戏真的没出帧，不存在双份）；
- 画面**正常**（截图确认，飞船内部场景，桥侧 `x2`、40→80 fps、`gpu 1.5–3.0 ms`）；
- `age` 从之前的 9–12 ms 掉到 **3.2–3.5 ms**（WGC 那一跳更跟手了）。

⇒ **这是"FG 契约优先"路线里第一个完整可用的状态**：输入完整（含颜色 tag）、
不出双帧、画面正常、成本几乎为零。而它**不需要 claim**——只需要游戏把 FG 打开，
再让我们把 mode 关掉。

> 注意：`hudless/ui` 现在是"有了但没吃"（颜色仍走 WGC，见 §5.1 的占用决定）。
> 若以后要在 PRAGMATA 上做 UI 挖洞，这两条 tag 已经现成。

### 4.10 ✅ 端到端结论（用户实测 50 → 77）：三层各司其职

| 层 | 在做什么 | 证据 |
|---|---|---|
| 游戏（FG 开） | 自己出帧，present 从 **50 → 77**（×1.54，FG pass 本身吃渲染时间，所以不是干净的 2x） | RTSS 外部计数 |
| add-on | 在 FG **激活**状态下抓数据：`harvest: mv/depth/camera/clip/token/reset/hudless/ui/scaling/expose = 全 y` | `harvest:` 行 |
| WGC | 抓游戏窗口画面 | 桥侧 `capture 3840x2160` |
| 我们的 bridge | 用上面的数据做**自己的**插帧并上屏 | `xN=2.00`、`out: presents=80 expected=80` |

要点（也是这一整天的结论）：

1. **数据面来自"FG 激活"的管线**——这是 SR 路径拿不到的东西（`hudless/ui` 只有 FG
   管线初始化后才出现）；
2. **颜色不需要入槽**：WGC 抓屏已经拿到画面，producer 侧零额外成本（§9 的占用账成立）；
3. `SlFgOff` 决定"游戏出的帧要不要"：
   - `SlFgOff=0`：游戏 FG 全速跑（50→77），WGC 抓到的是 real+generated 混合流；
   - `SlFgOff=1`（§4.9）：我们只让游戏初始化管线、不许它出帧（present 保持 ~50），
     WGC 抓到的全是真实帧 —— 数据仍然完整。

### 4.11 计数器的一个坑（工具也要说清楚）

`sl.const #N` 与 `perf: frames=N` 在 Streamline 题上会**同步增长**（`slSetConstants`
可能每个 present 帧都调一次，含生成帧），所以 `present/render` 是 **1.02–1.17 也说明不了
"没有出帧"**。它只能当**下界探测器**：比值 2/3 是确凿的，比值 1 不是。
要判断游戏到底出没出帧，用**外部 present 计数**（RTSS）配菜单开关做对比 ——
50（关）vs 77（开）就是这次的判据。

### 4.12 ✅ v14 三件套真机验证（13:41，切桌面 + 中途开路径追踪）

同一次运行里把三件事全踩到了：

**1) 新鲜度门控（`input: source ... STALE`）**

```text
[13:41:51.554] input: source sl is STALE - the last frame is being replayed,
               consumer should fall back (--fg-of / --no-mv)   (860 ms since its last frame)
[13:41:51.764] init_swapchain: ... resize=0 - pausing capture until frame 8680 (max 8 s)
[13:41:59.795] shared: content size is now 2228x1254 (gen 2) - slots stay 3840x2160
[13:41:59.795] input: source sl is live again (47 ms since its last frame)
```

切到桌面 → 860 ms 后判 STALE → 回来时 swapchain 重建（`init_swapchain resize=0`）、
content size 换成 2228x1254（换画质档）→ **8 秒后自动恢复 `live again`**。
期间 bridge `presents=80/80`、`drops=0`，只有 `cadence` 因为输入掉到 ~32 fps 变成非整档。

**2) `declared_frames` / `harvest_flags` 透传**

```text
[bridge] shared: producer luid=... api=sl/streamline flags=0x3191 render=2560x1440 output=3840x2160
[bridge]   producer declaration: framesToGenerate=0 harvest=0x71F
           (mv=y depth=y cam=y clip=y token=y reset=n hudless=n ui=n)
```

这一轮游戏菜单里 FG 是**关**的（`framesToGenerate=0`，且 `hudless=n ui=n`），
所以它同时验证了"FG 关 ⇒ 颜色 tag 不出现"这个对照。之前 FG 开的那轮是
`framesToGenerate=1` + `hudless=y ui=y`。

**3) 换画质档（路径追踪 / 分辨率变化）不打断链路**

`content size gen 2 → 2228x1254`，slots 保持 3840x2160，没有重启游戏；
`harvest` 仍然全 y，`reset=y` 在转场时照常出现。

> 一个要留意的读数：恢复后 `sl.const` 出现 `zn=0.1000 zf=5992.0 fovY=0.2992rad(17.14deg)`
> —— 近/远平面和 FOV 都跟之前（0.01/998.7/58°）不同。17° 可能是瞄准/过场，
> 也可能是重建后的瞬时值；它不会被占位检测拦下（占位判定要求 90°），所以这条要记着，
> 下次再看到异常投影时先查 `sl.const` 的 zn/zf/fov 三元组。

### 4.13 手动切 FG 实现 = 手动切车道（实测，2026-11-10 14:10–14:15）

用户在两分钟内做了 FSR → DLSS-G → FSR 的切换。日志证明**游戏确实在换 FG 实现**：

```text
14:10:31  ffx: the game dispatches its OWN frame generation (ffxDispatchDescFrameGeneration)
14:14:31  ffx: the game dispatches its OWN frame generation (ffxDispatchDescFrameGeneration)
14:14:44  fg: dlssg.options mode=0 ... (frame generation OFF)
14:14:50  fg: dlssg.options mode=1 framesToGenerate=1 (=2x)
14:14:45  harvest: ... hudless=y ui=y ...      ← DLSS-G 开时才给颜色 tag
```

但**lane 2 没有点亮**（`input source` 一直是 `sl (api 5)`），原因有两个，都很有价值：

1. **`ffxDispatchDescFrameGeneration` 里没有 MV/深度**。看头文件：它只有
   `presentColor` / `outputs[4]` / `numGeneratedFrames` / `reset` / `frameID`。
   MV/深度在 **`ffxDispatchDescFrameGenerationPrepare`** 里。
2. PRAGMATA 的 **prepare 调用没经过我们挂的模块**。我们只挂了
   `amd_fidelityfx_loader_dx12.dll` 和 `amd_fidelityfx_upscaler_dx12.dll`；
   FSR-FG 的 provider 是 **`amd_fidelityfx_framegeneration_dx12.dll`**（它就在游戏目录里）。

顺带确认：`dlssg_sm86`（`Engine=XeSS`）**点不亮 lane 3** —— 它用自己
`dlssg_sm86\runtime\` 下的私有 `libxess_fg` 副本，我们挂的 4 个 `xefgSwapChain*`
钩子一次都没被调用（`xefg.const #` = 0）。

**修法（build `41ab5f14…`）**：把 `amd_fidelityfx_framegeneration_dx12.dll` 加入 ffx 模块名单
（`kMaxModules=4`，正好还有空位）。重新进游戏切到 FSR 后应当出现
`ffx: the game dispatches its OWN frame generation - it is now an input source (lane 2)`
和 `input source: ffxfg (api 8)`。

### 4.14 FSR-FG / XeSS-FG 两条车道在 PRAGMATA 上为什么仍是暗的（2026-11-10 14:25）

用户在同一局里做了 **FSR-FG → DLSS-FG → FSR-FG**（其中 DLSS-FG 位实际上是 sm86 用 XeSS 引擎在出帧）。
新 build（`161a7e8c…`）的覆盖度自报先给了第一个答案：

```text
fg: XeSS-FG data coverage 6/6 (complete)          ← lane 3 的 6 个入口全在
ffx: e:\pragmata\_storage_\amd_fidelityfx_framegeneration_dx12.dll hooked (ffx-api, slot 2)
```

但实际数据面：

| 计数（14:25 会话） | 值 | 含义 |
|---|---|---|
| `xefg.const #` | **0** | lane 3 一次没触发 |
| `ffx: first frame`（upscale） | 0 | 当时超分走的是 DLSS |
| `the game dispatches`（FSR-FG generation） | 1 | **FSR-FG 确实起来了** |
| `dlssg.options mode=` | 3（0/1/0） | DLSS-G 位：sm86 在转 XeSS |
| `input source:` | 1（只有 sl） | 两条新车道都没出数，选择器无从切换 |

两个原因都不在"钩子没挂"：

1. **PRAGMATA 的 FSR-FG 只发 `ffxDispatchDescFrameGeneration`（0x00020003），从不发
   `..._FRAMEGENERATION_PREPARE`（0x00020004）**。而按头文件，generation 那份只有
   `presentColor` / `outputs[4]` / `numGeneratedFrames` / `reset` / `frameID` ——
   **MV/深度/jitter/相机全在 PREPARE 里**。所以本作的 FSR-FG 没有可抓的矢量数据。
2. **sm86 的 `Engine=XeSS` 用的是它自己 `dlssg_sm86\runtime\` 下的私有 `libxess_fg` 副本**，
   我们挂在 `_storage_\libxess_fg.dll` 上的 6 个入口一次都没被调用。

⇒ 结论：**PRAGMATA 不是 lane 2 / lane 3 的有效样本**；它只能验证 lane 1（DLSS-G）和 lane 4。
要验 lane 2 需要一款"自己发 PREPARE"的 FSR-FG 题目，要验 lane 3 需要一款"原生用 XeSS-FG"的题目。

**新增的可观测性（`6b5f37f6…`）**：ffx 侧现在对每个**首次出现**的 dispatch desc 类型打一行

```text
ffx: dispatch desc type 0x00020004 seen for the first time  <== FSR-FG input data (lane 2)
```

下一款 FSR-FG 题目一跑，就能立刻回答"它到底发不发 PREPARE"，而不是像这次一样靠推断。

## 5. 待办




1. ~~游戏内 FG 开 + `SlFgOff=1`~~ ✅ 已验证（§4.9）；**若要游戏自己也出帧**用 `SlFgOff=0`
   （WGC 会抓到 real+generated 混合流，见 §4.10）；
2. **cadence**：21–25 ms 输入（40–48 fps）凑不出 120 Hz 整档，需要
   `--interp 5 --interp-cadence`（动态逐帧）或把超分档位降到能跑 60 fps 的档；
3. **`DEPTH` tag 的 `validUntilEvaluate`**：目前按设计不读，靠 NGX 侧通道的深度；
   若以后要只吃 SL 的深度，得把拷贝挪到 `slEvaluateFeature` 里；
4. **颜色**：`HUDLESS_COLOR` / `UI_COLOR_AND_ALPHA` 在 FG 开的配置下已经出现，
   但按 2026-11-10 的占用决定**暂不消费**（颜色仍走 WGC）；
5. **启动前先查残留进程**（§4.5）：今天两次踩到，"游戏起不来/莫名掉帧"都先查这个；
6. **`fg_ratio.py` 的定位**（§4.11）：下界探测器，别拿比值 1 当"没出帧"的证据。







