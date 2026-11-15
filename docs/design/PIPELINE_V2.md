# 生产端 → 消费端：整链路与同步（2026-11-13，含实测）

> ⚠️ **历史文档**：本文写于协议 v14 / bridge 时代，已被
> [`PIPELINE_V3.md`](PIPELINE_V3.md)、[`CONSUMER_ARCH.md`](CONSUMER_ARCH.md) 与
> [`../findings/PERF_ROUND_HANDOVER.md`](../findings/PERF_ROUND_HANDOVER.md) 取代。
> 仅保留“为什么这样同步”的历史推演。

> 本文描述**当前代码**（fakerFG + xess_mfg_producer + bridge，无 ReShade add-on）的完整流程、
> 两端的同步点，以及基于实测的性能优化顺序。协议：`src/mgpu_shared.h` v14。

## 1. 全局流程图

```
┌──────────────────────────── 游戏进程（渲染卡） ────────────────────────────┐
│  Cyberpunk2077.exe                                                          │
│                                                                             │
│  每帧：DLSS-SR Evaluate ──► fakerFG（proxy_nvngx 的 detour）                │
│        │   ├─ sr_harvest(params)         → MgpuFgFrame（MV/depth/jitter/…） │
│        │   ├─ TagAtEvaluate（local 模式才真正 tag）                          │
│        │   └─ publish_record()           → v2 sink（同步、list 仍打开）      │
│        │                                        │                           │
│        │                                        ▼                           │
│        │                         xess_mfg_producer.dll（同进程、同线程）     │
│        │                           1) request_mask 门控（没人要就不拷）       │
│        │                           2) claim_slot（credit；无空闲→覆盖最旧）   │
│        │                           3) StreamPercent>100：compute 缩 MV+depth │
│        │                           4) barrier + CopyTextureRegion → 跨卡槽位  │
│        │                                                                     │
│  Present ──► fakerFG（before_present / swapchain vtable hook）               │
│        ├─ local 模式：XeFG SetPresentId / 自己呈递 2X                       │
│        └─ producer_present(queue)：写 FrameShared 元数据 + Signal(fwd[k])    │
└─────────────────────────────────────────────────────────────────────────────┘
                                   │
                   跨卡 heap（命名 file mapping + plage 资源）
                   slot[4] × {mv, depth}，跨卡 fence fwd/back 各 4
                                   │
┌──────────────────────────── bridge.exe（副卡） ─────────────────────────────┐
│  WGC 抓游戏窗口 → D3D11 纹理 → 共享 NT 句柄 → D3D12 颜色                    │
│  每帧（一个 WGC 帧 = 一次 run_frame_ex）：                                   │
│    1) 用 WGC 时间戳与 frames[k].frame_qpc_ms 配对槽位（pair_slot_for_wgc）    │
│    2) copy_inputs_local(slot)：跨卡读 MV/depth → 本地 ring（copy 队列）       │
│    3) auto MV：producer flags 决定用游戏 MV / 光流 / 零矢量                   │
│    4) XeFG：tag(MV/depth/colour) → Dispatch → 生成 N 帧                      │
│    5) Present（DirectComposition 覆盖窗口，120Hz 节拍）                       │
│    6) ACK：consumed[k] = published[k]（除“下一帧要读的那格”）                 │
└─────────────────────────────────────────────────────────────────────────────┘
```

## 2. 一帧的时间线（consume 模式）

```text
t0  游戏开始渲染这一帧
t1  DLSS-SR Evaluate
      · fakerFG 记录输入（MV/depth 指针 + 声明状态 + jitter/相机/尺寸）
      · producer：claim slot → [compute 缩放] → barrier + 2×CopyTextureRegion（只录命令）
t2  Present
      · fakerFG 写 FrameShared（jitter/mvScale/matrices/frame_key/presented）
      · Signal(fwd[k])            ← 拷贝真正完成由 GPU 保证在这里之后
t3  WGC 交付这一帧的颜色（组合器时间戳 SystemRelativeTime）
t4  bridge：按时间戳配对到 slot k，拷 MV/depth 到本地 ring
t5  bridge：XeFG tag + 生成 + Present（120Hz，按 cadence 决定一张/两张）
t6  bridge：ACK consumed[k]
```

两段**结构性**延迟（任何同步机制都消不掉）：

| 段 | 来源 | 4K/21fps 实测 |
|---|---|---|
| 输入间隔 | 游戏 1X 的渲染时间（consume 下游戏不补帧） | 47.7 ms |
| WGC 交付 | 组合器在 present 之后才交帧，约 1 帧 | `game->wgc` 35.9 ms |

## 3. 同步点（谁等谁）

| 同步点 | 位置 | 行为 | 默认 |
|---|---|---|---|
| `consumed[]`（v10 credit） | 生产端 `claim_slot()` | 只写消费者已 ACK 的槽；**没有空闲时覆盖最旧的一格**并 `producer_overrun++` | 开 |
| `fwd` fence | 生产端 Present 后 `Signal` | 给"拷贝已完成"做 GPU 侧排序；消费端**主路径不等它** | 开 |
| slot 配对 | 消费端 `pair_slot_for_wgc` | 用 WGC 时间戳找 `frame_qpc_ms` 最接近的槽（dt ∈ [-2, 250] ms） | 开（`--no-pair-key` 关） |
| `fwd` fence 等待 | 消费端 `wait_slot()` | 只在 `--prefetch-inputs` 和 WARP 路径里等（超时 `--wait-ms`） | 关 |
| `gpu_throttle` | 消费端 | 每帧等自己的 present fence，保证不覆盖还在被 GPU 读的 ring | 开 |
| ACK 时机 | 消费端 `ack_previous_slot` | 在上一帧 GPU 工作确认完成后，把除"下一格"外所有 `published` 写回 | 开 |

**没有逐帧请求/应答**：ACK 只是信用额度，不产生往返。发布/订阅本身的开销是 560 B 快照 ×2 +
一次间接调用，实测 **22 ns/次**（MSVC /O2，2M 次）。

## 4. 数据面

| 流 | 来源 | 槽位格式 | 尺寸 |
|---|---|---|---|
| MV | 记录的 `stream[MV]`（或缩放后的副本） | 源格式（4K = R16G16_FLOAT） | `StreamPercent` 决定：100 → render；150 → 2/3；200 → 1/2 |
| depth | 同上 | `R32_TYPELESS`（源 D32/R32 家族） | 与 MV 相同 |
| colour | **不经生产端**，bridge 用 WGC | — | 输出分辨率 |
| 元数据 | `FrameShared`（jitter/mvScale/semantics/矩阵/frame_key/presented） | — | 每帧一次 |

## 5. 实测分解（CP2077 4K，RX 6750 生成）

```
4K 生成（默认）        gpu 26.7  present 20.0  load 98%   compose→photons 85.2 ms
4K + --den 2           gpu  2.1  present  5.1  load 23%   compose→photons 10.0–18.4 ms
```

结论：**瓶颈在副卡的生成分辨率**，不在同步。同局 `game->wgc` 35.9 ms、`bridge 47.3 ms`，
其中 bridge 的 47 ms = 拷贝 + XeFG 生成 + present，几乎全被 4K 生成吃掉。

## 6. 优化点（按实测收益排序）

| # | 优化 | 作用点 | 实测/预期 | 代价 |
|---|---|---|---|---|
| 1 | 生成分辨率 `--den 2` / `--scale 0.75`（UI「输出」） | bridge XeFG | **85 → 10–18 ms** | 插值画质 |
| 2 | `StreamPercent=150/200` | 生产端拷贝 + 跨卡 | 63.3 → 28.1 / 15.8 MB/帧，省 2–4 ms | MV/深度精度 |
| 3 | 覆盖最旧而不是丢弃（已实现） | 生产端 | `age` 44 → 36 ms | 极端落后时撕裂风险 |
| 4 | `--no-depth` | 跨卡带宽 | −14 MB/帧 | 去遮挡质量 |
| 5 | `--prefetch-inputs` | bridge 拷贝与 Present 重叠 | 省 `copywait`，改变帧配对 | 需要实测 |
| 6 | 生产端改用 copy 队列 | 生产端 GPU 队列占用 | 图形队列少 2–5 ms（**同带宽**） | 额外队列 + 跨队列同步 |
| 7 | 颜色改走 HUDless/UI（而不是 WGC） | 去掉 WGC 一帧 | 可省 ~1 输入间隔 | 需要在 fakerFG 里 tag 并合成，改动最大 |
| 8 | 游戏分辨率/DLSS 档位 | 输入间隔 | 47.7 → 31 ms（实测） | 画质 |

### 6.1 已确认无用的开关（可以从 UI/命令行去掉）

| 开关 | 原意 | 为什么现在不需要 |
|---|---|---|
| `--fg-proj-rz` | 合成投影用 reversed-Z / 无限远平面 | `libxess_fg 1.3.1` **不读 `projectionMatrix`**，只用 MV 重投影（`docs/findings/FINAL_CONFIG.md` §5.16 实测）；而且游戏的真矩阵默认也不用（`--fg-use-matrix` 关）。只有我们自己的 `--fg-warp` / `--fg-of` 才用它，而 `--fg-of` 默认不编译 |
| `--fg-fov` / `--fg-zn` | 合成投影的 FOV / near | 同上；UI 已不再暴露 |
| `--fg-jitter-flip` | 抖动取反 | jitter 现在直接来自 producer 的值，无实测收益 |
| `--no-mv` / `--fg-of` / `--mv-flip` | 手动指定 MV 来源 | 消费端已按 producer 的 `kInputFlagMvActive` 自动决定；保留仅作排障 |

UI 从 2026-11-13 起不再传 `--fg-proj-rz`，`proj_rz` 状态/持久化/开关也一并删除。

**不值得做的**：加槽位（更多内存/更大的环相位）、给发布加锁/队列、在消费端轮询生产端
（这些都只会增加延迟或开销）。
