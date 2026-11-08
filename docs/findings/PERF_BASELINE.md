# 性能基线与优化清单

> 这一页把**已经测过的**和**还没测的**分开：§2 是基线（**不要再测一遍**，直接引用），
> §3 是候选与**本轮已做的事**（每条都标了结论）。画质相关的变量看
> [`QUALITY_VARIABLES.md`](QUALITY_VARIABLES.md)，结论摘要看
> [`CONCLUSIONS.md`](CONCLUSIONS.md) §7。

实测机器：渲染 RTX 3080 Ti，帧生成 RX 6750 GRE，120 Hz 面板，Windows 11 WDDM 3.2
（环境细节见 [`ENV_REPORT.md`](ENV_REPORT.md)）。**换卡/换游戏这些数字都会变**，
它们只用来判断"哪个环节值得动"。

## 1. 怎么量（固定跑法 + 判据）

固定场景 + 固定外部锁帧 + 同一段 40 帧窗口做 A/B，只看 `--report` 输出的这几行
（`src/bridge/bridge.cpp` 的 `run_frame_ex` 尾部）：

```
[bridge]   breakdown: capture= work= present= gpu= wait= copy= colour= mvs= of= age= xell=
           fg= total= cpu= timeouts= mode=  xN=  game=  jitter=..  arr_gaps=  load=  slow=
           pace= repres= skip=  worst(gpu/pres/wait/copy/colour/mvs/of/age/xell/fg/total/gap)
           xNmiss=
[bridge]   bus: mv 2560x1440 14.1MB + depth 2560x1440 14.1MB = 28.1MB/frame (3.4 GB/s at 120.0 fps)
```

| 字段 | 含义（都在 `run_frame` 里量，QPC 时钟） |
|---|---|
| `capture` | 等下一帧 WGC 帧（`TryGetNextFrame` 的阻塞，在外层循环量） |
| `work` | 从取到帧到提交命令列表之间，我们自己的 CPU 开销 |
| `present` | `Present()` 的阻塞时长 —— **XeFG 的放行点** |
| `gpu` | 等上一帧 GPU 完成（`gpu_throttle()`，深度 1 帧；`--no-throttle` 时为 0） |
| `wait` | 等槽位 fence（`wait_slot()`），超时计入 `timeouts` |
| `copy` | 等"WGC 帧 → 我们的 D3D11 承接纹理"这次同卡拷贝在 D3D12 队列上完成 |
| `colour` | 那半边的 D3D11 侧：keyed-mutex 获取 + 全帧 `CopyResource`（4K 66 MB / 5K 118 MB） |
| `mvs` | 录 MV/深度**本地**拷贝的时间（这些拷贝把物理上在渲染卡的数据拉到插帧卡） |
| `of` | 本地光流（`--fg-of`） |
| `age` | WGC `SystemRelativeTime` → 我们取到手 = 游戏合成 → 本进程这一跳 |
| `xell` | `xellSleep()` 的阻塞时长 |
| `fg` | `xefgSwapChain` 的标注（tagging）调用 |
| `total` | 整个 `run_frame` |
| `cpu` | `total` 减掉上面所有已计时的阶段 = **没有归处的部分**（选槽、描述符/屏障设置、报告本身）。它涨了，问题在我们；它掉了，问题在 GPU/PCIe |
| `game` / `jitter` | 生产端帧时间 EMA / 到达间隔的 min..max |
| `arr_gaps` / `xNmiss` | 到达间隔例外（丢帧）计数 / 倍率判定缺失数 |
| `load` | `(gpu + present) / 输入间隔` —— paced loop 的占空比 |
| `slow` | 超过输入间隔的帧数 |
| `pace` / `repres` / `skip` | `--pace` 的补偿量 / 重播帧 / 跳过的帧 |

`bus:` 行是**唯一会离开渲染卡的逐帧流量**：MV（R16G16，4 B/px）+ 深度（R32，4 B/px），
尺寸取生产端发布的流尺寸。它就是 `StreamPercent` / `--no-depth` 改变的量 ——
当目标是"降总线"时看这一行，不要看 `total`。

另外三行是**输入/输出侧**的真相，判"掉帧"只能看它们：

```
[bridge]   arr(vb): 2x=38 3x=1 4x=1  n=40  need 2x (25.0 ms)  frames=1401  med dwm=25.0 wc=25.0
[bridge]   out:  presents=80  expected=80  vblanks=81
[bridge]   out2: GetFrameStatistics unsupported on this swap chain
```

| 行 | 含义 |
|---|---|
| `arr(vb)` | 每个输入到达间隔落在第几个 vblank（用帧自带的 `SystemRelativeTime`）。`n` = 这一窗口到达帧数：`n == frames` 就说明**一帧没丢**；桶比 `need` 大就是"晚到了" |
| `game(vb)` | 同样的分桶，但用的是**生产端**量到的"游戏两次 Present 之间"的原始间隔（`FrameShared.frame_time_ms`）。它和 `arr(vb)` 成对看才能分清"游戏自己抖"还是"抓帧/合成抖动" |
| `out:` | 交换链真实 Present 数 vs 期望（`frames × (interp+1)`）vs 这窗口实际过了几个 vblank。`presents < expected` ⇒ XeFG 少发帧；`vblanks > presents` ⇒ **有空刷新周期**（画面停住一拍），这是眼睛看到的"掉帧" |
| `out2:` | `GetFrameStatistics`；flip-model 交换链在 Windows 上通常不支持，支持时能给出"跳过了哪个 vblank" |


**判据优先级（从上往下，前一条不满足就别看后面）：**

1. 状态行的 `... %.1f fps out = %.2f vblank/frame` 应当是 **1.00**（默认值
   `--fg-rt-mode quant` 的功劳）；`NOT-EXACT` 标记就是周期性丢帧的来源；
2. `slow` = 0；`load` 95–100% 是 paced loop 的**正常值**，不是告警；
3. `gpu` + `present` ≈ 输入间隔（这两项就是"节拍在哪等"）；
4. `of` ≈ 0.3 ms、`copy` ≈ 0.1 ms、`mvs` ≈ 0 → 这些不是瓶颈，别再查；
5. `age` 5–20 ms 正常，> 50 ms 说明**合成器迟到**（不是我们的循环慢）。

> `worst(...)` 是历史最大值，只看量级，**不当均值用**。

可用工具：`--report N`、`--list-modes`、`--fg-probe`、`--dump` +
`scripts/analyse_dump.py`、`--probe-colour`、`--mv-shadow`、`--log-flush`（每行立即
flush，排查崩溃时用；默认按 200 ms 节流）。命令写法见 [`TOOLING_NOTES.md`](TOOLING_NOTES.md)。

## 2. 基线（已实测，别重测）

| 事实 | 数据 | 出处 |
|---|---|---|
| 跨卡通道是 **file-mapping 共享堆 + 系统内存**（非 P2P、非 CPU memcpy） | **9.19 GB/s**；V6 零拷贝 12.24 ms/帧，CPU 中转 20.9 ms（已否决） | `milestones/M0_RESULTS.md` |
| 每个真帧过总线的量 | 4K：MV 33.2 + 深度 33.2 = **66 MB**；5K：118 MB；`StreamPercent=150` 可砍半 | `shared_layout.h` + `ENV_REPORT.md` |
| **颜色不走总线** | WGC 抓 DWM 合成结果，我们的 D3D11/D3D12 在**同一块卡**上，卡内搬运 | `../design/ARCHITECTURE.md` §6 |
| 颜色拷贝（`Color=1`）实测代价 | 渲染卡 **80% → 95%（+15 个百分点，不是 +15%）**；而且 bridge **根本不读**这条流 | `../design/ARCHITECTURE.md` §6.1 |
| 光流成本 | **0.3 ms**（对比游戏 MV 在渲染卡上 +7.4 ms） | `QUALITY_VARIABLES.md` |
| 深度 / 投影 / FOV | **对画质无可测影响**；深度延迟 1/2/3 更差 | `CONCLUSIONS.md` §1 |
| XeFG 自身 GPU 时间 | 4K 2x ≈ **1–9 ms**（抖动），4K 3x ≈ **5–20 ms** | 两份运行时日志（`FINAL_CONFIG.md` §9） |
| 抖动主因是 **vblank 算术**，不是算力 | `0.96` / `1.24` / `1.86` vblank/帧 → 周期性丢帧；`quant` 已修 | `CONCLUSIONS.md` §5 |
| 消费者侧锁帧无效 | 必须用外部限帧器 | `FINAL_CONFIG.md` §9.16 |

**关键判断**：插帧卡上的 XeFG 时间是 Intel 闭源 DLL，**不可优化**。所以"性能优化"
的真实空间只有三块：① 我们自己的 CPU/同步开销 ② 显存与总线流量 ③ 抖动/稳定性
（第三块已经吃了一大口）。

## 3. 候选清单 → 本轮结论

### 3.1 已做（2026-11-08，`bridge.exe` 见 §3.4）

| 改动 | 位置 | 收益 | 为什么安全 |
|---|---|---|---|
| `gpu_throttle()` 与 3 个诊断等待复用**一个** fence 事件 | `wait_fence_value()` | 每帧少一次 `CreateEventW`+`CloseHandle`（60–120 次/秒 → 0） | 只在 `GetCompletedValue()` 可信的**本地** fence 上用；等待前后都重查 fence 值，剩余超时重算，迟到信号最多多转一圈 |
| 不再每帧 `GetBuffer()` 拿交换链缓冲 | `back_buffer()` | 每帧两次 `QueryInterface`+引用计数 → 0 | 交换链从不 `ResizeBuffers`，缓冲身份终生不变 |
| 流几何（MV/深度尺寸）只在 `generation` 变化时读 | `service_shared_layout()` | 每帧少一组**非缓存 PCIe 读**（seqlock + 4 个字段） | 生产端每次改尺寸都会 bump `generation`；另外 `mv_w==0` 时继续每帧重读，避免"先 attach 后发布"漏读 |
| `logf_` 改成按 200 ms 节流 flush（`--log-flush` 恢复逐行） | 文件顶部 | 每行一次 write 系统调用 → 每秒 ≤5 次 | 含 `WARNING`/`FAILED`/`note:` 等字样、以及 `hard_exit` 路径仍然立即 flush |
| 统计改用 QPC | `work` / `gpu` / `present` / `colour` | 这三项原来用 `GetTickCount64`，量化到 15.6 ms（`timeBeginPeriod(1)` 失效时会读成 0/16），`load` 和 `present` 告警跟着抖 | 纯统计，不参与任何控制流 |
| 新增 `colour=` 与 `cpu=` 两个字段 | breakdown | 颜色 hop（66/118 MB 同卡拷贝）和"没归处的时间"终于各有一行 | 只加日志 |
| 新增 `bus:` 行 | breakdown | 逐帧 PCIe 字节数与 GB/s 直接可读 | 只加日志 |

> 这一轮**没有**动帧循环结构、没有动 XeFG 接线、没有动 add-on。上面每条都是
> "少一次系统调用/少一次 PCIe 读/多一行可测量"，CPU 侧合计约几十 µs/帧 ——
> 在 paced loop 里（`gpu + present` 占满输入间隔）**不会体现在 fps 上**，
> 它体现为 `cpu` 字段更小、`load`/`present` 告警不再误报。真正的收益在下面 §3.2。

### 3.2 分析后判定"不做"的（带算术）

| 原候选 | 判定 | 依据 |
|---|---|---|
| **颜色链路省一次中转**（原 #2） | ❌ **零收益**，别做 | 现状每帧：WGC 帧 FP16 →(D3D11 拷贝)→ 承接纹理 FP16 →(D3D12 conv draw)→ 交换链 R10G10B10A2，即 66 W + 66 R + 33 W = **165 MB**。把 PQ 编码挪到 D3D11 先写 R10G10B10A2（66 R + 33 W），D3D12 再拷进交换链（33 R + 33 W）合计还是 **165 MB**。XeFG 的输入必须是交换链后备缓冲，这一跳省不掉 |
| **add-on 每帧两个全屏 pass**（原 #3） | ❌ 不是几 ms，是 0.1 ms 量级 | pass 只做"本地 RT → 本地 RT"，4K 一次 132 MB **本地**显存流量，3080 Ti 本地带宽 ~900 GB/s ⇒ ~0.15 ms。贵的是**跨卡**那 33 MB（见 §3.3） |
| **`wait_slot` 也复用事件**（原 #1 的另一半） | ❌ 故意保留每帧新建 | 跨卡 fence 在本节点 `GetCompletedValue()` 恒为 `UINT64_MAX`，无法区分"迟到信号"；复用会让 `g_fwd_expect[]` 虚增、读到**旧一帧**的槽（§2 第 2 条硬约束）。代价 ~1 µs |
| **`bridge.cpp` 拆 TU**（原 #4） | ⏳ 仍未做 | 纯可维护性；6.9k 行单 TU 编译约 20 s |

### 3.3 还剩下的真实空间（按性价比）

| # | 点 | 量级 | 怎么验 | 风险 |
|---|---|---|---|---|
| **1** | **跨卡流量本身**：`[capture] StreamPercent=150`（MV+深度都降到 ~1440p）或 `--no-depth`（只发 MV） | 已实测 `--no-inputs` 让 4K/DLAA 的 `gpu` **25.2 → 21.6 ms**，即跨卡搬运占 **3.6 ms/帧**；深度是其中一半 ⇒ `--no-depth` ≈ **-1.8 ms/帧**；`StreamPercent=150` 让两者都 ×1/2.25 | `bus:` 行 + `gpu` 字段 A/B | 低（`--no-depth` 画质"无可测差别"已有对照，但 `FINAL_CONFIG` §4.5 仍标为"掉质量开关"，见 §6） |
| **2** | `gpu_throttle` 深度 vs 吞吐（`--no-throttle`） | 互斥，需先定目标 | 同场景 A/B `total` 与 `compose → photons` | 低（实测更差，仅作对照） |
| **3** | `bridge.cpp` 拆 TU | 编译时间 | 编译通过 | 低 |

### 3.4 本轮产物

- `build/bridge.exe` md5 **`43f263f5…`**（构建：`./scripts/build_bridge.sh`，MSVC 14.44，
  `/W3` 零警告）。

### 3.5 真机首跑（2026-11-08 23:1x，CP2077，`C:\temp\mgpu\bin`）

第一次带着新字段跑起来，单次 45 s、`--report 40`，CP2077 3x（`--den 2 --no-mv
--interp 2 --unlock-mfg`）：

| 观察 | 数字 | 结论 |
|---|---|---|
| `timeouts` / `slow` / `xN` | 0 / 0 / 3.00 | 新二进制在真机上稳定，没有回归 |
| `cpu`（新增） | **0.0–1.3 ms** | 我们自己的 CPU 可以忽略 —— 循环就是卡在 GPU/PCIe 上，继续抠 CPU 收益为零 |
| `colour`（新增） | **0.2–0.4 ms** | 4K FP16 `CopyResource` + keyed mutex 很便宜；§3.2 的 165 MB/帧是**带宽**，不是延迟 |
| `bus`（新增） | 2227×1253 时 **10.6 MB/帧**；3840×2160 时 **31.6 MB/帧** | 一行就能看出 DLSS 模式换了（render size 变了）—— 这正是这张表存在的意义 |
| `--no-depth` | `bus` **10.6 → 0.0 MB/帧**，`xN` 仍 3.00、`xNmiss=0` | 深度流可以整条拿掉而不破坏 XeFG（3x 正常出帧） |
| `present size 1920x1080 != window client 3840x2160` | 警告 | `--den 2` 在 4K 下把 present 尺寸减半，DXGI 会拉伸；这是配方选择，不是 bug，但值得知道 |
| `age` 尖峰 | 偶发 49–72 ms（正常 0.6–10 ms） | WGC/合成器**积压**，和 `present` 一起涨，说明那一刻循环超了预算 |

> **性能差值这一轮没有定论**：两次"基线"之间游戏自己换了 DLSS 模式（深度流从
> 2227×1253 变成 3840×2160），而且输入没锁（`input=31 ms`、`jitter 16.7..41.7`、
> `cadence = 1.24 vblank/frame NOT-EXACT`，输出 96.8 fps 而不是 120）。
> A/B 必须在**同一个 render size + RTSS 锁 40 fps**下跑才算数 —— 步骤见 §5。

### 3.6 第一次干净的 A/B（A→B→A，CP2077，3840×2160，各 40 s）

渲染尺寸固定 3840×2160（`bus:` 两组都是 mv 3840×2160 / depth 31.6 MB），
A 与 B 之间只差 `--no-depth`，第三次再跑一遍 A 用来**量漂移**：

| field | A1 | B (`--no-depth`) | A3（又是 A） | 能不能下结论 |
|---|---|---|---|---|
| `bus` | 31.6 MB/帧 | **0.0** | 31.6 MB/帧 | ✅ 确定：整条深度流消失 |
| `total` | 9.30 | **7.75** | 8.90 | ✅ B 比两次 A 都低 ≈ **−1.2 ms** |
| `gpu` | 6.65 | 5.90 | 5.40 | ❌ B 夹在两次 A 之间 ⇒ 差异在噪声里 |
| `present` | 6.80 | 5.40 | 5.80 | ❌ 同上 |
| `age` | 2.75 | 16.80 | 5.20 | ❌ 两次相同配置的 A 就差 2.5 ms |
| `compose->photons` | 9.0–17.3 ms | 11.7–20.0 ms | 17.4–28.3 ms | ❌ 第三次 A 最差 ⇒ 全是场景/机器状态漂移 |

结论：

1. **`--no-depth` 在 4K 原生下真实收益 ≈ 1.2 ms 循环时间**，而不是算术上的 3.4 ms
   （31.6 MB ÷ 9.19 GB/s）—— 跨卡拷贝和 XeFG 的 GPU 工作是**重叠**的，串行账算不出来。
   它换来的是 1.3 GB/s 的总线占用归零，`total` 有更多余量。
2. **本机 A/B 的噪声是 2–10 ms**（`age` 2.75 vs 5.20、`compose->photons` 9 vs 20
   在两个完全相同的配置之间）。所以只有 > ~2 ms 的差异才值得信 ——
   以后跑 A/B 至少要 A→B→A 三趟，别用两趟定论。
3. 这一轮的条件仍然不理想：`cadence = 1.24 vblank/frame`（输出 96.8 fps 而不是 120）、
   `lock=0.00`，说明**输入没被 RTSS 锁在 40 fps**，面板不是限制因素，所有东西都有余量 ——
   这也是深度拷贝的代价大部分被藏起来的原因。要看到总线费用的真实大小，
   必须先把输入锁死（`cadence` = 1.00）。


### 3.7 顺手抓到的真 bug：`input=` 一直是 31 ms（墙钟采样偏差）

**症状**：RTSS 把 CP2077 锁在 40 fps，`breakdown` 里 `game=25.0ms`（对），但 cadence 行写着
`input=31.0ms = 3.72 vblanks -> rt=33.3ms -> 96.8 fps out = 1.24 vblank/frame NOT-EXACT`。
即 **XeFG 被喂了 4 个 vblank 的 `frameRenderTime`，输出只有 96.8 fps 而不是 120**。

**先排除"输入丢失"**：加了一行直方图 `arr(vb):`，把每个到达间隔按 vblank 分桶
（用 WGC 帧自带的 `SystemRelativeTime`，即合成器时间戳）：

```
arr(vb): 3x=40  n=40  need 3x (25.0 ms)  frames=1401  med dwm=25.0
```

40 帧全部落在 3 个 vblank（25.0 ms），`n` 与 `frames` 相等 ⇒ **一帧都没丢**，
`--pool 8` / `--jbuf 1` / `--slack 1` 三种都试过，`input=` 始终是 31 ms ⇒ 不是排队问题。

**根因**：`note_arrival()` 用 `GetTickCount64()` 的**墙钟差值**做间隔统计，而它有 1 ms 分辨率：
一次循环里排空两帧时，第二帧的差值是 0，被 `arr_push()` 的 `dt > 0` 过滤掉，
于是中位数被系统性推高（25 → 31）。而 `--fg-rt-mode quant` 用的正是这个中位数：

| | 修前 | 修后 |
|---|---|---|
| 间隔来源 | 排空时刻的墙钟差（丢 0 ms 样本） | 帧自带的 `SystemRelativeTime` |
| median | 31.0 ms | **25.0 ms** |
| `frameRenderTime`（quant） | 33.3 ms = 4 vblank | **25.0 ms = 3 vblank** |
| 输出 | 96.8 fps = 1.24 vblank/帧 ❌ | **120.0 fps = 1.00 vblank/帧** ✅ |
| `jitter` | 16.7..41.7 ms | **25.0..25.0 ms** |
| `load` / `total` | 90% / 23.0 ms | **44% / 11.6 ms** |

墙钟只在帧没有时间戳时才作为回退。这条同时解释了"`--fg-rt-mode quant` 改了但还是周期性丢帧"：
quant 一直在按错误的中位数吸附。

> 因此 `CONCLUSIONS.md` §5 里"3x，输入约 31 ms → 吸附 33.3 ms"那句是**这个 bug 的产物**：
> 40 fps 锁的真实输入是 25.0 ms，quant 之后就是 25.0 ms / 1.00 vblank/帧。

### 3.8 修好之后重跑同一组 A/B（4K 原生，40 fps 锁，A→B→A）

修完 §3.7 之后 `cadence` 三次全部 **1.00 vblank/帧**、`input=25.0 ms`，
A 与 B 只差 `--no-depth`：

| field | A1 | B (`--no-depth`) | A3 | 读法 |
|---|---|---|---|---|
| `cadence` | 1.00 vb | 1.00 vb | 1.00 vb | 三次都精确，量到了真东西 |
| `bus` | 31.6 MB/帧 | **0.0** | 31.6 MB/帧 | 整条深度流消失 |
| `gpu` | 9.60 | 7.10 | 10.30 | B 低 2.5–3.2 ms |
| `present` | 9.45 | 5.30 | 14.25 | B 低 4.2–9.0 ms |
| `gpu + present`（这才是预算） | 19.1 / 24.6 | **12.4** | 见左 | 输入预算是 25.0 ms |
| `load` | 76% | **48%** | **98%** | 带深度时 4K 原生 3x 已经**顶到预算** |
| `slow` | 0 | 0 | **1.5 帧/窗口** | 顶到预算 ⇒ 周期性丢帧，就是用户看到的现象 |
| `age` | 5.00 | 1.60 | 3.85 | B 的帧更年轻 |

结论：

1. **4K 原生 + 3x + 深度流 = 正好卡在 25 ms 预算上**（`gpu+present` 19–24.6 ms），
   余量不足时 `present` 会被 XeFG 拉长、`slow` 冒出来 —— 这解释了过去"周期性掉一帧"。
2. **`--no-depth` 是这一档最有效的单开关**：预算占用 98% → 48%，多出 7–12 ms。
   代价是 disocclusion 少了深度参考（画质是否可见差异仍需 A/B 定论，见 §6）。
3. 两次相同配置的 A 之间仍有漂移（76% ↔ 98%、`present` 9.45 ↔ 14.25），
   所以仍然只能信"B 比两次 A 都好"这类方向性结论。

### 3.9 2x@60 稳定性（修完 §3.7 之后，CP2077，120 Hz 面板，`--interp 1`）

输入锁 60 fps ⇒ 2x ⇒ 120 fps，输入预算 **16.7 ms**。两组各 60 s，只差 `--no-mv`：

| 指标 | 2x + 游戏 MV | 2x + `--no-mv` |
|---|---|---|
| 报告窗口数 | 85 | 58 |
| `cadence` 非 1.00 的窗口 | **0** | **0** |
| `slow > 0` 的窗口 | **0** | 2 |
| `xN` | 2.00 | 2.00 |
| `xNmiss` | 仅第 1 个窗口（启动，`xN=1.00, age=98.7`） | 同 |
| `arr(vb)` | 31/85 窗口是 `2x=40`，其余 `2x=36..39` + 3x/4x 各 1–4 | `2x=40` 为主 |
| `gpu + present`（预算 16.7） | **7.0** | **9.4** |
| `load` | 41% | 47% |
| `bus` | 21.3 MB/帧（1.2 GB/s） | 10.6 MB/帧（0.6 GB/s） |
| `age` | 13.9 ms | 6.7 ms |

结论：

1. **2x@60 现在是整拍且稳的**：85 个窗口没有一次 `NOT-EXACT`、没有一次 `slow`，
   `arr(vb)` 的 `n` 始终等于处理帧数 ⇒ **一帧都没丢**。
2. **预算只用了一半**（7–9 ms / 16.7 ms），2x@60 是这台机器上最舒服的一档；
   同一台机器 4K 原生 3x@40 的占用是 76–98%（§3.8）。
3. 每窗口仍有 1–4 帧晚到一个 vblank（`arr_gaps` 合计 121/3400 ≈ 3.6%），
   那是 RTSS/合成器抖动，被 XeFG 的插帧吸收，输出仍是 1.00 vblank/帧。
4. **2x 下开不开 MV 对性能没有可测差异**（`present` 3.7 vs 5.7 在噪声内），
   所以按画质选：**2x 用游戏 MV**（`SkipMv=0`，不加 `--no-mv`），与本项目既有结论一致。

### 3.10 用户报"看到很多掉帧" —— 输出侧确实有空刷新（2x@60，同一场景，各 60 s）

前面所有计数器都只看**输入**：`cadence = 1.00`、`slow = 0`、`arr(vb)` 的 `n == frames`。
它们都健康，但眼睛看到的掉帧在**输出侧**。新增 `out:` 行（交换链 `GetLastPresentCount`
+ 窗口时长 × 刷新率）之后，再看 `scripts/perf_windows.py`（**只统计游戏内窗口**，
剔掉 `load > 100%` 或 `skip > 2` 的加载窗口）：

| 配置 | 窗口 | `game>2vb` | `arr>2vb` | **空刷新率** | `load` | `gpu` | `present` |
|---|---|---|---|---|---|---|---|
| 纯 2x（`--interp 1`，`perf_out`） | 85 | **0.0%** | 5.5% | **5.0%** | 40% | 2.8 | 4.2 |
| 纯 2x（`--interp 1`，`perf_diag`） | 43 | 1.0% | 8.8% | **8.5%** | 34% | 3.2 | 2.9 |
| `--jbuf 1` | 82 | 0.0% | 10.2% | **8.0%** | 49% | 3.5 | 5.5 |
| **`--interp 2 --interp-cadence`** | 69–82 | 0.0% | 6.5–9.6% | **2.2%** | 68% | 5.3–5.9 | 6.1–6.6 |

> `game>2vb` 是**生产端**量的游戏 present 间隔里晚于 2 个 vblank 的比例。它几乎是 0
> ⇒ **游戏自己是稳的**。而 `arr>2vb` 有 5–10% ⇒ 抖动是在"游戏 present → 我们抓帧"
> 之间产生的（DWM 合成 / WGC 交付 / 我们的顶层覆盖窗口）。


读法：

1. **`presents` 始终等于 80**（40 帧 × 2）⇒ XeFG 没少发帧，问题不在它，也不在我们
   的提交。`vblanks` 却经常是 81–109 ⇒ **每 100 个刷新周期里有 5 个没有新画面**，
   最差的一窗有 29 个空 vblank（= 240 ms 画面停住）。这就是"很多掉帧"。
2. 空 vblank 与 `arr(vb)` 里的 **4x/5x 桶一一对应**：输入帧晚到 1–2 个 vblank
   （33–41 ms 而不是 16.7），插帧只能补一帧，于是留一个空 vblank。
   **根因在输入侧**：游戏/RTSS 没稳住 60 fps（每窗口 6–14/40 帧晚到）。
3. **`--interp-cadence` 是目前唯一有效的补救**（4.9% → 2.0%）：它按"这一帧实际过了几个
   vblank"决定插几帧，晚到的帧会多要一帧。注意它必须配 `--interp 2` 才会打补丁 ——
   `--interp 1 --unlock-mfg` 时补丁代码直接跳过（日志会说 `2x requested ... nothing to patch`），
   `maxSupportedInterpolations` 停在 1，cadence 模式就升不上去（这一轮实测 `switches=0`）。
4. **`--jbuf 1` 更差**（8.1%）：它是"延迟一帧吸收抖动"，只会把延迟加上去，不解决输入的空洞。
5. 三个配置里最大的单次间隔 220 ms 都出现在 `arr(vb)` 有 `8x` 桶的窗口
   （`jitter=16.6..116.7ms`、`worst colour 16.9ms`）⇒ 那是**游戏自己的卡顿**
   （资产流送/着色器编译），任何插帧参数都救不了。

顺带修掉一个一直误导人的日志：`--unlock-mfg` 的 "The patch did not take" 现在只在
**真的请求了 >2x** 时才打印（原来 `--interp 1` 也会报，因为 2x 时补丁本来就"nothing to patch"）。

### 3.11 "为什么显卡有余量还是掉帧" —— 抓帧侧抖动 + 我们自己的 drain-newest

新增 `game(vb):` 行（用 `FrameShared.frame_time_ms`，即生产端量到的"游戏两次 Present
之间"的原始间隔，和 `arr(vb)` 同样按 vblank 分桶）。60 s，2x@60：

| 直方图 | 结果 |
|---|---|
| `game(vb)`（游戏自己的 Present 间隔） | 游戏内窗口 **`2x=40`**，晚到比例 0–1% |
| `arr(vb)`（合成器时间戳，我们看到的到达） | 晚到比例 **5.5–10%**（同样的窗口！） |
| `out:`（交换链真实 Present） | `presents=80 expected=80`，`vblanks` 79–81 |

**所以问题不在渲染算力，也不在游戏 Present 的速率**：游戏每隔 16.7 ms 稳定交付一帧，
但**帧从游戏走到我们手上这一段**有 5–10% 的机会晚一个 vblank。这一段包含三样东西：
DWM 合成、WGC 交付、以及**我们自己的顶层覆盖窗口**（它在同一个显示器上以 120 fps 上屏，
让游戏窗口很难保持 independent-flip）。

输出侧因此每 100 个刷新周期丢掉 5–8 个（纯 2x），而 **`--interp 2 --interp-cadence`
把同一批数据压到 2.2%** —— 因为它按"这一帧实际过了几个 vblank"决定插几帧，正好补上
抓帧晚到的那一拍。

> 加载阶段的窗口不计入以上统计：`load` 116–376%、`skip` 16–50 帧、`age` 480 ms、
> `colour` 78 ms（同卡拷贝也被堵）、空刷新率 69% —— 那是**游戏还在加载/流送**，
> 与游戏内表现无关（用户确认）。`scripts/perf_windows.py` 默认就把这类窗口踢掉。

剩下可查的方向（还没测）：

1. **`--capture-monitor`**：改成抓显示器而不是窗口，看 `arr(vb)` 的晚到比例是否下降
   （能区分"窗口合成"还是"WGC 交付"）。
2. **覆盖窗口的影响**：把覆盖窗口放到第二块屏（`ROADMAP.md` 的 `--overlay-monitor`），
   游戏窗口恢复到 independent-flip 后 `arr(vb)` 应该更干净。
3. **`--pool 2/4/8`**：已经测过对 `arr` 无影响。

### 3.12 已实现并验证：`--overlay-hidden` 证明"就是覆盖窗口"，`--overlay-monitor` 可用但受拓扑限制

**实验（2x@60，同一场景，同一会话连跑两组，只差 `--overlay-hidden`）**：

| 配置 | 窗口 | `game>2vb` | `arr>2vb` | **空刷新率** | `load` | `gpu` | `present` |
|---|---|---|---|---|---|---|---|
| `--overlay-hidden`（窗口不显示，Present 照跑） | 74 | 0.0% | **0.7%** | **1.2%** | 28% | 3.2 | **1.5** |
| 可见覆盖窗口（紧接着的同一会话） | 68 | 0.8% | **8.5%** | **6.6%** | 40% | 2.9 | 4.1 |

结论：**抖动就是我们的覆盖窗口造成的**。把窗口藏起来（其余一切不变）之后，
`arr>2vb` 从 8.5% 掉到 0.7%，空刷新从 6.6% 掉到 1.2%，`present` 从 4.1 降到 1.5 ms。
机制和推断一致：全屏 topmost 覆盖窗口让游戏窗口失去 independent flip，
游戏和我们都得走 DWM 合成。

**`--overlay-monitor N` 实现情况**（本机两块屏：AMD 驱动 (0,0)-(3840,2160)、
NVIDIA 驱动 (3840,0)-(7680,2160)）：

| 检查项 | 结果 |
|---|---|
| 自动选卡 | `consumer adapter: 'NVIDIA GeForce RTX 3080 Ti' - drives the overlay monitor` ✅（按上屏屏选卡） |
| 覆盖窗口位置 | `overlay target: monitor ... rect=(3840,0)-(7680,2160) 3840x2160 (--overlay-monitor)` ✅ |
| 交换链 | `hwnd popup 10bit FLIP_DISCARD hr=0x00000000` ✅ |
| **但** | 因为那块屏接在**渲染卡**上，XeFG 被换到 3080 Ti ⇒ `note: same GPU as the producer` ⇒ 插帧和游戏抢同一张 GPU：`gpu` 3.2 → **66–72 ms**、`load` 249–293%、`slow=40`、游戏自己掉到 ~35 fps ❌ |

⇒ **在本机这个布线（一块屏接 A 卡、一块接 N 卡）上，`--overlay-monitor` 不可用**，
必须**两块屏都接帧生成卡**（同卡双输出）才有意义。代码已加前置校验：
目标屏若不由消费卡驱动，会先 WARNING 再回落到游戏窗口所在屏，不会黑屏。

### 3.13 单帧输入尖峰（快速转视角掉到 57~58 fps）—— `--fg-rt-spike`

**现象**：60 fps 锁 + 显卡有余量，但**快速移动 / 快速转视角**时掉到 57–58 fps；
和"锁 40 掉到 33–38"是同一种事。

**机制（新计数器能一条条对上）**：

1. 快速转视角 → 游戏某一帧超了 16.67 ms 的槽位 → 输入晚到 1 个 vblank
   ⇒ `arr(vb)` 出现 `4x`（33 ms）而不是 `2x`；
2. 我们照旧把 `frameRenderTime = 16.7 ms`（quant 的中位数）喂给 XeFG，
   XeFG 按这个"计划长度"把 **2 帧排在 2 个 vblank 里**；
3. 剩下的 2 个 vblank 没有画面 ⇒ `out: vblanks > presents`，眼睛看到的就是**顿一下**。

**处理（已实现，默认开）`--fg-rt-spike 1`**：当**上一帧**的间隔明显长于计划
（`> 1.35 ×` 计划且 < 100 ms）时，把**这一帧**的真实长度（吸附到整 vblank）喂给 XeFG
⇒ 同样 2 帧被摊在更长的间隔里 —— "动得慢一点"而不是"卡一下"。
封顶 3× 计划（真正的大卡顿不追）。日志：

```
[bridge] rt spike: last input 33.3ms (4.0 vblanks) > plan 16.7ms - frameRenderTime 33.3ms ...
[bridge]   rt spikes: 2 frame(s) reported at their real length this window (--fg-rt-spike 1)
```

**A→B 实测（2x@60，各 45 s，游戏内窗口）**：

| 配置 | 窗口 | `arr>2vb` | `empty vb` | `load` |
|---|---|---|---|---|
| `--fg-rt-spike 1`（默认） | 64 | 5.3% | 4.8% | 39% |
| `--fg-rt-spike 0` | 58 | 10.0% | 8.5% | 48% |

⚠️ **这组数字不能当结论**：B 那 45 s 里明显动得更猛（`arr>2vb` 10% vs 5.3%、`skip` 12 vs 4），
场景不同；而且 `empty vb` 这个指标**本身分不清"摊开"和"卡一下"**
（两种情况都是 2 帧 / 4 个 vblank）。判断只能靠眼睛：
**同一个位置、同样速度转视角，`--fg-rt-spike 1` 与 `0` 各玩 30 秒**。

想更彻底地填掉空洞，用另一半开关：**`--interp 2 --interp-cadence`**
（那一档实测空刷新 2.2%，因为它为晚到的帧多要一帧生成帧，而不是把时间拉长）。

### 3.14 四组终测：`raw` / `quant`+spike / `quant` 无 spike / `interp-cadence`

同一场景、2x@60、各 45 s，只统计游戏内窗口（`scripts/perf_windows.py`）：

| 组 | 窗口 | `arr>2vb` | `empty vb` | **空刷新 / 迟到**（归一化） | `xN` | `gpu` | `skip` |
|---|---|---|---|---|---|---|---|
| `--fg-rt-mode raw` | 63 | 6.3% | 5.0% | 0.79 | 2.00 | 2.8 | 2 |
| `--fg-rt-mode quant`（默认 + spike） | 63 | 7.4% | 6.1% | 0.82 | 2.00 | 2.7 | 2 |
| `quant --fg-rt-spike 0` | 61 | 7.3% | 6.7% | 0.92 | 2.00 | 4.0 | 3 |
| **`--interp 2 --interp-cadence`** | 55–61 | 8.6% | **2.8–3.1%** | **0.33** | 2.00–2.35 | **1.7–2.3** | 0 |

读法：

1. **"把真实间隔全喂给 XeFG"（`raw`）在输入锁死的场景下和 `quant + spike` 等价**
   （归一化 0.79 vs 0.82）—— 因为稳态下两者给出同一个数。一旦输入波动，`raw` 最抖
   （§9.14 实测），所以"永远喂 raw"只在锁定输入下安全。
2. **`--fg-rt-spike` 真实有效**：同样的迟到率下空刷新少约 11%（0.82 vs 0.92），
   且只在 7% 的帧上介入（这轮 63 个窗口共命中 **185 帧**）。
3. **`--interp-cadence` 是四组里唯一把空刷新减半的**（0.33），`gpu` 还更低、`skip=0`。
   它按"这一帧实际过了几个 vblank"多要 0.2–0.35 帧（`xN` 2.00→2.35），
   正好补上晚到的那一拍 —— **§9.18 的"失败"结论已撤回**（那是 §3.7 假间隔造成的）。
4. 顺带修掉 cadence 模式的日志 bug：`avg 1.00x / 60.0 fps out` 漏了 `+1`，
   实际是 2.0x / 120 fps，现在显示正确。


### 3.15 "突然跳到 148 Hz" —— 是 `show=` 的假象（已修），不是 FG 超额投放

用户假设：没有过滤器，前面缺的输入被一股脑投放给下一批，导致 FG 剧烈抖动。

**先查有没有真的超额投放**（`--interp 2 --interp-cadence`，61 个游戏内窗口）：

| `arr(vb)` | `presents` | `vblanks` | 超额 |
|---|---|---|---|
| `2x=26 3x=1 4x=13` | 95 | 107 | **-12** |
| `2x=28 3x=7 4x=4 5x=1` | 92 | 99 | -7 |
| `2x=30 3x=5 4x=5` | 91 | 96 | -5 |

**`presents` 从来没超过 `vblanks`**，`xN` 恒定 2.00 ⇒ XeFG 没有超额投放，面板也没被灌爆。

**148 Hz 的真正来源**：`show=` 原来是

```
show = 1000 / g_frame_ms_ema * ps.framesPresented      // 循环墙钟间隔 * 每批帧数
```

停顿之后 WGC 手里有几帧积压，循环连着处理（每帧各自 Present），`g_frame_ms_ema` 瞬间变小
⇒ `show` 读到 **148 / 164**，而面板才 120 Hz —— 它一个数都不代表输出。
现在改成交换链真实值 `show = 本窗口 presents / 秒`，同配置实测最大 **122**（120 + 取整）。

**顺带把"积压帧被连续处理"变成可测量**：新增 `bursts:` 计数（处理间隔 < 0.6× 输入间隔的帧数）。
实测罕见：63 个窗口里只出现 3 次、共 10 帧（约 0.4%），这些窗口的 `presents` 仍 ≤ `vblanks`。

**为什么不加"输入帧过滤器"**：

1. 池层面已经在丢 —— `--slack 0`（默认 drain-newest）丢弃积压帧，`skip=` 就是那个计数；
2. 加"最小处理间隔"＝自己配速，会和 XeFG 的 pacer 抢（§9.18 的教训：两个管理者打架时输入崩到 13-16 fps）；
3. 真正有效的两个手段已经就位：**`--fg-rt-spike`**（迟到帧喂真实长度，摊开而不是排满）
   和 **`--interp-cadence`**（晚到就多要一帧，实测空刷新 3.1% vs 纯 2x 的 5.0-6.7%）；
4. 想要"积压帧延后处理而不是丢弃/连续处理" = `--jbuf 1`（+1 帧延迟），可按需 A/B。

### 3.16 `--jbuf 1` A/B：没有可测收益，别加

2x@60、各 45 s、只统计游戏内窗口（`--interp 1`，其余相同）：

| 配置 | 窗口 | `arr>2vb` | `empty vb` | **空刷新/迟到** | `gpu` | `present` | `load` | `bursts` 窗口 | `presents` |
|---|---|---|---|---|---|---|---|---|---|
| 基线（`--slack 0`） | 59 | 7.7% | 6.4% | 0.83 | 2.9 | 4.5 | 42% | 2 | 80 |
| **`--jbuf 1`** | 64 | 5.0% | 4.3% | **0.86** | 3.2 | 4.9 | 46% | 6 | 80 |

读法：

1. **归一化后两者一样**（0.83 vs 0.86）。B 的绝对空刷新更低，只是因为那 45 s 里输入本来就少迟到
   （`arr>2vb` 5.0% vs 7.7%），不是 jbuf 的功劳。
2. `--jbuf 1` 反而让 `gpu`/`present`/`load` 略升，`bursts` 窗口从 2 增到 6
   （它把颜色拷走后就立刻能处理下一帧，循环更容易连着跑）。
3. 它应该换来 **+1 帧延迟**，但实测 `age` 反而从 28.5 掉到 10.4 ms —— 那是场景漂移
   （B 那段时间 WGC 更新鲜），不能算收益，也不能算损失。
4. `presents` 两边都恒为 80 ⇒ 没有超额投放，`--jbuf 1` 也没有改变投放量。

**结论：不加 `--jbuf`。** 平滑优先级交给 `--interp 2 --interp-cadence`（§3.14，归一化 0.33），
低延迟优先级保持默认 `--slack 0`（drain-newest）。

### 3.17 "最平滑"配方在三种输入帧率下的实测（`--interp-cadence` 自适应）

配方：`--interp 2 --interp-cadence --unlock-mfg --den 2 --pool 4`（4x 输入要改成 `--interp 3`，见下）。
`--interp-cadence` 按"两个真实帧之间过了几个 vblank"逐帧定倍率，所以 **同一套参数**
在 60/40/30 fps 输入下分别是 2x/3x/4x，输出都是 120 fps 整拍。

| 输入锁 | 期望倍率 | 窗口 | `arr>2vb` | **空刷新** | 归一化 | `xN` | `gpu` | `present` | `show` 最大 |
|---|---|---|---|---|---|---|---|---|---|
| **60 fps（2x）** | 2 | 90 | 7.2% | **2.5%** | 0.35 | 2.00（晚到帧 2.02–2.33） | 2.5 | 6.0 | 122 |
| **40 fps（3x）`--interp 2`** | 3 | 64 | 30.1% | **10.8%** | 0.36 | 2.5–2.7 | 1.5 | 2.8 | — |
| **40 fps（3x）`--interp 3`** | 3 | 63 | 44.7% | **1.5%** | — | 3.10–3.27 | 5.7 | 7.3 | 121 |
| **30 fps（4x）`--interp 3`** | 4 | 48 | 10.3% | **2.6%** | 0.25 | 3.88–3.98 | 2.0 | 4.3 | 120 |

> **40 fps 那一行是本轮最重要的发现**：这一档的输入**不是**稳定的 3 个 vblank，实测 `arr(vb)`
> 是 `2x=13 3x=14 4x=13`（平均正好 25 ms，但每帧在 2/3/4 个 vblank 之间跳）。
> `--interp-cadence` 遇到 4-vblank 的间隔要 4 帧才填满，而 `--interp 2` 只解锁到 3x（kmax=3），
> 于是被截断 ⇒ 每窗口 `presents` 102–114 而 `vblanks` 118–122（**每窗空 6–20 个**，10.8%）。
> 换成 `--interp 3`（解锁到 4x，kmax=4）之后：`presents` 117–130 vs `vblanks` 116–137，
> **空刷新 1.5%**，`xN` 3.10–3.27，`show` 最大 121。
>
> ⇒ **`--interp-cadence` 必须留出比标称多一点的头寸**：60 fps 用 `--interp 2` 够，
> **40 fps 必须 `--interp 3`**。稳妥做法是统一用 **`--interp 3`**（kmax=4）：它在 60 fps 下
> 照样按 2x 跑，只是多了 4-vblank 间隔的填洞能力。
>
> **30 fps（4x）实测反而是三档里最干净的**：`arr(vb)` 是 `4x=32 5x=4 3x=2 6x=1`（比 40 fps 稳得多），
> kmax=4 正好覆盖标称 4 个 vblank ⇒ `xN` 3.88–3.98、`presents` 152–158 vs `vblanks` 156–161、
> **空刷新 2.6%**、`show` 最大 120、**没有任何超额投放**（max over = 0）。

**三档汇总（都只统计游戏内窗口）**：

| 输入 | 命令 | `xN` | 空刷新 | 预算占用 `load` | `gpu` | `present` |
|---|---|---|---|---|---|---|
| 60 fps × 2x | `--interp 2`（`--interp 3` 也可以） | 2.00–2.33 | 2.5% | 49% | 2.5 | 6.0 |
| 40 fps × 3x | **`--interp 3`** | 3.10–3.27 | **1.5%** | 51% | 5.7 | 7.3 |
| 30 fps × 4x | `--interp 3` | 3.88–3.98 | 2.6% | 19% | 2.0 | 4.3 |

三档都在 `1.00–1.04 vblank/帧`，`slow ≈ 0`，`show` ≤ 121（面板 120 Hz）。
**统一用 `--interp 3 --interp-cadence --unlock-mfg` 一套参数覆盖 60/40/30 三档。**

对照（同一 60 fps 输入）：

| 配置 | 空刷新 | 归一化 |
|---|---|---|
| 纯 2x（默认 `quant` + spike） | 6.4% | 0.83 |
| **`--interp 2 --interp-cadence`** | **2.5%** | **0.35** |

补充：`show` 最大 122（`show=` 已改为交换链真实 Present 速率，§3.15）；
`presents` 80–93 且始终 ≤ `vblanks`（无超额投放）；`rt spikes` 命中 55/90 个窗口（晚到帧都被
按真实长度喂过），`bursts` 只有 4 个窗口。

**4x 的前提**（`--fg-probe --unlock-mfg --interp 3` 实测）：

```
[bridge] probe: maxSupportedInterpolations (stock) 1 -> 2x
[bridge] probe: maxSupportedInterpolations 3  ->  4x output
[bridge] probe: VERDICT MFG available, up to 4x (--interp 2..3 = 3x..4x).
```

⇒ 补丁是按**请求的倍率**打的：`--interp 2` 只解锁到 3x，要 4x 必须 `--interp 3`。
30 fps 输入（4 个 vblank/帧）若只用 `--interp 2`，cadence 会被截到 3x ⇒ 90 fps，不是 120。

### 3.18 把 `--interp` 提到 5（kmax=6）：三档空刷新全面下降，代价是负载 81–85%

`--unlock-mfg` 的补丁是**按请求解锁**的，`--interp 5` 会打 6x：
`--unlock-mfg: target 6x (5 interpolated) ... all 6 sites patched` → `maxSupportedInterpolations = 5`。
配合 `--interp-cadence`，kmax 从 3/4 提到 6 —— 连 5–6 个 vblank 的抖动帧也能填满。

同一台机器、4K、`--den 2`、各 60 s、只统计游戏内窗口：

| 输入 | `--interp`（kmax） | 窗口 | `arr>nom` | **空刷新** | `load` 中位 / 最大 | `gpu` | `present` | `slow>0` 的窗口 |
|---|---|---|---|---|---|---|---|---|
| 60 fps（2x） | 2（3） | 90 | 7.2% | 2.5% | 49% / — | 2.5 | 6.0 | 2 |
| 60 fps（2x） | **5（6）** | 89 | 7.6% | **0.8%** | 85% / 111% | 6.7 | 8.2 | 3 |
| 40 fps（3x） | 3（4） | 63 | 44.7% | 1.5% | 51% / — | 5.7 | 7.3 | 3 |
| 40 fps（3x） | **5（6）** | 63 | 34.6% | **0.4%** | 84% / 97% | 10.3 | 10.7 | 2 |
| 30 fps（4x） | 3（4） | 48 | 10.3% | 2.6% | 19% / — | 2.0 | 4.3 | 1 |
| 30 fps（4x） | **5（6）** | 46 | 8.2% | **0.6%** | 81% / 102% | 14.0 | 12.0 | 13 |

读法：

1. **kmax=6 让三档的空刷新都掉到 1% 以下**（0.8% / 0.4% / 0.6%），因为 4–6 vblank 的抖动帧
   现在都能被填满（`cadence hist` 里能看到 `5x=1..4`）。
2. **代价是负载从 19–51% 涨到 81–85%**：DLL 走 6x 容量后每输入帧的 GPU 时间明显变高
   （30 fps 那档 `gpu` 2.0 → 14.0 ms）。四档里 30 fps 的 `slow>0` 窗口最多（13/46），
   原因是它每输出帧只有 8.33 ms 预算，负载 81% 时任何抖动都会顶穿。
3. **三档都还有余量**（最大 load 111%/97%/102%，只有 1 个窗口越界），但余量只剩 15–19%。
   场景一重（快速转视角、流送）就会翻过去 —— 这时 `--fg-rt-spike`（§3.13）和
   cadence 本身的填洞能力把损失控制在 1% 以内。
4. `cadence:` 行在 60 fps 会显示 `0.95–0.96 vblank/frame`：那是**显示口径问题**
   （它拿中位数 16.7 ms 除以窗口内 `k` 的平均值，而窗口内平均间隔其实是 17.3 ms），
   真实值看 `out: presents vs vblanks`（均值 84.5 vs 85.3 = 0.9% 空）。

**结论**：要"最平滑"就是 **`--interp 5 --interp-cadence --unlock-mfg`**（三档通用）；
如果更在意延迟/功耗、或者场景很重，退到 `--interp 3`（60x2 甚至 `--interp 2`）。

### 3.19 动态场景（跑不满面板，无锁帧）：前馈 cadence 完胜闭环 `--dyn-mfg`

用户提议：把游戏调到"跑不满屏的最高负载"（无 RTSS、GPU 受限，实测游戏自己在 25/33 ms 之间跳，
`game>3vb` 10–17%）。这正是动态多帧该被考验的场景。三组各 60 s，4K `--den 2`：

| 组 | 命令 | 窗口 | **空刷新** | 每窗 `presents`/`vblanks` | `max empty` | `xN` | `out` 行 | `load` |
|---|---|---|---|---|---|---|---|---|
| **A** | `--interp 5 --interp-cadence --fg-rt-mode quant` | 59 | **1.0%** | 126.7 / 127.8 | 13 | 3.08–3.27 | 0.94–0.98 | 82% |
| **B** | 同上 + **`--fg-rt-mode raw`** | 60 | **0.7%** | 125.4 / 126.1 | **5** | 3.05–3.17 | 0.94–0.98 | 81% |
| **C** | `--interp 5 --dyn-mfg`（OptiScaler 控制器） | 56 | **7.6%** ❌ | 120.9 / 130.3 | 23 | **3.00 恒定**（54/56 窗口） | **1.33** 有 26 个窗口 | 41% |

读法：

1. **前馈按 vblank 计数的 cadence（A/B）把空刷新压在 1% 以内**，即使游戏自己 40% 的帧
   晚到（`arr>3vb` 40%）也一样 —— 它就是"实际过去几个 vblank 就补几帧"，构造上不需要收敛。
2. **闭环的 `--dyn-mfg` 差 8 倍（7.6%）**：54/56 个窗口固定在 3.00x（控制器有迟滞），
   于是输出在 `1.00` 和 `1.33 vblank/帧`之间来回跳 —— 这就是"动态多帧"最容易踩的坑：
   **倍率变得太慢，填不满中间的洞**。
3. **动态场景里 `raw` 略优于 `quant`**（0.7% vs 1.0%，max empty 5 vs 13，`skip` 15 vs 21）：
   输入从 25→33 ms 摆动时，中位数吸附（quant）会滞后一两个窗口。注意这**只在配合
   cadence 时成立** —— 没有 cadence 时 `raw` 是四档里最抖的（§9.14）。
4. 代价：cadence 的 `load` 81–82% vs dyn-mfg 41% —— 它真的把每个 vblank 都填了，
   换来的是画面不空。要不要这笔交易取决于你是要平滑还是要省 GPU/延迟。
5. `out = 0.94–0.98 vblank/frame` 是**口径问题**（中位数除以窗口内 k 的均值，§3.18），
   真实值看 `presents / vblanks`：0.6–0.9% 空。

**结论**：

| 场景 | 配方 |
|---|---|
| 锁帧（60/40/30） | `--interp 5 --interp-cadence --fg-rt-mode quant`（§3.18） |
| **动态/跑不满** | `--interp 5 --interp-cadence --fg-rt-mode raw`（0.7%） |
| 想要省 GPU（接受 1.33 vblank 的抖动） | `--dyn-mfg` |

### 3.20 `--fg-rt-mode auto`：判定要"按窗口"做，否则会来回翻

目的：锁帧时用 `quant`（中位数吸附整 vblank），帧率真的在漂时用 `raw`（真实间隔）。
两版判据的实测（同一台机器，未锁帧、游戏稳定在 ~40 fps 的状态）：

| 版本 | 判据 | 结果 |
|---|---|---|
| v1 | 快/慢 **EMA** 差 > 0.15 vb，逐帧判 | ❌ 把"锁 40 fps 的 ±1 vblank 抖动"当成帧率在动 |
| v2 | 16 样本中位数 vs EMA，逐帧判 | ❌ **来回翻**：`moving(33.3 vs 25.0) → raw` / `steady(25.0) → quant`，18 s 内翻 4 次 |
| **v3** | 短中位数（16 样本）vs 长中位数（64 样本），差 ≥ 0.6 vb，**每个统计窗口判一次 + 连续 3 个窗口一致才翻** | ✅ 60 s 内 `flips=0`，稳定停在 `quant` |

v3 的实测（未锁帧、帧率稳定在 40 fps 附近）：

```
[bridge]   rt auto: median 25.0 vs 25.0 (0.00 vblanks apart) -> quant  flips=0
[bridge]   rt auto: median 33.3 vs 25.0 (1.00 vblanks apart) -> quant  flips=0   ← 单个窗口不翻
```

| 组（同一状态） | 空刷新（全窗口） | 空刷新（steady） | `max empty` | 判定 |
|---|---|---|---|---|
| `quant` | 1.1% | 1.0% | 13 | — |
| `raw` | **0.8%** | **0.7%** | **5** | — |
| `auto`(v3) | 1.2% | 0.9% | 13 | 稳定停在 quant，`flips=0` |

**结论**：v3 判据安全（不会再误翻），但**保守**：在这种"未锁帧但帧率其实很稳"的状态里它选 `quant`，
比手动 `raw` 差 0.3 个百分点。想要那 0.3 pt 就手动 `--fg-rt-mode raw`；想要"绝不抖"就用 `auto`/`quant`。

**锁帧场景验证（60 fps）**：`--interp 5 --interp-cadence --fg-rt-mode auto` 跑 60 s，
**`flips=0`**、`cadence: rt=16.7ms (auto:quant) … = 1.00 vblank/frame`、
空刷新 **0.8%**（与纯 `quant` 的 0.8% 完全一致）⇒ auto 在锁帧下安静地等于 quant，符合设计。

> 尚未验证的一条：v3 在多长时间、多大变化下真的会切 `raw`。要触发它需要帧率**持续**移动
> （比如从密集街区走到空旷区），日志会出现 `input rate is moving (median … vs …)`。
> 手动测的话：`--fg-rt-mode auto` 不锁帧，在场景间来回走，`flips` 应该 ≥1 且不来回跳。

### 3.21 定稿：120 Hz 面板上的四档设置（4K 捕获，`--den 2` 输出）

一条命令覆盖全部四档：

```
bridge.exe --game <exe> --den 2 --pool 4 --interp 5 --interp-cadence --unlock-mfg
           --fg-ui none --fg-rt-mode auto
```

| 场景 | RTSS 锁 | 实际倍率 | **空刷新** | `load` | `gpu` | `slow>0` 窗口 |
|---|---|---|---|---|---|---|
| **x2**（60 fps 进） | 60 | 2x | **0.8%** | 85% | 6.7 ms | 3/89 |
| **x3**（40 fps 进） | 40 | 3x | **0.4%** | 84% | 10.3 ms | 2/63 |
| **x4**（30 fps 进） | 30 | 4x | **0.6%** | 81% | 14.0 ms | 13/46 |
| **dynamic**（不锁，跑不满） | — | 2–4x 自适应 | **0.7–0.9%** | 81% | 9.1 ms | — |

**降档规则**（唯一需要人盯的数）：日志里 `load` 长期 > 100% 或 `slow` 变多时：

| 场景 | 退到 | 空刷新变成 |
|---|---|---|
| 60 fps | `--interp 2`（kmax 3） | 2.5%（`load` 49%） |
| 40 fps | `--interp 3`（kmax 4） | 1.5%（`load` 51%） |
| 30 fps | `--interp 3` | 2.6%（`load` 19%） |

**不要加的开关**（都有实测反例）：`--jbuf` / `--slack N`（§3.16、§9.9）、`--dyn-mfg`（§3.19，7.6%）、
`--interp-auto`（§9.14，换档顿挫）、`--pace` / `--lock`（§9.16，与 XeFG pacer 抢）。

**前提**：4K 捕获 + `--den 2`（1080p 输出）。若要 4K 原生输出（`--den 1`），预算翻 4 倍，
`--interp` 必须降档（先试 3，再 2），并用 `load`/`slow` 实测确认。

### 3.22 动态逐帧 + 波动输入"一卡一卡"：**逐帧原始间隔**进相位累加器

场景：游戏未锁帧、输入中位数 25 ms（40 fps）但到达在 2/3/4/5/6 个 vblank 之间跳
（`arr(vb): 2x=7 3x=18 4x=17`）。此时空刷新指标看着还行，画面却一顿一顿。

三个假设都实测过（同一波动场景，各 50 s）：

| 假设 | 做法 | 结果 |
|---|---|---|
| A. `frameRenderTime` 与批次长度不一致（喂中位数，却按实际 vblank 数要帧） | `--fg-cad-rt 1`：rt = 批次 × vblank | ❌ 更差：空刷新 1.0% → **2.6%**，出现一个 37 vblank 的大洞 ⇒ 默认关 |
| B. 每帧调 `SetNumInterpolatedFrames` 太贵 | `--cadence-hold 15/30` 限流 | ❌ **白丢刷新**：`gpu` 完全不变（7.1–8.4 ms），空刷新 7.5% → 15.1% ⇒ 默认关 |
| **C. 相位累加器吃的是 `g_arr_last_raw`（逐帧原始间隔）** | `--cadence-smooth N`：先对到达间隔做 N 帧 EMA | ✅ **命中且双赢** |

C 的机理：一帧迟到（4x）就要 4 帧、下一帧早到（2x）就要 2 帧，倍率跟着抖动 **1:1**，
长期平均没错但画面节拍每帧都变——这就是"一卡一卡"。EMA 给出同样的平均速率，但档位稳定：

| 配置 | `switches` 中位/最大 | 空刷新（全部窗口） | 空刷新（稳态窗口） | `max empty` |
|---|---|---|---|---|
| `--cadence-smooth 1`（等于旧行为） | 29 / 37 | 3.9% | 3.8% | 27 |
| **`--cadence-smooth 8`（新默认）** | **12 / 26** | **1.3%** | **0.9%** | 24 |
| `--cadence-smooth 16` | 6 / 14 | 1.7% | 0.7% | 53 |

⇒ 档位切换少一半以上，**空刷新同时下降**（与 B 的取舍不同，这是纯收益）。16 帧开始反应偏钝，
遇到一次场景停顿会出现 53 vblank 的大洞，所以默认取 8。

三个开关的默认值：`--cadence-smooth 8`、`--cadence-hold 0`（关）、`--fg-cad-rt 0`（关）。

### 3.23 固定倍数模式"没以前丝滑"：**不是回归**，是固定倍数的结构上限

用户报告：动态逐帧改好后，固定模式反而觉得没以前丝滑。三件事逐条查过。

1. **代码路径没动。** `--cadence-smooth/--cadence-hold/--fg-cad-rt` 三处新代码全部在
   `if (g_interp_cadence)` 之内（`cadence_next_slots()` 只在动态模式下被调用），
   `fg_render_time_ms()` 里那条也被 `g_interp_cadence && g_fg_cad_rt` 挡住。
2. **配对 A/B（0.11.2 的 bridge 5b360904 vs 新版，固定 `--interp 2`，交替各 3 轮）：**

| 轮 | 新版空刷新 | 旧版空刷新 | 该轮 `game` 中位数 |
|---|---|---|---|
| 1 | **1.0%**（max 4） | 3.0%（max 28） | 20.7 / 20.6 ms |
| 2 | 4.6%（max 31） | **1.6%**（max 26） | 19.5 / 19.3 ms |
| 3 | 1.0%（max 6） | **0.1%**（max 2） | 18.3 / 18.3 ms |

⇒ 同一份程序不同轮之间就差 4.6 倍，**轮间波动远大于两版之差**，没有回归。
（第一次非配对的"旧 0.6% vs 新 1.6/3.2%"就是场景漂移造成的误判。）

3. **结构原因才是真的。** 固定模式下 `输出 = 转发帧率 × 倍数`，转发率由 bridge 按队列深度 1
   节流到 ~40 fps，一旦游戏掉到 30-50 fps 之间抖动，输出就离开 120 Hz，出现空洞；
   动态逐帧能把这段抖动吸收掉。所以"动态变好了"会直接让固定显得更差——这是对比效应。
   平稳场景下固定 3x 实测 **0.1–0.3% 空刷新**、`max empty=2`，已经很干净。

顺带试过的最后一个候选：**`--fg-rt-mode ema`**（把转发间隔的 EMA 喂给 pacer，而不是
中位数吸附整 vblank）。配对 3 轮，`auto` 0.1/0.1/0.3%，`ema` 0.2/0.4/0.5%（最大空洞 4-6 vs 2）
⇒ **更差**，`ema` 只作为 A/B 档保留，UI 仍用 `auto`。

### 3.24 `--fg-cad-rt`（frameRenderTime = 批次 × vblank）：理论对、实测**更差**

推理看起来无懈可击：输入不稳时 k 只能在 3/4 之间跳（35–45 fps 要 2.67–3.43 帧/真帧），
k=4 的批次要铺满 33.3 ms，而喂中位数（25.0 ms）会让 4 帧挤进 3 个 vblank 再空等 1 个。
所以做了 `--fg-cad-rt 1`：rt = 批次 × vblank。**人眼测出来更差，且 xN 抓到了原因：**

| 喂法 | `input` 中位数 | xN | presents/窗口 |
|---|---|---|---|
| `quant`（中位数，默认） | 25.0 ms | **3.00 / 3.00** | 120 / 121 |
| `cad:batch`（批次 × vblank） | 25.0 ms | 3.05 / **3.38** | 122 / 135 |

输入一直是 25.0 ms（40 fps），批次喂法下 xN 却漂到 3.38 —— 因为 **`Present()` 会按 pacer 的
预算阻塞，而输入间隔是在下一个 WGC 帧到达时量的**：预算抬高 → 我们回得更晚 → 量到的间隔变长
→ k 往上爬 → 预算更高…… 队列深度 1 下"输入帧率"本来就是**我们自己节流出来的**，这个环路
没有不动点（同样的机制在 §3.23 的固定模式和 `--interp-cadence` 早期失败里都出现过）。
中位数喂法则把 xN 钉在理想的 3.00 上，所以画面稳。

**结论：`--fg-cad-rt` 默认 `0`（关）**，只作为 A/B（发布包 `6 - cadence batch feed (A-B).bat`）。

空刷新指标在这件事上完全无用：交替各跑两轮，`cad:batch` 0.8% / 2.8%、`quant` 1.6% / 1.7%，
差别落在场景噪声里，而 xN 与人的观感一致。**能看"丝滑"的指标是 xN 是否守在整数值**，
不是空刷新比例。

（`--cadence-smooth 8` 仍然有效且保留默认：它让 k 不要在 3/4 之间每帧乱跳，切换次数 29→12。）

### 3.25 动态逐帧 + 运动矢量：观感崩（**只有人眼结论，数据待补**）

用户实测（2026-11-09）：动态逐帧下 `--no-mv` 非常好，开 MV（游戏 MV / `--fg-of`）就有问题，
固定模式不受影响。

⚠️ 我按 no-mv / game-mv / of 跑过三组，**那批数据作废**：环境没准备好（游戏不在同一场景），
`mv_game` 那组量到的 `input_med=16.7ms`、`cadence hist: 2x=17 3x=19`、`xN=2.35` 全是被测环境的
假象，不能用来下结论。等环境就绪后重测，矩阵：

| MV | cadence 默认 | +`--cadence-hold 60` | 固定 3x |
|---|---|---|---|
| `--no-mv` | 基准（已很好） | — | 对照 |
| 游戏 MV | 用户说差 | 待测 | 用户说正常 |
| `--fg-of` | 用户说差 | 待测 | §5.19 说 3x 下首选 |

假设（来自 `FINAL_CONFIG` §5.17）：3x/4x 是"2x 模型跑两遍 + 同一份 MV 算不同时间分数"，
不自洽本身产生闪烁；固定倍数下这个不自洽恒定，cadence 下 k 每帧变、分数跟着变，所以观感崩。

## 4. 明确不要碰

- 深度延迟 / 投影 / FOV —— 实测无差别（`CONCLUSIONS.md` §1）；
- 颜色拷贝（`Color=1`）—— 除非先补消费端通路，否则纯亏（`ROADMAP.md` §6）；
- 消费者侧锁帧、跨卡 fence 轮询 —— 都已证伪（`CONCLUSIONS.md` §3）；
- XeFG 内部参数调优 —— 闭源，只有 `--fg-*` 那几个胶水开关；
- 复用 `wait_slot` 的等待事件（见 §3.2）。

## 5. 下一轮怎么测（一次跑完，20 分钟）

**测试环境**（一直是这一套）：`C:\temp\mgpu\bin` 放 `bridge.exe` + 四个 Intel DLL +
`run_mgpu*.bat`；游戏是 `F:\steam\steamapps\common\PBZ\platform\steam\PBZ.exe`，
add-on 的 `game_capture.ini` 就在游戏目录里。本轮的新 `bridge.exe`（`43f263f5…`）**已经
部署在那里**，旧的留在 `bridge.exe.prev_43f2`，回滚就是换回文件名。

**无游戏烟雾测试已通过**（不需要开局，随时可复跑）：

| 命令 | 结果 |
|---|---|
| `bridge.exe --list-modes` | exit 0，11 个刷新率都列出，`--fg-rt-mode quant` 默认生效 |
| `bridge.exe --fg-probe` | exit 0，AMD 6750 GRE 上 `CreateContext/GetProperties` SUCCESS，`libxess_fg 1.3.1`，`maxSupportedInterpolations 1`（未开 `--unlock-mfg`，符合预期） |
| `bridge.exe --game NOSUCHGAME.exe --report 40` | exit 2，日志**完整到最后一行**（证明日志节流没有丢尾部：`hard_exit` 会 flush） |
| 同上 + `--log-flush` | 同上，逐行 flush 路径也正常 |

**A/B 怎么跑**（游戏开着、在平时那个位置、RTSS 仍锁 40 fps 的前提下）：

> 前置条件严格三条，缺一条数据就不能用：① 游戏的 DLSS/渲染模式**两次跑之间不许变**
> （看 `bus:` 行的 mv/depth 尺寸是否一致）；② RTSS 锁 40 fps，`cadence` 必须是
> **1.00 vblank/frame**（不是 `NOT-EXACT`）；③ 同一个位置/视角，别在跑的时候走动。
> 2026-11-08 23:1x 那次首跑就是因为 ① 和 ② 没满足，只能证明"二进制没坏"，
> 证明不了"省了多少 ms"（见 §3.5）。

| 组 | 怎么跑 | 看什么 |
|---|---|---|
| A | 双击 `C:\temp\mgpu\bin\perf_AB.bat` 的第一段（脚本自动跑 A 和 B） | 基线：`gpu` / `total` / `cpu` / `bus:` |
| B | 同上（脚本第二段，自动加 `--no-depth`） | `gpu` 应降 ~1.8 ms（4K/DLAA）或 ~0.5 ms（1440p 渲染），`bus:` 少一半；确认 `gen N/N` 不掉、画质无可见差别 |
| C | 见 `perf_AB.bat` 同目录的 `perf_C_README.txt`（`StreamPercent=150` 需要重启游戏，add-on 只在 init 读 ini） | `bus:` 应降到 ~44%，`gpu` 降；确认 MV 仍被消费 |
| D | `--log-flush` 已并进 A/B 两组（脚本 kills bridge，不 flush 会丢尾部） | 只用来确认日志节流没有掩盖问题 |

两个脚本都留在测试环境里：`perf_AB.bat`（自动 A+B 并调用分析）、`perf_ab.py`
（解析 `breakdown`/`cadence`/`bus` 三行，打印中位数与差值）。仓库里的同名副本是
`scripts/perf_ab.py`，用法：

```
python3 scripts/perf_ab.py --skip 3 perf_A.txt perf_B.txt
```

判定：**只要 A 的 `cadence` 是 1.00、`slow` = 0、`cpu` < 2 ms，本轮改动就是安全的**；
B/C 的收益按 `gpu` 与 `bus:` 的下降量记账，不看 fps（paced loop 里 fps 由锁帧决定）。


## 6. 未做的小尾巴（不影响性能，但会误导排查）

| 现象 | 位置 | 说明 |
|---|---|---|
| `--unlock-mfg` 在 2x 时打印 "The patch did not take" | `bridge.cpp` 的补丁校验 | **虚警**：2x 本来就不需要改数量上限 |
| 注释说 `--input-ms` 决定 `frameRenderTime` | `bridge.cpp` 的帧时间注释 | 只在 EMA 模式成立；`quant`（默认）用实测中位数 |
| `--no-depth` 的画质结论在文档里自相矛盾 | `FINAL_CONFIG.md` §4.5 说"掉质量"、§9 两组对照说"画质不变" | 需要一次明确的 A/B 定论，否则没人敢用它省带宽 |
| `frame_id ↔ WGC SystemRelativeTime` 严格配对 | —— | 零带宽、几十行，改善**可观测性**；做法见 `ROADMAP.md` §5 |
