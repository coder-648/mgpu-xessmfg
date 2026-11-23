# mgpu / lock_frame

**把游戏的渲染和帧生成拆到两块 GPU 上。** 游戏照常在自己的显卡上渲染 + 超分，
`lock_frame` 在另一块显卡上用 XeSS-FG 插帧，再叠加回游戏窗口上屏——
目标是在不换显卡的情况下把帧数做到 2x / 3x / 4x。

![主界面](docs/bilibili/images/01-main.png)

> 小白向图文教程：[`docs/bilibili/使用说明.md`](docs/bilibili/使用说明.md)（可直接发 B 站专栏）
> 技术文档索引：[`lock_frame/docs/README.md`](lock_frame/docs/README.md)

## 版本与维护状态

**正式版 1.2.9**（2026-11-23）· 共享协议 **v32** · GPL-3.0

- 跨卡只传 **MV / depth**（UI 合成为实验性、默认关闭；颜色走 WGC 单卡捕获）：
  MV 100% **比特精确**、depth 150%；硬件不支持无损码流时自动退回拷贝路径。
- 发布包**自带 Intel 运行时**（`libxess_fg.dll` / `libxell.dll`），解压即用；安装时写入
  `<游戏>\fakerFG\`（游戏进程里的 XeFG 只从这里加载）。

**维护状态：核心功能已到尽头。** 后续只做两件事——**修 issue** 和**加游戏支持**
（范围见[「维护范围」](#维护范围)）。
**UI 合成（HUDless + XeFG mode 5）仍是实验性**，发布默认关闭，见[「已知限制」](#已知限制)。

## 这个工具适合谁

- 有两块显卡（核显 + 独显，或两张独显），想把游戏帧数再往上推；
- 游戏本身支持 **DLSS Frame Generation**，并且能开 **无边框 / 窗口化全屏**；
- 单机 / 离线游戏。

以下情况不适合：带反作弊的在线游戏、必须独占全屏的游戏、只有一块显卡还想零损耗的用户
（单卡能跑，但会和游戏抢性能）。

## 5 分钟上手

1. **解压**发布包到任意可写目录（不要只解压进游戏目录），双击 `lock_frame_ui.exe`。
2. 首次启动看欢迎页 → 点「知道了，开始」。
3. 点「当前游戏」卡片右上角的 **选择游戏** → 「添加 exe…」选中游戏主程序
   （例如 `Cyberpunk2077.exe`），列表里点它一下即可。
4. 回到主界面点 **安装 / 应用配置**：会把组件装进游戏目录，并按所选模板把你当前的配置
   写进游戏里的 `fakerFG.ini`。
5. 启动游戏、进画面、开启 **DLSS 帧生成**（无边框 / 窗口化全屏），回到工具点 **刷新**：
   窗口和 producer 就绪后，「启动补帧」按钮亮起，点它即可。

| 选择游戏 | 高级设置 |
|---|---|
| ![选择游戏](docs/bilibili/images/02-choose.png) | ![高级设置](docs/bilibili/images/03-advanced.png) |

```text
首次启动
  └─ 选择游戏（添加 exe） → 安装 / 应用配置
        └─ 进游戏（无边框 + DLSS FG） → 刷新 → 启动补帧
              └─ 想调参数：操作行的「高级设置」（改的是当前游戏的配置）
```

## 核心功能

| 功能 | 说明 |
|---|---|
| 固定倍率 | 2x / 3x / 4x（120Hz 面板建议锁 60 / 40 / 30 fps） |
| fakerFG 输入源 | 固定 DLSSG（Streamline tag）；其他值强制回 `DLSSG`，`NoFG` 可显式禁用 |
| 游戏侧配置模板 | `fakerFG.ini`（精简，默认）/ `fakerFG.full.ini`（测试全参数），UI 内切换 |
| 就绪检查 | 安装、Intel 运行时、游戏窗口、producer、fakerFG.ini 五项状态 |
| 安装 / 还原 | 一键部署（自动备份 `dxgi.dll`）与一键还原；游戏运行中会拦截 |
| 配置管理 | 一游戏一配置（`profiles\*.ini`），点名字可重命名，ini 漂移会提示重新应用 |
| 逐游戏适配 | 内置 OptiScaler FG 输入表（49 行），按 `--game` 自动套用；`--game-quirks apply/log/off`，UI 可关 |
| 诊断 | 一键诊断包、内嵌 400 行日志、`pair:`/OSD 指标 |
| 界面 | 中/英双语、DPI 缩放、键盘操作、UIA 读屏 |

## 配置与日志

- 主配置（索引 + 全局 UI 偏好）：`lock_frame_ui.ini`
- 单游戏配置：`profiles\<游戏名>.ini`（倍率、输入源、MV 比例、GPU、OSD…全部在这里）
- 游戏侧配置：`<游戏目录>\fakerFG.ini`（安装/重新应用时按「配置版本」写入精简或完整模板；
  改前自动备份，保留 8 份）
- 日志：`lock_frame_ui.log` 与 `lock_frame_ui_HHMMSS.txt`

全部在 exe 旁边，便携式，不写注册表 / AppData。

## 渲染卡的额外开销（实测）

只有**渲染卡**（游戏所在的那块）的时间会影响帧率；消费卡在 3x@40 下还有约 88% 余量，
消费端的解码、unblend、VRAM 优化不会体现在 fps 上。

实测环境：CP2077 4K DLAA、锁 40 fps、3x 输出；渲染 **RTX 3080 Ti**，帧生成 **RX 6750 GRE**，
1.2.9 默认配置。换卡、换游戏、换分辨率都会变。

### 占用增量（RTSS）

| 状态 | 渲染卡占用 | 增量 | 成本归属 |
|---|---:|---:|---|
| 游戏原生（无本工具） | 94% | — | — |
| 开启 DLSS-FG 管线（未接 consumer） | 97.5% | **+3.5 pp** | 激活 FG 管线本身，必须支付 |
| + MV/depth 传输（降采样、编码、跨卡 DMA）+ WGC 捕获/配对 | 100% | **+2.5 pp** | 本工具的固定开销 |
| + UI 合成（实验性，sparse） | 103% | **+3 pp** | 可开关 |

三项合计 **+9 pp**（94% → 103%）。其中 **3.5 pp 是游戏开启 DLSS FG 管线本身就要付的成本**，
与 MV/depth 传输无关；MV/depth 及其余固定开销合计 **2.5 pp**；UI 合成单独 **3 pp**。

### 各 pass 时间（`prof:`，UI 合成开启时，4K/帧）

| pass | 时间 | 占 25.6 ms 预算 |
|---|---:|---:|
| MV 编码（100%，比特精确） | 0.7–1.3 ms | 2.6–5.0% |
| depth 编码（150%） | 0.79 ms | 3.1% |
| UI 编码（全分辨率） | 0.61 ms | 2.4% |
| UI synth（`P − H`） | 0.20 ms | 0.8% |
| **合计** | **2.9 ms** | **≈11%** |

这些是名义 GPU 时间，会与游戏剩余工作重叠：MV/depth 名义 1.5–2.1 ms，实际只增加
**2.5 pp（≈0.6 ms）**；UI 名义 0.81 ms，实际增加 **3 pp（≈0.8 ms）**。所以不要用毫秒直接
加总算占用。

从 1.2.7 到 1.2.9：depth 编码 2.05 → 0.79 ms（−61%，depth 走 150%），
UI 增量 5 pp → 3 pp（sparse UI codec，105% → 103%）。

### 开销来源

- **DLSS-FG 管线（+3.5 pp，固定）**：游戏启用 DLSS FG 就必须付的管线成本（present 拦截、
  tag 接管与记录等），不属于 MV/depth 传输。
- **MV/depth 传输（+2.5 pp，固定）**：两个全屏降采样 pass + 无损编码 + 跨卡 DMA，
  以及同卡的 WGC 捕获与 present 配对。旧的可选跨卡颜色流实测 80% → 95%（+15 pp），已废弃。
- **UI 合成（+3 pp，默认关）**：`synth 0.20 + encode 0.61 ≈ 0.8 ms/帧`。
- **消费端在副卡**：XeFG 插帧、unblend、叠加不占渲染卡；副卡与游戏同卡时会抢 GPU
  （日志 `note: same GPU as the producer`），不建议这样用。

数据与复测：逐 pass 与稀疏 A/B 见
[`PERF_OPT_2026-11-23.md`](docs/design/PERF_OPT_2026-11-23.md) §7，94/97.5/100/103% 见
[`UI_ITERATION_2026-11-23_SPARSE.md`](docs/design/UI_ITERATION_2026-11-23_SPARSE.md) §2.2，
颜色 80→95% 见 [`PERF_BASELINE.md`](docs/findings/PERF_BASELINE.md)。复测时在 `fakerFG.ini`
设 `ProfGpu=1` 看 `prof:` 行，或用 `lock_frame/run_perf.bat` 跑定长段落。

## 已知限制

- **UI 合成是实验性**：只在标题提供 HUDless/UI 平面时可用（1.2.9 实测 CP2077 4K 可复现），
  边缘/半透明 UI 仍可能拖影或伪影；发布默认关闭（`UiStream=0` / `UiSource=Off`），
  手动打开时走 XeFG mode 5（`--no-ui-mode5` 回 mode 4）。HDR 下若 XeFG 报
  `-12 MISMATCH_INPUT_RESOURCES`，在游戏侧 ini 加 `UiAlpha8=0` 后重启游戏。
- 全局扫描（Steam/Epic/GOG/注册表批量导入）暂从界面下线，改为手动添加 exe。
- 中文重命名依赖输入法提交字符，暂没有 IME 候选浮层。
- 没有「删除游戏配置」按钮：删掉 `profiles\*.ini` 后重启 UI 即可。
- 仅单机 / 离线使用；请勿用于带反作弊的在线游戏。

## 维护范围

1.2.9 之后只覆盖以下两类工作。

| 类型 | 范围 | 提交时请附上 |
|---|---|---|
| Bug 修复 | 崩溃、黑屏/冻死、配对错帧、帧数明显回退、安装/还原/部署失败 | 「一键诊断」包、`lock_frame_ui.log`、`<游戏>_fakerFG.log`、producer 日志 |
| 新游戏支持 | 让未验证的游戏跑起来：输入车道选择、`--game` 匹配、必要的手工 quirk | 游戏名与版本、显卡型号、是否开 DLSS FG、倍率、一次 ≥30 秒实机运行的完整日志 |

文档里写错或过时的内容也欢迎指出（请注明版本和位置）。

**不做 / 已确认无解**

- **新的插帧算法或倍率模型**：模型固定 **keep2x（model 18，所有倍率）**，
  `--mfg-model=port` 只是显式实验入口（3x 略好、4x 更差，见 `lock_frame/docs/RELEASE_NOTES_1.2.7.md`）。
- **UI 合成的通用方案**：只在有 HUDless/UI 平面的标题上可用，不承诺做成与游戏无关的
  通用方案（见[「已知限制」](#已知限制)）。
- **跨卡颜色传输**：颜色仍走 WGC 单卡捕获，跨卡的只有 MV/depth（以及实验性的 UI）。
- **反作弊在线游戏、独占全屏游戏**：设计上就不支持。
- **全局扫描 / 批量导入游戏**：仍从界面下线，需手动添加 exe。

不承诺修复时间：单人维护，按 issue 的先后顺序处理，能复现的优先。

## 从源码构建

```bash
./lock_frame/build.sh ui        # 只构建 UI → build/lock_frame_ui.exe
./lock_frame/build.sh all       # consumer / producer / fakerFG / UI
./lock_frame/release.sh         # 打 dist/lock_frame-<ver>.zip（默认含 Intel 运行时）
WITH_XESS=0 ./lock_frame/release.sh   # 精简包（不含 Intel DLL）
```

依赖：Windows SDK（含 `fxc`）、MSVC、Git Bash/WSL、Python 3。
详细构建与依赖说明见 `lock_frame/docs/ops/BUILD.md` 与 `docs/ops/DEPENDENCIES.md`。

## 目录

```
lock_frame/            当前主线：consumer、producer、fakerFG、UI、发布脚本
lock_frame/ui/         可视化客户端（mgpu_ui.cpp）
lock_frame/docs/       技术文档、发布说明、UI 设计
docs/bilibili/         B 站专栏图文（使用说明 + 截图）
src/                   早期实验实现（保留对照，发布以 lock_frame/ 为准）
third_party/           第三方依赖与许可（不入库，按 NOTICE 获取）
```

## 许可证与第三方

- 项目本体：**GPL-3.0**（见 `LICENSE` / `NOTICE`）。
- 随包的 Intel XeSS 运行时按 Intel 许可原样再分发（见包内
  `INTEL-XESS-LICENSE.txt`），未做任何修改。
- 其他第三方组件与许可见 `NOTICE` 与 `docs/ops/LEGAL.md`。

## 反馈

- 提交地址：<https://github.com/coder-648/mgpu-xessmfg/issues>
- 标题建议：`bug：<一句话现象>` 或 `游戏支持：<游戏名>`
- 先跑一次「一键诊断」，把生成的 zip 和 `lock_frame_ui.log` 一起提供；
- 说明游戏名、显卡型号、是否开启 DLSS FG、使用的倍率；
- 只接受 **bug 修复** 与 **新游戏支持** 两类请求（范围见[「维护范围」](#维护范围)）。
