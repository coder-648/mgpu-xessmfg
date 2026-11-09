# 变更记录

只记**用户能感知**的变化与对应实测依据。每条结论的原始数据在
`docs/findings/PERF_BASELINE.md` / `FINAL_CONFIG.md`。

## v0.13.2 —— 并行化复审：所有"还能并行"的地方都试过了，结论是默认全关（2026-11-09）

**一句话**：把 fg 链路上还能并行/异步的地方全部实现并实测了一遍 —— 3 个默认关的新开关
（跨卡拷贝走 COPY 队列、MV/深度提前一帧、add-on 把跨卡写搬出游戏队列）、1 个抗丢帧模式，
外加 3 个真修复。**新开关全部默认关闭，默认行为与本版之前完全一致**；每个开关的实测数据、
为什么留/为什么不用，都在 `docs/findings/PERF_BASELINE.md` §3.28–§3.30。

### 修掉的问题（默认生效）

1. **`--fg-warp` 两条 list 共用一份 command allocator。** 这条实验路径一个输入帧提交两条 list
   （插值中点 + 真帧），而中点那次 `Present()` 只是把 list 排进队列；原代码在两次提交之间
   `Reset()` 同一个 allocator —— D3D12 里对**还在执行**的 allocator 做 Reset 是未定义行为。
   现在两对 `(allocator, list)` 每帧轮换，靠下一帧开头的 `gpu_throttle()` 保证换进来的那对已退休；
   顺带修掉 `back_buffer()` 失败时第二条 list 往**已关闭**的 list 里录的问题。
2. **`WGC sees 60 fps but the game only renders 38.4 fps` 是假警报。** 它拿我们的**中位数**
   （16.7 ms）去比生产端的**一次原始采样**（26 ms）；真实情况是游戏 ~44 fps 落在 120 Hz 面板上，
   帧间隔在 2/3/4 个 vblank 之间正常跳动，两边**中位数**都只是"最常见的那个桶"。
   这条误报曾把排查引向"游戏自带 FG"（实测游戏 FG 是关的）。现在两边都取 16 帧中位数再比，
   并把 **DWM 重复合成**列为第二种可能原因（我们的顶层覆盖窗口会让游戏窗口失去独立翻转）。
3. **UI 的两个死键接上了。** `mgpu_ui.ini` 里的 `sdr` 与 `slack1` 一直存在，但
   `ID_SDR` / `ID_SLACK` **不在任何控件表里**、`build_cmdline` 也从不发对应开关 —— 等于摆设。

### 新增 UI 控件

| 位置 | 控件 | 作用 |
|---|---|---|
| 捕获 → **色彩空间** | `HDR10（默认）` / `SDR（BT.709）` | 选 SDR 发 `--sdr`：bridge 走 BT.709 + gamma 2.2，并把交换链色彩空间声明成 SDR。我们抓的是 scRGB(FP16)，HDR 源永不丢；在 HDR 显示器上选 SDR 会发灰，那是模式本身 |
| 帧生成 → **抗丢帧 (A/B)** | 开 / 关 | 发 `--slack-auto`：当排队的抓取帧确实带着**新内容**时，多留一帧而不是丢掉（代价是那一帧 +1 个间隔延迟） |

### 新增命令行开关（默认全关）

| 开关 | 作用 | 实测结论 |
|---|---|---|
| `--input-copy-queue` | MV/深度的跨卡拷贝走独立 COPY 队列，与颜色 convert draw 重叠 | `--den 2` 测不出来（重叠窗口只有 0.2–0.5 ms）；`--den 1` 单趟 A/B 有 −1…−2 ms 但体感更差（空刷新 34 vs 13）。**不用** |
| `--prefetch-inputs`（隐含上一条） | 把 slot N+1 的 MV/深度放在帧 N 的 Present 之后拷贝 | 机制 100% 命中（`pf 40/0`），但空刷新 0.32 → **2.90/窗**、15/60 窗口空 >4 个：搬运挪出帧外 → 循环跑赢 pacer → `--interp-cadence` 少要帧。**不用** |
| `[capture] OffloadQueue=1`（add-on） | 跨卡拷贝 + 我们自己 RT 的反向状态转换搬到 add-on 自己的队列 | 功能验收通过（`offload=9987`、`stalls=0`、`fail=0`、6 分钟无异常、回退链一次没触发），**收益未量到**（对照被场景漂移吃掉）。**不用**，游戏侧 add-on 已回滚 |
| `--slack-auto` | 抗丢帧：真有新内容时多留一帧 | 缺省关。实测它的 `held` 只有 3 次而丢了 20 次 ⇒ 20 次丢弃里生产端根本没发布新帧 = **重复帧**。建议**开着**（几乎零成本），但不是必须 |

### 顺手量到的事实（以后不用重测）

- **OSD 的 `drop N` 大多不是"掉帧"**：`--slack-auto` 的判据证明那些被丢弃的抓取帧里
  生产端 `frame_published` 没有变化 —— 是 DWM 把同一帧重复合成上来的，丢掉不损失画面。
  新日志把丢弃拆成 `[K real loss / M-K duplicate]`，**只有 K 是真丢内容**。
- **手工分次跑 A/B 在本机不成立**：同一配置两趟 `total` 差 **3.2 ms**、`age` 差 6 倍（7.9 vs 48.5 ms），
  比被测效应还大。必须用 `bin\12 - automatic A-B-C-A 4K (60s each).bat` 这种**自动连续**跑法。
- **我们自己没有任何硬编码帧率上限**：`--lock` / `--fg-out-hz` / `--pace` 默认全关，UI 里那行
  "建议锁帧"只是提示（不会传给 bridge）；交换链是 `Present(0)` + BufferCount 2 + MaxFrameLatency 1
  （限的是在飞帧数，不是帧率）。日志里的 `in=25 ms` 就是 RTSS 锁的 40 fps。

### 新增配方脚本

```
9  - 4K out 3x (A).bat                     4K 原生输出基线（--den 1 --interp 3）
10 - 4K out 3x + cq (B).bat                + --input-copy-queue
11 - 4K out 3x + prefetch (C).bat          + --prefetch-inputs
12 - automatic A-B-C-A 4K (60s each).bat   自动连续跑四段（本机唯一可信的 A/B 方式）
13 - smoothest + anti-drop (A-B).bat       配方 5 + --slack-auto
8  - smoothest + copy queue (A-B).bat      配方 5 + --input-copy-queue
```

### 这一版真正可用的东西

`--fg-warp` 的双 allocator 修复（真隐患）、`breakdown` 的 `cq=` 字段、
`collect_retired_inputs()` 的 swap-remove、UI 的「色彩空间」与「抗丢帧」两个控件、
`--slack-auto`、以及上面那条**假警报的修正**。其余全部默认关。

## v0.13.1 —— RE Engine / PRAGMATA 支持（2026-11-09，已实测通过）

**结论**：PRAGMATA 这条路走通了 —— `input source: ngx`、`shared ready`、bridge 正常出帧、
热键可以启动。排查过程中修掉 4 个真问题，其中两个属于"看起来在跑、其实什么都没发生"的静默失效。

### 修掉的问题

1. **NGX 入口不一定在驱动 shim 上。** RE Engine（Streamline）的 NGX 入口来自
   **游戏目录自带的 `nvngx_dlss.dll`**；旧判据只认 `_nvngx.dll` ⇒ 一个钩子都没装
   （ReShade 仍然报告 add-on 注册成功，但零数据）。现在判据 = 文件名含 `nvngx`
   ＋ **真的导出 `NVSDK_NGX_D3D12_EvaluateFeature`** ＋ 排除 `dlssg/dlssd/dlssnr`
   （CP2077 那些只读版本号的副本仍被第 3 条挡掉）。顺带：一个 provider 挂上就停，
   不再对转发到同一地址的 shim 重复挂载并打出 `NGX hooks FAILED`。
2. **采集暂停按帧算是慢加载游戏的灾难。** `init_swapchain` 之后 `+120 帧`，而 PRAGMATA
   加载期只有 **0.8 fps** ⇒ 实际暂停 **150 秒**，整个启动阶段不产出任何数据。
   现在改成「帧数或 **8 秒**，先到先算」。
3. **`LoadLibrary` 钩子拖慢 RE Engine 的加载**（用户 A/B：关掉 add-on 加载就正常）。
   新增 `[capture] LoaderHook=0` —— 完全不碰 `kernel32`，改为每 120 帧扫一次模块表。
   同时加了回调计时：实测我们的回调合计 **0.17 ms/帧**（barrier 0.02 ms/帧，约 400 次/帧）
   ⇒ "慢"确实来自钩子本身，不是回调里的活。
4. **游戏可能不从你改的目录读文件。** PRAGMATA 实际从
   `E:\PRAGMATA\_storage_\game_capture.addon64` 加载（进程级重定向），于是它找的 ini 是
   `_storage_\game_capture.ini` —— 我们改根目录那份**从来没被读过**，`Log` 一直是默认 0，
   所以"日志一行都没有"。
   - add-on：本地 ini 不存在时回退到 `%LOCALAPPDATA%\mgpu\game_capture.ini`；日志打不开时
     回退到 `%LOCALAPPDATA%\mgpu\game_capture-<pid>.log`；
   - 新增**面包屑** `%LOCALAPPDATA%\mgpu\breadcrumb.log`（每次 `AddonInit` 写一行：add-on
     的真实路径、ini 路径、`Log` 值、日志是否打开）—— 这次就是它一眼定位的；
   - 安装器新增 **6/6 镜像目录**：任何子目录里已有同名 add-on 或同名代理 DLL，就把
     add-on + ini 也放一份过去（`_storage_` 这类容器从此自动覆盖）。

### 实测（PRAGMATA）

```
config: ... SkipMv=0 DepthConvert=1 ... LoaderHook=0 Input=auto (path e:\pragmata\_storage_\game_capture.ini)
NGX provider: ...\_nvngx.dll   ->   NGX hooks active
dlss feature flags 0xB: MVJittered=0 MVLowRes=1 DepthInverted=1
input source: ngx (api 1) - pinned for this process
shared ready: name=Local\mgpu_mailbox_v1 pid=23056 used=253.6 MB reserved=253.6 MB slots=4
perf: frames=10680  begin=20ms  barrier=2.4ms (41k calls / 241k items)  present=1.4ms
```

排查全过程（含"GPU 0% 不是掉驱动"、"热键其实收到了"两次误判的证伪）见
`docs/findings/RE_ENGINE_NOTES.md` §3.1、§6.1–§6.4。
- 全部通过后：重建发布为 **v0.13.1**（0.13.0 的 add-on 不含以上修复）。

---

## v0.13.0 —— UE5「模型运动伪影」结案（2026-11-09）

**一句话**：伪影的主因是**上采样倍率**，不是运动矢量。游戏内 **DLAA（或最高画质档）+ `--interp 1 --fg-of`**
是本次唯一实测"干净"的组合；UE5 交给 NGX 的那条 MV 流是坏数据，不要用。
（完整证据链、排除表、dump 数字：`docs/findings/UE5_MV_OPTISCALER.md` §7–§8。）

**用户能感知的变化**

- **新配方**：`--interp 1 --fg-of` + 游戏内 DLAA（4K 下 26→52 fps，实测无模型边缘伪影）。
  DLSS 性能档（720p→4K，3 倍放大）**连不插帧的游戏本身**都有一点伪影，那部分不该算在插帧头上。
- **`--fg-of` 真的能用了**：以前 `motionVectorScale` 透传游戏的 `MV.Scale`（PBZ 报 1.0），
  库把 screen-UV 的 0.009 当成 0.009 像素 ⇒ 光流等价于没生效。现在用 MV 纹理尺寸
  （日志 `mvscale=(1278.0000, 720.0000)`）。**这条影响所有用 `--fg-of` 的配方（含 3x/4x 推荐档），
  `MV.Scale=1.0` 的游戏以前等于没开光流。**
- **add-on 开始读游戏声明的 DLSS flags** 并转发 `MVJittered` / `MVLowRes` / **`DepthInverted`（新增）**：
  日志 `dlss feature flags 0x4B: MVJittered=0 MVLowRes=1 DepthInverted=1`（需 `game_capture.ini` 的 `Log=1`）。
  PBZ 声明 `MVJittered=0`，所以"UE5 的 MV 带抖动、开 `JITTERED_MV` 就能修"这条**被证伪**。
- 新开关：`--fg-mv-scale raw|px|<float>`、`--fg-no-jittered-mv`；
  `--fg-jitter-zero` 重新生效（我们的颜色是上采样后的画面，jitter 已解掉，这是它的 A/B 对照）。
- `scripts/analyse_dump.py` 新增 **units fit**（拟合 `motion_px = mv_raw × k` 判 MV 单位与配对）；
  新配方脚本 `scripts/recipes/run_ue5_mv_ab.bat`（`auto/jit/nojit/depthinv/jitdepth/mvscale/nomv/of/ofsign/offov/jz/nodepth/only/dump`）。

**实测（PBZ，2x @ 60 锁帧，`cadence` 精确）**

| 档 | 结果 |
|---|---|
| 游戏 MV（`auto`）/ `jit` / `depthinv` | 伪影 + **闪烁** |
| `--no-mv` | 不闪，仍有边角伪影 |
| `--fg-of`（scale 修好前 / 后） | 与 `--no-mv` 相同 / 仍相同 ⇒ 伪影与 MV 无关 |
| **DLAA + `--fg-of`** | **干净** |

dump 出来的游戏 MV：0.5% NaN/inf、17% 饱和到 fp16 上限（±65504）、x 中位 0 / y 中位 +31、
与真实位移相关性 ≈0 —— 不是可用的速度场（与 2026-11-07 对 PBZ 的旧结论一致）。

---

## v0.12.2 —— 文档：UE5 备注（2026-11-09）

- README 新增「游戏引擎备注」：**UE5 不建议用运动矢量**（加 `--no-mv`）+ **必须开「分层窗口」**
  （`--overlay-layered`，UI 默认已开），并把原因写清楚（UE5 的 MV 在抖动后的画面上算、单位/方向
  不统一；UE5 自认为"未激活"时会丢菜单点击）。同样的备注也进了发布包里的 `README.md`
  （= `docs/ops/USAGE.md` §6）。

---

## v0.12.1 —— add-on 安装器改为强制覆盖（2026-11-09）

**修的是"装好了但跑不起来"这类问题**：旧 add-on / 旧 `game_capture.ini` 残留会让协议对不上、
或让新参数（`StreamHeight=1440`）看着设了其实没生效。

- 安装器**把所有文件强制覆盖**成本包版本（不再"存在就跳过"）；覆盖前改名备份
  （`x.mgpu_bak` → `.mgpu_bak2` … 最多 9 份，绝不覆盖更早那份原件）；
- 每个文件写完立刻 SHA256 校验，不一致直接失败并说明原因（被占用 / 杀软拦截 / 没权限）；
- 装完打印结果表（新建 / 覆盖 / 已是最新 + 备份名）并扫描残留（只提示不删）；
- 新增 `-KeepReShadeIni`：保留用户自己在 ReShade 面板里调过的 `ReShade.ini`，只补两个键；
- 重复运行安全（内容一致 → "已是最新"，不产生新备份）。

顺带修掉一个真 bug：安装新建文件（例如 shader）时 `Split-Path -Leaf` 拿到空备份名会抛
"Path 为空字符串"，导致安装在第 4 步中断。

---

## v0.12.0 —— 动态逐帧可用、MV 不再 drop（2026-11-09）

> 本版包含 0.11.3 – 0.11.12 的全部改动（0.11.2 之后的工作）。

**新功能**

- **动态逐帧模式**（UI 的"模式"分成 固定 / 动态逐帧 两块，互斥）：
  每帧按实际经过的 vblank 数决定倍率（`--interp-cadence`），输出恒定 1 帧/vblank，
  输入 35–45 fps 也能把 120 Hz 填满。配套：
  - `--cadence-smooth N`（默认 8）：先对"到达间隔"做 N 帧 EMA 再算倍率。
    逐帧原始间隔会让倍率跟着抖 1:1（切换 29 次/秒），实测 EMA 后切换 12 次/秒、
    空刷新 3.9% → **1.3%**。
  - `--fg-out-hz N`：输出帧率上限（`0` = 面板刷新）。把"120 Hz 面板只要 60 fps"
    变成 2x 而不是 4x；最低 2x，够不到时会打 warning 说明实际跑到多少。
- **`[capture] StreamHeight`（add-on，默认 1440）**：跨卡流（MV+深度）的高度上限，
  宽度按渲染宽高比算。4K 渲染由 63.3 → **28.2 MB/帧**；`0` = 跟随渲染尺寸（旧行为）。
- UI 的"输出帧率"改成手输数字框；"倍数"在动态模式下作为容量上限。

**修复**

- **开 MV 就 drop**：根源不是插帧算法，而是渲染卡每帧为 MV+深度扛的 63 MB 跨卡流量
  和两个全屏转换 pass（渲染卡被游戏吃满，转换抢不到执行窗口）。`StreamHeight=1440`
  之后**实测不再 drop** ⇒ 动态逐帧也能正常用游戏 MV。
- 到达间隔统计曾用 `GetTickCount64`（1 ms 粒度，0 ms 样本被丢），中位数报 31 ms
  而不是 25 ms，倍率被吸附到 4 ⇒ 输出只有 96.8 fps。改用 WGC 的 `SystemRelativeTime`
  后 3x@40 = **120.0 fps / 1.00 vblank**。
- `show=` 曾按 `loop_fps × framesPresented` 计算（120 Hz 面板上读出 148/164），
  改成 `presents / 窗口时长` ⇒ ≤ 122。
- bridge 的 `Present`/fence 等待不再每次新建事件（`wait_fence_value`），后缓冲缓存、
  只在 generation 变化时重读流几何；日志 flush 限流 200 ms（`--log-flush`）。

**诊断**

- `breakdown` 里新增 `colour=` / `cpu=`；新增 `bus:`（每帧跨卡字节与 GB/s）、
  `arr(vb):`（到达间隔的 vblank 分布）、`out:` / `out2:`（present 与 vblank 计数）、
  `game(vb):`、`bursts:`、`rt spikes:`、`rt auto:`。
- `bus:` 超过 48 MB/帧或 `inputs+copy` 占循环 ≥25% 时，额外打 `headroom:` 建议；
  流尺寸 > 1440p 时提示 add-on 的 `StreamHeight=1440` 能省多少。
- `--overlay-monitor N` / `--overlay-hidden`：覆盖窗口放到指定显示器、
  或只跑交换链不上屏（用来隔离"同屏覆盖窗口抢 independent flip"的问题）。

**已知取舍（都实测过，别再试）**

| 尝试 | 结论 |
|---|---|
| `--cadence-hold N`（保持倍率 N 帧） | 三组都更差：空刷新 6.6–10.4%、`slow` 10–13 帧/窗口。用丢帧换稳定，不值 |
| `--fg-cad-rt 1`（frameRenderTime = 批次×vblank） | 理论上更自洽，实测更差：`Present` 按预算阻塞，而输入间隔在下一帧到达时才量 ⇒ xN 从 3.00 漂到 3.38 |
| 动态逐帧 + `--fg-of`（光流） | 同卡多一个全屏 pass，drop 明显增加；30 秒段看不出来，长玩露馅 |
| `--no-depth` | disocclusion 会渗色（`HANDOFF` §8-28） |
| 固定 3x @44–45 fps 输入 | 45×3 = 135 > 120，必然超额，`slow` 23–32/窗口 |

**UI**

- 模式分成"固定 / 动态逐帧"两块（互斥，模式外的行灰显且点不动）；
  移除 `--dyn-mfg` 实验档。
- 动态逐帧下"运动矢量"行会显示权衡：`游戏 MV（动态逐帧可能闪烁）` /
  `AMD 光流（动态逐帧更易 drop）`。
- 修 Win+Tab 后覆盖窗口丢失：创建即带 `WS_EX_TOOLWINDOW`、DWMWA peek 排除、
  `ITaskbarList::DeleteTab`；`fg_is_shell_ui()` 把任务视图/Alt+Tab/Win+D 排除在焦点丢失之外。
  （不要用 `GWLP_HWNDPARENT` 设 owner —— 会让游戏鼠标输入失效。）

## v0.10.x 及更早

见 `git log`、`docs/findings/HANDOFF.md`（里程碑历史）与
`docs/findings/ARCHIVED_EXPERIMENTS.md`（已移除的实验与结论）。
