# 配置速查（当前链路，2026-11-14）

本文只讲**当前链路**的可配置项：

- 游戏侧：`fakerFG.ini`（fakerFG 与 producer 都读它）
- 消费端：`xess_mfg_consumer.exe` 命令行 / `mgpu_ui.exe`
- 构建：环境变量（`MGPU_STAGE` 等）

旧 `game_capture.ini`（ReShade add-on）已归档到
[`archive/CONFIG_game_capture.md`](archive/CONFIG_game_capture.md)。

---

## 1. 路径占位符

| 占位符 | 含义 | 例 |
|---|---|---|
| `<GAME_DIR>` | **直接包含游戏 exe** 的目录 | `D:\Games\XXX\bin\x64` |
| `<TOOLS_DIR>` | consumer + UI + Intel DLL 同目录 | `D:\mgpu\bin` |
| `<STAGE_DIR>` | WSL 构建中转目录，必须 Windows 可见 | `C:\temp\mgpu`（默认） |
| `<DUMP_DIR>` | `--dump` 输出的 `.raw` 目录 | `<TOOLS_DIR>\dump` |
| `<RTSS_DIR>` | RivaTuner Statistics Server 安装目录 | — |

---

## 2. 环境变量（构建/打包）

| 变量 | 作用 | 默认 |
|---|---|---|
| `MGPU_STAGE` | 构建暂存目录（cl 不能用 UNC 工作目录） | `/mnt/c/temp/mgpu` |
| `REBUILD` | `make_release_fg.sh`：`1` = 先重编四件产物 | `0` |
| `MGPU_VERSION` | 发布版本号 | `0.11-<sha>` |
| `SKIP_XESS_DLLS` | `1` = 发布包不带 Intel 的 `libxess_fg.dll` / `libxell.dll` | `0` |

---

## 3. `fakerFG.ini`

位置：游戏 exe 同目录。**只在启动时读一次**；改完要重开游戏。

### 3.1 必看键

| 键 | 默认 | 说明 |
|---|---|---|
| `Mode` | `consume` | `off` / `observe` / `idle` / `local` / `consume`（`bridge` = consume 别名） |
| `StreamPercent` | `150` | 跨卡流尺寸：150≈2/3（4K→2560×1440，28.1 MB）；100=1:1（63.3 MB）；200=1/2；400=1/4。COPY-only 列表会自动回退到 100 |
| `Interpolated` | `1` | `local` 模式每真帧生成几张（1 = 2x）；上限见启动日志 |
| `Publish` | `1` | `0` = 完全不发布/不传输（只用于测量发布路径开销） |
| `Log` | `1` | 写 `<游戏exe>_fakerFG.log`（每行 flush） |
| `InjectReshade` | `0` | 当前链路不需要 ReShade；只有想加别的 shader 时才设 1 |

### 3.2 FG 链路（一般不用动）

| 键 | 默认 | 说明 |
|---|---|---|
| `OwnSwapchain` | `1` | 接管交换链（本链路前提） |
| `RealSwapchainObject` | `1` | 交还 libxess 真链 + vtable 钩子 |
| `FakeDlssgFeature` | `0` | 0 = 拒绝标题自己的 DLSS-G，整帧渲进我们交出的链（链路模式） |
| `WriteOutputReal` | `0` | provider 模式才设 1 |
| `MvResample` | `1` | 全分辨率 MV → depth 尺寸（已同尺寸时自动跳过） |
| `TagAtEvaluate` | `1` | tag 在 Evaluate 上；`MvResample` 只在路径里执行 |
| `ForceBorderless` | `1` | 必须为 1，否则 libxess 判全屏并拒绝插帧 |
| `FgInput` / `FgOutput` | `Upscaler` / `XeFG` | 输入取 DLSS-SR 参数块，输出由 XeFG 呈递 |
| `Passthrough` | `0` | 1 = 不回答、不接管（等效没装） |
| `ArchSpoof` | `1` | 向 Streamline 回答 sm_80+，让 30 系菜单出现 FG |

### 3.3 输入声明 / 诊断

| 键 | 默认 | 说明 |
|---|---|---|
| `MvState` / `DepthState` | `0` | 资源声明状态（0 = 64 = NON_PIXEL_SHADER_RESOURCE） |
| `MvJittered` / `HighResMv` / `DepthInverted` | `0` / `0` / `1` | 覆盖 XeFG init flags（一般不用动） |
| `TagHudless` / `TagUi` | `0` | 是否把 HUDless/UI 交给 XeFG（Upscaler 车道不发） |
| `CameraNear` / `CameraFar` / `VerticalFov` | `0.1` / `100000` / `60` | 合成投影参数（有游戏真矩阵时不用） |
| `DebugView` | `0` | 在画面标记插值帧 |
| `DebugOnlyGenerated` | `0` | 只显示生成帧（最强“是否在生成”的验证） |
| `IgnoreInitChecks` | `0` | 跳过 XeFG 初始化检查（排障用） |

### 3.4 producer 相关

| 键 | 默认 | 说明 |
|---|---|---|
| `StreamPercentMv` | = `StreamPercent` | 只覆盖 MV 流（协议 v15） |
| `StreamPercentDepth` | = `StreamPercent` | 只覆盖深度流 |
| `Mode=consume` | — | 生产端只发布、不在本卡补帧 |

> `StreamPercentMv=100` + `StreamPercentDepth=200` 是“MV 全分辨率 + 深度半分辨率”，
> 消费端需加 `--upsample`（XeFG 要求两流同尺寸）。

---

## 4. 消费端参数

完整列表以源码为准（`main.cpp` / `state_overlay.cpp`）；这里按用途分组。

### 4.1 产品路径

| 参数 | 说明 |
|---|---|
| `--game <exe>` / `--hwnd 0x…` | 目标窗口 |
| `--interp N` | 固定倍率：N=1 → 2x |
| `--interp-cadence` | 动态逐帧，N 为容量上限 |
| `--fg-out-hz N` | 输出上限（0 = 面板） |
| `--fg-rt-mode …` | `frameRenderTime` 来源（默认 `auto`，UI 用 `auto`） |
| `--pool N` / `--latency N` | WGC 帧池 / 最大延迟 |
| `--den N` / `--scale F` | 生成分辨率 |
| `--sdr` | SDR 输出 |
| `--gpu N` | 指定插帧卡 |
| `--no-osd` | 关闭 OSD |
| `--unlock-mfg` | 解锁 3x–6x（个人研究） |
| `--fg-ui none` | 不合成 UI 纹理 |

### 4.2 输入/画质 A/B

| 参数 | 说明 |
|---|---|
| `--upsample` | 消费卡重建 MV/深度到渲染尺寸 |
| `--stream-auto` | 自动每流 `StreamPercent` |
| `--stream-asym` | MV 1:1 + 深度降档（隐含 `--upsample`） |
| `--no-depth` | 不传深度 |
| `--no-marker` | 关闭 marker 配对 |
| `--no-prefetch-inputs` | 关闭输入预拷 |
| `--pub-fifo` / `--no-pub-fifo` | 默认开：按发布队列顺序消费；关掉后仍拷贝，但按 publication 值匹配 |
| `--no-mv` / `--fg-of` / `--mv-flip` | MV 来源覆盖（排障） |

### 4.3 诊断

`--report N`、`--log-flush`、`--dump N`、`--marker-probe`、`--probe-colour`、
`--list-modes`、`--no-capture-thread`、`--colour-queue N`、`--mv-shadow`、
`--fg-probe`、`--exit-on-focus-loss` 等。默认不要开。

动态逐帧相关（2026-11-14 第三轮）：锚点是 **交给 XeFG 的时点**，每批在 tag 前等待
vblank 槽位（`target_{n+1}=target_n+k×vblank`），批次首尾相接；`--no-fg-pace` 关闭该
pacer 做 A/B。`frameRenderTime` 默认 `k × vblank`（`--fg-cad-rt 0` 回到输入均值）。

**默认策略是 `--cad-policy locked`**（2026-11-15 起）：`k = ceil(target/输入率)`，
每批预算恒为 `k × vblank`，vblank 周期由 DWM 的 `qpcRefreshPeriod` 实测
（本机 8.3332 ms = 120.002 Hz）。配合把游戏锁到 `120/k`（40→3x、30→4x、60→2x），
间距误差约 0.4%、k 恒定、相位 std ≈0.16 ms。
`--pool 2` / `--colour-queue 1` / `--latency 1` 是默认值。

A/B 用的 carry 入口 = `--cad-policy fill --cadence-hold N`：相位进位 + 死区，`N` 就是控制器的
`minDwell`（默认 4，carry A/B 用 8/12/15）。`--cad-switch-loss measure|auto` 默认
`measure`：每个报告窗口用 `expected` vs `GetLastPresentCount` 算出“每次计数切换丢几张”，
只写日志；`auto` 才让控制器据此增减 dwell（真机验证前不要开）。日志里的 `out=` 是
**诚实输出率**，cap-limited（输入低于 target/J）时会低于目标并明确报 `regime=cap-limited`。
固定 3x（锁 40）/4x（锁 30）仍是产品对照入口。


---

## 5. 调试文件（放游戏目录，重启游戏生效）

| 文件 | 作用 |
|---|---|
| `marker_disable.on` | 生产端不画四角 marker（A/B 画质/开销） |
| `marker_repair_off.on` | 消费端不回填 marker（标记留在画面上） |
| `dump_frame.on` | 整帧回读 `frame_dump.raw` / `marker_corners.raw` |

---

## 6. `mgpu_ui.ini`

与 `mgpu_ui.exe` 同目录，保存每个游戏配置档的全部设置与目标窗口；
每次改动即时写盘。旧的 `jitter_zero` / `tearing` / `sdr` / `no_mv` 等键仅用于迁移，
界面不再暴露。详见 [`UI.md`](UI.md)。
