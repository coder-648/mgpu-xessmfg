# 协议 v13 + Streamline 输入源：CP2077 阶段记录

> 日期：2026-11-10　状态：**已实跑，功能可用；下一场景继续验证**
> 目标：`E:\SteamLibrary\steamapps\common\Cyberpunk 2077\bin\x64\Cyberpunk2077.exe`
> 相关：`docs/design/FG_INPUT_PLAN.md`、`docs/design/PROTOCOL.md`、`src/game_capture/in_fg.cpp`

## 0. 一句话

CP2077 上已经做到：

```text
游戏 DLSS-SR 开、DLSS-FG 关
  → Streamline slSetConstants / slSetTag / slEvaluateFeature
  → MV / 深度 / 相机 / 投影 / jitter / reset / frame_key
  → 现有共享槽 + FrameShared v13
  → bridge XeFG（4K 2x）
```

- 画面正常，颜色仍走 WGC；
- 4K `--den 1` 可稳定跑；
- frame_key ↔ WGC 时间配对已生效；
- **没有**做 HUDless/UI、颜色入槽、进程内 XeFG、swapchain 接管。

## 1. 当前配置（已验证）

`game_capture.ini`：

```ini
[capture]
Log=1
FgSniff=1
Input=auto
SlInput=1
SlFgOff=0
FgSuppressNgx=0
SkipMv=0
StreamlineCamera=1
Color=0
StreamPercent=100
StreamHeight=1440
```

游戏内：

```text
DLSS Super Resolution：开
DLSS Frame Generation：关
```

`dlssg_sm86.ini`：

```ini
[FrameGeneration]
Enabled=1
Engine=DLSS
MaxGeneratedFrames=1
```

说明：CP2077 上 sm86 只作为“让 DLSS-G 在 RTX20/30 可用”的存在；
本阶段最终不靠它出帧，也不靠它取数。

## 2. 已实现

### 2.1 协议 v13（只追加元数据）

`src/mgpu_shared.h`：

- `FrameShared` 追加：
  - `clip_to_prev_clip[16]`
  - `prev_clip_to_clip[16]`
  - `frame_key`
  - `frame_qpc_ms`（QPC ms，和 WGC `SystemRelativeTime` 同钟）
  - `present_id`
  - `fg_flags`
  - `mv_invalid_value`
  - `camera_pos/up/right/fwd`
  - `camera_pinhole[2]`
- 新增 flags：
  - `kInputFlagHasPrevClip`
  - `kInputFlagResetHistory`
  - `kInputFlagMvDilated`
  - `kInputFlagMv3D`
  - `kInputFlagCameraMotion`
  - `kInputFlagFgInput`
- 新增 `kInputApiSl = 5`、`kInputApiNvngxFg = 6`。
- **没有新增纹理流**。

### 2.2 Streamline 输入源

`src/game_capture/in_fg.cpp`：

- `[capture] SlInput=1` 时，把原来的只读诊断源变成输入源；
- 捕获：
  - `slSetConstants`：相机/投影/jitter/mvecScale/near/far/fov/flags/reset；
  - `slSetTag` / `slSetTagForFrame`：depth/MV 的 native `ID3D12Resource*`、extent、lifecycle、state；
  - `slEvaluateFeature`：frame token、`frame_qpc_ms`、serial；
  - `slGetPluginFunction`：为 `SlFgOff` 预留的 DLSS-G 入口；
- `poll()` 返回 `FrameInputs`；`track()`/`hold()` 交给现有 host 拷贝路径；
- 通过 `GetModuleHandleW` 补抓被 loader 队列丢掉的 `sl.interposer` / `sl.dlss_g` / `nvngx_dlssg`。

### 2.3 CP2077 特化修正

日志证据：

```text
fg: sl.const ... mvS=(1.000000,1.000000) mvPx=1x1 ...
fg: dlssg.ngx ... mvecScale=(3840.0000,2160.0000)
```

- SL 的 `mvecScale=(1,1)` 是误导值；
- 用 NGX 的 `MV.Scale=(3840,2160)` 侧通道修正；
- 在线 probe 独立判成 `mv=uv(measured)`；
- `cameraFOV` 是角度制（75/60），源里转弧度；
- `cameraViewToClip` 有效，bridge 不再用 `--fg-fov`。

### 2.4 frame_key ↔ WGC 配对

`src/bridge/bridge.cpp`：

- WGC 每次交付记录 `SystemRelativeTime`（`g_wgc_ts_ms`）；
- 选槽时扫描 `frames[s].frame_qpc_ms`，选“最新且不晚于 WGC 时间”的槽；
- 窗口 `-2..250 ms`；
- 没匹配就回退原来的轮转；
- `--no-pair-key` 可关闭。

首次实测：

```text
[bridge] frame-key pair: WGC 8150986.6ms -> slot 2
         (frame_key=140715473866168, age 15.6ms)
```

### 2.5 resetHistory

`kInputFlagResetHistory` → `cd.resetHistory`：

```cpp
cd.resetHistory = (fr.fg_flags & mgpu::kInputFlagResetHistory) ? 1u : 0u;
```

逻辑已接；场景切换/传送的 A/B 仍待测。

### 2.6 已采集但**未消费**（按“不做额外工作”收口）

| 字段 | 状态 |
|---|---|
| `clip_to_prev_clip` / `prev_clip_to_clip` | 已发布，未消费 |
| `camera_pos/up/right/fwd` | 已发布，未消费 |
| `mv_invalid_value` | 已发布，未用于清洗 |
| `mv_dilated` / `mv_3d` / `camera_motion_included` | flag 已发布，未决策 |
| `present_id` | 已发布，未配对 |
| HUDless / UI | 完全没碰 |

## 3. v12 → v13 的数据丰富度

| 数据 | v12 | v13 | 进入 XeFG |
|---|---|---|---|
| MV | NGX 指针 | SL tag：native + extent + lifecycle + state | ✅ |
| 深度 | NGX 指针 | SL tag（新鲜度待证明） | ✅ |
| `view_to_clip` | 有 | 有 | ✅ |
| `clip_to_prev_clip` | ❌ | ✅ | ❌ |
| `prev_clip_to_clip` | ❌ | ✅ | ❌ |
| near/far/vfov | 多数 0 | ✅ SL 声明 | ✅ |
| camera vectors | ❌ | ✅ | ❌ |
| jitter | ✅ | ✅ | ✅ |
| MV scale | NGX | SL + NGX 修正 | ✅ |
| depthInverted / mvJittered | ✅ | ✅ | ✅ |
| mvDilated / mv3D / cameraMotion | ❌ | ✅ | ❌ |
| resetHistory | ❌ | ✅ | ✅ |
| MV invalid marker | ❌ | ✅ | ❌ |
| frame_key / frame_qpc_ms | ❌ | ✅ | ✅ WGC 配对 |
| tag extent/lifecycle/state | ❌ | ✅ | extent/state 用于拷贝 |

结论：**采集层明显更丰富；消费层只增加了 frame_key↔WGC 和 resetHistory，其余保持一一对应。**

## 4. 实跑数据（RTX 3080 Ti + RX 6750 GRE）

4K 原生 2x：

```text
bridge.exe --game Cyberpunk2077.exe --den 1 --pool 4 --interp 1
           --fg-ui none --fg-proj-rz --fg-rt-mode quant
```

日志：

```text
/mnt/c/temp/mgpu/bin/bridge_v13_pair.txt
```

关键行：

```text
producer api=sl/streamline flags=0x31D1
producer semantics: mv=uv(measured) sign=prev-cur(declared) depth=reversed(measured)
projection: from the game's own view-to-clip ... fovY=51.01deg aspect=1.778
frame constants: ... mvscale=(3840.0000, 2160.0000) camera=producer
bb=3840x2160
out: presents=80 expected=80
gpu=0.7–9.8ms
present=6–9ms
compose->photons=13.7–22.1ms
```

帧循环健康：

```text
wait=0.0ms
timeouts=0
q=0–2/4
ov=36（历史累计，主要在启动）
```

## 5. 失败/禁止的路径

### 5.1 `FgSuppressNgx=1` 不能用于 CP2077

- 日志会打印 short-circuit；
- 但游戏的 SL/DLSS-G present 链路继续跑；
- 结果是**黑屏 + 仍按 FG 节奏**；
- 已把配置改为 `FgSuppressNgx=0`，游戏内手动关 FG。

### 5.2 `SlFgOff=1` 在 CP2077 不生效

- `slDLSSGSetOptions` 没有通过当前 hook 到的路径拿到；
- `sl.dlss_g` 的 `slGetPluginFunction` 也没有稳定命中；
- 代码保留，但 CP2077 使用 `SlFgOff=0`。

### 5.3 4K 慢的根因不是 XeFG

早期 4K `gpu=20–40ms` 是：

- 游戏自己的 FG 还在跑；
- `FgSuppressNgx=1` 的失败路径；
- 两套插帧叠加。

游戏 FG 关闭后，4K `--den 1` 的 `gpu` 为 0.7–9.8ms。

## 6. 当前已知缺口

1. **深度新鲜度**：CP2077 的 SL `DEPTH` tag 不是每帧都出现；`g_sl_cap.depth` 会保留上一份指针。流是通的，但没有证明每帧都是新鲜 SL depth。下一个场景要重点验证。
2. **clip 矩阵/camera 向量/invalid marker** 只发布，不消费。
3. **frame_key 配对是时间戳近似**：WGC 没有游戏帧号；窗口外会回退轮转。
4. **resetHistory** 逻辑已接，但没有场景切换 A/B。
5. **没有 HUDless/UI**：颜色/UI 仍由 WGC + XeFG 插值承担。

## 7. 新场景（第 3 款）开始前

> 场景 2（The First Berserker: Khazan，UE 4.27）已跑完：见
> `docs/findings/FG_INPUT_V13_KHAZAN.md`。经验：**先看游戏把 SL 用在哪**——
> Khazan 的 SL 只做 DLSS-G（引擎插件里没有 `sl.dlss.dll`），DLSS-SR 走游戏自带的
> `nvngx_dlss.dll`，所以输入源是 `ngx` 而不是 `sl`，而 SL 的声明只在它自己的 FG
> 管线活着时存在。

开始前先确认这四件事：

1. **游戏 exe / 目录**（部署 add-on + 写 ini 用）；
2. 游戏内 **DLSS-SR 开、DLSS-FG 关**（本项目的 FG 由 bridge 出）；
3. 分辨率 / 输出屏（先 4K `--den 1`；超预算再 `--scale` / `--den 2`）；
4. 同款游戏换区域时不用重启组件，换游戏才重新部署。

跑起来后按下面两条分支看日志：

**A. SL 是输入源时**（日志 `input source: sl (api 5)`）

- [ ] `fg: sl.const #N` 出现，且 `camera: using the input source's own view-to-clip`；
- [ ] `fg: sl.tags` 是否同时有 `MOTION_VECTORS` 和 `DEPTH`；
- [ ] bridge `producer api=sl/streamline`、`mv=...`、`mvscale`；
- [ ] `frame-key pair` 出现（需要游戏自己 evaluate FG）。

**B. NGX 是输入源时**（`input source: ngx (api 1)`，如 Khazan）

- [ ] `ngx: sr mv=... depth=...` 的分辨率 vs 渲染分辨率（判断要不要 `HiResMv`）；
- [ ] `semantics: mv=...`，并且**让角色走 10 s** 再看 `probe: mv`——静止画面下
      探针只会给 `ambiguous`/`UV`，那是噪声不是结论；
- [ ] 相机：`camera: not used` ⇒ 用游戏自己声明过的 `zn/zf/fovY` 补
      `--fg-fov/--fg-zn`，别用默认 60°；
- [ ] `*-Win64-Shipping.exe` 会被 bridge 强制光流，需要游戏 MV 时加 `--game-mv`。

通用：

- [ ] `wait=0`、`timeouts=0`、`out: presents=80 expected=80`；
- [ ] `gpu` / `present` 在预算内（4K 参考：2x `gpu` 中位 7 ms、`present` 7 ms）；
- [ ] cadence `1.00 vblank/frame` 才算整档（`quant` 行会给整档所需的输入帧率）；
- [ ] 场景切换时看 `resetHistory`；
- [ ] **不要 `taskkill bridge.exe`**：overlay 消失后游戏会跟着退（Khazan 上 2/2 复现）。


## 8. 构建/部署快照

## 9. v14 在 CP2077 上的验证（2026-11-10 13:45）

同一套 v14 add-on（`0742e272…`）+ bridge（`20da1722…`）在 RED4 上跑起来：

```text
config: SlInput=1 SlFgOff=0 Claim=0
fg: Streamline hooks active (5 installed, 1 not exported)
harvest: mv=y depth=y camera=n clip=y token=y reset=n hudless=n ui=n scaling=io expose=n placeholder=n
input source: sl (api 5) - pinned for this process
camera: using the input source's own view-to-clip (v13, sl)

[bridge] producer declaration: framesToGenerate=0 harvest=0x31B
         (mv=y depth=y cam=n clip=y token=y reset=n hudless=n ui=n)
[bridge] producer supplies the camera (near/far/vfov) - the projection no longer depends on --fg-fov
[bridge] frame constants: ... camera=producer fov=51.0deg zn=0.02 zf=16000
[bridge] cadence: input=16.7ms = 2.00 vblanks -> 1.00 vblank/frame, 120.0 fps out
[bridge] out: presents=80 expected=80    breakdown: gpu 0.5-6.6ms present 5.8-6.1ms age 4.2-4.9ms
frame-key pair 命中一次（设计上只打第一行）
```

- **新发现（已修）**：`harvest` 报 `camera=n`，但 bridge 明确说 `camera=producer fov=51.0deg`。
  原因是 **CP2077 在 `cameraFOV` 里放的是角度（75/60）**，而我在 harvest 判定里按弧度比较
  （`0.05..2.5`），75 > 2.5 就直接判否；poll() 的路径有度数→弧度的转换，所以两侧不一致。
  修法：`camera_fov_radians()` 统一转换，并且占位检测同时接受 90° 与 90 度（`fov90 || fov≈90.0`）。
  ——这正是 `harvest` 位图的价值：**它把"发布出去的 flag"和"我们以为声明里有的东西"对不上的地方暴露出来了**。
- 与 v13 相比，本作新增可见项：`scaling=io`（CP2077 也 tag SCALING_INPUT/OUTPUT），
  `hudless=n`（FG 关着时确实不给，与 PRAGMATA 一致）。
- 数值面依旧健康：4K 原生 2x、60 fps 进 → **120.0 fps 整档**、`gpu` ≤6.6 ms、`age` 4–5 ms。

### 9.1 光追下设备移除（`DXGI_ERROR_INVALID_CALL`）—— 已定位并加固

现象：**开光追 + 启动 bridge ⇒ 游戏闪退**。ReShade 日志给出确凿原因：

```text
13:50:14:215 | ERROR | Device was lost with DXGI_ERROR_DEVICE_REMOVED!
13:50:14:215 | ERROR | > Device removal reason is DXGI_ERROR_INVALID_CALL.
```

同一毫秒，add-on 的日志是：

```text
13:50:14.112   [mv] convert pass 3840x2160 -> 2560x1440, tracked state = 0x8
```

对照同一张日志里所有历史成功的行：

| 时间 | 光追 | `tracked state` | 结果 |
|---|---|---|---|
| 13:46:30 | 关 | **0xC0**（ALL_SHADER_RESOURCE） | 正常，120.0 fps 整档 |
| 13:50:14 | **开** | **0x8（UNORDERED_ACCESS）** | 设备移除 |

⇒ 开光追后，CP2077 的 MV 在 present 时刻**仍处于 UAV（异步队列还在写）**；
我们对它做 `UAV → ALL_SHADER_RESOURCE` 转换并采样，GPU 判定为非法调用 → 设备移除 → 游戏死。

**加固（build `dff85181…`）**：`record_downsample` / `record_copy_region` /
`record_depth_convert` 三处统一加锁：

```text
[mv] skipped: left in UNORDERED_ACCESS (0x8) at present time - reading it here can remove the
     device (seen on CP2077 with ray tracing). Use --fg-of or --no-mv for this title.
```

即**绝不碰"游戏留在 UAV 状态"的资源**。代价是该流这一帧不产出，画面侧由消费端回退；
收益是**游戏不会崩**。光追场景推荐直接 `--fg-of`（光流 MV，不读游戏 MV）。


```text
build/game_capture.addon64  md5 f9a7d9ca0982cacc90d410a112175c48   (v13 + ensure_minhook 修复)
build/bridge.exe            md5 9eb9268cc0c466b7b06f994400958f44
```

部署位置：

```text
E:\SteamLibrary\steamapps\common\Cyberpunk 2077\bin\x64\game_capture.addon64
C:\temp\mgpu\bin\bridge.exe
```

协议：`kSharedVersion = 13`，add-on / bridge / UI 必须同版。

### 9.2 真正的根因：光追把 MV 的**像素格式**换了（RG16F → RGBA16F）

§9.1 的对照表里还有第二处差异，而且它才是 `INVALID_CALL` 的直接来源：

```text
shared: mv at 2560x1440 (height cap, format 34) ...   ← 光追关：R16G16_FLOAT
shared: mv at 2560x1440 (height cap, format 10) ...   ← 光追开：R16G16B16A16_FLOAT
ngx: sr mv=... fmt=10                                 ← 同一时刻 NGX 也是 fmt 10
```

而转换 pass 的 SRV 是**写死 R16G16_FLOAT**（`ds_run`）。D3D12 的 typed view 必须
与资源同格式族 ⇒ 用 RG16F 视图采样 RGBA16F 资源 = **非法调用** ⇒
`DXGI_ERROR_DEVICE_REMOVED / INVALID_CALL` ⇒ 游戏闪退。

**修法（`bd0f4f62…`）**：SRV 格式跟着源资源走

```cpp
switch (src_fmt) {
case DXGI_FORMAT_R16G16B16A16_FLOAT: srv_fmt = ...A16_FLOAT; break;   // 光追开
case DXGI_FORMAT_R32G32B32A32_FLOAT: srv_fmt = ...A32_FLOAT; break;
case DXGI_FORMAT_R32G32_FLOAT:       srv_fmt = R32G32_FLOAT; break;
default:                             srv_fmt = R16G16_FLOAT;          // 光追关
}
```

shader 只读 `.rg`，所以多出来的两个通道不影响结果；流仍然是 RG16F。

**同时保留 §9.1 的 UAV 防护，但加了开关** `[capture] MvFromUav=0|1`（默认 0）：
既然格式已修，UAV 那一条是否仍然致命只需要翻一次 ini 就能验证（失败模式是游戏崩）。
若 `MvFromUav=1` 跑通，就说明光追下 MV 可以正常取；跑不通就保持默认 + `--fg-of`。
