# M1 事件循环 A/B：bridge A5 vs xess_mfg_consumer（2026-11-14）

同一场景、同一组 A5 参数：

```
--den 2 --pool 4 --interp 3 --unlock-mfg --interp-cadence
--fg-rt-mode auto --fg-ui none --pub-fifo --report 40
```

CP2077，窗口 3840x2160，面板 3840x2160@120 HDR；生产端 RTX 3080 Ti，消费端
RX 6750 GRE。两次日志都提示 `present size 1920x1080 != window client 3840x2160`
（`--den 2` 的既有行为，两边一致，不是 M1 引入的）。

## 0. 本次是同一游戏会话

- 生产端 pid 4500，A5 先跑，V2 紧接着跑（不重启游戏）；
- 原始日志：
  - `bridge_A5_2026-11-14_samesession.log`（35 个 report 窗口）
  - `bridge_V2_m1_2026-11-14_samesession.log`（36 个窗口）
- 注意：A5 在 attach 时 `shared block flags=0x0`，V2 attach 时已是 `0x2051`；
  同一会话里后跑的进程看到的是更新后的 producer 状态。

## 1. 每窗口中位数

| 指标 | A5 bridge | M1 consumer | 变化 |
|---|---:|---:|---:|
| `total` | 24.60 ms | 24.55 ms | -0.05（持平） |
| `gpu` | 17.50 ms | 12.45 ms | **-5.05** |
| `present` | 6.60 ms | 10.25 ms | **+3.65** |
| `age` | 9.50 ms | 7.80 ms | **-1.70** |
| `load` | 94.0% | 95.5% | +1.5 |
| `skip`（中位/均值） | 0 / 0.43 | 0 / 0.44 | 一致 |
| `cq` | 0.00 ms | 0.00 ms | 无变化 |
| `xN` | 3.00 | 3.00 | 一致 |
| `mvs` | 0.50 ms | 0.65 ms | +0.15 |

`gpu` 与 `present` 仍是跷跷板，这次方向与上一轮相反（A5 偏 GPU、V2 偏 present），
`total` 完全相同。按既定口径只看 `total`/`load`/`skip`：M1 与 A5 持平，没有回归。
单次运行的 `gpu`/`present` 分配会随运行顺序/时钟/场景变化，不能单独下结论。

## 2. 输入/输出质量

| 指标 | A5 | M1 |
|---|---:|---:|
| `pf` 命中 | 1344 / 35 窗口（中位 40） | 1396 / 36 窗口（中位 40） |
| `pf` miss | 17（1.3%） | **5（0.36%）** |
| `pf` overflow | 0 | 1 |
| `arr` 3x | 1257/1374 = **91.5%** | 1247/1415 = **88.1%** |
| `OUTPUT FRAMES MISSING` | 2/35 | 2/36 |
| `VBLANK MISSED` | 0 | 0 |
| `xNmiss` | 0 | 0 |

本次 M1 的 `pf` miss 更低（0.36% vs 1.3%），但 3x 比例低 3.4 个百分点。上一轮跨会话
对比时方向相反（V2 91.5% vs A5 85.1%），所以 **3x 的差异目前不能归因于 M1**，更像
场景/运行顺序/到达抖动。需要 A/B/A 或多次采样才能判断。

## 3. 事件循环与 CPU

`proc:` 每 1 s 窗口：

| 指标 | A5 | M1 |
|---|---:|---:|
| `proc` ms/frame 中位 | **6.64** | **7.03** |
| 单核占比中位 | **26.6%** | **28.1%** |
| `wakes/s` 中位 | —（旧轮询） | 76 |
| 其中 `frame` / `pub` | — | 16 / 9 |
| `arm` | — | **0.38 ms/s（40 次/窗口，约 9 µs/次）** |

- `wakes/s` 中位 76，确认旧的 ~1000 次/s 空转轮询消失；`frame`/`pub` 只是等待返回时
  命中的事件，队列里有帧时会跳过 wait。
- **`arm` 只有 0.3–0.8 ms/s，约 9 µs/次**：跨适配器 `SetEventOnCompletion` 很便宜，
  不是 CPU 差异来源；不需要做自适应关闭。
- CPU 仍是 V2 高 **+0.39 ms/frame（+5.9%，+1.5 个百分点单核）**。这个量级可能来自
  WGC `FrameArrived` 的 COM 回调线程 + 消息唤醒路径（旧 bridge 是 1 ms 轮询），也可能
  有运行顺序/场景差异；`arm` 已排除。M1 不是零成本，但相对 25 ms 预算约 1.6%。

## 4. 结论

- M1 功能完整：`FrameArrived` 和 publication fence 都注册成功，事件循环稳定，
  `total` 与 A5 持平，`load`/`skip` 无回归，`age` 低 1.7 ms；
- `arm` 证明 publication fence 本身很便宜，可以保留；
- CPU 还剩 +0.39 ms/frame 的小差距，暂不阻塞 M2；
- 3x 比例在两次运行中方向相反，不能作为 M1 的稳定收益或回归。

## 5. 下一步

> M2 已实现（capture 线程 + colour 队列，`--no-capture-thread` 可回到 M1）：
> 用 `mgpu_test_V2_m1.bat`（单线程）与 `mgpu_test_V2_m2.bat`（capture 线程）在同一
> 游戏会话里各跑 60 s，先看 `present`/`OUTPUT FRAMES MISSING`/`arr`/`qdrop`。

1. **同会话 M1→M2**：确认 capture 线程没有引入黑屏/崩溃，并对比
   `present`、`OUTPUT FRAMES MISSING`、`arr` 3x、`total`、`qdrop`。
2. **producer credit window**：`Cyberpunk2077_mfg_producer.log` 里出现
   `credit window OFF (consumer silent for 30 frames)` 和
   `no ACKed slot - overwriting the oldest`。ACK 在 `run_frame()` 后发送，presenter
   阻塞时生产端会覆盖槽位；M3 的 input 线程应把 ACK 从 present 路径剥离。
3. **`pub` 唤醒率**：M3 的 publication 队列/序号 matcher 之前，先确认
   `SetEventOnCompletion` 是否真的按 publication 触发；如果始终被合并，就保留
   `prefetch_publications()` 作为主路径，不把它当性能来源。
4. **`OUTPUT FRAMES MISSING`**：一次 `expected=160` 说明 cadence 在 4x/3x 之间切换，
   属于已知的 cadence+MV flicker/ghosting 问题，留给 M4。

## 6. 历史运行（仅参考）

- `bridge_A5_2026-11-14_noproc.log`：更早的旧 exe A5，没有 `proc:`。
- `bridge_A5_2026-11-14_proc.log`：03:50 的 A5（带 `proc:`），与 V2 不是同一游戏会话，
  producer `flags`/`mvs` 路径不同，只用于确认 `proc:` 字段和趋势。
