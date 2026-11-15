# 100% 配对（颜色 ↔ MV/depth）：设计、落地改动与实测手册

最后更新：2026-11-15。本文对应 `TOTAL_ARCHITECTURE_AND_DATAFLOW.md` 的 §3/§5：
那里的描述是**改造前**的行为，本文给出改造后的算法与验收方法。

---

## 1. 问题（一句话）

原设计用"**上一帧解码出的 marker id + 1**"去猜当前颜色的发布号。它只在
"处理过的颜色序列严格连续"时成立；WGC 丢一帧、DWM 重复一帧、或复用/跳过一帧，
配对就会静默错位——而 `pf served` 仍然记成命中。

形式化：

```
C_k           = 第 k 个被处理的颜色帧的发布号（当时未知）
D_k           = 第 k 次解码得到的 marker（= C_{k-1}，因为 staging 读晚一帧）
旧代码 tag 的 = D_k + 1
正确 ⇔ C_k = C_{k-1} + 1
```

丢帧（C_k = C_{k-1}+2）→ 配到旧一帧；重复（C_k = C_{k-1}）→ 配到新一帧。
两种都无检测、无计数。

---

## 2. 改造后的算法

### 2.1 当前帧解码（不再有 +1）

`marker_decode_all()` 现在把四角 copy 进 staging 后**读刚填的那一格**：
先 `Map(DO_NOT_WAIT)`，若 `WAS_STILL_DRAWING` 再做一次**阻塞 Map**（144 px，
实测应在亚毫秒级；超过 1 ms 会打一条一次性日志）。只有阻塞也失败时才退回上一帧
的 staging，并置 `MarkerBlocks.stale = true`；`stale` 的 id **永不参与 tag**。

### 2.2 精确匹配（id + 时间双条件）

`src/mgpu_pair_core.h::find_exact()`：

```
候选 slot s 满足：
    (frames[s].frame_id & 0xFFF) == marker.seq12
且  circular_distance(frames[s].frame_qpc_ms & 0xFFF, marker.ms12) <= 40 ms
取 published 最大的候选；若仍有两个候选（4096 帧回绕）→ ambiguous，不 tag
```

- ms12 字段以前完全没用；现在它负责**拒绝过期发布**并**消解回绕**；
- 找不到时区分 `no_slot`（发布已被覆盖）与 `ms_fail`（同 seq12 但时间不对），
  两者都走 1X，不再"猜一个"；
- 旧的 `unreadable -> last+1 by succession` 路径已删除。

### 2.3 验证（tag 前的最后一道门）

选择 slot 之后、tag 之前：

```
effective_id = g_local_id[slot] ?: frames[slot].frame_id   // 本地副本的 id 优先
verified = (marker 解出) && (effective_id == marker 的完整 frame_id)
```

- 比较的是**本地副本**的 id：共享槽可以在拷贝退休后被生产者合法复用，
  此时 live slot 已经是新发布，但我们的本地数据仍是颜色对应的那一版；
- `verified == false` → 本帧不打任何 tag，`SetEnabled(0)` 走 1X；
  下一次 verified 时 `SetEnabled(1)` 并强制 `resetHistory` 一次。

### 2.4 ACK 语义修正（这是另一半）

旧 `ack_previous_slot()` 会把**除两个槽以外所有槽**的 `frames[k].published`
写进 `consumed[k]`——那是"我已经有你最新的一版"的谎话，生产者于是可以覆盖
仍在预拷/尚未显示的数据。

新语义：

```
for each slot:
    if 本地副本存在 && copy fence 已退休 && local_pub > consumed:
        consumed = local_pub          // 只 ACK 真正拷到本地的那个版本
if 本帧是帧内现拷:
    consumed[slot] = max(consumed, 本帧选择时快照的 published)
```

"拷完即可 ACK"是安全的：数据已经在 AMD 本地，共享槽可以立刻复用。
这同时消除了 under-ACK（每帧只还一个槽）与 over-ACK（还了没拷的版本）。

### 2.5 容量（v18：4 → 8 槽）

`kMaxSlots = 4`、`kSharedVersion = 19`（第一次实测 lag max=1，4 槽足够；8 槽会让启动变慢）。容量按
`kMaxSlots > p99(colour lag) + 2` 定；新增的 `pair:` 行直接报 `lag(avg/max)`。
映射大小 = render 尺寸 × 4 B × 2 流 × 槽数（2227×1253 ≈ 90 MB，4K ≈ 265 MB；StreamPercent 或更低渲染分辨率可降低）。

### 2.6 为什么这能到 100%

"100%" 定义为**实际交付给消费者的颜色帧**：

1. 每帧的身份来自它自己（当前帧 marker），不依赖序列连续性；
2. 每个身份都必须同时在"id"和"时间"两个维度匹配，否则不打 tag；
3. tag 前再与本地副本的 id 比对一次，错配概率为 0（不是"更低"，是条件不成立）；
4. 无法配对时输出 1X，而不是错配——把"未命中"变成可见、可控的降级；
5. ACK 只在本地数据就绪后推进，生产者不会覆盖尚未取证的数据。

注意：WGC 本身可以合法地丢弃或重复呈现（`MinUpdateInterval`、DWM 重复合成），
所以"每个 producer 发布都被颜色消费"仍然做不到；能做到的是
**每个被消费的颜色都配对正确，且未配对时明确 1X**。

---

## 3. 改了哪些文件

| 文件 | 改动 |
|---|---|
| `src/mgpu_pair_core.h`（新） | 精确匹配、ms12 校验、ACK 谓词、容量公式（纯 C++） |
| `src/xess_mfg_consumer/consumer_internal.h` | 引入上面的头 |
| `src/xess_mfg_consumer/modules/frame.cpp` | 当前帧解码；删除 +1/succession；精确匹配 + 验证；未验证走 1X；`pair:` 指标；wide-depth 修复 |
| `src/xess_mfg_consumer/modules/input_cache.cpp` | ACK 改为"本地副本 fence 退休 + 精确版本"；宽深度 `continue`；预拷不再因 `g_reuse_frame` 停止；记录 `g_local_id` |
| `src/xess_mfg_consumer/modules/frame_io.cpp` | `g_local_id[]`；`g_fwd_expect` 扩到 8 |
| `src/mgpu_shared.h` | `kMaxSlots = 8`、`kSharedVersion = 18` |
| `tests/pair_core/sim.cpp` + `run.sh`（新） | 19 项纯逻辑检查（连续/重复/丢帧/覆盖/回绕/ms 失配/ACK/容量） |
| `scripts/pair_report.py`（新） | 解析 `pair:`/`pf:`，输出通过/失败判定 |

---

## 4. 本机已验证 / 未验证

已在本机（Linux）执行：

```
bash tests/pair_core/run.sh            # 19 checks, 0 failures
python3 scripts/pair_report.py ...     # 合成日志：PASS / strict FAIL / 坏日志 FAIL 均符合预期
```

**未**执行：Windows 编译、游戏内实测、真实日志分析——本环境没有 MSVC/D3D12/游戏。
因此下节的步骤需要在你那边跑；把日志发回来即可按第 6 节判定。

---

## 5. 实测步骤（Windows / 游戏机）

1. 重新构建三件产物（协议 v18，必须同时更新）：
   `scripts/build_faker_fg.sh`、`scripts/build_producer.sh`、`scripts/build_consumer.sh`
   （或对应的 .bat/VS 工程），确认 consumer 启动日志出现 `slots=8`。
2. 基线场景：CP2077 4K，**锁 40 fps**（整除点），默认 optimal：
   `run_locked_default.bat`，跑 5 分钟。
3. 非整除/抖动场景：解锁 43 fps 左右，跑 5 分钟。
4. 停顿场景：跑动中 alt-tab 一次 / 打开地图 2 秒，回到游戏再跑 1 分钟
   （验证 `lost(1X)` 出现但 `mismatch` 仍为 0）。
5. 每次运行保留 consumer 日志（含 `--report` 窗口行）。

可选对照：`--wgc-minint 0`、`--pool 3` 各跑一次，比较 `lag(max)` 与 `lost`。

---

## 6. 日志判定（把日志发回来时按这个看）

```
python3 scripts/pair_report.py <log>
python3 scripts/pair_report.py --strict <log>
```

| 指标 | 目标（非 strict） | 目标（strict / "100%"） |
|---|---|---|
| `mismatch` | 0 | 0 |
| `ms_fail` | 0 | 0 |
| `ambiguous` | 0 | 0 |
| `lost(1X)` 比例 | ≤ 0.5% | 0 |
| 帧内拷贝（missed+in-flight） | 越低越好 | 0 |
| `pf overflow` | 0 | 0 |
| `lag max` | < slots − 2 | < slots − 2 |

若 `lost` 偏高：看 `unreadable`（marker 可读性）、`no_slot`（颜色太晚，需加槽/
降 WGC 延迟）、`ms_fail`（配对到了错误的时隙，通常是 marker 时间与发布时基不一致）。
若 `lag max` 接近 `slots`：把 `--wgc-minint 0`、`--pool 3`、presenter 优先级先做掉。

---

## 7. 实测记录（2026-11-15 06:31，第一次 locked40）

部署与握手正常：consumer `slots=8`、producer `used=540.6 MB slots=8`，
marker 当前帧解码 `exact=1320/1320`（`ms_fail=0`、`ambiguous=0`），
但 `mismatch=1155`、几乎每帧打印 `no verified MV pair -> 1X`，FG 被反复关掉。

根因（日志 + 代码对照）：**验证发生在输入路径之前**。ACK 改为"拷完即 ACK"后，
生产者可以立刻复用共享槽；此时本地缓存里的副本常常还是上一版，而验证拿它和
当前 marker 比，必然 mismatch。修复（已部署 v3）：

1. 验证移到"输入拷贝/tag 准备"之后；
2. 缓存命中若 `g_local_id[slot] != marker id`，降级为现拷（`g_pf_stale`），
   不再直接判 1X；
3. 重复交付（reuse）沿用上一帧的验证结果，不重新对 last_slot 验证。

重测后 `mismatch` 应为 0、`exact` 应回到 ~40/窗、`lost(1X)` 接近 0。

### 7.1 启动慢的最终结论（07:30）

A/B/C/D 四组（不注入也测）点击→主菜单都是 42–47 s，确认是本地其他软件占 CPU，
与 producer/consumer 无关。按用户要求把 producer 还原到加闸门之前的 v19/4 槽构建，
consumer 保留配对修复、入口去重与启动计时。发布闸门/事件门仅作历史记录。


---

## 8. 还没做的（需要下一轮）

1. **colour sideband（`stream[0]`）**：唯一"构造上 100%"的方案；需要 fakerFG harvest
   颜色 + producer 复制/PQ + consumer 绕过 WGC。当前 producer 不放置 stream 0，
   `--fg-colour shared` 已改为告警并回退。
2. **presenter 优先级/MMCSS**、`--pool 2`、`GetLastPresentCount` 闸门——见
   `TOTAL_ARCHITECTURE_AND_DATAFLOW.md §12.6`（仍未做，需要实机验收）。
3. `lag` 的高分位统计（当前只有 avg/max）；若 max 长期贴近容量，再考虑 12 槽。
