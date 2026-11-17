# mgpu / lock_frame

**把游戏的渲染和帧生成拆到两块 GPU 上。** 游戏照常在自己的卡上渲染 + 超分；
`lock_frame` 在另一块卡上用 XeSS-FG 插帧，再叠加到游戏窗口上屏。

当前正式版：**lock_frame 1.0.0**，共享协议 **v28**，开源 **GPL-3.0**。
详细文档：[`lock_frame/docs/README.md`](lock_frame/docs/README.md)。

> 当前主线是 `lock_frame/`。`src/` 下的实现是同一套技术的早期版本/实验代码，
> 保留用于对照与历史追溯；发布和使用以 `lock_frame/` 为准。

## 核心要点

- 颜色走 WGC，不再从渲染卡复制 4K 颜色；跨卡只传 MV + depth。
- MV/depth 默认 `StreamPercent=150`（每维 2/3，字节 4/9），消费卡联合重建。
- 颜色四角 marker（seq12 + ms12 + CRC8）与 producer frame_id 精确配对；
  配不上就本帧 1X，不猜槽。
- 固定倍率 2x / 3x / 4x；120Hz 面板分别锁 60 / 40 / 30 fps，输出恒定 120。
- 32 槽 publication ring 吸收 WGC 异步延迟；健康状态机 + 光流降级 +
  session/ACK 恢复。

## 核心功能

| 功能 | 说明 |
|---|---|
| 固定倍率 | 2x / 3x / 4x（`--mode 2x / 3x / 4x`；旧 `30x4/40x3` 为别名） |
| fakerFG 输入源 | DLSSG（默认，已实测）/ NVNGX / Upscaler / FSR |
| 颜色 | WGC，scRGB/HDR 原样；SDR/HDR10 输出可切 |
| 配对 | 四角 marker 精确匹配；失败 1X；重复帧按 seq12 去重 |
| 降级 | 无 MV/配对失败走 FidelityFX 光流；depth-only 游戏可用 |
| 恢复 | 64 帧健康窗口；持续错位才 reset；consumer 重连新 session |
| UI | 选窗口、倍率、输入源；目标显示器刷新率；环境自检；日志轮转 |
| 诊断 | `pair:` / OSD `mvN` / `--dump` / `collect_diagnostics.bat` |

## 核心数据

| 项目 | 实测/默认 |
|---|---|
| 4x（旧 30x4）长跑 | 精确配对约 99.2%，输出约 99.77%，`show=118~120` |
| 3x（旧 40x3）稳态 | `exact=40 no_slot=0`，`presents=120/120` |
| 延迟（120Hz 填满） | 2x@60 ≈ 40–48ms；3x@40 ≈ 55–65ms；4x@30 ≈ 73–82ms |
| 跨卡流量（4K，150%） | MV+depth ≈ 28MB/帧（不再传 4K 颜色） |
| 共享 ring | 32 槽；v28 控制块与数据区分离 |
| 2x 依赖 | 原版 `libxess_fg` 即可 |
| 3x/4x 依赖 | `--unlock-mfg`；实测 `libxess_fg 1.3.1.78` / `libxell 1.3.2.10` |

## 使用条件

- Windows 10 1903+ / Windows 11，D3D12。
- 推荐双 GPU：游戏渲染卡 + 帧生成卡；单卡可跑但不建议。
- 游戏无边框/窗口化全屏；开启 DLSS Frame Generation（或对应输入 API）。
- 自备 `libxess_fg.dll` / `libxell.dll`（正式包默认不分发）。
- 仅单机/离线；不要在带反作弊的在线游戏中使用。

## 快速开始

1. 解压发布包，运行 `deploy_game.bat "游戏 exe 目录"`。
2. 把 `libxess_fg.dll` / `libxell.dll` 放到 `lock_frame_ui.exe` 同目录。
3. 启动游戏，开启 DLSS Frame Generation，锁到建议输入帧率。
4. 运行 `lock_frame_ui.exe`：选窗口、2x/3x/4x、输入源 DLSSG，启动。

## 项目结构

| 路径 | 内容 |
|---|---|
| `lock_frame/` | 当前正式版：consumer、producer、fakerFG、UI、构建/发布 |
| `lock_frame/docs/` | 规划、发布说明、测试步骤、配置 |
| `docs/` | 架构、协议、性能基线、踩坑记录（含早期链路） |
| `src/` | 早期实现/实验代码（legacy/对照） |
| `third_party/` | 本地依赖，整体不入库；按 `docs/ops/DEPENDENCIES.md` 获取 |
| `lock_frame/third_party/nlohmann/` | 唯一随仓库跟踪的第三方源码（MIT） |

## 已知边界

- 非整除刷新率（如 165Hz）不承诺完美节奏；UI 会提示。
- 3x/4x 的字节补丁只对实测版本成立；失败会回退并打日志。
- NVNGX / Upscaler / FSR 输入源为实验性；DLSSG 为正式支持。
- 低帧自适应默认关闭；开启后可能有轻微 flicker/ghosting。
- 正式包未签名；用 `SHA256SUMS.txt` 校验。

## 第三方与许可

本项目 GPL-3.0（`LICENSE`）。第三方逐项见 `NOTICE` 与
`docs/ops/LEGAL.md`；`third_party/` 整体不入库。主要项目：

- OptiScaler（GPL-3.0）
- AMD FidelityFX SDK（MIT / AMD 许可）
- Universal MFG Unlocker（MIT）
- nlohmann/json（MIT）
- MinHook（BSD-2-Clause）
- ReShade（BSD-3-Clause）
- Intel XeSS SDK（Intel 许可；默认不分发）
- NVIDIA DLSS SDK / Streamline（NVIDIA 许可）
- Magpie（GPL-3.0）
- dlssg_for_sm86（GPL-3.0；NVIDIA 组件不转授权）
- Microsoft Windows SDK / DirectX 12
