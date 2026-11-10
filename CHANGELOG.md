# 变更记录

只记**用户能感知**的变化与对应实测依据。每条结论的原始数据在
`docs/findings/PERF_BASELINE.md` / `FINAL_CONFIG.md`。

## v0.14.0 —— 多游戏通用：读游戏自己说的 + WGC 节流解除（2026-11-10）

**这一版有两条主线。** 一是"别再逐游戏猜参数"：MV 单位/符号/深度方向/投影全部改成**读游戏自己的声明**
（猜不出来的才由在线探针兜底）。二是把一整天排查出来的根因修掉 —— **WGC 捕获被默认节流在 ~60 fps**，
这是此前"怎么调都差一点"的真正原因。

### 用户可见的变化

| 以前要试 / 别扭的地方 | 现在 |
|---|---|
| `--mv-flip` 逐游戏试 MV 符号 | 按 API 契约（上一帧−当前帧），**不用开** |
| `--fg-mv-scale` 逐游戏试单位 | 从 `MV.Scale` / XeSS 速度空间推导（`≈1`=像素、`≈渲染宽`=UV、`≈宽/2`=NDC） |
| `--fg-depth-inv` 逐游戏试深度方向 | 用游戏的 `DepthInverted`，缺失时由探针实测 |
| `--fg-fov` 猜投影 | 用**游戏自己的投影矩阵**；游戏什么都不给才回退 `--fg-fov` |
| 高帧数场景"只有 60 fps" | **WGC 节流默认关闭**（见下） |
| 插帧在高速场景反而拖累 | UI「模式」新增 **关闭 (1:1 直通)**；bridge 会主动提示该切了 |

### **最关键的一条**：WGC 捕获节流默认关闭

`GraphicsCaptureSession::MinUpdateInterval` 默认是一个合成器周期（≈16.6 ms）⇒ WGC 最多交付
~60 帧/秒，**游戏跑再快也只能拿到 60**。实测设为 0 后"操作瞬间流畅跟手了很多"，且此前所有
捕获率测量都卡在 59.6 fps 也由此解释。现在 bridge 默认 0（`--wgc-minint 16` 可恢复旧行为）。

### 稳定性

- **add-on 热卸载崩溃**（`std::mutex` 已析构后仍被回调 → 空指针）：改 `SRWLOCK` + `g_addon_alive`
  守卫。这是明末"装上 add-on 就启动崩溃"的根因；
- loader 钩子改为**只入队**、在帧内排空（不在 `LoadLibrary` 里装 MinHook 跳板）；
- `sl.interposer` 钩子收回为**仅 `FgSniff=1` 时安装**（相机默认走 XeFG frame constants）。

### 已知边界（不是 bug）

- **非整数倍率 + 固定刷新率无解**（如 53 fps 在 120 Hz 上 = 2.26 帧/输入）：只能靠面板 VRR
  或把输入锁到刷新率的整数分之一；
- 游戏自身出现 100 ms 级的洞时，任何插帧都补不回来（内容缺失）；
- 个别游戏的 FSR 运行时会**在销毁时 double-free**（影之刃零实测），这类标题改用 DLSS / XeSS。

### 升级提示

- `bridge.exe` / `game_capture.addon64` / `mgpu_ui.exe` **必须同版**（协议 v12，版本不符会明确提示）；
- `game_capture.ini` 新增 `StreamlineCamera`（默认 1）；
- 旧配方里的 `--mv-flip` / `--fg-mv-scale` / `--fg-depth-inv` **留着也无害**，但已不再需要。

下面是本次的详细记录（按时间顺序，含实测数据）。

### add-on：只读抓游戏自己的帧生成（`[capture] FgSniff=1`，2026-11-10）

**一句话**：新增一个**只读诊断**开关，把游戏交给它自己**帧生成**的那套参数打进 add-on 日志 ——
**投影矩阵 / `clipToPrevClip` / MV 尺度 / jitter / 深度方向 / MV 与深度的真实分辨率**。
只改 add-on，不改 bridge、不动共享内存协议、不发布任何数据。

- 三条路径按游戏实际用哪个自动分流：Streamline（`sl.interposer` 的 `slSetConstants` /
  `slSetTag*` / `slEvaluateFeature` / `slDLSSGGetState`）、原生 DLSS-G（`nvngx_dlssg`
  的 `DLSSG.*` 参数）、集成式 XeSS-FG（`xefgSwapChainTagFrameConstants` /
  `TagFrameResource` / `InitFrom*`）。
- **纯观察者**：每个钩子把原调用原样转发，参数一个字节都不改；`poll()` 恒返回 `false`
  ⇒ 永远不会被 `Input=auto` 钉住，也不会往共享块里写东西。
- **默认关**（`FgSniff=0`），需要同时 `Log=1`。日志里每个挂点都会打"装了几个 / 有几个没导出"，
  第一次调用一定打印 `#1`；只有"hooks active"却没有 `#1`，才说明这条路径这个游戏没走。
- 结构体按 Streamline 2.x 镜像（编译期 `sizeof` 断言 + 运行期 GUID 校验），对不上就打原始
  字节、不解释。`nvngx_dlssg` 的导出名是带前缀的 `DLSSG_NVSDK_NGX_D3D12_*`，两种都试。
- 用途：UE5 那条路拿不到相机（`--fg-fov` 只能猜），而这里游戏自己给的就是真值；顺带能一次
  定死"UE5 报 `MV.Scale=1.0` 到底是什么单位"（`docs/findings/FG_SNIFF.md` §4）。

`build_addon.sh` 通过 `unity.cpp` 自动带上新文件，构建 0 warning / 0 error。

### add-on：有效区裁切 + 在线 MV 单位探针（2026-11-10）

**一句话**：两个只读改动，把"每款游戏都要人猜 MV 单位/深度尺寸"变成**运行时量出来**。

1. **`crop:` 有效区裁切**。有的题目把深度/MV 纹理开得比渲染分辨率大（PBZ：深度
   **2564×1444**，渲染 **2562×1441**，SL 也只声明 2562×1441 这个子矩形）。原来的
   `ds_run` 按整张纹理归一化采样 ⇒ 场被拉伸 0.08%，越靠边越偏 1–2 像素。现在 `uvMax`
   塞进原本空着的两个 32-bit 常量槽（根签名/PSO 都不动），只在小幅超出（≤16 px）时才裁，
   超出更多就保持旧行为并打一行警告。**代价 0**。日志：`crop: depth tex 2564x1444 -> valid 2562x1441 (uvMax=…)`
2. **`probe:` 在线 MV 单位探针**。每 120 帧把 MV 流左上 64×64 拷到一个 READBACK 缓冲
   （拷贝正好在 `ds_run` 里 MV 已经是 `COPY_SOURCE` 的那一刻），用独立 fence 在 present
   里 Signal、下一帧才 Map ⇒ **完全不阻塞**，16 KB 读回。CPU 统计有效/invalid/large 比例
   与平均幅值，除以 `mvScale` 还原成游戏原样后判定 `unit=pixels|UV|ambiguous`。
   **符号**还没做（需要 prev-depth ping-pong 做 ±mv 残差），日志里明确写 `sign=unknown`
   而不是猜。

依据与后续路线（A-v2 符号 / 质量分 / profile 缓存 / 协议 v11 真投影）：
`docs/findings/INPUT_SEMANTICS.md`。

### add-on：输入语义归一化 + 三个源的声明映射（2026-11-10）

**一句话**：把"每个游戏都要人猜"的那部分，从散装 flag 变成我们自己的一套**归一化字段**，
三个源各自把 API 的声明翻进来，翻不出来的留 `unknown` 交给探针。

- `inputs.h`：`FrameInputs` 加 `MvUnit{mv pixels/uv/ndc}`、`MvSign{unknown/prev-cur/cur-prev}`、
  `CameraSource{none/declared/matrix}`、`invalid_marker`、`valid_w/h`（**不进共享协议**，
  本轮只用于日志 + 交叉验证）。
- 映射（纯查表）：
  - **NGX**：`MV.Scale > 1.5` ⇒ `uv`（"乘 2560 得像素"就是声明），否则 `unknown`（PBZ 报 1.0）；
    相机 `none`（NGX 不导出相机）。
  - **XeSS**：`USE_NDC_VELOCITY ? ndc : uv` —— 三家唯一明说速度空间的 API。
  - **FSR 2.2/3.x**：`uv` + `camera=declared`（它导出真实 near/far/vfov）。
  - 全部：`mv_sign=unknown` —— 没有任何 API 声明方向，等 ±mv 残差那一版。
- 日志：`semantics: mv=uv sign=unknown camera=declared depth=reversed valid=2562x1441`
  （每变化一次一行），以及 `probe:` 里新增 `declared=…` 与
  `(agrees|DISAGREES with the declaration)` —— 声明与实测第一次有了对照。

### add-on：A-v2 符号测量（±mv 深度残差，2026-11-10）

**一句话**：`mv_sign` 从 `unknown` 变成**量出来**。探针每轮连拍两帧（gen0 = 上一帧、gen1 = 当前帧），
MV 与深度在**同样 4 个位置**各取 128×128（4 块覆盖上中/下中/左中/右中），CPU 上把上一帧深度沿
`+mv` / `−mv` 重采样，比较与当前帧深度的相对残差：

- `+mv` 对齐更好 ⇒ `prev-cur`（XeFG/FSR 要的那种）；
- `−mv` 更好 ⇒ `cur-prev`（游戏向量反了，等于自动判出 `--mv-flip` 的必要性）；
- 残差比值 ≥0.85 ⇒ `unknown`（画面太静，不能下结论），并把 `quality=1−ratio` 打出来 —— 这是
  A-v3 质量分要用的那个数。

日志：`probe: sign used=… plus=… minus=… ratio=… -> prev-cur (+mv aligns)  quality=0.59`

代价：每 2 s 多 8 次 64 KB 的 `CopyTextureRegion` + 一次 Map（同一个 fence，不阻塞），
`kProbeDim` 从 64 提到 128 给快速平移留出余量。
**前提**：要有深度（PBZ 的 XeSS 段没有 ⇒ 不会有这行）、画面在动、两帧都被拷到。

### 实验结论：k×vblank 批次预算更差，倍率黏性也不做（2026-11-10 07:55）

对动态逐帧做了最后两个实验，结论是**保持现状**，把非整数倍率的残留起伏交给面板 VRR。

**① `--cad-batch-rt`（批次预算 = k×vblank + 用游戏端 present 间隔当输入节奏）：实测更差。**

```
out: presents=128 vblanks=128   ✓
out: presents=140 vblanks=154   ← 14 个空刷新周期
out: presents=135 vblanks=156   ← 21 个（14%）
```

原因：输入 **29.15 ms = 3.50 个 vblank**（最坏的分数比例，正好一半）。k=3 时预算 25.0 ms 比实际间隔
短 4.15 ms ⇒ 空洞；k=4 时多 4.15 ms ⇒ 挤压。"每帧恒 8.33 ms"和"不留空洞"在**固定刷新屏 + 半 vblank
比例**下不能同时成立。用间隔喂（现状）虽然间距略有起伏，但预算与间隔一致、**没有空洞**，是更好的取舍。
⇒ 开关保留但默认关闭，不设成默认。

**② `--cadence-hold`（倍率黏性）：不做。** 数据显示倍率每窗口切换 25~30 次（约 40 个真实帧），
确实偏多；用户决定不再调，交给面板 VRR 吸收。

**最终形态**：动态逐帧 = 均值定基调 + 相位累加（Bresenham）+ 突变/掉帧补偿 + 有效区约束；
非整数倍率的残留交给 **VRR（显示器侧开启即可，不需要 `--vrr`）**，或锁帧到刷新率的整数分之一。


### 详细：WGC 节流（MinUpdateInterval）实测与取舍

`GraphicsCaptureSession` 的 `MinUpdateInterval` 默认是一个合成器周期（≈16.6 ms），**WGC 因此
最多每 16.6 ms 交付一帧 ⇒ 捕获被钉在 ~60 fps**，游戏跑得再快也只能拿到 60。

**实测**：设为 0 之后（PBZ）"操作瞬间流畅跟手了很多"；随后对比 `--no-fg` 与 2x 两种配方，
用户确认"体验明显变好"。回头看，这一天里所有捕获率测量都卡在 59.6 fps、`--no-fg` 直通也顿、
各种 cadence 调整都"治不好"——根因就在这里：**我们一直在用被削到 60 fps 的输入做分析**。

改动：

- **bridge 默认 `--wgc-minint 0`（不节流）**；`--wgc-minint 16` 恢复旧的 ~60 fps 上限（仅命令行）；
- **UI 不提供该选项**（默认即正确值）；
- **UI「模式」新增「关闭 (1:1 直通)」**（`--no-fg`）：节流解除后输入常到 70~110 fps，
  此时 XeFG 最少 2x 只会让面板显示不了的帧变成重复帧 —— 直通最跟手；
- bridge 检测到"输入 ≥ 面板一半"时会**提示一次**并建议改用直通模式；
- 启动日志固定打印 `[bridge] WGC MinUpdateInterval = 0 ms (accepted)`。


**UI 里的 VRR 开关已撤销。** 一整天的实测结论是：自研路径（填满 vblank 的动态逐帧）整体优于
VRR 均匀间隔 —— VRR 没有解决卡顿，因为瓶颈不在 pacing，而在输入帧率自身的抖动
（见下一节的直通对照）。`--vrr` 参数保留在 `bridge.exe` 上仅供实验，不再出现在 UI 里；
同一天从 UI 撤下的还有"抗丢帧 (A/B)"（对插帧是负优化）。


用 `--no-fg`（1:1 直通）做对照，日志给出了明确答案：

| 指标 | 实测 | 预算 |
|---|---|---|
| `age`（游戏出图 → 我们拿到） | **2.2 ms** | — |
| `present`（等 vblank） | **1~2 ms**（max 16） | 8.3 ms |
| `total`（每帧总耗时） | **2.5 ms** | 8.3 ms |

**我们的捕获/上屏链路是干净的**；同一次会话里游戏自己的节奏是
`avg p50 = 52.7 fps / recent p50 = 55.7 / 标准差 8.3`（还出现过 183 ms 的洞）。

结论（数学必然，不是实现缺陷）：

- 53 fps = 每帧 18.8 ms = **2.26 个刷新周期**；
- 固定 120 Hz 屏上，不插帧 → 每帧占 2/3 周期交替 = 顿；
- 插帧填满 → 每输入要 2.26 帧，只能 2/3 交替 ⇒ **运动速度在 ±33% 之间跳**，仍是顿；
- **非整数倍率 + 固定刷新率无解**。DLSS-G 的对策同样是"锁基础帧率"或"VRR"，它没有算法上的解。

因此本项目的正确用法：

| 场景 | 用法 |
|---|---|
| 面板支持 VRR | 开 UI 的 **VRR 均匀间隔**：k 固定、间隔恒定（例 53 fps → 2 帧/输入 → 106 Hz，每帧 9.4 ms） |
| 固定刷新率 | **RTSS 锁到刷新率的整数分之一**（120 Hz → 60 或 40），倍率才是整数 2x/3x |
| 输入本身在抖/有大洞 | 无解：插帧不能补出不存在的时间，只能降画质让输入先稳住 |

后续不再在这条线上继续调 cadence —— 数据表明路径耗时 2.5 ms、`presents ≈ expected`，
缺口全部来自源端节奏。


**症状**：动态逐帧开了以后"看着不流畅、偶尔像少了几帧跳过去"，锁不锁帧都一样。
日志给出了**可量化**的原因，一共三层：

1. **两个估计器互相矛盾**：累加器用"最后一帧 + 8 帧 EMA"，日志却打 16 样本中位数，
   于是出现 `input=25.0ms = 3.00 vblanks -> avg 5.00x, 200.0 fps`（3 个 vblank 发 5 帧）。
2. **倍率没有"经过多少 vblank"的上限**，还会被单帧尖峰分支（`rt=41.7ms = 5 vblanks`）
   喂一个和批次不匹配的时长 ⇒ 3 帧铺 5 个周期 ⇒ 一半周期两帧、一半空着。
3. 修完上面两条后剩下 **12% 的空刷新周期**（`out: presents=120 vblanks=135`），
   根因是**右偏分布下中位数低估速率**：中位数永远 25.0ms，而真实均值 28.1ms（3.375 vblanks）。

**改动**：

- 累加器与日志统一用**同一个估计器**；倍率硬上限 = `ceil(经过的 vblank)`；相位夹到 ±0.5
  （Bresenham），倍率只能一档一档变；去掉叠加 EMA（中位数外面再套 EMA 会多 0.5~0.8 s 滞后）。
- cadence 模式下 `frameRenderTime` 直接喂"决定批次的同一个间隔"，并**停用单帧尖峰分支**
  （波动已被累加器吸收，那个分支在持续抖动下会每帧触发）。
- `t_in` 从中位数改为**均值**（16 样本环，单样本上限 3× 中位数）。
  → `out: presents` 与 `vblanks` 从此对得上（122/120、135/134、141/140、129/129）。
- 新增 `--vrr` + UI 开关 **VRR 均匀间隔**：面板跟随（G-Sync/FreeSync）时不再追求填满每个
  vblank，而是固定 k 帧、间隔均匀（例：28.6 ms × 3 帧 = 105 Hz，每帧恒 9.5 ms），
  消掉"周期满但速度抖"的 3/4 交替。开了它就不再发 `--fg-out-hz`（两者互斥）。
- 修掉一个 UI bug：`运动矢量` 选项的读取范围检查写死 `>2 归零`，导致选了"AMD 光流"下次启动
  变成"关闭"。
- 新增两个默认规则：`*-Win64-Shipping.exe`（UE）**自动改用光流 MV**；MV 不可用（协议第 19 位）
  时自动回退光流（`--no-of-fallback` 可关）。


**症状**：明末（Wuchang: Fallen Feathers）装上 add-on 后**启动即崩**，UE 报
`EXCEPTION_ACCESS_VIOLATION reading address 0x0`。UE 自己的 minidump 栈：

```
msvcp140!mtx_do_lock+0x74
game_capture+0x2964      ← 我们
game_capture+0xab2c
dxgi!… → amd_fidelityfx_dx12!ffxQuery → Project_Plague（**RHI 渲染线程**）
```

**根因**：ReShade 会**反复加载/卸载** add-on（PBZ 一次启动装 5 次可证）。`std::mutex` 有析构函数，
析构后内部指针变成 `nullptr`，下一次 `lock()` 就是读地址 0。而我们的 `LoadLibraryW` 钩子在
`AddonUninit` 之后仍可能被游戏触发 → 走进 `drain_pending_modules → on_module → lock(g_hook_mutex)`。

**修法**：

1. `g_hook_mutex` / `g_log_mutex`：`std::mutex` → **`SRWLOCK`**（`SRWLOCK_INIT`，POD、无析构）；
2. 新增 `g_addon_alive`：`AddonInit` 末尾置位、`AddonUninit` **第一行**清零，
   `notify_sources()` / `drain_pending_modules()` 先检查 —— 迟到的回调直接返回，不再碰
   `g_sources`（同样是带析构的 `std::vector`）。

顺带（同一轮，为定位这条崩溃）：loader 钩子改为**只入队**、在 `on_begin_effects` 排空
（不在 `LoadLibrary` 里装 MinHook 跳板）；初始化时的模块扫描延后到第一帧；
`sl.interposer` 的钩子收回为**仅 `FgSniff=1` 时安装**（相机默认走 XeFG frame constants）。

**验证**：明末从"启动即崩"变为**正常进入并用自己的 MV 出帧**。


最后一个"发出去没人用"的协议位。生产端在 `semantics` 里标记自己的 MV 不可用时，消费端**这一帧**
改用 bridge 自己的光流，而不是把垃圾矢量喂给 XeFG。

- 触发条件严格：`kSemQualityKnown` **且** `kSemMvUsable` 为 0 —— 即探针真的测过并判定不可用。
  没有判定（未测/未声明）时行为完全不变。
- **不丢生产端 MV**：与 `--fg-of` 不同，这条回退不会取消 `kRequestMv`
  （否则就再也切不回来了）。判定恢复可用即当帧切回。
- 每帧状态机：`g_of_active`（本帧用谁）+ `g_of_fresh`（本帧光流是否真的跑了）。
  打标签和 `motionVectorScale` 都要求 `fresh`，避免把上一帧的旧光流场当成本帧的。
- `--no-of-fallback` 关掉这条回退（等价于旧行为）。
- 日志（每次切换只打一次）：
  `[bridge] producer marked its motion vectors UNUSABLE (quality=38/255) - falling back to our optical flow until it says otherwise`

**现状说明**：因为我们改成了"声明优先"，符号测试在单位已声明时会被跳过 ⇒ `kSemQualityKnown`
只在"单位未声明、探针真的跑了"的源上出现。所以在 PBZ/DLSS 这类声明齐全的标题上，这条回退
**不会触发**（也就不会有副作用）；它服务于 XeSS-无深度/无声明那类真正需要兜底的场景。


**结果先讲**：投影矩阵通了。

```
camera: state enabled=1 src_camera=0 read=1 tick=26783937 age=31ms shape=1 raw_tick=26783937 seq=4 -> USED
camera: using the game's own projection matrix (Streamline constants / XeFG frame constants)
  rows [+0.9742 0 0 0] [0 +1.7321 0 0] [0 0 0 -1.0000] [0 0 10.0000 0]
  fovY=60.00deg aspect=1.778 - --fg-fov is no longer used for this title
[bridge] projection: from the game's own view-to-clip (v12): fovY=60.00deg aspect=1.778
```

**根因（本轮最重要的一条架构事实）**：`raw_tick=0 seq=0` 而写者明明执行了 —— 说明读写不在同一份内存。
日志里早就写着 `init_swapchain resize=0/1`，PBZ 启动时重建 swapchain，**ReShade 会重载 add-on**：

- 旧实例的 XeFG 钩子仍挂在 `libxess_fg.dll` 上 → 它调用 `publish_fg_camera`，写**旧实例的**全局；
- 新实例接管 present 路径 → 读**新实例的**全局（永远是 0）。

旁证：同一次启动里 `fg sniffer: ON` 打了 **5 次**（04:26:05–04:26:10），就是 5 次装载。

**修法**：交接改用**命名共享内存** `Local\mgpu_fg_camera_v1`（`CreateFileMappingA` 匿名页），
同会话内所有实例共享同一段字节；`seq` 用 `InterlockedIncrement64` 包住。映射失败时回落到进程内静态槽。

**这条事实的影响面不止相机**：
1. 任何"必须跨实例"的状态都不能用 `static`（我们的 `g_sem`、探针状态在重载后会重置 —— 目前无害，
   因为新实例会重新测/重新发布，但要知道它会重置）；
2. 钩子是**旧实例装的**，`MH_CreateHook` 的"已装过就跳过"守卫是**每实例一份**，
   重载后新实例会试图再钩一次（MinHook 返回 ALREADY_CREATED，我们当"已装"处理）；
3. 排查时看到"日志出现两次/五次"先想重载，别怀疑逻辑。

顺带修掉一处日志显示错误：XeFG 不传 near/far/fov（都是 0），那行曾打成 `fovY=0.00deg`，
现在从矩阵反算显示（bridge 一直是对的）。

**关于 60.00°**：这正是"用游戏自己给的"的结果 —— 它和早先 SL constants 推出的 46.7° 不一致，
按你的决定仍以游戏的为准（游戏自己的 FG 就用这个值，我们与它保持一致）。想改回测量值：
`StreamlineCamera=0` + `--fg-fov 46.7`。


用户拍板："直接默认采用游戏自身支持的方案。" 落到实现上就是两条：

1. **不再用 `--fg-fov` 质疑游戏的矩阵**。之前我在文档里提过"若 FOV 与命令行差太多就保留命令行值" ——
   这个想法被否决了。理由站得住：那份矩阵是游戏**给自己 FG 用的**，我们猜的 60° 没有资格覆盖它。
   现在的规则是"有就用，且只用它"。
2. **差异要可见，不要静默**：bridge 在两者相差 >5° 时打一行 note（只打一次），
   明确写"keeping the game's"。这样遇到"占位 60°"这类集成方，日志能一眼看出来，
   想改回命令行猜测只需 `StreamlineCamera=0`。

配套：`camera:` 相关的一次性诊断（`not used (<原因>)`、`camera source = …`）也进了这一版，
下轮跑完就能定位"矩阵为什么没被采用" —— 这是当前唯一未知项。


**实跑发现**：`03:59` 那次会话里 **没有任何 `sl.const` 行** —— PBZ 这次的 FG 是游戏自己经
**XeSS-FG** 出的（`fg: xefg.const #1 ... jit=(-0.438,+0.389)px mvs=(1.000,1.000)`），
根本没走 Streamline。于是 v12 的矩阵永远是空的，`--fg-fov` 还在顶着。

**但这正是好消息**：XeFG 的 `xefg_swapchain_frame_constant_data_t` 里**本来就有**

```c
float viewMatrix[16];        // row-major
float projectionMatrix[16];  // row-major, jitter 走单独的 jitterOffsetX/Y
```

而我们早就钩着 `xefgSwapChainTagFrameConstants`（只用来打日志）。现在同一处顺手把它交给
`publish_fg_camera()`，于是 v12 有**两条独立来源**：

| 来源 | 何时有 | 备注 |
|---|---|---|
| Streamline `sl::Constants.cameraViewToClip` | 游戏用 SL 出 FG（DLSS-G） | 官档注明无 jitter |
| **XeFG `frameConstantData.projectionMatrix`** | 游戏自己经 XeSS-FG 出 FG（unlocker） | jitter 单独传，投影本身是干净的 |

**仍然存在的空档**（如实记录）：如果游戏既不用 SL 也不开任何 FG（纯 DLSS-SR 超分），
我们**拿不到任何相机**，`--fg-fov` 继续生效。这是数据不存在，不是我们没读。

同一轮还确认了两件事（都在 v11/v12 的改动范围内）：
`probe: mv ... declared=pixels (agrees with the declaration)`（in_xess/in_ngx 的声明修正生效）、
`[bridge] producer semantics: mv=pixels(declared) sign=prev-cur(declared) depth=reversed(measured)`
（声明已能发布到消费端）。


**要解决的问题**：DLSS-SR 不导出相机，所以所有 NGX 标题的投影一直是 `--fg-fov` 猜出来的
（默认 60°），而 PBZ 实测垂直 FOV 是 **46.7°** —— 差 13°，直接体现在插帧的对齐上。
这是"探针/表"都解决不了的那一类：数据就在游戏手里，只是没人读。

**做法**（协议 v12，追加在 `FrameShared` 末尾，v11 之前的偏移不变）：

- `in_fg.cpp`（诊断用 Streamline 嗅探器）在 `slSetConstants` 里把 `sl::Constants.camera_view_to_clip`
  交给主机。官档说明该矩阵**不含 TAA jitter**，正是消费端需要的形态。
  转发发生在**任何日志门之前**（`want_line()` 会早退，而日志默认还是关的）。
- `inputs.h` / `game_capture.cpp`：新增 `FrameInputs.view_to_clip[16]`、`kInputFlagHasMatrix`，
  以及一对无锁交接函数 `publish_fg_camera()` / `fg_camera()`（序号计数器防撕裂）。
- 生产端**只在源自己没有相机时**才填（`kInputFlagHasCamera` 优先，FSR 3.1 那条路不受影响），
  并且**不往 `g_inputs.flags` 里塞持久位** —— 否则嗅探器安静下来之后矩阵会僵住。
  改成每帧判断 + 发布**采集时刻** `matrix_tick_ms`，消费端超 500 ms 就当没有。
- 合法性检查复用了 OptiScaler 的形状测试（w 是 ±z、无 w 平移、xy 不混），
  一个"被转过的相机矩阵"或垃圾缓冲都过不了。
- `bridge.cpp`：有新鲜矩阵时**从它反算 FOV/aspect**（`fovY = 2·atan(1/m[5])`、`aspect = m[5]/m[0]`），
  再走既有的 `approx_projection[_rz]`；near/far 仍取源声明。**不是**把矩阵原样交给 XeFG ——
  核实 OptiScaler 后改的主意：他们拿到同一个矩阵也只用来反算，且注明"UE 传的 `cameraViewToClip`
  不可信"（`inputs/FG/Streamline_Inputs.cpp:207`）。原样直喂留 `--fg-use-matrix` 做 A/B。
  `--fg-no-proj` 仍然最高优先。
- `[capture] StreamlineCamera=0` 可以关掉这个交接（嗅探器本身继续为日志服务）。
- **钩子门控也改了**：以前 Streamline 钩子只在 `FgSniff=1` 时装；现在"只为相机"也要装
  （`StreamlineCamera=1` 且 `FgSniff=0` 时只装 `sl.interposer` 与 `libxess_fg` 两族，
  DLSS-G 钩子仍然只服务日志）。否则默认配置下这个特性永远不会生效 —— 这是落地时差点漏掉的一环。

验证手段（下次进游戏看两行）：
```
camera: using the game's own view-to-clip matrix (Streamline constants)
  rows [...] zn=0.1000 zf=75000.0 fovY=46.70deg - --fg-fov is no longer used for this title
[bridge] projection: using the game's own view-to-clip (v12), not --fg-fov
```



**起因**：±mv 探针的判据越修越苛刻（残差求和 → 胜率 → z 分数 + 残差门 → 256 patch + 梯度门），
而它本来就不该是主判据。查同仓的 OptiScaler 源码（`temp/optiscaler_src`，同为 GPL-3.0）得到三个实证：

1. **他们的游戏表里没有我们要的东西**：`OptiScaler/misc/GameConfigs.cpp`（106 条）只调
   `DxgiSpoofing` / `FGAllowedFrameAhead` / `ColorResourceBarrier` / `RestoreComputeSignature`
   这类"管道"开关，**零条**涉及 MV 单位、MV 符号或深度方向。
2. **单位他们根本不判**：`NVSDK_NGX_Parameter_MV_Scale_X/Y` 直接抄进 FSR/XeSS 的
   `motionVectorScale`（`upscalers/fsr2_212/FSR2Feature_Dx12_212.cpp:255-266`），
   深度方向用 init flag（`DepthInverted()`）。即"信声明、不测量"。
3. **按 exe 的表对 PBZ 会是错的**：同一个 exe，FSR 会话声明 uv（`MV.Scale≈2880`，实测 uv），
   DLSS 会话声明 pixels（`MV.Scale=1.0`，实测 pixels）—— 表无法表达"随 API/档位变"。

**所以复用他们的表解决不了问题，问题是我们的读法错了。** 关键一行是
`pixels = stored × MV.Scale`（DLSS 就是这么消费的）：于是 `MV.Scale=1.0` 不是"不可知"，
而是"已经是像素"。两段 PBZ 会话 2/2 与实测一致。

**改动**：

- `in_ngx.cpp`：由声明推导单位——`scale≈1` → pixels、`≈渲染宽` → uv、`≈宽/2` → ndc；
  符号按 API 契约直接写 `MvSign::prev_minus_cur`（所有超分 API 都定义成"上一帧−当前帧"，
  游戏若反过来会弄坏它自己的 DLSS；`MvFlip=1` 保留给少数说谎的标题）。
- `probe:` 单位**不再覆盖声明**，只在声明缺失时发布；单位已声明时 **±mv 符号测试直接跳过**，
  日志一行：`probe: sign skipped (mv unit declared=pixels, sign is the API contract)`。
- `[capture] MvNormalize=auto`：把声明得到的单位变成流里的换算（pixels→÷流宽、ndc→×0.5）。
  **默认仍是 0（原样透传）** —— 它改变的是 bridge 收到的数据，必须是显式动作，不能静默变。

净效果：PBZ 这类"声明齐全"的标题，脆弱路径**完全不参与决策**；探针只在 API 什么都不说时兜底。

### 顺带修掉两个从 OptiScaler 源码里发现的缺口

对着他们的 `inputs/FG/XeFG_Inputs_Dx12.cpp:514` 逐行核对，确认 XeFG 的口径是
`motionVectorScale = 每个 velocity 单位等于多少像素`，且 **NDC 时 Y 要取负**
（`mvScaleY *= size.y * -0.5f`）。于是：

- **发布的 `mv_scale` 现在按流里的实际单位折算**。之前它只是转发游戏声明；一旦开了
  `MvNormalize`（流改成 UV），XeFG 拿到的速度会差 ~2560 倍 —— 画质静默劣化，最难查的那种。
- **`mv_unit == ndc` 时 `mv_scale_y` 取负**（原来只乘 2）。bridge 侧的
  `XEFG_SWAPCHAIN_INIT_FLAG_USE_NDC_VELOCITY` 还没接，已列进
  `docs/findings/OPTISCALER_QUIRKS.md` §5 的第一条。

对照分析全文（105 条表怎么分类、哪 8 条能搬、我们缺什么）见 `docs/findings/OPTISCALER_QUIRKS.md`。

### XeSS 实跑抓到的四个问题（2026-11-10 03:49，已部署）

`03:45` 那次 XeSS 会话暴露了几件只有真跑才会出现的事：

```
xess: first execute render=2562x1441 output=3840x2160 depth=0000000000000000 mv=… reset=1
       (HIGH_RES_MV: no depth, publishing MV only)
semantics: mv=uv sign=unknown camera=none depth=forward valid=2562x1441      ← 两个字段都是错的
crop: mv tex 3840x2160 vs reported 2562x1441 - excess too large, sampling the whole texture
probe: mv mean|v|=(44.848,3.113) -> unit=pixels  declared=uv  (DISAGREES with the declaration)
mvscale=(1.000000, 1.000000)                                                  ← 同一条日志里
```

1. **`in_xess` 把单位判错了**：`USE_NDC_VELOCITY` 没置位时它声明 `uv`，但 XeSS 的默认速度空间是
   **像素**。证据就在同一份日志里：`mean|v|=44.8` + `mvscale=(1,1)` —— 44 不可能是 UV。
   已改成 `ndc : pixels`。这是"声明优先"之后的第一个真实收益：探针当场指出了声明的错，改一行即可。
2. **`sign=unknown` 是自找的**：`in_xess`/`in_ffx` 还写着 `unknown`，只有 `in_ngx` 声明了契约符号。
   三个源现在统一声明 `prev_minus_cur`，并且**声明也会发布到 v11 的 semantics 字里**
   （以前只有探针能写那两位，所以"声明齐全"的源在 bridge 侧反而显示 unknown）。
3. **`depth=forward` 是假话**：XeSS 这条路径**根本没有深度纹理**，日志却按"没有 InvertedDepth 标志
   = forward"打印。现在打印 `depth=n/a (no depth)`。
4. **`crop:` 的措辞误导**：MV 3840×2160 vs 渲染 2562×1441 时我们**并不是放弃**，而是 `uvMax=1.0`
   把整张贴图重采样到渲染网格 —— 这正是 OptiScaler `ForceFGRenderSizeMVs` 想要的"FG 要渲染尺寸的 MV"。
   所以 `OPTISCALER_QUIRKS.md` §3.2 里"待做 MvResample"这一项**其实早就实现了**，只是日志写着
   "excess too large"，看起来像失败。已改为 `resampling the whole texture`，并把该行动项标记为"无需改动"。


| # | 改动 | 位置 | 为什么 |
|---|---|---|---|
| 1 | `mv_unit == ndc` → 置 `kInputFlagMvNdc`；bridge 据此设 `XEFG_SWAPCHAIN_INIT_FLAG_USE_NDC_VELOCITY` | `game_capture.cpp`、`bridge.cpp` | 不声明的话 XeFG 把 [-1,1] 的 NDC 速度当 UV 读（XeSS 的 `USE_NDC_VELOCITY` 正是这种源） |
| 4 | `offload_same_device()`：队列/资源不同 device 时回落到游戏队列并记日志 | `game_capture.cpp` | 双 GPU 进程里混 device 是设备移除，崩在离拷贝很远的地方（OptiScaler 的 `CreateSLOnThe2ndDevice` 同类坑） |
| 5 | `sl.tags` 日志打出 lifecycle 名字，`eValidUntilEvaluate` 的 depth/MV 标注 `[staged for Evaluate - not read as input]` | `in_fg.cpp` | `lc=2` 这种数字看不出"SL 还可能改写它"的意思 |

构建：add-on 0 警告、bridge 0 警告，两者都已覆盖到 `F:\...\PBZ\platform\steam\`。


**1. 三次崩溃的归因：不是我们。** 用 `cdb` 分析 `PBZ.exe.25748.dmp`（切档位那次）：

```
FAILURE_BUCKET_ID: HEAP_CORRUPTION_ACTIONABLE_BlockNotBusy_DOUBLE_FREE_c0000374_dxgi.dll!Unknown
  … ntdll!RtlFreeHeapFast ← dxgi ← D3D12Core!CDestructionNotifier::Destroy
  ← D3D12Core!CLayeredObject<CBackingCommandAllocator>::Release ← dxgi
  ← amd_fidelityfx_upscaler_dx12+0x279a ← …_loader_dx12 ← PBZ
```

游戏自带的 FSR 运行时在**销毁路径**里把同一块 D3D12 command allocator 释放了两次；
栈里没有 `game_capture` / `bridge` / `ReShade`。PBZ 里**改用 DLSS / XeSS 后即可正常进入游戏**。
细节与证据：`docs/findings/PBZ_NOTES.md`。

**2. `probe: sign` 的第二次修正：多尺度搜索。** 上一版把流值当像素偏移
（`× kProbeDim / render`），对 UV 源方向是反的 ⇒ 位移≈0 ⇒ `plus`/`minus` 几乎相同。
现在对三种候选尺度（pixels / uv / ndc）各算一遍 ±mv 的深度残差，取**对齐最好**的那个：

```
probe: sign scale=uv used=… static=… plus=… minus=… ratio=0.43 -> prev-cur (+mv aligns)  quality=0.57
```

胜出的尺度同时就是**单位的第二份独立证据**（`kSemMvUnit*` 会被它覆盖），
`ratio` 决定符号与质量分。日志新增 `scale=` 字段。

**3. 模块表饱和的修复（03:01 已部署）**：`in_fg.cpp` 的模块登记表 4→8 槽 + 新增"已忽略"表，
并给每个入口加了"已装过就跳过"的守卫 —— 复现证据是同一会话里 `fg sniffer:` 从 48 次降到 9 次。

**4. 符号判定改成"配对胜率"（A-v3，03:19 已部署）**：残差求和会被少数去遮挡/远平面点主导 ——
PBZ 实测 `plus=431.7979 minus=539.9221`（22,206 个采样）其实是 **20%** 的差，但除以样本数只剩
1.9%，于是被 0.85 的阈值判成 `ambiguous (need motion)`，白扔了一轮数据。
现在逐纹素投票："±mv 哪边把上一帧深度对齐得更好"，`quality = 2×|胜率−0.5|`：

```
probe: sign scale=pixels used=22206 static=3150 wins=68.6% mean|d| plus=0.0195 minus=0.0243 -> prev-cur (+mv aligns)  quality=0.37
```

配套的三条纪律：小于 1/4 纹素的运动（两侧落在同一批纹素里，只有插值噪声）单独计入 `static=`；
两个极点（反向 Z 天空=0 / 正向 Z 天空=1）都不参与投票；`margin > 3%` 才下结论（22k 票上已不可能是噪声）。

**5. 胜率版第一次实跑就暴露了选尺度的缺陷（03:23 修复，待部署）**：03:21 那行是

```
probe: sign scale=uv used=446 static=1 wins=40.4% mean|d| plus=0.00323 minus=0.00322 -> cur-prev  quality=0.19
```

`plus` 与 `minus` 只差 **0.3%**（等于没信号），却因为小样本（446 票）的胜率更极端，**把 22,206 票的
`pixels` 挤掉了** —— 排序按 `|胜率−0.5|` 没考虑样本量。修法两条：

- 排序改用**二项 z 分数** `(wins − n/2)/√(n/4)`，并要求 `used ≥ 2000`；
- 再加一道**残差差门**：胜出方向的 `mean|d|` 至少要差 2%，否则写
  `unknown (votes lean one way but both offsets fit equally well)` —— 03:21 那种 0.3% 会被直接判掉。

日志因此多了 `z=` 和 `big=`（被 16 纹素上限甩掉的 texel 数，能一眼看出哪个尺度把偏移放大了）。

**6. 03:26 又跑出两行，暴露 patch 尺寸不够（03:29 修）**：

```
probe: sign scale=ndc used=16565 static=502 big=44432 wins=65.7% z=40.3 plus=0.00137 minus=0.00137 -> unknown (votes lean one way but both offsets fit equally well)
probe: sign scale=uv  used=14497 static=842 big=46222 wins=77.3% z=65.6 plus=0.00214 minus=0.00222 -> prev-cur
```

`big=46,222 / 63,504` —— **73% 的 texel 位移超过 16 纹素上限**。这次 `probe: mv` 报的是
`mean|v|=(33~56, …)`，正常转视角的运动量就是 33~56 像素，128 的 patch + 16 上限
把**正确尺度**饿死了，投票全来自近乎静止的天空/UI（所以 `mean|d|` 只有 0.2%）。

四处修改：

- `kProbeDim` **128 → 256**，上限改为 `kProbeDim/4 = 64`（缓冲区 4 MB，每 2 s 拷 4 MB）；
- warp 采样点必须都落在 patch 内，否则计入 `out=`（以前靠 clamp，边缘 texel 是错的）；
- 新增**深度梯度门**：`slope_per_texel × |offset| < 2% × depth` 的 texel 不投票，计入 `flat=`。
  平坦墙面上 ±mv 采样结果完全相同，投票只是舍入噪声 —— 03:21 的
  `plus=0.00323 minus=0.00322` 就是这么来的；
- 新日志字段：`used= static= flat= big= out=`，一眼看出票是从哪一步漏掉的；
- 计算量反而降了：**循环步长 2**（场是空间相关的，隔一个 texel 结论一样），梯度用
  `dp_at(x±2)` 代替 4 次双线性。256² × 4 spots × 3 尺度全双线性会在每 2 s 的 present 回调里
  造成可见抖动，现在相当于 128 版的 ~1.5 倍。

好消息是**残差门当场生效**了：03:26 第一行 votes 65.7%、z=40.3，但两侧 `mean|d|` 一模一样
（`0.00137 / 0.00137`），被判成 `unknown (votes lean one way but both offsets fit equally well)` ——
换上一版这里就会错误地宣布 `cur-prev`。

**一句话**：探针量出来的东西不再只躺在 add-on 日志里，而是**随每帧一起发布**给消费端。

- `mgpu_shared.h`：`kSharedVersion` 10 → **11**；`FrameShared` 末尾追加
  `semantics`（打包好的 `kSem*` 位）+ `valid_w/h`（有效子矩形）。**只追加**，v10 及以前的偏移不变。
- add-on：`probe_report()` 把三项结论写进 `g_sem`（MV 单位 / MV 方向 / 深度方向 + 质量分
  `1−残差比` + `MV usable` 位），发布时写进每一帧。
- bridge：新增一行（**只记录、不改行为**，开关仍然优先）：
  ```
  [bridge] producer semantics: mv=pixels(measured) sign=prev-cur(measured) depth=reversed(measured)
           quality=148/255  [MV usable] valid=2562x1441
  ```
- 规矩照旧（`docs/design/PROTOCOL.md` §6）：两端必须同一次构建，版本不符硬拒绝。
  已同时部署新 `bridge.exe`（旧版备份 `bridge.exe.before_v11`）。

## v0.13.2 —— 并行化复审：所有"还能并行"的地方都试过了，结论是默认全关（2026-11-09）

**一句话**：把 fg 链路上还能并行/异步的地方全部实现并实测了一遍 —— 3 个默认关的新开关
（跨卡拷贝走 COPY 队列、MV/深度提前一帧、add-on 把跨卡写搬出游戏队列）、1 个抗丢帧模式，
外加 3 个真修复。**新开关全部默认关闭，默认行为与本版之前完全一致**；每个开关的实测数据、
为什么留/为什么不用，都在 `docs/findings/PERF_BASELINE.md` §3.28–§3.30。

### 修掉的问题（默认生效）

1. **`--fg-warp` 两条 list 共用一份 command allocator。** 这条实验路径一个输入帧提交两条 list
   （插值中点 + 真帧），而中点那次 `Present()` 只是把 list 排进队列；原代码在两次提交之间
   `Reset()` 同一个 allocator —— D3D12 里对**还在执行**的 allocator 做 Reset 是未定义行为。
   现在两对 `(allocator, list)` 每帧轮换，靠下一帧开头的 `gpu_throttle()` 保证换进来的那对已退休；
   顺带修掉 `back_buffer()` 失败时第二条 list 往**已关闭**的 list 里录的问题。
2. **`WGC sees 60 fps but the game only renders 38.4 fps` 是假警报。** 它拿我们的**中位数**
   （16.7 ms）去比生产端的**一次原始采样**（26 ms）；真实情况是游戏 ~44 fps 落在 120 Hz 面板上，
   帧间隔在 2/3/4 个 vblank 之间正常跳动，两边**中位数**都只是"最常见的那个桶"。
   这条误报曾把排查引向"游戏自带 FG"（实测游戏 FG 是关的）。现在两边都取 16 帧中位数再比，
   并把 **DWM 重复合成**列为第二种可能原因（我们的顶层覆盖窗口会让游戏窗口失去独立翻转）。
3. **UI 的两个死键接上了。** `mgpu_ui.ini` 里的 `sdr` 与 `slack1` 一直存在，但
   `ID_SDR` / `ID_SLACK` **不在任何控件表里**、`build_cmdline` 也从不发对应开关 —— 等于摆设。

### 新增 UI 控件

| 位置 | 控件 | 作用 |
|---|---|---|
| 捕获 → **色彩空间** | `HDR10（默认）` / `SDR（BT.709）` | 选 SDR 发 `--sdr`：bridge 走 BT.709 + gamma 2.2，并把交换链色彩空间声明成 SDR。我们抓的是 scRGB(FP16)，HDR 源永不丢；在 HDR 显示器上选 SDR 会发灰，那是模式本身 |
| 帧生成 → **抗丢帧 (A/B)** | 开 / 关 | 发 `--slack-auto`：当排队的抓取帧确实带着**新内容**时，多留一帧而不是丢掉（代价是那一帧 +1 个间隔延迟） |

### 新增命令行开关（默认全关）

| 开关 | 作用 | 实测结论 |
|---|---|---|
| `--input-copy-queue` | MV/深度的跨卡拷贝走独立 COPY 队列，与颜色 convert draw 重叠 | `--den 2` 测不出来（重叠窗口只有 0.2–0.5 ms）；`--den 1` 单趟 A/B 有 −1…−2 ms 但体感更差（空刷新 34 vs 13）。**不用** |
| `--prefetch-inputs`（隐含上一条） | 把 slot N+1 的 MV/深度放在帧 N 的 Present 之后拷贝 | 机制 100% 命中（`pf 40/0`），但空刷新 0.32 → **2.90/窗**、15/60 窗口空 >4 个：搬运挪出帧外 → 循环跑赢 pacer → `--interp-cadence` 少要帧。**不用** |
| `[capture] OffloadQueue=1`（add-on） | 跨卡拷贝 + 我们自己 RT 的反向状态转换搬到 add-on 自己的队列 | 功能验收通过（`offload=9987`、`stalls=0`、`fail=0`、6 分钟无异常、回退链一次没触发），**收益未量到**（对照被场景漂移吃掉）。**不用**，游戏侧 add-on 已回滚 |
| `--slack-auto` | 抗丢帧：真有新内容时多留一帧 | 缺省关。实测它的 `held` 只有 3 次而丢了 20 次 ⇒ 20 次丢弃里生产端根本没发布新帧 = **重复帧**。建议**开着**（几乎零成本），但不是必须 |

### 顺手量到的事实（以后不用重测）

- **OSD 的 `drop N` 大多不是"掉帧"**：`--slack-auto` 的判据证明那些被丢弃的抓取帧里
  生产端 `frame_published` 没有变化 —— 是 DWM 把同一帧重复合成上来的，丢掉不损失画面。
  新日志把丢弃拆成 `[K real loss / M-K duplicate]`，**只有 K 是真丢内容**。
- **手工分次跑 A/B 在本机不成立**：同一配置两趟 `total` 差 **3.2 ms**、`age` 差 6 倍（7.9 vs 48.5 ms），
  比被测效应还大。必须用 `bin\12 - automatic A-B-C-A 4K (60s each).bat` 这种**自动连续**跑法。
- **我们自己没有任何硬编码帧率上限**：`--lock` / `--fg-out-hz` / `--pace` 默认全关，UI 里那行
  "建议锁帧"只是提示（不会传给 bridge）；交换链是 `Present(0)` + BufferCount 2 + MaxFrameLatency 1
  （限的是在飞帧数，不是帧率）。日志里的 `in=25 ms` 就是 RTSS 锁的 40 fps。

### 新增配方脚本

```
9  - 4K out 3x (A).bat                     4K 原生输出基线（--den 1 --interp 3）
10 - 4K out 3x + cq (B).bat                + --input-copy-queue
11 - 4K out 3x + prefetch (C).bat          + --prefetch-inputs
12 - automatic A-B-C-A 4K (60s each).bat   自动连续跑四段（本机唯一可信的 A/B 方式）
13 - smoothest + anti-drop (A-B).bat       配方 5 + --slack-auto
8  - smoothest + copy queue (A-B).bat      配方 5 + --input-copy-queue
```

### 这一版真正可用的东西

`--fg-warp` 的双 allocator 修复（真隐患）、`breakdown` 的 `cq=` 字段、
`collect_retired_inputs()` 的 swap-remove、UI 的「色彩空间」与「抗丢帧」两个控件、
`--slack-auto`、以及上面那条**假警报的修正**。其余全部默认关。

## v0.13.1 —— RE Engine / PRAGMATA 支持（2026-11-09，已实测通过）

**结论**：PRAGMATA 这条路走通了 —— `input source: ngx`、`shared ready`、bridge 正常出帧、
热键可以启动。排查过程中修掉 4 个真问题，其中两个属于"看起来在跑、其实什么都没发生"的静默失效。

### 修掉的问题

1. **NGX 入口不一定在驱动 shim 上。** RE Engine（Streamline）的 NGX 入口来自
   **游戏目录自带的 `nvngx_dlss.dll`**；旧判据只认 `_nvngx.dll` ⇒ 一个钩子都没装
   （ReShade 仍然报告 add-on 注册成功，但零数据）。现在判据 = 文件名含 `nvngx`
   ＋ **真的导出 `NVSDK_NGX_D3D12_EvaluateFeature`** ＋ 排除 `dlssg/dlssd/dlssnr`
   （CP2077 那些只读版本号的副本仍被第 3 条挡掉）。顺带：一个 provider 挂上就停，
   不再对转发到同一地址的 shim 重复挂载并打出 `NGX hooks FAILED`。
2. **采集暂停按帧算是慢加载游戏的灾难。** `init_swapchain` 之后 `+120 帧`，而 PRAGMATA
   加载期只有 **0.8 fps** ⇒ 实际暂停 **150 秒**，整个启动阶段不产出任何数据。
   现在改成「帧数或 **8 秒**，先到先算」。
3. **`LoadLibrary` 钩子拖慢 RE Engine 的加载**（用户 A/B：关掉 add-on 加载就正常）。
   新增 `[capture] LoaderHook=0` —— 完全不碰 `kernel32`，改为每 120 帧扫一次模块表。
   同时加了回调计时：实测我们的回调合计 **0.17 ms/帧**（barrier 0.02 ms/帧，约 400 次/帧）
   ⇒ "慢"确实来自钩子本身，不是回调里的活。
4. **游戏可能不从你改的目录读文件。** PRAGMATA 实际从
   `E:\PRAGMATA\_storage_\game_capture.addon64` 加载（进程级重定向），于是它找的 ini 是
   `_storage_\game_capture.ini` —— 我们改根目录那份**从来没被读过**，`Log` 一直是默认 0，
   所以"日志一行都没有"。
   - add-on：本地 ini 不存在时回退到 `%LOCALAPPDATA%\mgpu\game_capture.ini`；日志打不开时
     回退到 `%LOCALAPPDATA%\mgpu\game_capture-<pid>.log`；
   - 新增**面包屑** `%LOCALAPPDATA%\mgpu\breadcrumb.log`（每次 `AddonInit` 写一行：add-on
     的真实路径、ini 路径、`Log` 值、日志是否打开）—— 这次就是它一眼定位的；
   - 安装器新增 **6/6 镜像目录**：任何子目录里已有同名 add-on 或同名代理 DLL，就把
     add-on + ini 也放一份过去（`_storage_` 这类容器从此自动覆盖）。

### 实测（PRAGMATA）

```
config: ... SkipMv=0 DepthConvert=1 ... LoaderHook=0 Input=auto (path e:\pragmata\_storage_\game_capture.ini)
NGX provider: ...\_nvngx.dll   ->   NGX hooks active
dlss feature flags 0xB: MVJittered=0 MVLowRes=1 DepthInverted=1
input source: ngx (api 1) - pinned for this process
shared ready: name=Local\mgpu_mailbox_v1 pid=23056 used=253.6 MB reserved=253.6 MB slots=4
perf: frames=10680  begin=20ms  barrier=2.4ms (41k calls / 241k items)  present=1.4ms
```

排查全过程（含"GPU 0% 不是掉驱动"、"热键其实收到了"两次误判的证伪）见
`docs/findings/RE_ENGINE_NOTES.md` §3.1、§6.1–§6.4。
- 全部通过后：重建发布为 **v0.13.1**（0.13.0 的 add-on 不含以上修复）。

---

## v0.13.0 —— UE5「模型运动伪影」结案（2026-11-09）

**一句话**：伪影的主因是**上采样倍率**，不是运动矢量。游戏内 **DLAA（或最高画质档）+ `--interp 1 --fg-of`**
是本次唯一实测"干净"的组合；UE5 交给 NGX 的那条 MV 流是坏数据，不要用。
（完整证据链、排除表、dump 数字：`docs/findings/UE5_MV_OPTISCALER.md` §7–§8。）

**用户能感知的变化**

- **新配方**：`--interp 1 --fg-of` + 游戏内 DLAA（4K 下 26→52 fps，实测无模型边缘伪影）。
  DLSS 性能档（720p→4K，3 倍放大）**连不插帧的游戏本身**都有一点伪影，那部分不该算在插帧头上。
- **`--fg-of` 真的能用了**：以前 `motionVectorScale` 透传游戏的 `MV.Scale`（PBZ 报 1.0），
  库把 screen-UV 的 0.009 当成 0.009 像素 ⇒ 光流等价于没生效。现在用 MV 纹理尺寸
  （日志 `mvscale=(1278.0000, 720.0000)`）。**这条影响所有用 `--fg-of` 的配方（含 3x/4x 推荐档），
  `MV.Scale=1.0` 的游戏以前等于没开光流。**
- **add-on 开始读游戏声明的 DLSS flags** 并转发 `MVJittered` / `MVLowRes` / **`DepthInverted`（新增）**：
  日志 `dlss feature flags 0x4B: MVJittered=0 MVLowRes=1 DepthInverted=1`（需 `game_capture.ini` 的 `Log=1`）。
  PBZ 声明 `MVJittered=0`，所以"UE5 的 MV 带抖动、开 `JITTERED_MV` 就能修"这条**被证伪**。
- 新开关：`--fg-mv-scale raw|px|<float>`、`--fg-no-jittered-mv`；
  `--fg-jitter-zero` 重新生效（我们的颜色是上采样后的画面，jitter 已解掉，这是它的 A/B 对照）。
- `scripts/analyse_dump.py` 新增 **units fit**（拟合 `motion_px = mv_raw × k` 判 MV 单位与配对）；
  新配方脚本 `scripts/recipes/run_ue5_mv_ab.bat`（`auto/jit/nojit/depthinv/jitdepth/mvscale/nomv/of/ofsign/offov/jz/nodepth/only/dump`）。

**实测（PBZ，2x @ 60 锁帧，`cadence` 精确）**

| 档 | 结果 |
|---|---|
| 游戏 MV（`auto`）/ `jit` / `depthinv` | 伪影 + **闪烁** |
| `--no-mv` | 不闪，仍有边角伪影 |
| `--fg-of`（scale 修好前 / 后） | 与 `--no-mv` 相同 / 仍相同 ⇒ 伪影与 MV 无关 |
| **DLAA + `--fg-of`** | **干净** |

dump 出来的游戏 MV：0.5% NaN/inf、17% 饱和到 fp16 上限（±65504）、x 中位 0 / y 中位 +31、
与真实位移相关性 ≈0 —— 不是可用的速度场（与 2026-11-07 对 PBZ 的旧结论一致）。

---

## v0.12.2 —— 文档：UE5 备注（2026-11-09）

- README 新增「游戏引擎备注」：**UE5 不建议用运动矢量**（加 `--no-mv`）+ **必须开「分层窗口」**
  （`--overlay-layered`，UI 默认已开），并把原因写清楚（UE5 的 MV 在抖动后的画面上算、单位/方向
  不统一；UE5 自认为"未激活"时会丢菜单点击）。同样的备注也进了发布包里的 `README.md`
  （= `docs/ops/USAGE.md` §6）。

---

## v0.12.1 —— add-on 安装器改为强制覆盖（2026-11-09）

**修的是"装好了但跑不起来"这类问题**：旧 add-on / 旧 `game_capture.ini` 残留会让协议对不上、
或让新参数（`StreamHeight=1440`）看着设了其实没生效。

- 安装器**把所有文件强制覆盖**成本包版本（不再"存在就跳过"）；覆盖前改名备份
  （`x.mgpu_bak` → `.mgpu_bak2` … 最多 9 份，绝不覆盖更早那份原件）；
- 每个文件写完立刻 SHA256 校验，不一致直接失败并说明原因（被占用 / 杀软拦截 / 没权限）；
- 装完打印结果表（新建 / 覆盖 / 已是最新 + 备份名）并扫描残留（只提示不删）；
- 新增 `-KeepReShadeIni`：保留用户自己在 ReShade 面板里调过的 `ReShade.ini`，只补两个键；
- 重复运行安全（内容一致 → "已是最新"，不产生新备份）。

顺带修掉一个真 bug：安装新建文件（例如 shader）时 `Split-Path -Leaf` 拿到空备份名会抛
"Path 为空字符串"，导致安装在第 4 步中断。

---

## v0.12.0 —— 动态逐帧可用、MV 不再 drop（2026-11-09）

> 本版包含 0.11.3 – 0.11.12 的全部改动（0.11.2 之后的工作）。

**新功能**

- **动态逐帧模式**（UI 的"模式"分成 固定 / 动态逐帧 两块，互斥）：
  每帧按实际经过的 vblank 数决定倍率（`--interp-cadence`），输出恒定 1 帧/vblank，
  输入 35–45 fps 也能把 120 Hz 填满。配套：
  - `--cadence-smooth N`（默认 8）：先对"到达间隔"做 N 帧 EMA 再算倍率。
    逐帧原始间隔会让倍率跟着抖 1:1（切换 29 次/秒），实测 EMA 后切换 12 次/秒、
    空刷新 3.9% → **1.3%**。
  - `--fg-out-hz N`：输出帧率上限（`0` = 面板刷新）。把"120 Hz 面板只要 60 fps"
    变成 2x 而不是 4x；最低 2x，够不到时会打 warning 说明实际跑到多少。
- **`[capture] StreamHeight`（add-on，默认 1440）**：跨卡流（MV+深度）的高度上限，
  宽度按渲染宽高比算。4K 渲染由 63.3 → **28.2 MB/帧**；`0` = 跟随渲染尺寸（旧行为）。
- UI 的"输出帧率"改成手输数字框；"倍数"在动态模式下作为容量上限。

**修复**

- **开 MV 就 drop**：根源不是插帧算法，而是渲染卡每帧为 MV+深度扛的 63 MB 跨卡流量
  和两个全屏转换 pass（渲染卡被游戏吃满，转换抢不到执行窗口）。`StreamHeight=1440`
  之后**实测不再 drop** ⇒ 动态逐帧也能正常用游戏 MV。
- 到达间隔统计曾用 `GetTickCount64`（1 ms 粒度，0 ms 样本被丢），中位数报 31 ms
  而不是 25 ms，倍率被吸附到 4 ⇒ 输出只有 96.8 fps。改用 WGC 的 `SystemRelativeTime`
  后 3x@40 = **120.0 fps / 1.00 vblank**。
- `show=` 曾按 `loop_fps × framesPresented` 计算（120 Hz 面板上读出 148/164），
  改成 `presents / 窗口时长` ⇒ ≤ 122。
- bridge 的 `Present`/fence 等待不再每次新建事件（`wait_fence_value`），后缓冲缓存、
  只在 generation 变化时重读流几何；日志 flush 限流 200 ms（`--log-flush`）。

**诊断**

- `breakdown` 里新增 `colour=` / `cpu=`；新增 `bus:`（每帧跨卡字节与 GB/s）、
  `arr(vb):`（到达间隔的 vblank 分布）、`out:` / `out2:`（present 与 vblank 计数）、
  `game(vb):`、`bursts:`、`rt spikes:`、`rt auto:`。
- `bus:` 超过 48 MB/帧或 `inputs+copy` 占循环 ≥25% 时，额外打 `headroom:` 建议；
  流尺寸 > 1440p 时提示 add-on 的 `StreamHeight=1440` 能省多少。
- `--overlay-monitor N` / `--overlay-hidden`：覆盖窗口放到指定显示器、
  或只跑交换链不上屏（用来隔离"同屏覆盖窗口抢 independent flip"的问题）。

**已知取舍（都实测过，别再试）**

| 尝试 | 结论 |
|---|---|
| `--cadence-hold N`（保持倍率 N 帧） | 三组都更差：空刷新 6.6–10.4%、`slow` 10–13 帧/窗口。用丢帧换稳定，不值 |
| `--fg-cad-rt 1`（frameRenderTime = 批次×vblank） | 理论上更自洽，实测更差：`Present` 按预算阻塞，而输入间隔在下一帧到达时才量 ⇒ xN 从 3.00 漂到 3.38 |
| 动态逐帧 + `--fg-of`（光流） | 同卡多一个全屏 pass，drop 明显增加；30 秒段看不出来，长玩露馅 |
| `--no-depth` | disocclusion 会渗色（`HANDOFF` §8-28） |
| 固定 3x @44–45 fps 输入 | 45×3 = 135 > 120，必然超额，`slow` 23–32/窗口 |

**UI**

- 模式分成"固定 / 动态逐帧"两块（互斥，模式外的行灰显且点不动）；
  移除 `--dyn-mfg` 实验档。
- 动态逐帧下"运动矢量"行会显示权衡：`游戏 MV（动态逐帧可能闪烁）` /
  `AMD 光流（动态逐帧更易 drop）`。
- 修 Win+Tab 后覆盖窗口丢失：创建即带 `WS_EX_TOOLWINDOW`、DWMWA peek 排除、
  `ITaskbarList::DeleteTab`；`fg_is_shell_ui()` 把任务视图/Alt+Tab/Win+D 排除在焦点丢失之外。
  （不要用 `GWLP_HWNDPARENT` 设 owner —— 会让游戏鼠标输入失效。）

## v0.10.x 及更早

见 `git log`、`docs/findings/HANDOFF.md`（里程碑历史）与
`docs/findings/ARCHIVED_EXPERIMENTS.md`（已移除的实验与结论）。
