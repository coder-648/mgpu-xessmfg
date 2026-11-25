# mgpu / lock_frame

**把游戏的渲染和帧生成拆到两块 GPU 上。** 游戏在自己的显卡上渲染 + 超分，
`lock_frame` 在另一块显卡上用 XeSS-FG 插帧，再叠加回游戏窗口上屏——
不换显卡，把帧数做到 **2x / 3x / 4x**。

![主界面](docs/bilibili/images/01-main.png)

> 小白图文教程：[`docs/bilibili/使用说明.md`](docs/bilibili/使用说明.md)
> 技术文档索引：[`lock_frame/docs/README.md`](lock_frame/docs/README.md)

| 当前发布 | 共享协议 | 许可证 | 维护状态 |
|---|---|---|---|
| **1.3.3-local.ui**（2026-11-25） | **v34** | GPL-3.0 | 核心功能定稿，只修 issue + 加游戏支持 |

上一版是 [`1.3.0`](lock_frame/docs/RELEASE_NOTES_1.3.0.md)（2026-11-24）；1.3.1 / 1.3.2 是
本地验证与回退，没有单独发布。本版改动与实测见
[`RELEASE_NOTES_1.3.3-local.ui.md`](lock_frame/docs/RELEASE_NOTES_1.3.3-local.ui.md)。

## 它解决什么问题

单卡开启 DLSS 帧生成时，插帧会和游戏抢同一份算力。本工具把两条流水线拆开：

- **渲染卡**：游戏渲染 + DLSS 超分照常进行，只多传三样东西——**MV、depth、UI 支撑域 patch**；
- **副卡**：解码、还原 HUDless、跑 XeFG 插帧、叠加回游戏窗口；
- **颜色不跨卡**：由消费端在同卡上 WGC 捕获，跨卡流量只有上面三项。

跨卡走无损码流（MV 100% 比特精确、depth 150%），硬件不支持时自动退回拷贝路径。
发布包自带 Intel 运行时（`libxess_fg.dll` / `libxell.dll`），解压即用。

## 适合谁 / 不适合谁

| 适合 | 不适合 |
|---|---|
| 有两块显卡（核显 + 独显，或两张独显） | 带反作弊的在线游戏 |
| 游戏支持 **DLSS Frame Generation** | 必须独占全屏的游戏 |
| 能开 **无边框 / 窗口化全屏** | 只有一块显卡又要求零损耗（单卡能跑，但会和游戏抢性能） |
| 单机 / 离线游戏 | |

## 5 分钟上手

1. **解压**发布包到任意可写目录（不要只解压进游戏目录），双击 `lock_frame_ui.exe`。
2. 首次启动看欢迎页，点「知道了，开始」。
3. 「当前游戏」卡片右上角 → **选择游戏** → 「添加 exe…」，选中游戏主程序
   （例如 `Cyberpunk2077.exe`），在列表里点它一下。
4. 回到主界面点 **安装 / 应用配置**：组件装进游戏目录，并按所选模板写入 `fakerFG.ini`。
5. 启动游戏、进画面、开启 **DLSS 帧生成**（无边框 / 窗口化全屏），回到工具点 **刷新**；
   窗口和 producer 就绪后，「启动补帧」按钮亮起，点它。

| 选择游戏 | 高级设置 |
|---|---|
| ![选择游戏](docs/bilibili/images/02-choose.png) | ![高级设置](docs/bilibili/images/03-advanced.png) |

```text
首次启动
  └─ 选择游戏（添加 exe） → 安装 / 应用配置
        └─ 进游戏（无边框 + DLSS FG） → 刷新 → 启动补帧
              └─ 想调参数：操作行的「高级设置」（只改当前游戏）
```

从旧版升级请点一次 **安装 / 应用配置**，让新的 `fakerFG.ini` 模板
（`TransportSlots` / `UiSynthThreshold` / `UiStats`）写进游戏目录；替换
`xess_mfg_producer.dll` 后需要重启游戏。

## 它怎么工作

```text
游戏进程（渲染卡）                         消费进程（副卡）
─────────────────────────                ─────────────────────────
游戏渲染 + DLSS 超分
  └─ DLSS-G evaluate (Streamline)
       └─ fakerFG.dll 记录 MV / depth / HUDless
            └─ xess_mfg_producer.dll
                 ├─ 降采样 + 无损编码 + 跨卡 DMA ──▶ lock_frame.exe
                 │                                   ├─ 解码 / 联合重建
                 │                                   ├─ 还原 HUDless（UI patch）
                 │                                   └─ XeFG 插帧
                 └─ 同卡 WGC 捕获颜色 ───────────────▶  叠加回游戏窗口
```

安装时 UI 会把 `fakerFG.dll` 写成 `dxgi.dll` / `nvngx_dlssg.dll` 两个别名，并把 Intel
运行时放进 `<游戏>\fakerFG\`（游戏进程里的 XeFG 只从这里加载）。原文件都会先备份，
可一键还原。

## 核心功能

| 功能 | 说明 |
|---|---|
| 固定倍率 | 2x / 3x / 4x；120 Hz 面板建议锁 60 / 40 / 30 fps |
| 输入源 | 固定 DLSSG（Streamline tag）；其他值强制回 `DLSSG`，`NoFG` 显式禁用 |
| 逐游戏适配 | 内置 OptiScaler FG 输入表（54 条），按 `--game` 自动套用；`--game-quirks apply/log/off`，UI 内可关 |
| 配置管理 | 一游戏一配置（`profiles\*.ini`），点名字重命名，ini 漂移会提示重新应用 |
| 安装 / 还原 | 一键部署（自动备份 `dxgi.dll`）与一键还原；游戏运行中会拦截 |
| 就绪检查 | 安装、Intel 运行时、游戏窗口、producer、`fakerFG.ini` 五项状态 |
| 游戏侧模板 | `fakerFG.ini`（精简，默认）/ `fakerFG.full.ini`（全参数），UI 内切换 |
| 诊断 | 一键诊断包、内嵌 400 行日志、`pair:` 与 OSD 指标 |
| 界面 | 中 / 英双语、DPI 缩放、键盘操作、UIA 读屏 |

### 1.3.3 的变化

- **显存**：`TransportSlots=16`，CP2077 4K 映射 1773.6 → **1017.6 MB**（−43%）。
- **配对**：环满时只复用安全槽（`reclaimed SAFE slot`），新 run 不再覆盖消费者要用的槽。
- **判活**：consumer 退出 3 s 后 producer 暂停拷贝，不再空转编码。
- **UI mode 4**：5×5 hard edge 设为默认，LUT 直方图按 92% 逐帧衰减（约 12 帧记忆），
  小 UI 移动抖动明显减小。
- **稳定性**：跨卡读等待 `fwd[slot]`、重建结果按 publication 校验、barrier 收尾改 `COMMON`；
  `--d3d-debug` 下 barrier / state 告警 0 条。

## 性能开销（实测）

只有**渲染卡**（游戏所在的那块）的时间影响帧率；消费卡在 3x@40 下还有约 88% 余量，
消费端的解码、unblend、显存优化不会体现在 fps 上。

实测环境：CP2077 4K DLAA、锁 40 fps、3x 输出；渲染 **RTX 3080 Ti**，帧生成 **RX 6750 GRE**，
1.2.9–1.3.0 默认配置。换卡、换游戏、换分辨率都会变。

### 渲染卡占用增量（RTSS）

| 状态 | 占用 | 增量 | 成本归属 |
|---|---:|---:|---|
| 游戏原生（无本工具） | 94% | — | — |
| 开启 DLSS-FG 管线（未接 consumer） | 97.5% | **+3.5 pp** | 开启 FG 管线本身 |
| + MV/depth 传输（降采样、编码、跨卡 DMA）+ WGC 捕获/配对 | 100% | **+2.5 pp** | 本工具固定开销 |
| + UI 合成（默认，patch-only mode 4） | 102.5% | **+2.5 pp** | `UiPatch=0` 可回退 |

合计约 **+8.5 pp**（94% → 102.5%）。其中 3.5 pp 是游戏开 DLSS FG 本身就要付的，与传输无关；
MV/depth 与 WGC 合计 2.5 pp；UI patch 约 2.5 pp（比 1.2.x sparse UI 的约 3 pp 少，
因为 stream 3 / mode 5 已关闭）。

### 各 pass 时间（`prof:`，UI 合成开启，4K/帧）

| pass | 时间 | 占 25.6 ms 预算 |
|---|---:|---:|
| MV 编码（100%，比特精确） | 0.7–1.3 ms | 2.6–5.0% |
| depth 编码（150%） | 0.79 ms | 3.1% |
| UI 编码（全分辨率） | 0.61 ms | 2.4% |
| UI synth（`P − H`） | 0.20 ms | 0.8% |
| **合计** | **2.9 ms** | **≈11%** |

这些是名义 GPU 时间，会与游戏剩余工作重叠：MV/depth 名义 1.5–2.1 ms，实际只增加
**2.5 pp（≈0.6 ms）**；UI 名义 0.81 ms，实际增加 **2.5 pp**。不要用毫秒直接加总算占用。

从 1.2.7 到 1.2.9：depth 编码 2.05 → 0.79 ms（−61%）；UI 增量 5 pp → 3 pp
（sparse UI codec，105% → 103%）。

### 开销来源

- **DLSS-FG 管线（+3.5 pp，固定）**：present 拦截、tag 接管与记录等，不属于 MV/depth 传输。
- **MV/depth 传输（+2.5 pp，固定）**：两个全屏降采样 + 无损编码 + 跨卡 DMA，以及同卡
  WGC 捕获与配对。旧的可选跨卡颜色流实测 80% → 95%（+15 pp），已废弃。
- **UI 合成（+2.5 pp，默认开）**：`synth 0.20 + encode 0.61 ≈ 0.8 ms/帧`；`UiPatch=0` 可回退。
- **副卡与游戏同卡会抢 GPU**（日志 `note: same GPU as the producer`），不建议这样用。

复测：`fakerFG.ini` 设 `ProfGpu=1` 看 `prof:` 行，或用 `lock_frame/run_perf.bat` 跑定长段落。
逐 pass 与稀疏 A/B 见 [`PERF_OPT_2026-11-23.md`](docs/design/PERF_OPT_2026-11-23.md) §7，
94/97.5/100/102.5% 见
[`UI_ITERATION_2026-11-23_SPARSE.md`](docs/design/UI_ITERATION_2026-11-23_SPARSE.md) §2.2，
颜色 80→95% 见 [`PERF_BASELINE.md`](docs/findings/PERF_BASELINE.md)。

## 配置与日志

| 文件 | 内容 |
|---|---|
| `lock_frame_ui.ini` | 索引 + 全局 UI 偏好（语言 / 缩放 / 最近游戏） |
| `profiles\<游戏名>.ini` | 每个游戏一份完整配置（倍率、输入源、MV 比例、GPU、OSD…） |
| `<游戏目录>\fakerFG.ini` | 游戏侧配置；安装时按模板写入，改前自动备份（保留 8 份） |
| `lock_frame_ui.log` / `lock_frame_ui_HHMMSS.txt` | 日志（保留最近 8 次运行） |

全部在 exe 旁边，便携式，不写注册表 / AppData。

## 已知限制

- **UI 合成**（默认开）：producer 只传 UI 支撑域 patch，consumer 还原干净 HUDless，固定 mode 4；
  `UiPatch=0` 可回退。需要游戏在 DLSS-G evaluate 里提供 HUDless；HDR 要求 R10/HDR10，
  SDR 标题自动退化（不 tag UI）。逐通道 LUT 无法表达交叉通道项，支撑域内仍有约 3–4% 残差。
- **4K 映射余量**：默认 `TransportSlots=16`（CP2077 4K 实测约 1.0 GB）。出现 `pair: no_slot`
  时把 `fakerFG.ini` 的 `TransportSlots` 调到 24/32 并重启游戏。
- **4x 画质**：边缘模糊带比 3x 宽；3x 仍是推荐档。
- **port 模型**：`--mfg-model=port` 只是实验入口，默认 keep2x（model 18，所有倍率）。
- 全局扫描（Steam/Epic/GOG/注册表批量导入）已从界面下线，需手动添加 exe。
- 中文重命名依赖输入法提交字符，暂无 IME 候选浮层。
- 没有「删除游戏配置」按钮：删掉 `profiles\*.ini` 后重启 UI。
- 仅单机 / 离线使用；请勿用于带反作弊的在线游戏。

## 维护范围

核心功能已定稿，后续只覆盖两类工作。

| 类型 | 范围 | 提交时请附上 |
|---|---|---|
| Bug 修复 | 崩溃、黑屏/冻死、配对错帧、帧数明显回退、安装/还原/部署失败 | 「一键诊断」包、`lock_frame_ui.log`、`<游戏>_fakerFG.log`、producer 日志 |
| 新游戏支持 | 让未验证的游戏跑起来：输入车道选择、`--game` 匹配、必要的手工 quirk | 游戏名与版本、显卡型号、是否开 DLSS FG、倍率、一次 ≥30 秒实机运行的完整日志 |

**不做 / 已确认无解**

- 新的插帧算法或倍率模型：keep2x（model 18）固定，port 只作实验入口。
- 跨卡颜色传输：颜色仍走同卡 WGC，跨卡只有 MV/depth（以及 UI 支撑域 patch）。
- 反作弊在线游戏、独占全屏游戏：设计上就不支持。
- 全局扫描 / 批量导入游戏：仍从界面下线。

不承诺修复时间：单人维护，按 issue 顺序处理，能复现的优先。文档写错或过时也欢迎指出
（请注明版本和位置）。

## 从源码构建

```bash
./lock_frame/build.sh ui        # 只构建 UI → build/lock_frame_ui.exe
./lock_frame/build.sh all       # consumer / producer / fakerFG / UI
./lock_frame/release.sh         # 打 dist/lock_frame-<ver>.zip（默认含 Intel 运行时）
WITH_XESS=0 ./lock_frame/release.sh   # 精简包（不含 Intel DLL）
```

依赖：Windows SDK（含 `fxc`）、MSVC、Git Bash/WSL、Python 3。
详细说明见 `lock_frame/docs/ops/BUILD.md` 与 `docs/ops/DEPENDENCIES.md`。

## 目录结构

```text
lock_frame/            当前主线：consumer、producer、fakerFG、UI、发布脚本
lock_frame/ui/         可视化客户端（mgpu_ui.cpp）
lock_frame/docs/       技术文档、发布说明、审查记录
docs/bilibili/        B 站专栏图文（使用说明 + 截图）
src/                   早期实验实现（保留对照，发布以 lock_frame/ 为准）
third_party/           第三方依赖与许可（不入库，按 NOTICE 获取）
```

## 许可证与第三方

- 项目本体：**GPL-3.0**（见 `LICENSE` / `NOTICE`）。
- 随包的 Intel XeSS 运行时按 Intel 许可原样再分发（见包内 `INTEL-XESS-LICENSE.txt`），
  未做任何修改。
- 其他第三方组件与许可见 `NOTICE` 与 `docs/ops/LEGAL.md`。

## 反馈

- 提交地址：<https://github.com/coder-648/mgpu-xessmfg/issues>
- 标题建议：`bug：<一句话现象>` 或 `游戏支持：<游戏名>`
- 先跑一次「一键诊断」，把生成的 zip 和 `lock_frame_ui.log` 一起提供。
- 说明游戏名、显卡型号、是否开 DLSS FG、使用的倍率；只接受 **bug 修复** 与
  **新游戏支持** 两类请求（范围见[「维护范围」](#维护范围)）。
