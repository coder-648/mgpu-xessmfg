# M2 实测：capture 线程 + colour 队列（2026-11-14）

同一游戏会话，producer pid=24720：

- `bridge_V2_m1_2026-11-14_m2session.log`：`--no-capture-thread`（M1 单线程）
- `bridge_V2_m2_2026-11-14_depth2.log`：M2 capture 线程，colour 队列深度 2

参数同 A5：`--den 2 --pool 4 --interp 3 --unlock-mfg --interp-cadence
--fg-rt-mode auto --fg-ui none --pub-fifo --report 40`。

注意：M1 attach 时 producer `flags=0x0`，M2 attach 时已是 `0x2051`（同一会话里后跑的
进程看到更新后的 producer 状态），`mvs` 0.5 → 0.1 ms；CPU/`mvs` 对比仍有一点输入路径
差异。

## 1. 每窗口中位数

| 指标 | M1 单线程 | M2 深度 2 | 变化 |
|---|---:|---:|---:|
| `total` | 24.50 ms | 24.80 ms | +0.30 |
| `gpu` | 9.80 ms | 14.90 ms | +5.10 |
| `present` | 13.40 ms | 9.30 ms | -4.10 |
| `age`（capture 时刻） | 9.80 ms | **0.30 ms** | -9.50 |
| `capture`（队列驻留） | 0.00 ms | **29.90 ms** | +29.90 |
| `load` | 96.0% | 97.0% | +1.0 |
| `skip`（均值） | 0.46 | 0.00 | 略好 |
| `proc` ms/frame | **6.45** | **5.08** | **-1.37** |
| 单核占比 | **25.6%** | **20.5%** | **-5.1pp** |
| `qdrop`/窗口 | 0 | 0.45（最大 9） | 可接受 |
| `cap`/s | 0 | 80 | capture 线程在跑 |

`gpu`/`present` 仍是跷跷板；`total` 只 +0.3 ms，但 **`capture` 29.9 ms** 说明
presenter 几乎总是晚一帧取色：端到端 colour 延迟 ≈ `age` + `capture` ≈ **30.2 ms**，
M1 是 **9.8 ms**，实际延迟增加约 **20 ms**。

## 2. 输入/输出质量

| 指标 | M1 | M2 深度 2 |
|---|---:|---:|
| `pf` miss | 11（0.7%） | 15（0.9%） |
| `pf` overflow | 0 | 0 |
| `arr` 3x | 89.3% | 86.6% |
| `OUTPUT FRAMES MISSING` | 2/41 | 6/43 |
| `VBLANK MISSED` | 0 | 0 |

M2 的 3x 和 missing 略差，符合“队列多一帧延迟”的预期；不是 capture 线程本身丢帧
（`qdrop` 只有 0.45/s）。

## 3. 配对疑点（拖影/ghosting）

M2 的 30 ms 队列延迟会放大一个既有问题：**colour 与 MV/depth 的配对**。

- `pair_slot_for_wgc()` 目前**只按 `frame_qpc_ms` 选槽，完全没有使用 `frame_key`**；
- 第一次 `pair:` 日志显示 `frame_key=0`。旧版本只在启动时打印一次，无法判断后续
  key 是否恢复；M2 已改为前 8 帧 + 每 40 帧打印一次；
- colour 在队列里多等一帧时，producer 的 slot 可能已经被新一轮 publication 覆盖；
  `--pub-fifo` 的 4 项队列只能吸收有限延迟，超过后就会出现“颜色配到旧/新 MV”，
  这正是拖影的典型来源；
- M2 深度 2 的 `capture=29.9 ms` 与 3x 变差、`OUTPUT FRAMES MISSING` 变多同时出现，
  符合“配对延迟”而不是“capture 线程丢帧”（`qdrop` 只有 0.45/s）。

### 快速诊断（已加入，不用重编）

```bash
# 默认（时间戳配对）
mgpu_test_V2_m2.bat
# 选更旧的 producer 槽（如果 MV 比 colour 新一帧，应该改善）
mgpu_test_V2_m2_pair_m25.bat
# 选更新的 producer 槽（如果 MV 比 colour 旧一帧，应该改善）
mgpu_test_V2_m2_pair_p25.bat
# 完全回到 round-robin，作为配对失效的对照
mgpu_test_V2_m2_nopair.bat
```

每次日志看：

- `pair:` 行的 `key=`/`present=`/`id=`/`age=`：如果 `key` 一直为 0，说明 producer
  没有可用的 sequence key，M3 的“序号 matcher”必须先用时间戳推导；
- 如果某个 bias 明显消除拖影，说明是配对相位问题，应把该偏移并入 M3；
- 如果三个都不改善，拖影更可能来自 MV 语义/尺度或 cadence 4x/3x，而不是配对。

## 4. 结论与优先级

- capture 线程本身成功：CPU **-1.37 ms/frame**、`age` 从 9.8 ms 降到 0.3 ms，
  presenter 不再等 WGC；
- 但深度 2 的队列让 presenter 总是消费上一帧，`capture` 29.9 ms，端到端延迟变差，
  `OUTPUT FRAMES MISSING`/3x 也退了一点；
- 队列默认深度改为 **1**，并用 `--colour-queue N` 做 1/2/4 的 A/B；
- **配对/M3 应优先于 M4**：拖影是用户可见的主要缺陷，M4 的 pacer 在错误输入上
  调不出稳定结果；M3 的 input 线程 + publication 队列正好能同时解决 ACK/credit window
  和“colour 晚到导致 slot 被覆盖”的问题。

## 5. 下一步

1. 同一会话先跑 M1（单线程）与 M2（默认深度 1），确认 `capture`/`qdrop`/`total`；
2. 用 `pair_m25` / `pair_p25` / `nopair` 做拖影 A/B，并看 `pair:` 的 key；
3. 若偏置能消除拖影：进入 M3，把 input 线程 + publication 队列 + 序号 matcher 一起做，
   并把 WGC→producer 的固定延迟/偏置作为匹配参数；
4. 若偏置无效：先查 MV 语义/尺度与 cadence（4x/3x 复用同一 MV），再回到 M3。


## 6. 三组偏置复测（2026-11-14）

同一游戏会话，M2 默认深度 1：

| 运行 | `capture` | `total` | 3x | missing | pf miss | 选槽 |
|---|---:|---:|---:|---:|---:|---|
| `--pair-bias-ms -25` | 3.3 ms | 24.0 ms | 86.3% | 3/38 | 8 | slot3 21 次 |
| `--pair-bias-ms +25` | 4.1 ms | 24.6 ms | 89.1% | 10/55 | 10 | slot3 25 次 |
| `--no-pair-key`（round-robin） | 3.3 ms | 24.6 ms | 89.4% | 5/36 | 3 | — |

三组都仍有拖影。更关键的是：

```text
pair: slot N key=0 present=0 id=10923 age=...ms
```

- **所有 `key`/`present` 都是 0**，只有 producer 的 `frame_id` 非零；
- `pair_slot_for_wgc()` 只按 `frame_qpc_ms` 选槽，没有真正的序列 key；
- 偏置 ±25 ms 下 `age` 仍在 0–56 ms 分布，选槽在 0–3 之间跳动，
  固定相位补偿无法稳定覆盖实际延迟；
- round-robin 的 3x/missing 与偏置版相近，进一步说明**拖影不是简单的 ±1 帧选槽问题**。

### 结论：先做 MV/cadence 根因隔离，不要继续加偏置

在继续 M3 之前，先做两个决定性对照（同一场景）：

1. `--no-mv`（已有 `mgpu_test_A11_nomv.bat`）
   - 拖影消失 → 问题在 producer MV 内容/尺度/时间对齐，而不是选槽相位；
   - 拖影仍在 → 不是 MV，继续看 cadence/XeFG。
2. 固定 3x 或 2x（`mgpu_test_A6_fixed3x.bat`，或 `--interp 1 --lock 40`）
   - 固定倍率后拖影明显减少 → 是 `--interp-cadence` 在 3x/4x 间复用同一 MV 的问题；
   - 仍存在 → 是 MV 质量/2x 模型重复插值（日志已说明 3x/4x 复用 2x 模型）。

### 对 M3 的影响

M3 的“序号 matcher”需要 producer 与 consumer 共享一个 token。目前：

- producer 的 `present_id`/`frame_key` 为 0，`frame_id` 只是 producer 内部序号；
- WGC 只提供 `SystemRelativeTime`，没有 present id；
- 因此 consumer 无法构造真正的“同帧 key ↔ WGC 帧”匹配，M3 最多只能做
  “时间戳 + 固定/自适应延迟”的 publication 队列。

所以在 producer 能提供可用 key 之前，**M3 不应被当作拖影修复**；它仍能解决 ACK/credit
window 和 CPU 问题。优先级应是：

1. `--no-mv` / 固定倍率根因隔离；
2. 若指向 MV：先修 MV 语义/尺度/时间对齐（以及 producer 的 key）；
3. 若指向 cadence：先修 4x/3x 复用同一 MV 的策略；
4. 最后再做 M3 的输入线程/队列和 M4 pacer。


## 7. 根因更新：producer 时间戳不是 QPC

复测三组偏置后定位到时钟错误：

```cpp
// src/faker_fg/provider.cpp (before)
f.frame_qpc_ms = static_cast<uint64_t>(GetTickCount64());
```

- `GetTickCount64` 默认约 **15.6 ms** 粒度，且不是 QPC；
- consumer 的 WGC `SystemRelativeTime` 是 **QPC 100 ns**；
- 这解释了 `pair:` 的 `age` 为什么是 0.3/13.3/22.3/50.3 ms 这类量化跳变，
  以及为什么 ±25 ms 偏置都无法稳定选中同一槽。

已改为 `QueryPerformanceCounter` 换算的毫秒，并覆盖**所有** `frame_qpc_ms` 写入点：
`provider.cpp`、`proxy_streamline.cpp`、`proxy_nvngx.cpp`；consumer 的
`g_frame_ms_ema`/matrix 新鲜度判断也改为 QPC。重建并部署 `fakerFG.dll`（旧文件备份为
`dxgi.dll.before_qpc_2026-11-14` / `nvngx_dlssg.dll.before_qpc_2026-11-14`）。

下一次运行先看：

- `pair:` 的 `age` 是否变成连续、窄分布（不再以 ~16 ms 跳）；
- 默认/±25 ms 三组的拖影是否改善；
- 如果 age 已经稳定但仍有固定偏移，再做 present-time ring（在 present hook 记录
  `present_qpc_ms`/`present_seq`，consumer 用 WGC 时间戳匹配 present ring，再用
  `seq` 选择 publication）。


## 8. 时钟使用审计（producer / faker_fg / consumer）

| 组件 | 位置 | 计数器 | 用途 | 处理 |
|---|---|---|---|---|
| faker_fg | `provider.cpp` | `frame_qpc_ms` | Evaluate 帧时间 | **已改 QPC** |
| faker_fg | `proxy_streamline.cpp` | `frame_qpc_ms` | Streamline Evaluate | **已改 QPC** |
| faker_fg | `proxy_nvngx.cpp` | `frame_qpc_ms` | NVNGX Evaluate | **已改 QPC** |
| faker_fg | `proxy_nvngx.cpp` | `g_sr.tick` | 未读取的内部记录 | 可忽略 |
| faker_fg | `proxy_nvngx.cpp` | 模块扫描 500 ms | 模块发现节流 | 无影响 |
| faker_fg | `fg_swapchain.h` | waitable 超时 | Present 超时 | 无影响 |
| producer | `producer.cpp` | publish EMA | QPC → `frame_time_ms` | 已是 QPC |
| consumer | `frame.cpp` | `g_frame_ms_ema` | XeFG `frameRenderTime` | **已改 QPC** |
| consumer | `frame.cpp` | matrix 新鲜度 | 投影是否过期 | **已改 QPC** |
| consumer | `pair_dump.cpp` | `g_wgc_ts_ms` | 配对时间戳 | QPC（`SystemRelativeTime`） |
| consumer | `frame_io.cpp` | `g_last_arrival` 回退 | 无 timestamp 时的 dt | GetTickCount64，仅回退 |
| consumer | M2 `main.cpp` | `wall_ms`/`queue_ms` | 队列驻留统计 | GetTickCount64，仅报告 |
| consumer | `frame.cpp` | present/proc 统计 | 日志 | 仅报告 |
| consumer | `--lock`/`--pace` | `g_lock_next`/pace | 可选限速 | ms 级；A5 未使用 |

**仍存在的 ms 级环节**：

1. `MgpuFgFrame.frame_qpc_ms` / `FrameShared.frame_qpc_ms` 仍是 **uint64 毫秒**
   （QPC 换算后截断）。对 25 ms 帧距通常够用，但边界处仍有 1 ms 误差；如果要彻底
   消除，需要新增 `frame_qpc_100ns`（协议 v16）并在 consumer 优先使用。
2. `present_id/frame_key` 仍为 0，没有真正的序列 key；只能靠时间戳。
3. `--lock`/`--pace` 的 ms 计数器不影响 A5 默认路径，但开启时会回到 ~15.6 ms 粒度。

先验证三个 QPC 写入点是否让 `pair:` 的 `age` 变成连续分布；如果 ghosting 仍在，
再决定是升到 100 ns 时间戳还是直接做 present-time ring。


## 9. QPC 修复后的新观察（2026-11-14）

最新一次 `bridge_V2_m2.log`（fakerFG 已换成 `now_qpc_ms()` 版本）：

```text
pair: WGC ... -> slot 1 key=0 present=0 id=4549 age=12.2ms
pair: WGC ... -> slot 1 key=0 present=0 id=4555 age=12.2ms
pair: WGC ... -> slot 3 key=0 present=0 id=4558 age=12.2ms
...
pair: ... id=12145 age=-1.0ms
```

- `age` 已经连续（12.2/15.5/20.5/24.0 ms），说明 **QPC 时钟修复生效**；
- 但 `id` 大约每帧 **+3**，producer 日志也显示每个 WGC 帧对应 3 条 record；
- fakerFG 日志在同一毫秒打印 3 次相同的
  `record: upscaler lane ... mv=... depth=...`（指针相同）。

这意味着 producer 把同一个 Evaluate/SR record **重复发布 3 次**。它不直接改变 MV
内容，但会造成：

- 每个颜色帧有 3 个候选 publication，`pair_slot_for_wgc()` 只是按时间挑一个；
- `--pub-fifo` 队列、slot 轮转、producer credit window 都被 3 倍放大；
- `frame_id` 序列不再和 WGC 一帧一一对应，真正的序号 matcher 更难做。

**这仍不是拖影的充分解释**（3 条重复记录的 MV/depth 指针相同），但必须修：publish
应当一帧一次，而不是一次 Evaluate/一次 Present 重复三次。

下一步优先级：

1. **先做 MV/cadence 隔离**（日志自己已经提示 `--interp-cadence` + 3x/4x 会 ghosting）：
   - `--no-mv`（A11）：拖影消失 → MV 内容/尺度/时间对齐；
   - 固定 3x 或 2x（A6/`--interp 1`）：明显改善 → cadence 复用 MV 的问题。
2. **producer 去重**：按 `frame_qpc_ms`（同一 Evaluate 的重复 publish）或
   `(mv,depth,frame_qpc)` 去重，只发布一次；M3 的 publication 队列应一帧一条。
3. **present-time ring** 仍可作为最终方案，但当前 age 已连续，先不要用它掩盖 MV 问题。


## 10. 落地：present-time ring + 单帧发布

在 QPC 修正后 `pair:` 的 age 已连续，但 producer 每个 WGC 帧仍约发布 3 条 record
（`id` 每帧 +3；fakerFG 日志同一毫秒 3 次相同 `record: upscaler lane`）。因此这一步：

1. **present-time ring**（`src/mgpu_present_ring.h`）：
   - fakerFG 在 `hk_present` 的 `o_present` 返回后写入
     `{present_qpc_100ns, eval_qpc_100ns, record.seq, frame_key}`；
   - consumer `open_present_ring()` 映射只读视图；
   - `pair_slot_for_wgc()` 先用 WGC 时间找 ring，得到 `record.seq`，再按
     `FrameShared.frames[s].frame_id == seq` 选槽；ring 缺失/找不到才回退时间戳。
2. **producer 单帧发布**：`publish_frame()` 对相同 `frame_qpc_ms` 直接返回，避免重复
   copy / slot churn。
3. consumer 日志新增 `present-ring: seq=... -> slot ...`，可直接确认是否走 ring。

已部署：

- `dxgi.dll` / `nvngx_dlssg.dll` = `60fa098c...`
- `xess_mfg_producer.dll` = `fba8d322...`
- `xess_mfg_consumer.exe` = `37828a43...`

下一次运行检查：

- fakerFG 日志 `present ring: ready`；
- consumer 日志 `present ring mapped`；
- `present-ring: seq=... -> slot ...` 是否出现；
- `pair:` 的 `id` 是否不再每帧 +3；
- 拖影是否变化。如果 ring 命中后仍拖影，问题就在 MV 内容/尺度/cadence，而不在
  颜色↔MV 配对了。


## 11. present ring 已命中，重影与配对无关（2026-11-14）

最新一次 M2：

- `present ring: commit` 正常写入；
- consumer `present-ring: seq=... -> slot ...` **42 次命中**；
- `present-ring: seq=... not in slots` **0 次**；
- `pair:` 的 id 增量从每帧 +3 变为 **+1**（producer 去重生效）。

这说明颜色与 MV/depth 已经按 **record seq** 精确配对，重影仍然存在，因此剩余原因只可能是：

1. MV 内容/尺度/单位或 downsample 对齐；
2. 3x/4x cadence 复用同一 MV；
3. 2x 模型重复插值。

最新指标：`total` 24.7 ms，`gpu` 12.6，`present` 5.4，`capture` 5.2，`load` 94%，
3x 88.6%，`OUTPUT FRAMES MISSING` 5/35，`pf` miss 5。

下一步对照（已加脚本）：

- `mgpu_test_V2_m2_nomv.bat`（`--no-mv`）
- `mgpu_test_V2_m2_fixed3x.bat`（`--interp 2 --lock 40`）
- `mgpu_test_V2_m2_2x.bat`（`--interp 1 --lock 40`）

判定：

- `--no-mv` 后重影消失 → MV 内容/尺度问题；
- fixed 3x 后改善 → cadence；
- 2x 后改善 → 3x/4x 复用 2x 模型；
- 三者都仍有 → XeFG/输入格式问题，不再查配对。


## 12. 3x/4x 对照：重影来自 MFG 路径本身（2026-11-14）

| 运行 | `xN` | `total` 中位 | `load` 中位 | missing | 结果 |
|---|---:|---:|---:|---:|---|
| `--interp 1`（2x，固定） | 2.00 | 10.6 ms | 43% | 1/51 | **画面干净** |
| `--interp 2`（fixed 3x，无 cadence） | 3.00 | 8.8 ms | 39.5% | 2/78 | **仍有拖影** |
| `--no-mv` | 1.00（XeFG 停用） | 1.9 ms | 7% | 18/19 | FG 不生成，测试无效 |

结论：

1. **2x 干净**：说明 colour↔MV 配对、QPC 时钟、MV 内容在 2x 下都是正确的；
2. **fixed 3x 仍拖影**：去掉 `--interp-cadence` 后仍存在，因此不是 cadence 倍率变化；
3. `--unlock-mfg` 的日志明确写着：
   > every generated frame uses the 2x model (the MFG model is not in this build)
   即 3x/4x 是**重复使用 2x 模型 + 同一套 MV**，重影是这条 MFG 路径的固有限制；
4. `--no-mv` 不是有效对照：XeFG 需要 MV，去掉后它直接不生成（`xN=1`、`xNmiss` 大量）。

因此：

- 拖影不再是消费者配对/时钟问题；
- 3x/4x 要干净，只有几条路：
  - 使用 2x（当前最稳）；
  - 换带真实 MFG model 的 libxess_fg.dll；
  - 用 `--fg-warp`/自研按时间分数生成 MV 的路径（工作量大，另立里程碑）；
- M3（input 线程/队列）和 M4（pacer）仍可做，但不会修复 3x/4x 的 MFG 拖影。


## 13. M3：publication queue 按 seq 匹配

上一步虽然 present ring 命中了，但 `run_frame()` 的 `--pub-fifo` 分支仍然
`pub_q_pop()` 盲取最旧队头并覆盖 `pair_slot_for_wgc()` 的 `slot`。也就是说 ring 的
精确 seq 只用于日志，MV/depth 仍可能来自旧 publication。

M3 修正：

- `PubQueueEntry` 增加 `frame_id`；
- `pub_q_pop_seq(seq, ...)` 按 present-ring seq 找到匹配的 publication；
- `pair_slot_for_wgc()` 命中 ring 时记录 `g_pair_seq`，`run_frame()` 优先按它 pop；
- 找不到 matched seq 时打印 `pub-queue: seq=... not queued`，回退队头。

这之后，“颜色帧 seq → publication queue → 本地 MV/depth copy”是真正一致的。
如果 2x 仍干净、fixed3x 仍拖影，就可以最终确认 3x/4x 是 MFG 路径本身的问题。


## 14. 2x 轻微拖影：queue frame_id 快照时机

M3 第一次运行后，2x 也出现了轻微拖影。日志：

- `pub-queue: seq=... not queued` 出现 8 次；
- `pf` 1025 hits / 16 misses / 6 overflow；
- `present-ring` 本身命中正常。

根因：`prefetch_publications()` 先读取 `published` 并拷贝，最后才读
`g_ctl->frames[ss[i]].frame_id` 入队。拷贝期间 producer 可能已经覆盖同一 slot，
queue 里记的是**新 frame_id**，本地 MV/depth 却是旧 publication；于是
`pub_q_pop_seq(seq)` 找不到，回退到队头 -> 偶发旧 MV -> 轻微拖影。

修复：在读取 `published` 的同一轮循环里快照 `frame_id`，排序时和 `pp` 一起移动，
`pub_q_push` 使用这个 id。已部署 `xess_mfg_consumer.exe` `00099bc3...`。

下一次验证重点：

- `pub-queue: seq=... not queued` 是否归零或只剩启动 1 次；
- 2x 是否恢复干净；
- fixed3x 是否仍有拖影（若仍存在，才回到 MFG 模型/同一 MV 复用问题）。


## 15. 抖动后拖影：queue miss 回退 + history reset

现象：2x/3x 都会在一段干净画面后，遇到一次 drop/jitter，拖影出现，随后过一段时间
又自行恢复。

日志对应关系：

- `pub-queue: seq=... not queued - falling back to head` 出现；
- 同一窗口有 `qdrop` / `OUTPUT FRAMES MISSING` / `pf overflow`；
- 回退到队头时 MV/depth 可能来自另一帧，XeFG history 被污染，之后要若干帧才收敛。

修复：

1. `pub_q_pop_seq` 未命中时**不再取无关队头**：
   - 若 ring slot 的本地 cache 仍对应 `published`，使用它；
   - 否则不喂错误 MV，设置 `g_force_reset_history`。
2. `run_frame` 把 `g_force_reset_history` OR 进 `cd.resetHistory`，下一帧直接丢弃
   被污染的 XeFG history。
3. M2 colour 队列丢弃 / `--lock` 丢帧也设置该标记。

已部署 `xess_mfg_consumer.exe` `640e9982...`。这应把“抖动后拖影持续一段时间”缩短为
最多一帧的 reset，而不是等 history 自己收敛。

