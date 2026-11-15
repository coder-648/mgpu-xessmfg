# bridge.exe 结构（2026-11-14 拆分后）

> 🧊 **legacy**：`bridge.exe` 是当前消费端 `xess_mfg_consumer.exe` 的前身，
> 已冻结为 A/B 基线。当前消费端结构见 [`CONSUMER_ARCH.md`](CONSUMER_ARCH.md)；
> 本文只用于理解两棵树的同源关系。

`bridge.exe` 负责：WGC 抓取消费卡上的游戏画面 → 跨卡拿到 producer 的
MV/depth → 在第二张卡上跑 XeSS-FG → 按 vblank 呈现。它是**独立进程**，
不能注入游戏（ReShade 的 D3D12 hook 是进程全局的，第二厂商设备会死锁）。

## 1. 构建模型

```text
src/bridge/bridge.cpp          ← 公共 include / using + 模块索引（~110 行）
src/bridge/modules/*.inc       ← 16 个实现模块，按数字前缀顺序 #include
scripts/build_bridge.sh        ← 校验模块 → 暂存到 C:\temp\mgpu\bridge → cl
src/bridge/build.bat           ← 单文件 cl 命令（只编译 bridge.cpp）
```

**为什么还是一个翻译单元？** 现有的实现有 200+ 个文件级 `static` 状态
（设备、队列、fence、producer 视图、统计量、开关）。直接拆成多个 `.cpp`
需要先设计一个显式的 `BridgeContext` 并把每个引用改过去，这是一次独立的、
需要真机回归的大改。当前先做**物理拆分**：保持原有定义顺序，模块之间只允许
“使用上面已定义的符号”，因此编译语义与拆分前完全一致（exe 尺寸相同）。
下一步再按 §5 的顺序把叶子模块提升为独立 TU。

## 2. 模块表（依赖顺序）

| 模块 | 行数 | 职责 | 关键符号 |
|---|---:|---|---|
| `010_platform.inc` | 229 | 日志/退出、进程与窗口查找、z-order 恢复 | `logf_`, `hard_exit`, `find_game_window` |
| `020_state_overlay.inc` | 874 | 全局状态与开关、producer hints、overlay 窗口/ placement | `g_*` 配置开关, `overlay_rect`, `wnd_proc` |
| `030_devices_capture_fg.inc` | 561 | 适配器/设备、WGC、XeSS-FG 初始化、`--list-modes` | `init_devices`, `start_wgc`, `init_fg` |
| `040_cadence.inc` | 1652 | 到达间隔统计、render-time 输入、MFG 动态帧数、平滑 | `arr_*`, `fg_*_tick`, `cadence_next_slots` |
| `050_shaders.inc` | 430 | convert/resample/prepare 的 HLSL 与 PSO | `k_conv_hlsl`, `init_convert`, `init_prepare` |
| `060_frame_io.inc` | 920 | 输出统计、fence、publication 队列、流几何、本地拷贝、dummy 输入 | `pub_q_*`, `copy_inputs_local`, `g_pub_q` |
| `070_osd.inc` | 309 | OSD 窗口、topmost/z-order 修复、GPU 节流 | `osd_update`, `assert_topmost`, `gpu_throttle` |
| `080_warp_upsample.inc` | 477 | 实验 `--fg warp` 插值器 + `--upsample` 联合重建 | `k_warp_hlsl`, `record_upsample` |
| `090_stream_auto.inc` | 317 | `--stream-auto` 按流 StreamPercent 控制器、warp PSO | `stream_controller_tick`, `init_warp_pso` |
| `100_convert_warp.inc` | 287 | resize kernel 常量、convert draw、warp 呈现路径 | `conv_constants`, `run_frame_warp` |
| `110_pair_dump.inc` | 311 | WGC 时间戳↔槽配对、`--dump` 录制、颜色探针 | `pair_slot_for_wgc`, `dump_record` |
| `120_dump_of.inc` | 673 | dump 落盘、FidelityFX 光流 setup/record/selftest | `of_init`, `of_record`, `dump_flush` |
| `130_input_cache.inc` | 511 | 帧入口/退休/ACK、共享布局服务、COPY 队列、预拷/FIFO | `prefetch_publications`, `run_frame` |
| `140_frame.inc` | 1437 | `run_frame_ex`：单个 WGC 帧的完整录制路径 | `run_frame_ex` |
| `150_probe_mfg.inc` | 191 | `libxess_fg` 探针、`--unlock-mfg` 补丁 | `fg_probe`, `apply_unlock_mfg` |
| `160_main.inc` | 1325 | 参数解析、启动、capture/paced/shared-colour 三种循环 | `main` |

模块校验：`python3 scripts/check_bridge_modules.py src/bridge`（构建脚本会先跑）。
它检查索引与磁盘文件一致、没有重复/漏引、编号有序。

## 3. 修改规则

1. **新增代码**放到对应职责的模块；如果它依赖某个模块里的符号，就必须排在
   那个模块之后。不要为了“看起来整齐”重排模块——顺序就是依赖声明。
2. **不要跨模块拆 raw string / `#if` 块**。HLSL 都用 `R"HLSL(...)HLSL"`，切点
   必须落在顶层的空行（本次拆分用扫描器检查 raw string / 块注释 / 预处理深度）。
3. 模块内部保持现有命名：`g_` = 文件级状态，`k_` = 常量，`_proc` = Win32 回调。
4. 新的实验路径优先放进 `080`（质量/插值）或 `120`（光流）并在启动日志里明确
   标注 opt-in；不要再往 `140_frame.inc` 里堆新的独立子系统。

## 4. 当前的输入缓存有两种模式

| 模式 | 开关 | 行为 |
|---|---|---|
| 单槽轮转预拷（默认） | `--prefetch-inputs`（默认开） | 上一帧 Present 后拷下一槽，命中用 `g_pf_valid`；miss 帧内补拷 |
| publication 缓存 | `--prefetch-publications` | 每个新 publication 拷进对应槽，命中按 `local_pub[s] == published` |
| publication FIFO | `--pub-fifo` | 同前预拷，但主线程按 publication 升序 **pop 队头**，不做 key 比较 |

FIFO 的队列在 `130_input_cache.inc`（`pub_q_push/pub_q_pop`），生产端复用槽时
会丢弃引用该槽的旧 entry（`dropped by overflow`）。它只保证“拿到的 MV/depth 是
已拷贝的那一版”，不改变消费侧吞吐；`total > 输入间隔` 时仍会 drop。

## 5. 下一步（按风险从低到高）

1. **`run_frame_ex` 内部拆分**：引入 `FrameContext`（局部变量集合），拆成
   `frame_collect()` / `frame_prepare_targets()` / `frame_record_convert()` /
   `frame_tag_inputs()` / `frame_present()`。这是纯机械移动，用 A/A5 日志对比
   `gpu/present/total/skip` 验证。
2. **`main` 参数解析独立**：把 9136 起的大段 `wcsstr` 解析收进
   `parse_args(argc, argv) -> Config`，三种循环各自成为函数。
3. **叶子模块独立 TU**：`010_platform`（日志/窗口）和 `050_shaders`（无全局
   依赖的部分）最先可以拆；然后是 `120_dump_of`。
4. **实验路径隔离**：`--fg warp`、`--half`、`--mv-shadow` 目前仍在主路径文件里；
   确认无回归后移到 `experimental/` 或统一的编译开关，减少 `140_frame` 的分支。
5. **删除已验证废弃的开关**：按 `docs/findings/ARCHIVED_EXPERIMENTS.md` 的结论
   逐项移除，而不是一次性删除（每个都要先确认脚本/文档不再引用）。
