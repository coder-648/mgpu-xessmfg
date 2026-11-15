# 消费端架构：`xess_mfg_consumer.exe`（2026-11-14）

> 本文描述当前消费端的**实际代码形态**（`src/xess_mfg_consumer/`）。
> 旧 bridge 的结构见 [`BRIDGE_ARCH.md`](BRIDGE_ARCH.md)（legacy，仅作历史参考）。
>
> 相关：链路总览 [`ARCHITECTURE.md`](ARCHITECTURE.md)、协议 [`PROTOCOL.md`](PROTOCOL.md)、
> 性能基线 [`../findings/PERF_ROUND_HANDOVER.md`](../findings/PERF_ROUND_HANDOVER.md)。

## 1. 构建形态

- fakerFG 式 unity build：`unity.cpp` 是唯一编译单元；
- `consumer_internal.h` 放公共前置（Windows/D3D11/D3D12/WGC/XeSS/FFX）；
- `modules/*.cpp` 按依赖顺序 `#include`，每个模块保留自己的 `static` 状态；
- 与 `src/bridge/` **不共享编译单元**，只共享 `src/mgpu_shared.h` 等 ABI 头；
- 构建：`scripts/build_consumer.sh`，先跑 `check_bridge_modules.py`。

## 2. 模块表（16 个）

| 模块 | 职责 |
|---|---|
| `platform.cpp` | 日志、退出路径、游戏窗口查找 |
| `state_overlay.cpp` | 全局共享状态、命令行开关、producer 提示、覆盖窗口/显示器放置 |
| `devices_capture_fg.cpp` | 适配器/设备创建、WGC 会话与 `FrameArrived`、XeSS-FG 初始化、`--list-modes` |
| `cadence.cpp` | 到达节奏、`frameRenderTime` 喂法、MFG 计数控制器、平滑辅助 |
| `shaders.cpp` | 颜色转换 / 重采样 / prepare 的 HLSL 源与 PSO |
| `frame_io.cpp` | 统计输出、fence、publication 队列、流几何、本地拷贝、哑输入 |
| `osd.cpp` | OSD 窗口、topmost/z-order 修复、节流刷新 |
| `warp_upsample.cpp` | 实验性 `--fg warp` 插值器 + `--upsample` 联合 MV/深度重建 |
| `stream_auto.cpp` | `--stream-auto` 每流 `StreamPercent` 控制器 + warp PSO |
| `convert_warp.cpp` | 缩放核常量、颜色转换 draw、warp 上屏路径 |
| `pair_dump.cpp` | WGC 时间戳/槽位配对回退、`--dump` 录制、颜色探针 |
| `dump_of.cpp` | dump 落盘、FidelityFX 光流初始化/录制/自检 |
| `input_cache.cpp` | 帧入口、退役输入、ACK、共享布局服务、拷贝队列、预拷/FIFO |
| `frame.cpp` | `run_frame_ex()`：每帧颜色→配对→输入→tag→XeFG→Present 的主路径 |
| `probe_mfg.cpp` | `libxess_fg` 探针、`--unlock-mfg` 补丁 |
| `main.cpp` | 参数解析、初始化、capture/present 主循环 |

## 3. 线程与队列

```text
WGC FrameArrived（WinRT 线程）
    └─ SetEvent(g_frame_event)                 不在这里做 GPU/堆操作

capture 线程（M2，默认开）
    └─ TryGetNextFrame → ColourItem{texture, wgc_ts, dt, age}
        └─ 有界队列（默认深度 1，--colour-queue N）

presenter 线程（主线程）
    ├─ MsgWaitForMultipleObjectsEx(colour, publication fence)
    ├─ prefetch_publications() / arm_pub_events()
    ├─ colour_pop() → run_frame() / run_frame_warp()
    └─ Present() → ACK
```

- `--no-capture-thread` 回到 M1 单线程循环（WGC 与 Present 串行）；
- `--colour-queue N` 加深颜色队列（会增加延迟，M2 实测深度 2 约 +20 ms）；
- 输入拷贝走独立 COPY 队列（`kCopyRing = 8` 个 allocator/list），
  Present 不再等跨卡拷贝；
- publication 队列（`g_pub_q`，每槽一条）与 `--pub-fifo` 控制消费顺序；
  默认开启 copy-on-publication + FIFO（`--no-pub-fifo` 仍拷贝但按键匹配，
  `--no-prefetch-publications` 回到轮转预拷）。

## 4. 一帧的主路径（`run_frame_ex`）

1. **颜色**：WGC 纹理（D3D11）→ keyed mutex → 三缓冲共享 sink
   （`mgpu_colour_sink0/1`）→ D3D12 转换 draw；
2. **marker**：从颜色四角解码 32-bit 短雪花（`seq12 | ms12` + CRC-8）；
3. **配对**：
   - 用 marker 的 `seq12` 在 `frames[]` 里找 `frame_id`（`+1` 为 staging 延迟）；
   - 命中则本帧 MV/depth 就来自该 publication；
   - 解不出时用 present-ring/时间戳启发式兜底，并打 `marker pair: heuristic slot X -> Y`；
   - 都没有则 `coast`（复用上一帧）+ `resetHistory`，并计数 `marker: miss`；
4. **输入**：
   - 预拷命中（`pf hits`）→ 直接用本地 VRAM 副本；
   - 未命中 → 在 COPY 队列补拷（`pf misses`）；
   - `--upsample` → 在同一列表里把 MV/深度重建到渲染尺寸；
   - `--stream-auto` → 按 WGC age 与带宽请求新的 `StreamPercent`；
5. **tag + 插帧**：设置 `frameRenderTime` / jitter / 矩阵 / 复位，
   `xefgSwapChainTagFrameResource` / `TagFrameConstants`，`SetNumInterpolatedFrames`；
6. **Present**：XeFG 呈递到覆盖窗口；成功后 ACK `consumed[slot]`；
7. **统计**：`breakdown` / `bus:` / `arr(vb):` / `pf:` / `marker:` / `HITCH` / `SLOW frame`。

## 5. 颜色 sink 与 marker

- 颜色 sink **三缓冲**：每帧翻 `g_sink`（`% 3`），SRV 堆三个描述符，转换 pass 永远采样刚写的那张；
- GPU 落后 >=3 帧时跳过本次拷贝、复用上一帧颜色，避免 `CopyResource` 被驱动串行化
  （双缓冲时代单帧 120–220 ms 的根因；三缓冲把安全余量从 1 帧提到 2 帧）；
- marker 回填在转换 pass 的 `ps_main` 里做：对四角像素的 UV 向内偏移一个块宽，
  零额外 pass/copy/barrier；呈现画面干净，游戏窗口本身仍带标记（设计如此）；
- `marker_disable.on`（生产端不画）、`marker_repair_off.on`（消费端不回填）、
  `dump_frame.on`（整帧回读）是排查开关。

## 6. 输入缓存与预拷

- 共享槽固定 4 个，每个槽的 MV/depth 是 placed resource；
- 消费端为每个槽维护本地副本（`g_local_mv/depth`），命中时直接当 SRV；
- 预拷在上一帧 Present 之后、在 COPY 队列上等 `fwd` 并拷下一槽；
- 未命中时本帧补拷；`--no-prefetch-inputs` 回到“WGC 到达后再拷”的旧顺序；
- ACK 会一次性退还所有不用的槽，避免队列长期停在 3/4。

## 7. 节奏与倍率

- 固定倍率：`--interp N`（`N=1` 为 2x）；
- 动态逐帧：`--interp-cadence`，按实际经过的 vblank 数决定本帧生成几张；
  `--interp` 此时只是容量上限；
- **动态逐帧的锚点（2026-11-14 第三轮）**：不再以 WGC 到达为锚，而是以
  **交给 XeFG 的时点**为锚。每批在 `TagFrameConstants` 前等到 vblank 槽位：
  `target_{n+1} = target_n + k_n × vblank`，批次首尾相接，每个 vblank 恰好一张输出。
  帧早到就等槽位，帧迟到才顺延；`--no-fg-pace` 可关闭做 A/B。
- **默认调度器：`--cadence-source governor`（旧 bridge 原理的兼容实现）**：
  `k = ceil(0.96 × 输出目标 / 输入帧率)`，即**固定整数倍率，输出不低于目标 96%**；
  输入快于整除点时多生成的帧由面板丢弃，视觉稳定在 115–120。只有输入持续越过
  阈值（约 24 帧）才允许降一档，升档立即执行，避免整数倍率抖动。
  旧的 `mean`/`tick` 相位调度保留为 A/B。
- **动态逐帧的默认机制**：
  1. 到达间隔取 16 样本均值的 EMA（`--cadence-smooth` 默认 **8**）；
  2. `phase += T_in × out_hz / 1000`，`k = round(phase)`，`phase -= k` 并夹在 ±0.5；
  3. **相位算出的 k 直接应用**到 `SetNumInterpolatedFrames(k-1)`，没有 step/hold；
  4. **count 只在 `gpu_throttle()` 之后、本帧 tag 之前应用**；
  5. **count 变更只在上一批输出已呈现后发生**（`GetLastPresentCount` 排空）；
  6. `frameRenderTime = k × vblank`，与 FG 输入槽位一致；
  7. **重复/复用捕获帧不推进相位**。
- 关键不变量：**本帧应用的 k 必须等于本帧 `frameRenderTime` 覆盖的 vblank 数**。
  旧实现用输入均值作为预算，3x/4x 切换时会把 k 帧挤进 k±1 个 vblank，pacer 只能丢拍。
- **实测限制（2026-11-14 CP2077 den1 + MV）**：固定 3x/4x 平滑
  （固定 3x：`HITCH=0`、`presents=120/120`、`load 13–54%`）。动态按上述批次边界 +
  批次预算重做后仍需真机复测；固定 3x（锁 40）/4x（锁 30）继续作为产品对照入口。
- `--cadence-source tick` 是 A/B；`--fg-out-hz` 限制输出帧率；3x+ 需要 `--unlock-mfg`。
- `--cadence-hold N` 在 `--cad-policy fill`（carry）下就是控制核心的 `minDwell`（默认 4，
  carry A/B 用 8/12/15），不是已退役项；`--cad-switch-loss measure|auto` 默认只测量
  “每次计数切换丢几张”，`auto` 才据此调 `minDwell`。


## 8. 流档位

| 开关 | 行为 |
|---|---|
| `fakerFG.ini: StreamPercent` | 生产端默认档位（默认 150≈2/3；100=1:1，200=1/2，400=1/4） |
| `--stream-auto` | 消费端按 WGC age/带宽请求每流百分比（2 窗确认 + 2 s 驻留） |
| `--stream-asym` | MV 保持 1:1、深度降档；隐含 `--upsample`（XeFG 要求同尺寸） |
| `--upsample` | 在消费卡把 MV/深度重建到渲染尺寸（A/B） |
| `--no-depth` | 不传深度（跨卡减半），代价是 disocclusion 变差 |

## 9. 诊断与日志

| 行 | 含义 |
|---|---|
| `breakdown:` | 每窗口均值：`capture/work/present/gpu/wait/copy/colour/mvs/cq/of/age/xell/fg/total/load` |
| `bus:` | 每帧跨卡字节与带宽 |
| `arr(vb):` | 输入到达间隔落在几个 vblank（必须是整数） |
| `pf:` | 预拷命中/未命中/队列溢出 |
| `marker:` | decoded / dup / fallback / miss / pub_stall |
| `HITCH` | 单帧间隔 >2× 期望时打印 |
| `SLOW frame` | 单帧 >55 ms 时打印阶段耗时 |
| `LATENCY` | compose→present / click→photons 估算 |

## 10. 当前已知问题（下一阶段重点）

1. 预拷 miss 3–8/40：选槽策略与 copy 队列深度可继续优化；
2. ✅ 颜色 sink 已改三缓冲（GPU 落后 >=3 帧才可能长阻塞）；
3. M3（input 线程 + 独立 matcher）尚未实现，presenter 仍承担配对与补拷；
4. 开关过多，实验路径与产品路径混在 `main.cpp`；
5. 纯逻辑（marker/cadence/stream controller）缺少单测；
6. 与 `bridge` 双树并存，修复需要手工同步。

> 优化优先级与验收标准见 [`../PROJECT_STATUS_2026-11-14.md`](../PROJECT_STATUS_2026-11-14.md) §5。
