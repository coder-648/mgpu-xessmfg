# lock_frame 改进规划（固定倍率 + 游戏侧锁帧）

> 对象：`lock_frame/`（v28，2x/3x/4x，WGC 颜色 + marker 精确配对）。
> 参照：`docs/design/PACING_ADOPTION_PLAN.md`（三方合流方案）、
> `docs/design/PACING_VS_OPTISCALER.md`（现状对照）、
> `docs/design/OPTISCALER_FPS_PACING.md`（OptiScaler 的限帧实现）。
> 姊妹文档：`docs/design/DYNAMIC_FRAME_DESIGN.md`（新版本，自由渲染 + 输出封顶）。
>
> **定位不变**：颜色走 WGC、MV/depth 走共享槽、倍率固定、输入节拍由游戏侧
> （游戏/RTSS）保证。本文只在这个前提下修正确性与节奏，**不引入第二套限帧 owner**，
> 也不把主循环改成槽驱动（那是 dynamic_frame 的事）。

---

## 1. 当前实现（作为改进的基线）

```
游戏（渲染卡）
  fakerFG：记录 MV/depth，画面四角画 32-bit marker
      │  xess_mfg_producer：只发布 MV + depth（StreamPercent=150）
      ▼
共享内存（32 槽）+ WGC（DWM 侧的颜色拷贝）
      │
lock_frame.exe（AMD）
  颜色：IDirect3D11CaptureFrame（scRGB FP16，HDR 原样）
  身份：解四角 marker → seq12/ms12/CRC8 → 与 frames[].frame_id 精确匹配
  时序：note_arrival（SystemRelativeTime）→ arr/jitter →（可选）--lock / --lock-auto
  生成：--mode 2x/3x/4x（g_fg_interp 固定）→ TagFrameResource/Constants → Present
  低延迟：XeLL 全套标记 + xellSleep（A 卡睡眠为空实现）
```

关键位置：

| 关注点 | 文件:行 |
|---|---|
| 架构与"为什么固定倍率" | `lock_frame.cpp:1-17` |
| `--lock` / `--lock-auto` 解析 | `modules/160_main.inc:330-341` |
| `--lock` 的实际丢弃循环 | `modules/160_main.inc:1380-1401` |
| `--lock-auto` 的 (rate, m) 求解 | `modules/030_devices_capture_fg.inc:505-576` |
| 到达间隔与 vblank 直方图 | `modules/160_main.inc:1024-1070` |
| XeLL 标记 + sleep 顺序 | `modules/140_frame.inc:437-445` |
| 其余 XeLL 标记 | `modules/140_frame.inc:914-923`、`:1248-1261` |
| `colour_seq` 水位（配对 ACK） | `modules/140_frame.inc:101-125` |
| `--pace` 固定 tick（会重复上一帧） | `modules/160_main.inc:1254-1268` |
| `g_no_xell` 只影响日志 | `modules/020_state_overlay.inc:279-280` |

---

## 2. 目标与不变式

| 不变式 | 含义 | 现在是否成立 |
|---|---|---|
| **L-INV-1** | 呈现帧率 ≤ 面板刷新 R | ❌ 超供时由 DWM 抽帧，不在我们控制下 |
| **L-INV-2** | 输入被接受的比例与倍率匹配（`accepted × m = R`） | ⚠️ 只在游戏精确锁帧时成立 |
| **L-INV-3** | 相邻被接受输入的间隔（`in_step`）恒为 `m` vb；欠供一个时隙时出现 `2m` vb | ❌ 未观测 |
| **L-INV-4** | 延迟有界（队列深度 ≤ 1） | ✅ WGC 池"排空取最新" |
| **L-INV-5** | XeLL 标记完整、frame id 单调 | ✅（但 sleep 在 START 之后，见 D6） |

---

## 3. 缺陷清单（按严重度）

### D1 `--lock` 的时基和单位都不够用（严重）

```cpp
// modules/160_main.inc:1391-1400
const ULONGLONG now = GetTickCount64();                            // 15.6 ms 粒度
const ULONGLONG period = (ULONGLONG)(1000.0 / g_lock_fps + 0.5);   // 整数 ms
if (now + 1 < g_lock_next) { ++g_lock_drops; continue; }
g_lock_next += period;
```

三个问题：

1. `GetTickCount64` 在多数系统上按 15.6 ms 跳，`now + 1` 的判据在两次跳之间
   **只会误判**：40 fps（25 ms）时一个 tick 里可能同时看到"早到"和"迟到"的帧。
2. `period` 是整数 ms：60 fps → 17 ms（实际 58.8 fps），30 fps → 33 ms（30.3 fps）。
   最常用的 60 档直接偏 1.2 fps。
3. 代码注释自己写了"WGC 到达有 1–2 ms 抖动，用 GetTickCount64 做早到就丢会周期性丢真实帧，
   输入 30 接受成 20"（`:339-341`），于是 `--mode` 干脆不启用 `--lock`——
   **正确的问题 + 错误的结论**：该换时基和网格，而不是放弃。

### D2 输入节拍测不到 41/35（严重）

`note_arrival` 用 `IDirect3D11CaptureFrame::get_SystemRelativeTime` 作为到达时间
（`:1040-1059`）。那是**合成器时间戳，被量化到 vblank**：23.8 ms（42 fps）和
25.6 ms（39 fps）都会被读成 3 vb = 25 ms，直方图 `g_arr_hist[k]` 只能落在整数 vb 上。

所以现在的锁帧版**从数据上就看不见** 41–42 / 35–39 这两侧摆动，所有 cadence 逻辑
（`--interp-auto`、`--lock-auto`、`--fg-rt-mode quant`）都建立在一个被量化的输入上。

更准确地说：游戏侧的未量化数据其实**已经在结构体里**——

| 字段 | 含义 | 现状用途 |
|---|---|---|
| `frames[].frame_time_ms` | producer 测的"游戏两次 Present 的间隔"EMA（未量化） | 只在 `140_frame.inc:993-1001` 分桶成 `game(vb)` **诊断直方图**，且是 1 vb 分辨率 |
| `frames[].frame_qpc_ms` | producer 抓这一帧时的 QPC 毫秒（1 ms 分辨率） | 只用于 marker 配对与 `ms12` |

两者都没有进入 cadence / `rt_*` / `--lock-auto` 的决策路径。

### D3 `--lock-auto` 只接受整数比，非整除直接放弃（中）

```cpp
// modules/030_devices_capture_fg.inc:531-539
const double ratio = cap / lock;
if (dabs_(ratio - round(ratio)) > 0.12) { continue; }   // 26 -> 24 fps 被拒绝
```

注释的理由是对的："丢 1 in 13 会出现一个 2T 间隙"。但正确解法是**把丢弃均匀分布**
（Bresenham），而不是拒绝——43 fps 这类游戏现在永远拿不到 exact cadence，
退回"不锁"，于是又回到超供。

### D4 超供（41–42）没有任何处理（严重）

`--mode` 不启用 `--lock`（`:364-369` 还把 `g_lock_auto` 关掉），所以输入比目标快时：
WGC 池"排空取最新"会**丢掉池里的旧帧**，但丢弃时刻由 DWM 决定，不在 `m·vb` 网格上。
结果：XeFG 每批出的 m 张输出挤在 `m·vb` 内，而输入更快 → 呈现帧率 > R →
**由 DWM 抽帧**。抽哪张、隔多久，都不受控。

### D5 欠供（35–39）没有任何处理，也没有观测（中）

`g_fgi_*` 那套相位/迟到机制只在 `src/xess_mfg_consumer` 里有，lock_frame 没有。
lock_frame 的补偿只有 `--pace`（固定输出 tick + 重复上一帧），而它：
- 默认关闭；
- 与 XeFG 自身的 pacing 串行（`src/bridge` 里有明确告警）；
- **重复上一帧会复用 `present_id`**，XeLL 会打
  `Marker already submitted for frame:` / `Different marker submitted before ...`。

### D6 XeLL 的调用顺序与观测（低，但便宜）

```cpp
// modules/140_frame.inc:437-445
xellAddMarkerData(g_xell, fid, XELL_SIMULATION_START);
xellSleep(g_xell, fid);
xellAddMarkerData(g_xell, fid, XELL_SIMULATION_END);
```

OptiScaler 是**先 sleep 再打 START**（`ll_xell.cpp:125-132`）；libxell 里有
`Sleep() called after markers.` 这类告警串。另外没有 `XELL_INPUT_SAMPLE`、
没有 `xellSetLoggingCallback`，`xell=` 恒为 0 也没有解释。

### D7 标记无去重（低）

OptiScaler 在 `input_common.cpp:658-665` 对"同类型 + 同 frame id"的重复标记直接返回。
我们没有，`--pace` 的重复帧会直接污染 XeLL 的帧统计。

### D8 3x/4x + 游戏 MV 的已知闪烁（中，属质量）

`FINAL_CONFIG.md` §5.17：固定 3x/4x 会"用一个 MV 集服务多个时间分数"，
表现为闪烁/重影；`--interp-cadence` + MV 更差。当前只有启动日志提示（`:395-408`），
没有自动降级。

### D9 观测缺口（中）

没有 `in_step`/`out_step`、`drop%`、`underfill%`，也没有"提交间隔 vs 显示间隔"的区分；
`drops = g_lock_drops + g_pace_skips`（`modules/070_osd.inc:106`）把两种完全不同的丢帧混在一起。

---

## 4. P0：正确性与观测（不改节奏模型）

| 项 | 改动 | 落点 | 验收 |
|---|---|---|---|
| **P0-1 换时基** | `--lock` 改用 QPC + 双精度周期 `1000.0/fps`；绝对网格 `next += period`；早到判据改为 `now < next - 0.5·period` | `modules/160_main.inc:1391-1400` | 60 fps 档实测 `60.0 ± 0.1`；`lock_drops` 不再周期性抽风 |
| **P0-2 换测速源** | 用 `frames[].frame_qpc_ms` 的逐帧差分（1 ms 分辨率）+ `frame_time_ms`（EMA）作为 cadence 与 `rt_*` 的**唯一来源**；`game(vb)` 直方图改成 0.25 vb 分桶；`--lock-auto` 的 `cap` 也改用它 | `modules/140_frame.inc:993-1001`、`modules/060_frame_io.inc:160-186`、`modules/040_cadence.inc:612-631` | 41–42 / 35–39 能按 ~1 ms 分辨率看到；`--fg-rt-mode auto` 变得可信 |
| **P0-3 `in_step` 观测** | 记录"被接受帧之间的 `frame_qpc_ms` 差 / vb"，窗口直方图 `{m, 2m, ≥3m}` + `repeats/min` | 报告行（`modules/140_frame.inc:1455-1465`） | 锁 40 + 3x 时 `{3}` 单桶；42 fps + 门开时 `{2.9, 5.7}`（≈95%/5%）；门关时 `{2.9}` 单桶 |
| **P0-4 丢帧分类** | `lock_drops` / `slot_drops` / `repeats` 分开计数，OSD 不再合并 | `modules/070_osd.inc:106`、`:140_frame.inc:1462` | 三种丢帧各自可见 |
| **P0-5 XeLL 契约** | sleep 移到 `SIMULATION_START` **之前**；补 `XELL_INPUT_SAMPLE`；接 `xellSetLoggingCallback`（限流） | `modules/140_frame.inc:437-445` | `xell=` 变非 0，或日志明确 `No sleep algorithm available.` |
| **P0-6 标记守卫** | 同类型同 id 丢弃；`g_reuse_frame` 时**不发**标记；`present_id` 严格递增（断言 + 计数） | `modules/140_frame.inc:437,914,1248` | 不再出现 `Marker already submitted`；`--pace` 下 XeFG 不抖 |

> **P0-2 的配套（P0-2b）**：`--fg-rt-mode` 默认从 `kFgRtQuant` 改成 `auto`。
> `quant` 会把 28.6 ms 吸附成 25 ms，只有换到干净的测速源之后 `auto` 才可信；
> 两者必须一起上，否则 P0-2 看不到画面收益（详见 §11.2/§11.3）。

---

## 5. P1：两侧摆动的处理（lock_frame 版）

### P1-1 `--lock` 支持非整数比例（Bresenham）

```cpp
// 目标：每秒接受 lock_fps 帧，输入 f 帧/秒
acc += lock_fps / f_game;                    // f_game 来自 P0-2，慢 EMA（0.5 s）
if (acc < 1.0) { ++slot_drops; continue; }   // 丢这一帧
acc -= 1.0;                                  // 接受
```

与 `--lock-auto` 的整数比约束（D3）二选一：默认 Bresenham，
`--lock-auto` 在"正好整除"时优先（零误差），非整除时交给 Bresenham。
验收：43 fps 输入 + 锁 40 时 `in_step` 只出现 `{3,4}`，4 vb 的占比 ≈ 3/43，无连续两个 4 vb。

### P1-2 `--mode` 默认启用"时隙丢弃"（治超供）

固定倍率下 `slot_ms = m·vb`（C=R 的特例）。在 `run_frame` 之前判断：

```
if (now < next_slot) { ++slot_drops; continue; }   // 同一时隙的第二张：丢
next_slot += slot_ms;
```

`--mode 3x` 在 42 fps 输入下仍只接受 40 张/秒，**输出不会超过 R**，
而且丢在 WGC 帧上（省掉拷贝/转换/FG）。
lock_frame 的 ACK 是 marker 驱动的 `colour_seq` 水位（`140_frame.inc:101-125`），
丢帧不需要额外 ACK：下一张被接受的帧会一次性把水位推过去。

验收：41–42 fps 时 `presents/vblanks ≤ 1.001`，`slot_drops ≈ 1 − R/(m·f)`，
`in_step` 恒 `{m}`（因为丢弃发生在时隙格上）。

### P1-3 欠供的相位浮动（治 35–39）

lock_frame 没有 pacer，欠供全靠面板重复吸收。最小改动：
`--lock` 的网格 resync（`:1400`）改成 `next = max(now, next) + period`
（迟到浮动，不吸附整周期），并记录 `underfill` 事件。
这样 35–39 的亏损摊成偶发 `m+1` vb 步，而不是"整整一个时隙的定格"。

### P1-4 `--lock-auto` 的目标从 median 改成 p5

```cpp
// 现在：const double cap = 1000.0 / arr_median_ms();   // 中位数
// 改成：cap_p5 由 16 样本直方图取，并要求 lock ≤ cap_p5 * 0.98
```

理由：中位数意味着一半时间跑不进去；`--lock-auto` 的目的是"长期稳定"，
应按**下分位**选，并允许一部分丢弃（与 P1-2 配合）。

---

## 6. P2：质量与结构

| 项 | 内容 |
|---|---|
| **P2-1 3x/4x 的 MV 策略** | 固定倍率下 MV 复用的闪烁是已知问题：`--mode 3x/4x` + 游戏 MV 时默认警告一次，并给出两条路（`--no-mv` / `--fg-of`）；或按 `--of-miss-fallback` 自动切光流 |
| **P2-2 高分辨率定时器** | `--pace` 与 `--lock` 的等待改 `CreateWaitableTimerExW(HIGH_RESOLUTION)`，减少 1 ms 睡眠过冲 |
| **P2-3 观测口径** | 报告里把 `arr`（合成器）与 `game`（游戏时钟）并列，注明前者被 vblank 量化 |
| **P2-4 `--pace` 的去留** | 与 XeFG 同开会串行；建议在 `--mode` 下直接拒绝（打一行错误），只保留 `--fg-warp` 可用 |

---

## 7. 实验矩阵（120 Hz，2x/3x/4x）

| # | 改动 | 对照 | 通过标准 |
|---|---|---|---|
| **L1** | P0-1 + P0-2（时基与双口径） | 现状 | 60 fps 锁实测 60.0±0.1；日志能区分 41/35 |
| **L2** | P1-2 时隙丢弃 | 现状（DWM 抽帧） | `presents/vblanks ≤ 1.001`；`slot_drops` 可见；`in_step` = `{m, 2m}`（≈95%/5%） |
| **L3** | P1-1 Bresenham（43→40） | 现状（不锁） | `in_step` `{2.8, 5.6}`，5.6 的占比 ≈ 3/43，无连续两个长间隔（--lock 路径，本次未落地） |
| **L4** | P1-3 相位浮动（35–39） | 现状（整时隙） | `repeats/min` 下降、无连续 2 次；`underfill` 可见 |
| **L5** | P0-5/P0-6 XeLL 顺序 + 去重 | 现状 | `xell=` 非 0 或明确不支持；无 `Marker already submitted` |
| **L6** | P2-1 MV 提示/自动光流（3x/4x） | 现状 | 闪烁类主观项改善；`of=` ≤ 0.5 ms |

每组 2 分钟同场景，报告必须给：`in_step` 直方图（m / 2m / 3m）、`out` 的 p50/p95、`slot_drops`、`underfill`、
`presents/vblanks`、`age`、`rhythm`、主观。

---

## 8. 风险与回滚

| 风险 | 影响 | 回滚 |
|---|---|---|
| P1-2 时隙丢弃与 WGC 抖动叠加，误丢真实帧 | 输入率下降（30 而非 40） | 开关 `--no-slot-gate`；先用 P0-2 的双口径确认误丢率 |
| Bresenham 在 f 快速变化时图案抖动 | 步长不均 | `acc` 只在 f 的 0.5 s EMA 变化 >5% 时重算 |
| XeLL 顺序调整改变 XeFG 呈现 | 节奏变化 | 单次 A/B，一行回退 |
| 相位浮动让网格离开 vblank | `grid_phase` 漂移 | 限制 ±1 vb，超出强制重锚 |

**总原则**：每个改动都必须"单开关、可 A/B、失败即回旧行为"，不做跨模块重构。

---

## 9. 与 dynamic_frame 的边界（本文明确不做）

| 不做 | 原因 | 在哪做 |
|---|---|---|
| 槽驱动主循环（定时等待 + 同槽丢弃） | 改变整个帧循环语义，超出"改进" | `DYNAMIC_FRAME_DESIGN.md` |
| 颜色从 WGC 换成 stream 0 | lock_frame 的带宽收益建立在这个选择上 | 同上（dynamic_frame 用颜色入槽） |
| 输出上限 C < R 的一般化 | lock_frame 的 C 恒为 R | 同上 |
| 自由渲染（不锁游戏） | 与 lock_frame 的定位冲突 | 同上 |

---

## 10. 一句话

lock_frame 的问题不在"固定倍率"，而在**测量**和**丢弃**：
测速被合成器量化（D2）、`--lock` 的时基和单位不够（D1）、超供时不敢丢（D4）、
欠供时只能靠 `--pace` 重复帧（D5）。
把这三件事修好（P0 + P1），它在 41/35 场景下的表现就能追上 `src/xess_mfg_consumer`
现有的 `fg_input_pace`，而带宽收益保持不变。

---

## 11. 逐项风险·收益，以及相对默认设置的提升

### 11.1 默认设置基线（v28，逐项对照代码）

| 项 | 默认值 | 位置 |
|---|---|---|
| 颜色 | **WGC**（`g_fg_colour_shared` 被强制为 false） | `160_main.inc:736` |
| 倍率 | `--mode`（UI 必传）；无 mode 时用 `g_fg_interp_max` | `040_cadence.inc:885` |
| 输入锁 | **不做**：`--lock` / `--lock-auto` 默认关，且 `--mode` 会再关一次 `lock-auto` | `160_main.inc:330-369` |
| `--pace` | 关（`g_pace_ms = 0`，到达驱动） | `060_frame_io.inc:189` |
| `frameRenderTime` | **`kFgRtQuant`**：16 样本中位数吸附到整 vblank | `040_cadence.inc:24` |
| cadence 测速源 | WGC `SystemRelativeTime`（**vblank 量化**） | `160_main.inc:1036-1059` |
| `--interp-cadence` / `--interp-auto` / `--dyn-mfg` | 全关 | `160_main.inc` |
| WGC 池 / slack | 4 / 0（排空取最新） | `020_state_overlay.inc:274,330` |
| swapchain | buffers 2，max latency 1，不 tearing | `020_state_overlay.inc:273-277` |
| prefetch 三件套 | **强制全关**（锁帧版不接受"猜槽"） | `160_main.inc:735-742` |
| `--upsample` | 开（MV/depth 150% → 联合重建） | `160_main.inc:745` |
| MV 来源 | `g_mv_auto = true` → 用游戏 MV；**`MGPU_FG_OF=0`，光流默认没编进来** | `020_state_overlay.inc:196,130-131` |
| XeLL | 6 个 required 标记每帧发；`xellSleep` 在 `SIM_START` **之后**；`--xell` 只改日志 | `140_frame.inc:437-445` |

**默认路径（120 Hz、3x、游戏锁 40）的真实行为**：

| 输入 | 我们输出 | 默认下发生什么 |
|---|---|---|
| 42 fps | 126 | **超面板 5%，由 DWM 抽帧**，位置不受控 |
| 39 fps | 117 | `frameRenderTime` 恒 25 ms（quant 吸附），而真实间隔 28.6 ms → **每批尾部约 3.6 ms 空档** |
| 35 fps | 105 | 12.5% 的 vblank 没有新内容，且同样**集中在每批尾部** |
| 43 fps | 129 | 同 42，超 7.5% |
| 60 fps 档 + `--lock 60` | 58.8 | 整数 ms 周期（17 ms）+ 15.6 ms 时钟 |

### 11.2 逐项风险 / 收益

| 项 | 风险（触发条件 → 严重度 → 回滚） | 收益 | 默认路径可见变化 | 粗估 |
|---|---|---|---|---|
| **P0-1 换时基** | 只影响 `--lock`；默认不启用 → 低 → 删开关即旧行为 | `--lock 60` 从 58.8 修正到 60.0；丢弃时刻正确 | 默认无（除非开 `--lock`） | 0.5 人天 |
| **P0-2 换测速源** | 改变 cadence 输入源：`--interp-auto`/`--lock-auto`/`rt_*` 的判据都会变 → 中 → 保留旧源做 A/B | 41/35 首次可测；`rt_*` 与 `--lock-auto` 的 `cap` 变准 | `game(vb)` 有 0.25 vb 分辨率；配合 `rt auto` 后 39 fps 的批尾空档消失 | 1 人天 |
| **P0-2b `--fg-rt-mode` 默认 auto** | auto 依赖干净的测速，所以**必须在 P0-2 之后** → 中 → 单开关回 quant | 锁定场景仍用 quant（去掉 ±1 vb 噪声），移动场景用 raw（跟住真实间隔） | 39/35 fps 场景的空档从"每批 3.6 ms"变成均匀分布 | 0.5 人天（含在 P0-2） |
| **P0-3 `in_step`** | 纯读 → 无 → 无需回滚 | 41/35 变成可验收指标 | 报告多一行直方图 | 0.5 人天 |
| **P0-4 丢帧分类** | 纯计数 → 无 | 三种丢帧（lock / slot / repeat）分开 | OSD/报告可区分 | 0.5 人天 |
| **P0-5 XeLL 契约** | 顺序改动可能改变 XeFG 呈现 → 低 → 一行回退 | A 卡上大概率仍是 0，但能把"不支持"写死；若 cross-vendor 生效则延迟下降 | `xell=` 非 0，或日志明确 `No sleep algorithm available.` | 0.5 人天 |
| **P0-6 标记守卫** | 低 | 防未来（`--pace`/重复帧）污染 XeLL 统计 | 默认无（`--pace` 关） | 0.5 人天 |
| **P1-1 Bresenham** | 图案抖动（f 快速变化时）→ 中 → `acc` 只在慢 EMA 变化 >5% 时重算 | 43 fps 这类"非整除"输入可以真正锁到 40（当前直接放弃） | 默认无（默认不锁）；开 `--lock` 时可见 | 1 人天 |
| **P1-2 时隙丢弃** | **误丢真实帧**（WGC 抖动）→ 中高；但**在 P0-2 之后用游戏时钟判断，风险显著下降** → 单开关 `--no-slot-gate` | 超供时输出恰好 R、延迟有界、`in_step` = `{m, 2m}` | **42 fps：从"DWM 抽 5%"变成"按网格丢 5%"，节奏均匀**；43 fps 同理 | 1 人天 |
| **P1-3 相位浮动** | `next` 偏离 vblank → 低（限 ±1 vb）→ 开关 | 39/35 fps 的亏损摊成偶发 `m+1` vb，而不是整批定型 | 批尾空档消失，变成偶发一次 `m+1` | 0.5 人天 |
| **P1-4 `--lock-auto` 用 p5** | 锁值更低（更保守）→ 低 | 不再选到"一半时间跑不进去"的目标 | 默认关，无可比 | 0.5 人天 |
| **P2-1 MV 策略** | 自动光流要 `MGPU_FG_OF=1` 重编译，且 4K 下光流 2–3 ms 会吃掉预算 → 中高 → 只做警告 | 3x/4x 的闪烁改善 | 默认只有"警告 + 建议 `--no-mv`"，不自动切 | 1–2 人天 |
| **P2-2 高分辨率定时器** | 低 | `--lock`/`--pace` 过冲减少 | 默认无（都关） | 0.5 人天 |
| **P2-3 观测口径** | 无 | 报告不再把量化值与真值混在一起 | 报告两列并列 | 0.5 人天 |
| **P2-4 `--pace` 拒绝** | 低 | 防止与 XeFG 叠加串行 | 默认无 | 0.5 人天 |

（粗估合计 ≈ 7–9 人天，不含真机 A/B 时间。）

### 11.3 按建议顺序，每个阶段的画面变化

| 场景（120 Hz，3x） | 默认 | S1：P0 全做（≈3.5 人天） | S2：+P1-2 时隙丢弃（+1） | S3：+P1-1/3/4（+2） |
|---|---|---|---|---|
| **42 fps**（超供 5%） | 输出 126 → **DWM 抽 5%**，位置随机 | 同左，但日志能测出 42/23.8 ms | **输出 120**，按网格丢 5%，`in_step` = {3, 5.7}（≈95%/5%） | 同左 |
| **43 fps**（超供 7.5%） | 输出 129 → DWM 抽 7.5% | 同左，可测 | **输出 120**，网格丢 7%，`in_step` = {2.8, 5.6} | 同左 |
| **39 fps**（欠供 2.5%） | 批尾 ~3.6 ms 空档（quant 恒 25 ms） | `rt auto` 用真实 25.6–28.6 → **空档摊成 1,1,2** | 同左 | 同左（+相位浮动，更稳） |
| **35 fps**（欠供 12.5%） | 12.5% 空，**集中在每批尾部** | 输出 105，间距变均匀 | 同左 | 亏损摊成偶发一次 `m+1` |
| **60 档 + `--lock 60`** | 58.8（17 ms + 15.6 ms 时钟） | **60.0** | 60.0 | 60.0 |
| **3x/4x + 游戏 MV** | 已知闪烁（§5.17） | 同左 | 同左 | 同左 → S4 起给警告 |

> 关键点：**S1 不改一条输出帧**，它只让 `frameRenderTime` 和测速变准——但这一条就能让
> 39/35 fps 场景的"批尾小停顿"消失，因为 XeFG 不再按 25 ms 去摊一个 28.6 ms 的批次。
> S2 才开始改输出帧数（把超供收进面板）。

### 11.4 如果只做一件事

1. **P0-2 + P0-2b（换测速源 + `rt auto`）**：唯一在默认路径上"改判定、立刻见效"的项，
   而且是 P1-2 / P1-3 的前置（它们都依赖"这张帧属于哪个时隙"的判断）。
2. **P1-2（时隙丢弃）**：唯一能让"输出 ≤ R"从"靠 DWM 抽"变成"我们按网格丢"的项，
   画面变化最直接——但建议在 P0-2 之后做，否则判据还是 WGC 的量化时间。
3. **P0-1（换时基）**：只影响手动 `--lock`，但改动只有十几行，顺手做掉。

### 11.5 不建议先做

| 项 | 原因 |
|---|---|
| P2-1 自动光流 | 需要 `MGPU_FG_OF=1` 重编译；`160_main.inc:584-590` 记录 4K 下光流 2–3 ms 会吃掉 25 ms 预算。先做"警告 + 建议 `--no-mv`"即可 |
| P1-1 Bresenham | 默认不锁帧，没有用武之地；等 `--lock` 真正被用起来再说 |
| P0-5 XeLL 顺序 | 便宜，但 A 卡大概率仍为 0；把它当"把结论写死"的一次性动作，不要期待收益 |
| P2-2 高分辨率定时器 | 只有 `--pace` / `--lock` 用；这两个默认都关 |

### 11.6 为什么这些改动不会破坏 lock_frame 的定位

| 不变量 | 是否被改动 |
|---|---|
| 颜色走 WGC（带宽收益的来源） | ❌ 不动 |
| marker + `seq12/ms12` 精确配对，失败即 1X | ❌ 不动 |
| `--upsample`（MV/depth 150% → 联合重建） | ❌ 不动 |
| prefetch / pub-fifo 强制关闭（不猜槽） | ❌ 不动 |
| 倍率固定（2x/3x/4x），不做动态倍率 | ❌ 不动 |
| 输入锁仍由游戏侧负责（`--lock` 只是可选的兜底） | ⚠️ 只有 P1-1/P1-2 在 `--lock` 打开时才介入，默认全关 |

也就是说：**S1/S2/S3 全部是"在既有路径上加判定/加观测"，没有一条改变数据流。**
这是把改动控制在"可 A/B、可回旧行为"的前提。

---

## 12. v29 本次落地：实现清单与验证

只落了"默认路径可见"的项。**没有**改颜色路径、配对、upsample、prefetch，也没有引入定时器
（lock_frame 仍是"到达驱动 + 判定丢帧"）。

### 12.1 改动清单

| 项 | 文件:行 | 行为 |
|---|---|---|
| 游戏时钟测速 | `020_state_overlay.inc:339-363`（状态）、`140_frame.inc:170-218`（采样） | 每个颜色帧用 `frames[].frame_qpc_ms` 求间隔（1 ms）；输入 EMA 用**所有**帧，`frameRenderTime` 用**被接受**帧 |
| 抽帧门（时隙额度） | `140_frame.inc:224-242`（决策）、`:453-476`（调用） | 漏桶 `credit += target/f_in`，不足 1 就丢；丢在**颜色拷贝之前**，并把该槽 ACK 掉 |
| `frameRenderTime` 换源 | `040_cadence.inc:506-534`（raw/quant）、`:655-681`（ema/median/auto） | 四个模式全部优先用游戏时钟；WGC 只作启动/无发布时的回退 |
| 默认模式 auto | `040_cadence.inc:24-28`、`:136-158` | 供得上（`f_in ≥ target×0.98`）→ quant（贴 `m·vb`）；供不上 → raw（摊开真实间隔） |
| `in_step` / 丢帧观测 | `140_frame.inc:1559-1584` | 每窗口一行：`game clock: in … target … kept/dropped …` + `in(vb)` 0.5-vb 直方图 |
| OSD | `070_osd.inc:64-72`、`:106` | `in fps`/`game ms` 改游戏时钟；`drop` 计入 `g_slot_drops` |
| CLI 与启动日志 | `160_main.inc:337-342`、`:380-390`、`:760-765` | `--no-slot-gate` / `--slot-gate`；启动打印 gate 状态与 `rt` 来源 |

开关矩阵（默认值即"新行为"）：

| 条件 | 抽帧门 | 说明 |
|---|---|---|
| `--mode 2x/3x/4x`（默认） | **开** | 目标 = `refresh/(interp+1)` |
| `--lock` / `--lock-auto` | 关 | 已有显式限帧器，避免双重限帧 |
| `--pace` | 关 | 同上 |
| `--interp-cadence` / `--interp-auto` / `--dyn-mfg` | 关 | 它们自己按帧选 k，门会抢 |
| `--no-fg` / `--fg warp` | 关 | 没有生成帧可吸收丢弃 |
| `--no-slot-gate` | 关 | A/B 用，回到 v28 行为 |

### 12.2 日志怎么看

启动：
```
[lock] --fg-rt-mode auto (default): frameRenderTime = raw while the rate moves, quant while it is locked (source: the game's own present clock, frames[].frame_qpc_ms)
[lock] locked cadence: interp=2 (3x per input), report every 40 frames; slot gate: on (drops the surplus on the game clock)
```
每窗口（新增两行）：
```
[lock]   game clock: in 41.9 fps (23.87 ms) target 40.0 fps OVER -> kept=40 dropped=1 (2.4%) gate=on rate=moving
[lock]   in(vb): 2.9=39 5.7=1  (accepted-to-accepted, 0.5-vblank buckets)
```
- `in` 是**游戏时钟**测的输入率；`target` = 刷新 / (interp+1)；
- `OVER` 表示输入高于目标（门在工作）；
- `in(vb)` 出现 `2.9` 与 `5.7` 两个桶 = 每 21 帧丢 1 帧（3 vb 与 2×3 vb）；
- 若 `gate=off` 而输入 `OVER`，说明被 `--lock`/`--pace`/cadence 接管（预期）。

### 12.3 验证步骤（每组 2 分钟，同场景）

| # | 场景 | 命令 | 通过标准 |
|---|---|---|---|
| V1 | 锁 42 + 3x | 默认 | `kept ≈ 40`、`dropped ≈ 4.7%`、`in(vb)` = {3.0, 5.7}、`presents/vblanks ≤ 1.001` |
| V2 | 锁 39 + 3x | 默认 | `keeps` 全收、`gate=on` 但 `dropped=0`、`rt = auto:raw`、无"批尾定格" |
| V3 | 锁 35 + 3x | 默认 | `dropped=0`、`out ≈ 105`、`in(vb) ≈ 3.4` |
| V4 | 锁 40 + 3x | 默认 | `dropped=0`、`in(vb) = 3.0` 单桶、`rt = auto:quant` |
| V5 | 同 V1 | `--no-slot-gate` | 回到 v28：`dropped=0`、`in(vb) ≈ 2.9`、由 DWM 抽帧 |
| V6 | 60 档 + `--lock 60` | 默认 | （未落地 P0-1）仍为 58.8，作为已知项 |

### 12.4 门控逻辑的仿真实测（离线，非真机）

把 `slot_gate_drop()` + 接受间隔统计原样抄成独立程序，跑 60 s 理想输入：

| m | 输入 | 保留 | 丢帧率 | 最大连续丢 | 判定 |
|---|---|---|---|---|---|
| 3 | 40 | 40.0 | 0% | 0 | quant |
| 3 | 42 | 40.0 | 4.7% | 1 | quant |
| 3 | 43 | 40.0 | 6.9% | 1 | quant |
| 3 | 70 | 40.0 | 42.8% | 1 | quant |
| 3 | 39 | 39.0 | 0% | 0 | raw |
| 3 | 35 | 35.0 | 0% | 0 | raw |
| 2 | 62 | 60.0 | 3.2% | 1 | quant |
| 4 | 32 | 30.0 | 6.2% | 1 | quant |
| 4 | 60 | 30.0 | 50.0% | 2 | quant |

结论：保留率精确等于 `target`，丢帧**均匀**（50% 丢帧率下最多连续 2 张，已成对分布），
这正是"内容步长不被集中"的前提。

### 12.5 与文档的一处偏差（已修正）

实现时发现：**抽帧后"短批次"的真实间隔 < `m·vb`**（42 fps 下是 23.8 ms），
如果 `auto` 按"真实间隔"喂，XeFG 会把 3 张挤在 23.8 ms 里 → 输出回到超供。
所以 `auto` 的判据不是"速率是否在动"，而是**"供得上还是供不上"**：

```
f_in ≥ target×0.98  → quant（贴 m·vb，落网格）
f_in <  target×0.98  → raw  （摊开真实间隔，避免批尾定格）
```

`fg_rt_mode_name()` 也随之显示真实选择（`auto:quant` / `auto:raw`），不再是 WGC 的旧状态。

### 12.5b 实测修正二（Wuchang，2026-11-19）：不要在抖动输入上喂 raw

三组真机日志（A / B=`--slack-auto` / A）：

| | A1 | B | A2 |
|---|---|---|---|
| 输入（游戏时钟） | 36.8–39.7，均 38.4 | 30.0–39.6，均 38.1 | 36.4–39.6，均 38.6 |
| 门 kept/dropped | 1041 / 0 | 1119 / 0 | 681 / 0 |
| `in(vb)` 3.0 | 47.3% | 45.6% | 42.7% |
| `in(vb)` 2.5+3.5 | 52.2% | 54.0% | 56.6% |
| `in(vb)` ≥5.5 | 0.6% | 0.4% | 0.7% |
| `show` 均/最低 | 115/110 | 115/110 | 116/113 |
| rt 模式 | raw 22 / quant 5 | raw 24 / quant 5 | raw 12 / quant 6 |
| 实际喂的 rt | **17–39 ms** | 16.7–39 ms | 16.7–34 ms |
| slack 计数 | — | 7 held / 6 dropped（**4 真丢**） | — |

三个结论：

1. **默认 `auto` 在抖动输入上选了 `raw`**，把 17–39 ms 原样喂给 XeFG → 每批被要求在
   17 ms 或 39 ms 里摊 3 张，面板只能丢/等 → 观感顿挫，而窗口均值仍是 115。
2. **`--slack-auto` 在观感上更差**，虽然它确实减少了丢弃（29 个窗口共 6 次丢弃 = 0.21/窗，
   A 是 0.4–0.5/窗）。代价是 29 个窗口里 7 次 hold，且仍有 4 帧真内容被丢；B 是唯一出现
   `1.5 vb`（12.5 ms）接受间隔的一组 —— 那是"hold 之后两批背靠背交给 XeFG"的指纹，
   内容年龄在 0/25/50 ms 之间跳。**这条链路不要用 slack。**
3. 输入抖动在**上游**：游戏 present 12.5 ms（80/s），producer 发布只有 ~38/s 且间隔
   19–29 ms（`frame=` 字段同样 19–26 ms），所以短/长交替占了一半以上。

修正：`auto` 改用**中位数**判定，且只有"中位数 > `m·vb × 1.08`"（3x/120 下 < ~37 fps）
才选 `raw`；其余一律 `quant`（贴 3 个 vblank）。这样输出网格不再跟着 17–39 ms 抖。
待验证：`rg 'cadence: rt='` 应几乎全是 `25.0ms (auto:quant)`。

### 12.5c 验证（2026-11-19 00:21，`ab_20261119_002151.txt`）

| 指标 | 修正前（A1/A2） | 修正后 |
|---|---|---|
| `rt` 实际喂的值 | 17–39 ms 乱跳 | **25.0 ms，31 个窗口里 29 个** |
| `auto` 判定 | raw 22 / quant 5 | **quant 27 / raw 4** |
| 门 kept / dropped | 1041/0、681/0 | 1201 / **3**（只有一个窗口，输入短暂冲到 40.1+） |
| `show` 均 / 最低 | 115 / 110 | **117 / 113** |
| `breakdown total` 均 | 11–15 ms | **4–6 ms**（present 从 6–8 ms 降到 2.9–4.8） |
| `in(vb)` 3.0 | 47% / 43% | 45%（**输入抖动未变，属上游**） |
| 配对 / xNmiss | 0 / 0 | 0 / 0 |

结论：**输出侧已经稳定在 3 vb 网格**，`total` 反而更低（XeFG 不再被要求把 3 张塞进
17 ms，present 的阻塞时间随之下降）。剩下 54% 的 2.5/3.5 vb 全部来自输入自身
（`jitter=16.7..41.7ms`、`arr_gaps=1–2`/窗）。

补充修正（同轮）：抽帧门加 2% 死区——`q > 0.98`（即输入高出目标不到 2%）时**不丢**，
并把额度重置为 1。工作点正好在 40 fps 上下时，一次丢弃换来的是一个 2× 内容间隙，
不值得；只有真实超供（≥2%）才启动抽帧。

### 12.5d v30：把 `frameRenderTime` 从"模式"改成"连续量"

场景负载变化（40 → 36 fps 持续若干秒）暴露了 `quant/raw` 二选一的另一个问题：
**模式切换本身就是一次跳变**。v30 把它换成一个单向、限斜率的连续量（`rt_auto_feed_ms`）：

```
target = max(m·vblank, median)      // 永不低于计划：绝不向面板要 <1 vb 的间距（不丢帧）
deadband：target < plan×1.02 时取 plan（39–40 的工作点不抖）
feed 每批最多 +0.5 ms，恢复时立刻降到 target（变小的 feed 永远不会超额）
```

- 40 fps → feed 25.0 ms，1 张/vblank，速度准确；
- 进入 36 fps 场景 → feed 在 ~6 批（0.15 s）内从 25.0 爬到 27.8，**没有跳变**；
- 回到 40 fps → 立刻回 25.0。

### 12.5e v30 验证（2026-11-19 00:47，`ab_20261119_004726.txt`，39 个窗口）

这一轮覆盖到了慢场景（输入 32.8–40.1 fps），是本项目第一次把 `rt` 的三个不变量同时测到：

| 检查 | 结果 |
|---|---|
| `rt ≥ m·vb`（不会请求亚 vblank 间距 → 不丢帧） | **0 次违反** |
| `rt ≤ 输入 + 1 ms`（不会留下超过 1 ms 的洞） | **0 次违反** |
| 抽帧丢弃 | 0（输入最高 40.1，2% 死区挡住） |
| 运行中配对失败 | 0（3 行 miss 全是启动时同一次重锚） |
| `rt` 分布 | 25×13、26×12、27×6、28×4、30×4、31×1（跟着场景中位数） |
| 输出 | 40 fps 场景 120 fps；33 fps 场景 97–107 fps（= 3×输入） |
| 主观（用户确认） | **"整体稍慢但是顺"** |

场景边界的行为：

```
rt=25.0  input=31.7ms  out=94.8   ← 刚进慢场景，feed 还没爬起来（~0.35 s）
rt=28.0  input=28.0ms  out=107.1
rt=30.0  input=30.0ms  out=100.0
rt=31.0  input=31.0ms  out=96.8
rt=27.0  input=28.0ms  out=107.1  ← 场景变快，立即回落
rt=25.0  input=25.0ms  out=120.0
```

结论：**这条链路的节奏优化到此为止**。剩下的"稍慢"是 `输出 = 3 × 真实输入` 的算术结果，
只有 VRR（面板跟随）或提高 GPU 余量（让慢场景也到 40）能消除；`--slack*` 与动态倍率都已实测更差。

### 12.6 未落地（留给下一轮）

| 项 | 原因 |
|---|---|
| P0-1 `--lock` 换 QPC/双精度 | 只影响手动 `--lock`，默认关闭；改动小但需要单独 A/B |
| P0-5 XeLL 顺序 + `INPUT_SAMPLE` | 收益不确定（A 卡大概率仍 0），单独一次实验更清楚 |
| P0-6 标记去重 | 默认无重复帧（`--pace` 关），防未来 |
| P1-1 Bresenham（`--lock` 非整除） | 依赖 P0-1 |
| P1-4 `--lock-auto` 用 p5 | 默认关，等 `--lock` 稳定后再说 |
| P2-* | 见 §11.5 |
