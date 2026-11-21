# mgpu / lock_frame

**把游戏的渲染和帧生成拆到两块 GPU 上。** 游戏照常在自己的显卡上渲染 + 超分，
`lock_frame` 在另一块显卡上用 XeSS-FG 插帧，再叠加回游戏窗口上屏——
目标是在不换显卡的情况下把帧数翻倍到 4 倍。

当前正式版：**lock_frame 1.2.5**（共享协议 v30，GPL-3.0）。
MV/depth 默认走**无损码流**（硬件不支持时自动退回拷贝路径）；
发布包**自带 Intel 运行时**（`libxess_fg.dll` / `libxell.dll`），解压即用；
安装时它们会被写入 `<游戏>\fakerFG\`（1.2.1 起的修复：游戏进程里的 XeFG 只从这里加载）。

> UI 合成（HUDless/unblend）在 1.2.5 的结论是**暂无通用解决方案**，发布默认关闭，
> 只在显式本地 A/B 时可用，见 [`lock_frame/docs/UI_SYNTHESIS_GATE.md`](lock_frame/docs/UI_SYNTHESIS_GATE.md)。

![主界面](docs/bilibili/images/01-main.png)

> 小白向图文教程：[`docs/bilibili/使用说明.md`](docs/bilibili/使用说明.md)（可直接发 B 站专栏）。
> 详细技术文档：[`lock_frame/docs/README.md`](lock_frame/docs/README.md)。

## 这个工具适合谁

- 有两块显卡（核显 + 独显，或两张独显），想把游戏帧数再往上推；
- 游戏本身支持 **DLSS Frame Generation**，并且能开 **无边框 / 窗口化全屏**；
- 单机 / 离线游戏。

**不适合**：带反作弊的在线游戏、必须独占全屏的游戏、只有一块显卡还想零损耗的用户
（单卡能跑，但会和游戏抢性能）。

## 5 分钟上手

1. **解压**发布包到任意可写目录（不要只解压进游戏目录），双击 `lock_frame_ui.exe`。
2. 首次启动看欢迎页 → 点「知道了，开始」。
3. 点「当前游戏」卡片右上角的 **选择游戏** → 「添加 exe…」选中游戏主程序
   （例如 `Cyberpunk2077.exe`），列表里点它一下即可。
4. 回到主界面点 **安装 / 应用配置**：会把组件装进游戏目录，并把你当前的配置
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

## 每个游戏的配置存在哪

- 主配置（索引 + 全局 UI 偏好）：`lock_frame_ui.ini`
- 单游戏配置：`profiles\<游戏名>.ini`（倍率、输入源、MV 比例、GPU、OSD…全部在这里）
- 游戏侧配置：`<游戏目录>\fakerFG.ini`（安装/重新应用时写入，改前自动备份，保留 8 份）
- 日志：`lock_frame_ui.log` 与 `lock_frame_ui_HHMMSS.txt`

全部在 exe 旁边，便携式，不写注册表 / AppData。

## 核心功能

| 功能 | 说明 |
|---|---|
| 固定倍率 | 2x / 3x / 4x（120Hz 面板建议锁 60 / 40 / 30 fps） |
| fakerFG 输入源 | DLSSG（默认）/ NVNGX / Upscaler / FSR |
| 就绪检查 | 安装、Intel 运行时、游戏窗口、producer、fakerFG.ini 五项状态 |
| 安装 / 还原 | 一键部署（自动备份 `dxgi.dll`）与一键还原；游戏运行中会拦截 |
| 配置管理 | 一游戏一配置（`profiles\*.ini`），点名字可重命名，ini 漂移会提示重新应用 |
| 诊断 | 一键诊断包、内嵌 400 行日志、`pair:`/OSD 指标 |
| 界面 | 中/英双语、DPI 缩放、键盘操作、UIA 读屏 |


## 已知限制（1.2.0）

- 全局扫描（Steam/Epic/GOG/注册表批量导入）暂从界面下线，改为手动添加 exe。
- 中文重命名依赖输入法提交字符，暂没有 IME 候选浮层。
- 没有“删除游戏配置”按钮：删掉 `profiles\*.ini` 后重启 UI 即可。
- 仅单机/离线使用；请勿用于带反作弊的在线游戏。

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

- 先跑一次「一键诊断」，把生成的 zip 和 `lock_frame_ui.log` 一起提供；
- 说明游戏名、显卡型号、是否开启 DLSS FG、使用的倍率。
