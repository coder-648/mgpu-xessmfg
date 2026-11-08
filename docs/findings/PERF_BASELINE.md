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
