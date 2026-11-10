# mgpu 使用说明

**双卡帧生成**：让游戏用**自己的卡**渲染 + 超分（DLSS / FSR / XeSS），
另一块卡用 **XeSS-FG** 插帧，成品通过覆盖窗口上屏。

- 想要"引擎渲染在 3080 Ti、插帧在 6750 GRE"这种组合，就是它；
- A 卡 + N 卡混插可用，单卡也能用（插帧和渲染在同一块卡上）；
- 游戏本身**不需要**支持帧生成，只要支持 DLSS / FSR2+ / XeSS 之一。

不能做的事（先说清楚）：

| 做不到 | 原因 |
|---|---|
| 让游戏自带帧生成 + 本工具同时开 | 三层插值，画面必炸 |
| 支持 TSR / FSR1 / 原生 TAA | 拿不到运动矢量，插值质量不可接受 |
| 锁定游戏帧率 | 必须用外部限帧器（RTSS），本工具不改别人的限帧器 |
| 5K 原生高倍率 | 只有 2x@30 ≈ 60 fps 跑得动 |

---

## 1. 包里有什么

```
mgpu-<版本>/
├─ README.md            ← 本文件
├─ LICENSE  NOTICE        许可（GPL-3.0）与第三方署名
├─ SHA256SUMS.txt         每个文件的校验值（装完可核对）
├─ bin/                   ★ 工具侧：帧生成进程 + 前端
│   ├─ 0 - UI.bat             双击 → 打开前端
│   ├─ 1 - 3x optical flow.bat    固定 3x（40 fps → 120 fps）
│   ├─ 2 - 2x quality.bat         画质优先（60 fps → 120 fps）
│   ├─ 3 - 3x lowest cost.bat     预算最紧时用
│   ├─ 4 - dump frames.bat        抓帧做离线分析（排障/对比用）
│   ├─ 5 - smoothest (cadence).bat  ★ 推荐配方（动态逐帧，输入不稳时最顺）
│   ├─ 6 - cadence batch feed (A-B).bat  A/B 对照，平时不用
│   ├─ bridge.exe             帧生成进程（真正干活的）
│   ├─ mgpu_ui.exe            前端（选游戏窗口 / 开关 / 看状态）
│   ├─ libxess_fg.dll, libxell.dll   XeSS 帧生成运行时（**公开版包里不带**，见 §3.2）
│   └─ game_capture.addon64   备用副本（正常安装走 game-pack/，这是给手动拷贝用的）
├─ game-pack/             ★ 游戏侧：装进游戏目录的那一半
│   ├─ install.bat / uninstall.bat    拖 exe 即装 / 即卸
│   ├─ README.txt                     游戏侧说明
│   └─ files/                         ReShade 6.8 + add-on + ini + shader
└─ docs/                  设计、协议、配置、结论、踩坑清单
```

两份东西分工：`game-pack/` 进游戏目录（抓数据），`bin/` 放任意位置（插帧上屏）。

## 2. 前置条件

| 项 | 要求 |
|---|---|
| 系统 | Windows 10 1809+ / Windows 11（WGC 抓屏 + DirectComposition 要求） |
| 游戏 | 64 位；支持 DLSS 2+、FSR 2.2+ 或 XeSS（能选其一即可） |
| 显卡 | 一块卡渲染、另一块卡插帧（同一块也行）。XeSS-FG 在 A/N/Intel 上都能跑（3x 以上见 §7） |
| 限帧器 | **RTSS（RivaTuner Statistics Server）**，用来把游戏锁到目标输入帧率 |
| VC++ 运行库 | 14.40+（`msvcp140.dll` / `vcruntime140.dll` / `vcruntime140_1.dll`） |

> 显示刷新率决定"倍数"怎么选：**输出帧率 = 输入帧率 × 倍数**，
> 而且要落在整数个 vblank 上才顺。120 Hz 面板的常用解：
> `30×4 = 120`、`40×3 = 120`、`24×5 = 120`、`20×6 = 120`。

## 3. 安装（两步）

### 3.1 装进游戏

1. 完全退出游戏；
2. 把游戏的 **`.exe` 文件拖到 `game-pack\install.bat` 上**松手
   （或命令行 `install.bat "D:\Games\XXX\bin\x64"`）；
3. 目录要选**直接包含游戏 exe** 的那一层，不是上一级。

装完游戏目录里会出现：`dxgi.dll`（ReShade 6.8）、`game_capture.addon64`、
`game_capture.ini`、`reshade-shaders\Shaders\mgpu_depth_tap.fx`、`ReShadePreset.ini`。
原有的 `dxgi.dll` / `ReShade.ini` 会先备份成 `*.mgpu_bak`，装完做 SHA256 校验。
游戏开着会拒绝安装（DLL 被占用），先关游戏。

### 3.2 放好工具

把整个 `bin\` 目录放到任意位置（例如 `D:\mgpu\bin`），**保持里面文件在同一层**：
`bridge.exe` 与 `libxess_fg.dll` / `libxell.dll` 必须同目录，否则起不来。
不需要管理员权限，不需要安装。

#### 还差两个 Intel 的 DLL（公开版包不含）

`libxess_fg.dll`（XeSS 帧生成）和 `libxell.dll`（低延迟）属于 **Intel**，
按 Intel 自己的许可分发，**不能随本项目打包**。所以公开下载的包里它们不在了，
你需要自己放进去（本目录里有个 `README-DLL.txt` 也是这段说明）：

1. 取 **Intel XeSS SDK v3.0.2**：<https://github.com/intel/xess>（tag `v3.0.2`，
   或任何 XeSS 3 SDK 的 `bin/` 目录）；
2. 把 `bin\libxess_fg.dll` 和 `bin\libxell.dll` 拷到 `bin\` 里（与 `bridge.exe` 同一层）；
3. 完事，双击 `0 - UI.bat` 或任一配方脚本。

| 用途 | 版本要求 |
|---|---|
| 2x（`--interp 1`） | 任意 1.3.x 即可 |
| 3x 及以上（要 `--unlock-mfg`） | 只对实测版本成立：`libxess_fg 1.3.1.78` / `libxell 1.3.2.10`；别的版本会拒绝执行并打印 `*_unexpected_bytes`（故意的，不会瞎改） |

## 4. 游戏内设置

1. **关掉游戏自带的帧生成**（DLSS FG / FSR FG 都关）；
2. 超分/抗锯齿选 **DLSS** 或 **FSR**（XeSS 也行）；分辨率/预设随意；
3. 用 RTSS 把游戏**锁到目标输入帧率**（见 §2 的整数 vblank 解）；
4. 无边框/独占全屏都行，别用太小的窗口。

## 4.5 v0.14.0 有什么不一样（必读）

这一版把"逐游戏猜参数"换成了"读游戏自己声明的"：

| 以前（要试） | 现在（自动） |
|---|---|
| `--mv-flip` 逐游戏试符号 | 符号按 API 契约（上一帧 − 当前帧），**不用开** |
| `--fg-mv-scale` 逐游戏试单位 | 从 `MV.Scale` / XeSS 速度空间推导：`≈1` = 像素、`≈渲染宽` = UV、`≈宽/2` = NDC |
| `--fg-depth-inv` 逐游戏试深度方向 | 用游戏的 `DepthInverted`，缺失时由 add-on 探针实测 |
| `--fg-fov` 猜 FOV | 用**游戏自己的投影矩阵**（Streamline `sl::Constants` / XeFG frame constants）；游戏什么都不给才回退 `--fg-fov` |

两个新开关，一般都不用动：

- `game_capture.ini` → `StreamlineCamera=1`（默认）：取游戏投影矩阵。若某标题给的是占位值、
  你想改回命令行猜测，把它设 0 并显式给 `--fg-fov`。
- bridge → `--no-of-fallback`：关掉"生产端说 MV 不可用就改用我们自己的光流"的自动回退。

> `--vrr`（VRR 面板的"均匀间隔"模式）**不在 UI 里**：2026-11-10 的实测表明它没有解决卡顿
> （输入帧率自身的抖动占主导），参数保留在 `bridge.exe` 上仅供实验。

**WGC 节流：默认关闭（2026-11-10 实测）**

`GraphicsCaptureSession::MinUpdateInterval` 默认约 16.6 ms ⇒ **WGC 最多交付 ~60 帧/秒**，
游戏跑再快也只能拿到 60。实测把它设为 **0** 后"操作瞬间流畅跟手了很多"，且所有历史捕获率都
卡在 59.6 fps 也由此解释。因此：

- **bridge 默认 = 0（不节流）**；`--wgc-minint 16` 可恢复旧的 ~60 fps 上限（仅命令行）；
- **UI 不再提供这个选项**（默认既是正确值，就不该让人误关/误开）。

### 模式多了「关闭 (1:1 直通)」

节流解除后输入常常能跑到 **70~110 fps**，这时插帧反而有害：XeFG 最少也要 2x，120 Hz 面板显示不了
160/180 fps，多出来的帧只会变成重复帧。所以「模式」下拉现在有三项：

| 模式 | 命令 | 什么时候用 |
|---|---|---|
| 固定 | `--interp N` | 输入能锁到刷新率的整数分之一（60/40） |
| 动态逐帧 | `--interp-cadence` | 输入比面板慢、且无 VRR |
| **关闭 (1:1 直通)** | `--no-fg` | **输入已经 ≥ 面板的一半**（实测最跟手、延迟最低） |

bridge 检测到后者时会主动提示一次：
`note: input ~96.2 fps is already 80% of the 120 Hz panel … Switch the UI mode to "关闭 (1:1 直通)"`。

### 多游戏怎么测（每款游戏走一遍）

1. 把**该游戏的 exe** 拖到 `game-pack\install.bat` 上（ReShade + add-on + ini 装进它的目录）；
2. 游戏内：超分选 **DLSS 或 XeSS**，**游戏自带帧生成关掉**；输入帧率锁到面板一半（RTSS）；
3. 双击 `bin\2 - 2x quality.bat`（2x 用游戏 MV，画质最好）或 `bin\5 - smoothest (cadence).bat`；
4. 画面不对时按 §8/§9 判断；需要日志就把该游戏目录下 `game_capture.ini` 改成 `Log=1`
   （再要诊断细节加 `FgSniff=1`，只读、不影响行为）。

### 已知边界（不是 bug）

- **卡顿/不顺的根源通常是输入帧率本身**。用 `--no-fg`（1:1 直通）可以自证：我们的链路
  `total ≈ 2.5 ms`、`age ≈ 2 ms`，若直通时仍然顿，那就是游戏在抖。
  非整数倍率（如 53 fps 在 120 Hz 上 = 2.26）在**固定刷新率下无解** —— 只能
  ①开 VRR + UI 的「VRR 均匀间隔」，或 ②用 RTSS 把游戏锁到 120 的整数分之一（60/40）。
  游戏自己出现 100 ms 级的大洞时，任何插帧都补不回来（那是内容缺失）。
- **UI / add-on / bridge 必须同一版**：三者按同一个协议版本号编译（当前 v12）。混用旧 UI 会提示
  "协议版本不符（addon v12 / ui v10）" —— 更新时**三个一起换**，别只换游戏里那两个。
- 游戏**既不用 Streamline 也不开任何 FG**（纯 DLSS-SR 超分）时拿不到投影 ⇒ 用 `--fg-fov`
  （影之刃零实测垂直 FOV ≈ 46.7°）；
- 有些游戏的 FSR 运行时 `amd_fidelityfx_upscaler_dx12.dll` 在**销毁**时 double-free ⇒ 这类标题
  改用 DLSS / XeSS（影之刃零实测三次，崩栈里没有本工具）；
- 只有深度、没有超分的游戏只能靠光流 MV（`--fg-of`）。

## 5. 第一次运行

### 5.1 用前端（推荐）

1. 先启动游戏，进到游戏世界里；
2. 双击 `bin\0 - UI.bat`（或直接双击 `mgpu_ui.exe`）；
3. "运动矢量"选 **AMD 光流**、"倍数" 选 **3x**；
4. 点标题栏右上角的 **启动**（或按热键，默认 `Ctrl+PageUp`）；
5. 屏幕左上角出现单行 OSD 就说明通了：

```
120 fps   x3   in 40.0 fps   gpu 9.0 ms   game 25.0 ms   lock 40   drop 0
```

（上屏帧率 / 倍数 / 游戏输入帧率 / 等 GPU 的时间 / 游戏帧间隔 / 限帧 / 丢帧）

> **`drop N` 是什么**：它数的是"我们主动丢掉了一个**抓取帧**"。其中相当一部分是同一帧被
> 合成器重复送来的副本（我们处理的帧率与游戏自己 Present 的帧率一致时，丢的多半是副本），
> **不损失画面**。要区分"真丢内容"和"重复帧"，开着 `抗丢帧` 看日志那行
> `slack-auto: N held back, M dropped [K real loss / M-K duplicate]` —— 只有 **K** 是真的丢了一帧。

界面上的两个"模式"开关值得知道：

- **捕获 → 色彩空间**：默认 **HDR10**（BT.2020 + PQ）。我们抓的是 scRGB(FP16)，HDR 信息不丢；
  如果你的显示器是 **SDR**，选 `SDR（BT.709）` 才不会发灰（反过来在 HDR 屏上选 SDR 会发灰，属正常）。
- **帧生成 → 抗丢帧 (A/B)**：默认关。打开后只有"确实有新内容"的抓取帧才会被多留一帧
  （代价是那一帧 +1 个输入间隔的延迟），几乎零成本，建议开着。

界面上改任何一项，卡片里"启动命令"都会同步变化 —— 那就是实际执行的命令行。
界面说明见 `docs/ops/UI.md`。

### 5.2 用命令行 / 配方脚本

`bin\` 里有 14 份脚本，**直接双击就能跑**（不给参数时 bridge 处理"当前前台窗口"，
所以先点一下游戏窗口再双击脚本），也可以把**游戏的 exe 拖到脚本上**来指定目标。

| 脚本 | 用途 |
|---|---|
| `0 - UI.bat` | 启动前端 |
| `1 - 3x optical flow.bat` | 3x 满帧（固定倍数 + 光流） |
| `2 - 2x quality.bat` | 2x 画质优先 |
| `3 - 3x lowest cost.bat` | 预算最紧 |
| `4 - dump frames.bat` | 抓 colour/depth/MV 做离线分析 |
| **`5 - smoothest (cadence).bat`** | **动态逐帧，日常推荐**（输出恒定 1 帧/vblank） |
| `6 - cadence batch feed (A-B).bat` | A/B：另一种 `frameRenderTime` 喂法（实测更差，只作对照） |
| `7 - UE5 MV A-B.bat` | UE5 运动矢量 A/B 矩阵 |
| `8 - smoothest + copy queue (A-B).bat` | 配方 5 + 跨卡拷贝走 COPY 队列（实测无收益） |
| `9 - 4K out 3x (A).bat` | 4K **原生输出**基线（`--den 1 --interp 3`） |
| `10 - 4K out 3x + cq (B).bat` | 9 + `--input-copy-queue` |
| `11 - 4K out 3x + prefetch (C).bat` | 9 + `--prefetch-inputs`（实测空刷新变差） |
| **`12 - automatic A-B-C-A 4K (60s each).bat`** | **自动连续跑四段**：本机做性能 A/B 唯一可信的方式（手工分次会差 3 ms 以上） |
| `13 - smoothest + anti-drop (A-B).bat` | 配方 5 + `--slack-auto`（抗丢帧） |

等价的手打命令：

```bat
rem 3x 满帧（推荐）：光流 MV；--fg-rt-mode quant 把实测间隔吸附到整 vblank
bridge.exe --den 1 --pool 4 --interp 2 --fg-of ^
           --unlock-mfg --fg-ui none --fg-proj-rz --fg-rt-mode quant

rem 2x 画质优先：用游戏自己的 MV（game_capture.ini 的 SkipMv=0，默认就是）
bridge.exe --den 1 --pool 4 --interp 1 --fg-ui none --fg-proj-rz --fg-rt-mode quant

rem 最低开销：不用任何 MV
bridge.exe --den 1 --pool 4 --interp 2 --no-mv ^
           --unlock-mfg --fg-ui none --fg-proj-rz --fg-rt-mode quant
```

目标窗口的三种指定方式（任选其一）：

| 方式 | 说明 |
|---|---|
| 什么都不给（默认） | 用**当前前台窗口** —— 先点一下游戏再启动 |
| `--game Game.exe` | 按进程名找窗口（脚本的第一个参数 / 拖 exe 上去） |
| `--hwnd 0x1234` | 直接给窗口句柄（前端 `mgpu_ui.exe` 就是这么传的） |

`--interp N` = "每两帧真实帧之间插 N 帧"，所以 `--interp 2` 是 3x、`--interp 1` 是 2x。
非 Intel 显卡上要用 3x 以上，必须加 `--unlock-mfg`（见 §7）。

## 6. 配方怎么选

| 取向 | 命令要点 | 输出 | 画质/前提 |
|---|---|---|---|
| **动态逐帧（推荐）** | `--interp 5 --interp-cadence` + 游戏 MV | 35–45 → 120 fps | 每帧按实际经过的 vblank 数取倍率，输出恒定 1 帧/vblank。**前提：`game_capture.ini` 里 `StreamHeight=1440`**（默认值）；不要加 `--fg-of`、`--no-depth`、`--cadence-hold` |
| **UE5 / 高画质（DLAA）** | `--interp 1 --fg-of` + 游戏内 **DLAA**（或最高画质档） | 26 → 52 fps（4K DLAA） | **实测最干净**：伪影的主因是上采样倍率，1:1 渲染时没有（`docs/findings/UE5_MV_OPTISCALER.md` §8） |
| 3x 满帧（固定） | `--interp 2 --fg-of` | 40 → 120 fps | 光流 MV，无闪烁 |
| 2x 画质优先 | `--interp 1`（+ 游戏 MV） | 60 → 120 fps | 边缘伪影最少 —— **但 UE5 的 MV 流可能是坏的**，见下 |
| 最低开销 | `--interp 2 --no-mv` | 40 → 120 fps | 快转时边缘更脏 |

> **UE5 游戏**（影之刃零 / 黑神话 等）：**DLAA（或最高画质档）+ `--fg-of`** —— 2026-11-09 实测这是唯一"干净"的组合。
>
> 一整轮的排查结果（细节见 `docs/findings/UE5_MV_OPTISCALER.md`）：
> - PBZ 自己的 DLSS 声明是 `0x4B`（`IsHDR｜MVLowRes｜DepthInverted｜AutoExposure`），说明 MV 已去抖动、
>   深度是反向 Z（add-on 现在会读这个 flags，日志 `dlss feature flags ...` + `[bridge] initFlags=..`）；
> - 它交给 NGX 的那条 MV 流**是坏的**：0.5% NaN/inf、17% 饱和到 fp16 上限、与真实位移相关性 ≈0
>   ⇒ 喂它就闪 + 鬼影，`--fg-jittered-mv` / `--fg-mv-scale px` / `--fg-depth-inv` 都救不了；
> - 换成我们自己的光流（`--fg-of`）后**不再闪**，但**伪影依旧** —— 再对比发现：
>   **伪影的主因是上采样倍率**。DLSS 性能档（720p→4K，3 倍）连"不插帧的游戏本身"都有一点伪影，
>   而 **DLAA（1:1 渲染）** 下伪影消失。
>
> 所以：
> - **画质优先**：游戏内 **DLAA** + `--interp 1 --fg-of`（4K DLAA 下 26→52 fps，实测干净）；
> - 必须用 DLSS 性能/平衡档时：伪影随放大倍率走，与开不开 FG 关系没那么大，别把它算在插帧头上；
> - **运动矢量要按游戏试**（见下面的表）：这一条**和 UE5 版本 / 游戏的 NGX 接法有关**，
>   不是一个"UE5 就该用 X"的固定答案。

#### 运动矢量：现在只有三个选项

单位/符号/深度方向改成**读游戏自己的声明**之后（v0.14.0），"反向"这一档已经没有存在意义 ——
所有超分 API 定义的符号都是"上一帧 − 当前帧"，反过来会弄坏游戏自己的 DLSS。所以 UI 只剩：

1. **`游戏 MV`**（默认）—— 不额外花 GPU。**输入已经 ≥ 面板一半时它多半是错的**（插帧没意义），
   优先考虑下面的「关闭」；
2. **`AMD 光流`**（`--fg-of`）—— 游戏 MV 有鬼影/闪烁时用它（UE5 常见）；多一个全屏 pass，
   动态逐帧下更容易 drop；
3. **`关闭`**（`--no-mv`）—— 最便宜。

> `--mv-flip` 仍保留在 `bridge.exe` 命令行上（给极端实验），但 UI 不再提供。

已记录的实测结果：

| 游戏 | 运动矢量 | UI 里选 | 依据 |
|---|---|---|---|
| **影之刃：零** | **AMD 光流** | 运动矢量 → `AMD 光流` | 游戏 MV 流是坏数据（0.5% NaN/inf、17% 饱和到 fp16 上限、与真实位移相关性 ≈0）⇒ 喂它就闪 + 鬼影 |
| **明末：渊虚之羽** | **游戏 MV（反向）** | 运动矢量 → `游戏 MV (反向)` | MV 本身正确，只是符号与 XeFG 的 `previous-current` 约定相反 ⇒ 反向即干净 |
| 黑神话：悟空 | 待记录 | — | 按上面 1→4 的顺序试 |

判断标准只有一条：**慢速平移时看物体边缘有没有闪烁/拖影/鬼影**（反了的表现是边缘沿运动方向撕裂）。
>
> 另外**必须开着「分层窗口」**（`--overlay-layered`，默认已开）：UE5 自认为"未激活"时会
> 丢掉菜单点击（攻击照旧有反应），只有分层窗口下菜单才收得到鼠标。

为什么这样配、哪些思路已经排除（`--cadence-hold`/`--no-depth`/动态逐帧加光流…）的实测数据见
`docs/findings/QUALITY_VARIABLES.md` 与 `docs/findings/CONCLUSIONS.md`。

## 7. 关于 3x 以上（`--unlock-mfg`）

`libxess_fg` 在非 Intel 显卡上默认只允许 **2x**。`--unlock-mfg` 是在**内存里改
`libxess_fg.dll` 的 6 处判断**，把倍率放开到 3x–6x。

- 只对实测的那个版本成立（`libxess_fg 1.3.1.78` / `libxell 1.3.2.10`）；换版本会
  直接拒绝执行（日志里 `*_unexpected_bytes`），不会瞎改；
- **可能违反 Intel 的 EULA**，仅供个人研究；本项目不随仓库分发被修改的 DLL；
- 这个 build 没有 4x 模型，3x 以上是**把 2x 模型跑多遍** ⇒ 固定倍数下用**游戏 MV** 会闪，
  用**光流**不闪。这就是"3x 配光流"的原因。
- **动态逐帧例外**：cadence 会在批次之间改倍率，而光流要在同一张卡上多跑一个全屏 pass，
  实测 drop 明显增加。动态逐帧请用**游戏 MV 或关闭 MV**，并保证 `StreamHeight=1440`。

不想碰这些：用 **2x**（`--interp 1`），完全不需要 `--unlock-mfg`。

## 8. 怎么判断是否正常

| 看什么 | 正常 | 异常含义 |
|---|---|---|
| 左上角 OSD | `x3` 与设定一致，`in` ≈ 锁帧值 | 没有 OSD ⇒ bridge 没起来或没找到窗口 |
| `drop` | 0，或偶发 1–2 | 持续增长 ⇒ 输入端给不够（限帧没生效） |
| `gpu` | 4K 3x 光流约 8–10 ms | 明显超标 ⇒ 插帧卡太弱，降倍率或加 `--scale 0.75` |
| 日志 `gen N/N` | `3/3` 这种成对 | `2/3` ⇒ 生成帧没凑齐 |
| 日志 `[mv]` | 选游戏 MV 时出现 `stream enabled` | 没出现 ⇒ `game_capture.ini` 的 `SkipMv` 不是 0 |
| 画面 | 主体平顺 | 固定 3x/4x 闪烁 ⇒ 改用 `--fg-of`；动态逐帧闪烁 ⇒ 改用 `--no-mv` |
| `drop` 持续增长且开着 MV | 0 | `game_capture.ini` 里 `StreamHeight` 必须是 **1440**（默认）；旧 add-on 或设成 0 时 4K 渲染会传 63 MB/帧，渲染卡顶不住 |

日志位置：配方脚本会在自己目录写 `bridge_*.txt`；前端在状态栏显示最后一条关键日志。

## 9. 常见问题

**Q：游戏里没反应 / add-on 没加载**
看游戏目录有没有 `ReShade.log`；没有 ⇒ 代理 DLL 名字不对（换 `install.bat "...\bin\x64" d3d11` 再试）。
有日志但没数据 ⇒ `ReShadePreset.ini` 里必须启用 `MGPU_DepthTap`（add-on 只在"有启用效果"时被调用，
部署包已经处理，手工装时别删）。

**Q：bridge 启动就退出**
多半是没找到游戏窗口：先让游戏进到世界里再启动，或用 `--game Game.exe` 指定进程名。

**Q：OSD 有了但更卡**
用外部限帧器把输入帧率**锁准**（2x 锁 60、3x 锁 40、4x 锁 30）；`--fg-rt-mode quant` 会自动把实测间隔吸附到整 vblank。

**Q：快速转视角边缘脏 / 闪**
3x/4x 用游戏 MV 会闪 ⇒ 改 `--fg-of`；2x 时用游戏 MV 画质最好。

**Q：A 卡渲染 + N 卡上屏**
前端"帧生成用的 GPU"里选 N 卡。游戏窗口要留在**游戏卡接的那块屏**上；
"成品放到另一块屏"还没实现（见 `ROADMAP.md` 的 `--overlay-monitor`）。

**Q：能不能不装 ReShade？**
不行。add-on 是 ReShade 插件，且必须 6.8（ABI 对齐）。游戏侧那一半就是它。

**Q：游戏更新后失效**
游戏更新可能覆盖 `dxgi.dll` 或重置 `ReShade.ini`——重新跑一次 `install.bat` 即可。

## 10. 卸载

```
game-pack\uninstall.bat "D:\Games\XXX\bin\x64"
```

会把 add-on / ini / shader / 预设删掉，并把备份的 `dxgi.dll`、`ReShade.ini` 还原；
`reshade-shaders` 空目录一并清理。**装上 → 卸掉，目录内容和装之前一致。**
`bin\` 目录直接删掉即可，不留注册表项、不留系统目录文件。

## 11. 合规

- 本包是 **GPL-3.0** 软件，全文见 `LICENSE`；第三方组件署名见 `NOTICE`；
- `libxess_fg.dll` / `libxell.dll` 属于 **Intel**，按其自身许可使用，不是本项目的一部分
  —— 所以**公开下载的包里不含它们**，需自备（见 §3.2）；
  `game-pack\files\ReShade64.dll` 属于 **ReShade**（BSD-3-Clause）；
- `--unlock-mfg` 的风险见 §7 与 `docs/ops/LEGAL.md`；
- 联机游戏请勿使用（会钩游戏进程）。

## 12. 校验

```powershell
cd <包根目录>
Get-FileHash -Algorithm SHA256 -Path (Get-Content .\SHA256SUMS.txt | % { ($_ -split '  ')[1] })
```

或对单个文件用 `certutil -hashfile <文件> SHA256`，与 `SHA256SUMS.txt` 比对。

