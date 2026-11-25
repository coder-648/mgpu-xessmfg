# lock_frame 自身链路审查（2026-11-25，consumer + producer）

审查范围：**只针对 lock_frame 自己**（`lock_frame/modules/*.inc`、`lock_frame/producer/producer.cpp`），
不重复 `CHAIN_AUDIT_2026-11-25_ROUND2.md` 里已处理的 fakerFG 截取端项。

审查方法：按**当前默认配置**逐帧走一遍时序，再对每个等待/同步点、每处资源状态、每处
“跨卡读”做核对；性能项只保留在 `PERF_BASELINE.md` 噪声门槛（本机 2–10 ms，<2 ms 不下结论）
之上仍然成立的。

当前默认（UI `build_cmdline` + 发布版 `fakerFG.ini`）：

```
consumer : --mode Nx --interp N-1 --streams=mv,depth [--hudless-stream --ui-source off]
           g_upsample=ON(强制)  g_prefetch_inputs=false(强制)  g_input_copy_queue=true
producer : MvCodec=auto(→1, 100%)  DepthCodec=auto(→1, 150%)  UiPatch=1  HudlessStream=1
```

## 0. 结论摘要

| # | 级别 | 位置 | 问题 | 后果 |
|---|---|---|---|---|
| A | **P0** | `140_frame.inc:947` / `130_input_cache.inc:437-464` / `060_frame_io.inc:978` | 默认路径**从不等待 producer 的 `fwd[slot]`**（唯一一处等待在 prefetch，已被强制关闭）；`decode_mv_slot()` 注释声称 “wait_slot() 已等待过”，而 `wait_slot()` 只被 `run_frame_warp` 调用 | 可能读到 producer 尚未写完的 MV/depth；一帧鬼影/错位，难以复现 |
| B | **P0** | `producer.cpp:2384-2419` | `claim_slot()` 的 “reclaim oldest” 分支**永远命中**，v23.1 写的 “宁可丢帧也不覆盖最旧槽” 的 skip 分支是**不可达死代码**（2026-11-25 实机日志已确认触发） | 消费者变慢时覆盖它正要读的槽 = 文档里描述过的“画面缩/抖” |
| C | P1 | `080_warp_upsample.inc:448-451,482-483`；`140_frame.inc:1162,1178` | `g_up_valid/g_up_slot` 只在目标重建时清；重建 pass 某帧没跑时，同槽 4 帧后 tag 会拿到**上一轮**的重建结果 | tag 到过期 MV/depth 一帧 |
| D | P1 | `080_warp_upsample.inc:448-451` vs `060_frame_io.inc:1044-1047,1208-1211` | 重建 pass 声明的 `StateBefore = COPY_DEST`，但 codec 解码结束后两份本地纹理都在 `COMMON`（非 codec 的 copy 也是隐式提升，app 侧仍是 COMMON） | 非法 barrier（待 `--d3d-debug` 实测确认）；驱动侧属 UB |
| E | P1(潜在) | `130_input_cache.inc:437` vs `:511` | `prefetch_publications()` 有 `mv_codec_shared()` 守卫，`prefetch_next_inputs()` **没有**；而 `decode_*_slot()` 录的是 `g_list`（帧末已 Close/Execute） | 一旦重新打开 prefetch，会往已关闭的 list 录 Dispatch |
| F | P1 | `160_main.inc:1653-1668` | `--lock` 丢帧路径 `continue` 跳过 `jbuf_give_back()` | `--lock` + `--jbuf` 时纹理只增不减（实验组合） |
| G | P1 | `060_frame_io.inc:1631-1657` vs `140_frame.inc:982,1325` | 旧 clean-HUDless 路径本地纹理按 **MV 槽**（32）分配，共享槽只有 8；patch 路径按 `slot%8` | 4K 下 ~1.0 GB 显存白占（UiPatch=0 时） |
| H | P1 | `producer.cpp:4045-4050` | `want_patch` 硬性要求 `ui_format == R10G10B10A2`，SDR 标题（backbuffer RGBA8）拿不到 patch | SDR 下 UI 合成整体静默关闭 |
| I | P2 | `140_frame.inc:944-946` | `cq` 为真时 `record_ui_unblend()` 整段被跳过（UI copy 在 copy queue 上，顺序无法保证） | `--no-upsample` + `MvCodec=0/DepthCodec=0` 组合下 UI 合成无声失效（默认组合不受影响） |
| J | P2 | `producer.cpp:704-807` + `ensure_shared` | 映射/堆按 32 槽 × (MV+depth) 预留：4K codec 下 ≈ **1.7–2.3 GiB** | 启动慢、提交内存大；协议里 `StreamShared.slots` 已有能力，但 MV/depth 固定 32 |
| K | P2 | `producer.cpp:1912-1921` | 单 copy allocator：每帧在**游戏渲染线程**上等上一帧拷贝退休（<=50 ms） | 启动/抖动窗口内直接卡游戏线程；consumer 侧早已用 8 环 |
| L | P2 | `085_ui_unblend.inc:1812-1821` | 33³ LUT **每帧重建**（1/16 采样 dispatch + 2.3 MB 原子累加 + fill dispatch） | 消费卡稳态每帧多 2 个 dispatch；`load≈90%` 时是实打实的余量 |
| M | P2 | `115_marker.inc:233-239` | `payload != last` 每帧成立（ms12 每帧都变），**每帧一条日志** | 40 行/s 日志噪声；不是 CPU 瓶颈（`logf_` 已按 200 ms 节流） |
| N | P3 | `060:285 wait_slot` / `060:220 g_fwd_expect` / `020:812 g_fwd_last` / `140:337 pair_drain_slot` / `130:506 prefetch_publications` / `060:245` | 死代码 / 注释与实现漂移 | 维护风险（E 就是被“守卫缺失 + 注释过期”养出来的） |
| P | P2 | `160_main.inc:1418-1473` + `140_frame.inc:334-339,792-798` | `--fg-colour shared` 循环用 `run_frame(nullptr)` 驱动，而配对/拷贝/ACK 全在 `colour11 != nullptr` 分支里 | 该 CLI 路径实际**永远 1X**（MV/depth 不拷贝、不 tag）；默认不启用，属失效的实验路径 |
| Q | P1(环境) | 部署布局 | 把 `lock_frame.exe` 放在游戏目录时，它导入的 `dxgi.dll` 解析到 fakerFG，于是 consumer 进程里也有一份 fakerFG（第二个 XeFG context + XeLL + Present 钩子） | 2026-11-25 实测确认；发布包布局（工具独立目录）不会发生。A/B 基线必须用发布布局 |
| R | P2 | `140:556-562` + `producer.cpp:3979-3990` | consumer 用 `TerminateProcess` 退出，从不清理 `request_mask`；producer 只按掩码决定拷贝，`want==0` 又回退到 MV+depth | consumer 关掉后游戏侧仍每帧编码/拷贝两路流，直到游戏重启 |

### 实施状态（2026-11-25，同一轮）

已落地并编译通过（`./lock_frame/build.sh consumer`，仅既有 `wcsicmp` C4996 警告）：

| 项 | 落地 |
|---|---|
| **A** | `130_input_cache.inc` 新增 `order_publication_read(slot, copy_queue)`；`140_frame.inc` 在 `copy_inputs_local()` 之前按 `cq` 选择队列，把 `fwd[slot]` 以 `frames[slot].published` 排进队列。 |
| **C** | 新增 `g_up_pub`（060）；`record_upsample()` 成功时写入 publication，`ensure_upsample_targets()` / `release_shared_views()` 清零；`140_frame.inc` 的 tag 条件改为 `up_pair_ok = g_up_valid && g_up_slot == slot && g_up_pub == frames[slot].published`。reuse 帧 publication 不变，仍命中。 |
| **D** | `record_upsample()` 的两处 `StateBefore` 改为 `COMMON`。 |
| **E** | `prefetch_usable()` 在 `mv_codec_shared() \|\| depth_codec_shared()` 时返回 false（锁帧版本已强制关闭 prefetch，无行为变化）。 |
| **F** | `160_main.inc` 的 `--lock` 丢帧分支补 `jbuf_give_back()`。 |

仍未动（需要单独 A/B）：**B**（`claim_slot` 的 reclaim 语义，会改变丢帧量）、**G/H/I**
（显存与 SDR/UI 组合边界）、**J**（`TransportSlots`）、**K**（producer copy ring）、
**L**（LUT 间隔）、**M**（marker 日志量）。

> 构建产物 `build/lock_frame.exe` 的 md5 与 1.3.1 的已验证包不同；在 CP2077 / PBZ 跑完
> 回归（`pair: exact`、`xefg-in:` 尺寸、`load`、`slow`、`HITCH`）之前不要替换发布包。

---

## 1. 默认路径时序（作为后面每条的证据基线）

```
main loop
 ├ prefetch_publications()            : no-op（160:935 强制 g_pf_pub=false）
 ├ TryGetNextFrame / note_arrival     : WGC + SystemRelativeTime
 └ run_frame_ex(colour)
    1 service_shared_layout()         : 每帧读 input_flags + 3 个整型（廉价）
    2 collect_retired_inputs / probe_colour_black / lock_overlay_stall_gate
    3 lock_pair_colour()              : 4×CopySubresourceRegion + Map（阻塞） + ≤32 槽扫描
    4 slot_gate_drop()                : 游戏时钟漏桶
    5 gpu_throttle()                  : 等上一帧 fence（队列深度 1）
    6 ack_previous_slot / xellSleep
    7 D3D11 CopyResource(colour) + Signal + g_queue->Wait(fence11)
    8 g_list Reset + convert draw（PQ + 4 角 marker 修复）
    9 copy_inputs_local(slot)         : MV codec decode / depth codec decode（都在 g_list）
   10 record_upsample(slot)           : 1 个全屏 dispatch（target = 呈现尺寸）
   11 record_ui_unblend()（patch）    : LUT3D build(2 dispatch) + unblend dispatch
   12 tag MV / depth / HUDless + constants + present id + BACKBUFFER
   13 Close / Execute / Present / Signal
```

关键观察：**第 9 步读 producer 的共享槽时没有任何 `fwd` 等待**，这是下面 A 的全部依据；
第 9-11 步是消费卡上真正的 GPU 负载，D 与 L 都落在这里。

---

## 2. P0

### A. 默认路径不等待 producer 的发布 fence（跨卡读竞争）

证据：

- `130_input_cache.inc:452-454`（prefetch 路径）：
  ```cpp
  // GPU-side wait for the producer's publication of that slot - the only place in
  // the main path where our reads are ordered against the producer at all.
  g_copy_queue->Wait(g_fwd[next_slot].Get(), published);
  ```
  代码自己承认“唯一一处”。而 `160_main.inc:933-935` 把 `g_prefetch_inputs/g_pf_pub`
  **无条件置 false**，所以这“唯一一处”在 lock_frame 里永远不执行。
- `140_frame.inc:947-957`：配对成功后直接 `copy_inputs_local(slot)` / `decode_*_slot()`，
  没有任何 `g_fwd` 等待。
- `060_frame_io.inc:978-980`（`decode_mv_slot` 的前置条件注释）：
  “`wait_slot()` has already blocked on the slot's forward fence” —— 事实不成立：
  `wait_slot()` 只被 `100_convert_warp.inc:104`（`--fg-warp` 实验路径）调用。

为什么现在“看起来正常”：WGC 颜色比游戏 present 晚 1 帧左右（`age` 7–12 ms+），
producer 的异步拷贝通常早已完成。但这是**时序巧合**，不是同步保证；游戏帧变快、
producer copy 队列拥堵、或 resume/stall 之后会露出来，表现为单帧 MV/depth 是上一个
publication 的内容（边缘鬼影/轻微错位），无法从日志定位。

建议（零行为变化，只是补同步）：配对成功、记录拷贝/解码之前，把发布值排进同一条队列：

```cpp
// 140_frame.inc，pair 成功分支之后、copy_inputs_local 之前
static void order_against_publication(UINT slot) {
    if (!g_ctl || slot >= mgpu::kMaxSlots || !g_fwd[slot]) { return; }
    const uint64_t pub = g_ctl->frames[slot].published;
    if (pub == 0) { return; }
    // 判据与 140:945 的 cq 完全一致：copies 在哪条队列上，就在哪条队列上等
    if (copy_batch_uses_copy_queue()) { g_copy_queue->Wait(g_fwd[slot].Get(), pub); }
    else                              { g_queue->Wait(g_fwd[slot].Get(), pub); }
}
```

- `g_queue->Wait()` 是队列级等待（不阻塞 CPU），排在 `g_list` 的 Execute 之前即可生效；
  `g_fwd` 是 `SHARED_CROSS_ADAPTER`，队列 Wait 合法（prefetch 已在用）。
- 验证：`--d3d-debug` 无新增 validation；`pair: exact` 与 `load` 不变；`bus:` 不变。
  真机 A/B 看“偶发单帧鬼影”是否消失（这种问题只能靠长跑）。
- **新增的契约依赖**：这条等待让 consumer 的队列依赖 producer 一定 signal `fwd[slot]`。
  协议 v5 的设计就是 “metadata first, fence last”，所以这是既定契约；但如果 producer 打出
  `publish: Signal FAILED`（队列被移除/设备丢失），consumer 现在会等一个永不到来的 fence，
  表现为画面冻住而不是读到旧数据。出现那行日志时应直接当作 producer 端致命错误处理。

### B. `claim_slot()` 的“覆盖最旧槽”分支不可达，v23.1 的修复没生效

`producer.cpp:2369-2420`：

```cpp
for (i...) { p = g_fwd_value[i]; if (p == 0) { oldest = i; break; } if (p < oldest_pub) {...} }
if (oldest >= 0) { ... reclaim ...; return true; }       // ← 永远为真
// v23.1: 覆盖最旧槽可能正是消费者刚挑中的槽 => 丢帧更好
++c->producer_skipped; ... return false;                  // ← 不可达
```

`kMaxSlots > 0` 时 `oldest` 必然 >= 0（要么找到 p==0，要么找到最小值），所以
v23.1 注释描述的 bug（“画面 zooms/shifts every few frames”）在“无 ACK”场景下**仍然存在**。
`update_credit()` 的 30 帧宽限只覆盖“消费者完全死了”，覆盖不了“消费者只是慢了两帧”。

建议：

1. 先确认现场：日志里 `grep "reclaimed oldest slot"` 和
   `grep "no ACKed slot - publication skipped"`，后者应该永远不出现（若出现说明还有另一条路径）。
   **已确认（2026-11-25 20:4x 的 CP2077 producer 日志）**：一次正常会话里出现了
   `no ACKed slot - reclaimed oldest slot 4/5/6 (pub=24, 3 so far)` 和 `slot 13/14 (pub=11)`，
   都是 consumer 刚连上、`consumed[]` 还没被 ACK 的那 32 个 publication 之内 ——
   即启动窗口里 producer 就在覆盖“消费者接下来要配对”的槽。
2. 让 reclaim 变成**有界兜底**：连续 N（如 8）次 claim 失败才允许覆盖最旧槽，
   且绝不覆盖 `frames[i].frame_id >= colour_seq` 的槽；其余情况走 skip（消费者会
   重锚 + 补发，代价是一帧重复，可接受）。
3. 同一段代码里 `c->producer_skipped` 在 reclaim 分支也 +1，日志会误导“被跳过”，
   建议分开计数。

---

## 3. P1

### C. `g_up_valid` 跨帧残留 → tag 可能用同槽上一轮的重建结果

`080:482-483` 只在重建成功时 `g_up_slot = slot; g_up_valid = true;`；
`140:1162` / `140:1178` 只校验 `g_up_slot == slot`：

```cpp
if (g_upsample && g_up_valid && g_up_slot == slot && g_up_mv) { ...tag... }
```

槽是 4 个左右轮转，`record_upsample()` 在“目标已够大 / 源比目标大 / `--depth-lag>0` /
`g_pf_pub` 等”条件下会提前 return（`080:381-428`）。某一帧没跑、而同槽号在 4 帧后再次出现，
就会 tag 到 **4 帧前**的 MV/depth。默认配置下深度流比目标小，pass 每帧都跑，所以没暴露；
一旦用户改 `StreamPercent` / 分辨率 / `--no-upsample` 组合，就容易踩到。

建议：把“本帧生成”做成显式局部量，而不是跨帧全局：

```cpp
// run_frame_ex：在调用 record_upsample 之前
g_up_valid = false;                          // 每帧先失效
... if (g_upsample) { record_upsample(slot); }   // 成功时内部再置 true
// 更稳：key 用 (slot, frames[slot].published)，publication 变了就失效
```

### D. 重建 pass 的 `StateBefore` 与 codec 解码结束状态不一致

- `decode_mv_slot()` 结束：`dst: UAV -> COMMON`（`060:1044-1047`）；
  `decode_depth_slot()` 同样（`060:1208-1211`）。
- `record_upsample()` 却按 `COPY_DEST -> NON_PIXEL_SRV` 转换（`080:448-451`）。

非 codec 路径也不对：`copy_content()` 只是 `CopyTextureRegion`，资源是
`ensure_local_copy()` 用 `COMMON` 建的（`060:581-583`）；D3D12 的隐式提升不会改变
app 侧记录的状态，正确写法同样是 `COMMON -> NON_PIXEL_SRV`。

现在“能跑”是因为 COMMON 本身允许 SRV 读、且驱动对错误 `StateBefore` 往往容忍；但这是
非法用法，debug layer 通常会直接报（未实测）。建议：

1. 立即用 `--d3d-debug` 跑一次默认配置，`grep -i "statebefore\|STATE_TRANSITION"` 验证；
2. 把 `record_upsample` 的 before 状态改成 `COMMON`（两处），或给
   `g_mv_local/g_depth_ring` 维护一个状态表（推荐前者：代码里所有写入都以 COMMON 收尾）。

### E. `prefetch_next_inputs()` 缺 codec 守卫（潜在，重新开启 prefetch 时会踩）

`130:509-511`（`prefetch_publications`）有 `if (mv_codec_shared()) return;`，
`130:437-464`（`prefetch_next_inputs`）没有。而帧内解码
（`060:986` `ID3D12GraphicsCommandList* list = g_list.Get();`）录的是**当前帧**的 list，
prefetch 是在 `140:1919`（Present + Signal 之后）调用的，此时 `g_list` 已 Close 并 Execute。

顺带一个更隐蔽的后果：codec 流不进 `g_copy_count`，所以 prefetch 里
`recorded = (g_copy_count != before)` 为 false → `g_pf_valid` 不会置位 → 帧内解码仍会发生，
所以**目前只是每帧发出非法调用**，画面不一定坏；但若 MV/depth 一个是 codec、一个是纹理，
`recorded` 为 true → 帧内 copy 被跳过，MV 就真的停在旧内容上。

建议：`prefetch_usable()` 直接加
`if (mv_codec_shared() || depth_codec_shared()) { return false; }`，
并在 `160:934` 的强制关闭处写清“codec 会话不支持 prefetch”，或彻底删掉 prefetch 分支。

### F. `--lock` 丢帧路径泄漏 jbuf 纹理

`160_main.inc:1653-1663`：

```cpp
if (now + 1 < g_lock_next) { ++g_lock_drops; continue; }   // 没有 jbuf_give_back(texture)
...
jbuf_give_back(texture);                                   // 1668 才回收
```

`--jbuf > 0` 时每丢一帧就丢一张 4K FP16 纹理（`jbuf_take` 池里取不到就新建）。
只在 `--lock` + `--jbuf`（CLI 实验组合）出现，但属于确定性泄漏。
修法：`continue` 之前 `if (g_jbuf > 0) { jbuf_give_back(texture); }`。

### G. 旧 clean-HUDless 路径的本地纹理按 MV 槽分配（显存）

`060:1652-1657`：

```cpp
ensure_local_copy(g_hudless_local[slot], g_hudless[hslot].Get(), ...);
g_hudless_fresh[slot] = true;
```

共享 HUDless 只有 `HudlessSlots=8` 个槽（`hslot = slot % 8`），本地却按 32 个 MV 槽
各建一张全分辨率 R10（4K ≈ 33 MB/张 ⇒ 最坏 ~1.0 GB）。patch 路径（`060:1640-1644`）
已经正确使用 `hslot`；`140:982` 的读取也用 `[slot]`，两边“碰巧一致”，但很脆。

建议：clean 路径统一改用 `hslot`（读写一起改），4K 下上限 8×33 MB = 265 MB，
与 UI 本地环一致。

### H. SDR 标题被 `want_patch` 的 R10 条件挡掉

`producer.cpp:4045-4050`：

```cpp
const bool want_patch = g_ui_patch && ... &&
    g_ui_format == (uint32_t)DXGI_FORMAT_R10G10B10A2_UNORM && ...;
```

`UiPatch=1` 会强制 `g_ui_alpha8=false`，于是 SDR（backbuffer RGBA8）走
`ui_transport_format(RGBA8) -> RGBA8`，`want_patch` 恒 false，只留一条通用的
“ui-patch: synth not dispatched” 一次性日志。消费端 convert 的输出链一直是 R10，
patch 走 R10 传输没有硬性障碍。

建议：二选一并写进注释/文档 ——
(a) patch 模式强制 `g_ui_format = R10`（SDR 也 PQ 编码），或
(b) 允许 RGBA8 patch（`kPatchLut3D` 与 `ui_occ` 用码值域，不依赖 PQ），并补一条明确日志。

### I. `cq` 为真时 UI 合成静默失效（组合边界）

`140:944-946`：

```cpp
const bool cq = (pf_hit || g_of_miss || g_upsample || !pair_ok || mv_codec_shared())
                    ? false : input_copy_begin();
...
} else if (pair_ok && !g_of_miss && !g_no_inputs && !cq) {
    hudless_res = record_ui_unblend(...);     // cq 为真时整段不跑
}
```

`cq` 为真 = 输入拷贝在 COPY 队列上，`record_ui_unblend()` 需要 `present` 与 UI 本地纹理
在同一条 list 里过渡，所以作者选择跳过。默认组合（upsample=ON 或 codec=ON）时 `cq=false`，
不受影响；但 `--no-upsample` + `MvCodec=0/DepthCodec=0` 时 UI 合成会无声关闭，
日志里只有 `Hudless` 相关的一次性提示。建议至少加一条“UI 合成因 input-copy-queue 关闭”
的一次性日志，或在 cq 路径下强制把 UI 拷贝拉回帧内 list。

---

## 4. P2 性能

### J. 共享映射/堆按 32 槽预留：4K codec 下 ≈ 1.7–2.3 GiB

`layout_streams()` 给 MV/depth 固定 `slots = kMaxSlots(32)`（`producer.cpp:801-805`），
codec 槽按瓦片字节计：`(w/32)*(h/8)*1056 B`
（4K = 120×270 = 32400 瓦片 ⇒ 34.2 MB ≈ **32.6 MiB/槽**）。

| 流 | 槽数 | 单槽（4K 内容） | 合计 |
|---|---|---|---|
| MV (codec 100%) | 32 | 32.6 MiB | 1.02 GiB |
| depth (codec 150%) | 32 | ~14.5 MiB | 0.45 GiB |
| patch (stream 4, 8 槽) | 8 | 32.6 MiB | 0.25 GiB |
| **合计** | | | **≈1.7 GiB**（MV/depth 都 100% 时 ≈2.3 GiB） |

而 `NEXT_ROUND` 记录的 colour lag 最大 13 个 publication，16 槽已足够；32 槽是 v23 为
WGC 颜色 backlog 留的。协议里 `SharedControl.slots` 与 `StreamShared.slots` 本来就存在，
consumer 的 `place_shared_views` 已按 `st.slots` 放置，`140/130` 已按 `g_ctl->slots` 走。

建议：把“帧环槽数”从 `kMaxSlots` 常量解耦成会话字段 `TransportSlots`（默认 16，ini 可调）：
producer 发布 `probe.slots = N` 并在 `claim_slot` / `frames[]` / `limit = N-2` 使用它，
MV/depth 的 `streams[i].slots` 同步为 N。预计 4K 省 ~0.9–1.4 GiB 提交内存与一部分启动时间；
风险是 lag>16 时 `pair: no_slot` 上升，需要 A/B。

### K. producer 的异步 copy 只有一对 allocator/list

`copy_begin_frame()`（`producer.cpp:1912-1941`）在**每帧开头**等上一帧的 `g_copy_done`
退休（最多 50 ms），这段在游戏渲染线程上（由 `on_record` 调用）。consumer 侧同类问题早已
用 `kCopyRing = 8` 解决（`130:253-257`）。

建议：producer 也做 2–4 个 (allocator, list, fence value) 环；`g_copy_done` 只用于环槽复用
（本地 fence，`GetCompletedValue()` 可信）。收益看 `copywait=...ms`（启动窗口出现过 16 ms）
与偶发 hitch，不影响稳态 fps。

**实测证据（2026-11-25 20:4x 的 CP2077 producer 日志）**：
`copywait=2.9/16.0ms`、`1.0ms max=16.0ms` —— 16 ms 的峰值确实出现在正常会话里，
而 EMA 只有 1–3 ms，说明它是**偶发**的、被平均掩盖的渲染线程阻塞。
同一份日志还给出了 codec 后的真实跨卡流量：`fr: coded mv 7.3–12.9 MB/frame`、
`depth 4.7–6.2 MB/frame`（raw 31.6 / 14.1），即约 **12–19 MB/帧**，
比 `bus:` 行按原始尺寸算出的 45.7 MB/帧小得多（`bus:` 那行是上界）。

### L. 33³ LUT 每帧重建

`085:1817` 每帧 `record_patch_lut3d()`：一次 stride=4 的采样 dispatch（4K ≈ 52 万线程，
写 2.3 MB 带原子累加）+ 一次 `9³` fill dispatch。补丁的 H→P 映射是**时域缓变**的。

建议：按 N 帧（2–4）或“采样签名变化超过阈值”重建，其余帧复用 `g_patch_lut3d`；
先加 `--lut3d-interval N` 做 A/B，量 `total/load/slow`。注意
`record_patch_calibration()` 是 LUT 失败后的回退路径，保持按帧即可。

### M. marker 每帧一条日志 + 每帧 D3D11 阻塞读

- `115:233-239`：`payload` 含 `ms12`，每帧都变 ⇒ 每帧 `[lock] marker: seq12=...`。
  建议只在前 5 次 + 每 300 帧一次，或改成只在 seq12 变化时打。
- `115:74-175`：4 次 `CopySubresourceRegion` + 一次 `Map`（DO_NOT_WAIT，忙则阻塞）。
  这是每帧唯一的 CPU↔GPU 同步点，必须存在（“绝不错配”靠它）。
  可行方向（需实测，先别默认改）：`mgpu_pair_core` 已支持 `ms12` 时间匹配，可做
  `--pair-by-time` 的 A/B（用 `SystemRelativeTime` 对 `frames[].frame_qpc_ms` 做窗口匹配，
  跳过 staging 读回），用 `pair:` 命中率与 `age/total` 判断是否值得。

### N. 每帧 COM 查询 / 死代码

- producer 每帧 `copy_stream()`：`src->GetDesc()`、`dst->GetDesc()`、`list->GetType()`
  ×最多 5 条流；`fr_encode_dispatch()` 再 3 次 `GetDesc()`（`producer.cpp:1661,1664,1700`）。
  描述符在会话内不变，可按资源指针缓存（收益小，但都在游戏渲染线程上）。
- 死代码 / 注释漂移：`060:285 wait_slot`、`060:220 g_fwd_expect`、`020:812 g_fwd_last`、
  `140:337 pair_drain_slot`（恒 -1）、`130:506 prefetch_publications`（被 `160:935` 关闭）、
  `060:245` 注释写 “implied by prefetch / off by default” 但变量是 `true`。
  另外 `--no-prefetch-inputs` 会顺手把 `g_input_copy_queue` 也关掉（`160:756-760`，两者被耦合），
  而默认 `g_upsample=ON` 时 `cq` 恒 false，**copy queue 在默认路径里根本不参与**。
  建议清理或在注释里注明“lock_frame 强制关闭/默认不生效”，否则下一轮很容易再踩 E。

### O. 大于 3840 宽的渲染尺寸（5K）在纹理档会被静默跳过

`cap_dim()` 把纹理槽位卡在 `kStreamMaxW/H = 3840×2160`（`mgpu_shared.h:124-125`）。
渲染 5120×2880 且走**纹理**档（`MvCodec=0`，或 MV 格式不是 R16G16_FLOAT 被 codec 拒绝）
时，`copy_stream()` 命中 `sd.Width > dd.Width` 分支，只打一次
“is above the placed … skipped until the next session”，之后整段会话没有 MV/depth（1X）。
codec 档不受影响（`layout_streams` 用内容尺寸计算字节槽）。
建议：要么把 `kStreamMaxW/H` 提到 5120×2880（代价是纹理档内存），要么在
`compute_target()` 里把内容尺寸 clamp 到上限并打醒目日志（现在是“一次静默跳过”）。

### P. `--fg-colour shared` 是失效路径（永远 1X）

`160_main.inc:1418-1473` 的 shared-colour 循环每来一个 publication 就
`run_frame(nullptr)`；而 `140_frame.inc:338` 的整段配对/拷贝/ACK 都在
`!g_reuse_frame && colour11 != nullptr` 里面：

```cpp
if (!g_reuse_frame && colour11 != nullptr && g_ctl != nullptr) {  // nullptr -> 整段跳过
    const LockPairResult lp = lock_pair_colour(colour11);
```

结果：`pair_ok` 一直是 false → `copy_inputs_local()` 不执行 → `g_mv_gate_off = !pair_ok = true`
→ MV/depth 不 tag，永远 1X。UI 从不启用它，所以没被实机暴露。

建议：这个模式下 colour/MV/depth 来自**同一条 publication**，根本不需要 marker：
`pair_ok = true; pair_slot = g_mv_slot`（或用 `g_ctl->frames[]` 里最新 published），
并跳过 `lock_pair_colour()`；或者直接删掉这个 CLI 入口，避免它被当成可用 A/B。

### Q. 把 consumer 放在游戏目录 = 给 consumer 进程注入 fakerFG（实测）

`lock_frame.exe` 导入 `dxgi.dll`；如果它和游戏的 `dxgi.dll`（= fakerFG）在同一个目录，
Windows 会把这个代理加载进 consumer 进程。2026-11-25 的三次实测都能在
`lock_frame_fakerFG.log` 里看到对应的 pid：

```
==== fakerFG pid=8588 2026-11-25 21:21:54 ====
DllMain: attached as dxgi.dll, pid=8588
dxgi: create#1 hwnd=... 3840x2160 fmt=24 effect=4 flags=0x40
xefg: D3D12CreateContext -> OK (0); maxSupportedInterpolations=2
xell: libxell.dll -> ...; setSleepMode(lowLatency) -> 0
xefg: init 3840x2160 fmt=24 flags=0x1 maxInterpolated=2 uiMode=0
xefg: D3D12InitFromSwapChainDesc -> OK (0) in 250 ms
xefg: hooked swap chain Present (enabled) / Present1 / ResizeBuffers ...
present: 120 frames avg=8.6ms ...
```

即：consumer 进程里多了一份 XeFG context（250 ms 初始化）、一份 XeLL（lowLatency 模式）
和一套 Present/Resize 钩子，和 lock_frame 自己的 XeFG 挂在同一条交换链上。
发布包不会这样：README 明确要求工具放独立目录、`deploy_game.bat` 也只把
`dxgi.dll/fakerFG.dll/nvngx_dlssg.dll/xess_mfg_producer.dll` 放进游戏目录。

建议：**所有 A/B 和发布都用「工具目录 + `--game`」布局**。本轮已经把
`C:\temp\lock_frame_audit\`（exe + libxess_fg/libxell + xefg_programs）建好，
`audit_out_3x_keep2x.bat` 就是从那里启动、其它预检/日志不变。

### R. consumer 退出后 producer 仍在编码/拷贝

`140_frame.inc:556-562` 每帧写 `request_mask`，而 consumer 的退出路径是
`TerminateProcess`（`010_platform.inc:56-63`），没有机会清零；producer 端的
`want = request_mask | consumer_request` 于是保留最后一次掩码，`want==0` 时还回退到
`mv|depth`（`producer.cpp:3983-3990`）。实测：两次 run 之间的空档里
`publish #3600 ... mask=0x83`、`publish #4200 ... mask=0x3` 照常发生。

影响：用户 Ctrl+Alt+End 关掉 overlay 后继续玩，渲染卡仍每帧跑两个 codec encode +
跨卡写（`fr: coded mv ~7-19 MB + depth ~4-6 MB`），直到游戏进程重启。
建议：producer 每 ~1 s 重查一次 `kConsumerPresentEvent` 是否存在（事件随 consumer 进程
消失），不存在就把 `want` 归零并跳过拷贝；consumer 侧不需要改（进程已经死了）。

---

## 5. 建议补丁顺序（每步都可单独回归）

| 步 | 内容 | 风险 | 验证 |
|---|---|---|---|
| 1 | ✅ A：发布 fence 队列等待（已落地） | 低（只加 GPU 侧 wait） | `--d3d-debug` + 长跑鬼影；`pair:` 不变 |
| 2 | ✅ C：重建结果按 publication 校验（已落地） | 低 | `xefg-in:` 尺寸不变、画面不变 |
| 3 | ✅ D：`record_upsample` before-state 改 COMMON（已落地） | 低 | 同上；debug layer 报错清零 |
| 4 | B：reclaim 变有界兜底 | 中（会多丢帧） | `producer_skipped` / `pair no_slot` 对比 |
| 5 | ✅ E/F：codec 守卫 + jbuf 泄漏（已落地） | 低 | `--jbuf/--lock` 组合跑 3 分钟看句柄/显存 |
| 6 | J：`TransportSlots`（16） | 中（协议字段已就绪） | 4K 下 `no_slot` 与启动时间 |
| 7 | L：LUT 间隔 | 中（画质） | A→B→A，`total/load` 与 UI 边缘截图 |

## 6. 不要做（沿用既有结论，避免重复劳动）

- 不要为了 CPU 再去优化 `logf_` / `osd_update` / 中位数排序（`PERF_BASELINE` §3.28 已撤回，
  `cpu<2 ms`）。
- 不要重开 `--prefetch-inputs` / `--pub-fifo`：锁帧版的配对语义就是“颜色到了再读”，
  且 E 说明 codec 会话根本不兼容。
- 不要把颜色链路改成“D3D11 先转 PQ 再拷”（`PERF_BASELINE` §3.2 已算出总字节相同）。
- 不要在未验证 `--d3d-debug` 前改动 tag 的 `pCmdList` 语义（`--input-copy-queue` 那条边界
  仍是“tag 不带 list”，改变它需要单独 A/B）。

## 7. 本轮可直接采集的证据（不改代码）

1. 默认配置跑一次 `--d3d-debug`（30 s 即可），抓
   `grep -i "statebefore\|RESOURCE_BARRIER\|INVALID_CALL"` → 验证 D。
2. 同一份日志抓 `grep "reclaimed oldest slot"` → 验证 B 是否在真实场景出现。
3. `grep "copywait="` 看 producer 的拷贝退休等待分布 → 决定 K 的优先级。
4. 记录 4K 下 `lock_frame.exe` 的提交内存/工作集与 producer 日志里的
   `shared: begin mapping reserve=` → 验证 J 的 GiB 估算。
5. `grep -c "\[lock\] marker: seq12"` 看单次会话日志行数 → 验证 M。

---

## 8. 实测结果（2026-11-25 21:19–21:22，CP2077 4K，40 fps 锁，3x keep2x）

环境：`3840x2160` 渲染/呈现，DLSS FG 开，`--streams=mv,depth`，producer
`MvCodec=1 (100%) / DepthCodec=1 (150%)`，映射 `used=1773.6 MB slots=32`，
`mv 3840x2160 fmt 34 / depth 2560x1440 fmt 39`。三次 run 由 `audit_1/2/3` 启动。

| 指标 | run1 基线 | run2 UI patch(mode 4) | run3 `--d3d-debug` |
|---|---|---|---|
| 窗口数（40 帧/窗口） | 37 | 46 | 34 |
| `pair:` exact=40/no_slot=0 的窗口 | 36/37（首个窗口是启动 2 帧） | 45/46（同） | 33/34（同） |
| `xN=3.00` 的窗口 | 36/37（首窗口 1.00） | 45/46（首窗口 1.00） | 33/34（首窗口 1.00） |
| `load` | 7–60%（稳态 9–29%） | 8–83%（首窗口；稳态 9–29%） | 28–83%（首窗口；稳态 28–60%） |
| `slow` / `timeouts` / `xNmiss` | 0 / 0 / 0 | 0 / 0 / 0 | 0 / 0 / 0（1 次 HITCH） |
| `out:` presents / expected | 120/120（第 2 窗口 118/120，启动过渡） | 120/120（同） | 120/120（首个窗口启动过渡） |
| `OUTPUT FRAMES MISSING` / `VBLANK MISSED` | 1（启动）/ 0 | 1（启动）/ 0 | 1（启动）/ 0 |
| `wait=`（A 的新队列等待） | 全程 0.0 ms | 全程 0.0 ms | 全程 0.0 ms |
| `pf:` 行（E） | 0 | 0 | 0 |
| `[d3d12 …]` 校验消息（D） | — | — | 1045 条，**0 条** barrier/state/invalid-call |

run3 的 1045 条校验消息全部是良性的：

- 1039 条 `ID3D12Device::CheckFeatureSupport is superseded by ...CheckFeatureSupport1`
  （XeFG 库自己的调用，与本次改动无关）；
- 6 条 `CreateCommittedResource: Ignoring InitialState D3D12_RESOURCE_STATE_GENERIC_READ`
  （upload buffer 用 GENERIC_READ 创建，D3D12 规定忽略并按 COMMON 处理）。

**没有** `statebefore` / `RESOURCE_BARRIER` / `INVALID_CALL` / `DEVICE_REMOVED`，
即 D 修复后的两处 `COMMON` barrier 与实际状态一致，`record_upsample()` 不再发非法转换。

tag 侧（C）：三次 run 的 `xefg-in:` 都是
`mv=3840x2160 depth=3840x2160 hudless=0x0`（run2 为 `hudless=3840x2160`），
即 2560x1440 的 depth 码流每帧都被重建到 3840x2160 并成对 tag；没有任何
`--no-upsample tags MV ... skipping FG` 或尺寸 mismatch，说明 publication 校验没有误伤。
run2 的 patch 链完整：`ui-unblend: sparse dispatch ON (32400 tiles)` →
`HUDless rebuilt from 3840x2160 present + 3840x2160 HUDless patch (source=patch)`。

同一会话里另外确认了三条此前的推断：

- **B**：`no ACKed slot - reclaimed oldest slot 1/2/3 (pub=51)` 恰好出现在一个新 run
  刚连上、`consumed[]` 还没 ACK 的 32 个 publication 之内（本次会话 3 次）。
- **K**：`copywait` 只在 transport 建立后的前三帧到 16.0 ms（`publish #2/#3`），
  之后稳定在 1–6 ms；即峰值集中在启动窗口，符合“单 allocator 等退休”的模型。
- **J/M**：`shared ready ... used=1773.6 MB` 与 §4 的 1.7 GiB 估算一致；
  `marker: seq12` 行数 = 1475 / 1813 / 1342（≈ 每帧一条，与 M 一致）。

**结论**：A/C/D/E 通过（无回归、无校验错误），F 未被执行到（本轮没有 `--lock/--jbuf`）。
新增两条现场问题：**Q**（游戏目录布局把 fakerFG 注入 consumer，已在 `audit_out_*`
脚本里改用工具目录布局）和 **R**（consumer 退出后 producer 仍编码，待下轮修）。

---

## 9. 第二轮实施（B / J / R，2026-11-25 晚）

只动 producer + 两处 consumer 的会话令牌格式（协议仍是 v34，布局不变）：

### B. `claim_slot()`：只覆盖“消费者不可能还需要”的槽

`producer.cpp` 的重建分支改成有安全谓词的唯一兜底：

```cpp
const bool safe = (fid == 0) || (ack > 2 && fid + 2 <= ack);   // ack = colour_seq
```

- `fid == 0`：新会话刚清空 `frames[]`（`reset_consumer_session`），槽里没有帧，可直接复用
  —— 这样启动阶段仍能立刻填满环，不会多丢 8 帧；
- `fid + 2 <= colour_seq`：比消费者**最后一次验证**的帧还老两帧。消费者在配对时写
  `colour_seq`（早于 `gpu_throttle`），它正在处理的那一帧是 `colour_seq-1`，所以 margin 2
  才是“连在飞帧都不可能再读”的第一个值；
- 其余情况**丢帧**：`no safe slot to reuse`，消费者保持上一帧并重锚，收到 ACK 后自然恢复。

reclaim 不再 `++producer_skipped`（它不是 skipped），只有真正丢弃才 +1。

### J. `TransportSlots`（默认 16）

producer 端新增 `g_ring_slots`（`fakerFG.ini TransportSlots`，4..32），并让以下全部跟随它：
`layout_streams()` 的 MV/depth/colour 槽数、`place_slots()`、`probe.slots`、fence 创建循环、
`update_credit()` 的求和、`claim_slot()` 的环遍历、`publish_frame()` 的 watermark
`limit = ring-2` / `ack_in_window` / `g_next_slot`。`frames[]/consumed[]` 数组仍是 32 项
（协议不变），consumer 早已按 `c->slots` 和 `st.slots` 工作，因此**不需要**同步改 consumer
逻辑（本轮 consumer 只改了会话令牌，见 R）。

预计 4K codec 映射：1773.6 MB → ~890 MB（`shared ready ... ring=16 used=...`）。

### R. consumer 退出后停止拷贝

`consumer_session` 的高 32 位就是 consumer 的 pid（本轮把两处构造改成
`(pid << 32) | 低 32 位`，见 `040_cadence.inc` / `115_marker.inc`，避免长 uptime 把 tick
异或进 pid 半区）。producer 的 `consumer_keep_publishing()`：

- 每秒最多探测一次 `OpenProcess(SYNCHRONIZE) + WaitForSingleObject(0)`；
- 消失后给 3 s 宽限，之后暂停拷贝；
- 之后每 5 s 放行一次“保活发布”，万一把 pid 解错（例如机器连续运行 >49 天时写入的旧令牌）
  也能被一次 ACK 重新唤醒；
- 日志：`producer: consumer process is gone - pausing copies after 3 s` /
  `consumer process is alive - transport running`。

注意：不能用 `kConsumerPresentEvent` 判活 —— `lock_frame_ui.exe` 会为该事件持有一整个生命周期。

### 部署与验证

- `build/xess_mfg_producer.dll` → 游戏目录（需**重启游戏**）；
- `build/lock_frame.exe` → 游戏目录 + `C:\temp\lock_frame_audit\`；
- md5 每次重建都会变（PE 时间戳），本轮的值写在 `audit_logs\audit_deploy_info.txt`；
- 游戏目录 `fakerFG.ini` 增加 `TransportSlots=16`。

验证点（`audit_out_3x_keep2x.bat` + `audit_9_report.bat`）：

| 项 | 期望 |
|---|---|
| J | `shared: TransportSlots=16`、`shared ready ... ring=16 used=~890 MB`（旧值 1773.6 MB） |
| B | 启动阶段只有 `reclaimed SAFE slot`（`ack=0`、fid=0 的槽），没有覆盖已验证帧；稳态无 `no safe slot` |
| R | Ctrl+Alt+End 后 ~3 s 出现 `consumer process is gone`，producer 日志不再出现 `fr: coded` / `publish #`，心跳不再推进 |
| 回归 | `pair: exact=40 no_slot=0`、`xN=3.00`、`wait=0`、`slow/timeouts=0` 与 §8 相同 |

---

## 10. B/J/R 实测结果（2026-11-25 21:39–21:43，CP2077 4K/40 fps/3x keep2x）

三次 run（基线 / UI patch / `--d3d-debug`），producer 会话 pid 24176、21:39:44 启动。
（本轮仍然是从游戏目录启动的 consumer，`lock_frame_fakerFG.log` 里能看到
21:41:39 / 21:42:16 / 21:43:16 三个 pid —— 即 Q 的干净布局 **没有** 被执行。）

| 项 | 实测 | 判定 |
|---|---|---|
| J 映射 | `shared: TransportSlots=16`、`begin mapping reserve=1017.6 MB ring=16`、`shared ready ... used=1017.6 MB`（上轮 1773.6 MB） | **-756 MB（-43%）** |
| B 覆盖 | `reclaimed oldest slot` **0** 次；`reclaimed SAFE slot 7/8/9 (ack=0)` 只在 session 启动时出现（fid==0） | 安全路径生效 |
| B 丢帧 | `no safe slot to reuse` 共 15 行，全部落在 run 开始/结束（ack=3850/0/6120/0/6964 五个簇）；稳态零丢帧 | 符合设计 |
| R 判活 | `consumer process is gone/alive` 共 7 次切换；最后一次 gone 之后只剩 3 个 `fr: coded` 块（≈3 s 宽限 + 1 次 5 s 保活），不再连续编码 | 生效（见下注） |
| 配对 | 3 次 run 各只有启动窗口不完美（`presents=0`、`118/120`、`pair exact=1 no_slot=1`），其余 `exact=40 no_slot=0` | 与上轮一致 |
| 输出 | 28/30、50/52、16/18 个窗口 `presents=120 expected=120`；`VBLANK MISSED` 0；`wait=0.0ms` 全程；`pf:` 0 | 无回归 |
| `xN` | 每个 run 只有启动窗口 1.00，其余 3.00；`slow/timeouts/xNmiss` 0 | 无回归 |
| D（run3） | 1045 条校验消息（1039 `CheckFeatureSupport` + 6 `GENERIC_READ`），barrier/state/invalid-call **0** | 无回归 |
| UI patch（run2） | `ui-unblend: HUDless rebuilt ... (source=patch)`、`sparse dispatch ON (32400 tiles)` | 正常 |

两个观察（都不是故障，但要知道）：

1. **pair lag 变大了**：32 槽时 `lag(avg)≈0.97`，现在 `2.33–3.00`（run1）、`3.85–7.00`（run2）、
   `max=13`（run3，debug 层把 consumer 拖慢）。lag 只表示“配对的那一帧比环里最新帧旧多少”，
   配对本身仍是 exact，MV/depth 与颜色同帧，不影响画质。但 `TransportSlots=16` 下
   watermark 上限是 14，`max=13` 已经很贴边：某个标题若出现 **稳态** `no safe slot` 或
   `pair no_slot`，把 `TransportSlots` 调到 24/32（重启游戏）即可。
2. **`record: no slot for seq=...` 会在退出窗口刷最多 300 行**（`told_noslot` 上限），
   属于日志噪声，下一轮可顺手降频（例如每 60 帧一次）。本轮实测 76 行。

R 还需要一次更长的空闲窗口来确认“5 s 保活”的节奏：本次三次 run 之间都只隔了 1–2 s
（落在 3 s 宽限里），最后一次退出后 6 s 游戏就关了，只能看到 3 s 宽限 + 1 次保活。
如果要坐实，Ctrl+Alt+End 后在世界里停 20 s 再取日志，应看到
`publish #` / `fr: coded` 每 5 s 最多一组。
