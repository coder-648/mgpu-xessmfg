# mgpu —— 把"渲染"和"帧生成"拆到两块 GPU 上

> **当前链路（2026-11-14）**：`fakerFG.dll` → `xess_mfg_producer.dll` →
> **`xess_mfg_consumer.exe`**（本页旧段落里的 `bridge.exe` 是它的前身，已冻结）。
> 安装/启动看 [`docs/ops/USAGE.md`](docs/ops/USAGE.md)，
> 架构看 [`docs/design/ARCHITECTURE.md`](docs/design/ARCHITECTURE.md)，
> 全仓现状与优化清单看 [`docs/PROJECT_STATUS_2026-11-14.md`](docs/PROJECT_STATUS_2026-11-14.md)。

> 游戏照常用**自己的卡**渲染 + 超分（DLSS / FSR / XeSS），
> 运动矢量与深度走共享显存，另一块卡专职用 **XeSS-FG** 插帧，再通过覆盖窗口上屏。
> 实测环境：**RTX 3080 Ti 渲染 + RX 6750 GRE 插帧，4K / 120 Hz**。
> A 卡 + N 卡混插能用，**单卡也能用**（插帧和渲染落在同一块卡上）。

```
游戏 ──► 超分 ──► fakerFG.dll ─────────► xess_mfg_producer.dll ──► 共享显存 + 跨卡 fence ──► xess_mfg_consumer.exe ──► 屏幕
       (DLSS/FSR/XeSS)  (拥有 XeFG 链与 FG 输入)   (颜色 + MV/深度 同槽发布)              (XeSS-FG, 另一块卡)
                                                                                              ▲
                                                                                    mgpu_ui.exe（前端 / 热键 / 状态）
```

---

## 亮点

| 亮点 | 说明 |
|---|---|
| **双卡分工** | 插帧不抢游戏的 GPU：游戏帧率不降，插出来的帧在另一块卡上算。也可以用单卡只借 XeFG 的能力 |
| **2x – 6x 倍率** | 2x 是原生；3x–6x 靠 `--unlock-mfg`（**内存补丁，仅供个人研究**，见 §许可）。40 → 120、30 → 120 都能整拍 |
| **两种模式** | **固定倍数**（稳定、可预测）与**动态逐帧**（每帧按实际经过的 vblank 数取倍率，输入 35–45 fps 抖动时也能把 120 Hz 填满，实测空刷新 **1.3%**） |
| **整流输出** | 输出恒定 1 帧/vblank；`--fg-out-hz` 还能把"120 Hz 面板但只想要 60 fps"变成 2x 而不是 4x |
| **颜色与运动数据同源** | 游戏 backbuffer 的整帧颜色由 producer 与 MV/深度**同一次发布**送过来（协议 v22），consumer 不再抓屏；一条发布 = 一帧，槽号就是配对身份 |
| **带宽自己会喊** | 发布 ini 默认 `StreamPercent=150`：4K 的 MV + 深度压到 1440p（**28 MB/帧**，而不是 63），把渲染卡的余量留给游戏；日志 `bus:` 直接告诉你超没超 |
| **延迟有账可算** | `click→photons`（按下 → 像素发光）**填满面板时 = 2 × 输入间隔 + DWM hop**：2x@60 ≈ 40–48 ms、3x@40 ≈ 55–65、4x@30 ≈ 73–82（实测）。日志两行把它拆成 `game->wgc` / `bridge` / `scanout`，[§延迟](#延迟clickphotons-是什么有多少) 有公式与各档数据 |
| **可诊断** | `breakdown` / `bus:` / `arr(vb):` / `out:` 一整套计数器，`--dump` 抓成对帧做离线逐像素分析；每个结论都有实测出处（`docs/findings/`） |
| **纯本地、无服务** | 四个产物（`fakerFG.dll`、`xess_mfg_producer.dll`、`xess_mfg_consumer.exe`、`mgpu_ui.exe`），无驱动、不注入其他进程、无网络；全部可逆卸载 |
| **GPL-3.0** | 协议、shader、实验记录全在仓库里，欢迎改和提 |

> 不吹的部分写在下面「已知限制」里 —— 这个项目对"做不到的事"的记录比对"能做到的事"更详细。

---

## 四个产物，各管一段

### 1. 游戏侧：`fakerFG.dll` + `xess_mfg_producer.dll`

**`fakerFG.dll`**（部署成 `dxgi.dll` + `nvngx_dlssg.dll`，注入方式与 OptiScaler 同构）拥有整条 FG
管线：接管交换链、回答/转发 DLSS-G、把这一帧的输入整理成一条记录、用 **XeFG** 输出。链路标准见
`docs/design/XEFG_CHAIN_STANDARD.md`。

**`xess_mfg_producer.dll`** 由 fakerFG 加载，只做传输：订阅记录 → 在标题仍然打开的命令表上
`barrier + CopyTextureRegion`（MV/深度 1:1，不缩放、不转换）→ Present 之后 signal 跨卡 fence。

- **不再需要 ReShade，也没有 add-on**（2026-11-14 起，旧的 `game_capture` 实现已删除）。只有想加
  别的 shader 时才把 `InjectReshade=1`。
- 零配置可用：部署脚本会放一份 `fakerFG.ini`。

**`fakerFG.ini`（游戏 exe 同级目录）** 关键键：

| 键 | 默认 | 作用 |
|---|---|---|
| `Mode` | `consume` | `local` = 自检平台（fakerFG 在本卡跑完整 FG）；`consume` = 双卡（拿全输入、拒绝补帧、原生帧上屏，副卡出帧）；`idle` = 只出原生帧；`observe` = 转发真运行时做 A/B |
| `InjectReshade` | `0` | 传输不再依赖 ReShade；只想加别的 shader 时才设 `1` |
| `OwnSwapchain` / `RealSwapchainObject` | `1` / `1` | 接管交换链、交还 libxess 真链（链路前提） |
| `FakeDlssgFeature` | `0` | 链路模式：标题关掉自己的 FG，整帧渲进我们交出去的链 |
| `MvResample` | `1` | 全分辨率 MV 重采样到 depth 尺寸（已同尺寸时自动跳过） |
| `TagAtEvaluate` / `ForceBorderless` | `1` / `1` | tag 在 Evaluate 上；XeFG 只在无边框下插帧 |
| `FgInput` / `FgOutput` | `Upscaler` / `XeFG` | 输入取 DLSS-SR 参数块，输出由 XeFG 呈递 |
| `StreamPercent` | `100` | 跨卡流尺寸：100=1:1（63.3 MB/帧）；150≈2/3（4K→2560x1440，28.1 MB）；200=1/2；400=1/4 |
| `Publish` | `1` | `0` 只用于测量：完全不发布/不传输 |

跨卡流量由 `StreamPercent` 控制（默认 100 = 1:1，4K ≈ 63 MB/帧；150 → 2560x1440 ≈ 28 MB；200 → 1080p ≈ 16 MB）。

### 2. `xess_mfg_consumer.exe` —— 帧生成侧（真正干活的进程）

WGC 已删除（协议 v22）：consumer 直接读取 producer 发布的 **stream 0 颜色**（与 MV/深度同一槽、同一 fence），
配对就是槽号；**XeSS-FG** 插帧 → DirectComposition 覆盖窗口上屏。同时负责节拍、OSD、热键和全部诊断输出。

**日常会用到的参数**（完整表见 [`docs/ops/USAGE.md`](docs/ops/USAGE.md) §6）

| 参数 | 作用 | 备注 |
|---|---|---|
| `--game Game.exe` / `--hwnd 0x…` | 指定目标窗口 | UI 传 hwnd |
| `--interp N` | 固定倍率：`1`=2x、`2`=3x、`3`=4x… | 3x 以上需 `--unlock-mfg` |
| `--interp-cadence` | **动态逐帧**：按实际 vblank 数决定倍率 | `--interp` 变成容量上限（推荐 5） |
| `--fg-out-hz N` | 输出帧率上限（0 = 面板刷新） | 最低 2x |
| `--fg-rt-mode …` | 喂给 XeFG 的 `frameRenderTime` 估计 | UI 用 `auto` |
| `--latency N` | Present 队列深度 | 默认 1（WGC 已删除，`--pool/--colour-queue` 仅兼容旧 bat） |
| `--den N` / `--scale F` | 生成分辨率 = 抓到的 1/N / 上屏缩放 F | `--den 2` 省算力；`--scale 0.75` 是 5K 折衷 |
| `--gpu N` | 指定插帧卡 | 不传则自动 |
| `--upsample` | 消费卡重建 MV/深度到渲染尺寸 | A/B |
| `--stream-auto` / `--stream-asym` | 自动/非对称流档位 | A/B |
| `--no-depth` / `--no-marker` | 不传深度 / 关闭 marker 配对 | 都**不推荐**，仅在排障时用 |
| `--unlock-mfg` | 内存补丁打开 3x–6x | 仅供个人研究，见 §许可 |
| `--overlay-layered` / `--relay-input` | 分层窗口（菜单可点击）/ 输入转发 | 二选一 |
| `--no-osd` | 关掉单行 OSD | |
| `--dump N` / `--report N` | 抓成对帧 / 统计窗口长度 | 排障与 A/B 量化 |

> 运动矢量来源不再由用户选择：consumer 按 producer 发布的记录自动决定
> （`--mv-flip` / `--no-mv` / `--fg-of` 只留给排障）。
> marker 是当前的配对主键；时间戳/FIFO 只在解不出 marker 时兜底。

### 3. `mgpu_ui.exe` —— 前端

选窗口 → 选模式/倍数/输出 → 开始；热键开关（默认 `Ctrl+PageUp`），状态栏显示关键日志。
界面上每一行都对应一个 consumer 参数，卡片底部「启动命令」是**实时回显**，所见即所传。
支持多个单游戏配置档，每档独立保存设置与目标窗口。详见 `docs/ops/UI.md`。

### 4. 构建与部署

```bash
./scripts/build_faker_fg.sh          # fakerFG.dll
./scripts/build_producer.sh          # xess_mfg_producer.dll
./scripts/build_consumer.sh          # xess_mfg_consumer.exe
./scripts/build_ui.sh                # mgpu_ui.exe
./scripts/deploy_faker_fg.sh "<game exe dir>"
./scripts/make_release_fg.sh         # dist/mgpu-<ver>/ + .zip
```

装完游戏目录里是 `dxgi.dll` / `nvngx_dlssg.dll` / `fakerFG.dll`（同一份 fakerFG）+
`xess_mfg_producer.dll` + `fakerFG.ini`；不再需要 ReShade 或 add-on。
`--uninstall` 可逆。消费端与 UI 放在独立目录（两者必须同目录）。

---

## 按场景选配置

| 你的情况 | 怎么配 | 输出 | 实测 | `click→photons` |
|---|---|---|---|---|
| **游戏 35–45 fps 抖、不想锁帧** | 动态逐帧：`--interp 5 --interp-cadence` + 游戏 MV | 120 fps（1 帧/vblank） | 空刷新 **1.3%**，倍率切换 12 次/秒 | ~55–80（跟着瞬时倍率） |
| **能稳定 40 fps，想要最稳** | 固定 3x：`--interp 2 --unlock-mfg`，MV 用游戏 MV 或 `--fg-of` | 120 fps | 空刷新 **0.1–0.3%** | **55–58**（1440p 插值，推算；4K 装不下时 64.9） |
| **能稳定 60 fps，画质优先** | 2x：`--interp 1` + 游戏 MV | 120 fps | 边缘伪影最少（光流 0.3 ms 可省掉） | **~40–48**（四档里最低） |
| **能稳定 30 fps** | 4x：`--interp 3 --unlock-mfg` | 120 fps | 空刷新 2.6%（改用动态逐帧 + `--interp 5` 容量可到 0.6%） | **73–82**（延迟最差的一档） |
| **120 Hz 面板但只想要 60 fps** | 动态逐帧 + `--fg-out-hz 60` | 60 fps | 输入 30 时自动 2x，不是 4x | **别开 FG**：~75–85，原生 60 fps 只要 25–30 |
| **5K / 高分辨率** | 加 `--scale 0.75` | 按比例 | 5K 原生只有 2x@30 ≈ 60 fps 跑得动 | ~75–85（5K 与低延迟互斥） |
| **预算最紧 / 插帧卡较弱** | `--interp 2 --no-mv` | 120 fps | 少一个 pass；快转时边缘略脏 | 与对应倍率同（MV 不影响延迟） |
| **只有一块显卡** | 一样能跑 | — | 插帧与渲染同卡，收益比双卡小很多 | 更长（`gpu` 66–72 ms 级） |
| **要抓数据做离线分析** | `--dump 200` | — | 输出 `f<NN>_colour/depth/mv.raw` + `_meta.txt` | — |

现成配方脚本多为 bridge 时代（`scripts/recipes/`），仅作参数参考；
当前链路直接用 `game\run_consumer.bat` 或 `mgpu_ui.exe`，参数含义见
[`docs/ops/USAGE.md`](docs/ops/USAGE.md) §6。

### 延迟：`click→photons` 是什么、有多少

**`click→photons` = 从你按下/移动，到屏幕上的像素真的发光**（业界叫 click-to-photon；LDAT、
NVIDIA Reflex 测的就是它）。单位 ms，73 ms ≈ 60 Hz 下的 4.4 帧、120 Hz 下的 8.8 个刷新周期。

它不是"我们这一段的耗时"，而是四段之和：

```
click→photons ≈ 输入采样→合成(≥1 个输入帧) + game->wgc(age) + bridge + scanout(0..1 vblank)
```

| 段 | 谁产生 | 能不能测 |
|---|---|---|
| 输入采样 → 合成 | 游戏输入线程 / 引擎 | ❌ 要点击时间戳，只能按下限"≥1 个输入帧"估 |
| 合成 → 我们拿到帧 | DWM 合成器 → WGC（日志 `game->wgc`，即 `age`） | ✅ |
| 我们整帧 | 取帧、拷贝、`XeFG`、`Present()` 等 vblank（日志 `bridge` = `total`） | ✅ |
| 上屏 | 扫描到像素发光，0 到一个刷新周期（120 Hz = 0..8.3 ms） | ✅ |

**关键：`bridge` 不是可优化项，它由批次几何决定。** 填满面板时一批 `k+1` 帧要铺 `k` 个 vblank，
真帧只能最后出（XeFG 只能在 prev→cur 之间插帧），所以 `bridge ≈ 一个输入间隔 T_in`：

```
填满面板：  click→photons ≈ 2·T_in + age + [0, vblank]
没填满：    click→photons ≈ T_in + age            ← 拿空刷新换一个输入间隔的延迟
装不下：    更长，而且会掉帧                      ← 最差的一格
```

那个多出来的 `T_in` 是**插值本身的成本**，DLSS-G / FSR-FG 也一样，不是本项目的代码开销。

**各档数据（120 Hz 面板；"实测"有日志出处，"推算"是上面公式算的）**

| 场景 | 输入 → 输出 | `click→photons` | 状态 |
|---|---|---|---|
| 2x | 60 → 120 fps | **~40–48 ms** | 推算（四档最低） |
| 3x（1440p 插值） | 40 → 120 fps | **55–58 ms** ⚠ | 推算，**还没实测** |
| 3x（4K 插值） | 40 → 106 fps | **64.9 ms** | 实测（4K 装不下 120） |
| 4x（1440p 插值） | 30 → 120 fps | **73.2 ms** | 实测（54 窗口） |
| 4x（4K 插值，长跑） | 30 → 120 fps | **77.4 ms**（p99 137） | 实测（167 窗口） |
| 3x，游戏只有 24 fps | 24 → 73–76 fps | **~47–62 ms** | 实测（没填满，延迟反而低） |
| `--fg-out-hz 60` | 30 → 60 fps | ~75–85 ms | 推算 —— **这档别开 FG**：原生 60 fps 只要 25–30 ms |
| 5K（只有 2x@30 跑得动） | 30 → 60 fps | ~75–85 ms | 推算 —— 5K 与低延迟互斥 |
| 单卡（插帧与渲染同卡） | — | 更长（`gpu` 66–72 ms 级） | 实测（`--overlay-monitor` 那次） |

⇒ **同样 120 fps，2x@60 比 4x@30 低 25–35 ms。** 要低延迟就在"能填满面板"的前提下取最小倍数。

⚠ **唯一还没跑过的低延迟点**：3x@40 + 1440p 插值，同样 120 fps，比 4x@30 低约 20 ms：

```bat
xess_mfg_consumer.exe --game <exe> --den 2 --interp 2 --unlock-mfg --fg-ui none
```

RTSS 锁 40，跑 ≥60 s，用 `scripts/latstats.py <日志>` 看 `compose->photons` / `est. click->photons`，
同时确认 `skip` / `slow` 不恶化。4K 原生（`--den 1`）装不下这个档（实测只有 106 fps）。

**想降延迟只有三条路**（别的都被实测否掉了）：

1. **提高输入帧率** —— 每个 vblank 台阶 −8.3 ms（120 Hz），最大的一项；
2. **同样 120 fps 取最小倍数** —— 60 fps 进就用 2x，别为了同一块面板硬上 4x；
3. **接受空刷新** —— 降一档 / `--fg-out-hz`，换来一个输入间隔。

**别指望这些**（有实测反例）：`--den` / `--scale`（只降算力与 drop，**不降延迟**；1440p 40.5 vs
4K 43.0 差的就是 `age`）、`--no-mv` / `--fg-of`（只改 GPU 时间）、`--latency 2` / `--buffers 3`
（实测无差别）、`--jbuf` / `--slack`（每个 N 加一个输入间隔）、`--pace` / `--lock`（与 XeFG 的
pacer 抢）。XeFG 内部时间、`Present()` 的 vblank 对齐、DWM hop 都是结构性的，动不了。

**日志怎么读**：每个统计窗口两行，`LATENCY: compose->present …ms (game->wgc … + bridge …)` 与
`est. click->photons >= …ms`。写 `>=` 是因为游戏自己那一段（输入采样→合成）测不到，真实体感只
会更大。要真值只能上 LDAT / 光电二极管 + 高速相机；在 `Present` 上打桩的工具会改变被测系统，
测出来的数和体感都不再是日常状态。完整算账、三态判据与剩余候选见
`docs/findings/PERF_BASELINE.md` §3.27。

### 游戏引擎备注

| 引擎 | 备注 |
|---|---|
| **UE5**（影之刃零、黑神话、明末渊虚之羽…） | **运动矢量选哪个跟 UE5 版本 / 游戏的 NGX 接法有关，没有统一答案，必须逐个试。** 已知的两种结果：<br>① **游戏 MV 是坏的** ⇒ 用 **AMD 光流**（`--fg-of`，0.3 ms）。PBZ/影之刃零实测：交给 NGX 的那条 MV 流 0.5% NaN/inf、17% 饱和到 fp16 上限、与真实位移相关性 ≈0，喂它就闪 + 鬼影，换 flag/单位/符号都救不了；<br>② **游戏 MV 只是符号反了** ⇒ 用 **游戏 MV（反向）**（`--mv-flip`）。明末:渊虚之羽实测就是这个：MV 本身是对的，只是 `previous-current` 的符号与 XeFG 的约定相反。<br>**试的顺序**：先 `游戏 MV` → 边缘拖影/方向错就 `游戏 MV（反向）` → 两者都闪/鬼影才退到 `AMD 光流` → 最后 `--no-mv`。别一上来就用光流（它在同一张卡上多一个全屏 pass，动态逐帧下 drop 更多）。<br>**另外，伪影的主因往往不是 MV 而是上采样倍率**：DLSS 性能档（720p→4K）连"不插帧的游戏本身"都有伪影，**DLAA（1:1）下干净**。所以画质优先时先上 DLAA，再谈 MV（细节：`docs/findings/UE5_MV_OPTISCALER.md`）。<br>**必须打开「分层窗口」**（`--overlay-layered`，UI 里默认已开）：UE5 在自认为"未激活"时会**丢掉菜单点击**（攻击走 raw input 照旧有反应，表现就是"能打、点不动菜单"），只有分层窗口下游戏菜单才收得到鼠标。 |
| 其它（Unity / 自研 / DX11 老引擎） | 游戏 MV 一般可用。边缘拖影先试 `--mv-flip`（反向），再退到 `--no-mv`；少数游戏只给深度、不给 MV，那种情况直接 `--no-mv` 即可。 |

#### 已记录的 UE5 游戏（运动矢量怎么选）

UE5 各版本 / 各游戏把 MV 交给 NGX 的方式不一样（单位、符号、有没有抖动都不统一），所以这张表是**按游戏逐个实测**记下来的，**换游戏或换 UE5 版本都要重测**：

> **2026-11-14 起**：UI 不再提供"运动矢量"选项，consumer 每帧读取 producer 发布的记录自动决定
> （有 MV 就用游戏 MV，没有就退到光流/零矢量）。下表只在需要**手动覆盖**
> 时才用得上：`--no-mv` / `--fg-of` / `--mv-flip` 仍是 consumer 的命令行开关。

| 游戏 | 运动矢量（手动覆盖时） | 命令行 | 依据 |
|---|---|---|---|
| **影之刃：零** | **AMD 光流** | 运动矢量 → `AMD 光流` | 游戏 MV 流是坏数据（NaN/饱和/无相关性）⇒ 喂它就闪，光流才干净 |
| **明末：渊虚之羽** | **游戏 MV（反向）** | 运动矢量 → `游戏 MV (反向)` | MV 本身正确，只是符号与 XeFG 的 `previous-current` 约定相反 ⇒ 反向即干净 |
| 黑神话：悟空 | 待记录 | — | 按上面的顺序试 |

> 这三列之外的可能结果：`游戏 MV`（默认，最省）、`关闭`（`--no-mv`，只靠深度/图像本身，边缘最脏但最便宜）。
> 判断标准只有一条：**慢速平移时看物体边缘有没有闪烁/拖影/鬼影**；方向错（反向）的表现是"边缘沿运动方向撕裂"。

---

## 5 分钟上手

1. **装进游戏**：`./scripts/deploy_faker_fg.sh "<game exe dir>"`（先跑 §4 的四个 build 脚本）；
2. **游戏内设置**：关掉游戏自带帧生成；超分选 **DLSS / FSR2+ / XeSS**；用外部限帧器（RTX 驱动的
   Max Frame Rate / RTSS）锁到"目标上屏帧率 ÷ 倍数" —— 不想锁就直接用动态逐帧；
3. **起消费端**：双击 `game\run_consumer.bat`，或打开 `mgpu_ui.exe` 选窗口 → 启动；
4. **确认通了**：OSD 显示 `x2`/`x3`，日志 `marker: decoded` 增长、`HITCH` 不持续上涨。

判读与排障：`docs/ops/USAGE.md` §8–§9。

---

## 已知限制（先看这里，省你几天）

| 做不到 | 原因 |
|---|---|
| 游戏自带帧生成 + 本工具同时开 | 三层插值，画面必炸 |
| TSR / FSR1 / 原生 TAA | 拿不到运动矢量，质量不可接受（会优雅降级并在日志提示） |
| 在消费者侧锁帧 | 无效；必须用外部限帧器 |
| 5K 原生高倍率 | 只有 2x@30 ≈ 60 fps 跑得动 |
| A 卡渲染 + N 卡输出的完整组合 | 本机未跑通；`--overlay-monitor` 要求两块屏都接同一块卡（见 `ROADMAP.md`） |
| 零延迟 | 插帧本身有约 **1 个输入帧**的成本：`click→photons` = **2 × 输入间隔 + DWM hop**。120 Hz 上实测 2x@60 ≈ 40–48 / 3x@40 ≈ 55–65 / 4x@30 ≈ 73–82 ms，随倍率与配置变化（见 [§延迟](#延迟clickphotons-是什么有多少)） |

**这些已经试过并确定没用**（别再走一遍）：`--cadence-hold`（拿丢帧换稳定）、
`--fg-cad-rt`（pacer 自激）、动态逐帧加 `--fg-of`（同卡多一个 pass）、`--no-depth`（渗色）、
固定 3x 配 44–45 fps 输入（45×3 > 120，必然超额）。理由与数字见 `CHANGELOG.md` 和
`docs/findings/PERF_BASELINE.md`。

---

## 欢迎参与

这个项目是从"能不能把插帧拆到副卡上"一路试出来的，**大部分结论来自失败的实验**，
所以特别欢迎：

- **换游戏验证**：UE5 / 自研引擎的 MV 单位与深度来源各不相同，
  `docs/design/INPUT_SOURCES.md` 写了怎么加一个输入源；跑通了请带 `--dump` 出来的成对帧和日志；
- **换硬件验证**：A 卡渲染 + N 卡输出、Intel Arc、核显、双 A 卡……任何组合的实测都值钱
  （`docs/findings/ENV_REPORT.md` 是登记处）；
- **打包 / 前端 / 文档**：UI 是纯 Win32 + GDI（无第三方库），加一行开关很容易；
- **提 bug 请带三样**：consumer 日志（含 `breakdown` 与 `bus:` 行）、`fakerFG.ini` + `<exe>_mfg_producer.log`、
  游戏内的超分设置。有这三样基本能复现。

不要提交：`libxess_fg.dll` / `libxell.dll`（Intel 的，见 `docs/ops/LEGAL.md` §3）、
被 `--unlock-mfg` 改过的二进制、任何游戏资源。

**文档从 `docs/README.md` 进**：结论先行 `findings/CONCLUSIONS.md`、
性能基线 `findings/PERF_BASELINE.md`、踩坑清单 `findings/FINAL_CONFIG.md` §5。

---

## 许可

**GPL-3.0-or-later**（`LICENSE`），Copyright (C) 2026 bilibili夕阳虹舞。第三方组件逐项署名见 `NOTICE`。

两条务必先读（`docs/ops/LEGAL.md`）：

1. 本项目**不分发** XeSS 运行时的 `libxess_fg.dll` / `libxell.dll`（Intel 的许可），
   公开版包里不带，请自行从 XeSS SDK 获取；
2. `--unlock-mfg` 是在**内存里**修改这些库的 6 处判断以打开 3x–6x，
   **可能违反 Intel EULA，仅供个人研究**，风险自负。

---

## 目录

| 路径 | 内容 |
|---|---|
| `CHANGELOG.md` | **每个版本的变化 + 实测依据** |
| `docs/README.md` | **文档索引 —— 从这进** |
| `docs/ops/USAGE.md` | 完整使用说明（安装 / 游戏内设置 / 配方 / 判读 / 排障 / 卸载），也是发布包里的 `README.md` |
| `docs/design/` | 架构（当前）、consumer 架构、协议 v17、输入源、历史设计 |
| `docs/ops/` | 使用、构建、部署、配置（`fakerFG.ini` / consumer）、UI、依赖、合规 |
| `docs/findings/` | 结论先行、画质变量、性能基线、踩坑清单、实验档案、里程碑 |
| `src/faker_fg/` | FG 管线：注入、DLSS-G 契约、XeFG 链、记录（`MgpuFgGetApi`） |
| `src/xess_mfg_producer/` | 传输：订阅记录 → 降采样 → 拷贝进共享槽 → fence 发布 |
| `src/xess_mfg_consumer/` | 当前消费端：WGC + marker 配对 + XeFG + 覆盖窗口 |
| `src/bridge/` | 旧消费端（legacy，仅 A/B） |
| `src/mgpu_ui/` | 前端（Win32 + GDI，无第三方库） |
| `scripts/recipes/` | 旧配方脚本（bridge 时代，参考用） |
| `packaging/` | 当前只保留 `fakerFG.ini`；旧安装器已归档 |
| `tests/` | 各里程碑的验证程序（不参与正式构建） |

> 贡献前建议先读 `docs/findings/TOOLING_NOTES.md`（本仓库的命令行约定）和
> `docs/ops/BUILD.md`（四件产物怎么编）。

