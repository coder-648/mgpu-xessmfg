# 性能基线与优化清单

> 这一页把**已经测过的**和**还没测的**分开：§2 是基线（**不要再测一遍**，直接引用），
> §3 是候选（都要实测确认后才改代码）。画质相关的变量看
> [`QUALITY_VARIABLES.md`](QUALITY_VARIABLES.md)，结论摘要看
> [`CONCLUSIONS.md`](CONCLUSIONS.md) §7。

实测机器：渲染 RTX 3080 Ti，帧生成 RX 6750 GRE，120 Hz 面板，Windows 11 WDDM 3.2
（环境细节见 [`ENV_REPORT.md`](ENV_REPORT.md)）。**换卡/换游戏这些数字都会变**，
它们只用来判断"哪个环节值得动"。

## 1. 怎么量（固定跑法 + 判据）

固定场景 + 固定外部锁帧 + 同一段 40 帧窗口做 A/B，只看 `--report` 输出的这一行
（`src/bridge/bridge.cpp` 的 `run_frame` 尾部）：

```
[bridge]   breakdown: capture= work= present= gpu= wait= copy= mvs= of= age= xell= fg= total=
           timeouts= mode=  xN=  game=  jitter=..  arr_gaps=  load=  slow=  pace= repres= skip=
           worst(gpu/pres/wait/copy/mvs/of/age/xell/fg/total/gap) xNmiss=
```

| 字段 | 含义（都在 `run_frame` 里量） |
|---|---|
| `capture` | 等下一帧 WGC 帧（`TryGetNextFrame` 的阻塞） |
| `work` | 从取到帧到提交命令列表之间，我们自己的 CPU 开销 |
| `present` | `Present()` 的阻塞时长 —— **XeFG 的放行点** |
| `gpu` | 等上一帧 GPU 完成（`gpu_throttle()`，深度 1 帧；`--no-throttle` 时为 0） |
| `wait` | 等槽位 fence（`wait_slot()`），超时计入 `timeouts` |
| `copy` | 等"WGC 帧 → 我们的 D3D11 承接纹理"这次同卡拷贝在 D3D12 队列上完成（4K 66 MB / 5K 118 MB） |
| `mvs` | 录 MV/深度**本地**拷贝的时间（这些拷贝会把物理上在渲染卡的数据拉到插帧卡） |
| `of` | 本地光流（`--fg-of`） |
| `age` | WGC `SystemRelativeTime` → 我们取到手 = 游戏合成 → 本进程这一跳 |
| `xell` | `xellSleep()` 的阻塞时长 |
| `fg` | `xefgSwapChain` 的标注（tagging）调用 |
| `total` | 整个 `run_frame` |
| `game` / `jitter` | 生产端帧时间 EMA / 到达间隔的 min..max |
| `arr_gaps` / `xNmiss` | 到达间隔例外（丢帧）计数 / 倍率判定缺失数 |
| `load` | `(gpu + present) / 输入间隔` —— paced loop 的占空比 |
| `slow` | 超过输入间隔的帧数 |
| `pace` / `repres` / `skip` | `--pace` 的补偿量 / 重播帧 / 跳过的帧 |

**判据优先级（从上往下，前一条不满足就别看后面）：**

1. 状态行的 `... %.1f fps out = %.2f vblank/frame` 应当是 **1.00**（现在是默认值
   `--fg-rt-mode quant` 的功劳）；`= 1.00` 之外的 `NOT-EXACT` 标记就是周期性丢帧的来源；
2. `slow` = 0；`load` 95–100% 是 paced loop 的**正常值**，不是告警；
3. `gpu` + `present` ≈ 输入间隔（这两项就是"节拍在哪等"）；
4. `of` ≈ 0.3 ms、`copy` ≈ 0.1 ms、`mvs` ≈ 0 → 这些不是瓶颈，别再查；
5. `age` 5–20 ms 正常，> 50 ms 说明**合成器迟到**（不是我们的循环慢）。

> `worst(...)` 是历史最大值，只看量级，**不当均值用**。

可用工具：`--report N`、`--list-modes`、`--fg-probe`、`--dump` +
`scripts/analyse_dump.py`、`--probe-colour`、`--mv-shadow`。命令写法见
[`TOOLING_NOTES.md`](TOOLING_NOTES.md)。

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

## 3. 候选清单（按性价比排序，全部需要实测确认）

| # | 点 | 位置 | 预期收益 | 风险 | 怎么验 |
|---|---|---|---|---|---|
| **1** | 每帧两次 `CreateEventW` + `CloseHandle` | `bridge.cpp` `wait_slot()` / `gpu_throttle()` | 小（µs 级）但白给；60–120 fps 下每秒 120–240 次内核对象创建 | 低 | 改前后比 `total` / `wait` 均值 |
| **2** | WGC 颜色链路的显存流量 | 每帧 `CopyResource`（4K FP16 66 MB）+ FP16→R10G10B10A2 全屏转换（读 66 + 写 33）≈ **165 MB/帧**，@120 fps ≈ 20 GB/s 显存带宽 | 省一次中转可降显存压力/功耗 | 中（跨 D3D11/D3D12 必须走 shared handle） | 先量 `total - gpu - present` 在 4K vs 1440p 的差 |
| **3** | add-on 每帧的全屏 pass（MV downsample + depth convert） | `game_capture.cpp` 的 `record_downsample()` / `record_depth_convert()` | 省一次 pass ≈ 渲染卡每帧几 ms | 中（格式不同，合并要改 shader） | `StreamPercent` 对照 + 渲染卡 GPU 占用 |
| **4** | `bridge.cpp` 单 TU（约 6.9k 行） | 帧循环 / OSD / 5 个诊断工具混在一起 | 可维护性、编译时间；**不解性能** | 低 | 纯重构，编译通过即可 |
| **5** | `gpu_throttle` 深度 vs 吞吐 | 现为 1 帧在飞（最低延迟），`--no-throttle` 换吞吐 | 二者互斥，需先定优化目标 | 低 | 同场景 A/B `total` 与 `compose → photons` |
| **6** | `logf_` 每行 `fflush(stdout)` | `bridge.cpp` 顶部的 `logf_()` | 只在 `--dump` / 诊断时明显 | 极低 | 大 dump 时的 CPU 占用 |

### 3.1 候选 1 的注意点（复用事件不能盲改）

`wait_slot()` 与 `gpu_throttle()` 现在每帧新建一个自动重置事件。改成常驻事件时，
**迟到的信号会污染下一次等待**：上一帧的 fence 值如果在超时之后才 retire，事件会被
置位，下一帧的 `WaitForSingleObject` 立刻返回并记一次假的 `WAIT_OBJECT_0`
（顺带把 `g_fwd_expect` 推错）。可行的做法是复用事件 + 在等待前确认
`GetCompletedValue() >= 目标值`，否则先把事件消费掉；或者只在 `--wait-ms` 足够大
（实际不超时）的路径上复用。`dump_flush()` / `--probe-colour` / `--fg-of-selftest`
里的那三处是**一次性**的，不用动。

另外：MV 的 downsample pass 只在消费端真的要 MV 时才录
（`g_mv_active`），所以候选 3 的"每帧两个 pass"只发生在 MV 开启的配方里；
`--no-mv` / `--fg-of` 配方下这一项本来就不花时间。

## 4. 明确不要碰

- 深度延迟 / 投影 / FOV —— 实测无差别（`CONCLUSIONS.md` §1）；
- 颜色拷贝（`Color=1`）—— 除非先补消费端通路，否则纯亏（`ROADMAP.md` §6）；
- 消费者侧锁帧、跨卡 fence 轮询 —— 都已证伪（`CONCLUSIONS.md` §3）；
- XeFG 内部参数调优 —— 闭源，只有 `--fg-*` 那几个胶水开关。

## 5. 未做的小尾巴（不影响性能，但会误导排查）

| 现象 | 位置 | 说明 |
|---|---|---|
| `--unlock-mfg` 在 2x 时打印 "The patch did not take" | `bridge.cpp` 的补丁校验 | **虚警**：2x 本来就不需要改数量上限 |
| 注释说 `--input-ms` 决定 `frameRenderTime` | `bridge.cpp` 的帧时间注释 | 只在 EMA 模式成立；`quant`（默认）用实测中位数 |
| `frame_id ↔ WGC SystemRelativeTime` 严格配对 | —— | 零带宽、几十行，改善**可观测性**；做法见 `ROADMAP.md` §5 |
