# 双卡补帧：当前全局链路（PIPELINE v3，2026-11-14）

> ⚠️ **已有一处被后续工作取代**：本文仍把"WGC 时间戳查表（按 frame_qpc_ms）"当配对主键；
> 现在的主键是**游戏侧四角 marker**（`frame_id` 精确匹配），时间戳/present-ring 只作兜底。
> 最新链路与实测见 `docs/findings/PERF_ROUND_HANDOVER.md`；本文的 DMA/预拷/自动档位
> 和优化清单仍然有效。

> 取代 `PIPELINE_V2.md`（那份是 producer 重写前的）。本文描述**现在运行的代码**：
> producer 的 DMA 拷贝、consumer 的输入预拷、WGC 在消费卡、自动档位、可选升采样。
> 实测口径：CP2077 / 4K / 锁 40 fps / 3x / `StreamPercent=150`（A 组）。

## 1. 全局图

```text
┌────────────────── 渲染卡 (RTX 3080 Ti, 3D 占用 90%) ──────────────────┐
│ 游戏进程                                                              │
│  ├─ 每帧渲染（DLSS-SR 输出 MV/depth）                                  │
│  ├─ fakerFG Evaluate：harvest → MgpuFgRecord（mv/depth/相机/jitter）   │
│  │    └─ xess_mfg_producer.on_record()                                │
│  │         · StreamPercent 降采样（游戏队列，~0.1–0.3 ms）             │
│  │         · barrier + CopyTextureRegion 录进第二队列                   │
│  ├─ Present：fakerFG 写 FrameShared（frame_qpc_ms/frame_key）          │
│  │            → 游戏队列 Signal(frameFence)                            │
│  └─ 第二队列：Wait(frameFence) → 拷贝 MV/depth → Signal(fwd[slot])     │
│       · 源在 COPY_SOURCE（CP2077 0x800）→ **DMA/COPY 引擎**（不占 3D）  │
│       · 否则 → 第二条 DIRECT 队列（可发 barrier）                       │
│       · 拷贝量 28.1 MB/帧 @150（63.3 MB @100），1.1–2.5 GB/s            │
└───────────────────────────────────────────────────────────────────────┘
             │ 共享堆：命名 file mapping + ALLOW_CROSS_ADAPTER
             │ slot[4] × {mv, depth}；fwd[4] / consumed[4] / generation
             ▼
┌────────────── 消费卡 (RX 6750 GRE，WGC + 显示器都在这里) ──────────────┐
│ WGC 抓游戏窗口 → D3D11 FP16 → 共享纹理 → D3D12 颜色                    │
│   与下面的预拷**并行**；age(合成器→取到) 5.7–8.6 ms                     │
│                                                                       │
│ bridge 每帧（WGC 到达驱动）：                                          │
│   1. 预拷（上一帧 Present 之后已发起，见 §2）                           │
│        copy 队列 Wait(fwd) → 跨卡读 28.1 MB → 本地显存 ring            │
│   2. WGC 颜色到达 → pair_slot_for_wgc 查表（WGC 时间戳 vs frame_qpc_ms）│
│        命中 → 直接用本地已拷好的 MV/depth（pf hits）                    │
│        未命中 → 本帧补拷（pf misses；upsample 时留在 direct list）      │
│   3. 可选 --upsample：depth 引导 MV 重建到渲染尺寸（0.3–0.5 ms）        │
│   4. tag MV/depth/colour → XeFG Dispatch → Present（120 Hz）           │
│   5. ACK consumed[slot]；gpu_throttle 队列深度 1（不叠加延迟）          │
└───────────────────────────────────────────────────────────────────────┘
             │ 覆盖窗口 Present 到消费卡驱动的显示器
             ▼
          120 Hz 面板；scanout 0…8.3 ms
```

## 2. 一帧的时间线（A 组实测，4K/40，150 + 预拷）

```text
t0  游戏渲染这一帧                                    game 24.2–25.0 ms（锁 40）
t1  Evaluate：harvest + 降采样 + 录拷贝               CPU ~0.05 ms
t2  Present：写 FrameShared，Signal(frameFence)
    └ 第二队列：等 fence → DMA 拷 28.1 MB → Signal(fwd)   1.2–2.2 ms（present 之后）
t3  consumer 上一帧 Present 之后已在 copy 队列预拷下一槽（等 fwd，GPU 侧）
    CPU 同时阻塞在 TryGetNextFrame → 跨卡读与 WGC 并行
t4  WGC 交付（age 5.7–8.6 ms）→ 查表 → 命中直接用本地副本
t5  gpu_throttle + 颜色 hop + tag + XeFG     gpu 16.9–20.9 + present 2.3–4.4
t6  Present → 120 Hz → ACK
```

实测汇总（A 组 52 个窗口）：

| 项 | 数值 |
|---|---|
| `bus` | 28.1 MB/frame（1.1–1.2 GB/s） |
| `gpu`（XeFG） | 16.9–20.9 ms |
| `present` | 2.3–4.4 ms |
| `total` | **24.0–24.4 ms**（`load 87–96%`） |
| `skip` / 空 vblank | 1–4 / 2–12（40 帧窗口） |
| `pf` 命中 | 32–37 / 每窗 3–8 miss |
| 延迟 | compose→photons **30–41 ms**；click→photons ≥55–66 ms |
| 渲染卡 3D 占用 | **90%**（DMA 生效，`list type 3`） |

## 3. 同步点（谁等谁）

| 同步点 | 位置 | 现在 |
|---|---|---|
| `fwd` fence | producer 第二队列拷贝完成后 Signal | 消费端预拷在 copy 队列上 **GPU 侧 Wait**，CPU 不等 |
| slot 配对 | `pair_slot_for_wgc` | WGC 时间戳查表；命中不拷，miss 才补 |
| `gpu_throttle` | consumer 每帧等上一帧 GPU | 深度 1；`gpu` 即 XeFG 时间，不额外加延迟 |
| ACK | `ack_previous_slot` | 消费端 GPU 完成后退还 credit，无往返 |
| 自动档位 | 每统计窗口 | `gpu+present` 反馈 + age/带宽，2/4 窗确认 + 2 s 驻留 |

## 4. 还能优化什么（按实测收益/风险排序）

| # | 点 | 实测/预期 | 代价/风险 |
|---|---|---|---|
| 1 | **预拷 miss 3–8/40**：用“producer 最新发布槽”替代轮转 `next_slot`；copy 队列双缓冲，避免 miss 时 `input_copy_begin` 等上一份拷贝（日志里偶发 `cq 4–9 ms`） | 命中率 85–92% → ~98%；省 0.3–0.7 ms，消除尖峰 | 低（只改 prefetch 选槽与分配器数量） |
| 2 | **XeFG 4K 3x（16.9–20.9 ms）** | 闭源；`--scale 0.75` 时 `gpu 14.45`、`load 91%`、`skip 0`（F 组）；`--den 2` 时 `gpu 11.0`、`load 94.5%`（E 组） | 画质；不是代码问题 |
| 3 | **WGC/DWM 的 age 与 2x/4x 抖动** | `--capture-monitor` / `--overlay-hidden` A/B；当前 `arr(vb)` 2x=1–9、4x=2–7 | 低；`age` 5.7–8.6 ms 已比之前好 |
| 4 | **cadence 4x 尖峰**（`xN 3.0–3.12`） | 40 fps 下偶尔 4-vblank 间隔会要 4 帧；`--interp 2`（kmax 3）可减尖峰但空 vblank 变多 | A/B，不改默认 |
| 5 | **升采样 pass 0.3–0.5 ms** | 预拷后 A 的 `gpu+present` 23.9，加 0.45 仍 ≤25；B 组可测 | 画质取舍 |
| 6 | **生产端降采样 pass（0.1–0.3 ms）仍在游戏队列** | 可挪到第二条队列（present 后），收益很小 | 中；不建议 |
| 7 | **颜色走 producer（去掉 WGC）** | 渲染卡 +10–15%（历史 `Color=1` 80→95%），且当前 record 是 untagged 没有颜色 | 否决（见 PLAN §7.5） |
| 8 | **延迟结构 `2·T_in + age`** | 30–41 ms compose→photons；同 120 fps 下取最小倍数或提输入帧率 | 产品选择，代码无解 |

**已经确认不值得做的**（别回头）：加槽位、给发布加锁/队列、消费端轮询生产端、
`--jbuf` / `--slack` / `--pace` / `--lock`、生产端同队列拷贝、颜色走 producer。
