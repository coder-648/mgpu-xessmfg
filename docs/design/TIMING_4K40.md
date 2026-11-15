# 4K / 40Hz 双卡补帧：各环节理论耗时与全流程预算（推算，2026-11-13）

> 本文只做**算账**：把 `PIPELINE_V2.md` 的链路、`PERF_BASELINE.md` 的实测基线
> 和 `FINAL_CONFIG.md` §6.3/§9.13 的 4K 数据放到同一个时间轴上，回答两个问题：
>
> 1. 4K/40Hz 这一档，每个环节理论上各花多少时间；
> 2. 当前机制下，全流程总共要多久（每帧预算 / 端到端延迟各是多少）。
>
> 标注规则沿用 `PERF_BASELINE.md` §3.27：**实测** = 有日志出处；**推算** = 由公式算；
> **未验证** = 还没跑过。本机噪声 2–10 ms，<2 ms 的差不要当结论。

## 0. 口径与假设（先固定，否则数字没有意义）

| 项 | 取值 | 依据 |
|---|---|---|
| 输入 | **40 fps 锁死**，输入间隔 `T_in = 25.0 ms` | `PERF_BASELINE.md` §3.7/§3.8 实测 |
| 面板 | 120 Hz，`vblank = 8.33 ms` | 实测环境 |
| 倍率 | **3x**（`--interp 2 --interp-cadence`，kmax 3；40 fps 建议 `--interp 3`） | §3.17 实测 |
| 流程 | `Mode=consume`：游戏自己的 DLSS-G 被拒，游戏按 1X 原生呈现；fakerFG 只采输入 | `XESS_MFG_PRODUCER.md` §0.1.1 |
| 分辨率 | 4K 渲染 / 4K 输出，`--den 1` | 默认档 |
| 流 | `StreamPercent=100`（生产端 1:1），MV `R16G16` + depth `R32` | `producer.cpp`、`packaging/reshade_pack/fakerFG.ini` |
| 每帧流量 | MV 31.6 + depth 31.6 = **63.3 MB**（40 fps ⇒ 2.53 GB/s） | `XESS_MFG_PRODUCER.md` §0.1 实测 |
| 槽位 | `kMaxSlots = 4`，credit 无往返；发布 22 ns/次 | `mgpu_shared.h`、§0.1.2 实测 |

> 如果“4K40”指的是 40Hz 面板而不是 40 fps 输入，下面的**预算判断会反过来**
> （40Hz 面板上 3x 不能填满，只能 2x）；本文按项目文档里一贯的“40 fps 输入”口径。

## 1. 一帧的串行链（4K / 40 fps / 3x）

```text
t0        游戏开始渲染（含 DLSS-SR）                         25.0 ms  ← 结构项，占满整个预算
t1        fakerFG harvest + publish_record                    <0.01 ms CPU
t1'       producer claim_slot + barrier + 2×CopyTextureRegion  CPU ~0.03 ms
          └─ 跨卡写 63.3 MB（GPU）                            2.5–4.9 ms（与游戏剩余工作重叠）
t2        游戏 Present + Signal(fwd[k])                       ~0.02 ms CPU（不产生往返）
t3        WGC 交付这一帧颜色（game->wgc / age）                8–12 ms（DWM hop，~1 vblank+）
t4        bridge 配对槽位 + colour hop                         CPU 0.2–0.4 ms / 本地 165 MB
t5        copy_inputs_local（跨卡读 63.3 MB → 本地 ring）      3.6 ms（实测增量），理论 4.9–6.9 ms
t6        XeFG tag + SetPresentId                             CPU 0.1–0.3 ms
t7        XeFG 生成 3 帧（4K）                                ~16–17 ms（5–20 ms 区间）
t8        Present（被 XeFG pacer 按 vblank 放行）              4.8 ms（4K 超额档实测）
t9        scanout                                             0…8.33 ms
```

时间轴（稳态、每 25 ms 一个输入帧）：

```text
主卡   |-- 游戏渲染 25.0 ------------------------------------------|
       |  copy 2.5–4.9（排在游戏队列里，和渲染抢执行窗口）        |
       └─ Present ──────────────► fwd Signal
                    │ WGC / DWM 8–12 ms
副卡                └─ throttled GPU 20.6 ms ──────────► Present 4.8 ─► vblank
                     (colour 0.3 + copy 3.6 + XeFG 16.6)
```

## 2. 主卡（渲染卡）各环节

| 环节 | 理论耗时 | 性质 | 依据 |
|---|---|---|---|
| 游戏渲染 + DLSS-SR（1 帧） | **25.0 ms** | 结构项，预算本身；consume 下实测不锁帧 21.6 ms/帧（≈46 fps） | `XESS_MFG_PRODUCER.md` §0.1.1 实测 |
| harvest + `publish_record()` | **≤0.01 ms** | CPU；560 B 快照 ×2 + 一次间接调用 = 22 ns（微基准） | §0.1.2 实测 |
| `claim_slot()` + credit 检查 | **<0.01 ms** | CPU；读 4 个 `consumed[]`（系统内存） | `producer.cpp` `update_credit()` |
| barrier + 2×`CopyTextureRegion` 录制 | **0.02–0.05 ms** | CPU，命令表本来就在游戏手里 | 代码路径 |
| 跨卡写 MV+depth | **2.5–4.9 ms**（63.3 MB @ 13–25 GB/s） | GPU，可与游戏剩余工作重叠 | `XESS_MFG_PRODUCER.md` §0.1.3 实测/推算 |
| Present + `Signal(fwd)` | **~0.02 ms** | CPU；消费端主路径**不等** fwd | `PIPELINE_V2.md` §3 |
| **主卡合计（GPU 侧增量）** | **2.5–4.9 ms** | 相对 25 ms 预算 = 10–20% | — |

关键点：consume 下游戏 4K 原生实测 21.6 ms/帧，**加上 2.5–4.9 ms 的跨卡拷贝后是
24.1–26.5 ms**，正好压在 25.0 ms 上。这就是 `PERF_BASELINE.md` §3.17 里 40 fps 的
`arr(vb)` 不是稳定的 `3x`、而是 `2x=13 3x=14 4x=13` 的机制来源之一：**拷贝让主卡偶发超预算，
输入帧晚一个 vblank**。把 `StreamPercent` 降到 150/200 后拷贝只剩 1.2–2.2 / 0.7–1.2 ms，
这一档才会真正稳。

## 3. 副卡（生成卡）各环节

| 环节 | 理论耗时 | 性质 | 依据 |
|---|---|---|---|
| WGC 交付（`game->wgc` / `age`） | **8–12 ms**（p50 11.8 @4K、9.2 @1440p） | 结构项：DWM 合成后才交帧，≈1 个 vblank 以上 | `FINAL_CONFIG.md` §6.3.2 实测 |
| `pair_slot_for_wgc` 配对 | **<0.01 ms** | CPU；时间戳窗口 dt ∈ [-2, 250] ms，不引入等待 | `bridge.cpp` |
| `wait_slot()`（fwd fence） | **0**（默认关） | 主路径不等；只有 `--prefetch-inputs`/WARP 才等 | `PIPELINE_V2.md` §3 |
| colour hop（WGC→D3D11→D3D12） | **0.2–0.4 ms**（CPU）/ 本地 165 MB | 4K FP16：66 读 + 66 写 + 33 写，本地带宽量级 | `PERF_BASELINE.md` §3.5 实测 |
| `copy_inputs_local`（跨卡读 63.3 MB） | **3.6 ms**（实测增量）；理论 4.9 ms @13 GB/s（6.9 @9.19） | GPU；与 XeFG 工作部分重叠 | `PERF_BASELINE.md` §3.3 实测 |
| XeFG tag + `SetPresentId` | **0.1–0.3 ms** | CPU | `g_ms_fg` 字段 |
| XeFG 生成 3 帧（4K） | **5–20 ms**（本机反推 ≈16–17 ms） | 闭源、不可优化；其中“喂真 MV vs 喂零 MV”差 **7.4 ms** | `PERF_BASELINE.md` §2、`FINAL_CONFIG.md` §2.1.2 实测 |
| `gpu_throttle()`（队列深度 1） | **= 上一帧 GPU 时间 ⇒ 20.6 ms p50**（p99 31.8） | 这不是额外开销，是串行化的边界；GPU 受限时它就等于整帧 GPU 成本 | `FINAL_CONFIG.md` §9.13 实测 |
| Present（XeFG pacer 放行） | **4.8 ms**（4K 超额档）/ 7.3 ms（1080p 档） | vblank 节拍等待，不是浪费 | §9.13 / §3.17 实测 |
| scanout | **0…8.33 ms** | 0…1 个刷新周期 | LATENCY 行定义 |
| **副卡合计（p50）** | **`gpu` 20.6 + `present` 4.8 = 25.4 ms**（两项此消彼长，不是串行和） | 见 §4 的预算判断 | 实测 |

> `age` 的 8–12 ms 是**外推值**：4K 实测 p50 是 11.8 ms @33.3 ms 输入、9.2 ms @1440p，
> 按“约 1 个 DWM hop”推到 25 ms 输入；合成器积压时会更大（`PIPELINE_V2.md` §2 在 21 fps 下
> 测到 `game->wgc` 35.9 ms），所以 4.3 的延迟区间要按 8–15 ms 留余量。

`gpu` 20.6 ms 的构成（反推，与 `--no-inputs` / `--no-mv` / `--no-depth` 三组实测一致）：

```text
colour 0.3  +  跨卡读 3.6  +  XeFG 3×4K(含真 MV) 16.6  ≈  20.5 ms   ✓
```

其中 XeFG 那 16.6 ms 不是常数：同一拷贝、同一 pass，把 MV 换成零矢量会掉 7.4 ms
（`FINAL_CONFIG.md` §2.1.2 的 A/B/C 三组），说明**真实 MV 的数据相关性是 4K 3x 的主要成本之一**。

## 4. 当前机制下“全流程”的三种总时间

“全流程总时间”在这套链路里有三个不同口径，混用会得出互相矛盾的结论，必须分开看。

### 4.1 单个输入帧的处理墙钟（吞吐口径）

稳态下主卡每 25.0 ms 出一个输入帧，副卡每个输入帧跑一次 `run_frame_ex`：

```text
副卡 total（实测 p50）                        = 25.4 ms
  ├─ gpu_throttle（等上一帧 GPU）             = 20.6 ms
  ├─ Present（XeFG pacer 放行）               =  4.8 ms
  └─ CPU 可归因（work+colour+fg+inputs 录制） =  1.0–1.3 ms
```

> 注意这三项**不是简单相加**：GPU 受限时 `gpu` 高、`present` 低，两者此消彼长；
> 20.6 + 4.8 恰好等于 25.4 是 p50 层面的巧合，不是串行和。判断是否超预算用
> `gpu + present`（`load` 的定义），不要用 `total` 去反推。

- 25.4 ms **略大于** 25.0 ms ⇒ `load = 101.6%`，判据“`gpu + present > T_in`”成立；
- 一帧不丢时的上限是 `3 帧 / 25.4 ms ≈ 118 fps`；
- 但实测有 **58% 的帧超预算**（`FINAL_CONFIG.md` §9.13），WGC 池吸收不了持续欠账，
  每窗口丢 5.3 个输入帧 ⇒ `show ≈ 106 fps`、`skip 5.3`。
  也就是说：**理论吞吐 ~118 fps，机制实际交付 ~106 fps。**

### 4.2 每输入帧的 GPU 预算（能不能装下）

本机 4K 的可行性判据（`FINAL_CONFIG.md` §9.13）：

```text
需要：gpu_p50 + 1 vblank ≤ T_in
4K/40：20.6 + 8.33 = 28.9 ms  >  25.0 ms      ❌ 超 3.9 ms（约 16%）
```

几何解释：3x 要求 3 个输出帧落在 vblank 0/1/2（=16.7 ms），而 XeFG 的 GPU 工作就要 20.6 ms，
真帧只能滑到 vblank 3（25.0 ms）——**一个 vblank 的余量都没有**，任何抖动都直接变成丢帧。

同一判据下的相邻档（实测/推算）：

| 档位 | gpu_p50 | +vblank | 预算 | 结论 |
|---|---|---|---|---|
| 4K · 3x @40 | 20.6 | 28.9 | 25.0 | ❌ 106 fps，skip 5.3（实测） |
| 1440p · 3x @40 | ≈14.3 | ≈22.6 | 25.0 | ✅ 预计 120 fps（推算，未验证） |
| 1080p · 3x @40 | 5.7 | 14.0 | 25.0 | ✅ `load 51%`、120 fps（实测） |
| 4K · 4x @30 | 23.4 | 31.7 | 33.3 | ✅ 118 fps（实测，当前 4K 最优） |

### 4.3 端到端延迟（latency 口径）

代码里的公式（`bridge.cpp` 的 LATENCY 行）：

```text
compose->present = age + total + slack(0)
compose->photons  = compose->present + [0, vblank]
click->photons    ≈ T_in(≥1 输入帧) + compose->photons
```

代入 4K/40 实测值：

```text
compose->present  = 10 + 25.4            = 35.4 ms
compose->photons  = 35.4 + 0…8.33        = 35.4…43.7 ms   （实测中值 ~39.9）
click->photons    = 25.0 + 35.4 + 0…8.33 = 60.4…68.7 ms   （实测 64.9）
```

这就是“填满”状态的结构式：**`2·T_in + age + [0, vblank]`**。多出来的那一个 `T_in`
不是我们的开销，而是“插值帧必须先于真帧上屏”的几何成本（DLSS-G / FSR-FG 同样有）。

对照：1080p 生成时副卡循环只占 13.0 ms，剩余的 12 ms 变成等 WGC 的空闲，
延迟下移到 `25 + 10 + 13 + [0, 8.33] ≈ 48…56 ms`（推算）；1440p 约为 **55–60 ms**
（`FINAL_CONFIG.md` §6.3.2 的预测是 55–58，未验证）。

## 5. 结论（4K/40Hz 这一档）

1. **4K 原生 3x@40 在当前机制下装不下。** 副卡 `gpu + 1 vblank = 28.9 ms`，预算 25.0 ms，
   缺口约 3.9 ms（16%）；实测表现就是 `total 25.4/25.0`、`show 106`、`skip 5.3`。
   这不是同步或拷贝的锅（拷贝只占 3.6 ms 且与生成重叠），是 **4K 生成算力**。
2. **理论上限 ~118 fps，机制实际交付 ~106 fps。** 差额来自“58% 的帧超预算 + 池只有几帧吸收能力”。
3. **全流程端到端约 65 ms**（compose→photons 约 40 ms），其中：
   - 25.0 ms 是输入间隔（游戏自己的渲染，改它只能降画质/提帧率）；
   - 10 ms 是 DWM/WGC hop（不可控）；
   - 25.4 ms 是副卡生成 + vblank 等拍（4K 3x 的算力，闭源）；
   - 0–8.3 ms 是 scanout。
4. **要在这台机器上拿到 4K 输出 + 120 fps 整拍，只有把“生成”降下来**：
   - `--den 2`（1080p 生成、4K 拉伸）：`gpu 5.7`，`load 51%`，实测 120 fps；代价是插值分辨率；
   - 1440p 生成：预计 `gpu ≈14.3`，正好落在 22.6 ≤ 25，延迟约 55–60 ms（未验证，最值得先测）；
   - 或维持 4K 生成但改 4x@30（`gpu 23.4 + 8.33 = 31.7 ≤ 33.3`，实测 118 fps），
     代价是延迟 +8.3 ms（click→photons 77.4 ms）。
5. **顺手能省的两笔（不改画质档位）**：
   - `StreamPercent=150/200`：主卡拷贝 2.5–4.9 → 1.2–2.2 / 0.7–1.2 ms，把主卡从 25.1 ms 拉回预算内，
     输入帧不再偶发晚一个 vblank（这正是 40 fps 下 `arr(vb)` 抖成 2/3/4 的原因之一）；
   - `--interp 3`（kmax 4，必要时 5）：40 fps 的 3-vblank 标称间隔一旦晚到 4 个 vblank，
     kmax=3 会被截断，空刷新从 1.5% 恶化到 10.8%（§3.17 实测）。

## 6. 误差与复测

- 本机 A/B 噪声 **2–10 ms**（`PERF_BASELINE.md` §3.6），上表里 <2 ms 的差只作方向参考；
- `age` 取 10 ms 是“4K p50 11.8 @33.3 ms 输入”的外推（25 ms 输入约 8–12 ms）；
  DWM/WGC 积压时它会明显变大，4.3 的延迟区间按 8–15 ms 留余量；
- `gpu` 是 `gpu_throttle` 的等待值，等于上一帧 GPU 时间；GPU 不满载时会偏小，
  所以“装不装得下”要看 `gpu + present`，不要只看 `gpu`；
- 复测命令：`--report 40` + `scripts/latstats.py`（看 `compose->photons` / `game->wgc` / `bridge`），
  每档 ≥60 s、A→B→A 至少 3 轮；`load` 长期 >100% 或 `slow` 变多就是这一档装不下。
