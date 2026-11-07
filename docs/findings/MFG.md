# MFG：多帧生成（**已在本机跑通**）

> **结论（2026-11-06 实测）**：不需要换 DLL。官方 `libxess_fg.dll` 里 MFG 的代码是全的，
> 只是被 4 个字节 + `libxell` 里 1 个字节卡住。我们加了 `--unlock-mfg` 把它们改掉，
> 真机验证 `maxSupportedInterpolations` 从 **1 → 3**：
>
> ```
> [bridge] --unlock-mfg: all 6 sites patched and read back OK
> [bridge] probe: maxSupportedInterpolations 3  ->  4x output
> [bridge] probe: VERDICT MFG available, up to 4x.
> ```
>
> 验证命令（**不需要开游戏**）：`probe_fg.bat`（不带参数跑同目录 DLL）。

---

## 1. 之前的误判，以及真正的机制

我先前的结论是"官方 DLL 在非 Intel 卡上是硬性 2x，必须换社区构建"。**这是错的**，
因为我只看了 DLL 的字符串和 `GetProperties` 的返回值，没有看别人怎么绕过它。

真正的做法来自 **Universal MFG Unlocker**（`dashdogy/MFG-Unlocker`，MIT）——
OptiScaler 的 `[XeFG] UnlockMFG` / 日志里的 `XeLLUnlock::Apply` 就是同一套
（你的"Master 特殊构建"来自 `dashdogy/OptiScaler` 的 mfgunlocker 分支）。

它做的事情非常克制：**改 6 处立即数/跳转**，然后用 `GetProperties` 回读确认。

| # | 模块 | RVA | 原始字节 | 含义 |
|---|---|---|---|---|
| 1 | `libxess_fg.dll` | `0x1A45C2` | `C7 87 6C 01 00 00 01 00 00 00` | `mov dword [rdi+16Ch],1` 覆盖钳制 |
| 2 | `libxess_fg.dll` | `0x1A517D` | `BB 03 00 00 00` | `mov ebx,3` 默认上限 |
| 3 | `libxess_fg.dll` | `0x20973B` | `B8 01 00 00 00` | `mov eax,1` **`GetProperties` 上报的最大值** |
| 4 | `libxess_fg.dll` | `0x1A5DE4` | `74 09` | "切到 MFG 模型"的谓词（模型 18→16 在这份里不存在）→ `EB 06` 恒跳，让**每个插值帧复用 2x 模型** |
| 5 | `libxess_fg.dll` | `0x21B86B` | `83 F8 01 74 0F 83 FA 01 74 0A` | 实时改帧数时"变到/变回 1 就重建管线"的两个比较 → 都改 0 |
| 6 | `libxell.dll` | `0x00D1C9` | `83 FB 03 76 07` | `xellSetGeneratedFramesCount` 的 `cmp ebx,3; jbe` 上限 |

**我们这份 DLL 的 6 处字节与参考版完全一致**（逐点比对，脚本 `scripts/check_sites.py`）：

```
fg_override_clamp          RVA 0x1A45C2  C7 87 6C 01 00 00 01 00 00 00   MATCH
fg_default_ceiling         RVA 0x1A517D  BB 03 00 00 00                 MATCH
fg_reported_maximum        RVA 0x20973B  B8 01 00 00 00                 MATCH
fg_keep_2x_model           RVA 0x1A5DE4  74 09                          MATCH
fg_live_count              RVA 0x21B86B  83 F8 01 74 0F 83 FA 01 74 0A  MATCH
ll_generated_gate          RVA 0x0000D1C9  83 FB 03 76 07               MATCH
6/6 sites match the reference layout
```

> 这也解释了那三条字符串：`Expected maxInterpolatedFrames to be 1` = 第 1/3 处的检查；
> `Network model does not support requested MFG` = 第 4 处要切的模型**不在这份 DLL 里**；
> `XeLL version too old ... falling back to 2x` = 第 6 处。

**代价（必须知道）**：因为 MFG 模型不存在，每一帧插值都是 **2x 模型**跑出来的。
帧数变多了，单帧质量等同 2x。这是 Unlocker 自己的设计（`fg_keep_2x_model`），不是我们的失误。

---

## 2. 我们的实现

| 文件 | 内容 |
|---|---|
| `src/xefg_unlock.h` | 六个站点的字节表 + 全有或全无的写入（先校验模块所有权 / 页属性 / 原始字节，再 `VirtualProtect`→写→`FlushInstructionCache`→恢复属性→回读），失败自动回滚 |
| `src/bridge/bridge.cpp` | `--unlock-mfg`：说明开关含义、打印目标 `libxell` 路径、应用补丁，并把 `--interp` 的值作为倍数（默认 3 = 4x） |

关键点：

- **只改本进程内存**，不动磁盘文件，也不进游戏进程（我们是独立的 bridge 进程）。
  退出 bridge 后 DLL 文件仍然完好；想恢复原状只需重启 bridge 且不加 `--unlock-mfg`。
- **版本敏感**：站点是硬编码 RVA + 字节双重校验。换了 DLL 版本会直接报
  `...._unexpected_bytes` 并拒绝运行（不会半改）。
- **验证有两层**：写入时逐点回读；初始化时 `maxSupportedInterpolations` 必须是请求值，
  否则打 `WARNING: --unlock-mfg is on but the DLL still reports 1`。

开关：

| 开关 | 说明 |
|---|---|
| `--unlock-mfg` | 打补丁；倍数取 `--interp N`（N≤1 时默认 3，即 4x）。**副卡是 Intel 时不打**：`bridge` 会按适配器厂商跳过并打日志（原生 MFG 模型存在，打了反而把真模型换成重复的 2x 模型） |
| `--interp N` | 插 N 帧：2=3x、3=4x、4=5x、5=6x（解锁后有效范围 2..5） |
| `--fg-probe` | 打完补丁后立即回读 `maxSupportedInterpolations`，不需要游戏；与 `--unlock-mfg` 同用时会同时打印 **stock** 与 **patched** 两个值 |

---

## 3. 怎么用

```
1) 起游戏（5K，RTSS 锁 30fps）
2) <TOOLS_DIR>\run_mgpu_5k_mfg.bat
   = bridge.exe --den 1 --pool 4 --mv-newest --wait-ms 0 --interp 3 --unlock-mfg
3) 日志确认三行：
   --unlock-mfg: all 6 sites patched and read back OK
   applied maxInterpolatedFrames = 3 -> 4x output
   xN=4.00  xNmiss=0
4) 再看 skip：0-2 才算真的成立（5K 4x 的算力压力比 2x 大得多）
```

**倍率算术**（120Hz 面板）：30fps 输入 → 2x=60 / 3x=90 / **4x=120**。
40fps 输入开 4x 等于 160fps，超过面板，只会把 `present` 拖长、`skip` 变多。

**先测 3x**：`--interp 2`（90fps）在 6750 GRE 上更可能装进 33 ms 预算；
4x 若 `skip` 起来，就退 3x 或加 `--den 2`。

### 3.1 实测：5K 上 4x 撑不住（2026-11-06 10:08）

```
mode: ... interp=3 unlock=1
maxSupportedInterpolations = 3  /  MFG available: up to 4x
presented=4  xN=4.00  xNmiss=0          <- 4x 真的生效
gpu=39-44ms  present=4-16ms  total=48-54ms
recent=16-21 fps  load=89-102%  skip=19-26  slow=2-6
show=73-86
```

也就是说：**每一帧真实帧要花 ~40 ms GPU 时间**（5K 下每生成一帧约 13 ms × 3），
而锁 30fps 只给 33 ms。于是真实帧率掉到 ~19fps，19×4 ≈ **80fps**——
"只有 80Hz"不是倍率没生效，是**显卡先饱和了**，输出被 GPU 能力钉住。

对比 2x（同一份 DLL、同一场景附近）：`gpu=3-6ms / total=11ms / load=45% / skip=0`。

结论：**5K 与 4x 不共存**（差了 3-4 倍算力）。要 120fps 就得把插值分辨率降下来：
`--den 2`（2560×1440 插值 + 1.33× 放大）时每生成一帧约 1 ms，4x 轻松装进预算 →
`run_mgpu_4x.bat`（30 × 4 = 120，正好贴面板）。

### 3.2 长跑实测：4x@1440p 稳定 120fps（2026-11-06 10:22，关掉 RR）

3921 帧 / 99 个统计窗口 / 约 130 秒，`jitter` 已恢复非零（确认之前 `jitter=0` 是 RR 造成的）。

| 指标 | p50（中位） | 均值 | min | max |
|---|---|---|---|---|
| `show`（上屏） | **120** | 119 | 108 | 123 |
| `gpu` | 17.3 | 15.6 | 0 | 24.3 |
| `present` | 13.7 | 13.6 | 6.2 | 19.2 |
| `total` | 31.8 | 28.4 | 12.1 | 35.0 |
| `load` | 93% | 87% | 30% | 104% |
| `age` | 8.0 | 9.9 | 2.0 | 99.1 |
| `skip` | 0 | 0.2 | 0 | 3 |

- `xN=4.00` 出现在 **98/99** 个窗口（首个窗口启动时 1.00），`xNmiss=0`
- `HITCH` 1 次 / 99 窗口（`gap=253.6ms`，即 §9.3 那个已知的偶发打嗝）
- `[xefg error/warn]` 0

**`skip` 的归因**（15 个窗口有丢帧，逐条看它们是谁）：

| 主因 | 窗口数 |
|---|---|
| `age ≥ 10 ms`（WGC/DWM 帧"成串"到达，我们排空时覆盖了旧的） | **10 / 15** |
| `gpu < 12 ms`（GPU 明明有余量，不是算力问题） | **7 / 15** |
| `total > 31 ms`（真的贴到/超过 33.3 ms 预算） | **3 / 15** |

结论：这个配置下 `skip` 主要由 **WGC/DWM 的到达抖动**造成，而不是 GPU 不够。
`total` 只有 2% 的窗口超过预算（p50 31.8 / p90 33.1 / p99 35.0）。

> 首次 19 窗口的短跑（10:11，RR 仍开着）得到的是同一结论：
> `show 117–124`、`xN 4.00 / xNmiss 0`、`gpu 15.6–22.6 / present 9.8–11.2`、`HITCH 0`；
> 当时 `jitter=(0.000, 0.000)`，关掉 RR 后恢复非零——所以**联动项是 RR**，不是我们的输入有问题。

---

### 3.3 测量陷阱：外部延迟工具会让"指标好看、画面抖"

2026-11-06 10:41 那次 `_c` 出现了一组反常数据：

| 指标 | 正常（10:36 `_c`） | 挂 FLM 测延迟（10:41） |
|---|---|---|
| `present` | 12.4 ms | **2.6 ms** |
| `gpu` | 17.9 ms | **5.9 ms** |
| `total` | 31.5 ms | **4.4 ms** |
| `load` | 91% | **26%** |
| `show` / `xN` | 120 / 4.00 | 120 / 4.00 |
| 体感 | 顺 | **不顺（抖动）** |

原因：延迟测量工具（FLM 等）会 hook Present 或介入合成，于是——

1. `Present()` **不再等 vblank**（`present` 掉到 ~2.6 ms）；
2. 4 帧不再按 8.33 ms/帧 排出去，而是**成串推送**（`total` 只有 4.4 ms）；
3. `framesPresented` 仍是 4，所以 FPS 计数器照旧显示 120——
   **但视觉上是抖动**，因为 DWM 把它们挤在同一个合成周期里。

结论：**这类日志不能用来评价性能**，而且"计数器 120 但看着卡"正是这种状态的典型症状。
`bridge` 现在会自动识别并只警告一次：

```
[bridge] WARNING: presentation is not vblank-paced - total=4.4ms but the input
         interval is 33.1ms while 4 frames are presented per real frame.
[bridge]   something else hooks Present or our overlay is occluded
         (OSD / latency tool / FLM, RTSS, Steam overlay, a topmost window).
[bridge]   MFG still produces 4 frames, but they go out in bursts, so the
         picture judders while the counter reads 900 fps.
```

判据：`total < 0.35 × 输入帧间隔` 且 `framesPresented ≥ 2`。
正常 4x 配置是 `31.5 / 33.3 ≈ 0.95`，挂工具时是 `0.13`。

### 3.4 A/B：三次运行，差异在噪声内（2026-11-06 10:22 / 10:32 / 10:36）

三轮长跑，只有开关不同（其余全部相同：4x@1440p、锁 30、RR 关）：

| 指标 p50 | base（99 窗） | `--fg-rt 0 --fg-ui none`（79 窗） | `--fg-ui none`（90 窗） |
|---|---|---|---|
| `gpu` | 17.3 | 15.0 | **18.0** |
| `present` | 13.7 | 13.7 | **12.4** |
| `total` | 31.8 | 29.6 | 31.5（max **33.7**，唯一没过预算） |
| `load` | 93% | 86% | 91% |
| `show` | 120（108–123） | 119（**98**–125） | **120**（108–124） |
| `HITCH` | 1/99（1.0%） | **5/79（6.3%）** | 1/90（1.1%） |
| `skip` 窗口 | 15/99（15%） | 17/79（21.5%） | 15/90（16.7%） |

**结论修正**：第二轮看起来"`gpu` 降 2.3 ms"，但第三轮**没能复现**（反而 18.0）。原因是运行内噪声远大于运行间差异：

```
gpu 标准差（三次都一样）：sd ≈ 5.4–5.5 ms
gpu 运行内分段均值漂移：base 10.3→17.4 / b 15.2→10.8 / c 13.7→19.6   （7 ms 级别）
```

也就是说 `15.0 vs 17.3` 这 2.3 ms 只有 **0.4σ**，而且单次运行内部自己就漂 7 ms——
**这三次运行不足以证明任何单个开关的收益**。之前那条"`--fg-ui none` 实测省 2.3 ms"
是我对单次数据的过拟合，现予撤回。

同理，第二轮那 5 次 HITCH 也不能直接归因给 `--fg-rt 0`：第三轮（去掉它）只有 1 次，
但第二轮当时的场景/时段本身就不同（`age` 分布、`game` 帧时都有差异）。

**那什么是确定成立的**（远超噪声）：

| 结论 | 证据 |
|---|---|
| 5K 不能上 4x | `gpu` 39–44 ms，是 1440p 的 **2.4 倍**，远超噪声 |
| 1440p 4x = 120fps | 三轮 `show` p50 都是 119–120 |
| MFG 真的在出 4 帧 | 三轮 `xN=4.00 xNmiss=0` |
| 丢帧主因在 WGC/DWM | 三轮 `skip>0` 窗口里 80% 是 `age≥10ms` 或 `gpu<12ms` |

**实用取舍**：`--fg-ui none` 保留（它是初始化期的静态开关，我们本来就不提供 UI 纹理，
零风险，且与 OptiScaler 的默认 `UIComposition=false` 一致）；`--fg-rt 0` 不采用
（它改的是逐帧节奏，收益未证、风险未证，没有理由动）。

**要真正判定微优化，需要可重复的测量**：固定站位/固定视角、同一时段，每套开关跑 3 遍，
或用 `--report 200` 拉长统计窗口把噪声平均掉。以目前 `sd≈5.5ms` 计算，要分辨 2ms 的差异，
每套至少需要 ~30 个独立窗口且场景一致。


---

## 4. 风险与边界

| 项 | 说明 |
|---|---|
| 画质 | 所有插值帧复用 2x 模型 → 4x 的观感会比"原生 2x"稀，遮挡/转视角处更明显 |
| 算力 | 每多一帧就是一份实打实的 GPU 时间；5K 4x 在 6750 GRE 上大概率吃满 |
| 版本绑定 | 换 `libxess_fg.dll` / `libxell.dll` 版本会校验失败（这是有意的保护） |
| 第三方代码 | 字节表与做法来自 MIT 的 Universal MFG Unlocker（Michael Robles），已在 `src/xefg_unlock.h` 顶部注明 |
| 反作弊 | 我们只 patch 自己进程里加载的 DLL，不注入游戏；但仍不建议在联机游戏里使用任何帧生成注入方案 |
| 动态换档 | 站点 5 已打（实时改帧数不再重建），但"按目标帧率自动换档"的逻辑没做（见 §5） |

---

## 5. 动态换档（Dynamic MFG）值不值得做

先把账算清楚：4x@1440p 的 `load` 看着 74–104%，但 `present ≈ 13.7 ms` 主要是 **XeFG 在等 vblank 槽位**
（120Hz 下 4 帧正好铺满 33.3 ms），不是我们浪费。真正的 GPU 占用是中位
`gpu 17.3 ms / 33.3 ms ≈ 52%`（p99 24.3 ms ≈ 73%）——微优化能榨出来的很有限。

OptiScaler/Unlocker 的 DynamicMFG 是**每帧决定倍数**（目标 116fps：不够加档，够了减档）。
我们已经具备两个前提：

1. 运行时改帧数不重建管线（站点 5 已打）；
2. `xN`/`xNmiss`/`skip` 已经是可判定的指标。

**但结论是：在当前"锁 30 + 4x = 120"的配置里，动态换档帮不上忙，甚至会变差**（见 §5.1）。
它真正有用的场景是"输入不锁帧、帧率浮动"。如果将来要在那种场景启用，落点是：

```
if (skip > 0 或 xN < 期望) 且已连续 0.5s      → 降一档（4x→3x→2x）
if (skip == 0 且 total < 0.7×输入间隔) 连续 2s → 升一档（2x→3x→4x）
滞回 ≥0.5s；每档只调 xefgSwapChainSetNumInterpolatedFrames
```

### 5.1 为什么"动态换档"在锁 30 的配置上反而更差

OptiScaler 的 DynamicMFG 目标是"贴着目标帧率选最小倍数"，那套逻辑适合**输入帧率浮动**的场景。
我们这个配置里输入被 RTSS 钉在 30fps，而 **4x 恰好 = 120 = 面板刷新率的整数比**：

| 倍数 | 输出 | 与 120Hz 的关系 |
|---|---|---|
| 2x | 60 | 2:1，整除，顺 |
| 3x | 90 | **4:3，抖动** |
| **4x** | **120** | **1:1，最顺** |

所以在这里降档到 3x 会让画面从"每帧一次 vblank"变成"有时等有时不等"，
体感上比 4x 更糟。**动态换档只在输入不锁帧时才有意义**（例如 40–50fps 浮动），
那时可以让 120 恒定、倍数在 2x/3x 之间跳。

真正还值得做的是**重场景保底**：如果某场景在 4x 下 `gpu` 涨到 >26ms，
可以临时降一档把 90fps 的抖动换成不掉帧——这需要按场景实测确认后再决定。

其它已排除/低收益的路：

| 想法 | 结论 |
|---|---|
| 我们的拷贝/转换（`mvs`/`copy`/`work`） | 实测全是 0.0–1.7 ms，没有可挖的空间 |
| `--buffers 3` / `--latency 2` | 2x 时实测无差别；4x 贴 vblank 时理论上限也不在缓冲 |
| 把 MV/深度直接写进本地堆 | 跨卡 peer write 必须 `ALLOW_CROSS_ADAPTER`，本地副本无法省 |
| 自己实现 XeFG 网络（MFG-Unlocker 的 `source/native/xess/`，自带 UNet/splat 着色器 + CUDA 路线） | 工程量另开一个项目级别，不是调优 |
| flip queue / AMD flip meter（Unlocker v1.7.0） | 只适用于他们**自己 Present** 的路径；我们这条由 XeFG 接管 Present，且 RX 6000 没有该 meter |

---

## 6. 相关文件

| 文件 | 内容 |
|---|---|
| `src/xefg_unlock.h` | 六个站点与写入/回滚逻辑（含出处与解释） |
| `src/xefg_dyn.h` | DLL 运行时加载 + 函数表 |
| `src/bridge/bridge.cpp` | `--interp` / `--fg-lib` / `--fg-probe` / `--unlock-mfg`、`fg_probe()`、`xN`/`xNmiss` |
| `scripts/probe_fg.bat` | 能力自检（可选传 DLL 路径） |
| `run_mgpu_5k_mfg.bat` | 5K + 4x（含 `--unlock-mfg`） |
| `temp/check_sites.py` → **`scripts/check_sites.py`** | 逐点比对 DLL 字节的小工具（换版本时先用它） |
| `docs/findings/FINAL_CONFIG.md` | 运维总文档（开关全集、日志判据） |
