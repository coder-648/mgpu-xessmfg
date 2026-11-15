# 组件文档：xess_mfg_consumer.exe（第二张卡上的呈现 / FG 控制）

> **v22（2026-11-15）**：WGC 抓屏已删除。颜色、MV、深度现在来自 producer 的
> **同一条发布（stream 0/1/2）**，槽号就是配对身份；本文中所有 WGC / marker /
> present-ring / 颜色队列 的描述仅作历史记录。当前设计见
> `docs/design/COLOUR_TRANSPORT_V22.md`。

代码：`src/xess_mfg_consumer/`（unity 构建）。最后更新：2026-11-15。

---

## 1. 角色

在另一张卡（本机 AMD）上：**捕获颜色 → 配对 MV/depth → 转换 → 交给 libxess_fg 生成 → 上屏**，
并在这里做所有 cadence 决策。

## 2. 模块（unity 依赖顺序）

| 模块 | 职责 |
|---|---|
| `platform.cpp` | 日志、退出、控制台/窗口基础 |
| `state_overlay.cpp` | 所有**跨模块静态状态**（配置、计数器、标志）+ OSD 状态 |
| `devices_capture_fg.cpp` | 适配器/设备、WGC 会话与帧池、XeFG/XeLL 初始化、fences |
| `cadence.cpp` | **cadence**：pacer、估算、cadence core 配置、vblank 校准、render-time 选择 |
| `shaders.cpp` | 运行时编译的 convert/prepare 着色器（HLSL 字符串） |
| `frame_io.cpp` | 共享块视图、槽位、retire、copy 队列、prefetch/FIFO、上屏统计 |
| `osd.cpp` | 屏幕字/图 |
| `warp_upsample.cpp` | `--upsample` 的联合重建（MV+depth 到渲染尺寸） |
| `stream_auto.cpp` | `--stream-auto` 的每流 StreamPercent 控制器 |
| `convert_warp.cpp` | 转换/贴 marker/scar 修复、`--fg-warp` 实验路径、tag 前置 |
| `pair_dump.cpp` | `--dump`、`qpc_ms()`、配对偏置工具 |
| `dump_of.cpp` | 光流的 dump/readback |
| `input_cache.cpp` | 本地输入缓存、ACK、prefetch、copy-on-publication |
| `frame.cpp` | **run_frame_ex()**：整帧主流程 + 报告 |
| `probe_mfg.cpp` | `--fg-probe`（只探测 DLL 能力） |
| `main.cpp` | 参数解析、初始化、事件循环、热键 |

## 3. 每帧主流程（`frame.cpp: run_frame_ex`）

> **顺序以 `TOTAL_ARCHITECTURE_AND_DATAFLOW.md` §6 为准**（按代码逐行核对）：
> marker 解码与配对发生在 layout 之前，cadence 判决发生在 `gpu_throttle()` 之前。
> 下面的列表是同一流程的摘要。

```
(1) service_shared_layout()        世代/尺寸变化 → 释放本地拷贝与 pub 队列
(2) collect_retired_inputs()       延迟销毁被 XeFG 引用过的纹理（16 presents）
(3) 取颜色（WGC 队列 pop）→ 计算 queue age = capture
(4) marker 解码 + present-ring 查询 → 配对状态机（见 §4）
(5) 选 slot（复用帧用 g_last_slot，否则 pair_slot_for_wgc）
(6) 输入：prefetch 命中 → 只排队等 fence；否则 copy_inputs_local（帧内现拷）
(7) ack_previous_slot()            归还已退休槽的 credit（consumed[]）
(8) cadence core Update()          → k / 预算 / regime / reason
(9) gpu_throttle()                 等上一帧 GPU（深度 1）
(10) fg_set_interp(k-1)            换档（须在上一批退休后）
(11) colour stall guard → sink 轮转 → keyed mutex → CopyResource
(12) D3D11 fence 信号 → D3D12 等 → convert draw（scRGB→BT.2020 PQ）
(13) scar 修复（把 marker 覆盖的 16×16 贴回）
(14) MV gate（无 MV/生产者停摆 → SetEnabled(0)，恢复 → SetEnabled(1)）
(15) TagFrameConstants(frameRenderTime) + TagFrameResource(MV) + (depth) + (UI)
(16) 可选 pacer（见 §8）→ Present(0) → Signal(g_frame_fence)
(17) GetLastPresentStatus → 统计 xN；窗口结束时写报告
```

## 4. 配对状态机（颜色 ↔ MV/depth）

输入：marker 解码值（`g_marker_seq`）、present-ring 查询（`ring_seq`）、WGC 时间戳。

| 情况 | 处理 | resetHistory |
|---|---|---|
| marker 解码成功且等于 ring seq | 精确配对 | 否 |
| marker 不可读，但 ring 恰好 +1 | 视为精确（fallback） | 否 |
| 同一 seq（游戏一帧 present 两次） | **复用上一帧**（同一 slot/MV），连续上限 6 | 否 |
| ring 落后且生产者仍在发布 | 复用上一帧 | 否 |
| 无法识别（`marker_slot<0`） | 复用 + **强制 reset** | **是** |
| 生产者停摆（菜单/加载） | 原样呈现，MV 门控会关 FG | 否 |
| 颜色队列溢出丢帧 | 只计数，**不再 reset** | 否 |

> 2026-11-15 实测：8 次强制 reset 里 6–19 次来自颜色溢出（已改为不 reset），
> pair-gap 只有 0–3 次。

## 5. 输入路径与跨卡 fence

- **本地缓存**：`g_mv_local[s] / g_depth_local[s]`（默认堆纹理）——XeFG 不能直接用
  跨卡共享堆的资源（会报 "Failed to allocate a copy of DEPTH"）。
- **三种拷贝策略（生效顺序）**：
  1. **copy-on-publication + pub FIFO（默认）**：`--prefetch-publications` + `--pub-fifo`
     都默认开；捕获线程在 WGC 空闲时把**每个新发布**拷进本地缓存，按发布顺序入队
     （上限 4；同一槽的新发布会顶掉仍排队的旧条目）；
  2. 单槽预取（`--prefetch-inputs`，默认开但**被第 1 条短路**）：只有关掉
     copy-on-publication 才生效——在上一帧 Present 期间为**下一槽**录复制；
  3. 兜底：帧内现拷（占关键路径，`mvs` 时间会跳）。
- **跨卡 fence 读不出值**（`GetCompletedValue` 返回 UINT64_MAX），所以：
  消费者用**显式 expect**（`g_fwd_expect[]`）+ 一次性事件等待，ACK 用 `frames[s].published`。
- **ACK**：`ack_previous_slot()` 把自己不再读的槽写 `consumed[s] = frames[s].published`
  （绝不做的是"正在读"和"下一帧要读"的槽）。

## 6. 色彩路径

```
WGC 纹理 ──CopyResource──► 共享 sink[i]  （3 张轮转 + IDXGIKeyedMutex(0,20ms)）
                            │
                            └─ D3D12 convert draw（scRGB FP16 → BT.2020 PQ，全屏三角）
                                  │
                                  └─ scar 修复（pre-marker 16×16 贴回）
```

**止损（2026-11-15）**：一次 sink 拷贝曾阻塞 207–222 ms（`SLOW frame colour=209`），
导致 250 ms 卡顿 + 队列溢出 + reset。现在三个先兆任一成立就跳过本次拷贝（复用上一张）：
① 上次拷贝 >40 ms；② D3D11 侧 ≥2 张在飞；③ D3D12 侧 3 帧未退。
keyed mutex 拿不到（20 ms 超时）也不再硬拷。日志：`cskip=`、`colour: ... reusing`。

## 7. 与 XeFG 的交互（libxess_fg）

| 调用 | 时机 | 说明 |
|---|---|---|
| `D3D12CreateContext` / `InitFromSwapChainDesc` | init | 传入刷新率、格式、MFG 上限；**投影/jitter 等 init 旗标在此固定** |
| `SetNumInterpolatedFrames(n)` | 换档时 | n = k−1；**必须等上一批退休**，否则丢一张 |
| `GetProperties` | init | `maxSupportedInterpolations`（本机 3 → 4x）；`--unlock-mfg` 是为了突破厂商限制 |
| `SetEnabled(0/1)` | MV 门控 | 关掉时每帧只出 1 张（**注意**：别把它算进"出帧不足"反馈） |
| `TagFrameConstants` | 每 Present | 含 `frameRenderTime`（**语义未文档化**，见 §11） |
| `D3D12TagFrameResource` | 每 Present | MV / depth / UI；`tag_list=nullptr` 表示"资源活到下次 Present" |
| `GetLastPresentStatus` | 每 Present 后 | `framesPresented` → `xN`、`NoteOutputShortfall` |
| `GetLastPresentCount` | 换档前 | 排空判断（**语义含糊**：按调用还是按张数，见 §11） |

## 8. cadence 决策与 pacer

### 8.1 策略（`--cad-policy`）

| policy | 选 k | 预算 | 默认 |
|---|---|---|---|
| `optimal` | (m,k,paced) 最小代价：`wSmooth·qEff + wFps·|out−target|/target + wLatency·lat + wOver·overEff` | `T` 或 `k·m·V`（paced） | **是** |
| `locked` | `under ≤ 5%` 取 floor，否则 ceil；3 帧共识 | `k·V` | 否 |
| `fill` | 相位进位 + 死区 | `T`（或 `k·V`，用 `--cad-budget`） | 否 |
| `even` | `ceil(0.96·target·T)`，升立即/降等 24 帧 | `T` | 否 |

关键量：`q = |slots − k·m| / k`（长保持率）、`over = max(0, k·f/R − 1)`、
`kImp = 5%` 感知死区、`kLockMargin = 3%` 换档门槛、`_lockVotesNeed = 3`。

### 8.2 Commit（只按生效 k 记账）

```cpp
_phase += lam - appliedK;      // lam = 本帧"应出"；appliedK = DLL 实际生效
_phase = clamp(_phase, ±max(deadband, 1.0));
```
被拒/被门限挡住时误差留在相位里，长期均值不漂。

### 8.3 速率源

`s.inputMs` = 生产者 `FrameShared.frame_time_ms` 的 16 样本中位数（**每呈现帧一次**）。
**不要**用 `frame_qpc_ms`（Evaluate 时刻，一帧 3.3 次）或我们自己的到达间隔（含节流）。

### 8.4 pacer（`fg_input_pace` / `fg_input_pace_ms`）

- 仅在 `fill/even`（或 optimal 的 paced 分支）使用；默认 optimal 非 paced 时**没有 pacer**。
- 2026-11-15 修正：网格改成"按期推进"，迟到只按**整 vblank 跳过**；
  旧实现 `next = now + k·vb` 会把迟到永久编进相位（`vphase` 游走的原因）。

### 8.5 vblank 校准（`vblank_init`）

- 用 **DWM `qpcRefreshPeriod`**（本机 8.3332 ms = 120.002 Hz），不可用时退回
  120 次 `WaitForVBlank` 的长基线平均；
- `refresh_ms()` 优先返回实测周期；`cc.refreshHz = 1000/period`；
- 每帧 Present 后算 `phase = (t_present − t0) mod period`，按窗口输出 `vphase`。

## 9. 门控与保护

| 机制 | 触发 | 动作 |
|---|---|---|
| MV gate | `active_mask` 无 MV 或 `pub_stall_frames ≥ 8` | `SetEnabled(0)`；恢复时 `SetEnabled(1)`。门控期间**不推进 cadence、不计 shortfall** |
| tag 判空 | MV/depth 资源未就绪 | 跳过该 tag（否则 XeFG 返回 ERR_OTHER） |
| colour stall guard | 见 §6 | 跳过拷贝、复用上一张 |
| shortfall 学习 | 连续 3 次 `framesPresented < k` | `learnedMaxK--`（门控期间不计） |
| `--lock N` / `--lock-auto` | 可选 | 丢早到的捕获帧，把输入压到 N（**不能**消除非整除失真，只规则化） |
| `--no-throttle` | 可选 | 去掉深度 1 的 GPU 等待（+1 帧松弛，copy queue 回退） |

## 10. 观测：日志与报告字段

### 10.1 每个 `--report N` 窗口

```
cadence: rt=…ms (feed) input=…ms = …vblanks -> interp=… (avg …x, …fps out …)
breakdown: capture/work/present/gpu/wait/copy/colour/mvs/cq/of/age/xell/fg/total
           + game=arr pin pq jitter load slow pace repres skip cskip worst(...)
out: presents=… expected=… vblanks=…
vblank: period=…ms phase mean/std/min/max (n=…)
cadence core: k m paced q out render policy j learnedJ reason dwell swloss regime
switch cost: N change(s), M short
```

### 10.2 判读要点（这轮踩过的坑）

| 字段 | 真实含义 | 陷阱 |
|---|---|---|
| `show=` | 交付帧率**平均** | 119 与 118 观感可能差很多 |
| `presents/vblanks` | 面板填充率 | 必须用实测 period |
| `arr=` | **我们的**到达间隔（受 pacer 限制） | 曾被当成游戏帧率 |
| `pin=` | 生产者每帧间隔（决策用） | — |
| `pq=` | Evaluate 时间戳间隔（诊断） | 不是帧间隔 |
| `rhythm` | `|k−slots|`、`|T/(k·V)−1|` | 预算 = k·V 时它测的是**速度** |
| `vphase` | 提交（Present 前）在 vblank 周期的位置 | 不是显示落点 |
| `late` | pacer 到期后取值 | **无诊断价值** |
| `swloss` | 每次换档少上屏几张 | 含在途帧与丢帧，偏高 |

### 10.3 事件行

`reset-history: forced (pair-gap a, colour-drop b, producer c)`、
`colour: <原因> - reusing the last frame`、`tag: MV/depth not ready`、
`HITCH gap=…`、`SLOW frame: total=…`、`pf: served/missed/in-flight/overflow`、
`mv: auto -> …`、`pair: WGC … -> slot …`。

## 11. 已知问题（截至 2026-11-15）

**已修 12 个**（详见 `CONSUMER_AUDIT.md`）：相位夹取无界、`AcquireSync` 返回值、
sink 顺序、跨线程计数、warp 空指针、marker 记账、copy-ring 超时 Reset、空 tag、
`--no-fg` 子串匹配、`--cad-policy locked` 映射缺失、整数边界抖动、速率估计 ×3。

**结构性问题（已记录、部分已修）**：

| # | 问题 | 状态 |
|---|---|---|
| 1 | pacer 网格以 `now` 重锚 → 相位永久漂移 | **已修**（整 vblank 跳过） |
| 2 | vblank 校准只在 cadence 模式 | **已修**（init_fg 无条件） |
| 3 | 代价函数看不见超额供给（`min(R, f·k)`） | **已修**（wOver 项 + 5% 感知死区） |
| 4 | `locked` 用 `round` 选 k，35fps 会选 105Hz | **已修**（under≤5% 取 floor，否则 ceil） |
| 5 | 串联平滑 → 阶跃滞后 433–533 ms | **待修**（中位数 16→8、smooth 8→4） |
| 6 | `GetLastPresentCount` 与 Present 调用同量纲 → 排空闸门恒真 | 待修 |
| 7 | pub 队列 overflow 10–20% → 帧内现拷上关键路径 | 待查（加深队列？） |
| 8 | 呈现线程无优先级保护 / 无 MMCSS | 待修（3 行） |

## 12. 不变式

1. `fg_set_interp` 只在上一批退休后调用；
2. `frameRenderTime` 与本帧 k 匹配；
3. sink 只在确定拷贝时轮转；
4. `consumed[]` 只在该槽 GPU 退休后写；
5. resetHistory 只来自真实配对失败或游戏声明；
6. 门控期间不推进 cadence、不计 shortfall；
7. 任何"输入速率"必须 per-presentation 为单位。

## 13. CLI 速查（只列影响链路的）

`--interp N`（容量）、`--interp-cadence`、`--cad-policy optimal|even|fill|locked`、
`--cad-budget input|vblank`、`--cadence-hold N`、`--cad-input producer|arrival`、
`--cad-switch-loss measure|auto`、`--fg-out-hz`、`--fg-rt-mode`、`--fg-rt-spike`、
`--pool N`、`--colour-queue N`、`--latency N`、`--buffers N`、`--no-throttle`、
`--pub-fifo/--no-pub-fifo`、`--prefetch-inputs`、`--upsample`、`--stream-auto`、
`--lock N/--lock-auto`、`--no-mv/--no-depth`、`--overlay-layered/--overlay-hidden`、
`--unlock-mfg`、`--report N`、`--dump N`、`--fg-probe`、`--list-modes`。

