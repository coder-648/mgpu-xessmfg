# mgpu —— 把"渲染"和"帧生成"拆到两块 GPU 上

> 游戏照常用**自己的卡**渲染 + 超分（DLSS / FSR / XeSS），
> 运动矢量与深度走共享显存，另一块卡专职用 **XeSS-FG** 插帧，再通过覆盖窗口上屏。
> 实测环境：**RTX 3080 Ti 渲染 + RX 6750 GRE 插帧，4K / 120 Hz**。
> A 卡 + N 卡混插能用，**单卡也能用**（插帧和渲染落在同一块卡上）。

```
游戏 ──► 超分 ──► game_capture.addon64 ──► 共享显存 + 跨卡 fence ──► bridge.exe ──► 屏幕
       (DLSS/FSR/XeSS)  (D3D12, 渲染卡)                              (XeSS-FG, 另一块卡)
                                                                          ▲
                                                                mgpu_ui.exe（前端 / 热键 / 状态）
```

---

## 亮点

| 亮点 | 说明 |
|---|---|
| **双卡分工** | 插帧不抢游戏的 GPU：游戏帧率不降，插出来的帧在另一块卡上算。也可以用单卡只借 XeFG 的能力 |
| **2x – 6x 倍率** | 2x 是原生；3x–6x 靠 `--unlock-mfg`（**内存补丁，仅供个人研究**，见 §许可）。40 → 120、30 → 120 都能整拍 |
| **两种模式** | **固定倍数**（稳定、可预测）与**动态逐帧**（每帧按实际经过的 vblank 数取倍率，输入 35–45 fps 抖动时也能把 120 Hz 填满，实测空刷新 **1.3%**） |
| **整流输出** | 输出恒定 1 帧/vblank；`--fg-out-hz` 还能把"120 Hz 面板但只想要 60 fps"变成 2x 而不是 4x |
| **不碰你的画质设置** | 不改游戏文件、不从渲染管线取色（颜色走 WGC 抓屏），add-on 只**读**超分阶段已经算好的 MV/深度 |
| **带宽自己会喊** | MV + 深度默认压到 1440p（**28 MB/帧**，而不是 63），把渲染卡的余量留给游戏；日志 `bus:` / `headroom:` 直接告诉你超没超 |
| **可诊断** | `breakdown` / `bus:` / `arr(vb):` / `out:` 一整套计数器，`--dump` 抓成对帧做离线逐像素分析；每个结论都有实测出处（`docs/findings/`） |
| **纯本地、无服务** | 三个产物（两个 exe + 一个 add-on DLL），无驱动、不注入其他进程、无网络；全部可逆卸载 |
| **GPL-3.0** | 协议、shader、实验记录全在仓库里，欢迎改和提 |

> 不吹的部分写在下面「已知限制」里 —— 这个项目对"做不到的事"的记录比对"能做到的事"更详细。

---

## 三个组件，各管一段

### 1. `game_capture.addon64` —— 游戏侧（ReShade add-on）

跑在**游戏进程**里（也就是渲染卡上），挂在超分库（NGX / FfxApi / XeSS）的输出阶段，
把这一帧的**运动矢量 + 深度**（可选颜色）写进共享显存，并用跨卡 fence 通知另一侧。

- 依赖 **ReShade 6.8**（`dxgi.dll`），且必须**启用至少一个效果**（部署包带的
  `MGPU_DepthTap.fx` 就是干这个的，空转不耗性能）；
- 零配置可用：部署包会放一份调好的 `game_capture.ini`。

**`game_capture.ini`（放在 add-on 旁边，即游戏 exe 同级目录）**

| 键 | 默认 | 作用 | 建议 |
|---|---|---|---|
| `Input` | `auto` | 输入源：`auto` / `ngx`(DLSS) / `ffx`(FSR) / `xess` / `depth` | 保持 `auto` |
| `StreamHeight` | **`1440`** | 跨卡流（MV+深度）的**高度上限**，宽度按宽高比算 | 4K 游戏别改。`0` = 跟随渲染尺寸（63 MB/帧，容易 drop）；`1080` 再省一半；`2160` = 全分辨率 |
| `StreamPercent` | `100` | 在 `StreamHeight` 之上再按比例缩（`150` = 渲染的 2/3） | 一般不用动 |
| `SkipMv` | `0` | `1` = 永不产出 MV；`0` = 听 bridge 的请求（选"游戏 MV"必须 `0`） | 保持 `0` |
| `DepthConvert` | `1` | 深度转 R32_FLOAT 再共享 | **别设 0**，缺这个键旧版会静默失效 |
| `Color` | `0` | `1` = 额外把后台缓冲也拷过去 | 保持 `0`（颜色由 bridge 抓屏，开 1 是纯成本） |
| `StreamAlign` / `MvNormalize` / `MvFlip` | `1` / `0` / `0` | 对齐与 MV 单位/符号修正 | 别乱改 |
| `Log` | `0` | 写 `game_capture.log` | 排查时设 `1` |

### 2. `bridge.exe` —— 帧生成侧（真正干活的进程）

WGC 抓帧 → 跨卡取 MV/深度 → **XeSS-FG** 插帧 → DirectComposition 覆盖窗口上屏。
同时负责节拍（整 vblank）、OSD、热键响应和全部诊断输出。

**日常会用到的参数**

| 参数 | 作用 | 备注 |
|---|---|---|
| `--game Game.exe` / `--hwnd 0x…` | 指定目标窗口 | 配方脚本传第一个参数；前端传 hwnd |
| `--interp N` | 两帧真实帧之间插 N 帧（`2` = 3x、`1` = 2x） | 3x 以上需 `--unlock-mfg` |
| `--interp-cadence` | **动态逐帧**：按实际经过的 vblank 数逐帧决定倍率 | 把 `--interp` 当容量上限用（推荐 `--interp 5`） |
| `--cadence-smooth N` | 决定倍率前先对"到达间隔"做 N 帧 EMA（默认 `8`） | 输入抖动大时**别关**（关了倍率会跟着抖 1:1） |
| `--fg-out-hz N` | 输出帧率上限（`0` = 面板刷新） | 最低 2x；够不到会打 warning |
| `--fg-rt-mode ema\|raw\|median\|quant\|auto` | 喂给 XeFG 的 `frameRenderTime` 用哪种估计 | 默认 `quant`（吸附整 vblank）；前端用 `auto` |
| `--mv-flip` / `--no-mv` | 游戏 MV 反向 / 不用 MV | UI 里对应"运动矢量"下拉 |
| `--fg-of` | 用 **AMD 光流**代替游戏 MV | 固定 3x/4x 首选（不闪）；**动态逐帧别用**（同卡多一个 pass ⇒ drop 更多） |
| `--no-depth` | 不传深度 | **别用**：disocclusion 会渗色 |
| `--unlock-mfg` | 内存里改 `libxess_fg` 打开 3x–6x | 仅供个人研究，见 §许可 |
| `--den N` / `--scale F` | 生成分辨率 = 抓到的 1/N / 上屏缩放 F | `--den 2` 省算力；`--scale 0.75` 是 5K 的折衷 |
| `--gpu N` | 指定插帧用哪块卡 | 不传则自动（按输出显示器 / 避开游戏卡） |
| `--pool N` / `--latency N` | 帧池深度 / 最大帧延迟数 | 默认 4 / 1；延迟敏感就保持 `1` |
| `--overlay-monitor N` / `--overlay-hidden` | 覆盖窗口放到指定显示器 / 只跑不上屏 | 后者用来隔离"同屏覆盖窗口"造成的抖动 |
| `--overlay-layered` / `--relay-input` | 分层窗口（菜单可点击）/ 输入转发 | 二选一 |
| `--no-osd` | 关掉左上角单行 OSD | |
| `--dump N` | 抓成对帧（颜色/深度/MV）到磁盘 | 排障与 A/B 量化用 |
| `--report N` / `--log-flush` | 统计窗口长度（帧）/ 立刻刷日志 | 调性能时用 |

完整开关（含实验性）在 `docs/findings/FINAL_CONFIG.md` §4。

### 3. `mgpu_ui.exe` —— 前端

选窗口 → 选模式/倍数/输出 → 开始；热键开关（默认 `Ctrl+PageUp`），状态栏显示关键日志。
界面上每一行都对应一个 bridge 参数，卡片底部"启动命令"是**实时回显**，所见即所传。
支持多个单游戏配置档，每档独立保存设置与目标窗口。详见 `docs/ops/UI.md`。

### 4. `game-pack/` —— 安装器

把游戏 `.exe` **拖到 `install.bat` 上**即可：装 ReShade 6.8 + add-on + 调好的 ini + 空转效果，
`uninstall.bat` 一键还原。三个必踩的坑写在 `docs/ops/DEPLOY.md`。

---

## 按场景选配置

| 你的情况 | 怎么配 | 输出 | 实测 |
|---|---|---|---|
| **游戏 35–45 fps 抖、不想锁帧** | 动态逐帧：`--interp 5 --interp-cadence` + 游戏 MV | 120 fps（1 帧/vblank） | 空刷新 **1.3%**，倍率切换 12 次/秒 |
| **能稳定 40 fps，想要最稳** | 固定 3x：`--interp 2 --unlock-mfg`，MV 用游戏 MV 或 `--fg-of` | 120 fps | 空刷新 **0.1–0.3%** |
| **能稳定 60 fps，画质优先** | 2x：`--interp 1` + 游戏 MV | 120 fps | 边缘伪影最少（光流 0.3 ms 可省掉） |
| **能稳定 30 fps** | 4x：`--interp 3 --unlock-mfg` | 120 fps | 空刷新 2.6%（改用动态逐帧 + `--interp 5` 容量可到 0.6%） |
| **120 Hz 面板但只想要 60 fps** | 动态逐帧 + `--fg-out-hz 60` | 60 fps | 输入 30 时自动 2x，不是 4x |
| **5K / 高分辨率** | 加 `--scale 0.75` | 按比例 | 5K 原生只有 2x@30 ≈ 60 fps 跑得动 |
| **预算最紧 / 插帧卡较弱** | `--interp 2 --no-mv` | 120 fps | 少一个 pass；快转时边缘略脏 |
| **只有一块显卡** | 一样能跑 | — | 插帧与渲染同卡，收益比双卡小很多 |
| **要抓数据做离线分析** | `--dump 200` | — | 输出 `f<NN>_colour/depth/mv.raw` + `_meta.txt` |

现成配方在 `scripts/recipes/`；发布包里是 `bin\` 里的 `.bat`，双击即可
（`5 - smoothest (cadence).bat` 就是上表第一行）。

### 游戏引擎备注

| 引擎 | 备注 |
|---|---|
| **UE5**（影之刃零、黑神话等） | **不建议用运动矢量** —— 加 `--no-mv`（UI：运动矢量 = 关闭）。UE5 的 MV 是在**抖动后**的画面上算的，且不同项目常混用像素/UV 单位与反向矢量，3x/4x 下容易闪和边缘拖影；关掉还少一个全屏 pass + 每帧一半的跨卡流量，反而更稳（PBZ 实测 `gpu` 10.5–21.5 ms、`skip` 0）。<br>**必须打开「分层窗口」**（`--overlay-layered`，UI 里默认已开）：UE5 在自认为"未激活"时会**丢掉菜单点击**（攻击走 raw input 照旧有反应，表现就是"能打、点不动菜单"），只有分层窗口下游戏菜单才收得到鼠标。 |
| 其它（Unity / 自研 / DX11 老引擎） | 游戏 MV 一般可用。边缘拖影先试 `--mv-flip`（反向），再退到 `--no-mv`；少数游戏只给深度、不给 MV，那种情况直接 `--no-mv` 即可。 |

---

## 5 分钟上手

1. **装进游戏**：把游戏 exe 拖到 `game-pack\install.bat` 上（或先跑 `./scripts/make_addon_package.sh`）；
2. **游戏内设置**：关掉游戏自带帧生成；超分选 **DLSS / FSR2+ / XeSS**；用外部限帧器（RTX 驱动的
   Max Frame Rate / RTSS）锁到"目标上屏帧率 ÷ 倍数" —— 不想锁就直接用动态逐帧；
3. **起 bridge**：双击 `bin\5 - smoothest (cadence).bat`，或打开 `mgpu_ui.exe` 选窗口 → 开始；
4. **确认通了**：OSD 显示 `x3` 且 `drop` 不涨；日志 `gen 3/3`（生成帧成对）。

判读与排障：`docs/ops/USAGE.md` §8–§9。

---

## 已知限制（先看这里，省你几天）

| 做不到 | 原因 |
|---|---|
| 游戏自带帧生成 + 本工具同时开 | 三层插值，画面必炸 |
| TSR / FSR1 / 原生 TAA | 拿不到运动矢量，质量不可接受（会优雅降级并在日志提示） |
| 在消费者侧锁帧 | 无效；必须用外部限帧器 |
| 5K 原生高倍率 | 只有 2x@30 ≈ 60 fps 跑得动 |
| A 卡渲染 + N 卡输出的完整组合 | 本机未跑通；`--overlay-monitor` 要求两块屏都接同一块卡（见 `ROADMAP.md`） |
| 零延迟 | 实测端到端 ~40–70 ms，随倍率/配置变化 |

**这些已经试过并确定没用**（别再走一遍）：`--cadence-hold`（拿丢帧换稳定）、
`--fg-cad-rt`（pacer 自激）、动态逐帧加 `--fg-of`（同卡多一个 pass）、`--no-depth`（渗色）、
固定 3x 配 44–45 fps 输入（45×3 > 120，必然超额）。理由与数字见 `CHANGELOG.md` 和
`docs/findings/PERF_BASELINE.md`。

---

## 欢迎参与

这个项目是从"能不能把插帧拆到副卡上"一路试出来的，**大部分结论来自失败的实验**，
所以特别欢迎：

- **换游戏验证**：UE5 / 自研引擎的 MV 单位与深度来源各不相同，
  `docs/design/INPUT_SOURCES.md` 写了怎么加一个输入源；跑通了请带 `--dump` 出来的成对帧和日志；
- **换硬件验证**：A 卡渲染 + N 卡输出、Intel Arc、核显、双 A 卡……任何组合的实测都值钱
  （`docs/findings/ENV_REPORT.md` 是登记处）；
- **打包 / 前端 / 文档**：UI 是纯 Win32 + GDI（无第三方库），加一行开关很容易；
- **提 bug 请带三样**：`bridge` 日志（含 `breakdown` 与 `bus:` 行）、`game_capture.ini` 内容、
  游戏内的超分设置。有这三样基本能复现。

不要提交：`libxess_fg.dll` / `libxell.dll`（Intel 的，见 `docs/ops/LEGAL.md` §3）、
被 `--unlock-mfg` 改过的二进制、任何游戏资源。

**文档从 `docs/README.md` 进**：结论先行 `findings/CONCLUSIONS.md`、
性能基线 `findings/PERF_BASELINE.md`、踩坑清单 `findings/FINAL_CONFIG.md` §5。

---

## 许可

**GPL-3.0-or-later**（`LICENSE`），Copyright (C) 2026 bilibili夕阳虹舞。第三方组件逐项署名见 `NOTICE`。

两条务必先读（`docs/ops/LEGAL.md`）：

1. 本项目**不分发** XeSS 运行时的 `libxess_fg.dll` / `libxell.dll`（Intel 的许可），
   公开版包里不带，请自行从 XeSS SDK 获取；
2. `--unlock-mfg` 是在**内存里**修改这些库的 6 处判断以打开 3x–6x，
   **可能违反 Intel EULA，仅供个人研究**，风险自负。

---

## 目录

| 路径 | 内容 |
|---|---|
| `CHANGELOG.md` | **每个版本的变化 + 实测依据** |
| `docs/README.md` | **文档索引 —— 从这进** |
| `docs/ops/USAGE.md` | 完整使用说明（安装 / 游戏内设置 / 配方 / 判读 / 排障 / 卸载），也是发布包里的 `README.md` |
| `docs/design/` | 架构、协议 v10、输入源、厂商兼容计划 |
| `docs/ops/` | 构建、部署、配置（占位符 / ini）、UI、依赖、合规 |
| `docs/findings/` | 结论先行、画质变量、性能基线、踩坑清单、实验档案、里程碑 |
| `src/game_capture/` | add-on：钩超分 API，写共享槽 |
| `src/bridge/` | 消费端：XeFG + XeLL + WGC + 覆盖窗口 |
| `src/mgpu_ui/` | 前端（Win32 + GDI，无第三方库） |
| `scripts/recipes/` | 能直接跑的配方脚本 |
| `packaging/` | 部署包源文件（安装器 / 卸载器 / 预设） |
| `tests/` | 各里程碑的验证程序（不参与正式构建） |

> 贡献前建议先读 `docs/findings/TOOLING_NOTES.md`（本仓库的命令行约定）和
> `docs/ops/BUILD.md`（三件产物怎么编）。

