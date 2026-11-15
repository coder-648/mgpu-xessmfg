# FG 契约优先：让游戏以为在出帧，只钩 FG 管线

> 状态：设计（2026-11-10）。前身：`docs/design/FG_INPUT_PLAN.md`（v13，逐场景验证）、
> `docs/findings/FG_INPUT_V13_2077.md` / `_KHAZAN.md` / `_PRAGMATA.md`（三款游戏的实测）。

## 0. 结论先行

三款游戏的实测证明了一件事：**各引擎的 SR 路径千奇百怪，FG 路径反而高度一致**。

| | CP2077（RED4） | Khazan（UE 4.27） | PRAGMATA（RE Engine） |
|---|---|---|---|
| DLSS-SR 走哪 | Streamline | 游戏自带 `nvngx_dlss.dll`（NGX 直调） | Streamline（`sl.dlss.dll`） |
| FG 走哪 | Streamline | Streamline（引擎插件，只有 `sl.dlss_g.dll`） | Streamline |
| SL 是不是输入源 | 是 | **否**（只有 FG 期间有数据） | 是 |
| MV 单位 | UV（SL 报 `(1,1)` 是假的） | 像素（自报正确） | UV（SL 报 `(1,1)` 是假的） |
| 相机 | SL 每帧给真矩阵 | 无（要手填 fov） | SL 给真矩阵（`fovY=58°`） |
| depth tag lifecycle | 有，但不每帧 | `validUntilNow`，每帧 | **`validUntilEvaluate`（不能当输入读）** |
| 占位值 | `mvecScale=(1,1)` | 无 | `mvPx=1x1`、fov 90°、cameraPos 0 |
| frame token | 有（SL 出 FG） | **没有**（FG 关 ⇒ 不 evaluate） | 有 |

⇒ 逐游戏探 SR 路径是不可持续的。**唯一跨引擎稳定的接口是 FG 契约**：
一旦游戏决定出帧，它**必须**交出 MV、深度、HUDless 颜色、UI alpha、投影矩阵、
jitter、reset 和输出/backbuffer —— 这些不是"引擎特性"，是 API 合同。

所以路线改成：

```text
1. Claim   —— 让游戏相信 FG 可用且已开启（否则它永远不 evaluate）
2. Harvest —— 在 FG evaluate 处一次性拿下它必须提供的整套输入
3. Validate—— 一份通用校验器（资源尺寸 / 占位检测 / lifecycle / 单位），不认引擎
4. Own     —— 输出由我们呈现，游戏那边只看到"FG 在跑"
```

## 0.1 方法论：不逐游戏验证，只验证机制

一句话：**"这款游戏能过"永远不是结论，"这套判定规则在这种形状的输入上能过"才是。**

三款游戏的实测（RED4 / UE4.27 / RE）只用来**证明机制的形状**（哪些数据在 FG 契约里、
哪些是引擎自由发挥），**不用来建"支持列表"**。理由很直接：

- 游戏数量无限，组合（引擎 × 集成方式 × 版本 × 破解/DRM）远超能人工覆盖的量；
- 同一引擎不同游戏已经证明会不一样（Khazan 的 SL 只做 FG；PRAGMATA 的 SL 给占位值）；
- 一旦开始"这个游戏要加一条特判"，就回到了不可维护的老路。

所以最终形态必须是**运行时自证**：

```text
游戏 → FG 契约（claim 之后必然被驱动）→ 拿到一帧输入
     → 通用校验器：形状 / 新鲜度 / 非退化 / 单位自洽
     → 通过 → 消费；不通过 → 明确降级（SR 输入 → 光流 → 关闭），并在 harvest 位图里留痕
```

**"能不能用"由校验器的判定决定，而不是由游戏名单决定。**
一款新游戏上线，我们不需要认识它；它要么喂出能过校验的数据，要么被明确标成"喂不出"。

### 0.2 因此优先级：claim 是前提，探针只是兜底

- **FG 存在**的题目（绝大多数 3A）：**claim**（让游戏真的跑 FG 路径）⇒ 输入由契约保证，
  校验器只做"是否合格"的判断；
- **FG 根本不存在**的题目：claim 无从谈起，退回 SR 输入（v13 现状）+ 光流兜底；
- 判定是自动的：`CreateFeature(DLSSG)` / `slGetPluginFunction` 是否出现过，
  **不是查表**。

这也是为什么 A 阶段（harvest + validate）必须先落地：它是 claim 之后唯一能判断
"这次喂的对不对"的东西。

## 1. Claim：怎么让游戏以为在出帧

已有钩子（v13 已实现，只是目前只用于"记录/关 FG"）：

| API | 现在 | 改成 |
|---|---|---|
| `slDLSSGGetState` | 只读打印 `status/framesToGenerateMax` | 返回成功 + `status=eOK/available`、`framesToGenerateMax=N`（N=2 或多帧能力） |
| `slDLSSGSetOptions` | `SlFgOff` 时把 `mode` 改成 `eOff` | **反向**：把 `mode` 改成 `eOn`、`numFramesToGenerate` 钉成我们要的 N，并记住游戏请求的 N |
| `slEvaluateFeature(kFeatureDLSSG)` | 只抓 token | 保留真实调用，但**输出资源指向我们给的替身**（见 §1.2） |
| NGX `DLSSG.Available` / `DLSSG.FeatureEnabled` / `MultiFrameCount` | 只读打印 | 全部改写成"可用/已开/N" |
| `FgSuppressNgx` | 短路 evaluate | **废弃**（见 §1.3） |

**实现状态（2026-11-10）**：第一、二行已落地，开关是 `[capture] Claim=1`（**默认 0**）：

```text
config: SlInput=1 SlFgOff=0 Claim=0 (...)
fg: Claim=1 - reporting DLSS-G as available (2x) so the game keeps running its FG path;
    expect a duplicate/black picture until the output side exists
fg: Claim=1 - forcing slDLSSGSetOptions mode=eOn (framesToGenerate=1)
```

定位是**数据采集开关**：画面在拥有输出所有权之前一定是错的（重复帧/黑屏），
但 `harvest:` 位图会告诉我们这套集成到底喂不喂 MV/深度/相机/token。


### 1.2 Claim 必须与"交得出帧"绑定

CP2077 的黑屏不是偶然：当时是**抑制了 evaluate 却不接管 present**，
游戏的 present 契约（"我 present 一次，SL 会补 N 帧"）被破坏 ⇒ 只剩黑屏。

所以 claim 只有两种合法形态：

- **B 档 影子 present**：still 让游戏的 FG 跑，但把它的 `outputInterp`/backbuffer
  换成我们的私有资源（内容=当前帧），present 出去是"重复帧"；
- **C 档 完全接管**：我们自己的 swapchain/overlay 出图，游戏窗口压掉不显示。

两者都要求我们**先能在 C 档出图**，所以实现顺序是 C 的一半（overlay 已有）→ B → claim。

### 1.3 不做什么

- 不用 `FgSuppressNgx`；不用"跳过 `slEvaluateFeature`"（它会破坏 SL 的 swapchain/pacing）；
- 不在游戏没有初始化 FG 的情况下强行 claim（先确认它至少 create 过 DLSS-G 的 feature handle）。

## 2. Harvest：在 evaluate 处抓"它必须给的全套"

现成钩子（v13 已有，但只用了 MV/depth + 少量元数据）：

```text
sl.interposer: slSetConstants  slSetTag(viewport)  slSetTagForFrame  slEvaluateFeature  slGetFeatureFunction
sl.dlss_g:     slGetPluginFunction → slDLSSGSetOptions / slDLSSGGetState
nvngx_dlssg:   NVSDK_NGX_D3D12_CreateFeature / EvaluateFeature + NVSDK_NGX_Parameter 全量枚举
```

要抓的元素（**这就是统一适配的数据面**）：

| 类别 | 内容 | 现状 |
|---|---|---|
| 运动 | `MOTION_VECTORS`（含 lifecycle/extent/state） | ✅ 已抓，已消费 |
| 深度 | `DEPTH`（**必须在 evaluate 内拷** —— `validUntilEvaluate` 就是为这个存在的） | ⚠️ 只抓不读（PRAGMATA） |
| 颜色 | `HUDLESS_COLOR`（无 UI 的干净帧） | ❌ 未抓 |
| UI | `UI_COLOR_AND_ALPHA` | ❌ 未抓（三款实测里**都没有**这个 tag） |
| 缩放链 | `SCALING_INPUT` / `SCALING_OUTPUT` | ❌ 未抓（PRAGMATA 有） |
| 相机 | `cameraViewToClip` / `clipToPrevClip` / `prevClipToClip` / near·far·fov / pinhole / cameraPos | ✅ 已抓（只发布） |
| 时间 | `FrameToken` + QPC、`reset`、jitter | ✅ 已抓，配对/reset 已消费 |
| 节奏 | `numFramesToGenerate`（游戏请求几帧）+ `presented` 状态 | ✅ 已抓（未消费） |
| 输出 | `backbuffer` / `outputInterp` 指针 | ⚠️ 只在 sniff 里打印 |

**关键点**：`slSetTag` 的 lifecycle 决定拷贝时机 ——
`validUntilEvaluate` 的 tag 只能在 evaluate *内部*读，这正是"在 FG 流上抓数据"
比"在 SR 出口抓"更可靠的原因。

## 3. Validate：一份通用校验器，取代逐游戏特判

三款游戏暴露的都是"声明不可信"这一类问题，用同一套规则就能覆盖：

| 规则 | 反例（实测） |
|---|---|
| 尺寸只信资源 `GetDesc()`，不信 tag extent / `mvPx` | PRAGMATA：`extent=(0,0)` + `mvPx=1x1` ⇒ `render=0x0` ⇒ **MV 流全 0** |
| 占位矩阵检测：`fov≈90° + near=0.1 + far≈999.9 + cameraPos=0` ⇒ 判占位 | PRAGMATA 开局 / CP2077 菜单 |
| `mvecScale=(1,1)` 单独不能判单位，要与 `MV.Scale`、`probe` 交叉 | CP2077（UV 报成 `(1,1)`）、PRAGMATA 同理 |
| `mvPx ≤ 1` ⇒ 渲染尺寸未知，退回资源尺寸 | PRAGMATA |
| lifecycle=`validUntilEvaluate` ⇒ 不在 begin_effects 拷 | PRAGMATA 的 DEPTH |
| 帧 token 单调 + QPC 配对窗口 | CP2077/PRAGMATA 已验证 |
| **新鲜度**：token/`jitter`/常量哈希是否推进；连续 N 帧不变 ⇒ 标 `stale` | PRAGMATA 12:51–12:53：jitter 240+ 帧冻结，MV 指针不变，bridge 仍照抄 |
| **停滞帧不发布**（不能把上一帧当新帧发） | 同上：这是 2026-11-10 发现的一个真 bug |
| **内容非退化**：MV 全 0 / depth 全 0 / NaN 比例超阈值 ⇒ 判无效 | 三款都出现过"指针有效、内容为空" |


校验器输出一份**统一语义**的 `FrameInputs`（v14），下游只认这份语义，不认引擎。

## 4. Own：输出所有权（三档）

| 档 | 谁 present | 屏幕上是 | 用途 |
|---|---|---|---|
| A | 游戏照常 + 我们 overlay | 两份（会打架） | **只用于验证数据**，不上屏 |
| B | 游戏 present 我们给的"重复帧" | 只有我们 | claim 的安全形态 |
| C | 只有我们（游戏窗口隐藏） | 只有我们 | 终态 |

要拦的点：`IDXGISwapChain::Present/Present1`、SL 自己的 present 补帧路径、
以及 FG 输出资源（`outputInterp`）——B 档就是把它换成我们的私有资源。

## 5. 落地阶段（每阶段都能独立验收）

| 阶段 | 内容 | 验收 |
|---|---|---|
| **A. Harvest+Validate** | 在 FG evaluate 处抓齐 DEPTH/HUDLESS/UI/SCALING 的**能力位**，跑通用校验器；不 claim、不改呈现 | ✅ 已实现（`8a3b081a…`）：日志里出现 `harvest: ...` 一行 |
| **B. 颜色入槽** | HUDless / UI_ALPHA 跨卡进镜像（替掉 WGC 颜色） | ⏸ **本轮放弃**（占用账见 §9，决定见 §5.1） |
| C. Claim（B 档） | 让游戏相信 FG 在跑，输出交重复帧（**颜色继续走 WGC，producer 成本 ≈ 0**） | CP2077 不黑屏 + 数据完整；RE/UE4 同样 |
| **D. 多帧** | 游戏请求 N 帧 ⇒ 我们出 N 帧（不再依赖 `--unlock-mfg` 打补丁） | N=2/3 的分辨率与 pacing |

> A 阶段**不需要 claim**：Khazan 已经证明"FG 关着但管线还在"时也会每帧 setTag/setConstants，
> 所以先把数据面和校验器做完，claim 只是让"FG 根本没初始化"的题目也能上牌桌。

### 5.1 本轮范围（2026-11-10 决定）

按 §9 的占用账，**HUDless / UI 两条颜色流本轮放弃**（4K 上 +2.2~5.0 ms，等于 +10~25 点占用，
PRAGMATA 这种已经 97% 的题直接掉帧）。所以：

- 颜色继续走 **WGC**（`Own` 用 A/B 档，游戏窗口仍正常 present）；
- 本轮只做 **A（harvest + validate）**，产出 `harvest:` 位图作为统一数据面；
- claim（C 阶段）等 A 的数据面稳定后再上，且**不含颜色入槽**。

A 阶段已实现（build `8a3b081a…`），日志形状：

```text
harvest: mv=y depth=y camera=y clip=y token=y reset=n hudless=y ui=n scaling=-- expose=n placeholder=n
```

各字段含义：`scaling=io` 表示 SCALING_INPUT+OUTPUT 都在；`placeholder=y` 表示这一帧的
`cameraViewToClip` 是占位块（此时 camera/clip 位会被强制清掉，bridge 不会拿它当投影）。


## 6. 风险与未知（要提前设计，不要事后补）

| 风险 | 说明 | 缓解 |
|---|---|---|
| 游戏根本不 init FG | 例如把 DLSS-G 从菜单删掉/破解版关闭 | claim 前先探测 `CreateFeature(DLSSG)` 是否发生过；没有就退回 SR 路径（v13 现状） |
| present 契约破坏 | CP2077 的 `FgSuppressNgx` 已经演示过一次黑屏 | claim 必须与"我们交帧"绑定（§1.2），A 档只做数据 |
| Reflex / pacing | 宣称 FG 会改变游戏的 present 节奏与 Reflex 限帧 | 先量：`game_dt` / 输入间隔 / `slDLSSGGetState.presented` 三项对齐后才接 C 档 |
| HDR / 色彩空间 | 接管后色彩由我们负责（BT.2020 PQ vs scRGB） | 复用 bridge 现有 HDR10 管线（已在 CP2077/Khazan 验证） |
| 跨卡带宽 | HUDless + UI + MV + depth 全都要跨卡 | 4K HUDless 63 MB/帧 ×2（UI）——必须上**压缩/降采样**或只拷 MV/depth + WGC 颜色（A 档） |
| 反作弊 / DRM | RE Engine 这类题对注入敏感 | claim 只在"已确认 FG 存在"后开启，且提供 `Claim=0` 总开关 |

## 7. 协议：v13 → v14

v13 只追加了元数据。要真正"统一适配"，协议要开始承载**颜色**：

```text
FrameShared v14（追加，不改既有字段）
  hudless_slot / ui_slot / scaling_out_slot      // 新增纹理流（可空）
  ui_alpha_present : 1
  harvest_flags    : bitmask  (mv|depth|hudless|ui|scaling|clip|camera|expose|backbuffer)
  declared_frames  : 游戏请求的 numFramesToGenerate
  frame_token_state: 0=无 1=单调 2=回退
```

`harvest_flags` 是整个方案的"体检报告"：任何一款游戏只要这一位图能填满，
就说明它的 FG 管线把该给的都给了，**不需要再为它写特判**。

## 8. 验收矩阵（每引擎至少一款）

| 引擎 / 题目 | 实测 tag 列表 | 数据完整 | claim 后不黑屏 | 2x 画质 |
|---|---|---|---|---|
| RED4 / CP2077 | DEPTH + MV + **HUDLESS** | 待 A 阶段 | 待 C 阶段 | 待 B 阶段 |
| UE 4.27 / Khazan | DEPTH + MV + **HUDLESS** | 颜色有源、无 UI | — | — |
| RE Engine / PRAGMATA | SCALING_IN + SCALING_OUT + DEPTH + MV + EXPOSURE | **无 HUDLESS/UI**，颜色只能靠 `SCALING_OUTPUT` 或 WGC | — | — |
| UE5 / 影之刃零 | 待测 | — | — | — |

> 注意：**不是每款游戏都提供 hudless/ui**。三款实测里 `UI_COLOR_AND_ALPHA` 一次都没出现；
> `HUDLESS_COLOR` 只有 RED4 和 UE4 给了，RE Engine 给的是 `SCALING_OUTPUT`。
> 所以"统一适配"= **统一的数据面 + 明确的缺失位（`harvest_flags`）**，
> 缺失项各有兜底（`SCALING_OUTPUT` → 否则 WGC）。

## 9. 成本：颜色入槽到底贵多少（4K 实测量级）

### 9.1 每帧过总线的量（4K / 1440p 流）

| 流 | 尺寸/格式 | 4K 每帧 | 备注 |
|---|---|---|---|
| MV（现在） | 2560×1440 R16G16 | 14.7 MB | `--mv` 开销行 `bus` 里能看到 |
| depth（现在） | 2560×1440 R32 | 14.7 MB | 可 `--no-depth` 整条拿掉 |
| **HUDless 颜色** | 3840×2160 RGB10A2（4 B/px） | **33.2 MB** | 与 `Color=1` 那份实测同尺寸 |
| **UI 颜色** | 3840×2160 | **33.2 MB** | |
| **UI alpha** | 3840×2160 R8 | 8.3 MB | 若与颜色同槽可合并 |
| `SCALING_OUTPUT`（RE） | 3840×2160 | 33.2 MB | **已含 UI**，可一条顶两条 |

- 现状：**28.2 MB/帧**（MV+depth，`StreamHeight=1440`）；
- 只加 HUDless：**61.4 MB/帧**（×2.2）；
- HUDless + UI + alpha：**94.6 MB/帧**（×3.4）。

带宽不是问题：94.6 MB × 44 fps = **4.2 GB/s**，PCIe 4.0 x16 实测可用 ~25 GB/s。

### 9.2 真正贵的地方：那一笔写在**游戏的 command list** 上

仓库里已经量过同尺寸的代价（`[capture] Color=1`，`docs/design/ARCHITECTURE.md` §6.1）：

```text
5K = 56.25 MiB/帧（4K = 33.2 MB/帧）的 PCIe 写，追加在游戏自己的 command list 上
→ 渲染卡 GPU 占用 80% → 95%，余量从 20% 掉到 5%，峰值场景掉帧
```

也就是说，**4K 加一条 33 MB 的颜色流 ≈ +10~15% 渲染卡占用**（不是带宽，是那次拷贝的 GPU 时间）。
两条（HUDless+UI）就会把 3080 Ti 顶到 ~95%，这正是当年把 `Color=0` 定成默认的原因。

### 9.3 四个降本手段（按优先级）

| 手段 | 效果 | 代价/前提 |
|---|---|---|
| **用 `SCALING_OUTPUT` 单流顶替 HUDless+UI** | 94.6 → 33.2 MB/帧 | 没有 UI 分离，插值时 UI 仍会被"抹"（和今天的 WGC 一样）；RE Engine 有这条流 |
| **`OffloadQueue=1`（拷贝走自己的 COPY 队列）** | 把 GPU 时间从游戏 list 挪走 | 失败模式是设备复位，默认 0，**必须专门做一场验收**（ROADMAP 工程债） |
| **只在需要时拷颜色** | 平时 0 额外开销 | 只有"完全接管 present"（Own 的 C 档，游戏窗口不显示）才绕不开；B 档影子 present 仍可走 WGC（见 §9.6） |
| **降采样颜色（1440p）** | 33.2 → 14.7 MB/帧 | 显示画质直接下降，只能用于"对齐/遮罩"，不能用于显示 |

### 9.4 因此 B 阶段要拆成两步，并且先量后改

- **B1（推荐先做）**：用 `SCALING_OUTPUT`（有则用，没有则 HUDless）**替换** WGC，
  不额外加 UI 流。预期净增 ≈ 0（WGC 那一跳本来也在 GPU 上做），收益是
  **颜色与 MV/深度严格同帧** + 去掉 7–12 ms 的 WGC/DWM hop；
- **B2（真正加钱的部分）**：HUDless + UI + alpha 分离，为了 UI 插值/挖洞。
  必须先完成 `OffloadQueue=1` 的验收，否则 4K 下就是 +15% GPU。

> **2026-11-10 决定**：按 §9 的账，B1/B2 本轮都不做（见 §5.1）；颜色继续走 WGC。
> B1 的收益（严格同帧、去掉 7–12 ms DWM hop）留到 claim 之后再评估。

### 9.5 预期占用（按实测外推，渲染卡 = RTX 3080 Ti）

外推依据：`Color=1` 在 5K/56 MB 时把渲染卡从 **80% 推到 95%**（+15 点 @ ~40 fps）。
换算成时间 ≈ **+3.75 ms/帧**，即这条路径的有效拷贝吞吐 ~**15 GB/s**。
所以 4K 的 33.2 MB ≈ **+2.2 ms/帧**，占用增量按帧长等比放大：

| 场景 | 每帧新增拷贝 | 44 fps 下 | 60 fps 下 |
|---|---|---|---|
| 现状（WGC 颜色，MV+depth 28 MB） | 0 | 80%（基线） | 80%（基线） |
| + HUDless 一条（33 MB，游戏 list） | +2.2 ms | **≈ 90%** | **≈ 93%** |
| + HUDless + UI + alpha（75 MB） | +5.0 ms | **≈ 102%（爆）** | **≈ 110%（爆）** |
| + HUDless（改 `OffloadQueue=1`，COPY 队列并行） | 拷贝与 3D 并行 | **≈ 83–86%（待实测）** | ≈ 85–88% |
| + HUDless+UI（`OffloadQueue=1`） | 同上 | **≈ 88–92%（待实测）** | ≈ 92–96% |

结论：

- **单流（HUDless 或 SCALING_OUTPUT）** 在 60 fps 题上可接受（80→93），
  在 44 fps 高占用题上就是 **+10 点**，等于把 44 fps 打到 ~40；
- **双流（HUDless+UI）在 4K 不可接受**，除非 `OffloadQueue` 验收通过且题本身有余量；
- **PRAGMATA 这种 4K DLAA 已经 97% 的题，任何颜色入槽都直接等于掉帧**（没有余量可借）。

### 9.6 关键前提修正：claim **不需要**放弃 WGC

前面把"颜色入槽"当成 claim 的前提，这个前提不成立：

`B 档影子 present`（游戏把**重复帧** present 到它自己的窗口，我们的 overlay 盖在上面）
⇒ 游戏窗口仍然在正常 present ⇒ **WGC 仍然能抓到 4K 颜色，producer 侧额外成本 ≈ 0**
（多出来的只是 DWM 多合成几帧，那个 LUT 本来就在跑）。

所以成本顺序应该改成：

```text
claim（+WGC 颜色，≈0）
   → 有需求才加 HUDless（+2.2 ms，换 UI/边界的插值质量）
      → 有需求才加 UI+alpha（再 +2.8 ms，换 UI 挖洞）
```

颜色入槽真正的两个理由只剩：
1. **HUDless**：去掉 UI 后再插值，UI/字幕/准星不会被"抹"（画质收益最大）；
2. **严格同帧配对**：WGC 那 7–12 ms 的 DWM hop 消失（延迟/边界收益）。

两者都不是 claim 的前置条件，所以 **claim 可以零 GPU 成本先落地**。

### 9.7 验收口径（一次 A/B 就能定生死）

同一款游戏、同一场景、固定 60 s，四组：

| 组 | 颜色来源 | 看什么 |
|---|---|---|
| A0 | WGC（现状） | 基线：`gpu` / `present` / `colour` / 渲染卡占用（`nvidia-smi` 采样） |
| A1 | `SCALING_OUTPUT` 入槽 | 渲染卡占用相对 A0 的差、`colour=` 行、`age=` |
| A2 | HUDless 入槽 | 同上 |
| A3 | HUDless+UI 入槽 + `OffloadQueue=1` | 是否复位、占用是否回到 A0 水平 |

判据：**渲染卡占用增幅 ≤ 3% 且 `age` 中位数下降** 才算可接受。

## 10. 实证：颜色不入槽也走通了全链路（PRAGMATA，2026-11-10）

```text
游戏内 FG 开（present 50 → 77） + bridge 2x
  harvest: mv=y depth=y camera=y clip=y token=y reset=y hudless=y ui=y scaling=io expose=y
  颜色: WGC 抓窗口（producer 侧零额外拷贝）
  bridge: xN=2.00, presents=80/80
```

结论：**§5.1 的取舍（放弃 B 阶段）不影响 full chain**。FG 契约提供数据面、WGC 提供画面、
bridge 提供输出；`HUDLESS_COLOR` / `UI_COLOR_AND_ALPHA` 只有在 FG 管线初始化后才出现
（FG 关着时是 `hudless=n ui=n`），所以"让游戏相信 FG 在跑"的价值是**数据**，不是颜色。

`SlFgOff` 于是变成一个有意义的开关：`0` = 游戏自己也出帧（WGC 拿到 real+generated 混合流），
`1` = 只要它初始化管线、不许出帧（WGC 拿到的全是真实帧，数据同样完整）。

## 11. 动态数据：现有钩子能覆盖到哪（2026-11-10 评估）

| 动态变化 | 现在能否跟上 | 说明 |
|---|---|---|
| FG 开关 / `mode` 变化 | ✅ | `slDLSSGSetOptions` 每次调用都过钩子，`SlFgOff`/`Claim` 立即生效 |
| `numFramesToGenerate` 变化 | ✅ v14 | 抓到并透传：`ProducerInfo.declared_frames`，bridge 打印 `framesToGenerate=N <== the game itself is generating frames` |
| 渲染分辨率变化（质量↔DLAA） | ✅ | content-size gen 2/3（PRAGMATA 实测不用重启游戏） |
| tag 集合逐帧变化 | ✅ | 按类型合并，保留上一份已知值 |
| `reset` 置位 | ✅ | 逐帧传到 `kInputFlagResetHistory`（抓到过 2 次） |
| **SL 停止发布（管线被拆）** | ✅ v14 | `input: source ... STALE` + `kInputFlagMvSuspect`，不再重放旧帧 |
| **多 viewport** | ✅ v14 | 只采纳一个 viewport，另一个静默 250 ms 后才接管 |
| 资源被销毁/重建 | ⚠️ | `hold()` 保住了引用不会崩，但**不校验它还是不是游戏当前在用的那张** |
| `validUntilEvaluate` 的数据 | ⚠️ | 按设计不读；要在 evaluate 内拷才算真正吃到 |

所以答案是：**v14 之前能处理"值的变化"，不能处理"供给的中断与多路并行"**。v14 补齐了三件：

1. ~~新鲜度门控~~ ✅ 已实现：`serial`/QPC 超过 250 ms 没推进 ⇒ `input: source ... STALE`
   并置 `kInputFlagMvSuspect`；消费端据此回退（`--fg-of` / `--no-mv`），不再发旧帧；
2. ~~按 viewport 存~~ ✅ 已实现：`viewport_adopt()`，只采纳一个 viewport，
   另一个在当前 viewport 静默 250 ms 后才接管（分屏/反射探针不会串）；
3. ~~`declared_frames` 透传~~ ✅ 已实现：`ProducerInfo.declared_frames` + `harvest_flags`，
   bridge 启动时打印 `producer declaration: framesToGenerate=N <== the game itself is generating frames`。

仍留 ⚠️ 的两项（资源被重建不校验、`validUntilEvaluate` 不在 evaluate 内拷）见上表。

## 12. 四条 FG 车道：动态识别、统一出口（2026-11-10 定稿）

用户提出的形态，也是这份设计最终收敛的结论：**一个游戏切模式数据就变，所以不可能静态适配；
唯一方案是"运行时识别谁在出帧，然后从它那里动态取数"**。

```text
        ┌─ lane 1  DLSS-G   (sl.interposer / nvngx_dlssg) ─┐
        ├─ lane 2  FSR-FG   (ffx-api framegeneration)     ─┤
游戏 →  ├─ lane 3  XeSS-FG  (libxess_fg xefgSwapChain*)   ─┤→ 统一 FrameInputs v14 → 校验器 → XeFG
        └─ lane 4  无 FG   (SR 输入 + 光流/无 MV)          ─┘
```

四条车道的**出口是同一份东西**：`FrameInputs`（MV / 深度 / 相机 / clip / jitter / reset /
`frame_key` / `declared_frames`）+ `harvest_flags` 位图。消费端（bridge → XeFG）**完全不知道**
数据来自哪条车道 —— 这是"不做逐游戏适配"在代码层面的落点。

### 12.1 车道 × 钩子 × 现状

| 车道 | 关键入口 | 数据 | 现状 |
|---|---|---|---|
| **1. DLSS-G** | `slSetConstants` / `slSetTag` / `slEvaluateFeature`；`slDLSSGSetOptions/GetState`（经 `slGetFeatureFunction`）；`nvngx_dlssg` 的 `Create/EvaluateFeature` + `NVSDK_NGX_Parameter` | 相机 / clip / jitter / MV / depth / `FrameToken` / `numFramesToGenerate` | ✅ 已作为输入源使用（CP2077、PRAGMATA）。坑：tag 无 extent、`validUntilEvaluate`、占位矩阵、光追换格式、UAV 状态 |
| **2. FSR-FG** | `ffxDispatch` 里 `type == FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE` 的 `ffxDispatchDescFrameGenerationPrepare` | `frameID` / `jitterOffset` / `motionVectorScale` / `depth` / `motionVectors` / `cameraNear·Far·Fov` | ⏸ **代码就绪，待样本**：`make_ffxfg_source()`；PRAGMATA 只发 generation（无矢量数据），要验需要一款自发布 PREPARE 的题目 |
| **3. XeSS-FG** | `xefgSwapChainTagFrameConstants` / `TagFrameResource` / `D3D12InitFromSwapChain(Desc)` / `SetNumInterpolatedFrames` / `SetPresentId` | 视图/投影、jitter、MV scale、depth / MV、`initFlags`、帧数与 token | ⏸ **代码就绪，待样本**：`make_xefg_source()`，覆盖度自报 `6/6`；要验需要一款原生用 XeSS-FG 的题目 |
| **4. 无 FG** | 走 SR 输入源（`ngx` / `ffx` / `xess` / `depth`） | MV / depth / jitter（相机多半没有） | ✅ 现状（v13/v14 的主路径），消费端可退回光流 |

### 12.1b "100% 覆盖"到底覆盖了什么（2026-11-10 按导出表核对）

| vendor | 模块 | 与**输入数据**相关的入口 | 状态 |
|---|---|---|---|
| NVIDIA | `sl.interposer.dll` | `slSetConstants` / `slSetTag` / `slSetTagForFrame` / `slEvaluateFeature` / `slGetFeatureFunction` / `slDLSSGGetState` | ✅ 5 装 + 1 未导出（该构建没有） |
| NVIDIA | `sl.dlss_g.dll` | 只有 `slGetPluginFunction`（`slDLSSGSetOptions/GetState` 经它取） | ✅ |
| NVIDIA | `nvngx_dlssg.dll` | `CreateFeature` / `EvaluateFeature`（数据都在 Evaluate 的 `DLSSG.*` 参数里） | ✅ |
| Intel | `libxess_fg.dll`（31 个导出） | `TagFrameConstants` / `D3D12TagFrameResource` / `D3D12InitFromSwapChain(Desc)` / **`SetNumInterpolatedFrames`** / **`SetPresentId`** | ✅ **6/6**（后两个本轮新增，日志会打 `XeSS-FG data coverage 6/6 (complete)`） |
| AMD | `amd_fidelityfx_framegeneration_dx12.dll` | `ffxCreateContext` / `ffxConfigure` / `ffxDispatch` / `ffxQuery` / `ffxDestroyContext`（数据在 `..._FRAMEGENERATION_PREPARE` 的 dispatch 里） | ✅ 模块本轮加入名单 |

**剩下的已知缺口**（都是"生命周期/能力"类，不是每帧输入）：

1. `NVSDK_NGX_D3D12_ReleaseFeature` / `_Shutdown` 没挂 ⇒ 游戏拆除 FG feature 时我们只能靠 250 ms 新鲜度门控发现，不能立刻知道；
2. `NVSDK_NGX_D3D12_GetCapabilityParameters` 没挂 ⇒ claim 之前无法先问"这个驱动支持多帧吗"；
3. FSR **3.0 风格**的 swapchain API（`ffxFrameGenerationSwapChain*`）没挂 —— 本作的
   `amd_fidelityfx_dx12.dll` 根本没有这些导出（只有 FSR-FG provider 的 5 个 ffx-api 入口），
   但别的 FSR3.0 题会需要；
4. `xefgSwapChainSetEnabled` / `D3D12CreateContext` 没挂 ⇒ lane 3 的"是否已启用"只能靠有没有帧来判断。

> 结论：**每帧输入面的三条车道都已覆盖**；缺口集中在"谁在什么时候启用/拆除"，
> 那些靠 v14 的新鲜度门控兜底，但补上会让切换更快、claim 更可靠。

### 12.2 动态识别（取代配置项）

```text
每帧/每 120 帧：
  某条车道"活着" = 该 API 在最近 250 ms 内被调用过（v14 的新鲜度门控）
  优先级       = 3 > 1 > 2 > 4      ← 越接近 XeFG 原生输入越优先，无 FG 最后
  切换         = 当前车道 stale 且另一条车道 live ⇒ 直接切换（不重启、不看游戏名单）
```

无需 `Input=sl|ngx|ffx` 这类人工选择；它也解释了今天 CP2077 的行为：
FG 关 ⇒ lane 1 只剩 SR 声明 ⇒ 我们实际在用 lane 4 的数据面。

**手动切车道（2026-11-10）**：游戏内切换就是最省事的"车道选择器"，但要分清两类：

| 游戏内动作 | 是不是换车道 | 说明 |
|---|---|---|
| 关掉 FG（DLSS-FG / FSR-FG 任一） | ❌ 不换 | 车道还在，只是不再 evaluate；数据面退化成 SR（= lane 4 的形状）。PRAGMATA、CP2077 都验过 |
| **换 FG 实现**（DLSS ⇄ FSR3 ⇄ XeSS） | ✅ **真正换车道** | 游戏会去初始化另一个 API，对应钩子开始出数；动态选择器 90 帧内自动重挑 |
| 换分辨率 / 画质档 | ❌ 不换 | 同一车道内 content-size 变化（实测 gen 2/gen 3） |

前提是**我们这边"不干预"**：`Claim=0` + `SlFgOff=0`。否则是我们在改写游戏的选择，手动切换就失效。
判谁活着只看日志里谁在涨：`sl.const #` / `ffx: first frame` / `xefg.const #`。

> 现实约束：PRAGMATA 的 `config.ini` 写 `UpscalingAlgorithm=FSR3` 被忽略（0 次 `ffx: first frame`），
> 说明这类切换**必须走游戏内菜单**；另外 `dlssg_sm86`（`Engine=XeSS`）也点不亮 lane 3 ——
> 它用的是自己 `runtime\` 下的私有 `libxess_fg` 副本，不是我们挂钩的 `_storage_\libxess_fg.dll`。

### 12.3 每条车道都必须过的同一道关

| 关卡 | 说明 |
|---|---|
| 尺寸 | 只信资源 `GetDesc()`，不信 tag extent / `mvPx` |
| 格式 | SRV 跟随源格式（今天 CP2077 光追 `RG16F → RGBA16F` 就是这么崩的） |
| 状态 | 拒绝 `UNORDERED_ACCESS(0x8)` 的源（跨队列写入）；`MvFromUav=1` 可强制 |
| 占位 | 90°/0.1/999.9/原点矩阵判为占位；度数/弧度都认 |
| 新鲜度 | 250 ms 无新帧 ⇒ `stale` + `kInputFlagMvSuspect`，消费端回退 |
| 内容 | MV 全 0 而深度在变 ⇒ 判死场（`MV field looks DEAD`） |

### 12.4 落地顺序（按性价比）

1. ~~lane 3（XeFG→XeFG）~~ ✅ **已实现**：`make_xefg_source()`，在 `g_sources` 里排第一（优先级最高）；
2. ~~lane 1 加固~~：格式/UAV/新鲜度已补齐；`validUntilEvaluate` 的 evaluate 内拷贝仍待做；
3. ~~lane 2（FSR-FG）~~ ✅ **已实现**：解析 `ffxDispatchDescFrameGenerationPrepare`，
   走 `make_ffxfg_source()`；
4. **lane 4**：现状，保持。

> 现在的注册顺序就是优先级：`xefg → sl → ffxfg → ngx → ffx → xess`。
> `Input=auto` 仍然"谁先给出活帧就用谁"，配合 v14 的 250 ms 新鲜度门控，
> 游戏中途切换 FG 类型时会在 90 帧内自动重新挑选，不需要重启、不需要游戏名单。

## 13. DLSS-FG（lane 1）验证流程 —— 当前唯一在役的车道

### 13.1 一次完整验证 = 四条检查 + 一次切换

| 步骤 | 看什么 | 通过标准 |
|---|---|---|
| 1. 启动（游戏内 DLSS-SR 开、FG 关） | `fg: Streamline hooks active (N installed)`、`input source: sl (api 5)`、`harvest: ...` | 5/6 装上；`mv/depth/token/clip` 至少 y |
| 2. 进世界 | `camera: using the input source's own view-to-clip`（有 `sl.const` 相机时） | 没有就退 `--fg-fov`/`--fg-zn`（用 SL 声明过的值） |
| 3. 跑 `bridge --den 1 --pool 4 --interp 1` | `producer api=sl/streamline`、`presents=80/80`、`cadence 1.00 vblank` | `gpu`/`present` 在预算内，`timeouts=0` |
| 4. **游戏内切 FG 开/关** | `dlssg.options mode=1 / mode=0`、`fg: the game released its DLSS-G feature`、`input: source ... STALE / live again` | 关的那一侧 ≤250 ms 内 STALE；开回来立刻 `live again` |

第 4 步是 v14 新增的能力，也是"动态"的核心：**FG 开关不再需要重启游戏**。

### 13.2 已知的 lane 1 边界（写在代码里，不是待办）

| 现象 | 处理 |
|---|---|
| `sl.const` 报 `cameraFOV` 用**角度**（CP2077 75/60） | `camera_fov_radians()` 统一换算；占位检测同时认 90° 与 90 度 |
| `mvecScale=(1,1)` 但实际是 UV（CP2077 / PRAGMATA） | 用 NGX 的 `MV.Scale` 侧通道覆盖，并把 `mv_unit` 标成 uv |
| 光追把 MV 换成 `RGBA16F(fmt 10)` | SRV 跟随源格式（否则 typed view 不匹配 ⇒ 设备移除） |
| present 时 MV 仍在 `UNORDERED_ACCESS(0x8)` | 拒绝读取（`MvFromUav=1` 可强制），消费端回退 |
| SL 停止发布（FG 管线被拆） | 250 ms 新鲜度门控 + `kInputFlagMvSuspect` |
| 游戏显式 release DLSS-G feature | `ReleaseFeature` 钩子**立即**清空捕获（本轮新增） |

### 13.3 验收脚本（三款游戏同一套）

```bash
# 1) 看 lane 与覆盖度
grep -E "input source:|data coverage|producer declaration" <game_capture.log> <bridge_*.txt>

# 2) 看动态性（切 FG 之后）
grep -E "STALE|live again|released its DLSS-G|dlssg.options mode=" <game_capture.log>

# 3) 看预算
grep -E "breakdown|out: presents|cadence" <bridge_*.txt>
```

## 14. 车道优先级是**持续**判定，不是选一次（2026-11-10，PBZ 实测暴露）

### 14.1 实测：双向切换都成立（PBZ，14:52–14:53）

```text
[14:52:58] dlssg.options mode=0 ... (frame generation OFF)      input source: ngx (api 1)
[14:53:32] dlssg.options mode=1 framesToGenerate=1 (=2x)        ← 用户在菜单里打开 FG
[14:53:32] sl.const #1 ... fovY=1.3090rad(75.00deg) cameraViewToClip[1.3032 ...]
           sl.tags #1 n=3  DEPTH(0x8E0) / MOTION_VECTORS(0x800) / HUDLESS_COLOR(0x800)
[14:53:39] harvest: mv=y depth=y camera=y clip=y token=n reset=n hudless=y ui=n scaling=-- expose=n
[14:53:39] input source: switching ngx -> sl (a higher-priority lane is live)   ← 升舱
           semantics: mv=unknown sign=prev-cur camera=matrix depth=reversed valid=2562x1441
[14:53:37] dlssg.options mode=0 ...                              ← 用户关回 FG
[14:53:43] input source: ngx (api 1) - pinned for this process   ← SL 静默后自动回落
```

- **升舱**：更高优先级车道一活（`sl.const`/`sl.tags` 每帧都在）就在 120 帧内自动接管；
- **回落**：SL 因 FG 关闭而静默 ⇒ 90 帧后重新挑源，回到 `ngx`；
- 全程**没有重启游戏**，也没有任何按游戏名的规则。

PBZ 特有的两点（都已记录在案，不需要特判）：

1. `token=n`：它不调 `slEvaluateFeature`（FG evaluate 走的是 `nvngx_dlssg`/sm86），
   所以 SL 侧没有 `frame_key`；配对只能靠 QPC 时间戳；
2. `semantics: mv=unknown`：PBZ 的 `mvecScale=(0.000390,0.000694)`（=1/尺寸，像素制）
   走的是"尺度启发式"而不是"CAMERA 声明"，`mv_unit` 留给探针去定。




### 14.2 v14.2：serial 不能只在 `slEvaluateFeature` 里推进（PBZ 暴露）

`SlFgOff=1` 下 PBZ 的日志：

```text
fg: SlFgOff=1 - forcing slDLSSGSetOptions mode=eOff
sl.const #  49 次      sl.tags # 498 次        ← 声明一直在发
input source: ngx (api 1)                        ← lane 1 却是暗的
switching: 0 次
```

根因：`g_sl_cap.serial` 只在 `hook_sl_evaluate` 里 `++`。而 PBZ 的 DLSS-G evaluate 走
`nvngx_dlssg`（sm86），**不经过 `slEvaluateFeature`** ⇒ serial 永远不动 ⇒ `poll()` 永远 false
⇒ 明明每个 tag 都在，lane 1 就是不产帧。

修法（`in_fg.cpp`，sniff_tags 的 v13 capture 块）：

```cpp
// v14.2: tags are per-frame, so they mark a frame too - a title can publish a complete
// declaration and never call slEvaluateFeature (PBZ with our SlFgOff).
if (g_sl_cap.have_constants &&
    (g_sl_cap.harvest & (kHarvestMv | kHarvestDepth)) != 0) {
    g_sl_cap.serial = ++g_sl_cap_serial;
}
```

⇒ "FG 开着但不许出帧"（`SlFgOff=1`）从此也能拿到 lane 1 的完整数据，代价是**没有
`frame_key`**（token 只在 evaluate 里出现），配对退化为 QPC 时间戳。

### 14.3 动态逐帧（`--interp 5 --interp-cadence`）在 PBZ 4K den1 的预算

```text
cadence: rt=29.2ms (auto:quant) input=29.2ms = 3.50 vblanks -> interp=2
         (avg 3.40x, 116.6 fps out = 1.03 vblank/frame)   ← 输出恒定 1 帧/vblank，成立
cadence hist: 3x=24 4x=16 switches=32 refused=0
breakdown: gpu=19.9-27.5ms  present=4.8-14.6ms  cpu=3.2-5.5ms
HITCH gap=49-70ms gpu=25-27.5ms xN=3                 ← 超预算
presents=130-136 expected=120
```

结论：**动态逐帧本身工作正常**（1.03 vblank/frame），但 `interp 5` 在 4K `den 1` 下
`gpu≈25–27ms`、出现 HITCH —— 这个配方是给 `den 2` 设计的。PBZ 上要么 `--den 2`，
要么 `--interp 3`。


## 15. 「FG 关掉就拿不到数据」是误解：声明和出帧是解耦的

这是整个设计最容易被问住的地方。实测答案：**我们要的是"游戏的 FG 管线被初始化并持续发布声明"，
不是"游戏在出帧"**。`mode` 只影响 evaluate（出帧），不影响 `slSetConstants` / `slSetTag`。

| 题 | 游戏内 FG | 声明是否每帧 | `harvest` |
|---|---|---|---|
| Khazan（UE4.27） | **关** | `sl.const` / `sl.tags` 每帧 | mv/depth/hudless |
| Wuchang（UE5） | **从未开**（`dlssg.options` 0 次） | `sl.tags #1` 起每帧 | mv=y depth=y camera=y clip=y **hudless=y** |
| PRAGMATA（RE） | 关 | `sl.const` 每帧（`token=y`） | `hudless=n` |
| PRAGMATA（RE） | 开 | 同上 | `hudless=y ui=y` |

⇒ 结论：

1. **核心数据（MV / 深度 / 相机 / clip）在 FG 关闭时就有**，只要游戏的 FG 插件被加载/初始化过
   （UE 的 StreamlineCore、RE 的 sl.dlss_g 都在启动时就 create 了 feature）；
2. `HUDLESS/UI` 与 `frame_key` 是"更全"的那部分，**有的题关着也给**（Wuchang 给 hudless），
   有的题要开 FG 才给（PRAGMATA）；
3. 因此**正确顺序是"先看数据、再决定要不要动游戏的 FG"**，而不是先把 FG 关掉再找数据。
   add-on 现在会自己在日志里下结论（v14.4）：

```text
harvest: complete with the game's own FG setting - do not touch it
harvest: core is complete (mv/depth/camera/clip), no HUDLESS/UI, no frame token - the game's FG setting does not need changing
harvest: INCOMPLETE (0x9) - this title only publishes the full set while its FG pipeline is initialised...
```

4. 只有 `harvest: INCOMPLETE` 才需要动游戏的 FG；而"动"的两条路都有代价：
   - 游戏内开 FG（管线全活）：多拿 hudless/token，**但游戏也开始出帧**——我们必须在它上面再插一层，
     或者干脆不跑我们的 FG；
   - `SlFgOff=1` 去压：见 §13.2 —— 自己管 present 的题会黑屏、模拟型 FG（sm86/XeSS）压不住。

> 一句话：**"关掉 FG 才能停掉叠加"和"关掉 FG 就拿不到数据"这两句话都不是普适的**；
> 普适的是 harvest 位图 —— 它告诉你这道题现在给了什么，缺的那部分值不值得为它冒黑屏的风险。



PBZ（UE 5.7 + StreamlineCore）上第一次点亮 lane 1 时抓到：

```text
fg: sl.const #1440 vp=1 ver=2 jit=(+0.062,-0.463)px mvS=(0.000390,0.000694) mvPx=2562x1441
    zn=0.0100 zf=75000.0 fovY=1.3090rad(75.00deg) aspect=1.7779
fg: sl.tags #2853 n=3
  DEPTH          state=0x8E0 lc=0(onlyValidNow) extent=(0,0 2562x1441)
  MOTION_VECTORS state=0x800 lc=0(onlyValidNow) extent=(0,0 2562x1441)
  HUDLESS_COLOR  state=0x800 lc=0(onlyValidNow) extent=(0,0 3840x2160)
dlssg.options mode=1 (14:46:08) -> mode=0 (14:46:16)
```

但 `input source:` 仍然是 `ngx (api 1)`：**钉住的低优先级源只要还在出数，就永远不换**。
这正是"动态变化拿不到数据"的一种，所以规则改成：

```text
每 120 帧：若当前源之上还有"活着"的车道（250 ms 内有新帧）⇒ 立刻切过去
          g_sources 的顺序就是优先级：xefg → sl → ffxfg → ngx → ffx → xess
```

日志会明确写一行：

```text
input source: switching ngx -> sl (a higher-priority lane is live)
```

同时把 `ProducerInfo.input_api` 改成**每帧发布**（原来只在初始化时写一次），
这样 bridge 里的 `api=...` 也会跟着变。

> 另外记一笔 PBZ 的 lifecycle 是第三种：`lc=0 (onlyValidNow)` ——
> CP2077 是 `validUntilPresent`、PRAGMATA 是 `validUntilEvaluate`。三款游戏三种，
> 所以"拷贝时机"只能靠 tag 声明判断，不能靠经验。

## 15.1 sm86 不是"另一个插帧器"，它是**让游戏肯创建 DLSS-G feature 的开关**（实测）

同一台机器（RTX 3080 Ti）、同一款游戏（Wuchang，UE5）两次启动的唯一差别是
`dlssg_sm86.ini` 的 `Enabled`：

| sm86 | 游戏内 FG | `sl.const` / `sl.tags` | `input source` |
|---|---|---|---|
| `Enabled=1`（Engine=XeSS） | 关 | **每帧** | `sl (api 5)`，`harvest: mv/depth/camera/clip/hudless 全 y` |
| `Enabled=0` | 关 | **0 次** | `ngx (api 1)`（只剩 lane 4） |

⇒ 在 Ampere 上，游戏是通过 NGX 问"DLSS-G 可不可用"的；没有解锁器它就不创建 DLSS-G feature，
Streamline 也就**没有任何 FG 声明可发**。所以：

- **要 lane 1 的数据，sm86 必须装着并 `Enabled=1`**（它是"可用性"开关）；
- **但游戏内 FG 保持关闭**：此时 sm86 拿不到 evaluate，不会插帧 ⇒ 数据照发、画面不叠加；
- `Engine=DLSS` 还是 `Engine=XeSS` **在 FG 关闭时没有区别**（都不 evaluate）；
  只有你打算让游戏自己出帧时才有意义（DLSS=原生 2x；XeSS=2x–4x，且 HDR RGBA16F 会被拒绝）。

这就是本项目的**推荐配方**：

```text
dlssg_sm86.ini : Enabled=1            (enabler，缺了 lane 1 就是暗的)
game_capture.ini: SlInput=1 SlFgOff=0 Claim=0
游戏内          : DLSS-SR 开、DLSS Frame Generation 关
→ 数据：sl.const / sl.tags 每帧（含 HUDLESS）
→ 画面：游戏不出帧，只有我们的 FG
```


## 15.2 同一款游戏：`Engine=DLSS` vs `Engine=XeSS` 拿到的数据**不同**（Wuchang 实测）

Wuchang 是 UE5 + **HDR10**（`SetColorSpace1(12)`）。同一台机器、同一份 ini、游戏内 FG 都关，
只改 `dlssg_sm86.ini` 的 `Engine`：

| | `Engine=DLSS`（15:40） | `Engine=XeSS`（22:14） |
|---|---|---|
| `sl.const` / `sl.tags` | **每帧**（`#1` 起持续） | **0 次** |
| `harvest` | `mv=y depth=y camera=y clip=y hudless=y token=n` | 无（没有 SL 输入） |
| `input source` | `sl (api 5)` | `ngx (api 1)`（只剩 lane 4） |
| `dlssg.options` | 从未出现（游戏内 FG 关） | 从未出现 |
| 结论 | 游戏的 DLSS-G feature **被创建**，声明齐全 | 游戏的 DLSS-G **不可用**，没有声明可发 |

原因写在 sm86 自己的 README 里：

> An RGBA16F (HDR) back buffer is refused (`xefg_feature_refused`), it does not switch to DLSS-G.

⇒ **HDR 题上 `Engine=XeSS` 会让 DLSS-G 直接不可用**：游戏问"支持吗"得到否，
就不创建 feature，Streamline 也就没有 `slSetConstants`/`slSetTag` 可调。
`ngx: sr` 仍在出数（DLSS-SR 在跑），说明不是"还没进世界"，而是 FG 这一路根本没建立。

**所以 sm86 的 Engine 选择原则**：

| 目标 | 选择 |
|---|---|
| 只要 lane 1 的数据（推荐） | `Engine=DLSS` —— 原生路径，HDR 也成立；游戏内 FG 关着就不出帧 |
| 想让游戏自己出 3x/4x 帧 | `Engine=XeSS` + `MaxGeneratedFrames=3`，**但 HDR 题会被拒绝**（这条题上不可用） |

## 15.4 黑屏的真凶：我们的 NGX 钩子和 sm86 解锁器抢同一段 prologue

15.3 的隔离测试（同一台机器、同一份游戏内设置、只改 `game_capture.ini`）：

| 配置 | 游戏自己的 FG | 我们的 `sl` 数据 | 画面 |
|---|---|---|---|
| `FgSniff=1 SlInput=1`（NGX 钩子**装**） | feature 建了、每帧 evaluate，但出不了帧 | 完整 | **全黑** |
| `FgSniff=0 SlInput=0`（我们全不装） | 正常 2x | 无 | 正常 |
| `FgSniff=1 SlInput=1 FgNgxHooks=0`（只装 Streamline 钩子） | 正常 2x | **完整** | **正常** |

机理：`dlssg_sm86` 的 install 记录写着它钩在 `nvngx_dlssg.dll` **内部**——

```json
"event":"install","hooks":{"evaluate":["0x370c0","0x376aa"],"populate":["0x166d0","0x16b11"], ...}
"event":"runtime_redirect","mode":"bundled","game_runtime_used":false
"event":"kernel_pack","image":"cubin_sm86","kernels":71,"kernels_matched":71
```

Ampere 上只有它那份**重编译过的** DLSS-G 才会出帧。我们再去 MinHook 同一个模块的
`DLSSG_NVSDK_NGX_D3D12_EvaluateFeature`，两套内联钩子写同一段 prologue，结果就是
"我们的 `dlssg.eval` 照常打印、它的帧生成废掉" —— 画面全黑。

⇒ **装了 sm86 之类解锁器的游戏：`FgSniff=1` + `FgNgxHooks=0`**。
`sl` 车道不需要 NGX 钩子（`slSetConstants`/`slSetTag` 给的是同样的数字），
少一份 ngx 遥测，换数据完整 + 画面正常。

该配置下 `verify_pipeline.py` 明末 **9/9 PASS**，bridge 侧 `2.00x`、`142.2 fps @120Hz`、
`presents=80/expected=80`、`drops=0`。

## 15.3 明末（Wuchang）第三次实测：`sl` 只在**游戏内 FG 打开时**出现，而打开后画面全黑

2026-11-10 22:25–22:35，同一份安装（add-on `e0f5836c`、`Enabled=1`、`Engine=DLSS`、
`SlInput=1 SlFgOff=0 Claim=0`）、HDR10 开着，只改**游戏内**的 DLSS Frame Generation：

| | 游戏内 FG **关**（22:25） | 游戏内 FG **开**（22:29 起） |
|---|---|---|
| `sl.const` / `sl.tags` | **0 次** | 每帧（`sl.const` 每 ~21 ms 一条） |
| `dlssg.eval` / `dlssg.ngx` | 0 | 每帧（`hdr=1 mf=1/1(max=3)`） |
| `input source` | `ngx (api 1)` | `switching ngx -> sl` ⇒ `sl (api 5)` |
| `harvest` | 只有 ngx（`mv=pixels camera=none`） | `mv=y depth=y camera=y clip=y token=n hudless=y` |
| 画面 | 正常 | **黑屏**（游戏仍在跑，桥仍以 2.00x 出帧） |

⇒ 修正 §15.1 的一句话：**这条题上"FG 关着也有 `sl` 声明"不成立**。15:40 那次能拿到 `sl`，
是因为当时游戏内 FG 是开着的（15:36 的 `SlFgOff=1` 正是为了压掉它）。所以：

```text
明末：sl 声明 ⇔ 游戏内 FG 打开（feature 被创建才有 slSetConstants）
```

**黑屏不是我们的管线**，证据：

1. 我们这边全部透传 —— `FgSuppressNgx=0`，`SlFgOff=0`，`dlssg.evaluate` 链式调用原函数；
2. bridge 在对侧照常工作：`presented=2 fg=1 SUCCESS`、`2.00x`、`116.4 fps @120Hz`；
3. `verify_pipeline.py` 对这次会话 **9/9 PASS**（`sl/streamline`、`0x3191`、2558x1438、
   uv、`inverted_depth=1`、`mvscale≈(2558,1439)`、`frame-key` 无、`token=n`）；
4. 同一个黑屏在 15:36 用 `SlFgOff=1` 也复现过 —— 说明这条题的**画面只经它的 FG swapchain 出**：
   一旦那一路不产出内容（我们压掉、或 shim 在 Ampere+HDR 上产不出），屏幕就是黑的。

⇒ 想要"画面正常 + `sl` 数据"目前无解；二者择一：

| 目标 | 配置 | 代价 |
|---|---|---|
| 画面正常 + 数据 | 游戏内 FG **关** | 这条题只剩 `ngx` 车道（像素 MV、无相机、无 clip） |
| `sl` 全量数据 | 游戏内 FG **开** | 屏幕黑（shim 的 DLSS-G 输出在 HDR10 上不成立） |

下一步排查黑屏：已把 `dlssg_sm86.ini` 的 `[Logging] Level` 提到 `2`（配置与能力），
下次启动会记录 shim 到底选了哪个引擎、有没有 `xefg_feature_refused`；再试**关 HDR** 后
FG 开是否恢复画面 —— 若恢复，则黑屏归因于 HDR RGBA16F 被拒，而不是 Ampere 本身。

## 22. FSR-FG 代理后端 v1 验收通过（CP2077，RTX 3080 Ti，2026-11-11）

### 结论

```text
我们的 amd_fidelityfx_framegeneration_dx12.dll（14 KB 代理）
   configure(FG effect) 吞掉   → 真运行库不接管 swapchain → 游戏按 1x 呈现
   dispatch(FRAMEGENERATION) 回 OK 不转发 → 一帧都不生成
   dispatch(PREPARE) 转发+记录 → 声明数据完整
bridge: input=25.5ms(=1x, ~39fps) → avg 2.00x → 78.4 fps out @120Hz, drops=0, presents=80/80
verify_pipeline.py: 9/9 PASS（api=ffx/fsr-fg、uv、depth=forward 一致、stream 2560x1440、frame-key）
```

也就是说：**游戏只出真实帧，插帧由第二张 GPU 完成** —— 而且**不经过颜色通路**（WGC 抓到的就是干净的 1x）。
这是本项目第一个"可以对外声称"的 FG 后端实现。

### 实现要点（`src/fg_stubs/fsr/framegen_stub.cpp`）

```text
· 不是空壳，是代理：DllMain 里 LoadLibrary(<同名>.orig)，把 5 个 ffx 导出全部转发
  （ffx-api 的加载器把所有 effect 都交给它加载的那个 provider，空壳会连带弄坏超分）
· 只过滤两类调用：
    configure  : (type & 0xFFFF0000) == FRAMEGENERATION effect  → 全部吞掉
    dispatch   : PREPARE(0x20004) 转发并记录；FRAMEGENERATION(0x20003) 回 OK 不转发
· 日志：bin/x64/fsrfg_stub_mgpu.log（proxy/configure/prepare 三条关键行）
```

### 三个必须记住的坑（每一个都对应一次崩溃）

| 现象 | 原因 | 修法 |
|---|---|---|
| `DEP at 0x0` | `ffxQuery` 回 OK 却没填输出 → 游戏调了空回调 | 未实现的类型一律回 `UNKNOWN_DESCTYPE` |
| `DEP at 0x0`（修复后） | 空壳拒绝了 **upscale** 上下文，超分被我们抢走 | 改成代理：全部转发给 `.orig` |
| `read at 0x28` | 只吞了 `0x20002`，其它 FG configure 仍转发给"没有 swapchain"的真运行库 | 按 **effect 号**吞掉全部 FG configure |

### 同时修好的两条数据语义（影响插帧质量）

```text
① mv_unit：FSR 的 motionVectorScale = 渲染尺寸(3840,2160) ⇒ 向量是 UV 空间
   （之前标成 pixels，发布出去的 scale 会被再乘一次分辨率）
② 相机平面：CP2077 在 FG prepare 里给 near=16000 / far=0（反向），
   检测到反向就交换，不再判成"没有相机"
   → 现在 semantics: mv=uv sign=prev-cur camera=declared depth=forward
```

## 25. 复盘：ReShade 当宿主时，链路到底是怎么成立的（2026-11-11）

自建宿主踩过的坑（加载期卡死、设备被移除）全部可以在这份复盘里找到对应，所以先把它写清楚。

### 25.1 注入与生命周期

```text
ReShade 以 dxgi.dll 代理形式放进游戏目录
  → 游戏加载 dxgi.dll 时 ReShade 进入进程
  → 它在初始化期（设备创建路径里）做全部重活：建自己的 immediate command list、
    扫描模块、装钩子、编译 effect
  → 之后从 game_capture.addon64 加载我们的 add-on，调用 AddonInit
我们的 add-on：register_addon → 注册 5 个事件 → 之后只做"记录 + 提交"
```

**第一条分工**：重活在初始化期做完；每帧只做轻量工作。

### 25.2 我们注册的 5 个事件各给什么

| 事件 | ReShade 给的 | 我们用它做什么 |
|---|---|---|
| `init_effect_runtime` | `effect_runtime*`（device / queue / 截图尺寸） | 记下设备与队列指针 |
| `init_swapchain` | swapchain + resize 标志 | resize 后暂停采集（资源全部重建） |
| `barrier` | `resource[] + 旧 usage[] + 新 usage[]` | 维护"被观察资源"的当前状态表 |
| `reshade_begin_effects` | 游戏那条 command list + rtv | **每帧唯一的工作点**：claim 槽位、记录拷贝 |
| `reshade_present` | `effect_runtime*` | fence Signal + 发布（写共享内存帧记录） |

### 25.3 数据本身不来自 ReShade

ReShade 只给"设备/命令表/barrier/每帧时机"这四类**基础设施**；**上采样器的参数是我们自己钩出来的**：

```text
sl.interposer.dll        slSetConstants / slSetTag（9 个钩子）→ MV/pixels、depth、camera、jitter
nvngx_dlss.dll / _nvngx  NVSDK_NGX_D3D12_EvaluateFeature     → MV/depth 指针 + 尺寸 + 语义
amd_fidelityfx_dx12.dll  ffxDispatch(PREPARE)                → jitter、mvScale、相机、depth
libxess_fg.dll           xefgSwapChainTagFrameConstants/…    → view/proj、jitter、MV scale
```

所以"无 ReShade"要复刻的**只有基础设施**，数据侧一行没变。

### 25.4 每帧真实时序（ReShade 宿主）

```text
游戏: 录制本帧 → ExecuteCommandLists → Present
                                   ↓
ReShade: 在 Present 路径里调用 reshade_begin_effects(游戏 list, rtv)
            ↓ 我们：
            fetch_inputs()            取最近一帧的上采样器数据
            ensure_shared(device)     建 253 MB 共享段（首次）
            claim_publish_slot()      等消费者 ACK 的槽位
            记录 barrier + 拷贝       全部记在【游戏那条 list】上
         ReShade 继续画它的效果 → 真 Present
         reshade_present → 我们在游戏队列上 Signal，然后写共享内存帧记录
```

### 25.5 它为什么稳（三条纪律）

```text
① 触发点在"提交之外"：begin_effects 是在 Present 路径里跑的，此时游戏的
   ExecuteCommandLists 已经返回 —— 我们的 253 MB 分配、槽位等待都不在提交调用栈里
② barrier 状态是完整观测：ReShade 包装了整个命令表接口，ResourceBarrier 与
   enhanced Barrier 都经过它，所以它给我们的 old/new state 永远是真的
③ 不在 loader 锁内装钩子：模块扫描与装钩子全部在初始化期完成，loader 通知只入队
```

### 25.6 自建宿主对照表（这次踩的坑就是这三条的镜像）

| 纪律 | ReShade | 我们（自建宿主）走过的弯路 | 现在的状态 |
|---|---|---|---|
| ① 触发点 | Present 路径（提交之外） | 放在 `ExecuteCommandLists` 里 → 加载期卡死 | 已对齐：加载门 + 分配搬到宿主定时线程 |
| ② 状态跟踪 | 包装接口，两条 barrier 全覆盖 | 先只有 ResourceBarrier，后用 hint 兜底 → 设备被移除 | 已包装 list + 只信观察到的状态 |
| ③ loader 锁 | 只在初始化期装钩子 | loader 钩子里同步 `on_module()` → 渲染线程 120 s 超时 | 已改回"入队 + 5 ms 定时消费" |

结论：**自建宿主不是"能不能"的问题，而是必须逐条复刻这三条纪律**。目前 ②③ 已对齐，
① 只做了一半（加载期让路，但重活仍在提交栈内），这就是下一步要改的：

```text
把 ensure_shared() 从 frame_tick 搬到宿主的定时线程：
    首帧只置"请求"标志 → 定时线程负责分配 253 MB 并发布 ready
    frame_tick 里只保留"读 ready 标志 + 记录拷贝"，零分配
```

### 25.7 复盘结论：第三个坑是一个越界转发（2026-11-11 收尾）

对齐三条纪律之后，加载期仍然卡死，日志给出了确切位置：

```text
wrap: ExecuteCommandLists #222000 count=3 ... publish done
wrap: ExecuteCommandLists FAULTED (code=0xC0000005)      ← 驱动里 AV，两个线程各一次
```

原因是转发缓冲写死成 8 项：

```cpp
ID3D12CommandList* unwrapped[8]{};
if (count <= 8) { 解包; to_submit = unwrapped; }
// count > 8 时 to_submit 仍是【包装对象】→ 驱动按自己的结构体解引用 → AV
```

加载期 UE 一次提交超过 8 条命令表，正好踩中，于是渲染线程死掉、加载停住。这一条与拷贝、
与分配全无关 —— 这也是"关掉拷贝照样卡"的真正原因。

修法：任意 count 都用 `_alloca` 解包（无固定上限、无堆分配）；SEH 日志带出错地址
（`GetExceptionInformation()` 只能在过滤表达式里取，改成 filter/handler 分离）。

修后实测（无 ReShade，`version.dll` + 自有宿主，`SlInput=1 SkipDepth=1`）：

```text
0 次 FAULTED，174900 次提交，4680 帧发布，0 次 slow frame_tick
shared ready: used=253.6 MB slots=4（由宿主定时线程在加载期建好）
perf: begin 约 0.002 ms/帧，barrier 1.4 ms 累计 / 35151 次调用，present 0.1 ms
进入世界并长时间稳定
```

## 24. 性能基线（2026-11-11，先量后改）

### 实测：我们每帧花多少

数据来自明末一次 11 分钟会话的 `perf:` 行（每 120 帧汇总一次，数值是**窗口合计**）：

```text
perf: frames=12480 begin=11.3ms barrier=3.4ms (78326 calls / 273952 items) present=2.0ms
                                   ↓ ÷120 帧
每帧：begin 0.094ms + barrier 0.028ms + present 0.017ms ≈ 0.14ms
一帧输入 25ms（≈40fps）⇒ 我们占 0.55%
```

```text
barrier 78326/120 = 653 次/帧、273952/120 = 2283 项/帧  ← 不是问题：只做定长表比对，无锁竞争
热路径里没有一次堆分配（begin_effects 全程 0 处 std::string / vector / new）
```

### 实测：bridge 每帧花多少

```text
breakdown: capture=0.0 work=0.7 present=11.3 gpu=2.4 age=6.3 total=13.5ms   input=23.4ms
LATENCY:   compose->present 19.8ms (game->wgc 6.3 + bridge 13.5)
```

13.5ms < 23.4ms ⇒ **每帧还有 ~10ms 余量**；`present=11.3ms` 是等 dblock/vblank 的阻塞时间，
不占 CPU。也就是这条链路不是瓶颈，游戏自己渲染才是（40fps）。

### 这一轮真正改掉的东西

```text
① 每帧一次 ini 文件打开（唯一非免费的项）
   in_fg.cpp hook_dlssg_evaluate 里每次 evaluate 都调 config_int("FgSuppressNgx")
   → config_int 会 resolve_ini_path + GetPrivateProfileIntA（开文件、解析）
   → 改为 config_int_once()（定长表 + SRWLOCK 记忆化），并在该钩子里缓存成 static
② "每个源各自记忆化"收敛为宿主统一一份（inputs.h: config_int_once）
   四个源里 12 处 config_int 全部换到记忆化版本，后续新增钩子不会再踩同一个坑
③ 退役产物移出构建：retired/dlssg（DLSS-G 文件替换，签名门，永不可用）、
   version_proxy/legacy/version.cpp（1000 行的第一版，四个子系统都造成过崩溃）
   ⇒ build.bat 现在只编 FSR + XeSS-FG；build_proxy.sh 不再 stage 无用源文件
```

### 还剩的三个性能杠杆（都带代价，没动）

| 杠杆 | 收益 | 代价 / 现状 |
|---|---|---|
| 颜色走共享流（`Color=1` + `--fg-colour shared`） | 砍掉 `game->wgc 6.3ms`，延迟 19.8 → ~13ms | §16 的 XeFG `Failed to allocate memory for a copy of HUDLESS_COLOR` 未解决 |
| 深度总线（14 MB/帧 ≈ 0.6 GB/s） | 减带宽/减显存流量 | 只能靠 `--no-depth` 或降采样，属画质取舍 |
| 池深/时序 | 已是最优 | `--slack 0` 已经是"排空并只留最新帧"，池深不产生延迟 |

结论：**游戏侧没有可榨的性能（0.55%），bridge 侧有余量**。真要再降延迟，唯一的路是
把颜色从 WGC 换到共享流，而那要先修 §16 的 XeFG 分配错误 —— 不是靠调参能绕过的。

## 23. XeSS-FG 代理 v1：三个后端全部落地（2026-11-11）

### 结构（第三个后端，与前两个同一套规则）

```text
src/fg_stubs/xess/xefg_stub.cpp      31 个导出（= libxess_fg.dll v1.3.1 的全集）
  CreateContext            -> 给游戏一个"我们自己的"句柄（同时尝试建一个真上下文，只用于查询）
  InitFromSwapChain(Desc)  -> 接受、记录 initFlags/maxInterp，**不转发**（不接管 swapchain）
  TagFrameConstants        -> 记录 view/proj 矩阵、jitter、MV scale、reset、frame time
  D3D12TagFrameResource    -> 记录 HUDLESS/DEPTH/MV/UI/BACKBUFFER 的指针与 extent
  SetPresentId/SetEnabled/SetNumInterpolatedFrames/... -> 记录 + 成功
  GetLastPresentStatus     -> {framesPresented=1, frameGenResult=0, isFrameGenEnabled}
  AIL 三件                 -> 转发给 .orig
libxess_fg.ini  [stub] ForwardInit=0/1   （1 = 转发接管，作为对照组 A）
```

### 为什么是"provider"而不是"透明代理"

FSR 那边能透明转发，是因为 `ffxConfigure(FG)` 与 `ffxDispatch(PREPARE)` 是分开的：
吞掉 configure（接管）仍能让 PREPARE 走真库。XeFG 没有这个分界 —— 声明
（`TagFrameConstants`/`TagFrameResource`）必须落在一个已接管的 swapchain 上才有效。
所以这里换成 provider：句柄归我们，声明进我们的日志与 add-on，接管永不发生 ⇒ 游戏自己
present ⇒ 1x。§21 的"节拍住在 FG 的 swapchain 里"在这一版是**构造上**成立的，不是靠实验。

### 自检（Windows，2026-11-11 04:33）

```text
LoadLibraryW(无 .orig 的孤立副本) -> handle 非 0
31 个导出 GetProcAddress 全部非 0（抽查 6 个关键项）
xefg_stub_mgpu.log:
  proxy: real runtime NOT found (missing .orig?) - queries will be synthesized
  xess-fg stub loaded (provider: records declarations, never takes the swapchain over; ForwardInit=0)
版本资源: FileVersion 1.3.1.0 / ProductName "Intel XeSS Frame Generation"
```

### API 级冒烟测试（没有 XeFG 场景时的替代验证，同日 04:39）

原生支持 XeFG 的游戏太少（OptiScaler 也只走 FSR / DLSS-G 两条路），所以把"加载与调用
不报错"做成可复现的用例：`tests/xefg_smoke/xefg_smoke.cpp` 按集成的真实顺序走一遍
（`scripts/build_xefg_smoke.sh` 编译）。

```text
GetVersion -> D3D12CreateContext -> GetProperties -> D3D12GetProperties
-> GetInitializationParameters -> InitFromSwapChainDesc -> BuildPipelines
-> SetDescriptorHeap -> {TagFrameConstants, SetPresentId, D3D12TagFrameResource x3,
   SetNumInterpolatedFrames, GetLastPresentStatus} x N -> Destroy
```

| 被测 DLL | 结果 |
|---|---|
| 真库 `libxess_fg.dll` 1.3.1（基线，证明用例有效） | `Destroy` 之外多处 `r=-3/-4`（没有真队列/swapchain 时按调用顺序拒绝） |
| 我们的 stub（20 KB，`.orig` = 真库） | **0 unexpected，0 missing export**，`status: presented=1 result=0 enabled=1` |

stub 侧日志（真实记录，`_orig` 上下文在 3080 Ti 上也能建出来）：

```text
createContext: handle=… real=…（真上下文只为查询）
init(desc): 3840x2160 fmt=10 flags=0x1 -> accepted, takeover NOT forwarded
const#1: present=1000 jitter=(0.2500, -0.3750) mvScale=(2558.0, 1438.0) reset=1 dt=23.00ms
res: DEPTH   extent=2558x1438 | res: MV extent=2558x1438 | res: HUDLESS extent=3840x2160
destroy: handle=…（const calls=2, resource calls=6）
```

⇒ 编解码路径、句柄生命周期、记录逻辑都通；只差一个真游戏来喂真实的指针与矩阵。

### 部署（换掉游戏目录里那一份）

```text
<game>\libxess_fg.dll        <- build/libxess_fg.dll（我们的，20 KB）
<game>\libxess_fg.dll.orig   <- 原版（22.9 MB，从 third_party/xess/bin 或游戏备份）
<game>\libxess_fg.ini        <- [stub] ForwardInit=0
add-on: FgSniff=1（它在进程内也钩 libxess_fg，名字含 xess_fg 即可，两条数据路径互补）
```

验收看三行：`init: ... takeover NOT forwarded`、`const#1: jitter=… mvScale=…`、
`res: DEPTH/MV/HUDLESS ... extent=…`；bridge 侧应看到 `api=xefg`（或 add-on 的 xefg 车道）
且 `input≈1x`。

## 22. DLSS-G 不能替换文件：签名门（2026-11-11，路线改成 sm86 式"接管加载 + 内存补丁"）

### 事实

把 `Engine/Plugins/Runtime/Nvidia/Streamline/Binaries/ThirdParty/Win64/nvngx_dlssg.dll`
换成我方 23 KB 代理（含 `VS_VERSIONINFO 310.9.16.0`、53 个导出、全部转发到 `.orig`）后：

```text
游戏内 打开帧生成提示 "硬件不支持"
Engine/.../dlssg_stub_mgpu.log     不存在        ← DllMain 从未执行
同一 DLL 独立测试 LoadLibraryW     handle≠0      ← 代码与导出都没问题
同位置换回原版 310.2.1             FG 可开        ← 差别只在"这份二进制"
```

签名核对（`Get-AuthenticodeSignature`）：

```text
Valid      v310,9,16,0   dlssg_sm86\runtime\nvngx_dlssg.dll     ← sm86 随包的运行库
NotSigned  v310.9.16.0   我方 fg_stubs\nvngx_dlssg.dll
Valid      v310.2.1      Engine\...\Win64\nvngx_dlssg.dll       ← 游戏自带
```

⇒ **Streamline/NGX 只接受签名有效的 `nvngx_dlssg.dll`**，在 `LoadLibrary` 之前就否决，
所以任何"手写代理放进游戏目录"的方案都不会被执行（FSR-FG 不受此限：`amd_fidelityfx_framegeneration_dx12.dll`
由游戏自己 `LoadLibrary`，没有签名门，代理 v1 一次通过）。

### sm86 的做法（`third_party/dlssg_for_sm86-main/docs/ARCHITECTURE.md` 75 / 613）

```text
· 它从不替换游戏文件；随包文件是 "未修改的原版运行库"（签名完好）
· VERSION.dll 代理钩住 LoadLibrary：请求 nvngx_dlssg.dll 时把它自己那份（原版）映射进进程并返回句柄
  2026-10-19 起失败就返回 ERROR_DLL_INIT_FAILED，绝不再把游戏自带的那份放过去
· 真正的改动全部发生在映射后的镜像上：Detours 函数钩子 + 运行库 RVA 常量（`profile.h`）
  ARCHITECTURE 633 的 hookMinimum() 就是改 `mov eax,0x190; ret` 的立即数 —— 与我们最小代理的 ② 同一件事
```

### 我方定下的结构（三个后端统一）

```text
① 文件不动（保持签名）
② FG 可用性：最小 version.dll 代理（arch 答复 + 运行库最低架构常量）
      —— 已实测：明末 / CP2077 的 FG 选项出现且可开（§20）
③ 生成短路 / 数据收割：进程内打补丁（add-on 的 in_ngx / in_ffx / in_sl），不碰文件
④ 帧由第二张 GPU 出（bridge），游戏侧保持 1x
```

结论：`src/fg_stubs/dlssg/` 的**文件替换式代理在这条路上作废**（源码保留作记录，同 `version.cpp`），
它只在"游戏自己 LoadLibrary 且无签名校验"的宿主里才可能有效；XeSS-FG（`libxess_fg.dll`）
属于后一类，模板照 FSR 那份继续。

### 实测：我们的"内存后端"本来就在，且已跑通（明末，同日 04:09）

纯自有配置（`version.dll` = 最小代理，`nvngx_dlssg.dll` = **原版 310.2.1 未替换**，`dlssg_sm86 Enabled=0`）：

```text
[04:09:11] fg: DLSSG hooks active (4 installed, 4 not exported)     ← 在已映射的原版上装钩子
[04:10:11] harvest: core is complete (mv/depth/camera/clip)          ← sl 车道完整
[04:10:21] camera: using the game's own projection matrix (Streamline constants)
游戏内 DLSS Frame Generation 可打开；无 error/crash；进程正常退出
```

`fg: DLSSG hooks active` 来自 `in_fg.cpp::install_dlssg_hooks()`（create / evaluate / requirements /
capability / release 五个钩子，`FgProvide` 与 `FgSuppressNgx` 都挂在这条链上）——
**这就是 sm86 后端的等价物，我们早就有，不需要新开关**（我曾加过一个 `FgHookDlssg`，
实测日志里 provider 已是 `_nvngx.dll`，那段分支根本不会执行，已删除）。

本次会话三条数据观察（记下来备查）：

```text
· 没有 dlssg.eval#1：游戏把 FG 打开了、Streamline 也交了 sl 常量与 tag，但运行库从未被 evaluate
  —— 与"Ampere 上 310.2.1 不可能出帧"一致，游戏侧保持 1x（这正是方案 A 要的形态）
· tag 分辨率不统一：MOTION_VECTORS / DEPTH 在 2558x1439（渲染尺寸），
  HUDLESS_COLOR 在 3840x2160（输出尺寸）—— 导出 HUDless 时必须按各自 extent 取样
· semantics: mv=pixels（明末在 sl 车道上报像素），camera 在 FG 打开后才由 sl 常量给出
```

### 同一夜的端到端验收（bridge，04:15）

```text
bridge: --den 1 --pool 4 --interp 1 --fg-proj-rz --fg-rt-mode quant（颜色走 WGC，不经过我们的流）
input 18.7..33.9ms → avg 2.00x → 77..101 fps（58/58 窗口）drops=0 presents=80/80
verify_pipeline.py: 9/9 PASS
   api sl/streamline | flags 0x3191 | render 2558x1438 | output 3840x2160
   mv_unit pixels | depth reversed | mv_scale (2558, 1438) | stream 2558x1438 | pairing token=n
```

⇒ "自己开 FG 可用性 + 原版运行库不出帧 + 第二张 GPU 出帧"的纯自有链路闭合，
不需要 sm86，也不需要任何被替换的文件。

## 21. 实验结论：短路"生成"无法改变节拍（三个后端都一样）（2026-11-11）

### 下一步

同一模板复制到另外两个后端：

```text
XeSS-FG → libxess_fg.dll 代理：吞 xefgSwapChainPresent 的插值，转发其余
DLSS-G  → nvngx_dlssg.dll 代理：吞 FG swapchain（sl.dlss_g）与 evaluate 的生成部分
两者都用 "转发到 .orig，只过滤 FG 生成/接管" 的结构，不接管 swapchain ⇒ 天然 1x。
```


### 用 FSR-FG 做的对照实验（CP2077，RTX 3080 Ti）

FSR-FG 与显卡无关（30 系上真出帧），所以它是干净的对照组。打开 `FgCut=1`
（短路 `ffxDispatchDescFrameGeneration`，PREPARE 照常透传）：

```text
[03:10:27] ffx: FgCut&1 - generation dispatch short-circuited
结果：帧数仍是 FG 后的值 + 画面上下黑色条带闪烁
```

原因与 DLSS-G 完全相同：**节拍住在 FG 自己的 swapchain 里**（FSR-FG 是
`ffxConfigureDescFrameGeneration` 把 swapchain 交给 FFX 包装；DLSS-G 是 `sl.dlss_g`
自己的 swapchain）。短路生成只让"该填内容的那一帧"变空，present 数量不变 ⇒ 一半是空帧。

### 因此三条后端只有两种可行架构

| 方案 | 游戏 FG | 数据来源 | 颜色来源 | 是否需要颜色通路 |
|---|---|---|---|---|
| **A（推荐）** | **关**（不接管 present） | 上采样器车道：`ngx` / `ffx` / `xess`（MV/depth/jitter/MV scale/camera） | WGC 抓游戏窗口（干净 1x） | ❌ 不需要 |
| **B** | 开（拿到 FG 契约数据：token/reset/clip/HUDLESS） | FG 车道：`sl` / `ffxfg` / `xefg` | **我们的共享流**（`Color=1`） | ✅ 需要 |

A 已经多次验证（PBZ 2x/116fps、CP2077、Khazan、明末 9/9），且**不经过颜色通路**；
B 的价值只在于 FG 契约里那几个额外字段，代价是必须修好 §16 里记录的 XeFG 共享颜色分配问题。

### 我方的定位（明确下来）

```text
"游戏 FG 开 + 我们短路" 不是我们的方案：它必然留下空帧。
我们要的是：
   游戏侧 FG 关（呈现由游戏自己做，1x）
   + 数据从上采样器车道取（通用，任何厂商/任何卡）
   + 插帧在第二张 GPU 上由我们完成
FG 契约数据（token/reset/clip）只在需要更高质量判定时，用方案 B 作为可选增强。
```

这也顺带解释了为什么"我们替游戏打开 FG"这件事只在方案 B 里有意义：
它换来的是契约数据，而不是更好的输出。

### 代码状态

- `[capture] FgCut` 位掩码保留（1 = FSR-FG，2 = Streamline DLSS-G，4 = NGX DLSS-G，7 = all），
  默认 0。它的作用**限于内容**，不改变节拍 —— 实验价值大于实用价值；
- FSR-FG 车道的相机合并已实现（从同一 ffx 会话的 upscaler 描述符借 near/far/vfov），
  但 CP2077 的 upscaler 描述符里这些字段也是 0，故 `camera=none` 是诚实的结论；
- `valid_w/h` 已改为发布"实际取样纹理"的尺寸，而不是描述符声明的 renderSize。


### 结果

**明末**（`dlssg_sm86 Enabled=0`，只用我们的 `version.dll`）：游戏内 **DLSS Frame Generation 可以打开**，
同时 add-on 的 `sl` 车道数据完整：

```text
[02:31:26] arch: physical architecture is 0x170
[02:31:26] arch: answering 0x90 instead of 0x170（sl.common 在 slInit 里比较的值）
[02:32:45] harvest: mv=y depth=y camera=y clip=y token=n reset=n hudless=y
[02:32:45] input source: switching ngx -> sl
```

**CP2077** 同样：正常启动 + 出现 `NVIDIA DLSS Frame Generation`（此前在"全功能代理"下崩了三次）。

### 只保留两件事（其余全部删除）

```text
① 架构答复：NvAPI_GPU_GetArchInfo → 0x190（物理 0x170 实读）
     不这样做，sl.common 在 slInit 里就把 sl.dlss_g 卸掉，游戏连选项都不给。
② 运行库最低架构：内存里把 `mov eax,0x190; ret` 的立即数改成物理架构
     这是 sm86 的 "coreArch"（ARCHITECTURE.md 633/637）：NGX 核心在 slInit 内拿它和
     物理架构比，不符就记 FeatureInitResult=0xbad0000b，Streamline 不再 CreateFeature。
     外部回答能力参数救不了这一步 —— 只有改常量本身有效。
```

删掉的四个子系统**全部是猜测，且都造成过故障**：

| 删掉的东西 | 故障 |
|---|---|
| NGX 能力/需求/Init 三钩子 | CP2077 崩溃（`0x400`），且并非必需 |
| 版本号改写 | 版本块越界写 → 堆损坏 → 崩溃 |
| DXGI 适配器 vtable | 首版截断 vtable → fatal error；虽有日志证明游戏读了适配器，但它不是闸门 |
| `GetProcAddress` 钩子 | loader lock 死锁 → 进程卡住 |

### 实现要点（`src/version_proxy/version_min.cpp`，300 行）

```text
· 只包装 1 个 id：0xD8265D24（本机真实 GetArchInfo id）/ 0x2D3A1A3A（公开值）
· 触发点：预加载 nvapi64.dll（系统 DLL，安全）；nvngx_dlssg 出现后扫一次并打补丁
· 扫描限制：可执行节内、立即数只认 0x150–0x1FF、绝不动自己的目标值
· 日志加锁；`mgpu_proxy.ini` 两个开关：ArchSpoof / MinArchPatch（默认都开）
```

## 20. 最小代理成功：用自己的 300 行复刻出 sm86 的核心机制（2026-11-11）

## 19. 决策记录：一度把 FG 可用性交回 sm86（2026-11-11，后来被 §20 推翻）

### 全链路闭合（同一晚，明末）

```text
代理   arch: answering 0x90 instead of 0x170          ← 我们让游戏肯开 FG（sm86 Enabled=0）
add-on input source: sl   harvest: mv depth camera clip 全 y
bridge cadence: input=22.9ms(=1x 真实帧) → avg 2.00x → 87.3 fps out @120Hz  drops=0
       out: presents=80 expected=80
verify 9/9 PASS（api/flags/render/output/mv_unit/depth/mv_scale/stream/pairing）
```

也就是"**我们打开 FG → 游戏按 1x 输出真实帧 → 数据完整交给 bridge → 另一张卡出 2x**"这条主线首次完整成立。
关键副作用：因为 Ampere 上运行库出不了插值帧，游戏输出天然是干净的 1x，**颜色继续走 WGC 即可**，
不需要 §16 那条（仍带 XeFG 分配失败问题的）共享颜色通道。

### 下一步

FG 已经由我们自己"打开"，接下来回到主线，不需要再碰兼容层：

```text
FG 开（我们打开） → add-on 采数据（已完整） → 若要我们自己出帧：
     SlFgOff=1 或 FgSuppressNgx=1 切断游戏 FG 内容 + Color=1 + bridge --fg-colour shared
```


### 结论

我们自己那套"从外部把 FG 说成可用"的代理（`src/version_proxy/`，四层：架构 / NGX 能力 / 插件版本 / DXGI 适配器）
**工程上不成立，已停用**。三个游戏的实测：

| 标题 | 现象 |
|---|---|
| Wuchang | 架构、版本、适配器全部伪装成功（日志确认命中），仍未出现 FG；它连 NGX/Streamline 都不问就返回"硬件不支持" |
| CP2077 | 连续三次启动崩溃（`0xFFFFFFFF`、`0x400` 两次），最后连日志都没有 |
| PBZ | **是唯一成功的**：能力按我们的答案翻成 `Available=1`，游戏内 FG 真的打开了（`mode=1`）——但它本来就支持 FSR-FG，无法证明是我们的功劳 |

而 sm86 的方案是被社区广泛验证过的，且它的文档把根因写得很清楚（见 §18 引用的 ARCHITECTURE.md 633/637）：
**运行库把自己声明的最低架构硬编码成 `0x190`，NGX 核心拿它和物理架构比较 → `FeatureInitResult=0xbad0000b`。
这次比较发生在核心初始化（`slInit` 内）时，早于任何外部注入点，所以"从外面回答"注定救不了它。**
sm86 能work，是因为它**替换了运行库本体并在其内部改架构答复**，而不是在外面骗。

### 因此定下的分层（从此为唯一支持路径）

```text
FG 可用性（游戏是否肯出现 DLSS-G 选项）
    └── 交给 dlssg_sm86（version.dll 代理 + 它自己的 310.9.16 运行时）—— 不重写、不替换
数据采集（MV / depth / 相机 / clip / HUDLESS 声明）
    └── 我们的 add-on：FgSniff=1 SlInput=1，FgProvide=0（只观察，不接管）
出帧（独立 GPU 上重放 XeFG）
    └── 我们的 bridge
```

### 已回收的开关（全部恢复默认）

```text
add-on : FgProvide=0  FgSuppressNgx=0  Color=0     （只读采集）
sm86   : Enabled=1    Engine=DLSS/XeSS             （各题原本的值）
代理   : 三个游戏里的 version.dll 均改名为 version.dll.mgpu-off，sm86 原代理已还原
```

### 保留的实验代码（不删除，但默认不启用）

`src/version_proxy/` 保留在仓库里，带 `mgpu_proxy.ini` 的四项开关（默认全 0 才是安全态），
并在文件头写清了它的失败原因与三个崩溃点（NVAPI id 签名、早期预加载、版本块越界写）。
它唯一还有价值的结论是**知识性**的：真实 NVAPI id 是 `0xD8265D24`；Streamline 的闸门在 `slInit`；
运行库的最低架构是硬编码的 `0x190`。这些已写进本节与 §17/§18。


## 16. 接管输出（实验 A）：颜色也走我们的共享流

### 为什么必须走到这一步

§15.3/§15.4 之后链路只剩一个缺口：**游戏自己的帧生成被切断后，它的 swapchain 仍按 2x 出**，
一半的 present 没有内容（黑），而 bridge 的颜色一直来自 WGC 抓游戏窗口 —— 抓到的正是黑帧。
`FgSuppressNgx=1` 的实验给出确切数字：屏幕黑、RTSS 显示 2x、bridge 照常 2.00x 出帧。

所以"用我们自己的工具做 FG"必须是三段：

| 段 | 状态 |
|---|---|
| 采集 FG 输入（MV/depth/camera/clip/HUDLESS 声明） | ✅ §15 |
| 切断游戏 FG 的**内容**（NGX 核心短路，不碰 enabler） | ✅ §15.4 |
| **接管输出**：自己拿真实帧出插帧 | ← 本节 |

### 实现（全部在开关后面，默认行为不变）

**add-on（`Color=1`）**

```text
· 颜色槽位规格修正：原来写死 5120x2880，比 kStreamMaxW/H 大，
  reserve_bytes() 按预留算 → used > reserve → 整块共享内存被禁用。
  现在：内容尺寸 = 真实后台缓冲，容量 = kStreamMaxW/H，逐帧发布 streams[0].width/height。
· 拷贝从 CopyResource 改成 CopyTextureRegion + 内容矩形，
  后台缓冲不必等于槽位尺寸（和 MV/depth 的 v9 规则一致）。
· 实测日志：
    [color] content size 3840x2160 (slot 3840x2160) - the consumer crops
    [color] CopyTextureRegion 3840x2160
    shared ready: used=380.3 MB reserved=380.3 MB slots=4
```

**bridge（`--fg-colour shared`）**

```text
· 不再启动 WGC；颜色从 stream[0] 跨卡拷进颜色汇（copy_inputs_local，和 MV/depth 同一条路）
· 帧的节拍改由生产端发布驱动（frames[].published），不再由 present/WGC 驱动
· 颜色汇格式跟随来源：共享模式用 R10G10B10A2（游戏后台缓冲原样 = HDR10 PQ），
  WGC 模式仍是 scRGB FP16；convert pass 新增 mode 3（源已是显示编码 → 直通，不再 PQ 二次编码）
```

### 结果（明末，RTX 3080 Ti 出帧 + RX 6750 GRE 出插帧）

```text
[bridge] colour sink 3840x2160 fmt 24 (HDR10 PQ, straight from the add-on) shared to D3D12
[bridge] --fg-colour shared: copying 3840x2160 from the producer's colour slot (slot size 3840x2160)
[bridge] --fg-colour shared: driving frames from the producer's publications (4 slots)
[bridge]   cadence: input=26.0ms = 3.12 vblanks -> interp=0 (avg 2.00x, 76.9 fps out) drops=0
```

`input=26.0 ms` 就是**真实帧节拍**（≈38 fps），不再是之前的 14 ms（游戏 FG 的 2x 节拍）
—— 说明"游戏只出真实帧、我们按真实帧插"这件事在节拍上已经成立。

**剩下的缺陷（本节到此为止的原因）**：共享颜色模式下 XeFG 报

```text
[xefg error] Failed to allocate memory for a copy of HUDLESS_COLOR
[xefg error] Resource pointer is null.
```

同一份 bridge、同一款游戏，**WGC 模式 0 条这样的错误**（`bridge_v14_wu_2x.txt`，2.00x / 116–147 fps 正常）。
所以是共享颜色路径引入的，还要单独查。三个候选：

1. `--probe-colour` 只在第 1 帧采样，当时跨卡拷贝还没落地 → "BLACK CAPTURE" 未必是真黑，要先让 probe 在第 N 帧再采；
2. XeFG 的 swapchain 颜色拷贝与"我们不再用 D3D11 写颜色汇"有关（keyed mutex 不再 Acquire/Release）；
3. 深度的本地副本在该节拍下尚未就绪（`Resource pointer is null` 指的就是某个 tag）。

### 复现命令

```bash
# add-on: Color=1  FgSniff=1  SlInput=1  FgNgxHooks=0  FgSuppressNgx=1
C:/temp/mgpu/bin/run_wu_shared.bat     # bridge --fg-colour shared --probe-colour 60
```


## 17. 不借 sm86 的第二块：在 slInit 之前回答架构（`version.dll` 代理）

### 这一层发现的"为什么之前的钩子都没被调用"

- `nvngx_dlssg.dll` **不导出** `NVSDK_NGX_D3D12_GetCapabilityParameters`；上一版实现在
  `cap == nullptr` 就 `return`，于是连它的 `GetFeatureRequirements` 都没钩上 —— 已改成
  "这个模块有什么就钩什么"；
- `NVSDK_NGX_D3D12_Init_with_ProjectID` 之前被 `g_real_ngx_init != nullptr` 挡住而**从未装钩**
  —— 现在两个 init 各有独立的原始指针，都会打印真实返回值并强制 `0x1`；
- 结论性的观察：**游戏进程自己从不问 NVAPI**（sweep 里从未出现 `Project_Plague.exe`），
  它只问 `_nvngx` / `nvngx_dlssg` 和文件版本 —— 这就是为什么"伪装显卡名"不是必需的，
  而"伪装架构 + 伪装插件版本 + 替代 NGX 答复"才是完整的一套。


### 为什么 add-on 一定来不及

`sl.interposer.dll` 自己带着这些字符串，等于把答案写在墙上：

```text
getNVDAArchitecture
Found GPU %d, arch=0x%x
NvAPI_GPU_GetArchInfo(nvapiGpuHandles[i], &archInfo)
Feature '%s' is not supported on this platform
Ignoring plugin '%s' since it is was not requested by the host
```

而 sm86 的文档把时机讲死了：**Streamline 2.x 在 `slInit` 里就决定帧生成插件"本硬件不支持"**，
`sl.common` 拿 `NvAPI_GPU_GetArchInfo` 的架构比 `sl.dlss_g` 的最低要求（AD100 = 0x190），
然后把插件卸掉 —— 这发生在 D3D 设备存在之前，远早于任何运行库加载。
实测印证：add-on 的 NVAPI 钩子落在 `slInit` 之后 **7 秒**。

### 我们的实现：`src/version_proxy/version.dll`

```text
· 只做一件事：nvapi64_impl!nvapi_QueryInterface 的结果里，把架构结构改成 0x190
· 在 DLL_PROCESS_ATTACH 起线程轮询 nvapi64(_impl)，进程启动 ~1.2 s 内武装
· 转发 version.dll 的全部 16 个导出到 system32（它只是落点，游戏本来就要加载它）
· 不跑 NVIDIA 的任何代码，也**不伪装设备名**
```

拒绝列表是硬要求（sm86 #535/#538/#540/#542 的血）：`_nvngx` / `nvngx_*` / `nvapi*` /
DriverStore / `\NVIDIA\NGX\models\`（`models\sl_*` 除外）**必须看真架构**，
否则超分 snippet 走 arch≥0x180 的内核路径，直接 `DXGI_ERROR_DEVICE_HUNG`。

### 关键发现：公开的 id 是错的

sweep（记录每个 `nvapi_QueryInterface` id 及其**调用模块**）抓到了真相：

```text
sl.interposer.dll 只问 7 个 id：0150E828(E5AC921F=EnumPhysicalGPUs)、D8265D24、593E8644、
                                33C7358C、2926AAAD、AD298D3F
0x2D3A1A3A（公开文档里的 NvAPI_GPU_GetArchInfo）**根本不在其中**，也不在 sl.interposer 的字节里
→ 真正被用来填 NV_GPU_ARCH_INFO 的是 0xD8265D24（参数 2）
```

我们的做法因此不是"猜 id"，而是**通用包装 + 结构自证**：对 7 个 id 各套一个 4 参数转发器，
调用后检查两个参数指向的结构首 dword 是否为 `0x20010`（NV_GPU_ARCH_INFO 的 version）；
是就改 `arch` 字节。日志里因此直接写出是哪个 id 命中的：

```text
arch: spoofed through NVAPI id 0xD8265D24 (struct was arg2)
```

### 当前状态与下一步

- ✅ 架构伪装已生效（Ampere 0x170 → 0x190），且**没有**依赖 sm86；
- ✅ add-on 侧补上了核心的 `NVSDK_NGX_D3D12_GetFeatureRequirements`（它会答 min_arch=0x400）；
- ❌ `sl.dlss_g.dll` 仍不在进程里 → Streamline 还有第二个否决条件，三个候选：

  1. 插件自己的能力检查（它去问 NGX 核心，拿到 `0xbad0000b` 架构拒绝）；
  2. 宿主必须显式请求（`slSetFeatureLoaded(eDLSSG, true)`），而这条题没请求；
  3. 插件是拿**另一个 id/另一条路径**读架构（sweep 只覆盖了 sl.interposer 自己问的 id）。

  对应的三个下一步：把包装器从 7 个 id 放宽到"任意 id + 结构自证"（不改调用约定，SEH 保护）、
  用**轮询模块表**（而不是 Hook LoadLibrary，实测会让游戏 3 秒内退出）记录
  `sl.dlss_g` 的出现/消失、以及在这些证据之上决定是否直接 hook `slSetFeatureLoaded`。


## 18. 最后一个闸门：DLSS-G **插件的文件版本**（2026-11-11）

把 API 层所有已知闸门都答成"支持"之后，游戏仍然弹"硬件不支持"。用代理记录 `GetFileVersionInfo`
的调用者与目标文件，抓到了它读什么：

```text
[00:59:35.825] ver: GetFileVersionInfo(nvngx_dlssg.dll)
[00:59:35.826] ver: GetFileVersionInfo(nvngx_dlssg.dll)
```

对比同一台机器上的两个版本，问题一眼就能看出：

| 文件 | 版本 | 结果 |
|---|---|---|
| 引擎自带 `Engine/Plugins/.../nvngx_dlssg.dll` | **310.2.1.0** | 判"硬件不支持" |
| `dlssg_sm86/runtime/nvngx_dlssg.dll`（之前能开 FG 的那份） | **310.9.16** | FG 正常 |

驱动 617.42，早就过了 DLSS-G 的 `MinDriverVersionMajor=520`，所以不是驱动；
`GetFeatureRequirements(feature=11)` 的原始答复也已经是"支持"（`supported=0 arch=0x190`）。
**唯一还差的就是这个版本号。**

⇒ 代理在 `GetFileVersionInfoW/A` 返回后把 `nvngx_dlssg.dll` 的 `VS_FIXEDFILEINFO`
改写成 **310.9.16.0**（`dwFileVersion/ProductVersion MS/LS` 四个 dword）：

```text
ver: nvngx_dlssg.dll reported as 310.9.16.0 instead of the engine's 310.2.1.0
```
