# consumer 全面审计（2026-11-15，第二轮）

目的：在继续推进 carry 之前，确认 consumer 没有“逻辑错误或算法设计错误”。
方法：按 unity 构建顺序读完全部 16 个模块，重点看状态机不变式、fence 与资源生命周期、
计数口径、门控条件、阈值边界；对能在纯计算层验证的部分用 `tests/cadence_core` 复算。
下面是结论，分“已验证正确 / 已修真 bug / 发现但未改”三类。

---

## 1. 已验证正确的不变式

| # | 不变式 | 位置 |
|---|---|---|
| 1 | 颜色帧是全生命周期成对的：`colour_push` 溢出时 `close_capture_frame`，`colour_pop` 之后 presenter 也会 `retire_capture_frame` | `main.cpp` |
| 2 | slot ACK 不碰“正在读”和“下一帧要读”的 slot；`consumed` 只在 GPU 退休后写 | `ack_previous_slot` |
| 3 | 跨适配器 fence 不在消费节点读 `GetCompletedValue`（会读回 UINT64_MAX），改用显式 `g_fwd_expect` + 一次性事件 | `wait_slot` |
| 4 | 本地拷贝优先，跨适配器共享纹理只读一次；MVP/depth 同一 slot 成对 tag | `copy_slot_streams`、tag 段 |
| 5 | copy queue 与 direct queue 的等待位置正确：convert 先入队，`Wait(copy fence)` 在 Present 之前 → 重叠且对 XeFG 有序 | `input_copy_wait` / `prefetch_wait` |
| 6 | 相位只按**真正生效**的 k 结算（`Commit(appliedK)`），请求被拒/被门限挡住时误差留在相位里 | `CadenceCore::Commit` |
| 7 | reuse 帧不推进相位、不换 sink、不推进 slot（`slot = g_last_slot`） | `run_frame_ex` |
| 8 | 世代变化只重建本地拷贝与 pub 队列，保留学习值（dwell/成本/统计） | `service_shared_layout` |
| 9 | jEff 三重夹取：DLL 上限、成本上限、可实现性学习（连续 3 次不满降一档） | `Update` |
| 10 | 24 项结构检查（J 扫描、cap-limited、成本上限、Reset、冷启动、相位有界…）全过 | `tests/cadence_core` |

---

## 2. 本轮修掉的真 bug

### 2.1 `input_copy_begin()` 忽略等待结果，会 Reset 仍在执行的 allocator

```cpp
wait_fence_value(g_copy_fence.Get(), slot_fence, 50);   // 返回值被丢掉
g_copy_alloc[i]->Reset();                               // 50ms 没退 = 未定义行为
```

D3D12 明确规定：allocator 仍被 GPU 执行时 `Reset` 是 UB，通常直接掉设备。
**修法**：等待失败就返回 false，让这一帧的拷贝留在自己的 list（走直接队列），
ring 指针照常前移（8 帧后再回来大概率已完成），并打一行日志。

### 2.2 tag 空资源（MV / depth 没准备好就 tag）

`rd.pResource = g_mv_local[slot].Get()` 可能为空（首帧、generation 刚变、局部拷贝失败），
空指针交给 XeFG 就是 `ERR_OTHER` —— 每次运行 `frame 1 ... ERR_OTHER` 正是它。
**修法**：空则跳过该 tag 并记一次日志，让这一帧按“没有 MV/depth”处理。

### 2.3 `--cad-policy` 默认仍是 `optimal`，且静默

`optimal` 是 4K 4x 上会 `ERR_OTHER`/挂住的旧路径；某个入口漏掉 `--cad-policy fill`
就会静默跑回它。**修法**：配置时如果 policy=optimal 就打 WARNING 指出该用
`--cad-policy fill --cadence-hold N`（不改默认值，避免影响既有入口）。

> 本轮更早一批已修的 6 个（相位夹取、mutex HRESULT、sink 顺序、atomic、warp null、
> marker 记账）见 `docs/PROGRESS_2026-11-15.md` §10。

### 2.4 `--no-fg` 子串匹配把 `--no-fg-pace` 变成“关掉帧生成”（实机暴露）

`wcsstr(L"--no-fg")` 会命中 `--no-fg-pace` → 整轮 `mode: fg=off`、xN=1.00。
实测：`run_consumer_carry_nopace.bat` 那一轮输出只有 49.9% 的预期张数，
而它本来要测的是“关掉输入 pacer”。历史上 `run_consumer_dynamic_nopace.bat` 同样中招。
**修法**：`flag_alone()` 精确匹配（结尾/空白/引号），`--no-fg` 改用它。

### 2.5 `learnedMaxK` 被 MV 门控误伤（实机暴露，已修）

MV 门控关掉 FG（菜单/加载/暂停）时，XeFG 每帧只出 1 张，而 `NoteOutputShortfall`
仍按 `g_fg_interp_actual+1` 对账 → 连续 3 帧就把 `learnedMaxK` 永久降一档
（4→2）。实测 `carry15` 跑到一半 `j=2 learnedJ=2 regime=cap-limited`，show 从 119 掉到 103，
且只有连续 400 帧满档才会 +1，整轮恢复不了。

**修法**：门控期间不计入 shortfall 并清零连续计数（`ForgetShortfall()`）；
门控期间把 cadence 样本标成 `reuse`，相位与决策都不推进（否则按 k 张记账、实际只出 1 张）。



---

## 3. 发现但本轮不改（附建议）

| 优先级 | 问题 | 影响 | 建议 |
|---|---|---|---|
| **高** | **fill/carry 仍在走输入 pacer**：`fg_input_pace(g_cad_last_k)` 把捕获延迟到 `k×vblank` 网格，而 `k` 是**上一帧**的 | 相当于我们主动拉长输入间隔；`--cad-input producer` 之后它不再污染 k，但延迟与队列压力还在 | 用 `--no-fg-pace` 做一次 A/B。若更好，fill 路径应默认不 pace（`paced` 分支保留给 optimal） |
| 高 | `canSwitch` 恒为 true（`GetLastPresentCount + 2 >= g_present_id`） | “等上一批排空再换档”这道闸门等于没有，换档可能切在批次中途 —— 每次换档丢 ~1 张的来源之一 | 用 DXGI `GetFrameStatistics().PresentCount`（真实上屏数）或 XeFG `GetLastPresentStatus` 的累计输出做闸门 |
| 中 | `prefetch_publications` 部分拷贝失败时，`g_mv_local` 已更新而 key 还是旧的 | 极端情况下“新 MV + 旧 depth”被一起 tag，1 帧伪影 | 失败时把该槽两路都标脏（`g_local_pub[s]=0`），强迫下一帧整对重拷 |
| 中 | `swloss` 含在途帧 | 每窗口多算约 k 张，`auto` 会推错方向 | 开 `auto` 前先减掉在途张数 |
| 低 | 死代码：`cadence_next_slots` / `cadence_tick_slots` / `cadence_hold_count` / `g_cad_expected_out` | 日志里仍会打 `cadence:` 行，容易和新核心的行混淆 | 删除，或明确标注 legacy |
| 低 | `wait_slot` 超时后用旧内容 | 配对会落到旧 slot（设计如此，已有注释） | 保持，但超时计数应进报告 |
| 低 | `producer_interval_ms()` 暂停时陈旧 | k 冻结在最后一档 | `g_pub_stall_frames > N` 时退回 arrival 时钟 |

### 对 carry 的结论

- **控制律本身没有问题**：相位记账、上限夹取、regime 滞回、可实现性反馈、Commit 契约
  都能在仿真里复现并通过 24 项检查。
- 实机上限来自三个系统级问题（输入 pacer、排空闸门失灵、colour 停顿），
  其中**输入 pacer 会直接影响 carry 的 A/B 结论**：它把捕获往后推，等价于人为降低输入
  速率，于是 `show` 变低会被误读成“carry 不行”。
- 因此推进顺序建议：**先 `--no-fg-pace` A/B → 再定 hold**。

---

## 4. 部署

- 本审计后的 `xess_mfg_consumer.exe` =
  `b865d074ca33e567464565759e783f7da4f1dc2c9876f3358c26aba6a5ed1010`
- 备份链：`…bak_20261115_audit`（`d1e61fa3`）→ `…_phasefix`（`a873f27b`）
  → `…_stallguard`（`59d985a0`）→ `…_pre_switchloss`（`ff990c2e`）
- 验证：cl exit 0（无新警告）、`tests/cadence_core` 24/24、`--list-modes` 冒烟通过
