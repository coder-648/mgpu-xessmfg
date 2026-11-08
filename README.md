# mgpu —— 把"渲染卡"和"帧生成卡"拆到两块 GPU 上

游戏照常用**自己的**卡渲染 + 超分（DLSS / FSR / XeSS），ReShade add-on 把
运动矢量 / 深度写进共享显存，另一个进程 `bridge.exe` 在**另一块卡**上用
**XeSS-FG** 插帧，再通过覆盖窗口上屏。

A 卡 + N 卡混插也能跑：渲染在 3080 Ti 上、帧生成在 6750 GRE 上（实测环境）。

```
游戏 ──► 超分 ──► game_capture.addon64 ──► 共享显存 + 跨卡 fence ──► bridge.exe ──► 屏幕
        (DLSS/FSR/XeSS)   (D3D12, 渲染卡)                          (XeSS-FG, 另一块卡)
                                                                        ▲
                                                              mgpu_ui.exe（前端/热键）
```

为什么必须是两个进程、协议长什么样：`docs/design/ARCHITECTURE.md`、
`docs/design/PROTOCOL.md`。

## 5 分钟上手

> 想直接拿成品包？`./scripts/make_release.sh` 会编译三件产物并打出
> `dist/mgpu-<版本>.zip`（含 `bin/`＋`game-pack/`＋`docs/`＋校验值），
> 用法写在包里那份 `README.md`（= [`docs/ops/USAGE.md`](docs/ops/USAGE.md)）。

> 前提：Windows 10 1809+、两块 GPU、游戏支持 DLSS / FSR2+ / XeSS。
> 构建见 `docs/ops/BUILD.md`；下面假设你已经有 `build/` 里的三件产物。

**① 装进游戏** —— 用打包好的部署包，把游戏的 `.exe` **拖到 `install.bat` 上**：

```bash
./scripts/make_addon_package.sh        # -> dist/mgpu_reshade_pack.zip
```

装完游戏目录里会有 `dxgi.dll`(ReShade 6.8)、`game_capture.addon64`、
`game_capture.ini`、`mgpu_depth_tap.fx`。全部可逆（`uninstall.bat`）。
细节与三个必踩的坑：`docs/ops/DEPLOY.md`。

**② 游戏内设置**

- **关掉**游戏自带的帧生成；
- 抗锯齿/超分选 **DLSS** 或 **FSR**（XeSS 也行，见 `docs/design/INPUT_SOURCES.md`）；
- 用**外部限帧器**（RTSS）把游戏锁到目标输入帧率 —— 消费者侧锁帧做不到这件事。

**③ 起 bridge**（在 `bridge.exe` 所在目录，推荐直接用配方脚本）

```bat
run_3x_of.bat Game.exe          rem 3x 满帧，推荐
```

或者用前端：双击 `mgpu_ui.exe`（要和 `bridge.exe` 同目录），选窗口 → 选倍率 → 开始，
热键开关。界面与每个开关的对应关系：`docs/ops/UI.md`。

看到 `gen 3/3`（生成帧配对成功）就算通了。日志判据：`docs/findings/FINAL_CONFIG.md` §6。

## 定稿配方

| 取向 | 命令 | 输出 | 备注 |
|---|---|---|---|
| **3x 满帧（推荐）** | `--interp 2 --fg-of` | 40 → 120 fps | 光流 MV、无闪烁、`gpu` ≈ 9 ms |
| 2x 画质优先 | `--interp 1`（+ 游戏 MV，`SkipMv=0`） | 60 → 120 fps | 边缘伪影最少 |
| 最低开销 | `--interp 2 --no-mv` | 40 → 120 fps | 快转时边缘更脏 |

可执行版本在 `scripts/recipes/`：`run_3x_of.bat` / `run_2x_quality.bat` /
`run_3x_nomv.bat` / `run_dump.bat`。

倍率与输入帧率必须让**输出间隔等于整数个 vblank**，否则"不卡但不匀"。
`--interp 2` = 3x，非 Intel 显卡需要 `--unlock-mfg`（**读 `docs/ops/LEGAL.md` 再用**）。

## 参数速查

两张表：**前端（UI）** 和 **游戏侧（ReShade add-on）**。命令行开关全集在
`docs/findings/FINAL_CONFIG.md` §4，这里只列你日常会碰的。

### A. `mgpu_ui.exe`（前端）

把 `mgpu_ui.exe` 放在 **`bridge.exe` 同目录**，双击运行。界面上每改一项，它拼出的
`bridge.exe` 命令行都会变 —— 卡片上"启动命令"那一行就是实时回显，**所见即所传**。

| 卡片 | 行 | 取值（默认 **加粗**） | 传给 bridge |
|---|---|---|---|
| 帧生成 | 类型 | `XeFG 1.3.1 (libxess_fg)`（只读） | — |
| | 模式 | **固定** / 动态（实验） | 动态时 `--dyn-mfg` |
| | 倍数 | 2x / 3x / **4x** / 5x / 6x | `--interp N-1` |
| | 输出 | **1:1 原生** / 5K→4K (0.75x) / 半分辨率 | `--den 1` / `--scale 0.75` / `--den 2` |
| | 运动矢量 | **关闭** / 游戏 MV / 游戏 MV(反向) / AMD 光流（3x 推荐） | `--no-mv` / （不带）/ `--mv-flip` / `--fg-of` |
| 捕获 | 捕获 API | `WGC`（只读） | — |
| | 帧池深度 | 2..6，默认 **4** | `--pool N` |
| | 最大帧延迟数 | 1..3，默认 **1** | `--latency N` |
| GPU / 显示器 | 帧生成用的 GPU | **自动**（bridge 按显示器/避开游戏卡选）或列表里任一块 | `--gpu N`（自动时不传） |
| | 输出显示器 | 只读：当前桌面模式 | — |
| 叠加层 | 分层窗口 (A/B) | 默认 **开**（菜单才收得到点击） | `--overlay-layered` |
| | 输入转发 (A/B) | 默认关（与上面互斥） | `--relay-input` |
| | 单行 OSD | 默认 **开** | 关掉时 `--no-osd` |
| 启动 | 开关热键 | 6 个预设 + 自定义，默认 **Ctrl+PageUp** | — |
| | 建议锁帧 | 只提示（算出 锁帧 × 倍数 = 上屏帧率），**不改**别人的限帧器 | — |
| | 启动命令 | 只读回显 | — |

> 「高级」卡片（FOV / 输入间隔）**已从界面移除**：XeFG 本来就不读合成投影（§5.16），
> 而 `frameRenderTime` 现在由 `--fg-rt-mode quant` 自动吸附到整 vblank，
> 比手填输入间隔更准。两项都还留在 `bridge.exe` 命令行里。

它还会**固定**发出这几个参数，界面不暴露：
`--unlock-mfg --fg-ui none --fg-proj-rz --fg-rt-mode quant` + `--hwnd 0x…`（目标窗口）。

要点：

- **限帧要你自己做**（RTSS 锁到 目标上屏帧率 ÷ 倍数）；UI 只是把数字算给你看。
- 热键默认作用于**前景窗口**（鼠标点进游戏里按就行），也可以钉死某个配置里的窗口。
- 热键：点一下开关 → 启动/停止；**Ctrl+点击**进按键捕捉（Esc 取消）。
- 配置存在 `mgpu_ui.ini`（与 exe 同目录），每个配置档独立保存全部设置 + 目标窗口。
- `mgpu_ui.ini` 里还留着 `jitter_zero` / `tearing` / `sdr` 等旧键，但**界面已不暴露、
  命令行也不再发送**（`--fg-jitter-zero` 已弃用：jitter 一律用游戏自己的）。
- 界面与实现细节：`docs/ops/UI.md`。

### B. ReShade / add-on（游戏侧）

add-on 读**自己旁边**的 `game_capture.ini`（即 `<GAME_DIR>\game_capture.ini`）。
部署包会放一份调好的，正常不用改。

| 键 | 取值（默认 **加粗**） | 说明 |
|---|---|---|
| `Input` | **auto** / ngx / ffx / xess / depth | 输入源，`auto` = 谁先给数据用谁。见 `docs/design/INPUT_SOURCES.md` |
| `Color` | **0** / 1 | `0` = 不做颜色跨卡拷贝（颜色由 bridge 用 WGC 抓屏）；`1` = 拷后台缓冲 |
| `DepthConvert` | **1** / 0 | 把深度转成 R32_FLOAT 再共享。**别设 0**，缺这个键时旧版会静默失效 |
| `SkipMv` | **0** / 1 | `1` = 永不产出 MV；`0` = 听 bridge 的请求位。**UI 里选"游戏 MV"必须 `0`** |
| `StreamPercent` | **100** / >100 | `>100` = MV/深度先缩小再跨卡（`150` → 2160p 缩到 1440p） |
| `StreamAlign` | **1** / 0 | 行对齐处理，别乱改 |
| `Log` | **0** / 1 | add-on 日志（也可用环境变量 `MGPU_CAPTURE_LOG=1`）；正常玩不写文件 |
| `SlotPolicy` | **0** / 1 | `0` = 没空槽就丢帧；`1` = 等 `SlotWaitUs` 微秒。**实测 `1` 无收益**，遗留开关 |
| `SlotWaitUs` | **2000** | 仅 `SlotPolicy=1` 生效 |
| `RenderWidth` / `RenderHeight` | 不设 | `≥256` 才生效，只在游戏上报渲染分辨率错误时强制 |
| `MvNormalize` / `MvFlip` | **0** / 0 | MV 单位/符号修正（一般用 bridge 的 `--mv-flip`，别动这里） |

ReShade 侧的三条硬要求（部署包已处理，手工装时必看）：

1. **ReShade 必须是 6.8.0**（`dxgi.dll`）—— add-on 的 API/ABI 与宿主对齐，换大版本要重编；
2. **至少启用一个效果** —— add-on 每帧逻辑跑在 `reshade_begin_effects` 里，没有启用效果
   ReShade 就不调它（部署包的 `MGPU_DepthTap` 空转效果就是干这个的）；
3. **游戏自带帧生成必须关，超分选 DLSS / FSR / XeSS**（TSR / FSR1 / 原生 TAA 拿不到 MV）。

完整占位符与环境变量（`MGPU_STAGE`、`RESHADE_SRC`…）见 `docs/ops/CONFIG.md`。

## 已经确认的事（省你几天）

- XeFG **确实消费运动矢量**（早期"死输入"的结论是错的）；
- **深度和投影/FOV 对画质没有可测量的影响**，别在这上面花时间；
- 3x 的闪烁来自"MFG 补丁把 2x 模型跑多遍"，不是你的配对写错了 ⇒ 3x 用光流；
- **节拍是 vblank 算术**：`--fg-rt-mode quant` 把实测输入间隔吸附到整 vblank
  （2x@60 → 16.67 ms、3x@40 → 33.3 ms），输出正好 1.00 vblank/帧，不做这件事就会有周期性丢帧。

完整变量表与实测数据：`docs/findings/QUALITY_VARIABLES.md`、
`docs/findings/CONCLUSIONS.md`。

## 已知限制

| 限制 | 说明 |
|---|---|
| 游戏自带的帧生成必须关 | 三层插值没人想要 |
| TSR / FSR1 / 原生 TAA | 不支持，优雅降级（画面正常、无 MV/深度，日志 15 s 内提示） |
| 消费者侧锁帧 | 不生效，必须用外部限帧器 |
| 5K 原生输出 | 只能 2x@30 ≈ 60 fps；`--scale 0.75` 是推荐折衷 |
| `--unlock-mfg` | 二进制补丁，可能违反 XeSS EULA，仅供个人研究 —— `docs/ops/LEGAL.md` |
| 组合 4（A 卡渲染 + N 卡上屏） | 本机未跑通，正解在 `ROADMAP.md` |

## 许可

**GPL-3.0-or-later**（`LICENSE`），Copyright (C) 2026 bilibili夕阳虹舞。
第三方组件逐项署名见 `NOTICE`。
两条必须先读的：`docs/ops/LEGAL.md`（GPL 由来、`--unlock-mfg` 声明、XeSS SDK
不可再分发）。

## 目录

| 路径 | 内容 |
|---|---|
| `docs/README.md` | **文档索引 —— 从这进** |
| `docs/ops/USAGE.md` | **完整使用说明**（安装 / 游戏内设置 / 三份配方 / 判读 / 排障 / 卸载），也是发布包里的 `README.md` |
| `docs/design/` | 架构、协议 v10、输入源、厂商兼容计划 |
| `docs/ops/` | 构建、部署、配置（占位符/ini）、UI、依赖、合规 |
| `docs/findings/` | **结论先行**、画质变量表、踩坑清单、实验档案、里程碑 |
| `src/game_capture/` | add-on：钩超分 API，写共享槽 |
| `src/bridge/` | 消费端：XeFG + XeLL + WGC + 覆盖窗口 |
| `src/mgpu_ui/` | 前端（Win32 + GDI，无第三方库） |
| `scripts/recipes/` | 四份能直接跑的配方脚本 |
| `docs/findings/ARCHIVED_EXPERIMENTS.md` | 已移除的历史实验脚本：参数 + 结论（可追溯"哪次测出来的"） |
| `tests/` | 各里程碑的验证程序（不参与正式构建） |
| `packaging/` | 部署包源文件（安装器/卸载器/预设） |

> 命令必须**单行**（换行会丢 shell 结束标记），长命令用 `./scripts/rcmd.sh`，
> 详见 `docs/findings/TOOLING_NOTES.md`。
