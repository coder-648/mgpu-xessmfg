# 文档索引

23 篇平铺太乱，所以分三层。**新读者按 1 → 2 → 3 读，其余按需查。**

## 1. 先读这三篇

| 文档 | 内容 |
|---|---|
| [`../README.md`](../README.md) | 项目是什么、5 分钟上手、三份配方、已知限制 |
| [`findings/CONCLUSIONS.md`](findings/CONCLUSIONS.md) | **结论先行**：这个 FG 消费什么、三条硬约束、倍率的真相 |
| [`design/ARCHITECTURE.md`](design/ARCHITECTURE.md) | 为什么是两个进程、一帧的生命周期、关键取舍 |

## 2. 动手

| 文档 | 内容 |
|---|---|
| [`ops/USAGE.md`](ops/USAGE.md) | **完整使用说明**：前置条件、两步安装、游戏内设置、三份配方、怎么判断正常、排障、卸载、合规 |
| [`ops/BUILD.md`](ops/BUILD.md) | 构建三件产物、第三方 SDK 的钉住版本、纯 Windows 构建、`make_release.sh` 打包 |
| [`ops/DEPLOY.md`](ops/DEPLOY.md) | 部署包（拖 exe 即装/即卸）+ 三个必踩的坑 |
| [`ops/CONFIG.md`](ops/CONFIG.md) | **占位符含义**、环境变量、`game_capture.ini` 每个键 |
| [`ops/UI.md`](ops/UI.md) | `mgpu_ui.exe` 界面与每个开关的对应关系 |
| [`ops/DEPENDENCIES.md`](ops/DEPENDENCIES.md) | 依赖版本、钉住的 commit、重建命令 |
| [`ops/LEGAL.md`](ops/LEGAL.md) | **GPL-3.0 由来**、`--unlock-mfg` 声明、XeSS SDK 再分发限制 |

## 3. 设计与发现

| 文档 | 内容 |
|---|---|
| [`design/PROTOCOL.md`](design/PROTOCOL.md) | 共享内存协议 v10 的字段、语义、版本演进 |
| [`design/INPUT_SOURCES.md`](design/INPUT_SOURCES.md) | DLSS / FSR / XeSS 三个输入源的能力差异与扩展方法 |
| [`design/VENDOR_COMPAT_PLAN.md`](design/VENDOR_COMPAT_PLAN.md) | 厂商兼容计划（输入源抽象 + 副卡泛化，M6.x） |
| [`findings/QUALITY_VARIABLES.md`](findings/QUALITY_VARIABLES.md) | **画质/预算变量作用表** + 怎么自己复测 |
| [`findings/FINAL_CONFIG.md`](findings/FINAL_CONFIG.md) | 踩坑清单 §5 + 实验档案 §9（**历史附录，结论看 CONCLUSIONS**） |
| [`findings/HANDOFF.md`](findings/HANDOFF.md) | 里程碑历史 + §8.6 十轮实验档案 |
| [`findings/MFG.md`](findings/MFG.md) | 多帧生成（`--unlock-mfg`）的原理与字节补丁说明 |
| [`findings/XESS_FG_NOTES.md`](findings/XESS_FG_NOTES.md) | XeFG 开发指南的提炼 |
| [`findings/OPTISCALER_NOTES.md`](findings/OPTISCALER_NOTES.md) | XeFG 的 MV 约定 / 相机运动重投影（上游 GPL-3.0） |
| [`findings/TOOLING_NOTES.md`](findings/TOOLING_NOTES.md) | 工具链与命令行的注意事项 |
| [`findings/ENV_REPORT.md`](findings/ENV_REPORT.md) | 实测环境记录 |

## 4. 历史

| 路径 | 内容 |
|---|---|
| [`findings/milestones/`](findings/milestones/) | M0–M4 各里程碑的步骤与结果（当时的结论，未必仍然成立） |
| [`findings/ARCHIVED_EXPERIMENTS.md`](findings/ARCHIVED_EXPERIMENTS.md) | 已移除的历史实验脚本：每个当时的参数与结论 |

## 阅读提醒

- `docs/findings/milestones/` 是**历史记录**，里面有些结论后来被推翻了
  （最典型的是"MV 是死输入"）。当前结论只看 `findings/CONCLUSIONS.md`。
- 仓库里没有本机路径：看到 `<GAME_DIR>` / `<TOOLS_DIR>` 这类尖括号就是占位符，
  含义见 `ops/CONFIG.md` §1。
