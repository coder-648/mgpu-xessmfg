# fakerFG · XeFG 链路标准（v1，2026-11-14 实测通过）

本文件是 **fakerFG 在 XeFG 链路上的权威说明**：结构、开关、判定规则、已知坑。
所有结论都有实测日志支撑，出处见 `docs/design/FAKER_FG_PORT.md` §18–§27。

---

## 1. 结构（与 OptiScaler 对齐）

```
游戏 ──CreateSwapChainForHwnd──► [我们的 DXGI 代理]
                                     │  OwnSwapchain=1
                                     ▼
                       libxess 建链（D3D12InitFromSwapChainDesc）
                                     │  D3D12GetSwapChainPtr
                          ┌──────────┴───────────┐
             RealSwapchainObject=1          RealSwapchainObject=0
             交还「真链」+ vtable 钩子        交还我们的 FgSwapChain 门面
                          │
游戏每帧：GetBuffer / 渲染 / Present ──► 我们的 Present 路径
                                        · XeFG 输入 tag（在 DLSS-SR 的 Evaluate 里）
                                        · SetPresentId
                                        · XeFG 呈递（真实帧 + 插值帧）
```

关键原则（来自 OptiScaler，也是踩坑后唯一自洽的形态）：

1. **交出去的是 libxess 的真链**，不是代理对象。对象身份不同会让标题渲染到别处。
2. **输入在 XeFG 收输入的那个时点捕获**：`xefg_tag_at_evaluate()`，用**标题自己打开的命令列表**。
3. **只有一个呈递者**：要么 XeFG（链路模式），要么标题（provider 模式）。两者同时呈递 = 双层 + 双 Present 错配。
4. **libxess 认为自己在全屏时会静默拒绝插帧**：`SetEnabled` 仍返回 SUCCESS。

---

## 2. 开关（全部在 `fakerFG.ini`）

| 开关 | 默认 | 含义 |
|---|---|---|
| `OwnSwapchain` | 1 | 1 = 接管交换链（本链路的前提）；0 = 只观察，无插帧 |
| `RealSwapchainObject` | 0 | 1 = 交还真链 + vtable 钩子（`Present 8 / SetFullscreenState 10 / GetFullscreenState 11 / ResizeBuffers 13 / ResizeTarget 14 / GetFullscreenDesc 19 / Present1 22 / Waitable 33 / ResizeBuffers1 39`）；0 = 门面对象 |
| `FakeDlssgFeature` | 1 | 1 = 自己答 DLSS-G CreateFeature，让标题进入 FG 模式并交出全部输入（**输入捕获模式**）；0 = 拒绝，标题保留自己的 FG 关闭状态、把整帧渲染进链（**链路模式**） |
| `WriteOutputReal` | 0 | provider 模式：每次 Evaluate 把 `DLSSG.Backbuffer` 写进契约的 `OutputReal`（标题的命令列表上） |
| `MvResample` | 0 | 1 = 把标题的全分辨率 MV 重采样到 depth 尺寸并换算成 render 像素（`shaders/mv_resample.hlsl`，构建期 fxc 内嵌） |
| `ForceBorderless` | 0 | **必须是 1**，否则 libxess 判全屏并拒绝插帧 |
| `FgInput` / `FgOutput` | NoFG | `Upscaler` + `XeFG` = 输入取 DLSS-SR 参数块，输出由 XeFG 呈递 |
| `TagAtEvaluate` | 1 | **必须为 1**：tag 在标题打开的命令列表上、且 `MvResample` 只在这条路径里执行。为 0 时 tag 走 present 路径，`MvResample` 永远不跑，`Mv`/`Depth` 资源尺寸不一致的标题会被库以 `MISMATCH_INPUT_RESOURCES` 拒绝 |
| `Interpolated` | 1 | 每个真实帧生成几个插值帧 |

顺带保留（与 OptiScaler 同名同义，默认全关）：`FGDrawUIOverFG`、`FGUIPremultipliedAlpha`、
`FGHudCutoff`、`FGHudlessCompare`。

---

## 2.1 模式（`Mode=`）与一条新的绑定规则

| `Mode` | 谁生成帧 | 游戏自己的 DLSS-G | 用途 |
|---|---|---|---|
| `off` / `observe` | 游戏的运行时（observe 我们还记录） | **允许**（转发给 `nvngx_dlssg_real.dll`） | A/B 基线、数据调查 |
| `idle` | 没人（1X，原生画面） | 拒绝 | 验证"不生成也不黑屏" |
| `local` | **fakerFG（XeFG，本卡）** | **拒绝** | 自检平台：整条 FG 由我们跑完 |
| `consume` | **第二块卡（bridge）** | **拒绝** | 双卡出货形态：拿全输入、拒绝执行、原生帧上屏 |
| `bridge` | 同 `consume` | 拒绝 | `consume` 的旧名字 |

**绑定规则（2026-11-14 修）**：只要模式属于"我们自己拥有 FG 决策"（`idle` / `local` /
`consume` / `bridge`），`NVSDK_NGX_D3D12_CreateFeature(DLSS-G)` **绝不能转发给真运行时**。
以前 `FakeDlssgFeature=0` 会走 `fg_create_impl()` 的转发分支，结果游戏自己的 DLSS-G 仍然在
渲染卡上出帧（日志表现为 XeFG 与游戏 FG 同时工作）；而 `local` 的要求是"整条 FG 由 fakerFG
完成"，`consume` 的要求是"一点补帧开销都不要"。现在这两种模式都返回
`NVSDK_NGX_Result_FAIL_FeatureNotSupported`，标题于是关闭自己的 FG。

实测（CP2077 4K，同一台机器）：

| 模式 | 游戏 present | 游戏侧 fgEnabled | bridge |
|---|---|---|---|
| `local` | ~31.9 ms（31 fps） | 1（XeFG 2X） | 不需要 |
| `consume` | ~21.6 ms（46 fps，无补帧开销） | **0（1X）** | `fg=1 SUCCESS`，2X 由副卡出 |

## 3. 两种配方

### A. 链路模式 —— **通用默认**（有生成；参考实现 / 输入校验台）

`packaging/reshade_pack/fakerFG.ini` 与各游戏 ini 都按这一组配置。两个开关之所以能当通用默认：

* `MvResample=1`：MV 已经和 depth 同尺寸时**自动跳过**（`mv.width == dp.width && mv.height == dp.height`
  直接 return），对低分辨率 MV 的标题（CP2077）零影响；
* `ForceBorderless=1`：XeFG 本来就不支持独占全屏，作为 FG 输出时这是必须的；对窗口化标题无副作用。

```ini
OwnSwapchain=1
RealSwapchainObject=1
FakeDlssgFeature=0
WriteOutputReal=0
MvResample=1        ; 仅当标题的 DLSS 旗标不含 MVLowRes（全分辨率 MV）
ForceBorderless=1
FgInput=Upscaler
FgOutput=XeFG
```

成功的判据（三条同时、且持续）：

```
xefg: lane=Upscaler tags: mv <W>x<H> …, depth <same W>x<H> …      ← MV 与 depth 同尺寸
xefg: SetEnabled: true (lowResMV=1 nativeAA=0)                    ← 门控按 OptiScaler 判定通过
xefg: 2X present status framesPresented=2 (was 2) fgEnabled=1 result=OK
```

### B. provider 模式（无生成时保底 + 输入捕获）

```ini
OwnSwapchain=0
FakeDlssgFeature=1
WriteOutputReal=1
```

判据：`provider: OutputReal <- DLSSG.Backbuffer (WxH), contract satisfied`，画面正确、原生帧率、
菜单仍显示 x2（契约成立），输入全部经 `provider_eval` 落盘。

---

## 4. 判定规则（对号入座）

| 现象 | 原因 | 处理 |
|---|---|---|
| 黑屏 + 菜单 x2 + 原生帧率 | 标题进了 DLSS-G 契约、只画 UI；我们没出图 | 链路模式：`FakeDlssgFeature=0`；或 provider 模式：`WriteOutputReal=1` |
| 黑屏 + `blit` 类回拷恢复正常 | 交出去的是代理对象，标题渲染到别处 | `RealSwapchainObject=1` |
| `fgEnabled=1` 只维持一两帧后掉 0 | `Interpolation is not supported in fullscreen mode` | `ForceBorderless=1` |
| `FG stays off - high-res MV … render != display` | 标题给全分辨率 MV + 缩放 | `MvResample=1`（真的重采样，不用声明技巧） |
| `result=MISMATCH_INPUT_RESOURCES` 交替出现 | 两个呈递者/两条链交替 | 只保留一个呈递者（`FakeDlssgFeature=0`） |
| 画面双层（FG 帧叠原始帧） | 同上 + 标题每评估 Present 两次 | 同上 |
| `ResizeBuffers failed`（UE5 fatal） | flip 链 flags 被改动 | 已内置 `keep_flags()`，看日志 `preventing ResizeBuffers flag change` |
| 驱动 0xC00000D5 之类崩溃 | 持有/使用已释放的链外纹理 | 不要缓存标题纹理指针（旧回拷方案已删除） |
| `crash 0xC00000FD` + `dxgi.dll+0xB4EA` | MinHook 的 trampoline 没接住（detour 递归） | 必须 `MH_CreateHook(target, detour, (LPVOID*)&orig)` |

---

## 5. 代码地图

| 文件 | 职责 |
|---|---|
| `src/faker_fg/proxy_dxgi.cpp` | 工厂钩子、接管判定（`OwnSwapchain`）、`xefg_start()` |
| `src/faker_fg/backend_xefg.cpp` | XeFG 后端：建链、tag、present 派发、门控（`xefg_frame_feedback`）、`MvResample` 调用点 |
| `src/faker_fg/fg_swapchain_hooks.cpp` | 真链 vtable 钩子（OptiScaler `HookFGSwapchain` 的 9 个槽位 + borderless + keep_flags + waitable） |
| `src/faker_fg/fg_swapchain.h` | 门面对象（`RealSwapchainObject=0` 时使用） |
| `src/faker_fg/mv_resample.cpp` + `shaders/mv_resample.hlsl` | 全分辨率 MV → render 尺寸 |
| `src/faker_fg/passes.cpp` + `shaders/{render_ui,render_ui_pm,hud_copy,hudless_compare}.hlsl` | OptiScaler 三个合成 pass（默认关闭） |
| `src/faker_fg/provider.cpp` | DLSS-G 契约：Create/Evaluate/Release、`FakeDlssgFeature`、`WriteOutputReal`、输入采集 |
| `src/faker_fg/proxy_streamline.cpp` | Streamline 代理与 `slSetTag` 输入车道（本标题未使用） |
| `scripts/build_faker_fg.sh` | 构建 + `fxc` 编译着色器 + `embed_cso.py` 内嵌 |
| `scripts/check_fakerfg_regression.py` | 日志回归检查（任意日志路径） |

---

## 6. 换游戏的最小流程

1. 先跑一次，读两条基线日志：
   - `sr: upscaler create flags 0x%llX -> … HighResMv=?`（决定 `MvResample`）
   - `sr: upscaler lane render=… target=…`（相等 ⇒ nativeAA，`MvResample=0` 即可）
2. 选配方 A 或 B（见 §3），只改 ini，不改代码。
3. 用 §4 的表对号入座；判定通过的标准是 §3 里那三条日志**持续**成立。
4. 回归：`python3 scripts/check_fakerfg_regression.py <log>`。

---

## 7. XeFG 输入类型与来源（当前实现）

XeFG 从我们这里只接受三类东西：**资源 tag**、**帧常量**、**呈现控制**。下表是每一条的
来源，代码位置见 §5。

### 7.1 资源 tag（`D3D12TagFrameResource`）

| XeFG 资源 | Upscaler 车道（Wuchang / CP2077 都用） | DLSSG 车道 | 尺寸 / 状态 / 生命周期 |
|---|---|---|---|
| `RES_MOTION_VECTOR` | `NVSDK_NGX_Parameter_MotionVectors`（DLSS-SR 参数块，`sr_harvest`）；`MvResample=1` 时改为我们自己重采样出的纹理 | `DLSSG.MVecs` | `IsLowResMV ? render : target`；重采样时 = depth 尺寸。state = `MvState`（默认 0x40 `NON_PIXEL_SHADER_RESOURCE`），ValidNow |
| `RES_DEPTH` | `NVSDK_NGX_Parameter_Depth` | `DLSSG.Depth` | `render`（参数块 `Width/Height`）。state = `DepthState`（默认 0x40），ValidNow |
| `RES_HUDLESS_COLOR` | **不发**（`Upscaler_Inputs_Dx12` 在上采样器车道只在不做 HUD fix 时跳过） | `DLSSG.HUDLess`，仅 `TagHudless=1` | target 尺寸 |
| `RES_UI` | **不发** | `DLSSG.UI`，仅 `TagUi=1` | target 尺寸 |
| `RES_BACKBUFFER` | **不发** | 不发（OptiScaler 只在非 Upscaler 车道 tag 矩形） | 决定插值区域；不发即整幅 |

尺寸口径：`render` = 参数块 `Width/Height`；`target` = `OutWidth/OutHeight`，缺省取链宽高。

### 7.2 帧常量（`D3D12TagFrameConstants`）

| 字段 | 来源 |
|---|---|
| `jitterOffsetX/Y` | 参数块 `NVSDK_NGX_Parameter_Jitter_Offset_X/Y` |
| `motionVectorScaleX/Y` | 参数块 `NVSDK_NGX_Parameter_MV_Scale_X/Y`，**原样**传（不乘分辨率） |
| `resetHistory` | 参数块 `NVSDK_NGX_Parameter_Reset` |
| `frameRenderTime` | 我们自己测的**无 FG 时**的 present 间隔（`g_frame_ms_no_fg`，OptiScaler 的 `FrameTimeSource::Input`） |
| `viewMatrix` | 相机位置/朝向（参数块的相机数据；未知时为单位阵） |
| `projectionMatrix` | near/far（参数块 `CameraNear/Far` 或 ini）、`vertical_fov_deg`、aspect（链宽高比）；`DepthInverted=1` 交换 near/far；`far==0` 视为无限远 |

### 7.3 呈现控制

| 调用 | 值 / 条件 |
|---|---|
| `SetEnabled(true)` | 门控：`IsLowResMV \|\| nativeAA \|\| ForceFGRenderSizeMVs \|\| IgnoreInitChecks`（`Activate()` 原判） |
| `SetNumInterpolatedFrames` | 激活时写入 `Interpolated`（上限 `maxSupportedInterpolations`） |
| `SetPresentId` | 每次 Present 一个递增 id（`tag_begin` 分配；`>3` 未派发则 `Deactivate`） |
| `SetSceneChangeThreshold` | 固定 0.7（OptiScaler 默认） |
| init flags | `INVERTED_DEPTH`（ini/游戏旗标）、`JITTERED_MV`、`HIGH_RES_MV`（`MvResample=1` 时不设）、`UITEXTURE_NOT_PREMUL_ALPHA`（`UiPremultiplied=0`） |
| `uiMode` | `AUTO`（无 UI 输入时等价 NONE） |
| 插值区域 | 未发 colour/backbuffer ⇒ 整幅链缓冲 |

### 7.4 与双卡的关系

以上每一项都已经在 `provider_eval` / `sr_harvest` 里落进 `MgpuFgFrame`（资源指针 + 尺寸 + 格式 +
声明状态 + jitter + mvScale + reset + 相机参数 + 帧时间），也就是远端 GPU 复现所必需的完整输入集。
当前顺序是：**先本地跑通（本文档 §3A）→ 再把这套输入搬到远端生成 → 再把结果接回呈递或写
`OutputInterpolated`**。

## 8. 生产端接入（2026-11-14）

链路模式（§3A，`FakeDlssgFeature=0`）以前**根本不发布记录**：`publish_record()` 只在
DLSS-G provider 的 Evaluate 里调用，而这条配置下标题的 DLSS-G 是关的。现在：

| 位置 | 行为 |
|---|---|
| `proxy_nvngx.cpp::publish_upscaler_record()` | upscaler Evaluate 里、`TagAtEvaluate` 之后发布记录；`tagged` 时用 `xefg_last_eval_frame()` 取 **post-MvResample** 的那一帧，否则用 `sr_apply()` 的原始输入 |
| `faker_fg.cpp::publish_record()` | 锁外同步派发 `MgpuFgApi::subscribe` 的 sink（API v2） |
| `game_capture/in_faker.cpp` | sink 里当场 `MgpuCaptureRecordFgInputs(record.cmd_list, …)`：barrier 只用声明状态、每帧一个槽、按帧去重；`poll()` 只交元数据 |

为什么必须是 sink 而不是轮询：记录的 `cmd_list` 只在 Evaluate 期间打开，到
`reshade_begin_effects` 已经关闭；资源声明是 `ONLY_NOW`。轮询后再拷贝 = 在关闭的 list 上
录命令，或读已被复用的资源。

验证日志（链路模式）：

```text
record: upscaler lane (tagged, post-MvResample) mv=… depth=… render=… out=… flags=0x…
api: capture sink subscribed
faker: subscribed to the fakerFG record (api v2)
input source: faker (api 9) - pinned for this process
  [MV] EvaluateFeature copy 2560x1440 fmt 16 (state 0xC0)
  [depth] EvaluateFeature copy 2560x1440 fmt 40 (state 0x30)
```

bridge 侧看到 `api=fakerFG`；`shared_layout.h` / 协议不变。

