# xess_mfg_consumer：新的消费端目标（设计，2026-11-14）

> **实现进度更新（2026-11-14 晚）**：M0/M1/M2 已落地，并额外完成 M2.5
> （present-ring 配对 → 现在 marker 为主键）、颜色 sink 双缓冲、输入预拷、
> `--stream-auto`/`--stream-asym`、`--upsample`、marker 解码/回填。
> 当前形态与实测见 `docs/findings/PERF_ROUND_HANDOVER.md` 与
> `docs/PROJECT_STATUS_2026-11-14.md` §2.3。

> 目标：在 `docs/design/BRIDGE_V2.md` 的事件驱动架构下，产出一个**新的消费端
> 可执行文件** `xess_mfg_consumer.exe`，与现有 `bridge.exe` 并存、可 A/B。
>
> 原则：consumer 有**独立的源码树**（`src/xess_mfg_consumer/`），以已经踩过坑的核心
> 模块为种子 fork，之后独立演进；`bridge` 不再包含任何 consumer 分支。只共享 ABI 头
> 与第三方库，不共享编译单元。

## 1. 为什么是独立源码树，而不是 bridge 里的构建变体

现有核心（`src/bridge/modules/010–150`）包含大量难替换的知识：

- 双卡 device/queue/fence、共享堆 placed resource、跨适配器 fence；
- WGC 的时间戳/age/arrival 统计、颜色格式与 HDR；
- XeFG 的 tag 顺序、barrier 状态、MFG/cadence、present 约束；
- 输入缓存/FIFO/COPY 队列/upsample。

这些代码在 `bridge.exe` 上已经验证，但把新循环以 `#if MGPU_CONSUMER_V2` 塞回
`src/bridge/modules/*.inc` 会让两套生命周期纠缠在同一个翻译单元里：任何 consumer
的改动都要先证明不会改变 bridge，bridge 的修复又要同步回 consumer。这里改为
**独立源码树**：

- `src/xess_mfg_consumer/` 有自己的入口、模块、着色器和构建脚本；
- `src/bridge/` 回到原有的轮询实现，不再包含任何 consumer 分支；
- 两边只共享 ABI 头（`src/mgpu_shared.h`、`src/xefg_*.h`）与第三方库；
- consumer 先以 bridge 的模块为种子 fork，之后可以按 M2–M4 自由重构（线程、
  SPSC 队列、序号 matcher、Pacer），不会把 bridge 一起改坏。

代价是 core 的修复需要手动 cherry-pick；在消费者模型仍在快速演进的阶段，这个
代价小于“一个宏同时控制两套主循环”的耦合。

## 2. 目录与构建

```text
src/bridge/                 # 原 bridge，保持轮询实现
  bridge.cpp
  modules/010–160

src/xess_mfg_consumer/      # 独立 consumer（M1 事件驱动）
  consumer_internal.h       # 公共前置（Windows/D3D/WinRT/XeSS/FFX）
  unity.cpp                 # 唯一编译单元，按依赖顺序 include modules/*.cpp
  modules/platform.cpp      # 日志/退出/窗口查找
  modules/devices_capture_fg.cpp   # 设备 + WGC + FrameArrived
  modules/input_cache.cpp   # producer 缓存 + arm_pub_events
  modules/frame.cpp         # run_frame()/XeFG 接线
  modules/main.cpp          # 参数解析 + 初始化 + M1 事件循环
  modules/…                 # 其余按职责拆分
  shaders/upsample_joint.hlsl
scripts/
  build_bridge.sh           # -> build/bridge.exe
  build_consumer.sh         # -> build/xess_mfg_consumer.exe
```

- 两个 exe 不共享任何 `.inc`/`.cpp`；构建脚本各自 staging 到
  `<STAGE_DIR>/bridge` 与 `<STAGE_DIR>/consumer`。
- consumer 采用 fakerFG 式的 unity build：`unity.cpp` 是唯一编译单元，
  `modules/*.cpp` 按依赖顺序 include；`check_bridge_modules.py` 会自动识别
  `unity.cpp` / `bridge.cpp` 入口。
- 需要把 bridge 的修复带过来时，按模块手工同步；不要再用 `#if` 宏跨树复用。

## 3. 迁移里程碑

| 里程碑 | 内容 | 验收 |
|---|---|---|
| **M0** | 独立树建立：consumer 从 bridge fork 核心模块，入口/构建分离 | `bridge.exe` 与 `xess_mfg_consumer.exe` 都能独立构建 |
| **M1** | WGC `FrameArrived` + producer event，`MsgWaitForMultipleObjects`，去掉 `Sleep(1)` | `proc:` 的 `wakes/s` 接近输入帧率，A5 不回归 |
| **M2** | capture 线程 + colour SPSC 队列；presenter 只消费 | `present` 不再阻塞 WGC，`OUTPUT FRAMES MISSING` 下降（已实现，待硬件 A/B） |
| **M3** | input 线程 + publication 队列；序号 matcher 替换 `pair_slot_for_wgc` | `pf` 40/40，`arr` 3x ≥95% |
| **M4** | Pacer/jitter 统一；旧 key-mode/实验路径清理；事件循环做默认 | `total ≤ 24.5 ms`，无周期性顿挫 |

> M1 已实现（2026-11-14）：`wakes/s` 取代了 `poll/s`，直接统计
> `MsgWaitForMultipleObjectsEx` 的返回次数；两个事件都不可用时退回 1 ms 超时。

每个里程碑都用同一段 60 s 场景、同一组 A5/A6/A10/A11 对比；任何一项回归就回退。

## 4. 接口边界

```cpp
struct FrameJob {              // presenter 唯一需要的输入
    ComPtr<ID3D11Texture2D> colour;   // WGC 颜色（已复制/已 fence）
    uint64_t producer_pub;            // publication 序号
    uint64_t wgc_ts_ms;
    ComPtr<ID3D12Resource> mv, depth; // 本地缓存视图（或 upsample 输出）
    uint32_t gen;                     // 几何 generation
    uint64_t input_fence;             // COPY 队列 fence
};
```

- `capture` 只产 `colour + wgc_ts`；
- `input` 只产 `producer_pub + mv/depth + input_fence`；
- `matcher` 产出 `FrameJob`；
- `presenter` 只做 tag/present/OSD/控制，不碰 WGC/COPY 队列；
- XeFG 的状态只在 presenter 线程改。

## 5. 与旧 bridge 的关系

- 旧 `bridge.exe` 保留为**回归基线**，直到 consumer 在 A5/A6/A10/A11 上全部达标；
- `--pub-fifo`/`--prefetch-publications`/`--jbuf` 在 consumer 里由后续的队列/Pacer 吸收，
  不再作为用户可感知的实验开关；
- `g_frame_event` 在 M1 已接线（`FrameArrived` → `SetEvent`），`g_pub_event` 由 producer
  的每 slot `fwd` fence 驱动；两个都不再是死代码；
- 颜色仍走 WGC/DWM，不增加渲染卡成本；这一点 consumer 不改。

## 6. 风险

- WGC `FrameArrived` 的 ABI 事件实现（`TypedEventHandler`）和线程模型；
- 共享 `fwd` fence 上 `SetEventOnCompletion` 的跨适配器可用性，否则需要协议 v16 的共享 event；
- 把 `run_frame_ex` 从主循环拆成 `presenter.render(FrameJob)` 时的全局状态收敛；
- 在线程化过程中保持 XeFG 的 tag/present 同线程，避免 1.3.1 的状态竞争。
