# src/xess_mfg_consumer

独立的事件驱动 consumer（`xess_mfg_consumer.exe`）。工程结构沿用 `src/faker_fg`：

- `consumer_internal.h`：公共前置（Windows/D3D/WinRT/XeSS/FFX 头、`using`）；
- `modules/*.cpp`：按职责拆分的实现单元，各自保留 `static` 状态；
- `unity.cpp`：唯一被编译的翻译单元，按依赖顺序 `#include` 这些 `.cpp`；
- 没有任何 `.inc`，也不复用 `src/bridge` 的编译单元。

与 `bridge` 的关系：

- 共享的只有 ABI 头（`src/mgpu_shared.h`、`src/xefg_*.h`）和第三方库；
- `bridge` 保持原有轮询实现，不包含 consumer 分支，也不构建 consumer；
- 本目录以 `bridge` 的模块为种子 fork，后续修改互不影响。

- 构建：`./scripts/build_consumer.sh`（先跑 `scripts/check_bridge_modules.py src/xess_mfg_consumer`）
- 着色器：`shaders/upsample_joint.hlsl`，由构建脚本用 fxc 编译并嵌入
- OSD 复用本目录的 `osd_panel.h`

v22 的循环（`modules/main.cpp`）：

- **没有抓屏**：颜色来自 producer 的 stream 0，与 MV/depth 同一发布、同一 fence；
- 主循环扫描 `frames[s].published`，取**最旧未处理**的一条 → `wait_slot(s)`（三个流都到齐）
  → `run_frame()`；一条发布只处理一次；
- 没有 publication 超过 250 ms（菜单/加载）时隐藏覆盖窗口，恢复后自动显示；
- 输入拷贝（颜色+MV+depth）在 `gpu_throttle()` 之后、`copy_publication_inputs()` 里一次完成，
  走 input copy queue 时由 direct queue 在提交渲染列表**之前**等待；
- ACK 在下一帧 `gpu_throttle()` 之后写 `consumed[s]`（含义 = 本地拷贝已退休）。

历史（v21 及以前，已删除）：WGC FrameArrived 事件、colour 队列/capture 线程、
marker 解码、present-time ring。保留这些描述只会误导——当前结构见
`docs/design/COLOUR_TRANSPORT_V22.md`。

新增模块时：

1. 在 `modules/` 建 `name.cpp`，文件头写职责，并在开头 `#include "../consumer_internal.h"`；
2. 在 `unity.cpp` 的依赖顺序里加入；
3. 跑 `python3 scripts/check_bridge_modules.py src/xess_mfg_consumer` 和构建。

