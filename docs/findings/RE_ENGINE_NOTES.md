# RE Engine 备注（PRAGMATA 案例，2026-11-09）

> 状态：**修复已实现，但未经真机验证**（游戏是破解版，启动加载极慢，暂时搁置）。
> 下次验证只需要看 `game_capture.log` 里有没有 `NGX provider: ...nvngx_dlss.dll` +
> `NGX hooks active`（步骤见 §5）。

## 1. 现象

PRAGMATA（`E:\PRAGMATA\PRAGMATA.exe`，RE Engine，DX12 + HDR10）：

- ReShade **确实加载了 add-on**：
  `Loading add-on from 'E:\PRAGMATA\game_capture.addon64' ...` /
  `Registered add-on "game_capture" v0.0.0.0 using ReShade API version 20.`
- 但**没有任何数据产出**（bridge 连不上，界面看起来就像"add-on 没加载"）；
- `game_capture.ini` 当时是 `Log=0`，所以连 add-on 自己的日志都没有。

## 2. 根因：NGX 入口不在驱动 shim 上

add-on 的 NGX 输入源只挂**驱动商店的 shim**：

```cpp
// src/game_capture/in_ngx.cpp（修复前）
if (!wcsstr(full, L"_nvngx.dll")) { return; }   // driver shim only, per M2
```

而 RE Engine 走 **Streamline**（游戏目录有 `sl.interposer.dll` / `sl.dlss.dll` / `sl.dlss_g.dll`），
它调的是**游戏目录自带的 `nvngx_dlss.dll`**（44 MB）。用 `strings` 可以直接确认这个 DLL
**导出 NGX 入口**：

```
NVSDK_NGX_D3D12_CreateFeature
NVSDK_NGX_D3D12_EvaluateFeature      ← 我们要挂的两个
NVSDK_NGX_D3D12_Init / Shutdown / ReleaseFeature / GetScratchBufferSize ...
```

文件名 `nvngx_dlss.dll` 不含 `_nvngx.dll` 子串 ⇒ 被过滤掉 ⇒ **一个钩子都没装** ⇒ 无输入。

> 这条"只挂 shim"的规则来自 M2 的 CP2077 结论（它加载的几个 `nvngx_dlss.dll`
> 只是为了读版本号）。所以修法不是简单放宽文件名，而是"文件名 + 真的导出入口"。

## 3. 修复（已编译，`build/game_capture.addon64` md5 `9663290d…`）

`src/game_capture/in_ngx.cpp` `hook_module_if_ngx()`：

1. 取**文件名**（不再匹配整条路径）；
2. 文件名必须含 `nvngx`（同时覆盖 `_nvngx.dll`、`nvngx.dll`、`nvngx_dlss.dll`）；
3. 排除 `dlssg` / `dlssd` / `dlssnr`（帧生成 / 光追重建 / NR 不是本项目的输入源）；
4. **`GetProcAddress("NVSDK_NGX_D3D12_EvaluateFeature")` 成功**才挂 —— CP2077 那些
   只读版本号的副本没有导出，仍然被挡掉。

副作用评估：若 shim 与运行时**同时**被挂，一帧会记录两次；`mgpu::input::hold()` 是
`AddRef + 延迟释放`（`game_capture.cpp:1495`），成对释放，不会泄漏；`serial` 多增一次
对 `poll()` 无影响（它只判断"有没有更新"）。

### 3.1 真机验证（2026-11-09 08:21，部分通过）✅

替换 add-on 后第一次进游戏，`game_capture.log` 出现：

```
[08:21:40.321] NGX provider: C:\WINDOWS\System32\DriverStore\...\_nvngx.dll @ 00007FF9A3960000
[08:21:40.415] hooked NVSDK_NGX_D3D12_EvaluateFeature @ ...8740
[08:21:41.832] NGX provider: E:\PRAGMATA\nvngx_dlss.dll @ 00007FF909CC0000
[08:21:41.920] hooked NVSDK_NGX_D3D12_EvaluateFeature @ ...CF4DA0     ← RE Engine 走的就是这条
[08:21:42.010] hooked NVSDK_NGX_D3D12_CreateFeature   @ ...CF4C30
[08:21:43.602] NGX provider: ...\_nvngx.dll
[08:21:43.602] hook NVSDK_NGX_D3D12_EvaluateFeature FAILED             ← 见下
```

⇒ **判据修对了**：两个 provider 都挂上了，并且拿到了游戏的声明

```
dlss feature flags 0xB: MVJittered=0 MVLowRes=1 DepthInverted=1 (feature 1)
```

`0x0B = MVLowRes | DepthInverted`（这次没 `IsHDR`，与 PBZ 的 `0x4B` 不同；HDR 取决于游戏内设置）。

**同时暴露一个日志级 bug（已修）**：shim `_nvngx.dll` 的导出是**转发**到我们已经挂过的
运行时函数，MinHook 在第二次挂同一地址时失败，于是打印了吓人的
`NGX hooks FAILED` —— 功能上无害（第一组钩子仍在工作），但会误导排查。
现在：一旦某个 provider 成功挂上就 `g_ngx_hooked = true`，不再尝试别的模块；
失败也只打印 `NGX hook attempt failed, ignored`，且不再覆盖 `g_module`。
（`build/game_capture.addon64` md5 `9663290d…`）

**仍未验证的部分**：这轮之后游戏卡在自身启动/加载里（见 §6），没有进世界，
所以 MV/深度是否可用、插帧效果如何都还没测。

## 4. 该游戏的其它已知情况

| 项 | 值 |
|---|---|
| 引擎 / 显示 | RE Engine，DX12，**HDR10 活跃**（`IDXGISwapChain3::SetColorSpace1(12)`） |
| 超分 | `config.ini`：`UpscalingAlgorithm=DLSS`、`DLSSQuality=DLAA`、`FrameGeneration=Off`、`RayTracing=OFF` |
| 已装 mod | **REFramework**（`re2_framework_log.txt` / `reframework/`）、`dlssg_to_fsr3_amd_is_better.dll`、`amd_fidelityfx_*` |
| Streamline | `sl.interposer.dll` / `sl.dlss.dll` / `sl.dlss_g.dll` / `sl.reflex.dll` 等 |
| 渲染卡 | RTX 3080 Ti（ReShade 日志：`Running on NVIDIA GeForce RTX 3080 Ti Driver 617.42`） |
| 特殊 | **破解版**：启动/加载极慢，是这次没能真机验证的直接原因 |

> OptiScaler 对这个游戏有专门的条目（`pragmata.exe`：`RestoreComputeSignature = true`、
> `DxgiSpoofing = false`、quirk `PregmataFixDLSSModes`），说明 RE Engine 这类题目对
> 注入/签名比较敏感 —— 如果我们后面还要在这类游戏上做事，值得回看它的做法
> （`temp/optiscaler_src/.../misc/GameConfigs.cpp:303-316`）。

## 5. 下次验证步骤（10 分钟）

游戏目录里已经放好：

- `game_capture.addon64.new` —— 修好的构建（md5 `db33a9c7…`）；
- `apply-mgpu-addon.bat` —— **关掉游戏后**双击：确认进程退出 → 旧文件改名
  `game_capture.addon64.prev` → `.new` 顶上去；
- `game_capture.ini` 已设 `Log=1`（备份 `.mgpu_bak2`）。

然后启动游戏进世界（DLSS/DLAA 开）跑十几秒，看 `game_capture.log` 是否出现：

```
NGX provider: E:\PRAGMATA\nvngx_dlss.dll @ ...
NGX hooks active
input source: ngx (api 1) - pinned for this process
shared: mv at ... (input render size, format 34), depth source ...
```

出现即通。之后：

1. 把 release 重做成 **v0.13.1**（含此修复，`0.13.0` 的 add-on 没有）；
2. 观察 RE Engine 的 MV 是否可用（`7 - UE5 MV A-B.bat <exe> dump 1` + `analyse_dump.py`
   的 units fit），DLAA 情况下预期与 PBZ 一样干净；
3. 若 REFramework 与我们的钩子冲突（表现为进出世界崩溃），先卸 REFramework 再测一次。

## 6. 观察：加载期 GPU 占用 0%（**不是**掉驱动，2026-11-09 08:30）

现象：PRAGMATA 启动/加载阶段任务管理器里 GPU 占用 0%，怀疑驱动掉了。实测排查：

| 检查 | 结果 |
|---|---|
| 系统事件 `4101`（TDR，显示驱动重置） | **近 3 天一条都没有** |
| `nvlddmkm` 事件 | 只有 `Id 153`，时间都是 **11-08 20:24–20:27**（昨天），与本次无关 |
| `nvidia-smi` | `P0`、`0 %`、`4462/12288 MiB`、`39 °C`，能枚举到 `PRAGMATA.exe` |
| 进程状态 | `Responding=True`；5 秒内 CPU `+5.7 s`（~114% 单核），**不是挂死**；主窗口标题 `PRAGMATA` |
| 磁盘 I/O | 5 秒**读增量 0 MB**，`PhysicalDisk` 队列长度 `0.0` |
| 崩溃转储 | `CrashDumps` 里只有无关的 `MFResident.exe`，没有 PRAGMATA 的 |
| 应用日志 | 没有与 `PRAGMATA / bridge / nvngx / d3d12 / dxgi` 相关的错误 |
| ReShade 日志 | 该轮最后是反复 `CreateDXGIFactory2`；**`game_capture.log` 这轮一行都没写** |

⇒ 结论：**驱动是好的**；游戏卡在自己的启动/加载路径（CPU 自旋、不读盘、不建交换链），
连 ReShade 之后的 add-on `AddonInit` 都没走到 —— 所以也与 add-on 无关。
破解版 loader / DRM 校验在等待时通常就是这个形状。

下次若再遇到，按这个顺序排除：

1. 先把 **REFramework** 临时移走（`dinput8.dll` + `reframework\`），它同样 hook D3D12；
2. 看 `re2_framework_log.txt`（RE Engine mod 框架自己的日志）与游戏自己的崩溃报告；
3. 用 `procdump -ma` 抓一次转储看主线程栈（是自旋还是等一个不会来的事件）；
4. 确认 `re_chunk_000.pak`（19.8 GB）+ `.sub_000.pak`（16 GB）所在盘没有掉速/校验。

## 6.1 另一个观察：启动"非常非常慢"（待归因，2026-11-09）

用户反馈：装上抓取组件后启动变得极慢（出现画面了，但过程很长）。

从日志能确定的（`ReShade.log` / `ReShade.log1` / `game_capture.log`）：

| 观察 | 数值 |
|---|---|
| ReShade 自己覆盖的启动区间 | **~50 秒**（`08:27:19→08:28:08`），最大单段空隙 21.2 s |
| add-on 是否在干活 | **`shared ready` 一次都没出现** ⇒ 整个进程里它没发布过数据，加载期几乎不做事 |
| add-on 的"无输入"心跳 | `no upscaler input yet` 是每 **120 帧**打一次；它从 `08:21:45` 跳到 `08:24:14` ⇒ 那 148 s 里只有 **0.8 fps**（游戏在加载，不是我们在跑） |
| DLSS 声明 | `0xB`（`MVLowRes｜DepthInverted`），`08:21:46` 与 `08:24:16` 各一次，中间是加载 |

⇒ 时间基本花在**游戏自己的加载**（36 GB 的 `re_chunk_000.pak` + `.sub_000.pak`、破解 loader、shader 编译）；
add-on 在这一段没有路径会耗时（`on_begin_effects` 在拿不到输入时立刻返回，`barrier` 只做 ≤8 项查表）。

**2026-11-09 追加：A/B 做完了 —— `mgpu-toggle-addon.bat` 关掉 add-on 后加载恢复正常**，
所以这笔账确实是我们的（add-on 关掉、ReShade 仍加载时就不慢，说明不是 ReShade）。
当前主嫌疑：`kernel32!LoadLibrary*` 的进程级钩子 —— RE Engine 的加载器是多线程的，
我们的钩子里每次成功加载都要 `GetModuleFileNameW`，与 loader lock 抢跑很容易形成护送效应。

为此加了两样东西（add-on md5 `181687fb…`）：

1. **`[capture] LoaderHook=0`**：完全不碰 `kernel32`，改为每 120 帧扫一次模块表；
2. **回调计时**：每 120 帧（以及前 5 帧）打一行
   `perf: frames=.. begin=..ms barrier=..ms (.. calls / .. items) present=..ms`，
   直接把"我们的回调占了多少时间"量出来 —— 若这些数字都很小，那问题就只剩 loader hook。

下一轮验证：把 `LoaderHook=0` 跑一次（PRAGMATA 的 ini 已设好）。若加载恢复正常 ⇒ 结论确定，
把该键对 RE Engine 默认设 0（或彻底改成"扫描 + 需要时再挂"）；若仍然慢 ⇒ 看 `perf:` 行的
三个数字哪个大。

（下面这条 A/B 记录保留作历史）游戏目录另有两个一键开关（都要先关游戏）：

| 脚本 | 作用 | 这一档能回答 |
|---|---|---|
| `mgpu-toggle-addon.bat` | 只把 `game_capture.addon64` ↔ `.off` | 是不是 **add-on** 的锅（ReShade 仍然加载） |
| `mgpu-toggle-reshade.bat` | 把 `dxgi.dll` ↔ `dxgi.dll.off` | 是不是 **ReShade** 的锅（游戏完全原状） |

三档各测一次"从双击到主菜单"的秒数：现状 / 关 add-on / 关 ReShade。若三档接近 ⇒ 是游戏自身；
若关掉 add-on 就快 ⇒ 我这边要查 `LoadLibrary` 钩子在 RE Engine 加载器上的开销。

**顺带修掉一个真 bug（2026-11-09）**：`init_swapchain` 之后的采集暂停原来是纯**按帧**计的
（`pausing capture until frame +120`）。这游戏加载期只有 0.8 fps ⇒ 120 帧 = **150 秒**，
整个启动期都处于暂停（这也是为什么 `shared ready` 一直没出现）。现在改成
「帧数或 **8 秒**，先到先算」（`g_skip_until_ms`），高帧率下行为不变，慢加载游戏不会再被卡住整段启动。
add-on 重编：md5 `ddf2d8c7…`。

顺带记一个无关紧要但会误导的现象：`ReShade.log` 里出现过
`Preset ... uses unknown technique 'MGPU_DepthTap@mgpu_depth_tap.fx'`（下一次启动就好了），
是 ReShade 在特效还没编译完就套 preset 的时序问题，影响的只是"深度绑定"那一次。

## 6.2 热键"没反应"的真相（2026-11-09 08:43）

`mgpu_ui` 的热键其实是**收到了**的：

```
08:43:03.708  WM_HOTKEY id=1
08:43:03.709  hotkey/button: running=0 child_pid=0
08:43:03.709  start: target ... is not the producer (pid=26344 producer_pid=0 version=0 expect=10)
```

`target_is_producer()` 打不开 `Local\mgpu_mailbox_v1`（打开失败时它把 producer_pid/version 填 0），
也就是 **add-on 那一刻还没有创建过共享块** —— UI 的这条检查是故意的（防止对着浏览器启动 bridge），
所以"热键没反应"实际是"add-on 还没发布过任何一帧"。

顺带做了一次隔离实验：用最小宿主（`C:\temp\mgpu\addon_host.cpp`）直接 `LoadLibrary` 这个
add-on 并调用 `AddonInit(addon, host)` —— **返回 1 且日志正常写出**（`game_capture.log` 5972→6764 B），
说明 DLL 本身没问题；失败只发生在游戏进程里，而且是在 `open_log()` 之前/之中（那一轮 
ReShade 已经打印了 `Registered add-on`，那行是 `AddonInit` 内部 `reshade::register_addon` 触发的，
说明 `AddonInit` 确实跑到了）。

**改进（add-on md5 `a653aff…`）**：`open_log()` 在本地文件打不开时，自动落到
`%LOCALAPPDATA%\mgpu\game_capture-<pid>.log`，并把失败原因（Win32 错误码）记下来。
下次一跑就能区分"写不进去"和"根本没跑到"。

### 6.3 根因：游戏从 `_storage_\` 读文件（2026-11-09 08:54）

面包屑一装上就定位了（`%LOCALAPPDATA%\mgpu\breadcrumb.log`）：

```
[08:52:56] pid 22976  AddonInit  addon=E:\PRAGMATA\game_capture.addon64          ini=...\game_capture.ini      Log=1  g_log=open   ← 宿主自测
[08:54:41] pid 5160   AddonInit  addon=e:\pragmata\_storage_\game_capture.addon64 ini=...\_storage_\game_capture.ini  Log=0  g_log=null   ← 游戏
```

**游戏进程是从 `E:\PRAGMATA\_storage_\` 加载 add-on 的**（`_storage_` 是它自己的 50 文件 / 528 MB
DLL 镜像；`dir /AL` 看不到联接点，是进程级重定向）。于是：

- 它找的 ini 是 `_storage_\game_capture.ini` —— **一直不存在** ⇒ `Log` 取默认 0 ⇒ **没有任何日志**；
- 我们改的 `E:\PRAGMATA\game_capture.ini`（Log=1 / LoaderHook=0）**从来没被读过**；
- `SkipMv` / `StreamHeight` / `DepthConvert` 等也全部取默认值。

修法（v0.13.1）：

1. 把 `game_capture.ini` + add-on 放进 `_storage_\`；
2. add-on 增加 **ini 回退**：本地 ini 不存在时读 `%LOCALAPPDATA%\mgpu\game_capture.ini`；
3. 安装器新增 **6/6 镜像目录**：任何子目录已有同名 add-on / 同名代理 DLL，就自动也装一份。

### 6.4 收尾：全链路验证通过（2026-11-09 09:01）

`_storage_\game_capture.log`（39 KB）与 bridge 日志：

```
config: ... SkipMv=0 DepthConvert=1 ... LoaderHook=0 Input=auto (path e:\pragmata\_storage_\game_capture.ini)
NGX provider: ...\_nvngx.dll   ->   NGX hooks active
dlss feature flags 0xB: MVJittered=0 MVLowRes=1 DepthInverted=1
input source: ngx (api 1) - pinned for this process
shared ready: name=Local\mgpu_mailbox_v1 pid=23056 used=253.6 MB reserved=253.6 MB slots=4
perf: frames=10680  begin=20ms  barrier=2.4ms (41k calls / 241k items)  present=1.4ms
mgpu_ui: start: pid=24128 cmd="bridge.exe" --hwnd 0x340B74 ... --interp 5 --interp-cadence ...
```

`perf` 那行同时给"加载慢"结了案：我们的回调合计 **0.17 ms/帧**（barrier 0.02 ms/帧、
约 400 次/帧），所以慢确实来自 `LoadLibrary` 钩子本身，回调里没有活。

## 7. 同一个坑可能影响谁

任何"**把 NGX 运行时放进游戏目录并直接调用**"的题目，例如 RE Engine 全家
（`re2/re3/re4/re7/re8`、`devilmaycry5`、`streetfighter6`、`pragmata`）以及部分
Streamline 集成的 UE 项目。修复后它们是"能钩上"；是否**好用**（MV/深度质量）另说。
