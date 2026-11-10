# Release notes

每个版本一份，可直接粘到 GitHub Releases。版本历史看 `CHANGELOG.md`。

---

## v0.14.0 —— 多游戏通用：读游戏自己说的 + WGC 节流解除

> 游戏用自己的卡渲染 + 超分（DLSS / FSR / XeSS），另一块卡专职跑 **XeSS-FG** 插帧，
> 再通过覆盖窗口上屏。A 卡 + N 卡混插可用；单卡也能跑。

**下载**：`mgpu-0.14.0.zip`（16.5 MB）
`sha256: 883c42b7625ad2bcaa5a99a217073f115c8b19a4f76508d6306dec96aded9568`

> 本包**内含** Intel 的 `libxess_fg.dll`（22.9 MB）与 `libxell.dll`，解压即跑
> （目标版本 `libxess_fg 1.3.1` / `libxell`，`--unlock-mfg` 只对实测过的版本成立）。
> 若要做**不含**它们的公开分发，跑 `SKIP_XESS_DLLS=1 ./scripts/make_release.sh`。

### 🎯 两条主线

**1. 别再逐游戏猜参数。** MV 单位 / 符号 / 深度方向 / 投影，全部改成**读游戏自己的声明**，
猜不出来的才由在线探针兜底：

| 以前要试 | 现在 |
|---|---|
| `--mv-flip` 试符号 | 按 API 契约（上一帧−当前帧），不用开 |
| `--fg-mv-scale` 试单位 | 从 `MV.Scale` / XeSS 速度空间推导 |
| `--fg-depth-inv` 试深度方向 | 用游戏的 `DepthInverted`，缺失时探针实测 |
| `--fg-fov` 猜投影 | 用**游戏自己的投影矩阵**；游戏不给才回退 |

**2. WGC 捕获节流默认关闭（本版最重要）。** `GraphicsCaptureSession::MinUpdateInterval` 默认
一个合成器周期（≈16.6 ms）⇒ WGC 最多交付 ~60 帧/秒，**游戏跑再快也只拿到 60**。实测关掉后
"操作瞬间流畅跟手了很多"。这也是此前所有"怎么调都差一点"的根因 —— 我们一直在用被削到 60 fps 的输入做分析。

### ✨ 新东西

- UI「模式」多了 **关闭 (1:1 直通)**（`--no-fg`）：输入已经 ≥ 面板一半时它最跟手，
  bridge 检测到这种情况会主动提示；
- **协议 v12**：真实投影矩阵（Streamline `sl::Constants` / XeFG frame constants），
  跨实例用命名共享内存交接；
- 诊断脚本随包：`A`(jittered MV) / `B`(光流) / `C`(VRR) / `D1`(无 FG 归因) / `D2`(固定 2x) /
  `D3`(VRR+半分辨率) / `E`(WGC 不节流) / `F`(k×vblank 预算)。

### 🛠 修掉的问题

- **add-on 热卸载崩溃**（`std::mutex` 已析构后仍被回调 → 空指针，改 `SRWLOCK` + 存活标志）——
  这是"某游戏装上 add-on 就启动崩溃"的根因；
- loader 钩子改为只入队、帧内排空（不在 `LoadLibrary` 里装 MinHook 跳板）；
- `in_xess` 把 MV 单位判成 UV（实为像素）；声明不发布导致消费端看不到符号；
- UI：`运动矢量` 选"AMD 光流"存不住、卡片行数写死、协议版本不符时提示不清；
- cadence 三个真 bug：估计器不一致、倍率无上限、时长与批次错配（空刷新周期 12% → 0）。

### ⚠️ 已知边界（不是 bug）

- **非整数倍率 + 固定刷新率无解**：如 53 fps 在 120 Hz 上 = 2.26 帧/输入，只能靠面板 VRR
  或把输入锁到刷新率的整数分之一（60/40）；我们做过 `k×vblank` 预算与倍率黏性两个实验，
  实测都更差，因此保持现状；
- 游戏自身出现 100 ms 级的洞时，任何插帧都补不回来（内容缺失）；
- 个别游戏的 FSR 运行时会**在销毁时 double-free**（影之刃零实测），这类标题改用 DLSS / XeSS。

### 🔄 升级提示

`bridge.exe` / `game_capture.addon64` / `mgpu_ui.exe` **三件套必须同版**（协议 v12）；
`game_capture.ini` 新增 `StreamlineCamera`（默认 1）。旧配方里的
`--mv-flip` / `--fg-mv-scale` / `--fg-depth-inv` 留着无害，但已不需要。

---

## v0.13.2 —— 并行化复审：能并行的都试过了，结论是"默认全关"

> 游戏用自己的卡渲染 + 超分（DLSS / FSR / XeSS），另一块卡专职跑 **XeSS-FG** 插帧，
> 再通过覆盖窗口上屏。A 卡 + N 卡混插可用；单卡也能跑。

**下载**：`mgpu-0.13.2.zip`（16.5 MB）
`sha256: 25dff7e0daaf08a9d16bee83e7a2150570d6d8160baf07ad866a9dd4f5878558`

> 与 `v0.13.1` 一致，本包**内含** Intel 的 `libxess_fg.dll`（22.9 MB）与 `libxell.dll`，
> 解压即跑（目标版本 `libxess_fg 1.3.1.78` / `libxell 1.3.2.10`，`--unlock-mfg` 只对实测过的版本成立）。
> 若要做**不含**它们的公开分发，跑 `SKIP_XESS_DLLS=1 ./scripts/make_release.sh`，
> 取法与版本要求见随包生成的 `bin\README-DLL.txt`（见 `docs/ops/LEGAL.md`）。

### 🎯 这一轮做了什么

把 fg 链路上**还能并行/异步的地方全部实现并真机实测了一遍**，然后按数据决定留哪些：

| 方向 | 做法 | 结论 |
|---|---|---|
| 跨卡 MV/深度拷贝 | `--input-copy-queue`：搬到自己那条 COPY 队列，与颜色 convert draw 重叠 | ❌ `--den 2` 测不出来；`--den 1` 单趟 −1…−2 ms 但体感更差 |
| 提前一帧预取 | `--prefetch-inputs`：slot N+1 在帧 N 的 Present 之后拷 | ❌ 机制 100% 命中，但空刷新 0.32 → **2.90/窗**（循环跑赢 pacer） |
| 搬出游戏队列 | `[capture] OffloadQueue=1`（add-on） | ❌ 功能验收通过（9987 帧 / 0 stall / 0 fail），收益未量到 |
| 抗丢帧 | `--slack-auto`（UI 开关） | ✅ 建议开：几乎零成本，只在"真有新内容"那几帧 +1 个间隔延迟 |

**新开关全部默认关闭**，默认行为与 v0.13.1 完全一致。

### 🎁 UI 新增两个控件

- **捕获 → 色彩空间**：`HDR10（默认）` / `SDR（BT.709）`。之前 `sdr` 是 ini 里的死键
  （有字段、没控件、也不传给 bridge）。我们抓的是 scRGB(FP16)，HDR 源永不丢；
  在你的显示器是 SDR 时选 `SDR` 才不会发灰。
- **帧生成 → 抗丢帧 (A/B)**：对应 `--slack-auto`。

### 🩹 修掉的问题

1. **`WGC sees 60 fps but the game only renders 38.4 fps` 是假警报。** 它拿我们的**中位数**
   去比生产端的**一次原始采样**：游戏 ~44 fps 落在 120 Hz 面板上时，帧间隔本来就在 2/3/4 个
   vblank 之间跳。这条误报把排查引向"游戏自带 FG"（实测游戏 FG 是关的）。现在两边都取
   16 帧中位数，并把 **DWM 重复合成**列为第二种可能原因。
2. **`--fg-warp` 两条 list 共用一份 command allocator**：对**还在执行**的 allocator 做 Reset
   是 D3D12 的未定义行为。现在双 allocator 轮换；顺带修掉 `back_buffer()` 失败时第二条 list
   往已关闭的 list 里录。
3. **UI 的 `sdr` / `slack1` 两个死键**接上了（见上）。

### 📊 顺手量到的事实（以后不用重测）

- **OSD 的 `drop N` 大多不是掉帧**：`--slack-auto` 的判据证明，被丢弃的抓取帧里生产端
  `frame_published` 没有变化 —— 是 DWM 把同一帧重复合成上来的副本，丢掉不损失画面。
  新日志把它拆成 `[K real loss / M-K duplicate]`，**只有 K 是真丢内容**。
- **手工分次跑 A/B 在本机不成立**：同配置两趟 `total` 差 **3.2 ms**、`age` 差 6 倍。
  仓库里新增 `bin\12 - automatic A-B-C-A 4K (60s each).bat`（自动连续跑四段）—— 本机唯一可信的跑法。
- **我们自己没有任何硬编码帧率上限**：`--lock` / `--fg-out-hz` / `--pace` 默认全关，
  UI 里"建议锁帧"只是提示（不传给 bridge）。

### ⚠️ 兼容性

- 目标 DLL 仍是 `libxess_fg 1.3.1.78` / `libxell 1.3.2.10`（`--unlock-mfg` 只对实测过的版本成立）；
- `game_capture.ini` 新增 `OffloadQueue=0`（默认关，键不存在也等价）；
- 一切原配方、原命令行参数行为不变。

细节与全部实测数据：`docs/findings/PERF_BASELINE.md` §3.28–§3.30。

---

## v0.13.1 —— UE5 伪影定位到「上采样倍率」+ RE Engine/PRAGMATA 支持

> 游戏用自己的卡渲染 + 超分（DLSS / FSR / XeSS），另一块卡专职跑 **XeSS-FG** 插帧，
> 再通过覆盖窗口上屏。A 卡 + N 卡混插可用；单卡也能跑。

**下载**：`mgpu-0.13.1.zip`（16.4 MB）
`sha256: c44dd6304474773a7f3c6efd94f607d62c3dc7d4e9ea0678cad85995c97bc5e7`

### 🎯 UE5 的「模型运动伪影」结案：主因是上采样倍率

一整轮对照之后可以确定：**不是运动矢量的问题**。

- **游戏交给 NGX 的运动矢量流本身是坏数据**（PBZ 实测：0.5% NaN/inf、17% 饱和到 fp16
  上限 ±65504、与真实位移相关性 ≈0）⇒ 喂它就**闪 + 鬼影**；
- 换成我们自己的光流（`--fg-of`）后**不闪**，但伪影还在 —— 再对比发现 **DLSS 性能档
  （720p→4K）连"不插帧的游戏本身"都有伪影**；
- **DLAA（1:1 渲染）+ `--interp 1 --fg-of` 干净**。这就是 UE5 的新推荐配方。

顺手修掉一个一直存在的真 bug：`--fg-of` 的 `motionVectorScale` 以前透传游戏的 `MV.Scale`
（PBZ 报 `1.0`），库把 screen-UV 的 `0.009` 当成 **0.009 像素** ⇒ **光流等价于没生效**
（所以历史上"光流不闪"其实是"光流没开"）。现在用 MV 纹理尺寸。

### 🧩 新增 RE Engine / PRAGMATA 支持

- **NGX 入口不一定在驱动 shim 上**：RE Engine 走 Streamline，NGX 入口在游戏自带的
  `nvngx_dlss.dll` 上。旧判据只认 `_nvngx.dll` ⇒ **一个钩子都没装**（ReShade 说注册成功，
  但零数据）。现在按"文件名 + 真的导出 `EvaluateFeature`"判定。
- **`[capture] LoaderHook=0`**：不碰 `kernel32!LoadLibrary*`，改为每 120 帧扫模块表 ——
  修掉"装上 add-on 后该游戏启动极慢"（A/B 已确认是我们的钩子）。
- **采集暂停从 `+120 帧` 改成「帧数或 8 秒」**：加载期只有 0.8 fps 时，旧逻辑等于暂停 150 秒。
- **配置/日志回退**：游戏可能从 `_storage_\` 这类重定向目录读文件（PRAGMATA 就是），
  本地读不到时回退 `%LOCALAPPDATA%\mgpu`；新增面包屑 `breadcrumb.log` 一行定位问题。
- **安装器 6/6「镜像目录」**：任何子目录已有同名 add-on / 同名代理 DLL，就自动也装一份。

### 🔍 新工具 / 新开关

- `--fg-mv-scale raw|px|<float>`、`--fg-jittered-mv` / `--fg-no-jittered-mv`；
  `--fg-jitter-zero` 重新生效（颜色是上采样后的画面，jitter 已解掉）。
- 回调计时 `perf: frames=.. begin=.. barrier=..`（实测回调合计 0.17 ms/帧）。
- `scripts/analyse_dump.py` 新增 **units fit**；新配方脚本
  `7 - UE5 MV A-B.bat`（`auto/jit/nojit/depthinv/jitdepth/mvscale/nomv/of/ofsign/offov/jz/nodepth/only/dump`）。

### ⚠️ 升级提示

- `bin\` 直接覆盖；游戏侧建议重跑 `game-pack\install.bat`（它现在会自动处理 `_storage_`）。
- **`--fg-of` 的行为变了**（以前在 `MV.Scale=1.0` 的游戏上等于没开），
  3x/4x 的观感可能与旧版不同 —— 值得在 CP2077 等游戏上重测一次。

---

## v0.13.0 —— UE5「模型运动伪影」结案：主因是上采样倍率

> 游戏用自己的卡渲染 + 超分（DLSS / FSR / XeSS），另一块卡专职跑 **XeSS-FG** 插帧，
> 再通过覆盖窗口上屏。A 卡 + N 卡混插可用；单卡也能跑。

这版把 UE5（影之刃零 / 黑神话 / 明末 …）上一直存在的**模型边缘伪影**查清了：
**主因不是运动矢量，而是上采样倍率** —— 插帧插的是上采样之后的画面，DLSS 性能档
（720p→4K，3 倍）连"不插帧的游戏本身"都有一点伪影；换成 **DLAA（1:1）+ `--fg-of`** 就干净。

### ✨ 新配方

- **UE5 / 画质优先**：游戏内 **DLAA**（或最高画质档）+ `--interp 1 --fg-of`
  （4K DLAA 下 26→52 fps，实测无模型边缘伪影）
- 必须用性能/平衡档时：伪影随放大倍率走，与开不开插帧关系没那么大

### 🔧 修掉的真 bug

- **`--fg-of` 之前基本没生效**：光流场是 screen-UV，`motionVectorScale` 却透传游戏的
  `MV.Scale`（PBZ 报 `1.0`）⇒ 库把 `0.009` 当成 0.009 像素。现在用 MV 纹理尺寸
  （日志 `mvscale=(1278.0000, 720.0000)`）。**影响所有 `--fg-of` 配方，含 3x/4x 推荐档。**
- **add-on 开始读游戏声明的 DLSS flags** 并转发 `MVJittered` / `MVLowRes` / **`DepthInverted`**；
  bridge 在创建 swap chain 前再读一次，迟到会打 WARNING。PBZ 声明
  `0x4B = IsHDR｜MVLowRes｜DepthInverted｜AutoExposure`（`MVJittered=0`）。

### 🔍 新工具

- `--fg-mv-scale raw|px|<float>`、`--fg-no-jittered-mv`；`--fg-jitter-zero` 重新生效（A/B 用）
- `scripts/analyse_dump.py` 的 **units fit**：拟合 `motion_px = mv_raw × k`，直接判 MV 单位与帧配对
- `scripts/recipes/run_ue5_mv_ab.bat`：`auto/jit/nojit/depthinv/jitdepth/mvscale/nomv/of/ofsign/offov/jz/nodepth/only/dump`

### ⚠️ 注意

- UE5 交给 NGX 的运动矢量可能是**坏数据**（PBZ 实测：0.5% NaN/inf、17% 饱和到 fp16 上限、
  与真实位移相关性 ≈0）⇒ 那条流别用，用 `--fg-of` 或 `--no-mv`
- 详细证据链：`docs/findings/UE5_MV_OPTISCALER.md` §7–§8

---

## v0.12.2 —— 双卡帧生成：动态逐帧可用、MV 不再 drop

> 游戏用自己的卡渲染 + 超分（DLSS / FSR / XeSS），另一块卡专职跑 **XeSS-FG** 插帧，
> 再通过覆盖窗口上屏。A 卡 + N 卡混插可用；单卡也能跑。

这版把"输出不够丝滑"这条线彻底查清了，落成三件事：**动态逐帧**（输入帧率抖动也能填满高刷）、
**跨卡流量减半**（开 MV 不再掉帧）、**安装器强制覆盖**（不再被旧文件坑）。

### ✨ 新功能

**动态逐帧模式**（UI 模式分成「固定」/「动态逐帧」两块，互斥）

- 每帧按实际经过的 vblank 数决定倍率（`--interp-cadence`），输出恒定 **1 帧/vblank**，输入 35–45 fps 也能把 120 Hz 填满
- `--cadence-smooth N`（默认 8）：先对到达间隔做 EMA 再算倍率 —— 逐帧原始间隔会让倍率跟着抖 1:1
- `--fg-out-hz N`：输出上限（0 = 面板刷新）。把"120 Hz 面板只要 60 fps"变成 2x 而不是 4x
- UI「输出帧率」改成手输数字框；「倍数」在动态模式下作为容量上限

**`[capture] StreamHeight=1440`（add-on 侧，默认）**

- 跨卡流（MV + 深度）的高度上限，宽度按渲染宽高比自动算；4K 渲染由 **63.3 → 28.2 MB/帧**
- `0` = 跟随渲染尺寸（旧行为）；`1080` 再降一档；`2160` = 全分辨率

### 🐛 修复

- **开 MV 就 drop**：根源不是插帧算法，而是渲染卡每帧为 MV+深度扛的 63 MB 跨卡流量 + 两个全屏转换 pass（渲染卡被游戏吃满，转换抢不到执行窗口）。`StreamHeight=1440` 后**实测不再 drop** ⇒ 动态逐帧也能正常用游戏 MV
- **到达间隔量错**：原来用 `GetTickCount64`（1 ms 粒度，0 ms 样本被丢），中位数报 31 ms 而非 25 ms，倍率被吸附到 4 ⇒ 输出只有 96.8 fps。改用 WGC 的 `SystemRelativeTime` 后 3x@40 = **120.0 fps / 1.00 vblank**
- `show=` 曾按 `loop_fps × framesPresented` 算（120 Hz 面板读出 148），改成 `presents / 窗口时长`
- `Present`/fence 等待不再每次新建事件；后缓冲缓存、流几何只在 generation 变化时重读；日志 flush 限流 200 ms
- Win+Tab / Alt+Tab / Win+D 后覆盖窗口丢失
- **安装器**：新建文件时 `Split-Path -Leaf` 拿到空备份名会中断安装（已修）

**安装器改为强制覆盖**（v0.12.1）：所有文件一律覆盖成本包版本，覆盖前轮转备份（`.mgpu_bak` → `.mgpu_bak2` … 最多 9 份），逐文件 SHA256 校验，装完打结果表 + 扫残留；新增 `-KeepReShadeIni` 保留你自己的 ReShade 设置

### 📊 实测（CP2077 4K / 120 Hz，3080 Ti 渲染 + 6750 GRE 插帧）

| 配置 | 输出 | 空刷新 | 备注 |
|---|---|---|---|
| 动态逐帧（输入 35–45 fps 抖） | 120 fps | **1.3%** | 倍率切换 12 次/秒 |
| 固定 3x @40 fps | 120 fps | **0.1–0.3%** | 最大空洞 2 个 vblank |
| 固定 2x @60 fps | 120 fps | 2.5% | 画质优先 |
| 固定 4x @30 fps | 120 fps | 2.6% | 用动态逐帧可到 0.6% |

### 🔍 诊断增强

`breakdown` 新增 `colour=` / `cpu=`；新增 `bus:`（每帧跨卡字节与 GB/s）、`arr(vb):`（到达间隔的 vblank 分布）、`out:`/`out2:`（present 与 vblank 计数）、`game(vb):`、`bursts:`、`rt spikes:`、`rt auto:`、`headroom:`（跨卡预算超阈值时给降载建议）

### ⚠️ 升级必读

- **add-on 与 bridge 必须同版本**（协议 v10，版本不匹配会直接拒绝通信）—— 用包里的 `game-pack/install.bat` 重装一次游戏侧
- `game_capture.ini` 新增 `StreamHeight`（默认 1440）。旧 ini 会让 4K 渲染仍走 63 MB/帧 ⇒ 请让安装器覆盖它
- 旧的 `--dyn-mfg` 实验档已从 UI 移除（命令行仍在）

### 🚫 已实测排除（别再试）

| 尝试 | 结论 |
|---|---|
| `--cadence-hold N`（保持倍率 N 帧） | 用丢帧换稳定：空刷新 6.6–10.4%、`slow` 10–13 帧/窗口 |
| `--fg-cad-rt 1`（frameRenderTime = 批次 × vblank） | 理论上更自洽，实测更差：`Present` 按预算阻塞，输入间隔在下一帧到达时才量 ⇒ xN 从 3.00 漂到 3.38 |
| 动态逐帧 + `--fg-of`（光流） | 同卡多一个全屏 pass，drop 明显增加（30 秒段落看不出，长玩露馅） |
| `--no-depth` | disocclusion 会渗色 |
| 固定 3x 配 44–45 fps 输入 | 45×3 = 135 > 120，必然超额 |

### 📦 下载

| 文件 | 说明 | 大小 |
|---|---|---|
| **`mgpu-0.12.2-public.zip`** | **公开版**：不含 Intel 的 `libxess_fg.dll` / `libxell.dll`，请自行从 XeSS SDK 获取（包内 `README-DLL.txt`） | 2.8 MB |
| `mgpu-0.12.2.zip` | 本地完整版（含运行时 DLL，仅自用，不用于分发） | 15.6 MB |

```
SHA256  mgpu-0.12.2-public.zip  4bf2096116f04bff6e7fdc2d4b6fb2b1369a1aebc358d5e770ccab72ba9f5ee9
SHA256  mgpu-0.12.2.zip         1b856a65aed0f62e055c62e948b131238faf76961f6682f45c2cb8140ebde045
MD5     mgpu-0.12.2-public.zip  ab5ddc7df68dfa0a818c6e3b4ef1ee13
MD5     mgpu-0.12.2.zip         5f7e7066d818291390ef2069224cad71
```

### 已知限制

游戏自带帧生成必须关；不支持 TSR / FSR1 / 原生 TAA（拿不到 MV）；必须用外部限帧器锁帧；
5K 原生只有 2x@30 ≈ 60 fps 跑得动；A 卡渲染 + N 卡输出的完整组合本机未跑通；端到端延迟 ~40–70 ms。

**许可**：GPL-3.0-or-later。`--unlock-mfg` 在内存里修改 `libxess_fg` 以打开 3x–6x，
**可能违反 Intel EULA，仅供个人研究**（`docs/ops/LEGAL.md`）。

---
