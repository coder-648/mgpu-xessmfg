# lock_frame 文档索引

| 文档 | 内容 |
|---|---|
| [`../README.md`](../README.md) | 项目说明、构建、部署、日志判读 |
| [`UI_CLIENT_PLAN.md`](UI_CLIENT_PLAN.md) | 可视化客户端：易用性分析、简易模式规格、落地状态 |
| [`RELEASE_NOTES_1.0.0.md`](RELEASE_NOTES_1.0.0.md) | 1.0.0 发布说明 |
| [`RELEASE_1.0.0.md`](RELEASE_1.0.0.md) | GitHub Release 发布正文（下载/校验/安装/已知限制） |
| [`RELEASE_NOTES_1.3.0.md`](RELEASE_NOTES_1.3.0.md) | 1.3.0 发布说明（UI 合成 mode 4 定稿，DLSS FG 双卡链路完成） |
| [`RELEASE_NOTES_1.3.1-local.camvm.md`](RELEASE_NOTES_1.3.1-local.camvm.md) | 1.3.1 本地验证版（XeFG 输入契约：view/proj/rect/extent） |
| [`RELEASE_NOTES_1.3.2-local.camvm.md`](RELEASE_NOTES_1.3.2-local.camvm.md) | 1.3.2 本地版（100/100，**已回退，勿用**） |
| [`RELEASE_NOTES_1.3.3-local.ui.md`](RELEASE_NOTES_1.3.3-local.ui.md) | **1.3.3-local.ui 发布说明**（A/C/D/E/F + B/J/R + UI mode 4） |
| [`RELEASE_PLAN_2026-11-17.md`](RELEASE_PLAN_2026-11-17.md) | 正式版规划、决策记录、实施状态 |
| [`PRE_RELEASE_20261117.md`](PRE_RELEASE_20261117.md) | 尝鲜版说明（历史） |
| [`FG_PIPELINE.md`](FG_PIPELINE.md) | fakerFG 输入/输出管线 |
| [`UI_SOURCE_V29.md`](UI_SOURCE_V29.md) | UI 流 v29：GameUi / HUDless 自造 UI、开关与门控 |
| [`UI_SYNTHESIS_GATE.md`](UI_SYNTHESIS_GATE.md) | UI 合成闸门：仅当游戏默认稳定提供 UI 流时才允许重新开启 |
| [`RUN_STEPS.txt`](RUN_STEPS.txt) | CP2077 运行步骤 |
| [`../scripts/capture_dataset.bat`](../scripts/capture_dataset.bat) | **抓一套可复用数据集**（colour+MV+depth+meta）：一条命令改 ini / 等重开游戏 / 等 `dump complete` / 写 `manifest.json`；设计见 `docs/design/LOSSLESS_TRANSPORT.md` §11.5 |
| [`PBZ_TEST_STEPS.txt`](PBZ_TEST_STEPS.txt) | PBZ 测试步骤 |
| [`PBZ_DLSSG_LANE_TEST.md`](PBZ_DLSSG_LANE_TEST.md) | PBZ DLSSG 输入车道测试 |
| [`PER_GAME_COMPAT_2026-11-25.md`](PER_GAME_COMPAT_2026-11-25.md) | 逐游戏兼容性：本轮落地与剩余清单 |
| [`NEXT_ROUND_2026-11-26.md`](NEXT_ROUND_2026-11-26.md) | 下一轮回归、未覆盖链路的验证项与禁止项 |
| [`SELF_AUDIT_2026-11-25.md`](SELF_AUDIT_2026-11-25.md) | **lock_frame 自身链路审查**：consumer/producer 的同步、状态、显存与性能项（A–O） |
| [`UI_MODE4_2026-11-25.md`](UI_MODE4_2026-11-25.md) | **UI 合成（mode 4）审查**：patch 的渲染卡开销、小 UI 抖动定位与 A/B 步骤 |
| [`test-configs/pbz_dlssg_lane.ini`](test-configs/pbz_dlssg_lane.ini) | PBZ DLSSG 车道测试配置 |
| [`archive/LOCK_FRAME_30X4_NOTES.md`](archive/LOCK_FRAME_30X4_NOTES.md) | 30x4 版本的历史设计笔记 |
