# 项目状态与组件审计（2026-11-14）

> 本文是一次**全仓体检**：当前代码到底跑的是哪条链路、每个组件做到什么程度、
> 哪些文档还能信、下一步最值得动哪里。
> 事实来源：`src/` 工作树（未提交改动包含最新的 marker/consumer 工作）、
> `docs/design/*`、`docs/findings/PERF_ROUND_HANDOVER.md`、`CHANGELOG.md` 顶部条目、
> `dist/mgpu-0.11-marker/` 发布物。
>
> **一句话**：链路已经闭环（`fakerFG` → `producer` → `consumer`），4 款游戏实测通过；
> 当前最大的问题不是"跑不起来"，而是 **文档落后于代码、consumer 与 bridge 双树并存、
> 没有自动化测试/CI、发布版本号两套**。

---

## 0. 结论摘要

### 已经成立的事

| 项 | 状态 | 证据 |
|---|---|---|
| 游戏侧拥有 FG 管线 | ✅ | `fakerFG.dll`（dxgi + nvngx_dlssg 代理）在 Wuchang / PBZ / Khazan / CP2077 上 2X 正常 |
| 双卡出货形态 | ✅ | `Mode=consume`：游戏侧 1X（无补帧开销），副卡 `xess_mfg_consumer.exe` 出帧 |
| 无 ReShade 依赖 | ✅ | 旧 `src/game_capture/`（约 1.2 万行 add-on）已删除；传输由 `xess_mfg_producer.dll` 承担 |
| 颜色↔MV/深度精确配对 | ✅ | 游戏侧四角 6×6 短雪花码 + CRC-8；52 s 实测 `decoded=2069 / miss=3` |
| 跨卡 28.1 MB/帧档 | ✅ | `StreamPercent=150`（4K → 2560×1440）；DMA 拷贝不占渲染卡 3D |
| 发布物可交付 | 🟡 | `dist/mgpu-0.11-marker/` 含 `game/` 全套；但版本号与 CHANGELOG/RELEASE_NOTES 不一致 |

### 组件总览

| 组件 | 成熟度 | 规模 | 当前定位 | 首要动作 |
|---|---|---|---|---|
| `fakerFG.dll` | ✅ 可用 / 🟡 可维护性差 | 14 cpp / ~9.3k 行 | 游戏侧 FG 管线所有者 | 瘦身 ini、隔离退役 pass、抽纯逻辑 |
| `xess_mfg_producer.dll` | ✅ 可用 / 🟢 结构最干净 | 1.4k 行 | 跨卡传输（记录→槽→fence） | 接口清理、拷贝流水诊断 |
| `xess_mfg_consumer.exe` | ✅ 可用 / 🟡 最复杂 | 16 模块 / ~12.9k 行 | 当前消费端（WGC + XeFG） | M3 线程化、统计/参数收敛 |
| `bridge.exe` | 🧊 冻结 | 16 模块 / ~10.8k 行 | 旧消费端 / A/B 基线 | 标 legacy，consumer 达标后归档 |
| `mgpu_ui.exe` | ✅ 可用 / 🟡 名实有偏差 | 2.2k 行 | 启动器 | 统一 consumer 命名、共享参数 schema、诊断包 |
| `scripts/` + `packaging/` | 🟡 两套体系 | ~50 脚本 | 构建/部署/发布 | 单一版本源、Windows 安装器、归档旧脚本 |
| `tests/` | 🔴 最薄弱 | 9 个历史探针 | 不参与发布 | 建 L0/L1/L2 测试与轻量 CI |

### 最大的四个结构性问题

1. **文档漂移（已处理大部分）**：架构/使用/构建/配置/部署/UI/协议已按当前链路重写并归档旧版；
   根 `README.md`、`RELEASE_NOTES.md`、`ROADMAP.md` 与部分 findings 仍是 add-on/bridge 语境。
   当前真实运行时是 `xess_mfg_consumer.exe` / 协议 **v17**。
2. **消费端双树**：`src/bridge/`（10.8k 行，16 模块）与 `src/xess_mfg_consumer/`
   （12.9k 行，16 模块）是 fork 关系，修复很容易只落在一棵树。
3. **没有自动化验证**：仓库无 CI；现有检查只有模块索引校验、单个本地日志回归脚本，
   没有纯逻辑单测、着色器编译检查、文档链接检查、打包清单校验。
4. **两套发布/版本体系**：`make_release.sh`（bridge/ReShade 时代，0.15.x）与
   `make_release_fg.sh`（当前链路，默认 `0.11-<sha>`）并存，`RELEASE_NOTES.md` 最新只到 v0.14.0。

---

## 1. 当前真实链路

```text
┌────────────── 渲染卡（游戏进程） ──────────────────────────────────────┐
│ 游戏                                                                  │
│  └─ dxgi.dll = fakerFG.dll                                            │
│       ├─ DXGI/NGX/Streamline 代理与早期加载拦截                        │
│       ├─ DLSS-G provider（回答契约、harvest 输入）                     │
│       ├─ XeFG backend（拥有 swapchain + Present；local 模式自己出帧）  │
│       ├─ marker：Present 前在链缓冲四角画 32-bit 短雪花码              │
│       └─ LoadLibrary("xess_mfg_producer.dll")                          │
│              └─ 记录 sink：降采样 + barrier + CopyTextureRegion        │
│                 第二队列（DMA 或 DIRECT）等游戏队列提交后拷贝           │
├────────────── 命名共享内存 Local\mgpu_mailbox_v1（协议 v17） ──────────┤
│  4 槽 × {MV, depth, scar(保留)} + fwd/back fence + consumed[] 信用窗口 │
└────────────── 消费卡（独立进程） ──────────────────────────────────────┘
   xess_mfg_consumer.exe
     ├─ WGC 抓游戏窗口（颜色只走 WGC，不过卡）
     ├─ 颜色 → 三缓冲共享 sink → D3D12 转换（scRGB→BT.2020 PQ）
     ├─ 解码四角 marker → 以 frame_id 精确配对 publication 槽
     ├─ 输入预拷（--prefetch-inputs）/ 可选 --upsample / --stream-auto
     └─ libxess_fg 插帧 → DirectComposition 覆盖窗口 → 120 Hz 面板
          ↑ 输入/参数/热键/状态
       mgpu_ui.exe（Win32 + GDI 启动器）
```

**关键边界**

| 模块 | 进程/形态 | 拥有什么 | 明确不做什么 |
|---|---|---|---|
| `fakerFG.dll` | 游戏进程，代理 DLL | FG 契约、XeFG 链、Present、输入记录、marker | 跨卡传输、共享槽布局 |
| `xess_mfg_producer.dll` | 游戏进程，普通 DLL | 订阅记录、降采样、拷贝、发布 fence、ACK 读取 | 任何 FG 决策、任何钩子 |
| `xess_mfg_consumer.exe` | 独立进程 | WGC 颜色、配对、XeFG 插帧、上屏、诊断 | 不碰游戏进程 |
| `bridge.exe` | 独立进程 | 与 consumer 同源的**旧**实现 | 已不再进入新发布包 |
| `mgpu_ui.exe` | 独立进程 | 启动/停止 consumer、参数回显、状态栏 | 不做实际渲染 |

### 产物与接口版本

| 产物 | 源码 | 构建 | 对外接口 |
|---|---|---|---|
| `fakerFG.dll` | `src/faker_fg/`（14 个 .cpp，~9.3k 行含头） | `scripts/build_faker_fg.sh` | 80 个代理导出 + `MgpuFgGetApi`（API v2） |
| `xess_mfg_producer.dll` | `src/xess_mfg_producer/`（1 文件，1421 行） | `scripts/build_producer.sh` | `MgpuProducerGetApi`（API v3） |
| `xess_mfg_consumer.exe` | `src/xess_mfg_consumer/`（16 模块，~12.9k 行） | `scripts/build_consumer.sh` | 命令行 |
| `mgpu_ui.exe` | `src/mgpu_ui/`（2190 行） | `scripts/build_ui.sh` | 启动器 |
| `bridge.exe`（遗留） | `src/bridge/`（16 模块，~10.8k 行） | `scripts/build_bridge.sh` | 命令行 |
| 共享 ABI | `src/mgpu_shared.h` 等（~2.6k 行） | 头文件 | `kSharedVersion = 17` |

> 硬约束：`kSharedVersion` 不匹配时两端**硬拒绝**。发布包必须同一次构建产出，
> 不能混用旧 `producer`/`consumer`。

---

## 2. 组件现状与可优化点

### 2.1 `fakerFG.dll` —— FG 管线的所有者（✅ 可用，🟡 可维护性差）

**做到的事**

- DXGI / `nvngx_dlssg` / Streamline / `kernel32!LoadLibrary*` 代理与早期拦截；
- DLSS-G provider：`Create/Evaluate/Release`、`Available/Requirements/SetOptions/GetState`；
- harvest：把参数块变成一条 `MgpuFgFrame` 记录（MV/深度/hudless/UI/矩阵/jitter/MV 尺度/reset/相机）；
- XeFG backend：`D3D12InitFromSwapChainDesc`、真链交还 + 9 个 vtable 钩子、tag、`SetPresentId`、Present 派发；
- 模式：`off / observe / idle / local / consume`（`bridge` 为 `consume` 别名）；
- `MvResample`（全分辨率 MV → depth 尺寸）、`ForceBorderless`、`TagAtEvaluate`；
- marker：四角 32-bit 短雪花（`seq12 | ms12`）+ CRC-8，默认开启，可用 `marker_disable.on` 关闭；
- `InjectReshade=0` 时不需要 ReShade；`Publish=0` 可做发布路径的开销基线。

**现状问题**

| # | 问题 | 影响 |
|---|---|---|
| 1 | `fakerFG.ini` 键面很大，混有 OptiScaler 同名开关、历史键、已退役键 | 新用户不知道哪些该动；注释之间偶有矛盾（如 `StreamPercent=150` 的目标尺寸写法） |
| 2 | `passes.cpp` + 4 个 UI/HUD 着色器默认全关；`proxy_kernel.cpp` 的 `NgxCoreProxy` 默认 0 | 增加 crash/维护面积，且来源是 GPL 参考实现 |
| 3 | 单一 TU（`unity.cpp` 顺序 include 14 个 cpp） | MSVC 单遍编译顺序敏感；纯逻辑（marker 码、MV 尺寸、配置解析）无法单测 |
| 4 | `mgpu_fg_api.h` 头注释仍以已删除的 `game_capture.addon64` 为消费者 | 读代码的人会找错模块 |
| 5 | marker 每帧一次 GPU pass（4 角一次 draw）+ 消费端读回 | 成本低但未量化；与"画面要求 1:1"强绑定 |
| 6 | 兼容性只在 4 款游戏实测，PRAGMATA 仍待跑 | 新引擎（RE / UE5 变体）可能直接失败 |
| 7 | 早期注入 + 加载器钩子 | 反作弊/完整性校验风险，需在文档里持续声明 |

**优化建议（按收益/风险）**

1. **配置面收敛**：把 ini 分为「用户必改（`Mode`/`StreamPercent`/`Interpolated`）/
   诊断（`Log`/`DebugView`）/ 退役（保留读取但日志警告）」三组；发布包只带精简 ini。
2. **退役代码隔离**：把 `passes.cpp`、`proxy_kernel.cpp`、`hud_*` 着色器用构建开关
   排除（默认不编译），既减小编译/崩溃面，也便于未来讨论更宽松许可（当前整体 GPL-3.0）。
3. **抽出纯逻辑模块**：marker 编码/CRC/四角几何、MV 重采样尺寸、配置解析放进
   不依赖 D3D 的头/小 TU，配 Python 或 C++ 单测，先把最容易回归的部分锁住。
4. **明确降级语义**：producer 缺失/版本不匹配、XeFG 初始化失败、`MISMATCH_INPUT_RESOURCES`
   各自只打一次明确日志，并保证"原生帧上屏、不黑屏"。
5. **单游戏配置档**：`fakerFG.ini` 按游戏保存为 `fakerFG.<exe>.ini`（或读取部署时生成的
   配置），避免用户在多游戏间来回改。
6. **把 marker 成本写进文档并做 A/B**：`marker_disable.on` 已有，补一条 52 s 对照，
   记录 GPU/CPU/画质差异即可。

---

### 2.2 `xess_mfg_producer.dll` —— 只做传输（✅ 可用，🟢 结构最干净）

**做到的事**

- `MgpuFgApi::subscribe` 拿记录；sink 内在仍打开的命令表上录 barrier + 拷贝；
- `StreamPercent` / 每流 `StreamPercentMv|Depth` 降采样（compute pass，MV/depth 强制同尺寸）；
- 第二队列异步拷贝：源在 `COPY_SOURCE` 时走 **DMA 引擎**，否则走第二条 DIRECT 队列；
  游戏 Present 不被传输阻塞；
- 4 槽 + 每槽 `consumed[]` 信用窗口；消费端静默 30 帧后退回轮转；
- `present()` 里写 `FrameShared` 元数据（含 `frame_id`/`frame_key`/语义位/矩阵）再 Signal `fwd`；
- 同一 `frame_qpc_ms` 只发布一次（防止 XeFG 链 + 标题 present 造成重复记录）。

**现状问题**

| # | 问题 | 影响 |
|---|---|---|
| 1 | ✅ 发布包 ini 默认改为 `StreamPercent=150`（4K 28.1 MB/帧） | 原默认 100 会多花一倍渲染卡带宽；COPY-only 列表自动回退 100 |
| 2 | ✅ COPY-only 命令表已做自动降级（首次遇到时切 1:1 并 bump generation） | 不再整帧丢数据；日志 `falling back to StreamPercent=100` |
| 3 | API v3 的 `record_scar` / `marker_tile` 已经退役（返回 5 / nullptr）但仍在头文件 ABI 里 | 读接口的人会误以为还存在该功能 |
| 4 | 单文件 1421 行，布局/降采样/拷贝/发布/信用窗口混在一起 | 修改风险集中；缺少可单测的纯函数边界 |
| 5 | 颜色/ UI 流不支持（设计如此，走 WGC） | 若未来 WGC 成为硬瓶颈，这条路的成本已在文档中评估为"不划算" |

**优化建议**

1. **默认档位**：发布 ini 改为 `StreamPercent=150`，或在 UI 中默认开启 `--stream-auto`；
   用 `bus:` 与画质 A/B 作为验收（低风险、立刻省一半跨卡带宽）。
2. ✅ **COPY 列表降级**：首次检测到 COPY-only 时切 `StreamPercent=100`、重写 content 矩形并
   bump generation，之后 1:1 拷贝；不再整帧丢数据。
3. **拷贝流水加深**：当前每帧一份 copy list、一次 `wait_fence(50ms)`；可评估
   2 份 in-flight copy（每槽独立 allocator/list），把偶发 `cq 4–9 ms` 尖峰摊平。
   先加 p95 统计，再决定是否值得。
4. **接口清理**：`mgpu_producer_api.h` 标注 `record_scar`/`marker_tile` 为 deprecated，
   在下一次版本号提升时移除；同步删除 producer 里的空实现。
5. **可测试化**：把槽位选择、信用窗口判定、每流百分比解析抽成纯函数（不依赖 D3D），
   便于用日志/单测覆盖。
6. **诊断**：除现有 `publish #N` 外，增加每 600 帧一行 `producer: dropped=N copy_p95=…ms
   dma=…`，让"是不是传输卡了"一眼可见。

---


### 2.3 `xess_mfg_consumer.exe` —— 当前消费端（✅ 可用，🟡 复杂度最高）

**做到的事**

- 消费卡上的 WGC 抓取（`FrameArrived` 事件）+ capture 线程 + 有界 colour 队列；
- D3D11→D3D12 颜色三缓冲 sink（`mgpu_colour_sink0/1/2`），转换 pass 里做 HDR（PQ/BT.2020）
  与四角 marker 回填（UV 内偏移，零额外 pass）；
- marker 解码（四角 6×6、2×2 格、min/max、CRC-8）→ **配对主键**；
  配对失败时的 present-ring/时间戳启发式只作兜底，并打 `marker pair: heuristic slot X -> Y`；
- 输入预拷：上一帧 Present 后在 copy 队列上预拷下一槽，命中直接用本地副本；
- `--stream-auto`（按 WGC age 与带宽调每流 StreamPercent，2 窗确认 + 2 s 驻留）、
  `--stream-asym`（MV 1:1 + 深度降档）、`--upsample`（depth 引导重建到渲染尺寸）；
- cadence：固定倍数 / 动态逐帧、`--fg-rt-mode`、`--fg-out-hz`、`--unlock-mfg`；
  动态逐帧使用 16 样本均值的 EMA（默认 8）、相位直接应用、重复帧不推进相位；
- OSD、`--dump`、`--mv-shadow`、`--probe-colour`、`--list-modes` 等诊断；
- 统计行：`breakdown` / `bus:` / `arr(vb):` / `pf:` / `marker:` / `HITCH` / `SLOW frame`。

**现状问题**

| # | 问题 | 影响 |
|---|---|---|
| 1 | ✅ 预拷已改为 copy-on-publication + FIFO 默认（`--pub-fifo`） | 仍存在少量 `pf_stale`（拷贝未完成时本帧补拷），但不再轮转猜槽 |
| 2 | ✅ 颜色 sink 已改三缓冲 | 双缓冲时代 GPU 落后 2 帧会 `colour=168 ms`；现在落后 >=3 帧才跳帧 |
| 2b | ✅ 动态逐帧已修正（EMA 默认 8、相位 k 直接应用、重复帧不推进、启动 2x） | 修复“一卡一卡”：倍率逐帧抖、相位与实际输出不一致、启动 6x 过冲 |
| 3 | 与 `src/bridge/` 是 fork，两个 16 模块树并存 | 同一修复要改两遍；行为容易分叉 |
| 4 | 命令行开关极多（~60 个），实验路径与产品路径混在一起 | `main.cpp` 1773 行；用户与 UI 都难以判断哪些该开 |
| 5 | 线程模型停在 M2（capture 线程；input 线程/SPSC matcher 未做） | 预拷与配对仍耦合在 presenter 循环里 |
| 6 | 统计以均值为主 | 尖峰/长尾看不出来（交接文档已建议改直方图） |
| 7 | 对 1:1 窗口强依赖 | 缩放/非整数倍时 marker 与 `present size != window client` 直接失败 |
| 8 | 纯逻辑（marker 解码、cadence、stream controller）没有单测 | 回归只能靠真机跑 52 s+ |

**优化建议**

1. ✅ **预拷默认走 copy-on-publication + FIFO**（`g_pf_pub=true` / `g_pub_fifo=true`）；
   `--no-pub-fifo` 保留拷贝但按 publication 值匹配，`--no-prefetch-publications` 回到轮转预拷。
   剩余 `pf_stale` 只来自“拷贝还在飞行中”的本帧补拷，下一步是拆批提交/加深 copy ring。
2. ✅ **颜色三缓冲**：`kColourSinks=3`，跳帧阈值改为落后 >=3 帧；验收看 `SLOW frame` 与
   `HITCH` 的 10 分钟漂移，而不是只看均值。
3. **M3 输入线程 + publication 队列**：按 `XESS_MFG_CONSUMER.md` 的 `FrameJob` 边界
   把 capture/input/matcher/presenter 解耦；目标 `pf` 40/40、`arr` 3x ≥95%、
   `total ≤ 24.5 ms`（4K/40/150 口径）。
4. **参数收敛**：把 `--jbuf/--slack/--slack-auto/--lock/--pace` 等标为遗留；
   产品路径只保留"倍数/输出/流档位/配对开关/诊断"几组；UI 与 consumer 共用一份
   选项 schema（见 §2.5）。
5. **统计升级**：`breakdown` 保留均值，另加每窗口 p50/p95/p99 与直方图（或 CSV），
   便于 A/B 和回归判断。
6. **纯逻辑抽离 + 单测**：marker 编解码、cadence 决策、stream controller、配对状态机
   都是无 D3D 依赖的输入/输出，可先做成头文件模块 + 测试程序。
7. **失败可见**：共享内存版本不匹配、producer 缺失、WGC 尺寸不等于窗口客户区，
   都要在 UI 状态栏与日志首屏明确显示（不要只在滚动日志里）。
8. **与 bridge 的关系**：consumer 达标后**冻结 bridge**；不要再把新特性加进 bridge。
   中期把两树合并为"一个 core + 两个入口"，或直接归档 bridge。

#### 当前实测基线（同一台机器，仅作参照）

| 指标 | 数值 |
|---|---|
| 输入 / 输出 | 40 fps 输入 → 2X 80 fps（120 Hz 面板） |
| 跨卡带宽 | 28.1 MB/帧 ≈ 1.1 GB/s @40fps |
| `gpu` / `present` | 15–18 ms / 6–9 ms |
| `capture` / `age` | 8–14 ms / 9–28 ms |
| `load` | 51–99%（随场景） |
| 配对 | `marker: decoded=2069 / miss=3`（52 s） |
| 卡顿 | `--lock 0` + 游戏锁 40：`HITCH` 0–2 次 |

> 来源：`docs/findings/PERF_ROUND_HANDOVER.md` §4。数字是单机单场景，换硬件/游戏必须重测。

---

### 2.4 `bridge.exe` —— 旧消费端（🧊 冻结，保留为 A/B 基线）

- 16 个模块、~10.8k 行，与 consumer 同源 fork；`src/bridge/README.md` 与
  `docs/design/BRIDGE_ARCH.md` 只描述它。
- 已不进入 `make_release_fg.sh` 的发布包；`dist/mgpu-0.15.7`（11-10）是它最后的时代。
- **建议**：
  1. 在文档与 UI 中明确标为 legacy；
  2. 给 `src/bridge/` 加文件头横幅"仅供回归，不再接收新特性"；
  3. consumer 通过 A/B 后，把 bridge 移入 `src/legacy/`（或删除），避免继续双维护；
  4. 若要保留更久，只允许"同步 consumer 已验证的修复"，不允许反向新增。

---

### 2.5 `mgpu_ui.exe` —— 前端（✅ 可用，🟡 名实与功能有偏差）

**做到的事**：枚举窗口 → `--hwnd` 给 consumer；倍数/输出/帧池/延迟/色彩空间/叠加层/
热键/GPU 选择；`no_depth` / `no_marker` / `upsample` / `stream_auto` / `stream_asym` /
`input_prefetch` 开关；启动命令实时回显；底栏解析 consumer stdout；多配置档。

**现状问题**

| # | 问题 | 影响 |
|---|---|---|
| 1 | 内部命名仍大量使用 `bridge`（变量、日志、注释、`bridge_*.txt`） | 与真实进程 `xess_mfg_consumer.exe` 不一致，排障时容易找错日志 |
| 2 | `ID_GPU` 仍硬编码显示 `AMD Radeon RX 6750 GRE` | 换机器显示错误；应枚举实际适配器 |
| 3 | 没有直接的 `StreamPercent` 数值控制，只有 auto/asym | 想手动固定 150 的用户只能改 ini |
| 4 | 不显示 `fakerFG.ini` 的 `Mode`（consume/local） | 用户不知道游戏侧是双卡还是自检平台 |
| 5 | 启动命令里硬编码 `--unlock-mfg --fg-ui none --fg-rt-mode auto --pub-fifo --report 40` | 产品默认值藏在代码里；换默认必须改代码 |
| 6 | 不读 producer/consumer 日志内容，仅解析少数统计行 | 出错时无法在 UI 内看到"版本不匹配/没有共享内存" |
| 7 | 配置档与参数 schema 与 consumer 各写一份 | 新增开关容易漏同步（`--no-marker` 就曾是新键） |

**优化建议**

1. **改名为 consumer 语义**：日志文件 `mgpu_ui_*.txt`、内部变量、提示文案统一；
   保留 `bridge` 只作为 legacy 说明。
2. **共享选项 schema**：把 consumer 的参数表（名称/默认/范围/中文说明）抽成一份
   （如 `scripts/consumer_args.json` 或生成头文件），UI 与文档都从它生成。
3. **GPU 枚举**：用 DXGI 枚举适配器，显示真实名称与 LUID；默认选择"非生产者卡"。
4. **增加流档位控件**：`StreamPercent` 数值（100/150/200/400）或"自动"，
   并显示当前 `bus:` 估算。
5. **诊断包**：一键收集 `fakerFG.ini`、`*_fakerFG.log`、`*_mfg_producer.log`、
   consumer 日志、GPU/驱动版本、启动命令，输出 zip；这是提 bug 最大的效率提升点。
6. **启动前校验**：consumer 存在、同目录 DLL、游戏窗口有效、是否已在运行；
   失败时给出明确中文原因。
7. **显示节奏状态**：`arr(vb)`、`xN`、`pf`、`marker miss` 用颜色/告警突出，而不只是滚日志。

---


### 2.6 脚本、打包与发布（🟡 可用，但两套体系）

**当前有效**

| 脚本 | 用途 |
|---|---|
| `build_faker_fg.sh` / `build_producer.sh` / `build_consumer.sh` / `build_ui.sh` | 当前四件产物 |
| `deploy_faker_fg.sh` | 部署/卸载游戏目录（三名字同步） |
| `make_release_fg.sh` | 当前发布包（`game/` + docs + SHA256 + zip） |
| `check_bridge_modules.py` | 两棵树的模块索引校验（当前两树均通过） |
| `check_fakerfg_regression.py` | fakerFG 日志回归（默认路径写死本机） |
| `extract_hlsl.py` / `raw_to_png.py` | 着色器校验 / dump 转图 |

**问题**

1. **版本号分裂**：`make_release_fg.sh` 默认 `0.11-<sha>`，而 CHANGELOG 已到 0.14/0.15，
   `RELEASE_NOTES.md` 停在 v0.14.0；最新包名 `mgpu-0.11-marker` 与文档版本无法对应。
2. **旧脚本仍在**：`build_addon.sh`、`make_addon_package.sh`、`make_release.sh`、
   `make_local_release.sh`、`build_m0.sh`、`build_xefg_smoke.sh`、`recipes/*.bat`
   等大量 bridge/ReShade 时代入口，`docs/ops/BUILD.md` 仍把它们当主路径。
3. **发布包缺安装器**：`game/` 需要手工复制；旧 add-on 包有 `install.bat/ps1`
   （备份、校验、回滚），新链路没有等价的 Windows 安装/卸载。
4. **无 CI / 无清单校验**：打包只做 SHA256；没有"文件是否齐全 / 版本是否一致 /
   文档链接是否有效"的自动检查。
5. **磁盘卫生**：`dist/` 1.5 GB（46 个发布目录 + 旧 zip）、`temp/` 130 MB；
   多次实验包未清理，容易误发旧包。
6. **NOTICE/LEGAL 落后**：仍以 add-on / bridge 为主语，未提 consumer/producer；
   `consumer_internal.h` 里有一处注释把 `xefg_dynamic_count.h` 写成 MIT（实际 GPL-3.0）。

**优化建议**

1. **单一版本源**：`VERSION` 文件或 `git describe` 生成，`make_release_fg.sh`、
   CHANGELOG 顶部、UI 标题栏、`run_consumer.bat` 注释全部引用它。
2. **Windows 安装器**：为 `game/` 提供 `install.bat`（备份 `dxgi.dll`/`nvngx_dlssg.dll`、
   SHA256 校验、拒绝游戏运行中安装）与 `uninstall.bat`（按备份还原）。
3. **打包清单断言**：脚本内校验 `game/` 必须有 9 个文件（三份 fakerFG + producer +
   consumer + UI + ini + 两个 XeSS DLL 可选），并核对 `fakerFG.dll` 与两个改名的
   副本 `sha256` 一致。
4. **CI（轻量）**：GitHub Actions 跑：`bash -n scripts/*.sh`、`py_compile`、
   `check_bridge_modules.py` 两棵树、HLSL `fxc` 编译、文档相对链接检查、
   `make_release_fg.sh` 的 dry-run/清单检查。不需要 GPU。
5. **归档旧脚本**：建 `scripts/legacy/`，把 add-on/bridge 时代脚本移入并在
   `docs/ops/BUILD.md` 只留一段"历史构建"。
6. **清理 dist/temp**：保留最新 2 个本地包 + 一个公开包；其余移出仓库或删除。

---

### 2.7 `tests/` 与验证（🔴 最薄弱）

**现状**

- `tests/*`、`src/m2_probe`、`src/m3_depth`、`src/bridge/osd_test.cpp` 是**历史探针**，
  不参与正式构建；结论仍被 `docs/findings/` 引用。
- 唯一"自动"检查是 `check_bridge_modules.py`（结构）与
  `check_fakerfg_regression.py`（读本机固定路径日志）。
- 没有任何纯逻辑单测、着色器编译检查、文档链接检查、打包检查。

**建议的最小测试金字塔**

| 层 | 内容 | 运行环境 |
|---|---|---|
| L0 静态 | shell/python 语法、模块索引、HLSL `fxc`、文档链接、发布清单 | CI，无需 GPU |
| L1 纯逻辑 | marker 编解码/CRC/四角几何、cadence、stream controller、槽位选择 | Linux/Windows 单测 |
| L2 日志回归 | 把健康日志切成 fixture，跑 `check_fakerfg_regression.py` 等断言 | CI |
| L3 真机 | 52 s 场景 + `marker/pf/HITCH/SLOW` 阈值；GPU 专用，人工/自托管 | Windows + 双卡 |

先把 L0/L1/L2 做出来，L3 只保留固定 checklist（脚本化启动 + 收集日志）。

---

## 3. 文档现状：哪些能信，哪些要修

### 3.1 权威文档（当前链路）

| 文档 | 可信度 | 说明 |
|---|---|---|
| `docs/design/ARCHITECTURE.md` | ✅ 高 | **已重写**：当前两段式架构、一帧生命周期、关键取舍 |
| `docs/design/CONSUMER_ARCH.md` | ✅ 高 | **新增**：consumer 16 模块、线程/队列、marker 配对、预拷、流控 |
| `docs/design/XEFG_CHAIN_STANDARD.md` | ✅ 高 | fakerFG 链路的权威说明（结构、开关、判据） |
| `docs/design/XESS_MFG_PRODUCER.md` | ✅ 高 | producer 实现与实测；§0.2 已被后续 StreamPercent 实现部分推翻 |
| `docs/design/PROTOCOL.md` | ✅ 高 | 已升到 v17，补了 v11/v15/v16/v17 条目 |
| `docs/ops/USAGE.md` | ✅ 高 | **已重写**：当前发布包、安装、启动、开关、排障、卸载 |
| `docs/ops/BUILD.md` | ✅ 高 | **已重写**：四件产物 + legacy bridge、依赖、打包 |
| `docs/ops/CONFIG.md` | ✅ 高 | **已重写**：`fakerFG.ini` + consumer 参数 + 调试文件 |
| `docs/ops/DEPLOY.md` | ✅ 高 | **已重写**：复制 `game/`、`deploy_faker_fg.sh`、回滚、自检 |
| `docs/ops/UI.md` | ✅ 高 | **已重写**：UI 行 → consumer 参数映射 |
| `docs/findings/PERF_ROUND_HANDOVER.md` | ✅ 高 | 当前消费端链路、日志速查、基线、五个坑 |
| `docs/findings/PERF_BASELINE.md` / `CONCLUSIONS.md` / `FINAL_CONFIG.md` | 🟡 中 | 字段/结论仍有效，但混有 add-on 时代语境 |
| `CHANGELOG.md` | 🟡 中 | 顶部条目最新；越往下越是历史，且与发布版本号脱节 |

### 3.2 仍需注意的文档

| 文档 | 问题 | 建议动作 |
|---|---|---|
| `README.md`（根） | 组件名/配方仍以 `bridge.exe` 为主语 | 已加顶部横幅；下一步按 `USAGE.md` 重写主体 |
| `ROADMAP.md` | 大量条目是 add-on/bridge 时代 | 顶部加“当前 backlog 见 PROJECT_STATUS”并归档旧条目 |
| `docs/design/PIPELINE_V2.md` | 历史链路（producer 重写前） | 保持历史；已加索引说明 |
| `docs/design/PIPELINE_V3.md` | 配对仍写时间戳查表 | 已加横幅：marker 已成为主键 |
| `docs/design/BRIDGE_ARCH.md` / `BRIDGE_V2.md` | 只描述旧 bridge | 标 legacy；consumer 见 `CONSUMER_ARCH.md` |
| `docs/design/XESS_MFG_CONSUMER.md` | 停在 M1/M2 | 已加 M2.5 现状说明 |
| `docs/RELEASE_NOTES.md` | 最新 v0.14.0，当前包是 `0.11-marker` | 统一版本号后补当前版本说明 |
| `docs/ops/DEPENDENCIES.md` | 混有 ReShade/add-on 依赖 | 拆出“当前链路依赖”一节 |
| `NOTICE` / `docs/ops/LEGAL.md` | 已补 producer/consumer，但历史段落仍在 | 下次许可复核时一并清理 |

### 3.3 仍缺的文档

1. **跨游戏已知问题清单**：UE5 放大倍率、RE Engine、HDR/缩放、反作弊风险；
2. **版本策略**：CHANGELOG / RELEASE_NOTES / `make_release_fg.sh` / UI 标题的统一来源；
3. **legacy 归档策略**：`bridge`、旧脚本、旧 `dist/` 的冻结与删除时间表；
4. **真机测试 checklist**：52 s 场景、日志阈值、崩溃符号化步骤（可做成模板）。

---


## 4. 风险清单

| 风险 | 说明 | 缓解 |
|---|---|---|
| 单机验证 | 性能/兼容结论都来自 3080 Ti + 6750 GRE + 120 Hz | 文档明确标注；欢迎硬件登记；关键结论保留原始日志 |
| 闭源 XeFG | 3x/4x 依赖 `--unlock-mfg` 内存补丁；画质/稳定性不可控 | 默认 2x；3x+ 标为研究用途；保留 `--fg-of` 等替代 |
| 许可 | 整体 GPL-3.0（OptiScaler 逐字代码）；Intel DLL 不可再分发；补丁可能违反 EULA | 不分发 XeSS DLL；公开包 `SKIP_XESS_DLLS=1`；NOTICE/LEGAL 更新 |
| 反作弊/完整性 | 代理 dxgi + 加载器钩子 + 游戏进程内 DLL | 文档声明仅个人研究；不联网、不注入他人进程；提供卸载脚本 |
| 硬版本拒绝 | `kSharedVersion` 不匹配直接不通信 | 发布包整体产出；UI 启动前校验版本/同目录 |
| 崩溃面 | fakerFG 崩溃会带走游戏；consumer 崩溃只影响插帧 | producer/consumer 中不做标题资源决策；保留 `Passthrough`/`Mode=off` 回退 |
| marker 可见性 | 标记画在链缓冲四角，游戏窗口本身带标记（覆盖窗口负责干净呈现） | 仅在覆盖模式下使用；`marker_disable.on` 可关；文档说明 |
| 1:1 依赖 | 窗口缩放/非整数倍时 marker 与尺寸检查会失败 | 文档明确要求无边框 + 分辨率 1:1；UI 启动前提示 |

---

## 5. 优化 backlog（建议顺序）

### 本轮已落地（2026-11-14）

| 模块 | 改动 | 对应实测问题 |
|---|---|---|
| producer | `fakerFG.ini` 默认 `StreamPercent=150`；COPY-only 列表首次遇到自动回退 100 | 4K 跨卡 63.3 → 28.1 MB/帧；UE5 COPY 列表不再整帧丢数据 |
| consumer | 默认 `copy-on-publication + --pub-fifo`（新增 `--no-pub-fifo` 回退） | 预拷 miss/轮转猜槽；`pf` 只剩拷贝未完成的补拷 |
| consumer cadence | 默认 `--cadence-source mean`：smooth 8 + 相位 k 直接应用 + 重复帧不推进；`tick` 仅 A/B | 35–45 实测 tick 切换过多（12–38/40）导致卡顿，回到文档验证的 mean+phase |
| 测试配方 | **默认固定 3x（锁 40）**；固定 4x（锁 30）备选 | 固定 3x：`HITCH=0`、`presents=120/120`、`load 13–54%`；固定 4x 基本平滑；动态逐帧因 XeFG per-frame count 切换仍卡（HITCH31 / missing53） |
| consumer colour | 颜色 sink 双缓冲 → **三缓冲**（`kColourSinks=3`），GPU 落后 >=3 帧才跳帧 | 残余 `colour=168 ms`（GPU 落后 2 帧时撞车） |
| fakerFG | 无代码改动 | 目前没有实测到的性能瓶颈（marker 成本 <1 ms，publish 22 ns） |
| bridge / UI | 无改动 | legacy / 非性能路径 |

**动态逐帧验收**：启动后 `cadence hist` 直接是目标档（40 fps → 3x，35–45 → 3x/4x），
不会先出现 6x；`xN` 长期贴近整数；`arr(vb)` 波动时 `HITCH` 不持续增长；
`--cadence-source mean` 可作 A/B 对照。


### P0：一到两周内，低风险、立刻见效

| # | 事项 | 收益 | 验收 |
|---|---|---|---|
| 1 | ✅ 文档纠偏：ARCHITECTURE / CONSUMER_ARCH / USAGE / BUILD / CONFIG / DEPLOY / UI / PROTOCOL 已按当前链路重写，旧版归档 | 新人不再走错路 | 仍待：根 README 主体、RELEASE_NOTES、ROADMAP |
| 2 | 统一版本号与发布脚本；补当前版本 RELEASE_NOTES | 包与文档可对应 | `dist/mgpu-<ver>`、CHANGELOG、UI 三处一致 |
| 3 | ✅ `StreamPercent` 默认改 150（发布 ini）；COPY-only 列表运行时自动回退 100 | 4K 跨卡 63→28 MB/帧 | 已改；回退有日志 |
| 4 | ✅ 预拷默认改为 copy-on-publication + `--pub-fifo`（`--no-pub-fifo` / `--no-prefetch-publications` 可回退） | 不再轮转猜槽，`pf` miss 只剩队列溢出 | 40 帧窗口 `pf`、`cq` p95 |
| 5 | ✅ 颜色 sink 三缓冲（`kColourSinks=3`，落后 >=3 帧才跳帧） | 消掉残余 `colour=168 ms` | 10 分钟 `SLOW frame`/`HITCH` |
| 6 | 清理 API 死符号 + NOTICE/LEGAL + MIT 注释 | 减少误读与许可瑕疵 | `grep record_scar` 只剩 deprecated 说明 |

### P1：一个月内，结构性改善

| # | 事项 | 收益 | 验收 |
|---|---|---|---|
| 7 | 抽纯逻辑模块 + L0/L1/L2 测试 + 轻量 CI | 回归从"真机 52 s"降到提交级 | CI 绿；marker/cadence/stream 单测通过 |
| 8 | consumer M3：input 线程 + publication 队列 | `pf` 40/40、`arr` 3x ≥95%、`total ≤24.5 ms` | 同场景 A/B 不回归 |
| 9 | UI 与 consumer 共用参数 schema；GPU 枚举；诊断包导出 | 参数不再漂移；提 bug 效率 | UI 新增开关不需要改两处 |
| 10 | Windows 安装/卸载器 + 打包清单断言 | 用户可自助安装/回滚 | 安装后直接可跑；卸载还原 |
| 11 | 已知问题清单 + marker 专题 + 真机 checklist | 知识可交接 | 新读者按文档能定位代码 |
| 12 | 归档 bridge / 旧脚本 / 旧 dist | 降低双维护与误发风险 | `src/bridge` 标 legacy；`scripts/legacy/` |

### P2：中期，产品/质量选择

| # | 事项 | 说明 |
|---|---|---|
| 13 | 3x/4x 画质与稳定性专项 | 当前 2x 是原生；3x+ 依赖补丁，需明确"可交付/研究"边界 |
| 14 | 更多游戏/引擎验证（PRAGMATA、UE5 变体、RE Engine） | 输入语义与加载点差异大，需逐个记录 |
| 15 | 颜色走 producer（去掉 WGC） | 历史实测渲染卡 +10–15%，**当前否决**；仅当 WGC 成为硬瓶颈再评估 |
| 16 | 5K / 高分辨率路径 | `--scale 0.75`、`--den` 与生成分辨率解耦 |
| 17 | 消费端与 bridge 合并 core（或彻底删除 bridge） | 长期维护成本 |

---

## 6. 建议的文档结构（整理后）

```text
README.md                          ← 仍待按当前链路重写主体（已加横幅）
docs/
  README.md                        ← 索引与"文档新鲜度"表
  PROJECT_STATUS_2026-11-14.md     ← 本文（状态/审计/backlog）
  design/
    ARCHITECTURE.md                ← 已重写：当前架构
    CONSUMER_ARCH.md               ← 新增：consumer 16 模块/线程/marker/配对
    XEFG_CHAIN_STANDARD.md         ← fakerFG 链路权威
    XESS_MFG_PRODUCER.md           ← producer
    PROTOCOL.md                    ← 已升到 v17
    PIPELINE_V2.md / PIPELINE_V3.md← 历史链路 + marker 前置说明
    FAKER_FG.md / INPUT_CONTRACT.md ← 定案/输入契约（含进度注记）
    BRIDGE_ARCH.md / BRIDGE_V2.md  ← legacy bridge
    archive/
      ARCHITECTURE_reshade_addon.md
  ops/
    USAGE.md                       ← 已重写：当前发布包
    BUILD.md                       ← 已重写：四件产物 + legacy
    DEPLOY.md                      ← 已重写：当前部署/回滚
    CONFIG.md                      ← 已重写：fakerFG.ini + consumer 参数
    UI.md                          ← 已重写：consumer 参数映射
    DEPENDENCIES.md / LEGAL.md     ← 部分历史内容待清理
    archive/
      USAGE_bridge.md / BUILD_legacy.md / DEPLOY_addon_pack.md
      CONFIG_game_capture.md / UI_bridge.md
  findings/
    PERF_ROUND_HANDOVER.md         ← 当前性能基线
    ...（历史结论保持原样，标日期）
```

---

## 7. 附：文件与规模速查

| 路径 | 规模 | 角色 |
|---|---|---|
| `src/faker_fg/` | 14 cpp / ~9.3k 行 | 游戏侧 FG 管线 |
| `src/xess_mfg_producer/` | 1 cpp / 1.4k 行 | 跨卡传输 |
| `src/xess_mfg_consumer/` | 16 模块 / ~12.9k 行 | 当前消费端 |
| `src/bridge/` | 16 模块 / ~10.8k 行 | 旧消费端（冻结） |
| `src/mgpu_ui/` | 2.2k 行 | 启动器 |
| `src/*.h`（ABI/工具头） | ~2.6k 行 | 共享协议、XeFG 动态加载/补丁、present ring |
| `src/faker_fg/shaders/` | 6 个 HLSL | marker、MV 重采样、UI/HUD（后三者默认关） |
| `src/xess_mfg_producer/shaders/` | 2 个 HLSL | MV/深度降采样 |
| `src/xess_mfg_consumer/shaders/` | 1 个 HLSL | 联合升采样 |
| `scripts/` | ~50 个 | 构建/部署/发布/分析/诊断（含历史） |
| `tests/` | 9 个目录 | 历史探针，不参与发布 |
| `dist/mgpu-0.11-marker/` | 15 个文件 | 最新当前链路发布包 |
| `dist/mgpu-0.15.7/` | bridge 时代 | 历史包（勿与当前包混发） |

---

> 维护约定：本文件是**状态快照**，不是设计权威。设计以
> `docs/design/XEFG_CHAIN_STANDARD.md`、`docs/design/XESS_MFG_PRODUCER.md` 为准；
> 性能数字以 `docs/findings/PERF_ROUND_HANDOVER.md` / `PERF_BASELINE.md` 为准。
> 状态变化时请更新本文顶部的日期与 §0/§3 两张表。

