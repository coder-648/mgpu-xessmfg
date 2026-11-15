# 性能分析轮 · 交接文档（2026-11-14）

> **后续改动（同日）**：颜色 sink 已改为 **三缓冲**（`kColourSinks=3`，GPU 落后 >=3 帧才跳帧）；
> consumer 默认改为 **copy-on-publication + `--pub-fifo`**（`--no-pub-fifo` /
> `--no-prefetch-publications` 可回退）；发布 ini 的 `StreamPercent` 默认改为 **150**。
> 动态逐帧第二轮：**count 只在上一批 GPU retire 且输出已呈现后应用**
> （`GetLastPresentCount` 排空；未排空则沿用当前 count，相位继续记账），
> `frameRenderTime` 默认喂 **`k × vblank`**（`--fg-cad-rt 0` 可回到输入均值）。
> `--cadence-hold` 默认 0，仅作额外限频实验。固定 3x（锁 40）/4x（锁 30）
> 仍是稳定对照（固定 3x `HITCH=0`、`presents=120/120`、`load 13–54%`）。
> 本文剩余内容保留当时的实测语境。

> 本轮（scar/配对）已收尾：画面**零错位**、`marker: decoded` 逐帧命中、偶发大卡顿从 151 次
> 降到 0~2 次。**下一步是完整的性能分析**。这份文档只写"接手需要知道的"。
>
> 架构见 [`../design/ARCHITECTURE.md`](../design/ARCHITECTURE.md)，协议见
> [`../design/PROTOCOL.md`](../design/PROTOCOL.md)，历史基线见
> [`PERF_BASELINE.md`](PERF_BASELINE.md)（§1 的字段定义仍然有效）。

## 1. 链路一览（谁在哪张卡上跑什么）

```
游戏 (RTX 3080 Ti, 无显示输出)
  ├─ dxgi.dll = fakerFG（生产者/代理）：交出 swapchain、画四角标记、发布 MV/depth
  └─ xess_mfg_producer.dll：把渲染卡的 MV/depth 拷进跨卡共享槽（4 槽轮转）

  ↓ 跨卡 publication（MV 2560x1440 + depth 2560x1440 = 28.1 MB/帧）

消费端 xess_mfg_consumer.exe (RX 6750 GRE, 驱动显示器)
  ├─ WGC 抓游戏窗口（3840x2160 FP16 scRGB，窗口抓取，不含叠加层）
  ├─ 颜色 → 双缓冲共享 sink → 转换 draw（scRGB→BT.2020 PQ, R10G10A2）
  │     └─ 同一个 PS 里顺手把四角标记块用内侧邻居采样替换掉
  ├─ 解码四角标记（32 bit 短雪花 + CRC-8）→ **配对主键**
  └─ XeFG (libxess_fg) 2X 呈现 → 120 Hz 面板 80 fps 输出
```

颜色只走 WGC（不过卡）；跨卡的只有 MV/depth。标记只在游戏侧那帧可见，消费端呈现的
画面是干净的（回填在转换 pass 里完成）。

## 2. 本轮定稿的三件事（都有实测）

| 项 | 做法 | 实测 |
|---|---|---|
| **配对主键 = 标记** | 四角 6×6 块（3×3 格 × 2×2 px），32 bit 短雪花 `seq12｜low12(ms)` + CRC-8；解码后按 `frames[s].frame_id == (seq12+1)` 找 publication 槽（+1 = staging 晚一帧） | `decoded=2069 / miss=3`（52 秒）；`heuristic slot X -> Y` 纠错 23 次且**零 HITCH** |
| **回填在转换 pass 内** | `ps_main` 里对四角像素的 UV 向内偏移一个块宽（1:1 直通 = 像素偏移），零额外 pass/copy/barrier | 呈现画面干净，游戏侧仍带标记（设计如此） |
| **颜色 sink 双缓冲** | `mgpu_colour_sink0/1`，每帧翻转 `g_sink`，SRV 堆两个描述符 | `SLOW frame` 6→1、`HITCH` 6→2（52 秒） |

## 3. 日志速查（`bridge_*.log` = 消费端 stdout）

| 行 | 含义 / 什么时候看 |
|---|---|
| `breakdown: capture= work= present= gpu= wait= copy= colour= mvs= cq= of= age= xell= fg= total= … load= worst(…)` | 每 40 帧窗口的均值 + 最坏值；字段定义见 `PERF_BASELINE.md` §1。`load` = 每帧预算占用（>95% 时任何抖动都会丢帧） |
| `bus: mv … + depth … = 28.1MB/frame` | 跨卡带宽；`--no-depth` 直接减半 |
| `arr(vb): 3x=40 …` / `cadence: input=25.0ms = 3.00 vblanks … xN=2.00` | 输入节奏。`input` 必须是 120 Hz 的整数 vblank（25/16.7/33.3 ms），否则 XeFG 周期性退回 1X |
| `HITCH gap=… gpu=… pres=… xN=… drop=…` | 单帧间隔 >2× 期望时立刻打印；`gap` = 我们两次 Present 的间隔 |
| `SLOW frame: total=… decode=… layout=… colour=… inputs=… present=…` | **本轮新增**：自己单帧 >55ms 时打印，直接指出哪个阶段吃掉了时间 |
| `pf: N served, M missed, K copy-still-in-flight, D dropped by overflow` | publication 队列健康度；`dropped` 高 = 拷贝队列跟不上 |
| `marker: decoded= dup= fallback= miss= pub_stall=` | 配对健康度；`decoded` 应≈帧数，`miss` 应个位数 |
| `marker pair: heuristic slot X -> Y` / `slot N confirmed` | 标记与启发式猜测是否一致（纠错正是标记的价值） |
| `colour: GPU N frames behind - reusing the last frame` | 双缓冲之外的保护被触发（GPU 落后 >1 帧时跳过这次 66MB 拷贝） |
| `reset-history: forced after an MV/publication discontinuity` | 只在标记解不出且找不到后继时出现；正常应为 0 |

## 4. 已测基线（3080 Ti 渲染 + 6750 GRE 插帧，120 Hz 面板）

复现条件：游戏**锁 40 fps**（驱动 Max Frame Rate），`mgpu_test_nolock.bat`（`--lock 0`）。

| 指标 | 数值 |
|---|---|
| 输出 | 2X，80 fps（40 fps 输入 × 2），`xN=2.00` |
| `game`（输入间隔） | 25.0 ms = 3 vblanks @120Hz |
| `present` | 6~9 ms（XeFG 的放行点） |
| `gpu`（等上一帧完成） | 15~18 ms —— **GPU 一直有余量** |
| `colour`（66MB sink 拷贝） | 0.2~0.6 ms，但曾单帧 120~220 ms（驱动串行化，双缓冲已解决） |
| `capture` / `age` | 8~14 ms / 9~28 ms（WGC 一跳） |
| `mvs` / `copy` | 0.1~1.3 ms / 0~0.2 ms |
| `load` | 51~99%（随场景；`--lock 40` 时顶到 97~99%） |
| 跨卡带宽 | 28.1 MB/帧 ≈ 1.1 GB/s @40fps |
| 发布队列 | `dropped by overflow` ≈ 10/40 帧（拷贝队列常年落后约 25%） |
| 卡顿 | 151 次（`--lock 40`）→ **0~2 次**（`--lock 0` + 游戏锁 40） |

**重要结论**：这台机器上的卡顿**不是 GPU 算力问题**（卡顿瞬间 `gpu` 往往更低），而是
①输入节奏（非整 vblank / `--lock` 丢帧）②单帧阻塞（sink 拷贝）③publication 落后。

## 5. 已知开销与杠杆（按收益/代价排序）

1. **输入节奏**：游戏锁 40（或 30/60），让 `arr(vb)` 落在整数 vblank —— 这是"0 卡顿"的前提。
2. **`--no-depth`**：跨卡 28.1 → 14.1 MB/帧，`dropped by overflow` 应归零；代价是 XeFG 少
   一份遮挡判断。**下一轮第一个该做的 A/B**。
3. **`--lock`**：绝不要设在低于游戏实际帧率的值（游戏 35fps + `--lock 40` = 主动丢帧）。
4. **sink 三缓冲**：双缓冲已解决 5/6；若仍见 `colour` 长耗时，再补一张。
5. **`--pool`**：`qdrop` 显示捕获队列偶有丢弃，可试更大 pool。
6. **`present` 6~9 ms**：XeFG 内部，`--fg-rt-mode` 三档可 A/B。
7. **`xell`**：`xellSleep()` 已计时（通常 <1 ms）；某场景变大时可试关掉 XeLL。

**下一轮待办（建议顺序）**
1. `--no-depth` A/B：`dropped by overflow` / `HITCH` / `load` / 遮挡画质；
2. `arr(vb)` 抖动统计：锁 40 后是否 100% 落 3x；否则查 `drop`/`qdrop` 来源；
3. 把 `present`/`gpu` 的 40 帧均值改成直方图（需要时）；
4. `--pool` / `--fg-rt-mode` / `--lock` 组合扫描（每次只改一项）；
5. 10 分钟稳定性：`marker: miss`、`reset-history`、`HITCH` 是否随时间漂移。

## 6. 开关与脚本

| 文件（游戏目录 `bin/x64`） | 作用 |
|---|---|
| `marker_disable.on` | 生产端**不画**标记（A/B 画质/开销；需重启游戏） |
| `marker_repair_off.on` | 消费端**不回填**（标记留在画面上，看位置/大小） |
| `dump_frame.on` | 整帧回读 `frame_dump.raw` + `marker_corners.raw`（`scripts/raw_to_png.py` 转图） |
| `mgpu_test_marker_sidecar.bat` | 正式跑法（`--lock 40`），日志 `bridge_marker_sidecar.log` |
| `mgpu_test_nolock.bat` | **性能跑法**（`--lock 0`），日志 `bridge_nolock.log` |
| `mgpu_test_no_marker.bat` | 整条标记链路关闭（`--no-marker`），日志 `bridge_no_marker.log` |

脚本：`build_faker_fg.sh`、`build_consumer.sh`（构建期 `fxc` 校验运行时着色器）、
`build_producer.sh`、`deploy_faker_fg.sh`（**必须用它**，见 §8 坑 1）、
`extract_hlsl.py`、`raw_to_png.py`、`make_no_marker_bat.sh`。

## 7. 部署与崩溃定位

- 部署三件套必须同名同步：`fakerFG.dll` → **`dxgi.dll` + `nvngx_dlssg.dll` + `fakerFG.dll`**，
  外加 `xess_mfg_producer.dll`、`xess_mfg_consumer.exe`。
- 崩溃符号化：`build/xess_mfg_consumer.map`（构建已加 `/MAP`）；`RVA = rip - 模块基址`，
  `.text` 起始 RVA 0x1000 → map 里的 `0001:偏移`。
- 日志：游戏侧 `Cyberpunk2077_fakerFG.log`、`Cyberpunk2077_mfg_producer.log`；
  消费端 stdout → `bridge_*.log`；消费端内 fakerFG → `xess_mfg_consumer_fakerFG.log`。

## 8. 五个坑（已踩过，别再踩）

1. **游戏加载的是 `dxgi.dll`**，不是 `fakerFG.dll`；只更新后者 = 游戏跑旧代码。
2. **present ring 映射是 `FILE_MAP_READ`**：`InterlockedCompareExchange64(&ring->write,…)`
   会在只读页上触发 `0xC0000005`；只能对齐 volatile 读。而且它被**两个进程用不同计数器
   共写**，不能当帧 id 用。
3. **`Map` 的 `MapType` 和 `MapFlags` 是两个参数**：把 `DO_NOT_WAIT` 或进 MapType 会每次
   返回 `E_INVALIDARG (0x80070057)`。
4. **转换着色器是运行时编译的字符串**：`cl` 查不出语法错误（曾因改名 `pad0` 起不来），
   构建期已用 `extract_hlsl.py` + `fxc` 校验四个入口。
5. **往"上一帧还在读"的共享纹理写会阻塞 100+ms**（驱动串行化）。双缓冲后仍要保证 GPU
   落后 >1 帧时不覆盖（已内置跳帧保护），不要退回单 sink。

## 9. 建议不做的事

- 不要把颜色改成跨卡传输（WGC 必须保留为颜色源，这是产品前提）。
- 不要再用 present-ring 的 seq 当配对主键（坑 2）。
- 不要在 `--lock` 上贴着游戏帧率取值（40 vs 35 = 主动丢帧）。
