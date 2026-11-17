# lock_frame 正式版发布规划（2026-11-17）

> 状态：**规划已确认，首批功能已落地**。本文同时记录规划与实施状态。
>
> 目标版本：`lock_frame 1.0.0`（内部代号 `v28-fixed`）
>
> 产品定位：**开源发布（GPL-3.0）、双卡插帧消费端 + 游戏侧 fakerFG 输入层**。默认面向
> 单机/离线游戏；固定倍率 **2x / 3x / 4x** 由用户选择；fakerFG 侧开放
> **DLSSG / NVNGX / Upscaler / FSR** 四种输入车道（DLSSG 为默认且已实测）。
>
> **决策记录**
>
> - 2026-11-17：发布路线确定为**开源（GPL-3.0）**。原 §0.1 的 GPL 阻断项解除；
>   发布二进制时同时提供完整对应源码与 NOTICE 即可。仍需处理 Intel 库许可与
>   `--unlock-mfg` 的合规风险。
> - 2026-11-17：**3x/4x 保留 `--unlock-mfg` 现状**，UI 继续发出该开关，保证
>   非 Intel 卡上 3x/4x 开箱即用。接受 Intel 许可风险；发布说明必须显式声明
>   该开关会修改运行时库、可能导致许可/反作弊问题。
>
> **实施记录**
>
> - 2026-11-17：已落地 F1（`--mode 2x|3x|4x`，旧 `30x4/40x3` 保留别名）和
>   F2 的第一层（UI 新增“fakerFG 输入源”，写入/备份游戏目录 `fakerFG.ini`，
>   并强制 `FgOutput=Capture`、`Passthrough=0`）。UI 还新增“游戏窗口”下拉，
>   按目标显示器刷新率计算建议锁帧；`lock_frame.exe` / `lock_frame_ui.exe`
>   均已重新构建通过。剩余项见 §3.3–§3.7 与 §4。
> - 2026-11-17：新增 `version.h`（1.0.0）、UI 环境自检卡（版本/协议、GPU、
>   XeSS 运行时、游戏配置）、日志轮转（保留 8 份运行日志）。
> - 2026-11-17：新增 `release.sh`、`deploy_game.bat`、`collect_diagnostics.bat`、
>   `write_manifest.py`、`check_release.py`；已实际生成
>   `dist/lock_frame-1.0.0.zip` 和 `dist/lock_frame-1.0.0-src.zip`，校验通过。
> - 2026-11-17：consumer/UI 加 VERSIONINFO 与 `--version`；构建脚本支持
>   `MGPU_STAGE` / `MGPU_FXC` / `MGPU_VSDIR`；nlohmann/json 已 vendoring 到
>   `lock_frame/third_party/`，fakerFG 不再依赖未跟踪的 OptiScaler checkout。

---

## 0. 先看结论

正式版不是“把现在的尝鲜版改个版本号”，有 **1 个已解决的许可决策、2 个剩余
合规问题、5 个会直接影响用户第一印象的工程缺口**。

### 0.1 已决策：开源发布（原阻断项解除）

**项目本身是 GPL-3.0。选择开源发布后，GPL 不再是阻断项**：发布二进制时同时
提供（或按 GPL-3.0 的方式指向）完整对应源码即可。下面这些“污染点”从“必须移除”
变为“必须在源码包和 NOTICE 中如实保留”。

| 位置 | 来源 | 现状 | 开源发布处理 |
|---|---|---|---|
| `lock_frame/lock_frame.cpp:61` 引入 `xefg_dynamic_count.h` | OptiScaler `XeFG_DynamicCount.h`（GPL-3.0） | `--dyn-mfg` 控制器，锁帧模式下已强制关闭，但仍被编译进 `lock_frame.exe` | **保留**，随源码发布，并保留 GPL-3.0 头注释；`--dyn-mfg` 仍只作为实验开关 |
| `lock_frame/modules/040_cadence.inc:651-750` | 同上 | `XeFG_DynamicCount` 的全部接线 | 保留，随源码发布 |
| `lock_frame/faker_fg/backend_xefg.cpp:20-21` | OptiScaler `framegen/xefg/*`（GPL-3.0） | 文件头明确写了“flow follows” | 保留来源声明，随源码发布；在 NOTICE 中列明 |
| `lock_frame/faker_fg/fg_internal.h:18-19` | 同上 | 文件头声明镜像 OptiScaler 的 FG 流程 | 同上 |
| `lock_frame/faker_fg/passes.cpp`、`proxy_*.cpp`、`provider.cpp` | OptiScaler 移植/对照 | 多处功能与 OptiScaler 高度同构 | 同上 |
| `lock_frame/faker_fg/proxy_kernel.cpp:31` | `nlohmann/json.hpp` | 实际头文件来自未跟踪的 `third_party/OptiScaler-master/external/`，构建依赖不在版本库里 | vendoring 到 `third_party/nlohmann/`，附 MIT 声明；或移除 JSON 解析 |
| 根目录 `LICENSE` / `NOTICE` | GPL-3.0 | 与开源发布一致 | 保留，并补齐发布合规清单（源码包、第三方声明） |

> 结论：**不再需要净室重写**。但发布包必须同时提供完整源码（或 GPL 允许的
> 等价方式），并且 NOTICE 要覆盖 OptiScaler、MinHook、FidelityFX、nlohmann 等
> 所有随二进制分发的第三方组件。

### 0.2 剩余的合规/许可问题

1. ~~闭源路线怎么走~~ **已决策：开源（GPL-3.0）**。发布时提供完整对应源码；
   不再需要净室重写，也不需要更换 LICENSE。
2. ~~**3x / 4x 是否需要 `--unlock-mfg`**~~ **已决策：保留现状**。
   - UI 继续发 `--unlock-mfg`，非 Intel 卡上 3x/4x 开箱即用。
   - 已知风险：该开关在运行时修改 `libxess_fg.dll` / `libxell.dll` 的机器码，
     Intel 许可禁止修改；硬编码 RVA 只对实测版本成立。
   - 缓解：启动日志打印补丁目标与结果；补丁失败/版本不匹配时回退 2x 并给出
     可读警告；发布说明显式声明风险；不随包分发 Intel DLL。
3. **是否随包分发 Intel 运行时**
   - Intel 许可允许**原样再分发**，但必须附版权与许可文本；**不允许修改**。
   - 建议正式包不内置 `libxess_fg.dll` / `libxell.dll`，首次启动做依赖检查并给出
     “从哪里获取、放到哪里”的明确指引；或者单独提供“运行时包”，与主包分离。

### 0.3 五个直接影响体验的工程缺口

1. 固定倍率只有 `30x4 / 40x3` 两个写死的选项，且默认假设 120 Hz 面板。
2. fakerFG 的 `FgInput` 只能手改 ini，UI 不暴露、不校验、不备份、不按游戏记忆。
3. 没有正式发布脚本、版本资源、校验值、安装/卸载说明、首次启动自检。
4. 文档与代码不一致：`README` 写 16 槽，`PRE_RELEASE` 写 32 槽，实际
   `mgpu_shared.h:234` 是 `kMaxSlots = 32`；`lock_frame.cpp:716` 的注释仍写
   “默认 200”，实际 producer 默认 150。
5. 构建脚本硬编码本机路径（`/mnt/c/temp/mgpu`、VS2022 BuildTools、
   Windows Kits `10.0.26100.0`），换一台机器无法复现发布物。

---

## 1. 现状盘点（作为规划依据）

### 1.1 已经成立的部分

- 链路职责清晰：`fakerFG.dll`（游戏进程，输入记录 + marker）→
  `xess_mfg_producer.dll`（跨卡 MV/depth）→ `lock_frame.exe`（WGC 颜色 +
  marker 精确配对 + XeFG 上屏）。
- 颜色走 WGC，渲染卡不再支付 4K 颜色拷贝；MV/depth 默认 `StreamPercent=150`，
  消费端用 `upsample_joint.hlsl` 联合重建到输入画面尺寸。
- marker 是自包含的 32-bit code（seq12 + ms12 + CRC-8），四角冗余；配对失败时
  **不猜槽**，直接 1X，这是正确的安全性取舍。
- 已有健康状态机、光流降级、session/ACK 恢复、宽 depth 转换、窄带
  `--list-modes`、UI 热键/停止阶梯、stray 进程清理。
- 实测数据已经够支撑正式版叙事：30x4 长跑精确配对约 99.2%、输出约 99.77%，
  40x3 稳态 `exact=40 no_slot=0`。
- fakerFG 的输入车道代码已经存在：`FgInput=DLSSG / NVNGX / Upscaler / FSR`，
  且 `FgInput=DLSSG` 已在 RED、UE5、UE4 多款游戏实测符合预期。

### 1.2 代码规模与结构

| 部分 | 规模 | 说明 |
|---|---|---|
| `lock_frame/lock_frame.cpp` + `modules/*.inc` | 约 11.5k 行 | 单翻译单元、17 个模块，依赖顺序敏感 |
| `lock_frame/producer/producer.cpp` | 约 2.1k 行 | 独立 DLL，v28 共享布局 |
| `lock_frame/faker_fg/*` | 约 9.0k 行 | 14 个实现文件 + 2 个代理 DLL 身份 |
| `lock_frame/ui/mgpu_ui.cpp` | 约 2.2k 行 | 手绘深色 UI，无子窗口 |
| 合计 | 约 25k 行 | 其中包含大量 A/B 开关和 legacy 路径 |

### 1.3 共享协议现状

- `kSharedVersion = 28`，`kMaxSlots = 32`，`kStreamMaxW/H = 3840x2160`。
- UI 启动前会校验共享块的 `magic` 和 `version`（`mgpu_ui.cpp:976-992`），
  版本不匹配会拒绝启动；这是正式版必须保留的行为。
- 目前没有“组件版本清单”和“最低/最高兼容版本”的对外说明，用户混装旧
  producer / 新 consumer 时只能看到失败。

---

## 2. 正式版的产品边界

### 2.1 做什么

- 固定倍率 **2x / 3x / 4x**，用户显式选择；不做“自动倍率”作为默认。
- 目标输出 = 面板刷新率（默认 120 Hz），输入锁帧 = 刷新率 / 倍率。
- fakerFG 输入车道可在 UI 里选择，并写入/备份游戏侧 `fakerFG.ini`。
- 提供首次启动自检、依赖检查、日志打包、卸载/回滚说明。
- 开源发布（GPL-3.0）：随二进制提供完整对应源码与第三方声明。
- 支持单机/离线游戏；明确“不要在带反作弊的在线游戏中使用”。

### 2.2 不做什么（v1.0 明确不做）

- 不做动态逐帧/自动倍率作为默认卖点（保留“低帧自适应”作为实验开关）。
- 不做游戏内覆盖 UI（OSD 仍由 consumer 画）。
- 不做自动更新器（首版用整包替换 + 校验值）。
- 不支持独占全屏；统一要求无边框/窗口化全屏。
- 不支持 TSR/FSR1/原生 TAA（没有可靠 MV）。
- 不承诺所有游戏；首版给“已验证清单”，其余为“可尝试”。

---

## 3. 功能规划

### 3.1 F1：固定倍率 2x / 3x / 4x（P0，已落地）

**现状**

- UI 的 `fg_mode` 只有 0/1 两个值（`mgpu_ui.cpp:110`），下拉只有
  “30x4 / 40x3”（`:338`），启动命令固定发 `--mode 30x4|40x3`（`:770-775`）。
- consumer 的 `--mode` 只认 `40x3`，其余一律按 `30x4` 处理
  （`modules/160_main.inc:330-350`），并且这两个模式把“120 fps 输出”写死。
- `--interp` 本身已经支持 1..7（`:157-160`），所以底层能力是够的，缺的是
  产品层建模和 UI。

**目标模型**

用一张数据表描述固定倍率，而不是继续堆 `if`：

| 用户选择 | 插帧数 `--interp` | 120 Hz 面板输入 | 输出 | XeFG 库要求 |
|---|---|---|---|---|
| 2x | 1 | 60 fps | 120 fps | 原版库即可 |
| 3x | 2 | 40 fps | 120 fps | 原版库 + `--unlock-mfg`（当前实测 1.3.1.78） |
| 4x | 3 | 30 fps | 120 fps | 原版库 + `--unlock-mfg`（当前实测 1.3.1.78） |

**刷新率适配**

- 启动时读取**目标显示器**（不是主显示器）的当前刷新率。UI 现在用的是
  `EnumDisplaySettingsW(nullptr, ...)`（`mgpu_ui.cpp:444-450`），在有副屏/
  覆盖屏时会读错；应改为读覆盖窗口所在显示器。
- 推荐输入 = `refresh / multiplier`。只允许能整除的组合；不能整除时在 UI 里
  显示“该刷新率无法被 N 等分”，并提供“改用 120 Hz / 改用 2x”的建议。
- 首版建议先支持 120 Hz 的完整组合，再扩展 60/144/165/240 Hz；对 165 Hz
  这类不能整除的刷新率，明确提示“需要 VRR 或改刷新率”，不静默降级。
- 保留 `--fg-out-hz N` 作为高级选项，但 UI 默认只发“面板刷新”。

**输入锁帧**

- consumer 不做丢帧锁帧（现有 `--mode` 故意不启用 `--lock`，因为 WGC 抖动会
  周期性丢真实帧，见 `160_main.inc:331-333`）。正式版必须把这个约束变成
  **UI 的显式引导**：
  - 显示“请在 RTSS 或游戏内把帧率锁到 X fps”；
  - 提供“一键复制建议锁帧值”；
  - 启动后如果检测到输入长期低于目标，给出“输入未锁住”的提示，而不是让用户
    自己看日志。
- `--lock` / `--lock-auto` 保留为高级/实验开关，不出现在默认 UI。

**实现落点**

1. `modules/160_main.inc`：把 `--mode` 改为 `--mode fixed --mul 2|3|4`（或
   新增 `--mul`），保留旧字符串解析做兼容；`g_fg_interp = mul - 1`；
   `g_fg_interp_set = true`。
2. `modules/030_devices_capture_fg.inc`：把 `g_fg_interp_max` 检查提前到
   “选择倍率”处，倍率高于库能力时给结构化错误（错误码 + 用户文案 + 日志），
   并回退到可用倍率。
3. `ui/mgpu_ui.cpp`：
   - `State.fg_mode` 改为 `multiplier`（2/3/4）；
   - 下拉选项改为“2x / 3x / 4x”，每项附带“输入 60 / 40 / 30 fps @120Hz”；
   - “建议锁帧”行改为按目标显示器刷新率实时计算；
   - 启动命令改为 `--interp N`，不再依赖两个写死的 `--mode` 字符串。
4. 新增 `lock_frame/modes.h`（或等价的小头文件）作为**唯一倍率表**，UI 和
   consumer 共用，避免两边各写一份。

> 已完成：consumer `--mode 2x|3x|4x` + 旧别名、UI 三档下拉、按目标显示器
> 刷新率计算建议锁帧、不能整除提示。未完成：启动后把补丁结果/实际倍率回显到
> UI（当前看 consumer 日志）。

**验收**

- 三种倍率在 120 Hz 下分别输出 120 fps，`pair: exact` ≥ 99%，`show` ≥ 118。
- 2x 在原版 `libxess_fg` 上无需任何补丁即可工作。
- 3x/4x 启用 `--unlock-mfg`；补丁失败/库版本不匹配时，UI 明确提示并回退 2x，
  不出现黑屏或“假 4x”。
- 非 120 Hz 面板：能整除时正常；不能整除时给出明确提示，不崩溃。

### 3.2 F2：fakerFG 输入车道开放（P0，第一层已落地）

**现状**

- `FgInput` 已支持四个值，解析在 `faker_fg.cpp:552-643`；`FgInput` 不写时
  默认 `NoFG`，显式写了输入但没写输出时自动落到 `Capture`。
- UI 完全不感知 `FgInput`，用户只能手改游戏目录的 `fakerFG.ini`。
- 默认 `fakerFG.ini` 是 `FgInput=DLSSG`（`lock_frame/fakerFG.ini:18`）。
- 不同车道的派生开关差异很大（`fake_dlssg_feature`、`fake_sl_player`、
  `strip_dlssg`、`tag_at_evaluate`、`write_output_real/interp`），手改很容易
  改出“半配置”状态。

**目标**

- UI 提供“输入源”下拉：`DLSSG（推荐）`、`NVNGX`、`Upscaler`、`FSR`。
- UI 负责把选择写进游戏目录的 `fakerFG.ini`：
  - 写入前备份为 `fakerFG.ini.bak_<时间戳>`；
  - 只改 `FgInput`（以及必要的派生键），其余用户配置保持不动；
  - 写入后提示“重启游戏生效”。
- 启动前做一次“输入源 ↔ 游戏实际 API”自检：
  - `DLSSG`：要求游戏开启 DLSS Frame Generation（或至少调用 DLSS-G 契约）；
  - `NVNGX`：要求游戏走 NVSDK_NGX_DLSSG 参数块；
  - `Upscaler`：要求游戏开启 DLSS Super Resolution；
  - `FSR`：要求游戏加载 AMD FidelityFX FrameGeneration DLL 并调用
    `FFX FrameGenerationPrepare`。
- UI 显示自检结果和最近一次日志中的 `config: FgInput profile:` 一行。

**v1.0 范围建议**

| 车道 | v1.0 定位 | 理由 |
|---|---|---|
| `DLSSG` | **默认、正式支持** | RED / UE5 / UE4 已实测 |
| `NVNGX` | 实验性，可选 | 代码存在，但需要更多游戏验证 |
| `Upscaler` | 实验性，可选 | CP2077 等 DLSS-SR 路径可用，但依赖 SR 纹理 |
| `FSR` | 实验性，可选 | FFX 钩子存在，验证面最窄 |

**暂不做 `FgInput=Auto`**：自动选车道需要在同一帧里仲裁多个 lane 的记录，
虽然 `publish_record()` 已有同帧折叠/去重逻辑，但正式版首版不值得引入这个
不确定性。等四车道各自稳定后，再在 1.1 做 Auto（优先级 DLSSG > NVNGX >
Upscaler > FSR）。

**实现落点**

1. `faker_fg.cpp`：增加配置校验函数，把非法组合（例如 `FgInput=FSR` 但
   游戏没有加载 FFX）变成明确的日志和 UI 可读的状态位；保留“读一次”的规则。
2. 新增 `lock_frame/fakerfg_profiles/`：按 exe 名存放推荐 ini 模板
   （如 `Cyberpunk2077.exe.ini`、`PBZ-Win64-Shipping.exe.ini`），UI 首次识别
   游戏时提示“使用推荐配置”，不自动覆盖。
3. `ui/mgpu_ui.cpp`：新增“游戏侧配置”卡片，包含输入源下拉、配置文件路径、
   备份/恢复、打开日志目录。
4. 启动命令与 fakerFG 配置解耦：consumer 不因输入源变化而改变参数；输入源
   只影响游戏进程。

> 已完成：UI 下拉 + 备份写入 + `FgOutput=Capture`/`Passthrough=0`；
> 未完成：按 exe 名的推荐配置模板、启动前 API 自检。

**验收**

- 四种车道分别至少 1 款游戏跑通“画面正常 + `pair` 正常 + 无黑屏”。
- 切换车道后 UI 能正确备份/恢复 `fakerFG.ini`。
### 3.3 F3：首次启动向导与依赖检查（P0，环境自检已落地）

正式版第一次启动必须回答四个问题，而不是让用户看日志：

1. 是否满足系统要求（Windows 10 1903+ / Windows 11、D3D12、WGC 可用）。
2. 是否找到两块 GPU，且游戏窗口所在显示器由哪块 GPU 驱动。
3. 是否找到 `libxess_fg.dll` / `libxell.dll`，版本是否被支持
   （`GetProperties` 的 `maxSupportedInterpolations`）。
4. 游戏侧是否已放好 `fakerFG.dll` / `xess_mfg_producer.dll` / `fakerFG.ini`，
   共享块版本是否匹配。

失败时给出“缺什么、放哪里、下一步做什么”，并提供“复制诊断信息”按钮
（不联网、不上传）。

### 3.4 F4：配置与档案（P1）

- 现有 profile 机制保留，但把“游戏 exe / 窗口标题 / 输入源 / 倍率 / 输出刷新率”
  一起存进档案。
- 新增“按游戏自动匹配档案”：窗口标题或进程名匹配时自动选中。
- 首次运行生成默认档案；配置损坏时回退到默认值并提示，不崩溃。
- 所有写 ini 的操作都要有备份和原子替换（先写临时文件再 `MoveFileEx`）。

### 3.5 F5：诊断与日志（P1，日志轮转/诊断包已落地）

- 日志保留策略：单文件上限（例如 16 MB）+ 最多 5 个滚动文件，避免长期运行
  把磁盘写满。
- UI 增加“一键打包诊断”：收集 `lock_frame_ui.log`、consumer 输出、
  `*_fakerFG.log`、`*_mfg_producer.log`、`lock_frame_ui.ini`、系统/GPU 信息，
  打成一个 zip；不包含游戏资产。
- OSD 增加三项正式版必须可见的信息：
  - 当前倍率与目标输入帧率；
  - `pair` 健康度（exact / no_slot / unreadable）；
  - 当前使用的是游戏 MV 还是光流降级（已有 `mvN`，保留）。
- 日志里统一加 `build=<version> proto=<kSharedVersion>`，方便支持定位。

### 3.6 F6：健壮性（P1）

- 启动顺序：先检查共享块版本，再选 GPU，再建覆盖窗口；任一步失败都给出
  可读错误，不留下 topmost 的游戏窗口。
- consumer 退出时恢复游戏窗口的 `WS_EX_TOPMOST`（现有 UI 已有安全网，保留）。
- 游戏切分辨率/切 HDR/切全屏时：consumer 应能重连或明确提示重启，不静默黑屏。
- 增加“看门狗”：consumer 心跳超过 N 秒没有新 publication 时，UI 状态栏显示
  “等待游戏发布”，而不是只显示 0 fps。
- 长时间运行（≥2 小时）内存/句柄不增长；共享映射在退出时正确关闭。

### 3.7 F7：画质与性能默认值（P1）

- 固定 `StreamPercent=150` + 消费端 `--upsample` 作为默认，不再在 UI 暴露
  `--stream-auto` / `--stream-asym`。
- 为 2x/3x/4x 分别给一份“已验证默认”：
  - 2x：延迟最低，优先推荐给 60/120 Hz；
  - 3x：40 fps 输入，真实帧更多；
  - 4x：30 fps 输入，负载最高，作为“高倍率”选项并提示延迟代价。
- 文档里明确写出：同样 120 fps 输出，延迟 2x < 3x < 4x；4x 不是“更流畅的
  万能选项”。
- 保留低帧自适应（`--interp-cadence`）为实验开关，默认关闭，并在 UI 上说明
  可能产生 flicker/ghosting。
- 增加一次 4K/1440p 的显存与共享内存实测，写进发布说明：
  32 槽 × 150% MV/depth 的实际占用、consumer 峰值显存、推荐配置。

---

## 4. 工程与发布规划

### 4.1 代码清理（P0/P1）

| 项 | 动作 | 优先级 |
|---|---|---|
| `xefg_dynamic_count.h` + `--dyn-mfg` | 保留（GPL-3.0），随源码发布；UI 不暴露 | P1 |
| OptiScaler 派生代码 | 保留 GPL 来源声明，随源码发布；NOTICE 列明 | P0（开源合规） |
| `nlohmann/json.hpp` | vendoring 到 `third_party/nlohmann/`，附 MIT 许可；或去掉 JSON 解析 | P0（构建可复现） |
| 死开关/legacy 参数 | 列出“正式版保留 / 隐藏 / 删除”三张清单；UI 只保留用户能理解的项 | P1 |
| 文档不一致 | 统一槽位数（32）、StreamPercent（150）、协议版本（v28）、版本号 | P0 |
| 单 TU 模块顺序 | 保留现状，但加一个 `scripts/check_lock_frame_modules.py` 校验 include 顺序和符号 | P2 |
| 版本资源 | 给三个 exe/dll 加 `VERSIONINFO`（公司名、产品名、版本、协议版本） | P1 |
| 调试符号 | 发布包不带 `.pdb` / `.map`；内部保留符号服务器或单独归档 | P1 |

### 4.2 构建与可复现（P0，环境变量与发布脚本已落地）

- `lock_frame/build.sh` 目前硬编码：
  - `MGPU_STAGE=/mnt/c/temp/mgpu`
  - `FXC_WIN=/mnt/c/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64/fxc.exe`
  - VS2022 BuildTools 路径
- 正式版要求：
  1. 所有路径可由环境变量覆盖，缺省值写进 `docs/ops/BUILD.md`；
  2. 记录工具链版本（MSVC、Windows SDK、fxc）到 `build_info.txt`；
  3. 构建脚本失败即停（现在 `set -uo pipefail` 但没有 `-e`，部分失败会被吞）；
  4. 发布物从干净工作树构建，构建脚本输出 SHA-256。
- 建议增加一个 `lock_frame/release.sh`：
  - 参数：`VERSION=1.0.0`、`CHANNEL=stable|beta`；
  - 构建四件产物；
  - 生成 `manifest.json`（版本、协议版本、文件列表、SHA-256、构建时间、工具链）；
  - 生成 zip，不包含 Intel DLL（除非明确选择 runtime 包）；
  - 生成 `README.txt`、`LICENSE-THIRD-PARTY.txt`、`CHANGELOG`。

### 4.3 发布包布局（建议，release.sh 已落地）

```text
lock_frame-1.0.0/
  lock_frame_ui.exe
  lock_frame.exe
  fakerFG.dll
  xess_mfg_producer.dll
  fakerFG.ini                    ; 默认 FgInput=DLSSG / StreamPercent=150
  lock_frame_ui.ini              ; 可选：默认档案
  README.txt                     ; 5 分钟上手 + 已知限制
  CHANGELOG.txt
  LICENSE                        ; GPL-3.0 全文
  NOTICE                         ; 第三方组件与来源声明
  THIRD-PARTY-NOTICES.txt
  manifest.json
  sha256.txt
  docs/
    SUPPORTED-GAMES.md
    TROUBLESHOOTING.md
    UNINSTALL.md
  profiles/                      ; 按游戏 exe 的推荐配置模板
```

- 不包含：`libxess_fg.dll`、`libxell.dll`（除非单独 runtime 包）、`.pdb`、
  `.map`、构建中间文件、任何游戏资产。
- **GPL-3.0 源码义务**：随二进制提供完整对应源码。建议发布页同时挂
  `lock_frame-1.0.0-src.zip`（含 `lock_frame/`、`src/`、构建脚本、NOTICE、
  第三方声明），或在 README 里给出可长期访问的仓库地址与对应 commit。
  只发二进制、不给源码不符合 GPL-3.0。
- `fakerFG.ini` 的注释要改成面向用户的语言；当前注释里还有“自用/预算”等
  内部表述，正式版要清理。

### 4.4 版本与兼容策略（P0）

- 语义化版本：`MAJOR.MINOR.PATCH`，本次 `1.0.0`。
- 协议版本独立：`kSharedVersion = 28` 写进 manifest 和日志。
- 兼容矩阵：
  - consumer / producer / fakerFG 必须同版本；
  - UI 可以跨小版本，但要求协议版本一致；
  - 旧版本混装时，UI 明确提示“请同时更新四个文件”，而不是只报错。
- 保留 `lock_frame/docs/PRE_RELEASE_20261117.md` 作为历史，不删；正式版另写
  `lock_frame/docs/RELEASE_NOTES_1.0.0.md`。

### 4.5 安全与开源发布措施（P1）

- 发布物关闭调试输出、剥离符号；不把源码路径写进二进制（检查 `__FILE__`
  使用）。
- 代码签名（可选，但对“正式版”信任度影响很大）：至少给 exe/dll 做
  Authenticode 签名；没有证书时在 README 说明校验值。
- 反篡改不是首版目标；不要引入会误伤用户的内核驱动/注入保护。
- **GPL-3.0 合规**：发布页提供源码包或长期仓库地址；源码包包含构建脚本、
  NOTICE、第三方声明、Intel 库获取说明；`LICENSE` 与 `NOTICE` 随二进制一起
  分发。
- **`--unlock-mfg` 风险告知**：README、UI 帮助文本、启动日志三处都要说明它会
  修改运行时库、只对特定版本有效、可能违反 Intel 许可或触发反作弊；默认不
  隐藏，但用户必须知情。
- 明确声明：本工具会向游戏进程注入 DLL，可能触发反作弊；**禁止在在线游戏
  使用**。

---

## 5. 测试与验收

### 5.1 必测矩阵

| 维度 | 取值 |
|---|---|
| 游戏 | CP2077、PBZ、Wuchang（UE5）、一款 UE4、RE Engine（可选） |
| 输入车道 | DLSSG（必测）、NVNGX / Upscaler / FSR（各至少 1 款） |
| 倍率 | 2x、3x、4x |
| 分辨率 | 1440p、4K |
| 色彩 | HDR10、SDR |
| 刷新率 | 120 Hz（必测）、60 Hz、144/165 Hz（至少各一次） |
| 时长 | 30 秒冷启动、10 分钟常规、2 小时长跑 |

### 5.2 验收指标

| 指标 | 目标 |
|---|---|
| 配对 | 30x4 长跑 `exact` ≥ 99%，40x3 稳态 `exact=40 no_slot=0` |
| 输出 | `show` ≥ 118/120（120 Hz）；空刷新 ≤ 1% |
| 稳定性 | 2 小时无崩溃、无设备移除、无持续黑屏 |
| 恢复 | 一次配对失败后 ≤ 2 秒回到 `OK`；不出现永久 `DEGRADED` |
| 启动 | 冷启动到出帧 ≤ 10 秒（不含游戏加载） |
| 资源 | 退出后无残留进程、无残留 topmost、映射可释放 |
| 体验 | 首次启动能在 UI 内完成依赖检查并看到明确结论 |

### 5.3 需要补的自动化

- 保留 `tests/pair_core` 等纯逻辑测试，并纳入发布前脚本。
- 新增 `scripts/check_lock_frame_release.py`：
  - 校验四件产物存在、版本资源一致、协议版本一致；
  - 校验发布包里没有 `libxess_fg.dll` / `.pdb` / `.map`；
  - 校验 `fakerFG.ini` 的默认键；
  - 校验 SHA-256 清单。
- 可选：在 Windows 上用 `--list-modes` 做一次无游戏的冒烟测试。

---

## 6. 里程碑与排期（单人开发估算）

> 前提：3x/4x 已确定保留 `--unlock-mfg`。M1 需要把补丁结果、库版本和回退
> 2x 的文案接到 UI/日志上。

| 阶段 | 内容 | 产出 | 估算 |
|---|---|---|---|
| M0 | GPL 合规发布准备（源码包、NOTICE、构建可复现） | 可发布的开源基线 | 1–3 天（已完成） |
| M1 | 固定倍率 2x/3x/4x + 刷新率适配 | consumer + UI 可用 | 3–5 天 |
| M2 | fakerFG 输入源选择 + 配置备份 + 自检 | UI 配置卡片 + 模板 | 3–5 天 |
| M3 | 首次启动向导、诊断打包、日志滚动 | 体验闭环 | 3–5 天 |
| M4 | 发布脚本、版本资源、清单、文档 | RC 包 | 2–3 天（已完成） |
| M5 | 全矩阵测试 + 修复 + 发布说明 | 1.0.0 GA | 7–10 天 |
| 合计 | | | **约 3–6 周** |

### 建议的发布门槛

- P0 全部关闭；
- 必测矩阵中“必测”项全绿；
- 2 小时长跑通过；
- 发布包能在一台**没有开发环境**的干净机器上按 README 跑通；
- 许可文件、第三方声明、校验值齐全。

---

## 7. 风险清单

| 风险 | 影响 | 缓解 |
|---|---|---|
| GPL 合规缺失（只发二进制） | 违反 GPL-3.0 | 随包提供完整源码 + NOTICE + LICENSE |
| `--unlock-mfg` 法律风险 | 收到 DMCA/许可投诉、反作弊误判 | 保留现状；日志/README 显式告知；失败回退 2x；不随包分发 Intel DLL |
| Intel DLL 再分发 | 许可违约 | 不内置；首次启动检查 + 获取指引 |
| 输入未锁帧 | 用户看到抖动/1X，误判为 bug | UI 显式引导 + 启动后检测 |
| 刷新率不整除 | 输出节奏不整 | 只允许整除组合，其余明确提示 |
| 多车道配置半生效 | 黑屏/1X | 配置校验 + 模板 + 备份/回滚 |
| 32 槽共享内存过大 | 低显存机器失败 | 发布说明写清容量；后续按倍率动态槽位 |
| 反作弊 | 账号风险 | 明确“仅单机/离线” |
| 无代码签名 | SmartScreen 拦截 | 签名或提供校验值 + 说明 |

---

## 8. 需要你确认的决策（按顺序）

1. ~~**闭源路线**~~ **已确定：开源（GPL-3.0）**。发布时提供完整对应源码，
   `LICENSE` / `NOTICE` 随包分发。
2. ~~**3x/4x 的解锁方式**~~ **已确定：保留 `--unlock-mfg` 现状**，UI 继续发出，
   用户知情并接受 Intel 许可/反作弊风险。
3. **v1.0 输入车道范围**：只正式支持 DLSSG（其余实验）/ 四车道都作为正式
   支持（需要更多实测）。
4. **刷新率策略**：首版只支持 120 Hz 的 2x/3x/4x（最稳）/ 同时支持 60/144/
   165/240 Hz（需要额外适配与测试）。
5. **发布形态**：免费/付费、是否签名、是否提供 runtime 包、更新方式。
6. **最低系统要求**：Windows 版本、是否要求 Win11、是否支持 8 GB 显存。

> 建议默认：1 已确定开源、2 已确定保留 `--unlock-mfg`、3 只正式支持 DLSSG、
> 4 先 120 Hz 再加其他、5 免费 + 签名（可选）、6 Win10 1903+ / 双卡 /
> 建议 ≥12 GB 显存（4K 4x）。

---

## 9. 确认后的执行顺序（不在本次执行）

1. 完成 GPL 合规发布基线（源码包、NOTICE、构建可复现）。
2. 先做 F1（倍率）和 F3（自检），因为这两项决定用户能不能用起来。
3. 再做 F2（输入源）和 F4/F5（档案/诊断）。
4. 最后做发布脚本与全矩阵测试。
5. 每个里程碑结束都更新 `CHANGELOG.md` 和本文的“已完成/未完成”状态。

---

## 10. 附：文档/代码一致性检查

| 位置 | 状态 |
|---|---|
| `lock_frame/README.md` 槽位说明 | 已改为 `kMaxSlots = 32`，v26 段标为历史 |
| `lock_frame/docs/PRE_RELEASE_20261117.md` | 32 槽 ring 与代码一致 |
| `lock_frame/lock_frame.cpp` 头部注释 | 已改为 `StreamPercent=150`、2x/3x/4x |
| `lock_frame/README.md` 模式说明 | 已改为 2x/3x/4x 模型 |
| `lock_frame/README.md` 刷新率说明 | 已改为“默认 120 Hz；其他按整除规则” |
| `lock_frame/ui/mgpu_ui.cpp` 版本标签 | 已去掉写死的 `XeFG 1.3.1`，改为 `XeFG (libxess_fg)` |
| `lock_frame/ui/mgpu_ui.cpp` GPU 标签 | `ID_GPU` 已不在界面显示；GPU 由 `ID_GPUSEL`/自检卡读取实际适配器 |
| `docs/ops/LEGAL.md` | 已补 GPL-3.0 发布检查清单 |
| `NOTICE` | 已补 nlohmann MIT 声明 |
| 发布包 | `release.sh` 已加入 `LICENSE`、`NOTICE`、manifest、SHA256、GPL 源码包 |



