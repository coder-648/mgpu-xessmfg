# 协议 v13 场景 2：The First Berserker: Khazan（UE 4.27）

> 日期：2026-11-10　状态：**链路跑通，4K 2x 整档 120fps；MV 单位已实测确认**
> 目标：`F:\The First Berserker Khazan\BBQ\Binaries\Win64\BBQ-Win64-Shipping.exe`
> 上一场景：`docs/findings/FG_INPUT_V13_2077.md`

## 0. 一句话

Khazan 是 **UE 4.27**，它的 Streamline 只用来做 DLSS-G（引擎插件里只有 `sl.dlss_g.dll`，
没有 `sl.dlss.dll`），DLSS-SR 走游戏自带的 `nvngx_dlss.dll`（NGX 直调）。
所以本作最终**不是** `sl` 输入源，而是 `ngx` 输入源——但 SL 的声明恰好给了我们
CP2077 拿不到的两样东西：**每帧都在的 DEPTH tag** 和**完整的相机常数**。

## 1. 环境与配置

`game_capture.ini`（部署目录 `BBQ\Binaries\Win64`）：

```ini
Log=1
FgSniff=1
Input=auto
SlInput=1          ; 探针：本作确实有 sl.interposer，但只服务 DLSS-G
SlFgOff=0
FgSuppressNgx=0
SkipMv=0
StreamlineCamera=1
Color=0
StreamHeight=1440
StreamPercent=100
```

部署：`build/game_capture.addon64`（v13, md5 `f9a7d9ca0982cacc90d410a112175c48`）；
ReShade 在 `dxgi.dll`（6.8），`MGPU_DepthTap` 生效。

游戏内（用户手动）：**超分 = DLSS**，**帧生成 = 关**。

## 2. add-on 侧看到的三条路径

启动后一次日志里三条路径同时出现（Khazan 自己会同时加载 SL 和 nvngx_dlssg）：

```text
fg sniffer: Streamline  F:\...\Engine\Plugins\Runtime\Nvidia\Streamline\Binaries\ThirdParty\Win64\sl.interposer.dll
fg sniffer: DLSS-G      F:\...\BBQ\Binaries\Win64\dlssg_sm86\runtime\nvngx_dlssg.dll
NGX provider: C:\WINDOWS\System32\DriverStore\...\_nvngx.dll
```

- `sl.interposer` 不导出 `slSetTagForFrame` / `slDLSSGGetState`，走的是
  `slSetTag`（viewport 版本）+ `slGetFeatureFunction`，钩子 4 个装上；
- `dlssg_sm86\runtime\nvngx_dlssg.dll` 是机器上装过的 `dlssg_for_sm86` 运行时
  （`third_party/dlssg_for_sm86-main`），启动时会自己解包出
  `dlssg_sm86\{runtime,backend,kernels,sr,xefg,logs}`；
- NGX 只挂在驱动的 `_nvngx.dll` 上，**DLSS-SR 用的是游戏目录里的 nvngx_dlss.dll**
  （`ngx: sr ...` 经 `_nvngx` shim 转发，钩子能拿到）。

### 2.1 SL：DLSS-G 关闭时仍然每帧声明常数和 tag

```text
fg: sl.const #1 *CHANGED* vp=1 ver=2 jit=(+0.062,-0.463)px mvS=(0.000390,0.000694) mvPx=2562x1441
    zn=0.0100 zf=75000.0 fovY=1.3265rad(76.00deg) aspect=1.7779 dpi=1 cmi=1 mv3d=0 reset=0
    ortho=0 mvj=0 mvd=0 mvBad=3.403e+38 projShear=(-0.00015,...)

fg: sl.tags #263 *CHANGED* vp=1 n=3
  DEPTH             ( 0) ... 0x0 state=0x60  lc=0(onlyValidNow) extent=(0,0 2562x1441)
  MOTION_VECTORS    ( 1) ... 0x0 state=0x8   lc=0(onlyValidNow) extent=(0,0 2562x1441)
  HUDLESS_COLOR     ( 2) ... 0x0 state=0x400 lc=0(onlyValidNow) extent=(0,0 3840x2160)
```

对比 CP2077：**DEPTH 和 MOTION_VECTORS 每帧都在**，extent 就是渲染分辨率
（2562x1441，DLSS 质量档）；HUDLESS_COLOR 在输出分辨率（3840x2160），
state 已是 RENDER_TARGET。

`mvS=(0.000390,0.000694) = (1/2564, 1/1441)`：按 DLSS-G 头的定义
（`mvecScale` 是"把 MV 归一化到 [-1,1] 的缩放"），这是 **像素制** MV 的写法，
和 CP2077 那个误导性的 `(1,1)` 正好相反。

`mvBad=3.403e+38` 是 FLT_MAX 级别的 invalid marker，本作确实声明了它
（当前只发布不消费）。

### 2.2 切换超分档位后 SL 就不再发布

`sl.const` 最后一帧是 `#120`（11:53:09），随后把超分从"质量"改成 1:1
（渲染 3840x2160）时，UE 侧重建了 DLSS 管线：

```text
[11:53:23.178] ngx: sr mv=... 3840x2160 fmt=34 | depth=... 3840x2160 fmt=19
[11:53:23.180] semantics: mv=pixels sign=prev-cur camera=none depth=reversed valid=3840x2160
```

即 **SL 只在它的 FG 插件活着时发布常数与 tag**，FG 管线一重建就安静。
本作不能指望 `SlInput=1` 长期供数，它只是开局那一段的额外信息源。

### 2.3 最终输入源：ngx

```text
input source: ngx (api 1) - pinned for this process
semantics: mv=pixels sign=prev-cur camera=none depth=reversed valid=3840x2160
downsample: MV/depth pass targets are 3840x2160 (capacity), drawing 2560x1440
shared: mv at 2560x1440 (height cap, format 34), depth source 3840x2160 format 19 -> stream R32 (2560x1440)
shared: producer luid=00000000:00012F36 api=ngx flags=0x9 render=2560x1440 output=3840x2160
```

- `dlss feature flags 0x49: MVJittered=0 MVLowRes=0 DepthInverted=1`；
- `probe: depth ... -> reversed-Z ... declared=reversed (agrees with the declaration)`；
- **没有相机**（`camera: not used (no matrix published yet)`）：NGX 不导出相机，
  SL 那段又停了，bridge 落到合成投影 `--fg-fov 60`；游戏真实值是
  **76° / near 0.01 / far 75000**，用 SL 声明出来的值手动补上
  `--fg-fov 76 --fg-zn 0.01`。

## 3. bridge 侧

### 3.1 一条会误伤本作的自动判断

```text
[bridge] BBQ-Win64-Shipping.exe is an Unreal shipping build - using our optical flow
         for the motion vectors (its own MVs ghosted here; override with --game-mv)
[bridge] --fg-of: motion vectors come from FidelityFX optical flow ...
```

这条规则来自 UE5 的结论（影之刃零：UE5 的 MV 流是坏的）。Khazan 是 **UE 4.27**，
不该无条件套用；`--game-mv` 可覆盖。

### 3.2 实测：4K 下 2x 与 3x 两档（RTX 3080 Ti 产出 + RX 6750 GRE 消费）

用户侧操作：进世界 → 双击 `C:\temp\mgpu\bin\run_khazan_{2x,3x}.bat`。
两档之间**没有重启游戏**：add-on 的 content-size 机制接管了渲染尺寸变化。

```text
shared: content size is now 2228x1252 (gen 2) - slots stay 3840x2160, nothing rebuilt
shared: content size is now 2560x1440 (gen 3) - slots stay 3840x2160, nothing rebuilt
```

| | 2x（`--interp 1`） | 3x（`--interp 2 --unlock-mfg`） |
|---|---|---|
| 游戏内超分 | DLSS 平衡 → 渲染 2228x1252 | DLSS 质量 → 渲染 2560x1440 |
| 输入 | 16.7 ms = **60.0 fps** | 33.3 ms = **30.0 fps** |
| cadence | `input=16.7ms = 2.00 vblanks` → **1.00 vblank/帧，120.0 fps 整数** | `input=33.3ms = 4.00 vblanks` → 90 fps，**1.33 vblank/帧 NOT-EXACT** |
| presents | 80/80（首窗 1 次 `OUTPUT FRAMES MISSING` 是冷启） | 120/120（同样只有冷启那一次） |
| `gpu` 中位 | **7.3 ms** | **10.0 ms** |
| `present` 中位 | **7.0 ms** | **2.0 ms** |
| 其它 | `wait=0 timeouts=0`，`xN=2.00` | `wait=0 timeouts=0`，`xN=3.00` |

结论：**4K 下 2x（60 fps 进 → 120 fps 整档）是这套机器在本作上的甜点**；
3x 在 30 fps 输入时只能到 90 fps（1.33 vblank），要 120 整档得 **4x**
（`--interp 3 --unlock-mfg`）或让游戏回到 40 fps（3x 整档）。

### 3.3 MV 单位：**已确认 = pixels**（游戏自报正确）

用户在世界里行走/转视角后，探针给了两组"有位移"的读数：

```text
[12:10:43.773] probe: mv mean|v|=(1.155,0.977)   invalid=0.0% large=0.0% -> unit=pixels
               magnitude=1.155  declared=pixels  (agrees with the declaration)
[12:11:45.037] probe: mv mean|v|=(69.263,4.426)  invalid=0.0% large=0.0% -> unit=pixels
               magnitude=69.263 declared=pixels  (agrees with the declaration)
```

69 px 的均值只可能是像素制（UV 制下同一次甩视角只有 ~0.02）。所以：

- Khazan 的 `MV.Scale=(1,1)` 是**正确**的，不需要 `--fg-mv-scale`；
- 之前那几条 `unit=UV` / `ambiguous`（0.010–0.050）确实是**静止画面**，探针的
  "场景几乎不动，重试"分流是有效的；
- 也让「UE4 的 MV 不该套用 UE5 的 '坏 MV' 结论」有了数据支撑：`--game-mv` 的值
  与探针一致，可以直接用游戏 MV（不必强制光流）。

### 3.4 frame_key ↔ WGC 配对：本作**不会发生**

两份 bridge 日志里 `frame-key` 出现 **0** 次。原因是 v13 的 `frame_qpc_ms` 由
`slEvaluateFeature` 打戳，而 Khazan 的 FG 是关的（bridge 出帧），SL 侧永远不会
evaluate ⇒ 只有 `sl.const` / `sl.tags`，没有 frame token。

⇒ 现状：**frame_key 配对目前只是 CP2077（SL 出 FG）的能力**；NGX/FFX/XeSS 这类
"只出 SR"的源下 bridge 走回退轮转。要普及，最小的改法是让 host 在
`on_begin_effects` 给任意源盖一个 `frame_qpc_ms`（QPC 与 WGC 同钟），
不必等游戏给 frame token。


## 4. 采集 → 映射 → 消费对照（v13）

### 4.1 数据流：谁供数

```text
          ┌──────────────────────────────┐
  开局那段 │ sl.interposer (只服务 DLSS-G) │  sl.const / sl.tags 每帧
          └──────────────┬───────────────┘        │（切超分档位后停止）
                         │                        v
                   （不作为输入源）
                                                  ✗
          ┌──────────────────────────────┐
  全程     │ nvngx_dlss.dll (DLSS-SR)      │ ──> NGX 源：MV / depth / jitter / MV.Scale
          │ 经驱动 _nvngx.dll 转发         │      ✅ 本项目最终输入源 api=ngx
          └──────────────────────────────┘
```

`Input=auto` 的裁决很干脆：SL 源在 `poll()` 里要求 `have_constants && (mv || depth)`，
而切档之后 `sl.const` 再没来过 ⇒ 只剩 NGX 能给帧，于是钉死 `input source: ngx (api 1)`。
**注意**：这解释了为什么 CP2077 能拿到 SL 相机而本作不行——不是钩子没装上，是游戏自己停了发布。

### 4.2 逐项对照

| 数据 | Khazan 的声明 / 来源 | 实测证据 | FrameShared v13 字段 | 进 XeFG？ |
|---|---|---|---|---|
| MV 纹理 | NGX SR；另有 SL `MOTION_VECTORS` tag（每帧，`onlyValidNow`） | `ngx: sr mv=... 3840x2160 fmt=34`；流为 2560x1440 | `mv` 流 | ✅ |
| 深度 | NGX SR；另有 SL `DEPTH` tag（每帧） | `ngx: sr depth=... fmt=19`（2228x2048 / 3840x2160 两种） | `depth` 流 | ✅ |
| MV 单位 | `MV.Scale=(1,1)`；SL `mvS=1/尺寸` | 探针 `1.155 px` / `69.263 px` = 像素制 | `mv_scale(1,1)` | ✅ |
| MV 符号 | API 契约 `prev-cur` | 探针跳过（declared 优先） | flags | ✅ |
| 深度方向 | `DepthInverted=1`（`flags 0x49`） | 探针 `reversed-Z`，与声明一致 | `kInputFlagInvertedDepth` | ✅ |
| jitter | NGX 参数 + `sl.const jit=...px` | 与 `dlssg.ngx jit=` 同序列 | `jitter_x/y` | ✅ |
| near / far / vFOV | `sl.const zn=0.0100 zf=75000 fovY=1.3265rad(76.00deg)` | 只在 FG 管线活着时出现 | 手工 `--fg-fov 76 --fg-zn 0.01` | ✅（靠手填，非自动） |
| `view_to_clip` | `sl.const cameraViewToClip`（同上一行，会停） | 当前 `camera: src_camera=0 read=0` | **未发布** | ❌ 用合成投影 |
| `clip_to_prev_clip` / `prev_clip_to_clip` | `sl.const`（当时趋近单位阵，静止画面） | 已发布 | 同名字段 | ❌ 只存不用 |
| `reset` | `sl.const reset=0`，全程没置 1 | 传送/换图未录到 | `kInputFlagResetHistory` | ⚠️ 逻辑已接、无数据 |
| MV invalid marker | `sl.const mvBad=3.403e+38` | 已发布 | `mv_invalid_value` | ❌ 未用于清洗 |
| dilated / 3D / cameraMotion | `mvd=0 mv3d=0 cmi=1` | 已发布 | `kInputFlagMv*` / `CameraMotion` | ❌ |
| `HUDLESS_COLOR` tag | **每帧**，3840x2160，`state=0x400`（已是 RT） | `sl.tags n=3` 稳定出现 | **未采集** | ❌ 颜色仍走 WGC ← 最值钱的下一步 |
| `UI_COLOR_AND_ALPHA` | 本作 **没有**这个 tag（穷举只有 0/1/2） | `n=3` 固定 | — | ❌ |
| `frame_key` / `frame_qpc_ms` | 需要 `slEvaluateFeature`，本作 FG 关 ⇒ 从不调用 | bridge `frame-key` = 0 次 | `frame_key`/`frame_qpc_ms` | ❌ 配对不可用（§3.4） |
| `present_id` | 没看到 | — | `present_id` | ❌ |
| tag extent / lifecycle / state | `onlyValidNow`；`0x60` / `0x8` / `0x400` | 日志逐帧 | 拷贝用的初始状态提示 | ✅ |
| HiResMv | `flags=0x49` 含 `1<<3` | MV 纹理 3840x2160 > 渲染 2228x1252 | `kInputFlagHiResMv` | ✅ **无需换算**：值本身是输出像素，正好是 XeFG 的约定 |

`flags=0x49` 解码：`0x01` InvertedDepth ｜ `0x08` HiResMv ｜ `0x40` MvActive。
（早期 DLAA 那场是 `0x9`：没有 `MvActive`，因为 bridge 还没请求 MV。）

### 4.3 一句话总结

**采集层**：本作比 CP2077 更好——SL 三件套（DEPTH + MOTION_VECTORS + HUDLESS_COLOR）
**每帧都在**、extent/lifecycle/state 齐全、MV 单位自报且实测吻合、深度方向实测吻合。
但 SL 的报数**只在游戏自己的 FG 管线活着时存在**，一旦切超分档位就断供，
所以真正扛住全程的是 NGX 的 SR 输入。

**消费层**：仍然和 2077 一样"一一对应、最小化"——
MV / 深度 / jitter / MV 尺度 / 深度方向 / 场景；`clip 矩阵`、camera 向量、
invalid marker、dilated/3D flag 全部只存不用；`HUDLESS_COLOR` 就在手边但没吃。


## 5. 与 CP2077 的差异

| 项目 | CP2077 | Khazan |
|---|---|---|
| 引擎 / 超分 | RED4 / SL 全家桶 | UE 4.27 / NGX 直调 |
| SL 用途 | SR + FG 都走 SL | **只有 DLSS-G** |
| `sl.tags` | MV 有，DEPTH 不每帧 | **DEPTH + MV + HUDLESS 每帧** |
| `sl.const` 相机 | 每帧 | 只在 FG 管线活着时 |
| 输入源 | `sl/streamline` | `ngx` |
| 相机 | 游戏自己的矩阵 | 无（`--fg-fov 76 --fg-zn 0.01`） |
| 帧率 / 2x 结果 | 60 → 120（整数） | 60 → 120（整数，DLSS 平衡）；30 + 3x → 90（非整档） |
| `frame_key` 配对 | 生效 | 不生效（无 `slEvaluateFeature`） |


## 6. 现状与下一步

### 已收口

- 输入源、相机值（`--fg-fov 76 --fg-zn 0.01`）、深度方向（reversed，实测一致）、
  MV 单位（pixels，实测一致）都确定了；
- 4K 2x 整档跑通：60 → 120 fps，`gpu` 中位 7.3 ms、`present` 中位 7.0 ms；
- 渲染尺寸在 2228x1252 / 2560x1440 / 3840x2160 之间切换**不用重启游戏**
  （content-size gen，slots 不变）。

### 仍未做 / 已知缺口

1. **frame_key 配对在 NGX 源下不生效**：见 §3.4，需要在 host 侧给任意源盖 QPC 时间戳；
2. **`resetHistory` 没有 A/B**：本作 SL `reset` 全程为 0（`sl.const ... reset=0`），
   传送/换图时也没看到置 1，需要在场景切换时录一段；
3. **探针会卡**：本轮早先出现过"只测一轮就停"，正常场景下后来每 120 帧又恢复了，
   说明它依赖 `ds_run` 的帧配对，slot 忙/降档时会跳过一轮（可接受，但要知道）；
4. **一条过时的告警**：`WARNING: render size is now 3840x2160 but the shared block was
   built for 2560x1440 ... MV/depth copies will be rejected` —— 实测在 DLAA(3840) 下
   MV/depth 拷贝**照常成功**（探针后来能在 2560x1440 流上量到 69 px）。这句文案来自
   content-size 机制之前，建议改成"按 content size 重采样"之类的实话；
5. **UE shipping 启发式**：`bridge.cpp` 里对 `*-Win64-Shipping.exe` 强制光流的规则
   应该改成"UE5 才强制"或者在探针证明 MV 可用后自动退回 `--game-mv`；
6. HUDless / UI 仍未消费（本作 `HUDLESS_COLOR` 每帧都在，是后续最值钱的入口）。


