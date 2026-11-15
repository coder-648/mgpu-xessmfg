# 颜色入槽：彻底移除 WGC 的完整方案（协议 v22）

最后更新：2026-11-15。本文是**执行方案**，不是过程记录。结论先行，后面是
数据流、协议、逐文件改动清单、动态逐帧的适配分析、验收矩阵与回滚。

---

## 0. 结论（一句话）

**把颜色变成 producer 的一条同步流（stream 0），consumer 不再抓屏。**
发布（publication）成为唯一时钟：一条发布 = 一帧颜色 + 同一帧的 MV/depth + 元数据，
四者同槽、同 fence、同身份。WGC、颜色队列、marker 解码、present-ring 配对、
sink 轮转与 keyed mutex 全部删除。

代价（用户已确认接受）：渲染卡每帧多一次 4K 颜色复制，约 **6%**；
加上 2K MV + 2K depth + 元数据，总量约 **15%**。颜色格式按源格式**原样**复制
（FP16 scRGB 或 R10 PQ），producer 不做转换、不做 shader。

---

## 1. 为什么 WGC 这条路必须放弃（结论来自实测，不再重复验证）

| 问题 | 实测/结构原因 | 后果 |
|---|---|---|
| 跨卡到达时间不稳定 | WGC 的 `SystemRelativeTime` 被 DWM 量化到 2/3/4 vblank；异步队列传输无 100% 配对的到达保证 | 必然出现配对失败 → reset → 画面抖 |
| 与逐帧相位累加器不兼容 | 异步传输的到达间隔不是「每呈现帧一次」，而动态逐帧的 `T` 必须是 per-presentation | k 被我们自己的到达抖动推着走，carry/locked 的结论都建立在错误的输入上 |
| 颜色与运动数据不同源 | 颜色来自 DWM 合成，MV/depth 来自游戏 present 前 | 只能靠 marker/ring/时间戳猜身份，猜错就 reset |
| 250 ms 冻结 | 66 MB `CopyResource` 进 sink 会阻塞整个循环 | WGC 队列溢出 → 丢帧 → 再 reset |
| 无法摆脱的 6%+ 成本 | 抓屏后仍要拷进 sink、转换、上屏 | 这部分成本在 consumer 侧，且不受我们控制 |

**因此：不是「优化 WGC」，而是「删除 WGC」。** 只要颜色与 MV/depth 走同一条
发布，上面五条同时消失；剩下的只是渲染卡上那次复制（预算内）。

---

## 2. 目标架构

```
游戏（NVIDIA）: render + upscale
  │  fakerFG harvest（Evaluate，cmd_list 打开）
  │    BACKBUFFER（最终颜色，含 UI） + MV + depth + 元数据
  ▼
producer（游戏进程）
  │  claim_slot → 同一帧、同一个槽：
  │    stream 0 colour : 源格式原样 CopyTextureRegion（4K = 66.4 MB @FP16 / 33.2 MB @R10）
  │    stream 1 mv     : StreamPercent 缩放（默认 150 → 2/3 尺寸）
  │    stream 2 depth  : 同上
  │  publish: frames[s] 元数据 + fence_forward[s]
  ▼
共享映射 Local\mgpu_mailbox_v1（v22）
  ▼
consumer（AMD）
  │  只做一件事：等 publication → 同一槽拷 colour+MV+depth 到本地 → 转换 → XeFG → Present
  │  没有 WGC、没有颜色队列、没有 marker、没有 present-ring
  ▼
DWM → 面板
```

### 2.1 一帧的时序（目标）

```
游戏 present ──► producer: frames[s] 写入 + Signal(fwd[s])
                    │  （异步 copy 队列：等游戏队列 fence → 拷贝 colour/mv/depth）
                    ▼
consumer: wait_slot(s)  ← 这里就是「同步」的全部含义：等到同一帧的三个流都完成
   ├─ copy colour  s → sink（本地，D3D12 copy，跨卡读共享槽）
   ├─ copy mv/depth s → local
   ├─ cadence: k = f(frame_time_ms)（producer 每呈现帧一次的 EMA）
   ├─ convert（按 stream 0 的格式选 mode：FP16 → scRGB→PQ；R10 → pass-through）
   ├─ tag MV/depth → XeFG
   └─ Present(0) → Signal(frame fence) → 下一帧
```

**关键顺序（不变式）**

1. `wait_slot(s)` 必须在任何读 `g_colour[s]/g_mv[s]/g_depth[s]` 的命令之前；
2. 颜色、MV、depth 必须来自**同一个 `s`**（槽即身份，不再有配对状态机）；
3. `consumed[s] = frames[s].published` 只在本地拷贝真正退休后写；
4. `fg_set_interp` 仍然只在上一批退休后调用；
5. resetHistory 只允许来自游戏声明（`fg_flags`）或真正的“没有新发布”门控，
   不再有 pair-gap / colour-drop 两类。

---

## 3. 协议 v22（`src/mgpu_shared.h`）

| 项 | v21（当前） | **v22** | 理由 |
|---|---|---|---|
| `kSharedVersion` | 20（字段已到 v21） | **22** | 颜色流改变布局与语义 |
| `kMaxSlots` | 8 | **4** | 颜色与 MV 同槽后不再需要 8 深；4 槽 = 2 在飞 + 2 缓冲。共享内存 4×(66+33+33) ≈ **531 MB**（与之前 8 槽 MV+depth 相同量级） |
| stream 0 | 未放置 | **colour**：`slot_w/h = cap(output)`，`format = 源格式` | 原样复制，consumer 按格式选转换模式 |
| stream 1/2 | MV/depth | 不变（StreamPercent 仍有效） | 2K 数据 |
| stream 4 scar | 布局但从不写 | **移除放置**（常量保留为空位） | marker 机制随 WGC 一起删除 |
| `FrameShared` | 已有 `frame_id/qpc/frame_time_ms/flags/matrix` | 不变 | 元数据本来就在同一发布里 |
| `kRequestColor` | 存在（bit 2） | **成为默认请求位** | consumer 启动即请求颜色 |
| `colour_seq` | consumer 写、producer 未用 | **producer 用它做可选背压**（见 §4.4） | 颜色是最大流，必须能追得上 |

> 兼容策略：双方 `version != kSharedVersion` 直接拒绝（已有行为）。旧 producer +
> 新 consumer 或反之都不工作，这是有意的——本次是链路换代，不做双栈。

---

## 4. 颜色流设计

### 4.1 源资源选择（fakerFG 记录）

按优先级取一个，取到即用：

1. `MGPU_FG_STREAM_BACKBUFFER`（DLSS-G 的 Backbuffer，最终画面，含 UI）——与 WGC
   抓到的窗口内容最接近；
2. `MGPU_FG_STREAM_OUTPUT_REAL`（链上真实输出帧）；
3. `MGPU_FG_STREAM_HUDLESS`（无 UI 干净帧；只在没有前两者时使用，需同时传 UI，
   本版不做 UI 流）。

记录里的 `state` 是 fakerFG 声明的状态，producer 用它做 barrier（与 MV/depth 相同）。

### 4.2 格式与尺寸

- **格式 = 源格式**（`sd.Format`）。槽的 `format/slot_w/slot_h/offset/slot_size` 在
  `layout_streams()` 里按第一帧的源格式与 `cap_dim(output_w/h)` 一次放置；
- 4K FP16 = 66.4 MB/槽；4K R10 = 33.2 MB/槽；
- 分辨率/格式变化：内容矩形走 `StreamShared.seq` 更新；只有**容量变大**才会失败
  （打一行日志并跳过该流），与现有 MV/depth 的规则一致；
- consumer 的 sink 格式 = stream 0 的格式（`colour_sink_format()` 改为读共享头），
  拷贝不需要换格式。

### 4.3 复制路径

- 颜色需要 `COPY_SOURCE` 状态，COPY(DMA) 队列不能做 barrier → **颜色强制走
  `g_copy_q`（DIRECT）**；MV/depth 仍可走 DMA（当它们已在 COPY_SOURCE 时）。
  这是 producer 内部的选择，不新增协议字段；
- 与 MV/depth 在**同一个 copy list** 上、同一个 `fence_forward[s]` 发布：
  一条发布 = 三个流全部完成，consumer 只等一个 fence；
- 失败策略：颜色复制失败 → 整个发布仍然发出（MV/depth 可用），
  `active_mask` 不含 `kRequestColor`，consumer 本帧复用上一张颜色并**不推进 cadence 相位**
  （按 `reuse` 处理）。不 reset、不冻结。

### 4.4 背压与槽位回收

- consumer 在 Present 之后写 `colour_seq = frame_id`（已有）；
- producer 的 `slot_free(s)` 在信用窗口开启时已经要求 `consumed[s] >= published`；
  颜色入槽后，consumer 的 ACK 只在**颜色+MV+depth 的本地拷贝都退休**后写，
  因此现有信用窗口天然覆盖颜色；
- 颜色落后时（GPU 慢），consumer 跳过旧发布（取最旧未处理者，见 §5.2），
  producer 的 overrun 计数会上升——这是唯一需要观察的背压指标。

### 4.5 成本与可调项

| 流 | 4K 字节/帧 | 说明 |
|---|---|---|
| colour | 66.4 MB（FP16）/ 33.2 MB（R10） | 原样复制；**约 6% 渲染卡** |
| mv | 14.7 MB（150% → 2560×1440 RG16F） | 可 1K（200%）后续试 |
| depth | 14.7 MB（150% → 2560×1440 R32） | 同上 |
| 元数据 | < 4 KB | 可忽略 |

新增 ini 开关（默认全开，允许 A/B 与回滚）：

```
ColourStream=1        # 0 = 不发布颜色（只用于测量 producer 成本）
ColourPercent=100     # 预留：颜色降采样（本版不实现，写 100）
```

> 用户已确认 15% 的总预算；本版不引入颜色降采样，避免画质变量与成本变量混在一起。

---

## 5. Consumer 简化：删什么、留什么

### 5.1 删除（WGC 及其相关机制）

| 位置 | 内容 | 处理 |
|---|---|---|
| `devices_capture_fg.cpp` | WGC item / session / frame pool / `FrameArrived` / `MinUpdateInterval` / `adopt_capture_size` | **删除**；尺寸改由 stream 0（或 producer.output）决定 |
| `main.cpp` | `ColourItem`、`g_colour_q`、`colour_push/pop`、`colour_capture_thread`、`start_colour_capture`、`start_wgc` 调用、`--pool/--wgc-minint/--colour-queue/--legacy-poll/--slack*/--jbuf` | **删除** |
| `frame.cpp` | marker 解码、`pair_slot_for_wgc` 状态机、present-ring 查询、WGC 时间戳、colour stall guard、keyed mutex、`colour11` 参数 | **删除**（改由 publication 槽身份） |
| `frame_io.cpp` | pub FIFO / prefetch / copy-on-publication / 颜色队列计数 / `colour_seq` 的 WGC 分支 | 保留 ACK 与输入拷贝；**删除** WGC 专用的排队与预拷 |
| `input_cache.cpp` | 16 深 local ring（为颜色滞后准备） | 缩为 1 帧缓存（颜色不再滞后）；ACK 即时 |
| `cadence.cpp` | `open_present_ring()` | 删除 |
| `state_overlay.cpp` | `g_wgc_ts_ms`、`g_pool`、`g_frame_event`、sink 轮转、mutex | 删除 |
| `faker_fg` | `pass_marker` 四角绘制、`present_ring_start/begin/commit` | 删除调用（shader/PSO 一并删） |
| `src/mgpu_present_ring.h` | 整个文件 | 删除 |
| `tests/wgc_probe/` | 整个目录 | 删除 |
| 文档 | README/USAGE/COMPONENT_*/TOTAL_ARCHITECTURE 里的 WGC 描述 | 改为颜色入槽 |

### 5.2 保留并成为主路径

- `run_frame_ex()` 的骨架：cadence → throttle → ACK → `fg_set_interp` → 输入拷贝 →
  convert → tag → Present → 统计；
- `wait_slot()` / `fence_forward[]` / `consumed[]` 信用窗口；
- StreamPercent、`--upsample`、`--stream-auto`、`--fg-of`、`--no-mv/--no-depth`；
- 颜色转换 shader：`mode 3` 已经是「源已经是 PQ」的直通；FP16 走 mode 0/1/2。
  **mode 由 stream 0 的格式自动决定**，`--fg-colour-mode` 可覆盖。

### 5.3 新的主循环（publication-driven）

```
last = 0
loop:
  pump messages / hotkeys
  pick = -1; best = UINT64_MAX
  for s in slots:
      pub = frames[s].published
      if pub > last and pub < best: best = pub; pick = s
  if pick < 0: Sleep(1); continue
  last = best
  wait_slot(pick)                 # 同步：三个流都到齐
  run_frame(pick)                 # 只处理这一条发布
```

- **取最旧未处理**而不是最新：跳过发生在消费者跟不上时，且按发布顺序，
  动态逐帧看到的是真实的 per-publication 间隔；
- 一条发布只处理一次（`last` 单调），不需要 marker、不需要去重；
- 发布停摆（菜单/加载）→ 没有 `pick`，循环空转；`pub_stall_frames` 让 MV 门控
  在 8 帧后关 FG，与现状一致。

---

## 6. 动态逐帧能否直接适配（分析结论）

### 6.1 变好的部分

| 现状（WGC） | 颜色入槽后 |
|---|---|
| 输入间隔 = WGC 到达间隔（被 DWM 量化 + 我们的停顿污染） | 输入间隔 = `frames[s].frame_time_ms`（producer 每呈现帧一次的 EMA），**本来就是决策用的那个数** |
| 颜色丢帧 → 配对缺口 → reset | 没有颜色队列；只有「发布被跳过」，按最旧未处理处理，不 reset |
| `arr/pin/pq` 三套速率互相打架 | 只剩 `pin`（per-publication），`arr` 成为诊断量（我们的处理间隔），不再参与决策 |
| 颜色滞后 1 帧，配对要猜 | 同一槽，`lag = 0` |

### 6.2 不变的部分（仍然必须遵守）

动态逐帧的核心约束**没有变**：`sum(k_i) = 总 vblank 数`，而 `sum(T_in)` 是游戏给的，
所以 `k` 的长期均值被钉死为 `R/f`。

| 输入/面板 | 结果 | 结论 |
|---|---|---|
| 40 @120 | 3.000 | locked 完美（间距 0） |
| 43 @120 | 2.79 | 三选一：carry（换间距）/ locked（丢输入）/ VRR（不可用） |
| 35 @120 | 3.43 | 同上，偏 carry |
| 60 @120 | 2.000 | 2x 原生模型，最顺 |

**颜色入槽不会消除 7% 的非整除失真**，但它消除了「把失真归因错」的可能：
以前 `arr` 的抖动会把 7% 放大成 10–12%，现在 `T` 干净，控制器只面对算术本身。

### 6.3 可直接复用的策略

- `--cad-policy locked`：`k = ceil(target/f)` + 3% 死区 + 3 帧共识 + `k×vblank` 预算；
- `--cad-policy fill`：相位累加器（carry），用于输入波动大、允许换间距的场景；
- 输入时钟固定为 `producer`（本版没有第二选择，`--cad-input arrival` 删除）；
- `vblank` 仍用 DWM `qpcRefreshPeriod`（与颜色来源无关）。

### 6.4 需要重新测量的一格

颜色同槽后，**consumer 的每帧工作量变了**：
`66 MB 跨卡读 + copy` 取代 `66 MB sink 拷贝 + DWM 合成延迟`。
`total p95` 与 `late/phase` 需要重测，尤其是 4K FP16 颜色（66 MB）。
若 consumer 侧成为瓶颈（`colour=` 或 `copy=` 上升、`qdrop` 消失但 `show` 下降），
第一顺位是**颜色降采样**（`ColourPercent`），不是回到 WGC。


---

## 7. 逐文件改动清单（执行顺序）

| # | 文件 | 改动 | 验收 |
|---|---|---|---|
| 1 | `src/mgpu_shared.h` | v22；`kMaxSlots=4`；`kStreamColour`；scar 注释；`kRequestColor` 语义 | 双方版本一致才通信 |
| 2 | `src/xess_mfg_producer/producer.cpp` | 布局/放置 stream 0；`on_record` 取颜色并按帧复制；`use_dma` 兼容；`active_mask/stream_bytes[0]`；`ColourStream=0` 关闭 | 日志 `colour: 3840x2160 fmt 10`、`publish ... colour=1` |
| 3 | `src/xess_mfg_consumer/modules/main.cpp` | 删除 WGC/队列/线程；默认颜色入槽；slot-driven 主循环（最旧未处理）；参数清理 | 启动日志不再出现 WGC；`wait_slot` 后每发布一帧 |
| 4 | `devices_capture_fg.cpp` | 删除 WGC 段；尺寸从 producer.output / stream 0 取 | 不再引用 WinRT Capture |
| 5 | `state_overlay.cpp` | 删除 WGC 状态与 sink 轮转；`colour_sink_format()` 读 stream 0 | 无 `g_pool`/`g_wgc_ts_ms` 引用 |
| 6 | `frame.cpp` | 槽身份替代 marker/ring；颜色直接来自 `g_colour[slot]`；convert mode 自动；删除 stall guard/mutex；ACK 精确 | `pair:` 行只留 `slot=`；无 reset 归因 |
| 7 | `frame_io.cpp` / `input_cache.cpp` | 删除 pub FIFO/prefetch；`copy_inputs_local` 同时拷颜色；ACK 即时 | `pf:` 行删除；`colour=` 稳定 |
| 8 | `cadence.cpp` | 删除 present-ring；速率只读 `frame_time_ms` | `pin` = 发布间隔 |
| 9 | `faker_fg/*` | 删除 marker 绘制与 present-ring 写 | 游戏侧日志不再有 `marker draw` |
| 10 | 删除 `src/mgpu_present_ring.h`、`tests/wgc_probe/` | — | `grep -ri wgc src tests` 为空 |
| 11 | 文档 + README/USAGE/CHANGELOG | 改为颜色入槽；保留历史结论 | 索引一致 |

---

## 8. 验收矩阵

### 8.1 功能

| # | 场景 | 通过标准 |
|---|---|---|
| 1 | 4K HDR，锁 40，locked | `show ≥ 119`、`presents/vblanks ≥ 99.5%`、`HITCH ≤ 1`、无 reset |
| 2 | 4K HDR，不锁 43，fill/carry | `pin` 稳定 ≈23.2 ms；`k` 只在 3/2 间按策略切换；无 reset |
| 3 | 2x@60 | `show ≥ 117`、间距误差 < 3% |
| 4 | 菜单/加载（无发布） | 1X，不冻结，恢复后 FG 自动开 |
| 5 | 分辨率/超分档变化 | `generation` 变化 → 本地拷贝重建；不黑屏、不重启 |
| 6 | `ColourStream=0`（A/B） | 颜色不发布，consumer 复用上一帧；用于测量 producer 成本 |

### 8.2 指标（必须出现在日志里）

- producer：`colour: WxH fmt F`、`publish #N ... colour=1`、`colour copy ...ms`；
- consumer：`colour=`（跨卡读+拷贝）、`total p95`、`vphase`、`pin`、`show`、
  `presents/vblanks`、`HITCH`；
- 删除的指标：`arr` 不再参与决策、`qdrop`/`cskip`/`pair:`/`marker:` 不再存在。

### 8.3 静态检查

```bash
grep -ri 'wgc\|GraphicsCapture\|CaptureFramePool\|present_ring\|marker' src tests
bash tests/cadence_core/run.sh          # 24/24，控制律不变
bash tests/pair_core/run.sh             # 改为槽身份仿真（可选）
```

---

## 9. 风险与回滚

| 风险 | 概率 | 处理 |
|---|---|---|
| 4K FP16 颜色 66 MB 跨卡读让 consumer `total` 上升 | 中 | 先测 `ColourPercent=200`（1/2，33 MB）；再决定是否默认 |
| 渲染卡 +6% 使游戏掉帧（PRAGMATA 这类 97% 占用的题） | 中 | `ColourStream=0` 一键关闭；UI 暴露 |
| 颜色源选错（BACKBUFFER 不含 UI / 含 UI 重复） | 低 | 记录优先级 + 日志格式；必要时切 OUTPUT_REAL/HUDLESS |
| 槽位变 4 后高负载下 overrun | 低 | 看 `producer_overrun`；可回 6/8（内存换余量） |
| 旧包/旧 exe 混用 | 中 | 版本硬拒绝；发布包四件一起换 |

**回滚**：保留旧产物 zip；协议版本不同即拒绝，不会“半工作”。
代码层回滚点：本次改动前的 `xess_mfg_consumer.exe` / `xess_mfg_producer.dll`。

---

## 10. 执行顺序（本方案落地）

1. 协议 v22 + producer 颜色流（独立可验证：producer 日志出现 colour 复制）；
2. consumer 删除 WGC、改为 publication-driven；
3. 文档与测试清理；
4. 实机按 §8.1 跑 1→6，先 4K/锁 40，再 43 fps 动态；
5. 若 4K FP16 成本超预算，再做 `ColourPercent` 降采样（不在本版）。

> 与历史文档的关系：`DYNAMIC_CADENCE_ANY_CAP.md`、`PAIRING_100.md`、

---

## 11. 本版落地状态（代码锚点）

| 项 | 位置 | 状态 |
|---|---|---|
| 协议 v22：`kMaxSlots=4`、`kStreamColour/Mv/Depth`、`kInputFlagColourActive` | `src/mgpu_shared.h` | ✅ |
| producer 布局/放置 stream 0（输出尺寸、源格式） | `producer.cpp` `layout_streams()` / `ensure_shared()` / `place_slots()` | ✅ |
| producer 颜色复制 + 同一 fence 发布 + `ColourStream=0` | `producer.cpp` `on_record()` / `copy_stream()` / `publish_frame()` | ✅ |
| producer 颜色源优先级（Backbuffer → OutputReal → HUDLess） | `producer.cpp` `colour_source()` | ✅ |
| consumer 默认颜色入槽、请求颜色位 | `main.cpp`（open_shared 后）、`frame.cpp`（request_mask） | ✅ |
| publication-driven 主循环（最旧未处理 + 隐藏覆盖窗口） | `main.cpp` 主循环 | ✅ |
| 槽身份替代 marker/ring；输入时钟 = `frame_time_ms` | `frame.cpp` `run_frame_ex()` | ✅ |
| 颜色+MV+depth 一次拷贝（copy queue + 等待在 list 之前） | `input_cache.cpp` `copy_publication_inputs()`、`frame.cpp` | ✅ |
| ACK = 下一帧 `gpu_throttle()` 之后写 `consumed[]` | `input_cache.cpp` `ack_publication_previous/record()` | ✅ |
| convert mode 由 stream 0 格式自动决定；sink 格式 = stream 0 | `cadence.cpp`（open_shared）、`frame_io.cpp` `colour_sink_format()` | ✅ |
| WGC 设备/会话/帧池/线程/队列删除 | `devices_capture_fg.cpp`、`main.cpp`、`consumer_internal.h` | ✅ |
| present-ring 删除（fakerFG 不再写，consumer 不再读） | `faker_fg.cpp`、`fg_swapchain_hooks.cpp`、`backend_xefg.cpp`、`src/mgpu_present_ring.h`（删除） | ✅ |
| marker 绘制/解码调用删除 | `backend_xefg.cpp`、`frame.cpp` | ✅（函数体保留，已无调用） |
| `tests/wgc_probe/`、`scripts/recipes/run_diag_wgc.bat` 删除 | — | ✅ |
| 4K FP16 颜色成本实测、`ColourPercent` | — | ⏳ 需实机（本机无 Windows 工具链） |

### 11.1 未完成 / 需实机确认

1. **构建**：本工作区是 Linux，`cl.exe/fxc.exe` 在 Windows 侧；本次未能在本机编译。
   需要执行 `bash scripts/build_producer.sh`、`build_consumer.sh`、`build_faker_fg.sh`，
   然后用 `--list-modes` 冒烟。
2. **实机 A/B**：按 §8.1 跑 4K/锁40、43 fps 动态、2x@60；记录 `show / presents/vblanks /
   colour / total p95 / HITCH / vphase`。
3. **颜色源确认**：如果 BACKBUFFER 不含 UI（画面缺 HUD）→ 切 `OUTPUT_REAL`；
   如果含 UI 且 XeFG 插值把 HUD 拖影 → 需要启用 UI 流（stream 3），这是下一步。
4. **颜色尺寸变化**：sink 目前按启动时的 stream 0 尺寸创建；窗口/超分档变化后只复制
   重叠区域并记录日志。要完整支持需要重建 sink + swap chain（与 `generation` 同路径）。
5. **颜色降采样**：`ColourPercent` 只在 ini 里预留，未实现；先用实测数据决定。

> `TOTAL_ARCHITECTURE_AND_DATAFLOW.md` 里关于 WGC/marker/present-ring 的部分
> 在本版后**只作历史记录**；理论结论仍以 `DYNAMIC_MF_ANALYSIS.md` §6 为准。

