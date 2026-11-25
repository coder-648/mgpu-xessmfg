# 文档索引

23 篇平铺太乱，所以分三层。**新读者按 0 → 1 → 2 → 3 读，其余按需查。**

## 0. 当前状态（2026-11-25）

**当前发布：lock_frame 1.3.3**（共享协议 **v34**）。先看
[`../README.md`](../README.md)（项目说明）与
[`../lock_frame/docs/README.md`](../lock_frame/docs/README.md)（当前技术文档、发布说明、
审查记录）。本文件第 1–4 节是 0.14 时代的索引，**只作历史参考**。

| 文档 | 内容 |
|---|---|
| [`PROJECT_STATUS_2026-11-14.md`](PROJECT_STATUS_2026-11-14.md) | 全仓体检（**历史快照**）：链路、组件、文档新鲜度、风险、P0/P1/P2 backlog |
| [`PROGRESS_2026-11-15.md`](PROGRESS_2026-11-15.md) | 动态逐帧进度与基线确认（**历史快照**） |
| [`HANDOVER_2026-11-15.md`](HANDOVER_2026-11-15.md) | 交接清单（**历史快照**）：现行真值、已过时清单、部署状态、未完成项 |

> **当前链路** = `fakerFG.dll`（游戏进程：DLSSG 输入车道 + XeFG 输出）→
> `xess_mfg_producer.dll`（跨卡 MV / depth / UI 支撑域 patch，协议 v34）→
> `lock_frame.exe`（XeFG 上屏；颜色走**同卡 WGC 捕获**）。
> `bridge.exe` / `game_capture.addon64` / ReShade 安装器 / `xess_mfg_consumer.exe`
> 都是 **legacy**；旧版说明保留在 [`design/archive/`](design/archive/) 与
> [`ops/archive/`](ops/archive/)。
> 下面提到协议 17/22 或“WGC 已删除”的段落均已过时。

## 1. 先读这些


| 文档 | 内容 |
|---|---|
| [`../README.md`](../README.md) | **项目亮点**、三个组件与参数、按场景选配置、5 分钟上手、已知限制、如何参与 |
| [`../CHANGELOG.md`](../CHANGELOG.md) | **变更记录**：每个版本用户能感知的变化 + 对应实测依据 |
| [`RELEASE_NOTES.md`](RELEASE_NOTES.md) | **可直接粘到 GitHub Releases 的版本说明**（含下载与校验值） |
| [`findings/CONCLUSIONS.md`](findings/CONCLUSIONS.md) | **结论先行**：这个 FG 消费什么、三条硬约束、倍率的真相 |
| [`design/ARCHITECTURE.md`](design/ARCHITECTURE.md) | **当前架构**：fakerFG → producer → consumer、一帧生命周期、关键取舍 |
| [`design/CONSUMER_ARCH.md`](design/CONSUMER_ARCH.md) | **消费端架构**：16 个模块、线程/队列、marker 配对、预拷、流控 |

## 2. 动手

| 文档 | 内容 |
|---|---|
| [`ops/USAGE.md`](ops/USAGE.md) | **完整使用说明（当前链路）**：包内容、安装、启动、开关、自检、排障、卸载 |
| [`ops/BUILD.md`](ops/BUILD.md) | 构建四件产物 + legacy bridge、第三方依赖、打包、常见问题 |
| [`ops/DEPLOY.md`](ops/DEPLOY.md) | 当前部署（复制 `game/` + `deploy_faker_fg.sh`）、回滚、自检 |
| [`ops/CONFIG.md`](ops/CONFIG.md) | 占位符、环境变量、`fakerFG.ini`、consumer 参数、调试文件 |
| [`ops/UI.md`](ops/UI.md) | `mgpu_ui.exe` 界面与 consumer 参数的对应关系 |
| [`ops/DEPENDENCIES.md`](ops/DEPENDENCIES.md) | 依赖版本、钉住的 commit、重建命令（部分 add-on 时代内容） |
| [`ops/LEGAL.md`](ops/LEGAL.md) | **GPL-3.0 由来**、`--unlock-mfg` 声明、XeSS SDK 再分发限制 |

## 3. 设计与发现
| 文档 | 内容 |
|---|---|
| [`design/PROTOCOL.md`](design/PROTOCOL.md) | 共享内存协议 **v17** 的字段、语义、版本演进（§4 起的 v10 语义章节仍有效） |
| [`design/INPUT_SOURCES.md`](design/INPUT_SOURCES.md) | DLSS / FSR / XeSS 三个输入源的能力差异与扩展方法 |
| [`design/FG_CLAIM.md`](design/FG_CLAIM.md) | **FG 契约优先**：claim（让游戏以为在出帧）+ 在 FG evaluate 处抓全套输入 + 通用校验器 + 输出所有权（取代逐游戏探 SR） |
| [`design/FAKER_FG.md`](design/FAKER_FG.md) | **生产端新架构（定案）**：`fakerFG.dll` 拥有 FG 管线（provider + XeFG 输出 + 记录 API）；`game_capture` 已删除，传输改由 `xess_mfg_producer.dll` 承担。注入链、进程内接口、P0–P7 阶段与验收 |
| [`design/SYSTEM_MAP.md`](design/SYSTEM_MAP.md) | **三个组件的系统地图（2026-11-15）**：进程/适配器拓扑、职责边界、共享内存协议字段、一帧完整时序、不变式、版本演进 |
| [`design/COLOUR_TRANSPORT_V22.md`](design/COLOUR_TRANSPORT_V22.md) | **颜色入槽（当前）**：为什么删除 WGC、协议 v22（colour + MV + depth 同一发布）、producer/consumer 改动清单、动态逐帧适配分析、成本、验收矩阵与回滚 |
| [`design/TOTAL_ARCHITECTURE_AND_DATAFLOW.md`](design/TOTAL_ARCHITECTURE_AND_DATAFLOW.md) | **顶层视野（2026-11-15，代码核对版）**：全景架构图、三条数据流、逐环节算法（marker/配对/队列/fence/cadence/pacer/vblank）、状态机与不变式、字段→算法索引、分层定位表、15 处"文档 ≠ 代码" |
| [`design/PAIRING_100.md`](design/PAIRING_100.md) | **100% 配对（已落地）**：当前帧 marker、id+ms 精确匹配、验证失败走 1X、ACK 精确化、8 槽/v18、`pair:` 指标、实测步骤与 `scripts/pair_report.py` |
| [`design/COMPONENT_FAKERFG.md`](design/COMPONENT_FAKERFG.md) | **fakerFG 内部**：5 种模式、13 个模块文件、`MgpuFgFrame` ABI、marker/scar、已知语义陷阱、ini 关键键 |
| [`design/COMPONENT_PRODUCER.md`](design/COMPONENT_PRODUCER.md) | **producer 内部**：队列结构、每帧流程、槽位/信用窗口、StreamPercent、语义探测、诊断字段 |
| [`design/COMPONENT_CONSUMER.md`](design/COMPONENT_CONSUMER.md) | **consumer 内部**：16 模块、每帧 17 步、配对状态机、输入/色彩路径、XeFG 交互、cadence 与 pacer、日志字典、已知问题 |
| [`design/DYNAMIC_MF_ANALYSIS.md`](design/DYNAMIC_MF_ANALYSIS.md) | **动态多帧的理论与方案（2026-11-15）**：不可能三角的证明、为什么标量函数不够、被忽略的层级、代码层结构性缺陷、算法修正记录 |

| [`design/XESS_MFG_PRODUCER.md`](design/XESS_MFG_PRODUCER.md) | **生产端最终形态（已实现并实测）**：`xess_mfg_producer.dll` 取代 ReShade add-on —— 由 fakerFG 加载、只做"记录 → 槽位 → PCIe"；含 CP2077 实测证据与尚未搬过来的 `StreamPercent` |
| [`design/PIPELINE_V2.md`](design/PIPELINE_V2.md) | **历史链路（producer 重写前）**：流程图、一帧时间线、6 个同步点；当前见 PIPELINE_V3 + `PERF_ROUND_HANDOVER.md` |
| [`design/PIPELINE_V3.md`](design/PIPELINE_V3.md) | **链路 v3（2026-11-14）**：producer DMA、consumer 预拷、WGC 在消费卡、自动档位；配对主键已改为 marker，见 `findings/PERF_ROUND_HANDOVER.md` |
| [`design/BRIDGE_ARCH.md`](design/BRIDGE_ARCH.md) | **legacy** `bridge.exe` 源码结构：16 个模块的职责/依赖顺序（当前消费端见 `CONSUMER_ARCH.md`） |
| [`design/BRIDGE_V2.md`](design/BRIDGE_V2.md) | **legacy** bridge v2 设计：事件化、三线程、SPSC 队列；consumer 已吸收其中 M1/M2 部分 |
| [`design/XESS_MFG_CONSUMER.md`](design/XESS_MFG_CONSUMER.md) | **xess_mfg_consumer 设计**：独立源码树、M0–M4 里程碑；M2.5（marker/双缓冲/预拷）已实现，当前形态见 `CONSUMER_ARCH.md` |
| [`design/INPUT_CONTRACT.md`](design/INPUT_CONTRACT.md) | **输入契约与 drop 不敏感性**：为什么逐帧 colour↔MV 配对会结构性失效、producer colour shared / publication ring / 2x-only 的顶层方案 |
| [`design/PREFETCH_100.md`](design/PREFETCH_100.md) | **输入预拷命中率到 ~100% 的设计**：copy-on-publication 预拷线程、每槽本地缓存、命中/就绪判定与实现步骤 |
| [`design/TIMING_4K40.md`](design/TIMING_4K40.md) | **4K/40Hz 算账**：每个环节的理论耗时、三种“全流程总时间”口径（吞吐 / 预算 / 延迟）、以及 4K 3x@40 为什么装不下 |
| [`design/DECOUPLED_IDEAL.md`](design/DECOUPLED_IDEAL.md) | **生产/消费解耦的理想态**：两条覆盖条件（生产端拷贝 ≤ 帧内窗口、消费端拷贝 ≤ WGC 延迟）、当前代码逐条差距、4K/40 的收益与最小改动集 |
| [`design/STREAM_AUTOSCALE.md`](design/STREAM_AUTOSCALE.md) | **StreamPercent 自动计算**：以 WGC 延迟低分位为预算的动态控制器、各帧率稳态档位、改档代价与协议/生产端/消费端落点 |
| [`design/DYNAMIC_CADENCE_ANY_CAP.md`](design/DYNAMIC_CADENCE_ANY_CAP.md) | **动态逐帧的任意上限设计**：fillable / cap-limited / input-limited 三个 regime、双上限（面板 vblank + FG 卡成本）、切换成本学习、参考实现 `src/cadence_core.h` 与仿真入口 |
| [`design/MAGPIE_FG_PACING_REFERENCE.md`](design/MAGPIE_FG_PACING_REFERENCE.md) | **Magpie 的 XeSS-FG pacing 对照评审**：过期时隙不追加、启动估计 min-of-two、只归因自己的等待、输出间隔 p50/p95/p99、XeLL 单一限帧者；以及哪些深 hook 不要照搬 |
| [`design/OPTISCALER_FPS_PACING.md`](design/OPTISCALER_FPS_PACING.md) | **OptiScaler 固定帧率节奏控制**：`minimum_interval_us` 单一数字 + 唯一 owner、`FpsLimiter` 绝对时隙网格（过期不补/双侧重锚）、`FrameLimit` 相对间隔兜底、FG 倍率只乘一次（XeLL/Reflex Sync 例外）、2 ms 定时器+自旋、墙钟 deadline 与三个已知坑；含文件/行号索引 |
| [`design/PACING_VS_OPTISCALER.md`](design/PACING_VS_OPTISCALER.md) | **本仓节奏栈 vs OptiScaler/Reflex/XeLL**：把"节奏"拆成输入节拍/输出节拍/延迟三层；我们六段代码（发布驱动循环、`gpu_throttle`、`fg_input_pace(_ms)`、cadence core、legacy `--pace`、XeLL 只买延迟）与 9 种执行者逐项对比；同一目标 25 ms 的数字对照、四个本质区别（等待位置/`minimumIntervalUs` 语义/时间基准/能否发明帧）、缺陷清单与场景决策表 |
| [`design/PACING_ADOPTION_PLAN.md`](design/PACING_ADOPTION_PLAN.md) | **三方合流的采纳清单（P0/P1/P2）**：双卡硬约束（限帧必须在游戏进程、地板不可压、QPC 系统级共享）、P0 观测与守卫（XeLL 契约补全、单一 target/owner、超前重锚、失活状态机、分位数报告）、P1 控制律（相位锚换 `frame_qpc_ms`、切换验证/撤销/退避、Λ 自适应）、P2 结构与成本准入、明确不采纳项、目标架构与 7 组可回退实验矩阵；§10 自由渲染+固定倍率（均匀抽帧、MV 语义、m 按 p1 选）、§11 输入率两侧摆动（控制/重采样分离、相位浮动、单侧偏置、`in_step`/`out_step`）、§12 输出硬上限（槽驱动、INV-1/2/3、C=R/n、路线 A/B） |
| [`design/LOCK_FRAME_IMPROVEMENTS.md`](design/LOCK_FRAME_IMPROVEMENTS.md) | **lock_frame 改进规划与 v29 落地（固定倍率 + 游戏侧锁帧）**：不变式 L-INV-1..5、缺陷 D1–D9（`--lock` 用 `GetTickCount64`+整数 ms、测速被合成器量化看不到 41/35、`--lock-auto` 拒绝非整除、超供不丢/欠供只能 `--pace` 重复帧、XeLL 顺序、标记去重、3x/4x MV 复用）、P0/P1/P2 与逐项风险收益、默认设置基线、分阶段对比（S1/S2/S3）、实验 L1–L6；**§12 已落地**：游戏时钟测速、`--fg-rt-mode auto`（供得上 quant / 供不上 raw）、漏桶时隙抽帧门（`--no-slot-gate`）、`game clock:`/`in(vb):` 观测、V1–V5 验收与离线仿真表 |
| [`design/DYNAMIC_FRAME_DESIGN.md`](design/DYNAMIC_FRAME_DESIGN.md) | **dynamic_frame 设计（自由渲染 + 固定倍率 + 输出封顶）**：基线选 consumer v22 颜色入槽（丢帧可省跨卡拷贝）、槽驱动主循环（`slot_ms = m·1000/C`、同槽只留最新、相位浮动、空槽不推进）、`frameRenderTime = slot_ms`、MV 三方案（game/OF/producer 合并）与 auto 门限、D-INV-1..5、CLI、M0–M4 里程碑、F1–F7 实验、与 lock_frame 的分工 |
| [`design/UPSAMPLE_QUALITY.md`](design/UPSAMPLE_QUALITY.md) | **降采样后的升采样质量**：为什么双线性降采样是信息瓶颈、medoid/中位数、深度引导 JBU、非对称流（MV 全分辨率）与验证方法 |
| [`design/LOSSLESS_TRANSPORT.md`](design/LOSSLESS_TRANSPORT.md) | **用无损压缩替代降采样传输？（2026-11-21）**：两条不等式（字节/时间）、各流盈亏平衡压缩率、nvComp/hipComp 的五个落地障碍、"colour 换 R10 而不是压缩"、固定码率块变换设计草案与 gate 0–3；含 3080 Ti 实测（编码 0.13 ms/66 MB、降采样 0.30 ms）与 `scripts/lossless_ratio.py` |
| [`design/UI_UNBLEND_MODE4.md`](design/UI_UNBLEND_MODE4.md) | **B 方案定案（UI 流 + 消费端 unblend + XeFG mode 4）**：UI 流作为 composition 唯一门控、生产端只降采样 UI、消费端用 `UiUnblend` 重建 HUDless、tag 互斥与回退矩阵、成本与验收 |
| [`design/UI_HUDFIX_RENDER_CARD.md`](design/UI_HUDFIX_RENDER_CARD.md) | **渲染卡跑 hudfix、只传 UI 流的评估（2026-11-22）**：hudfix 产出的是 HUDless 而非 UI、"完整复用 = 完美兼容"不成立、UI 发布点必须在 present 侧（协议冲突）、5.5k–6.5k 行移植与新增 hook 面、渲染卡成本回归、三层门控与 M0 止损 |
| [`design/UI_PLANE_TRANSPORT_COST.md`](design/UI_PLANE_TRANSPORT_COST.md) | **DLSSG 车道的 HUDless / UI 传输成本（2026-11-22）**：逐标题平面证据、用生产码流内核实测的 UI 压缩比（4K 0.26–3.0 MB，11–128x）与 HUDless 折算价（R10 22.7–27.6 MB）、链路/环/编解码三项账、为什么半分辨率 UI 不该上码流、全分辨率 UI 与 HUDless 的取舍 |
| [`design/FORCE_UI_SUPPLY.md`](design/FORCE_UI_SUPPLY.md) | **能不能强制游戏按指定链路提供 UI（2026-11-22）**：把"强制"拆成链路/开关/产出三层；我们已强制链路（strip sl.dlss_g + 自答 GetState）、`enableUserInterfaceRecomposition` 在本架构无下游消费者、Khazan/CP2077 反例；可做的只有补齐 UI alpha tag 23/67/68 与普查；正解是"只要 HUDless，UI 由 present 侧差分" |
| [`design/UI_COMPUTE_QUALITY.md`](design/UI_COMPUTE_QUALITY.md) | **现有 UI 计算方式与理论效果（2026-11-22）**：synth/unblend 的低频/高频分解（低频恒等于 `H_low`）、mask 只决定黑分支、2-bit alpha 的有效阈值 ~0.83、SDR(RGBA8) 与 HDR(R10A2) 的差别、按 UI 类型的预期、以 `UIAlpha` 做"直接重建"的改进顺序；§3.6 给出"无损 + 不下采样"的重估与成本表 |
| [`design/XEFG_UI_INPUTS.md`](design/XEFG_UI_INPUTS.md) | **XeFG UI 合成的完整参数（2026-11-22）**：三种资源与 `Final=UI+(1−a)·HUDless` 公式、`uiMode`/`UITEXTURE_NOT_PREMUL_ALPHA`/composition state、每帧 tag 的 type/validity/base/size/state、模式 0–5 与必需资源矩阵、HDR 同格式/2-bit alpha 的厂商约束、我们的实现对照与错误码 |
| [`design/UI_COMPOSE_DLSSG_PLAN.md`](design/UI_COMPOSE_DLSSG_PLAN.md) | **DLSSG-only UI 合成落地计划（2026-11-22）**：Evaluate 采集 → 全分辨率 synth → 只传 UI → 副卡 unblend → mode 4/5；P0 普查/P1 纹理/P2 码流+每流槽数/P3 mode 5/P4 UIAlpha 的分阶段改动、协议 v31 字段、逐文件清单、验收矩阵、kill criteria 与回退 |
| [`design/UI_ITERATION_2026-11-23_SPARSE.md`](design/UI_ITERATION_2026-11-23_SPARSE.md) | **UI sparse 迭代记录（2026-11-23）**：1/2 稳定基线（8×8 group 早退 + 空 32×8 tile mode=3）、105%→103%、tag-time synth 与 R10 HUDless 快照两次失败实验、当前配置/构建哈希/复现步骤 |
| [`design/HUDLESS_SOURCES_DLSSFG.md`](design/HUDLESS_SOURCES_DLSSFG.md) | **DLSS-FG 输入链路的 HUDless 从哪来（OptiScaler 对照）**：6 条 `FgInput` 车道各自的 hudless API 面（SL tag / NGX `DLSSG.HUDLess` / FSR3 `currentBackBuffer_HUDLess` / ffx `presentColor` / XeFG `RES_HUDLESS_COLOR` / upscaler 的 Hudfix），逐游戏落点（CP2077、Khazan、PRAGMATA、Nixxes/UE/Unity）、L0→L5 兜底阶梯、validity/格式/尺寸闸门，以及 HudCutoff 为什么不能替代 hudless |

| [`design/PLAN_2026-11-13.md`](design/PLAN_2026-11-13.md) | **定案**：滤波器无条件改进 + `--upsample` 一个开关、XeFG 同尺寸约束下的取舍、A/B 计划与开关去留判据 |
| [`design/FAKER_FG_PORT.md`](design/FAKER_FG_PORT.md) | **fakerFG × OptiScaler 逐机制对照与移植清单**：入口/呈现/输入契约三段 + 已踩过的坑 + “不要再做的事” |
| [`design/FG_BACKEND.md`](design/FG_BACKEND.md) | "为什么不能当观察者"的论证（§9–§10 已作废，见 FAKER_FG） |
| [`design/VENDOR_COMPAT_PLAN.md`](design/VENDOR_COMPAT_PLAN.md) | 厂商兼容计划（输入源抽象 + 副卡泛化，M6.x） |

---

## 4. lock_frame 正式版

`lock_frame/` 是独立的固定倍率（2x/3x/4x）实现，文档不与主干混放：

| 文档 | 内容 |
|---|---|
| [`../lock_frame/README.md`](../lock_frame/README.md) | 构建、部署、运行、日志判读 |
| [`../lock_frame/docs/README.md`](../lock_frame/docs/README.md) | lock_frame 文档索引（规划/发布说明/测试步骤） |
| [`findings/QUALITY_VARIABLES.md`](findings/QUALITY_VARIABLES.md) | **画质/预算变量作用表** + 怎么自己复测 |
| [`findings/PERF_BASELINE.md`](findings/PERF_BASELINE.md) | **性能基线（别重测）+ 优化候选清单** + 怎么看 `breakdown` |
| [`design/PERF_OPT_2026-11-23.md`](design/PERF_OPT_2026-11-23.md) | **性能优化批 1**：`CodecStreamPercent` 档位、Dispatch 合法性修复、marker 逐帧 syscall 缓存、`capture=`/`g_frame_ms_ema` 计时修正、**sparse unblend**（空 UI tile 不采样、不跑 PQ，占用表由 codec 头派生）、**UI 本地纹理按 UI 槽位建**（省 ~0.8 GB）；§5 是编码器成本的实测方法（`ProfGpu` 时间戳 + `FrCodecAblate` 消融档） |
| [`findings/PERF_ROUND_HANDOVER.md`](findings/PERF_ROUND_HANDOVER.md) | **性能分析轮交接（2026-11-14）**：链路、日志速查、基线数字、杠杆排序、开关、五个坑 |
| [`findings/M1_EVENT_LOOP.md`](findings/M1_EVENT_LOOP.md) | **M1 A/B 实测（2026-11-14）**：A5 bridge vs 独立 consumer 的 `total`/`pf`/3x/`wakes/s`，以及 CPU 对比的注意事项 |
| [`findings/M2_CAPTURE_THREAD.md`](findings/M2_CAPTURE_THREAD.md) | **M2 实测**：capture 线程 CPU -1.37 ms/frame，但深度 2 队列 +20 ms 延迟；默认改深度 1 |
| [`findings/FINAL_CONFIG.md`](findings/FINAL_CONFIG.md) | 踩坑清单 §5 + 实验档案 §9（**历史附录，结论看 CONCLUSIONS**） |
| [`findings/HANDOFF.md`](findings/HANDOFF.md) | 里程碑历史 + §8.6 十轮实验档案 |
| [`findings/MFG.md`](findings/MFG.md) | 多帧生成（`--unlock-mfg`）的原理与字节补丁说明 |
| [`findings/XESS_FG_NOTES.md`](findings/XESS_FG_NOTES.md) | XeFG 开发指南的提炼 |
| [`findings/OPTISCALER_NOTES.md`](findings/OPTISCALER_NOTES.md) | XeFG 的 MV 约定 / 相机运动重投影（上游 GPL-3.0） |
| [`findings/UE5_MV_OPTISCALER.md`](findings/UE5_MV_OPTISCALER.md) | **OptiScaler 怎么处理 UE5**（影之刃/黑猴/明末）+ 我们缺的三步 |
| [`findings/FG_SNIFF.md`](findings/FG_SNIFF.md) | **只读抓游戏自己的帧生成**（`FgSniff=1`）：投影矩阵 / clipToPrevClip / MV 尺度从哪来、日志怎么读 |
| [`findings/FG_INPUT_V13_2077.md`](findings/FG_INPUT_V13_2077.md) | **协议 v13 场景 1：CP2077**（Streamline 当输入源 + `frame_key`↔WGC 配对 + 4K 预算） |
| [`findings/FG_INPUT_V13_KHAZAN.md`](findings/FG_INPUT_V13_KHAZAN.md) | **协议 v13 场景 2：Khazan（UE 4.27）**：SL 只做 DLSS-G、NGX 供数、**采集↔消费逐项对照**、`HUDLESS_COLOR` 未吃 |
| [`findings/FG_INPUT_V13_PRAGMATA.md`](findings/FG_INPUT_V13_PRAGMATA.md) | **协议 v13 场景 3：PRAGMATA（RE Engine）**：第一个真正 `input source: sl` 的游戏 —— 游戏自己的投影、`frame_key` 配对、`reset=1` 全部命中 |
| [`findings/INPUT_SEMANTICS.md`](findings/INPUT_SEMANTICS.md) | **输入语义的通用做法**：有效区裁切（`crop:`）+ 在线 MV 单位探针（`probe:`）+ 为什么不逐游戏打补丁 |
| [`findings/PBZ_NOTES.md`](findings/PBZ_NOTES.md) | **影之刃零专项**：FSR 自带 DLL 的 double-free 崩溃、实测输入语义、add-on 侧踩过的坑 |
| [`findings/OPTISCALER_QUIRKS.md`](findings/OPTISCALER_QUIRKS.md) | **OptiScaler 的 105 条游戏表逐条对照**：哪些能搬（模块发现、FG 输入口径），哪些不能（DXGI 伪装、swapchain 补丁），以及它揭示的两个缺口 |
| [`findings/RE_ENGINE_NOTES.md`](findings/RE_ENGINE_NOTES.md) | **RE Engine / PRAGMATA**：NGX 入口在游戏自带 `nvngx_dlss.dll` 上（修复待验证） |
| [`findings/TOOLING_NOTES.md`](findings/TOOLING_NOTES.md) | 工具链与命令行的注意事项 |
| [`findings/ENV_REPORT.md`](findings/ENV_REPORT.md) | 实测环境记录 |

## 4. 历史

| 路径 | 内容 |
|---|---|
| [`design/archive/ARCHITECTURE_reshade_addon.md`](design/archive/ARCHITECTURE_reshade_addon.md) | 旧架构：ReShade add-on + `bridge.exe`（协议 v10） |
| [`ops/archive/USAGE_bridge.md`](ops/archive/USAGE_bridge.md) | 旧发布包使用说明（bridge / game-pack） |
| [`ops/archive/BUILD_legacy.md`](ops/archive/BUILD_legacy.md) | 旧构建说明（add-on / bridge / ReShade） |
| [`ops/archive/DEPLOY_addon_pack.md`](ops/archive/DEPLOY_addon_pack.md) | 旧 ReShade add-on 部署包（拖拽安装器） |
| [`ops/archive/CONFIG_game_capture.md`](ops/archive/CONFIG_game_capture.md) | 旧 `game_capture.ini` 键表 |
| [`ops/archive/UI_bridge.md`](ops/archive/UI_bridge.md) | 旧 UI（启动 `bridge.exe`）的详细映射 |
| [`findings/milestones/`](findings/milestones/) | M0–M4 各里程碑的步骤与结果（当时的结论，未必仍然成立） |
| [`findings/ARCHIVED_EXPERIMENTS.md`](findings/ARCHIVED_EXPERIMENTS.md) | 已移除的历史实验脚本：每个当时的参数与结论 |

## 阅读提醒

- `docs/findings/milestones/` 是**历史记录**，里面有些结论后来被推翻了
  （最典型的是"MV 是死输入"）。当前结论只看 `findings/CONCLUSIONS.md`。
- 仓库里没有本机路径：看到 `<GAME_DIR>` / `<TOOLS_DIR>` 这类尖括号就是占位符，
  含义见 `ops/CONFIG.md` §1。
