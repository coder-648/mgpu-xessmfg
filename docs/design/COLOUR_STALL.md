# Colour sink 停顿：先止血、再根治（2026-11-15）

## 1. 现象

基线（固定 3x / 锁 40）一次运行里出现 3 次 250 ms 级卡顿，日志总是这三行连着：

```
present-ring: seq=2822 not in slots (frame_ids 2824/2825/2826/2827)
SLOW frame: total=249.0ms decode=0.0 layout=0.0 colour=209.3 inputs=0.3 present=39.4
HITCH gap=249.2ms gpu=33.3ms pres=11.3ms wait=0.0ms expect=25.0ms xN=3
reset-history: forced for frame seq=2825 (pair-gap 1, colour-drop 13, ...)
```

三次的 `colour=` 分别是 **209.3 / 222.4 / 207.6 ms**，而同一帧的 `inputs` 只有 0.3 ms。
同一轮里 8 次 reset，只有 1–3 次伴随这种卡顿；固定 4x 那轮 8 次 reset 只有 1 次卡顿。

## 2. 因果链（reset 是症状，不是原因）

```
GPU 侧依赖（上一批转换/插值还在读 sink，或 D3D11 上下文排队）
        │
        ▼
CopyResource 阻塞 207–222 ms          ← 真正的停顿
        │
        ▼
整个 run_frame() 停 250 ms            ← HITCH gap
        │
        ▼
WGC 帧在 2 深队列里溢出 → 丢颜色帧     ← colour-drop
        │
        ├─→ 处理下一帧时 present-ring 已对不上 → pair-gap
        └─→ 旧逻辑：丢帧就 forced resetHistory
```

所以 **“每次 reset 导致 250 ms” 不成立**：reset 是丢掉颜色帧之后的补救动作。
把 reset 当成原因去优化，只会去掉症状、留下 250 ms。

## 3. 为什么必须把它和动态逐帧隔离开

250 ms 停顿与“k 选得对不对”无关，但它会制造两种污染：

1. **输入估计被污染**：停顿期间没有帧到达，之后 WGC 一次性吐几帧（追赶突发）。
   旧路径用 WGC 到达间隔算输入速率，于是 T 在 23/25/33 ms 之间跳，
   k 被推着在 2/3/4 之间抖 —— 动态路径替系统停顿背锅。
2. **history 被反复丢弃**：每次 reset 让 XeFG 少 1–2 张插值帧，画面抖一下。

## 4. 策略（三层）

### L1 止血：不再让拷贝阻塞整个循环（已实现，`frame.cpp`）

拷贝前先看三个先兆，任一成立就跳过这一帧的拷贝（复用上一次写过的 sink，
代价是一帧重复颜色 ≈ 1 个 vblank 的轻微顿挫，而不是 250 ms 冻结）：

| 先兆 | 含义 |
|---|---|
| `g_last_colour_ms > 40` | 上一次拷贝本身就阻塞过，流水线在补欠账 |
| D3D11 侧 ≥2 张拷贝未完成 | GPU 落后，这次几乎必然阻塞 |
| D3D12 侧 3 帧都没退 | 旧判据，保留 |

连续跳过上限 4 帧，GPU 持续慢时仍然会给新颜色。

**同时修正一个顺序 bug**：原来先换 sink 再判定跳过，于是“复用帧”会读到
2–3 帧前的旧 sink（画面回跳）。现在先判定、再换 sink，复用帧读的就是上一次写的那张。

### L1c keyed mutex 超时不再硬拷（已实现）

`AcquireSync(0, 20)` 的返回值此前被忽略：没拿到锁仍然 `CopyResource`，
driver 只能把这次写排在读者之后 —— 这是 200 ms 级阻塞的另一个来源。
现在拿不到锁就复用上一帧并计数（`colour: keyed mutex busy ...`）。


### L1b 不再因颜色丢帧强制 reset（已实现，`main.cpp`）

丢的是“颜色”这一个流，不是配对身份：下一帧仍按 present-ring 的 seq 精确配对
MV/depth，配对失败会在 `frame.cpp` 自己 reset。所以颜色队列溢出不再写
`g_force_reset_history`，只计数（`reset-history ... colour-drop N`）。

### L2 隔离：cadence 用游戏自己的时钟（已实现，`frame.cpp`）

`--cad-input producer`（默认）让动态逐帧用 `FrameShared.frame_time_ms`（游戏自己的
present 间隔）判断输入速率，而不是我们的 WGC 到达间隔。我们的拷贝停顿、丢帧、
追赶突发都不再改变 k。`--cad-input arrival` 保留旧行为做 A/B。

### L3 根治：传输性能（后续，按性价比排序）

| # | 方案 | 收益 | 风险/成本 |
|---|---|---|---|
| 1 | **去掉 sink 拷贝**：持有 WGC 帧（Close 前不放）并以共享资源直接在 D3D12 读 | 省掉 4K 66 MB/帧的 CopyResource，停顿源直接消失 | WGC 池被占满会反压捕获；要验证 `--pool` 与 Close 时机 |
| 2 | 拷贝改走 **D3D12 copy queue**，与 FG 提交解耦，fence 门控 | 拷贝不再阻塞 CPU 线程 | 跨设备/共享纹理的 fence 语义要重做 |
| 3 | **降采样再上采样**：捕获侧先降到 1440p/1080p，消费侧再放大（已有 `--den/--scale`） | 拷贝量降 55–75% | 画质；需实测 |
| 4 | 捕获侧直接转 **R10G10B10A2**，省掉 FP16→PQ 的中间拷贝 | 少一次全帧写 | 色彩管线要改 |
| 5 | XeFG 与 colour copy 分属不同队列/优先级 | 减少互相等待 | 依赖库内部实现，不可控 |

验收指标（L1 上线后应看到）：`cskip` 只在尖峰出现（<1 帧/窗口）、
`colour` 均值 < 8 ms 且 max < 40 ms、`HITCH(>150ms)=0`、`reset-history` 只剩 pair-gap。

## 5. 观测字段

| 字段 | 位置 | 含义 |
|---|---|---|
| `cskip=N` | breakdown 行 | 本窗口因“拷贝会阻塞”而复用旧颜色的帧数 |
| `colour: <原因> - reusing the last frame ... (streak u, skipped u so far)` | 事件日志 | 每次跳过的原因与累计次数 |
| `colour=NN.Nms` / `worst(... colour X ...)` | breakdown 行 | EMA / 本窗口最大拷贝耗时 |
| `reset-history: ... (pair-gap a, colour-drop b, producer c)` | 事件日志 | 三类归因（colour-drop 现在不再触发 reset） |

## 6. 判断标准（下一轮）

- 若 `cskip` 有值且 HITCH>150 ms 消失 → L1 有效，L3 可以按“性价比”慢慢做；
- 若 `cskip=0` 却仍有 250 ms → 先兆条件没覆盖到（把 `kColourStallMs` 降到 20 ms 再试）；
- 若 `colour` 均值仍 > 30 ms → 直接上 L3-1/L3-3，别再调阈值。
