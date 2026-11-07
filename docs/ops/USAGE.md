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
│   ├─ 1 - 3x optical flow.bat    ★ 推荐配方（40 fps → 120 fps）
│   ├─ 2 - 2x quality.bat         画质优先（60 fps → 120 fps）
│   ├─ 3 - 3x lowest cost.bat     预算最紧时用
│   ├─ 4 - dump frames.bat        抓帧做离线分析（排障/对比用）
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

## 5. 第一次运行

### 5.1 用前端（推荐）

1. 先启动游戏，进到游戏世界里；
2. 双击 `bin\0 - UI.bat`（或直接双击 `mgpu_ui.exe`）；
3. "运动矢量"选 **AMD 光流**、"倍数" 选 **3x**、"输入间隔" 保持 **自动**；
4. 点标题栏右上角的 **启动**（或按热键，默认 `Ctrl+PageUp`）；
5. 屏幕左上角出现单行 OSD 就说明通了：

```
120 fps   x3   in 40.0 fps   gpu 9.0 ms   game 25.0 ms   lock 40   drop 0
```

（上屏帧率 / 倍数 / 游戏输入帧率 / 等 GPU 的时间 / 游戏帧间隔 / 限帧 / 丢帧）

界面上改任何一项，卡片里"启动命令"都会同步变化 —— 那就是实际执行的命令行。
界面说明见 `docs/ops/UI.md`。

### 5.2 用命令行 / 配方脚本

`bin\` 里有 4 份脚本，**直接双击就能跑**（不给参数时 bridge 处理"当前前台窗口"，
所以先点一下游戏窗口再双击脚本），也可以把**游戏的 exe 拖到脚本上**来指定目标。

| 脚本 | 用途 |
|---|---|
| `1 - 3x optical flow.bat` | 3x 满帧（推荐） |
| `2 - 2x quality.bat` | 2x 画质优先 |
| `3 - 3x lowest cost.bat` | 预算最紧 |
| `4 - dump frames.bat` | 抓 colour/depth/MV 做离线分析 |

等价的手打命令：

```bat
rem 3x 满帧（推荐）：光流 MV + 输入间隔 33.3 ms
bridge.exe --den 1 --pool 4 --interp 2 --fg-of --input-ms 33.3 ^
           --unlock-mfg --fg-ui none --fg-proj-rz --fg-rt-mode median

rem 2x 画质优先：用游戏自己的 MV（game_capture.ini 的 SkipMv=0，默认就是）
bridge.exe --den 1 --pool 4 --interp 1 --fg-ui none --fg-proj-rz --fg-rt-mode median

rem 最低开销：不用任何 MV
bridge.exe --den 1 --pool 4 --interp 2 --no-mv --input-ms 33.3 ^
           --unlock-mfg --fg-ui none --fg-proj-rz --fg-rt-mode median
```

目标窗口的三种指定方式（任选其一）：

| 方式 | 说明 |
|---|---|
| 什么都不给（默认） | 用**当前前台窗口** —— 先点一下游戏再启动 |
| `--game Game.exe` | 按进程名找窗口（脚本的第一个参数 / 拖 exe 上去） |
| `--hwnd 0x1234` | 直接给窗口句柄（前端 `mgpu_ui.exe` 就是这么传的） |

`--interp N` = "每两帧真实帧之间插 N 帧"，所以 `--interp 2` 是 3x、`--interp 1` 是 2x。
非 Intel 显卡上要用 3x 以上，必须加 `--unlock-mfg`（见 §7）。

## 6. 三份配方怎么选

| 取向 | 命令要点 | 输出 | 画质 |
|---|---|---|---|
| **3x 满帧（推荐）** | `--interp 2 --fg-of --input-ms 33.3` | 40 → 120 fps | 光流 MV，无闪烁 |
| 2x 画质优先 | `--interp 1`（+ 游戏 MV） | 60 → 120 fps | 边缘伪影最少 |
| 最低开销 | `--interp 2 --no-mv` | 40 → 120 fps | 快转时边缘更脏 |

为什么是这三个组合、"深度和 FOV 其实没用"这类结论的实测数据见
`docs/findings/QUALITY_VARIABLES.md` 与 `docs/findings/CONCLUSIONS.md`。

## 7. 关于 3x 以上（`--unlock-mfg`）

`libxess_fg` 在非 Intel 显卡上默认只允许 **2x**。`--unlock-mfg` 是在**内存里改
`libxess_fg.dll` 的 6 处判断**，把倍率放开到 3x–6x。

- 只对实测的那个版本成立（`libxess_fg 1.3.1.78` / `libxell 1.3.2.10`）；换版本会
  直接拒绝执行（日志里 `*_unexpected_bytes`），不会瞎改；
- **可能违反 Intel 的 EULA**，仅供个人研究；本项目不随仓库分发被修改的 DLL；
- 这个 build 没有 4x 模型，3x 以上是**把 2x 模型跑多遍** ⇒ 用**游戏 MV** 会闪，
  用**光流**不闪。这就是"3x 配光流"的原因。

不想碰这些：用 **2x**（`--interp 1`），完全不需要 `--unlock-mfg`。

## 8. 怎么判断是否正常

| 看什么 | 正常 | 异常含义 |
|---|---|---|
| 左上角 OSD | `x3` 与设定一致，`in` ≈ 锁帧值 | 没有 OSD ⇒ bridge 没起来或没找到窗口 |
| `drop` | 0，或偶发 1–2 | 持续增长 ⇒ 输入端给不够（限帧或 `--input-ms`） |
| `gpu` | 4K 3x 光流约 8–10 ms | 明显超标 ⇒ 插帧卡太弱，降倍率或加 `--scale 0.75` |
| 日志 `gen N/N` | `3/3` 这种成对 | `2/3` ⇒ 生成帧没凑齐 |
| 日志 `[mv]` | 选游戏 MV 时出现 `stream enabled` | 没出现 ⇒ `game_capture.ini` 的 `SkipMv` 不是 0 |
| 画面 | 主体平顺 | 闪烁 ⇒ 3x/4x 用了游戏 MV，改 `--fg-of` |

日志位置：配方脚本会在自己目录写 `bridge_*.txt`；前端在状态栏显示最后一条关键日志。

## 9. 常见问题

**Q：游戏里没反应 / add-on 没加载**
看游戏目录有没有 `ReShade.log`；没有 ⇒ 代理 DLL 名字不对（换 `install.bat "...\bin\x64" d3d11` 再试）。
有日志但没数据 ⇒ `ReShadePreset.ini` 里必须启用 `MGPU_DepthTap`（add-on 只在"有启用效果"时被调用，
部署包已经处理，手工装时别删）。

**Q：bridge 启动就退出**
多半是没找到游戏窗口：先让游戏进到世界里再启动，或用 `--game Game.exe` 指定进程名。

**Q：OSD 有了但更卡**
用外部限帧器把输入帧率锁住（3x 就锁 40）；再检查 `--input-ms`（自动档一般够）。

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

