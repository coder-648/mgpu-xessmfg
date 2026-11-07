# 交接：XeSS MFG 解锁 + 4K/VSR 链路调优（2026-11-06 → 11-07）

> **文档已重组（2026-11-08）**：本文件现在位于 `docs/findings/`，作为
> **历史档案**保留。当前结论见 [`CONCLUSIONS.md`](CONCLUSIONS.md)，
> 画质变量见 [`QUALITY_VARIABLES.md`](QUALITY_VARIABLES.md)。
> 本文 §8.6 之后的部分写于"MV 是死输入"被推翻之前，
> **以 `CONCLUSIONS.md` §2 为准**（XeFG 确实消费 MV；3x 用光流、2x 用游戏 MV）。

> 上一份交接文档是 `docs/findings/FINAL_CONFIG.md`。本文只写**这一轮新增/改变的东西**和**下一步从哪继续**。
> 新对话开工时读：本文 §8.6.1（MV/深度结论）→ `docs/findings/FINAL_CONFIG.md`（§2.1.2/§2.1.3 定稿配方 / §3 配置表 / §4 开关 / §6 日志）。
>
> **2026-11-07 整理轮已做**：脚本收敛（38 个实验脚本归档，2026-11-08 已移除）、`--fg-of` 全家用
> `MGPU_FG_OF` 宏关闭、协议 v5 死轮询删除、`third_party` 1.4 GB → 181 MB（入库 5 MB）、
> `git init` + 首次提交、文档拆成"配方 + 档案"。详见 §9。

## 0. 一句话现状

MFG 已在官方 DLL 上解锁（`--unlock-mfg`，实测 `maxSupportedInterpolations` 1→3），
4K 面板 + VSR 5K 抓取 + 4x 帧生成跑到 **`show` 中位 118、`click→photons ≈73 ms`、HITCH 1~2 次/百窗口**；
**没能做到"绝对稳 120"**，原因定位到限帧器（RTSS ±8.3 ms 量化），不在我们链路里。

**2026-11-06 收工时的最终选择（重要）**：日常链路改为
**桌面 4K + 游戏 4K + DLSS *Quality*（内部 1440p）+ RTSS 30 + `--interp 3`（4x）**，
即 `run_mgpu_4x_4k_native.bat`，实测 `gpu 20.3 / total 32.8 / skip 0.1 / show 120 / click→photons 74.2`。

- 不选 DLAA 的理由：① 兼容性（很多游戏，尤其虚幻 5 的，只给 Quality/DLAA 少数档，Quality 最通用）；
  ② DLAA 把内部渲染拉到 2160p，会给**渲染卡**加 ~2.25 倍像素压力，而它没有余量；
  ③ DLAA 下 MV/深度变 2160p，跨卡搬运 66 MB/帧、`gpu` +3.6 ms，4x@120 就装不进 33.3 ms 了（§8-25/27）。
- add-on 的 MV/深度流尺寸**自动跟随 NGX 上报的渲染分辨率**（Quality=1440p、Balanced≈1250p、
  Performance=1080p、DLAA=2160p 各自适配），换档位不用改配置，但**必须重启游戏**（§8-24）。
- 如果哪天想再试 DLAA：`[capture] StreamPercent=150` 让 add-on 先把 MV/深度渲染成 1440p 再传（§8-28）。

### 0.1 UE5 游戏（PBZ）的定稿配方（2026-11-06 实测通过）

**症状**：无论如何调 MV 刻度/方向、深度、分辨率，移动物体与屏幕边缘一直有伪影，还伴闪烁。

**最后定稿（画面干净、性能正常）**：

```
add-on (game_capture.ini)          bridge 命令行
  Color=0                            --game PBZ.exe --den 1 --pool 4 --no-mv
  StreamPercent=100                  --interp 3 --unlock-mfg --fg-ui none
  StreamAlign=1     ← 精确尺寸，绝不裁切   --fg-proj-rz --fg-fov 58.7 --fg-jitter-zero
  DepthConvert=1    ← 同帧真深度
  MvNormalize=0 / MvFlip=0
```
即 `run_mgpu_4x_4k_pbz_depthonly.bat`（**2026-11-07 定稿**；这一节下面 2026-11-06 的
原配方是 `run_mgpu_4x_4k_pbz.bat`，已归档（脚本本体已移除）。`--mv-newest` 已被删除）。

四条关键结论（每条都踩过坑）：

1. **MV 反而不要**：`--no-mv`（零 MV）+ 真深度 + 正确投影最干净；给真 MV 时 XeFG 成本 +10 ms 且伪影更多。
2. **投影的 FOV 决定边缘**：`--fg-fov 58.7`（= 90° 水平 @16:9，UE5 默认）时干净；55/62 都会重新出现人物边缘伪影。
   投影误差是"中心 0、越往边缘越大"，所以**只剩边缘伪影就等于 FOV 不对**。
3. **深度必须"同帧"**：ReShade tap 晚一帧 → 明暗闪烁；直接给游戏 depth 建视图 → 崩（其实是 64 位跨卡流的问题）；
   正解是**在游戏那块 typeless 深度上直接建 SRV**（UE5 给的是 `R32G8X24_TYPELESS`，格式 19），同帧、零拷贝。
4. **不要裁切**：`StreamAlign=8` 会把 2562×1441 裁成 2560×1440，0.08% 的尺度偏差让场和画面对不齐；
   `StreamAlign=1` 精确尺寸才对（MV 用普通 `CopyResource`，深度用 pass 整幅缩放到 2562×1441）。
5. **真 MV 会让画面闪烁 + 边缘变糊**（在"无裁切 + 真深度 + 正确 FOV"下复测，结论不变）——
   UE5 这套 MV 在时间上不自洽，XeFG 的遮挡判定会逐帧翻转，所以 `--no-mv` 是唯一可用解。
6. **剩余限制：快速移动时画面边框的伪影**（速度越快越明显）。原因是边框像素的复用来自画外，
   而零 MV 时 XeFG 无法区分"物体出画"和"镜头平移"。可选缓解：`--fg-scene 0.8`（让 XeFG 在
   剧烈变化时放弃插值、改为重复上一帧，用一点平滑度换掉边框糊）。

## 1. 代码状态（本轮改动的文件）

| 文件 | 行数 | 改动 |
|---|---|---|
| `src/xefg_dyn.h` | 190 | **新**。运行时 `LoadLibrary` 加载 `libxess_fg.dll` + 全函数指针表；`--fg-lib` 路径覆盖（`LOAD_WITH_ALTERED_SEARCH_PATH`）；缺导出逐项报错。不再静态链接 `libxess_fg.lib` |
| `src/xefg_unlock.h` | 203 | **新**。MFG 解锁的 6 处字节补丁（4×`libxess_fg` + `libxell` 计数门 + 2x 模型谓词），出处 Universal MFG Unlocker (MIT)；全有或全无、逐点回读、失败回滚 |
| `src/mgpu_shared.h` | 90 | **协议 v3 → v4**，`kMaxSlots` 2 → 4（`--slack` 需要；顺带把共享块降到 112.6 MB） |
| `src/game_capture/game_capture.cpp` | 1241 | `Color=0` 时**不再分配颜色流**（4 槽会白占 224 MB）；`g_capture_colour` 上移到 `ensure_shared` 之前；**MV/深度流尺寸改为跟随 NGX 上报的渲染尺寸**（原来写死 2560×1440，DLAA 下会全部拒绝拷贝），新增 `[capture] RenderWidth/Height` 覆盖与换档位告警（§8-24） |
| `src/bridge/bridge.cpp` | 4965 | 新开关：`--interp N`、`--fg-lib`、`--fg-probe`、`--unlock-mfg`、`--fg-rt 0`、`--fg-ui none`、`--fg-scene F`、`--fg-depth-inv`、`--slack N`、**`--down N`（缩放核：0 双线性 / 1 Catmull-Rom / 2 Mitchell / 3 Lanczos-3）**、**`--no-depth`**、**`--game <exe>`**；指标 `x2/x2miss` → **`xN/xNmiss`**；**`LATENCY` 两行**；`non-paced` 告警；`overlay_size()` 按**显示器**定窗口尺寸（去掉末尾双重缩放）；`%ls` 中文路径截断修复；删掉重复的 `--xell` 判断。**2026-11-07 整理轮**：`--fg-of` 全家进 `#if MGPU_FG_OF`（默认 0）、删除 `wait_slot_v5`/`slot_ready`/`g_ready_ever`/`g_last_used_id`/`--mv-newest`/`--fg-hi-res-mv`/`--fg-jittered-mv`（427 KB → 137 KB） |
| `src/bridge/build.bat` | — | 移除 `libxess_fg.lib`（保留 `libxell.lib`） |

## 2. 部署状态（**add-on 与 bridge 必须同一次构建**，协议 v6）

> **2026-11-07 第二轮（厂商兼容 M6.1/M6.2）**：`bridge.exe` 不再要求副卡是 AMD
> （`--gpu <spec>` / `--producer-luid`，见 `FINAL_CONFIG.md` §4.9），
> 共享内存协议 **v5 → v6**（生产端 LUID / 输入 API / flags / 相机 / 游戏帧间隔）。
> 两边必须一起重编；只换一边会打
> `shared block version 5, this bridge speaks 6 - rebuild/redeploy BOTH sides` 并退出。
> 计划与验收记录：`docs/design/VENDOR_COMPAT_PLAN.md`。

```
<TOOLS_DIR>\bridge.exe                       md5 7deec9a2…  ← 当前（2026-11-07 整理轮：
                                                                     --fg-of 宏关闭、v5 死轮询删除、
                                                                     --mv-newest/-hi-res-mv/-jittered-mv 删除）
                                                                     ※ md5 每次重编都会变（PE 头里有时间戳），
                                                                       要以 `git log` + 源码为准
<TOOLS_DIR>\archive\exe_prev\  (13 份历史 exe)
    bridge.exe.pre_cleanup  6633340943…  ← 与上面同一个（整理前的最后一版，427 KB 含光流代码）
    bridge.exe.prev12       d9ba117b…    (dump 异步，未含 --fg-of)
    bridge.exe.prev  等     …            (见 §8.6 各轮)

E:\…\Cyberpunk 2077\bin\x64\game_capture.addon64  md5 5fe0e5990b8d9f0c5ce14c58ae85a52a  ← v4
E:\…\Cyberpunk 2077\bin\x64\game_capture.addon64.bak  (旧 v3)
F:\…\PBZ\platform\steam\game_capture.addon64        md5 21cb96bd1544a45f35521a9d99f03d0c  ← 当前（SkipMv=1 默认）
```

> 2026-11-07 之后 bridge 与 add-on **都不再需要**协议 v5 的 `ready` 发布；
> add-on 那边的 `ready = 0` 写入保留（只是没人读），所以**旧 add-on 配新 bridge 仍然兼容**，
> 不用为了这轮清理重启游戏重装 add-on。

- `game_capture.ini` 必须是 `[capture] Color=0`（日志会打 `config: Color=0`）。
- 换 add-on **必须重启游戏**；版本不符时 bridge 会拒绝通信（有意保护）。

## 3. 可用配置与实测数据（全部为本机实测，RX 6750 GRE）

| 脚本 | 锁帧 | 插值→上屏 | `gpu` p50 | `total`/预算 | `show` | `skip` | `click→photons` |
|---|---|---|---|---|---|---|---|
| `run_mgpu_4x_4k.bat` ⭐**4K 日常** | 30 | 3840×2160 → 5K 桌面 | 21.2–23.4 | 29.6–31.3 / 33.3 | **118**（109–123） | 0.7–0.9 | **73.1–77.4** ⁽¹⁾ |
| `run_mgpu_4x_c.bat` ⭐1440p | 30 | 2560×1440 | 16.3 | 31.4 / 33.3 | 120 | 0.3 | 73.2 |
| `run_mgpu_4x.bat`（A/B 基线） | 30 | 2560×1440 | 16.3 | 31.4 / 33.3 | 120 | 0.3 | 73.2 |
| `run_mgpu_5k.bat`（原生 5K 2x） | 30 | 5120×2880 | 4–20 | 16–33 / 33.3 | 58–61 | 0–2 | ~78 |
| `run_mgpu.bat`（1440p 2x） | 40 | 2560×1440 | 3.7–5.3 | 7.5–17 / 25 | 76–80 | 0 | ~41 |
| `run_mgpu_3x_40.bat` ⏳**未测** | 40 | 2560×1440 | ≈14（推算） | ≈20 / 25 | ~120? | ? | ≈55? |
| `run_mgpu_4x_4k_native.bat` ⭐**新最优**（桌面 4K + **DLSS Quality**，内部 1440p，零缩放） | 30 | 3840×2160 | **20.3**（max 24.3） | 32.8 / 33.3 | **120**（114–123） | **0.1**（3/76 窗口） | **74.2**（58.9–126） |
| 同上 + **DLAA**（内部 2160p，MV/深度也是 2160p） | 30 | 3840×2160 | 27.1（max 30.5） | 34.4 / 33.3 | **110**（96–119） | 3.0（max 9） | ≈100（`game=34–40ms`） |

⁽¹⁾ 4K 日常那行是**较早会话**的数，原始日志已被 12:11 的 24fps 会话覆盖，见 §3.1。

### 3.1 数据出处与复核（2026-11-06 收工后用 `scripts/logstats.py` / `skipcause.py` 重算）

表里的数字来自**各自会话**的日志，不是同一个 `bridge_*.txt` 的多次采样；每份日志重算结果如下
（`show` 为窗口整数中位、`skip` 为窗口内丢帧数中位）：

| 日志（`<TOOLS_DIR>\`） | 窗口数 | `gpu` | `total` | `skip` | `show` | 与表中一致？ |
|---|---|---|---|---|---|---|
| `bridge_4x_c.txt` | 54 | 16.3 | 31.4 | 0（mean 0.3） | 120 | ✅ |
| `bridge_4x_4k_072.txt` | 29 | 21.5 | 32.2 | 1.0 | 118 | ✅（≈ 4K 基线 21.2–23.4） |
| `bridge_4x_4k_pace.txt` | 18 | 22.5 | 32.6 | 5.5 | 118 | ✅ |
| `bridge_5k_mfg.txt` | 13 | 40.4 | 47.2 | 17 | 83 | ✅（39–44 / 19–26 / ≈80） |
| `bridge_3x_4k.txt` | 14 | 10.9 | 16.2 | 0（mean 0.5） | **91** | ❌ 这次输入只有 **30 fps**（`game=32–34 ms`），`show 91 ≈ 30×3`，**不是** §5 里"3x@4K@40"那次 |
| `bridge_3x_4k_slack.txt` | 40 | 23.6 | 27.8 | 4.0 | 107 | ⚠️ 输入 30→34.7 fps（也不是 40）；"slack 抬 gpu 3.3 ms"✅，但"+37 ms 只换 0.7 帧"未复现 |
| **`bridge_2x_4k_native.txt`**（12:43，名字叫 2x 实际跑的是 4x，见 §8-23） | 76 | 20.3 | 32.8 | 0.1（3/76 窗口 >0） | **120** | ✅ 新最优，4K 原生长跑 |
| **`bridge_4x_4k.txt`** | **10** | **25.6** | **36.9** | **6.5** | **100** | ❌ 见下 |

**`bridge_4x_4k.txt` 已经被 12:11 的一次新会话覆盖**，那份日志里输入只有 **23.1–23.8 fps**（`game≈41 ms`、
`jitter=16.6..66.5ms`、`est. click→photons ≥ 96–104 ms`），所以它复算出来的 `show 100` 是
"限帧器把 30fps 抖动成 ~24fps"的下游结果，**不是**表里 4K 日常那行的反例。表里的 118 / 73 ms
只由 `_072`（同链路、scale 0.72）和更早的会话支撑，原始日志已不在盘上。

这次覆盖本身是一条结论：**同一份 `run_mgpu_4x_4k.bat`，不同会话的输入可能差 30→24 fps**，
下游 `gpu 21.5→25.6`、`total 32→37`、`show 118→100`。⇒ 每份日志都要记下 `game=` 和 `avg fps`，
否则数字不可比。**原因已查明：那次是 CPU 占用过高**（不是限帧器），见 §6-①。

## 4. 三个已标定的模型（比记数字更有用）

**① XeFG 成本模型（4K 基准）**

```
gpu ≈ 15.0 ms（固定：真实帧 + 我们的 5K→4K 下采样 pass + XeFG 基础处理 + 拷贝）
    + 2.8 ms × 插值帧数
4x@4K = 15.0 + 3×2.8 = 23.4   （实测 23.4）
3x@4K = 15.0 + 2×2.8 = 20.6   （实测 20.6）
5K 按像素 ×1.78（固定≈26.7、生成≈5.0 → 4x@5K≈41.7，实测 39–44）
```

推论：**4K 的 `gpu` 由固定成本主导** → `--scale 0.72`（−7% 像素）实测毫无收益（21.2→21.5）；
想省只能省整档（4K→1440p 才降到 16.3）。

**② 预算判据（实测标定）**

```
稳定：   gpu_p99 ≤ 输入间隔 − 3 ms
规划：   gpu_p50 + 8 ms ≤ 输入间隔        （8 ms ≈ 实测 p99−p50 抖动）
```
三组数据全部吻合（24.5≤30.3 ✅ / 27.7≤30.3 ✅ / 31.8>22.0 ❌ 对应 skip 0.3/0.7/5.3）。
**别用 `total` 判断**：它会饱和到输入间隔（`present` 用剩余时间对齐 vblank）。

**③ 延迟结构**

```
compose→present ≈ game->wgc(DWM hop) + 输入间隔     43.0 ≈ 11.8 + 33.3
click→photons  ≈ 1 个输入帧 + 上式 + 0..一个刷新周期(8.3ms)
```
推论：**只要装得下，延迟与插值分辨率无关**（1440p 40.5 vs 4K 43.0，差的只是 `age`）。
降延迟只有三条路：提高输入帧率（−8.3/档）、降倍数（−8.3 在 120Hz）、降 DWM hop（不可控）。

## 5. 本轮已排除的实验（脚本已改名为 `_FAILED_`）

| 实验 | 结果 |
|---|---|
| 5K + 4x（`_FAILED_run_mgpu_5k…` / `run_mgpu_5k_mfg.bat`） | `gpu 39–44` 超预算 → 19fps×4 ≈ 80fps、`skip 19–26` |
| 3x@4K@40 | `total 25.4 > 25.0` → `skip 5.3`、`show 106` |
| 3x@4K@40 + `--slack 1` | +37 ms 延迟只换 0.7 帧；**slack 自身把 gpu 抬了 3 ms**（扣住 WGC 缓冲） |
| 4K + `--pace 30` | 漏拍 9 次 HITCH（`gap≈50ms`）、`skip 5.1` |
| 4K + `--scale 0.72` | `gpu` 没降、`show` 最低 103 |
| `--fg-rt 0`（`run_mgpu_4x_b.bat`） | `present` 尾部变长、HITCH 5/79（被 `_c` 取代） |
| `--no-throttle` / `--buffers 3` | 更差 / 无差别（沿用旧结论） |
| FLM 测延迟 | **会改被测系统**（present 12.4→2.6 ms、4 帧成串、体感变抖）→ 只用日志 `LATENCY` 行 |
| `--present-dy N`（只把\"采样到的颜色\"上下平移 N 像素，想补 PBZ 底部一条竖带） | **已从代码里删除**：颜色被挪、depth/MV 没挪 ⇒ 模型拿到的引导与实际画面错位，边缘出现硬伪影。要动就必须**颜色+depth+MV 一起挪**，否则别动 |
| `--capture-monitor`（抓显示器而不是游戏窗口） | 把**我们自己的 overlay 也抓进去**（反馈回路），实测不可用；flag 保留仅为实验 |

> **复核提醒（§3.1）**：`_FAILED_run_mgpu_3x_4k*.bat` 的原始日志已被 30fps 会话覆盖，
> 而且当初判的是 `total 25.4 > 25.0`（§4-② 说 `total` 不能用来判装不装得下）⇒
> "3x@4K@40 装不下"这条**证据最弱**。1440p 版（§6-②）实测之前，别拿它当反例。

## 6. 遗留任务（按优先级）

**① 让输出真正稳 120：唯一杠杆在限帧器（不改代码，优先级最高）**

现状：`show` 109–123、中位 118。根因是**输入到达被量化到 120Hz 网格**：

```
jitter（每窗口内到达间隔 min..max）: 16.7..50.0ms / 16.7..41.7ms / 25.0..50.0ms
30fps 本该是干净的 33.3（= 4 个 vblank 槽）
```

RTSS 限帧精度 ±1 个面板周期（±8.3 ms），实际到达是 25/41.7/50 ms ⇒ 2/5/6 槽；
XeFG 每批要吐 4 帧，2 槽批次装不下 → 输出摆动。

**新增证据（12:11 的会话）**：同一份 `run_mgpu_4x_4k.bat`，那次输入只跑到 **23.1–23.8 fps**
（`game=41 ms`、`jitter=16.6..66.5ms`），于是 `gpu 25.6`、`total 36.9`、`skip 6.5`、`show 100`、
`est. click→photons ≥96–104ms` —— 即**输入一旦掉档，§3 里 4K 那整行数字都复现不了**。
**根因已确认：那次是 CPU 占用过高**（不是 RTSS；用户 2026-11-06 确认），不是限帧器量化。
所以①的第一判据是 **`game=` 必须落在 33.0–34.0 ms**：`game≈41ms` 说明**要么**限帧器在吐 24fps
**要么** 机器被别的负载抢了 CPU —— **先看任务管理器，再怀疑限帧器**。

按顺序试（每次 1 分钟）：RTSS limiter mode 改 **front/back edge sync** → 或改用**游戏内建限帧** →
或锁 **29.97** → 或开 RTSS 的 vblank 同步选项。
**判据**：`grep -o 'jitter=[0-9.]*\.[0-9.]*ms'` 出现 `33.3..33.3ms` 占多数 ⇒ 成了，`show` 会自己变 120。

**② 实测 1440p 3x@40（`run_mgpu_3x_40.bat`）/ 4K 原生 3x@40（`run_mgpu_3x_4k_native.bat`）**

原生 4K 链路把固定成本从 ~15 ms 打到 **11.9 ms**（4x 实测 20.3 = 11.9 + 3×2.8），
所以 3x@40 从"装不下"变成"边缘可试"：`gpu ≈ 11.9 + 2×2.8 = 17.5`，预算 25 ms，
判据 `gpu_p50 + 8 ≤ 25` 刚好压线。**这是唯一能把 `click→photons` 从 74 ms 降到 ~55 ms 的路**，
优先级高于 1440p 版本（画质不掉）。判据：`xN=3.00`、`game≈25ms`、`skip≈0`、`show≈120`、延迟 p50 < 60 ms。

**③ 收尾两个仍是旧名的失败脚本**（等你确认）

`run_mgpu_4x_b.bat`（`--fg-rt 0`，HITCH 5/79）、`run_mgpu_5k_mfg.bat`（5K 4x = 80fps）→ 加 `_FAILED_`。

**④ 15 ms 固定成本能不能再拆**

4K 的 `gpu` 里固定成本占 15 ms（我们的 5K→4K + HDR10 转换、XeFG 基础处理、拷贝）。
想再要余量就得动这里：可以加一个只做"转换 + 不做"的对照实验（例如临时跳过我们的 pass 看 `gpu` 掉多少），
确认那 15 ms 里有多少是我们的。

**⑤ 硬件延迟校准（可选）**

日志里的 `click→photons` 是**下限**（不含游戏输入采样）。有 LDAT 或 240fps 手机慢动作时，
实测一次并记录差值，以后只看日志即可。**Reflex Analyzer 对本方案无效**（它跟游戏自己的交换链）。

**⑥ 代码清理（低风险）**

`--xell`/`--no-xell` 是死开关（`g_no_xell` 只被日志引用）；`--half` 与 `--den 2` 重复；
`non-paced` 告警仍可能在静止画面误报一次（可再收紧或加 `--no-warn`）。

**⑦ 换到原生 4K 链路（VSR 5K↓4K 有性能问题，2026-11-06 改方案）**

新方案：**桌面 3840×2160 @120 + 游戏 4K + DLSS Quality（内部 1440p）+ VSR 关 + RTSS 30 + 4x**。
这样 WGC 抓 3840×2160、我们不做任何缩放、上屏 1:1，链路里**一次 resample 都没有**：

```
WGC 3840x2160 → [pass：只 HDR10，1:1] → XeFG @ 3840x2160 → 上屏 1:1
```

脚本：`run_mgpu_4x_4k_native.bat`（`--den 1 --pool 4 --mv-newest --interp 3 --unlock-mfg --fg-ui none`）。
相对 5K VSR 链路省掉：5K 读（118 MB→63 MB/帧 FP16）、整个缩放 pass、`--down` 的抽头。

实测时按这个顺序看：
1. 启动日志必须出现 `capture item 3840x2160`、`capture size taken from the window: 3840x2160
   (shared block declared 5120x2880)`、`resize filter: not needed ... is 1:1`、
   `overlay fits the monitor 3840x2160`、`FG swap chain: 3840x2160` —— 全对才算链路干净；
2. **`display mode` 必须是 @120 Hz**（59/60 就永远只有 ~58fps，见 §8-19）；
3. `game=` 33.0–34.0 ms（先排除 CPU 占用；4K+DLSS Quality 不该是 68 ms，那是 13fps）；
4. `gpu` p50 应**低于** 5K 链路的 23.4 ms。若基本没降，说明那 15 ms 固定成本主要在 XeFG 基础处理里，
   不在这条 pass 上 ⇒ 只能走 §6-④；
5. `skip` 目标 ≤0.9、`show` ≥118。

**2026-11-06 12:32 第一次跑（`bridge_4x_4k_native.txt`）暴露的两个问题，都已处理：**
- `capture pool / colour sink / FG swap chain` 全是 **5120×2880**，而窗口是 3840×2160（add-on 的
  颜色流尺寸是写死的占位值）→ 已改成以 WGC item 尺寸为准，见 §8-18；
- `display mode: 3840x2160 @ 59 Hz` → 4x 上限 ~58fps，属于显示链路（DSC/线材/刷新率设置），不是代码；
  同一份日志里 `game=68ms`（游戏自己只有 13.4 fps），所以那次 `gpu 44.5`、`skip 45`、`timeouts=200`
  都是"5K 输出 + 13fps 输入"的连锁反应，不是新链路的表现。

> `--down` 在这条 1:1 链路上自动跳过（`resize filter: not needed`），`run_mgpu_4x_4k_hq.bat`
> 的价值只在仍然走 `--scale 0.75` 的时候。

**2026-11-06 12:37 第二次跑（尺寸修好后）**：`capture item / pool / colour sink / FG swap chain` 全部 **3840×2160**，
5K 拉伸消失（§8-18 生效）；但面板是 **59 Hz**，于是 `xN=4.00` 把输入钉在 ~15 fps
（`game=66ms`、`present=28.5ms`、`gpu=35.8ms`、`load=98%`、`skip=40`、`show=59`、点击延迟 156ms）。
结论：**59/60 Hz 面板上 4x 没有意义**，改用 `run_mgpu_2x_4k_native.bat`（`--interp 1`，30fps→60fps）；
要有 4x 就必须先把 4K@120 搞定（DP1.4+DSC / HDMI2.1）。同时修掉了两条自检误报（§8-22）。

## 7. 工具（都在 `scripts/`）

| 工具 | 用途 |
|---|---|
| `probe_fg.bat` | DLL/能力自检；`probe_fg.bat --unlock-mfg --interp 3` 可验证解锁 |
| `logstats.py` | 一份日志 → p50/mean/min/max（gpu/present/total/load/skip/show） |
| `latstats.py` | 专算 `LATENCY` 行的分布（compose→photons / click→photons） |
| `skipcause.py` | 丢帧归因：到达成串 vs 真超预算 |
| `logdeep.py` | 超预算窗口计数 + 逐条列出 `skip>0` 的窗口 |
| `check_sites.py` / `pe.py` | 换 DLL 版本时逐点比对解锁字节 |
| `deploy_addon.sh` | 把 `build/game_capture.addon64` 拷进游戏目录（留一份 `.bak`）；**游戏开着会 I/O 失败**，必须先关游戏 |
| `deploy_game.sh <game-dir> [exe]` | **给第二款游戏**装全套：ReShade(`dxgi.dll` 取自 CP2077) + add-on + ini；之后用 `--game <exe>` 启动 bridge |
| `temp/check_down.py` | **CPU 侧等效实现**缩放核：算权重和、Nyquist 残留、阶跃过冲；改 `--down` 前后先在这里看数值 |
| `temp/extract_hlsl.py` + `temp/fxc_check.sh` | 把 `k_conv_hlsl` 抽出来用 `fxc.exe` 编 `ps_5_0`/`vs_5_0`（**D3DCompile 是运行期编译，写 shader 必须先在本地过一遍 fxc**） |
| `rcmd.sh` | 规避 Cline 命令挂起（**命令必须单行**，见 `docs/findings/TOOLING_NOTES.md`） |

## 8. 新增的坑（接续 `FINAL_CONFIG` §5 的十条）

11. **外部延迟工具会改被测系统**：FLM 挂钩后 `present` 12.4→2.6 ms、4 帧成串、体感变抖。
    bridge 现在会自检并警告 `presentation is not vblank-paced`。
12. **RTSS ±8.3 ms 量化**：30fps 内容落在 120Hz 网格上变成 2/5/6 槽，这是"稳 120"的唯一障碍。
13. **4K 的 `gpu` 由固定成本主导**：降 7% 像素毫无收益（21.2→21.5），要省只能降整档。
14. **持有 WGC 抓取帧有成本**：`--slack 1` 反而把 `gpu` 抬高 3 ms（缓冲被 pin 住）。
15. **`total` 会饱和到输入间隔**：判"装不装得下"要看 `gpu`/`skip`，别用 `total` 或"显卡利用率"。
16. **日志会被下一次运行覆盖**：`run_*.bat` 全是 `> bridge_xxx.txt`，同名只有一份；本轮 4K 日常的
    原始日志就是这样被 12:11 的会话盖掉的，只能靠 `_072` 反推。关键会话跑完先另存
    （`copy bridge_4x_4k.txt bridge_4x_4k_<mmdd>_<game fps>.txt`）。
17. **0.75 倍的"单点双线性"等于点采样**：5K→4K 时一个输出像素只压了 1.33×1.33 的输入，
    Nyquist 残留 67%（棋盘格实测 0.167–0.833），超采样的细节全变成摩尔纹；`--sharpen`
    只会把它一起拉硬。`--down 1`（Catmull-Rom，7 抽头/轴）降到 41%、`--down 3`（Lanczos-3，9 抽头）31%，
    且这个 pass 每个**真实帧**只跑一次，`gpu` 代价预计 <0.5 ms。注意 `--den 2` 是精确 2:1，
    双线性本来就是盒式平均，开不开差别很小。
18. **抓取尺寸不能信共享块**：add-on 里 `streams[0]`（颜色）写死 **5120×2880**，`Color=0` 时
    连资源都不分配 —— 它只是占位。以前游戏跑 5K，占位值恰好等于真实尺寸，所以没暴露；
    换成 4K 桌面后整条链路（colour sink / WGC 池 / FG 交换链）全按 5K 建，画面被 DXGI 拉回
    4K 窗口 = "缩放不对"。**现在一律以 WGC item 尺寸为准**（`capture size taken from the window`），
    并要求 `present size == window client`（不一致会打 WARNING）。教训：**任何"尺寸"都必须
    来自真正持有它的对象**，占位常量迟早会对不上。
19. **4K HDR 很容易掉到 59/60 Hz**：那条链路（含 DSC）一旦协商失败，`display mode` 就是
    `3840x2160 @ 59 Hz`，4x@30 就永远只能是 ~58fps。bridge 现在会直接告警。
20. **4x 在 60Hz 面板上不是"4 倍帧"，是"输入砍到 1/4"**：XeFG 每个输入帧要吐 N+1 帧，
    输入速率被钉在 `refresh/(N+1)`。59Hz + 4x ⇒ 输入 ~14.8fps（实测 15.0）、`present` 阻塞 28–32ms、
    `xN=4.00`、`show=59`、延迟 156ms。**60Hz 面板该用 2x**（`run_mgpu_2x_4k_native.bat`）。
21. **`game=` 会被自己的瓶颈污染**：它是"我们两次拿帧的间隔"，`max(游戏帧间隔, 我们的消费间隔)`。
    2026-11-06 12:37 那份日志里 `game=66ms` 看起来像"游戏只有 13fps"，实际是 59Hz + 4x 把输入钉住了。
    判别：`present` 占 `total` 40% 以上 + `load≈98%` ⇒ 先怀疑呈现端/我们，别急着怪游戏（`FINAL_CONFIG` §6.2.1）。
22. **首帧自检别放 init**：`present size != window client` 的检查最初写在 `init_fg` 里，而窗口是被
    `borderless()` **之后**才改成全屏尺寸的，于是每次都误报 3840x2135。挪到第一帧再查（`present == client`）。
23. **`--interp 1` 被 `--unlock-mfg` 的默认值吃掉**：`want = (g_fg_interp > 1) ? N : 3`，
    所以 `--interp 1 --unlock-mfg` 实际跑的是 **4x**（12:43 那份 `bridge_2x_4k_native.txt` 就是 4x 的数据）。
    现在用 `g_fg_interp_set` 区分"没给"和"显式给了 1"，`--interp 1` 就是 2x，且 `want<2` 时直接跳过解锁
    （`xefg_unlock::Apply` 只接受 2..5）。**教训：默认值和显式值要用两个变量分开存。**
24. **add-on 里 MV/深度流尺寸被写死 2560×1440**：它恰好等于「5K + DLSS Performance」和
    「4K + DLSS Quality」的内部渲染尺寸，所以一直没暴露。换 **DLAA**（4K 内部就是 2160p）后
    `copy_compatible()` 全部拒绝 → `[mv] skipped` → 桥一套 MV/深度都没收到，插值必然烂。
    现在 add-on 用 **NGX 上报的渲染尺寸**建流（`shared: mv/depth at 3840x2160 (NGX render size)`），
    并支持 `[capture] RenderWidth/Height` 覆盖；中途换档位会打 WARNING 让你重启游戏。
    **换档位不需要重编，但必须重启游戏**（共享块每个进程只建一次）；bridge **不用改**
    （它从 WGC 取尺寸、从 NGX 的 `mv_scale` 取映射）。bridge 现在也会打
    `MV 3840x2160, depth 3840x2160 (render resolution)`，只看 bridge 日志就能判断档位。
25. **DLAA 不是"只吃游戏卡"**（12:56 实测，4K DLAA + 4x）：MV/深度从 1440p 变 2160p 之后，
    `gpu`（6750 GRE 的真实单帧）**20.3 → 27.1 ms**，`wait` 0 → 1.9 ms（生产者每帧要搬 66 MB
    而不是 29.5 MB），同时游戏自己从 30fps 掉到 **26–29fps**（`game=34–40ms`）。
    两头一起超预算 ⇒ `skip` 0 → 3、`show` 120 → 110。**XeFG 的 MV/深度预处理是按这两个流的
    分辨率算的**，所以提高内部渲染分辨率会把副卡一起吃掉。
26. **"用 2K MV + DLAA 也还行"是一个假象**：那条路径上每帧 `copy_compatible()` 都失败、
    拷贝被静默跳过，XeFG 拿到的是**旧的/未初始化**的 MV —— 等于没有运动矢量。它更便宜
    （`gpu` 还是 1440p 的钱），但那不是"能用的配置"，只是"看起来凑合"。**任何"跳过拷贝"
    都不该被当成可用方案。**
27. **DLAA 的代价 100% 在跨卡搬运，不在插值**（2026-11-06 用 `--no-inputs` 做的对照实验，
    `--no-inputs` 喂的哑资源同样是 2160p，所以 XeFG 的预处理成本两次完全一样）：

    | 4K / 4x / 30 锁 | MV/深度 | 每帧搬运 | `gpu` p50 / p90 / p99 |
    |---|---|---|---|
    | DLSS Quality + 真输入 | 1440p | 29.5 MB | 20.3 / — / ~24.3 |
    | **DLAA + 真输入** | **2160p** | **66 MB** | **25.2 / 28.3 / 29.5** |
    | DLAA + `--no-inputs`（哑） | 哑 2160p | 0 | **21.6 / 24.4 / 25.7** |

    结论：**XeFG 根本不在乎 MV/深度分辨率**（21.6 ≈ 20.3），多出来的 3.6 ms 全是
    "把 66 MB 从 3080 Ti 搬到 6750 GRE"。所以要让 DLAA 也能进 33.3 ms 的预算，
    唯一有效的杠杆是**减少搬运量**（add-on 侧降采样 MV/深度再写共享内存，或 `--no-depth` 砍一半），
    而不是调插值参数。
28. **`--no-depth` 证明了深度不能省**：数字很好看（`gpu` 22.4、`show` 119、延迟 72ms），但物体与背景
    交接处出现**拖影/渗色**——深度是 XeFG 做 disocclusion 判定的依据，砍掉就露馅。
    所以正解是 **`[capture] StreamPercent=150`**：add-on 先把 MV/深度用一个小 pass 渲染成
    1440p（`src/mgpu_ds_shaders.h` 里是 fxc 编好的 DXBC，不依赖 d3dcompiler），再拷进共享块。
    传输 66 → 29.5 MB，深度数据仍在，XeFG 看到的就是一份 1440p 的 MV/深度。
    add-on 日志：`config: ... StreamPercent=150` + `downsample: MV/depth rendered at 2560x1440 ...`；
    bridge 日志：`MV 2560x1440, depth 2560x1440 (stream size)`。
29. **同名多进程会抓错窗口**：`Phantom Blade Zero`（PBZ）运行时有两个 `PBZ.exe` ——
    启动器（~34 MB，**没有窗口**）+ 游戏本体（~11 GB，窗口标题 `Phantom Blade Zero`）。
    `find_game_window()` 原来只取快照里**第一个**同名进程，抓到启动器 → 日志
    `game window not found - is PBZ.exe running?`，而游戏明明开着。
    现在枚举**所有**同名 PID、取客户区最大的窗口；精确同名找不到时还会做**子串匹配**
    （UE5 常见 `XXX-Win64-Shipping.exe`）。日志会打印每个 pid 有几个可见窗口，以及最终
    `using window <hwnd> of pid <pid> (client WxH)`。
30. **ReShade 会反复加载/卸载 add-on**：UE5 启动时连建多个探测设备，每见一个新设备就
    unload/reload 一次 add-on（PBZ 实测 5 次）。每次约 2 ms，总共 ~10 ms，**不是**启动慢的原因；
    但旧的 `open_log()` 用 `"w"` 会把日志清空，于是前几次探测的记录全丢。
    现在改成追加，并打印 `(game uptime N ms)` —— 一眼看出 add-on 是在启动第几毫秒被加载的。
31. **没有启用任何 ReShade 技术 → add-on 永远不跑**：add-on 的每帧逻辑挂在
    `reshade_begin_effects` 上，而 ReShade 在"零个启用效果"时不触发该事件。症状：
    游戏正常、`ReShade.log` 显示 add-on 注册成功，但 bridge 一直停在
    `waiting for 'Local\mgpu_mailbox_v1'`（现在等 10 s 会打一行提示）。CP2077 一直没暴露这个
    问题，是因为部署时顺手装了 `mgpu_depth_tap.fx` 并在 `ReShadePreset.ini` 里启用了它。
    `deploy_game.sh` 现在会给新游戏做同样的事（fx + preset + `EffectSearchPaths`）。
32. **ReShade 退出时会重写 `ReShade.ini`**：手改的 `EffectSearchPaths` 会在游戏退出后被打回
    默认值 `.\`，所以要么**在游戏关闭时**改，要么让特效落在 `.\` 能搜到的位置。
33. **深度和 MV 尺寸不一样，且 XeFG 要求两者必须同尺寸**（PBZ 实测）：UE5 交给 NGX 的
    `render`（=MV 尺寸）是 **2562×1441**，而深度资源是 **2564×1444**（差 2–3 px 的 padding）。
    两次踩坑：
    - 用同一个尺寸建两条流 → `CopyResource` 尺寸不符 → `[depth] skipped` → `ok=false` →
      add-on 在 stage 2 之后**再也不 publish**（`stats: copied` 卡住）；
    - 反过来按深度自己的尺寸建流 → 两条流尺寸不同 → **XeFG 每帧 `ERR_INVALID_ARGUMENT`**，
      `xN=1.00`、`xNmiss=40`，等于完全没有插值（`show` = 游戏自己的帧率）。

    正解：**两条流都按 MV 尺寸建**，深度用 `CopyTextureRegion` 裁掉多出来的 2–3 px
    （`record_copy_region()`，只丢最外圈几条像素，肉眼不可见）。这样 XeFG 看到的
    MV/深度尺寸始终一致。bridge 端分别用 `streams[1]`/`streams[2]` 的尺寸 tag（此时两者相等）。
34. **跨适配器 fence 读不到值**：`GetCompletedValue()` 在消费端返回 **UINT64_MAX**（PBZ 的
    `fwd=[18446744073709551615 ...]` 就是这个）。老代码把这个值存进 `g_fwd_last`，下一帧就去等
    `UINT64_MAX + 1 == 0` —— 永远等不到，于是**每帧 `wait≈20ms`、`timeouts` 每帧 +1**，
    bridge 一直用旧槽位的数据插值。现在不等 `GetCompletedValue()`，自己维护每槽的期望值
    （生产端按 1,2,3… signal，`SetEventOnCompletion` 对已越过的值会立刻触发）。
35. **黑线/条纹 = 拿错帧的 MV/深度**：上面 33+34 叠加的结果 —— MV 可能是几十毫秒前的，
    深度根本没有，XeFG 的运动补偿直接对不上，运动时出现细黑缝。修好 33/34 后应消失。
36. **在游戏进程里给 D32_S8X24 深度建 SRV + 画 pass = 设备崩**：PBZ 实测，`[depth] convert pass`
    一执行游戏就退出（`shared ready` 之后立刻没了）。深度 pass 因此**默认关闭**
    （`[capture] DepthConvert=0`），改由桥给 XeFG 一块**清过零**的深度（`--no-depth`）。
    代价：没有 disocclusion 判定 → **角色/物体互相遮挡处有拖影**（实测确认）。

    **最终方案（不碰游戏那块深度）**：让 `mgpu_depth_tap.fx` 替我们采样 —— ReShade 自己处理
    深度资源的状态和视图格式，把结果写进它的私有纹理 `MGPU_DepthOutTex`（R32F）。add-on 通过
    `find_texture_variable("mgpu_depth_tap.fx", "MGPU_DepthOutTex")` → `get_texture_binding()`
    → `device::get_resource_from_view()` 拿到那块纹理，下一帧用 `CopyTextureRegion` 拷进深度流。
    因为读的是**我们自己的特效输出**、且是"上一帧"的内容（状态已知 = RENDER_TARGET），
    不需要给游戏的 64 位深度建任何视图/猜任何状态 —— 这才是崩溃的根源。

    尺寸由 preset 的预处理定义指定，必须等于 MV 尺寸：
    `ReShadePreset.ini` → `PreprocessorDefinitions=MGPU_DEPTH_W=2562,MGPU_DEPTH_H=1441`（PBZ）。
    运行时开关仍是 `[capture] DepthConvert`（0 = 关，退回"平深度"）。
37. **深度失败绝不能阻断发布**：`ok` 曾经包含深度的结果，于是深度一失败就整帧不 signal，
    桥此后一直读旧槽位（`wait` 19.8ms、`timeouts` 每帧 +1、画面黑线）。现在 `ok` 只由 MV 决定，
    深度问题只记日志 —— 修好后实测 `copied` 持续增长、`skipped=0`、`wait=0`、`timeouts=0`。
38. **UE5 深度：技术可行但代价大于收益**（PBZ 最终结论）：ReShade 深度 tap 那条路是通的
    （`depth: using ReShade depth texture (2562x1441 fmt 41)`、每帧拷贝成功），但 ReShade 的
    深度 pass 在 `begin_effects` **之后**才跑，我们只能读到**上一帧**的深度；游戏 25–28fps 时
    这等于 36–40ms 的滞后 → XeFG 的 disocclusion 判定在帧间来回翻 → **模型明暗疯狂闪烁**。
    所以 UE5 这类游戏最终采用 **`--no-depth` + `DepthConvert=0`**：桥给一块清过零的平深度，
    运动矢量承担插值，代价只是"物体互相遮挡的边界轻微拖影"，换来稳定 113fps。
    要再进一步：把深度拷贝搬到 `reshade_present`（ReShade 各 pass 之后、同一个 command list
    还开着）做**同帧**拷贝，并验证 UE5 反向 Z 是否需要 `--fg-depth-inv`。
39. **MV 拷贝失败会伪装成"XeFG 不用 MV"**：加了 `StreamAlign=8` 把流尺寸对齐到 2560×1440 后，
    MV 源仍是 2562×1441，而我当时只给**深度**加了"尺寸不符就裁切"的分支、**忘了 MV** ⇒
    `[mv] skipped` → `ok=false` → 一帧都不发布（`stats: copied=0 skipped=10246`）。
    表象是"改 MV 的 scale/方向，画面一点变化都没有"——很容易误判成 XeFG 忽略 MV。
    现在 MV 也走 `record_copy_region()`（裁切 2×1 像素）。**判据：`stats:` 里 `copied` 必须增长。**
40. **帧常量：jitter 是真的，投影矩阵是我们猜的**（`xefgSwapChainTagFrameConstants`，bridge 一直有调）：
    `jitterOffset` 取自 producer 的 NGX `Jitter_Offset_*`（真值），`motionVectorScale` 固定 1.0，
    但 `projectionMatrix` 是 `approx_projection()` 造的**假矩阵（固定 60° FOV / zn 0.1 / zf 10000，
    非 reversed-Z）**，UE5 实际是 90° 左右 + 反向 Z。两个运行时开关用于排除这两项：
    `--fg-jitter-flip`（抖动取反）、`--fg-no-proj`（改用单位矩阵）。
    bridge 启动会打一行：`[bridge] frame constants: jitter=(..) proj=.. mvscale=(1,1)`。
41. **换 add-on 必须重启游戏，而且要用 build 戳确认**：`game_capture.addon64` 只在游戏进程启动时
    加载；游戏开着的时候我们能把文件换掉，但**正在跑的进程用的还是旧 DLL** —— 本轮就因此白跑了一轮
    （日志里还是旧版话术）。现在 `AddonInit` 第二行固定打印 `addon build: <日期> <时间>`，
    **每轮先看这一行**，时间戳不对就是没重启游戏。
    另外：DXGI 枚举值容易记错（`19` 是 `R32G8X24_TYPELESS`，`20` 才是 `D32_FLOAT_S8X24_UINT`），
    所以 UE5 的深度**本来就是 typeless**，可以直接建 SRV，不需要拷贝。

## 8.5 方案 A：自己算光流（AMD FidelityFX）—— 里程碑 1/2 已完成（2026-11-06 深夜）

> **2026-11-07 现状**：这条路线**已被证伪并停用**——MV 是死输入（§8.6.1 结论 4），
> `--fg-of` 整段代码用 `MGPU_FG_OF` 宏关掉了（默认不编译）。
> 本节保留为"当时怎么把 FFX SDK 编出来的"记录；`third_party/FFX`（1.1 GB 完整 SDK）
> 已删除，只留 `third_party/ffx/{include,lib}`（见 `docs/ops/DEPENDENCIES.md`）。

目标：不再跨卡搬 33–66 MB/帧的 MV，而是在 **6750 GRE 本地用 AMD 光流从颜色算出 MV**，灌进现有的 MV 流。

**里程碑 1：把 SDK 编出来（离线，一次）**

```bash
./temp/ffx_sdk_build.sh          # stage -> configure -> build（Release，只编 OF）
```
- 把 `third_party/FFX/sdk` 拷到 `<STAGE_DIR>\ffx\sdk`（cl/cmake 不能用 UNC 路径），
  （`third_party/FFX` 已于 2026-11-07 删除；要重建 lib 先按 `temp/ffx_sdk_build.sh` 顶部说明重新 clone）
  跳过 `tools/ffx_shader_compiler`(113 MB) 与 `tools/media_delivery`（CMake 里没人引用）。
- cmake 用 **VS2022 BuildTools 自带的那份**（`...\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`），
  系统 PATH 里没有 cmake/ninja。
- 只编光流：`-DFFX_API_BACKEND=DX12_X64 -DFFX_FSR3=ON -DFFX_OF=ON -DFFX_FSR3UPSCALER=OFF -DFFX_FI=OFF`。
- 产出 `<STAGE_DIR>\ffx\sdk\bin\ffx_sdk\ffx_opticalflow_x64.lib`(108 KB) +
  `ffx_backend_dx12_x64.lib`(726 KB)；着色器由预编译工具 `sdk/tools/binary_store/FidelityFX_SC.exe`
  在编译期生成 `*_permutations.h`，**不用我们自己编 shader**。
- ⚠️ 仓库里原来那份 `third_party/fidelityfx/Kits`（3.4 MB 减量包）是**新版 FFI 架构**，
  `ffx_opticalflow.cpp` 会走 `ffx_provider.h`，而它依赖的 `ffxProviderExternal` **那份包里没有定义**
  （上游 `origin/main` 也没有）⇒ 编不过。**用 `third_party/FFX` 这份完整 SDK**。
  另外 `PrebuiltSignedDLL/amd_fidelityfx_dx12.dll` 只导出 `ffx{CreateContext,DestroyContext,Configure,Query,Dispatch}`
  五件套（ffx-api 统一接口），里面**没有 opticalflow provider**（只有 FG/FSR2/FSR3UP）⇒ 光流只能走静态库。

**里程碑 2：在 6750 GRE 上真跑一次并量出代价**（`temp/ffx_of_test.cpp` + `temp/ffx_of_test.bat`）

合成两帧（3840×2160，B = A 右移 8 px），`ffxOpticalflowContextCreate/Dispatch`，读回矢量场：

| 输入 | 矢量场 | 解出的均值 | 单次 dispatch |
|---|---|---|---|
| 3840×2160 | 480×270 `R16G16_SINT` | **-7.99 / +8.00 px**（y≈0） | **1.2–1.5 ms**（含 fence 等待，best 1.21） |
| 2560×1440 | 320×180 `R16G16_SINT` | -7.99 / +7.98 px | 0.9–1.5 ms（best 1.11） |

⇒ **结论：光流在副卡上 4K 只要 ~1.3 ms**，比跨卡搬 66 MB（DLAA，实测 +3.6 ms）和搬 29.5 MB（1440p，约 +1.8 ms）都便宜，
而且**颜色本来就在 bridge 进程里**（WGC 抓的），完全不需要跨卡传 MV。

**踩到的四个坑（每个都表现为"矢量场全零"）**：

1. **前 6 帧一定是零**：`IsSceneChanged()` 里写死 `if (FrameIndex() <= 5) return true;`，
   `ComputeOpticalFlowAdvanced()` 在场景切换分支里只存 `(0,0)`。测试只跑 6 帧 ⇒ 永远看不到非零。
   **集成时必须接受开机后 ~6 帧（200 ms）没有真 MV**。
2. **输出是 render/8**：`GetOpticalFlowTextureSize(res, 8)` ⇒ 4K 得 480×270、1440p 得 320×180。
   分辨率低但值**是整像素**（不是 1/64 定点）：8 px 的位移解出 8，转 screen-UV 就是 `8 / 3840`。
3. **SCD 输出必须清零**：`opticalFlowSCD` 是跨帧保存的直方图/历史位，给未初始化内存会让它以为每帧都切换场景。
   FFX 自己在 `firstExecution` 时会 clear，但我们自己建资源时也要先填 0。
4. **拷贝回读的坑（只影响调试，不影响集成）**：buffer 里的 copy footprint `RowPitch` 必须 256 对齐
   （480×4 = 1920 → 要 2048），否则 copy 被静默丢弃，看到的永远是 0；
   从 `NON_PIXEL_SHADER_RESOURCE` 直接 CopyTextureRegion 在 AMD 上会**设备移除**（0x887A0005），必须先转 `COPY_SOURCE`。

**下一步（里程碑 3，未做）**：在 `src/bridge/bridge.cpp` 里集成

1. 用 `ffxGetInterfaceDX12` + `ffxOpticalflowContextCreate` 建上下文（分辨率 = 捕获尺寸）。
2. 每帧在 present 前 dispatch：输入 = 我们已有的捕获颜色（`g_hist`/captured，RGBA16F/scRGB），
   输出 `R16G16_SINT` 480×270 + SCD。
3. 把 SINT 像素值 / renderSize 转成 UV 写进 `g_mv_local`（`R16G16_FLOAT`，尺寸改成 480×270），
   `rd.resourceSize` 用这个尺寸 —— 与现在 tag 游戏 MV 的路径一致。
4. 保留开关（默认仍走游戏 MV），A/B 用 `bridge_*.txt` 比 `gpu` 与边缘质量。
5. 期望：2048² 之前光流成本 ≈1.3 ms，能抵掉 1.8–3.6 ms 搬运；**并且 MV 与颜色同帧、时间上自洽**，
   正好治 UE5 MV 抖动导致的闪烁/糊边（§0.1 的 `--no-mv` 配方可以退休）。

## 8.6 MV / 深度 / 颜色 的同步问题（2026-11-07 凌晨）—— 结论与实验档案

> **这一节分两半**：`8.6.1` 是**现状结论**（看一眼就够，日常只需要这部分）；
> `8.6.2` 起是**实验档案**（十轮对照实验的原始数据与推演，留作追溯，不要当操作手册读）。

### 8.6.1 结论（2026-11-07 定稿）

| # | 结论 | 依据 |
|---|---|---|
| 1 | **跨卡 fence 在消费端读不到值**（`GetCompletedValue()` 返回 `UINT64_MAX`），所以"等槽完成"从来没生效过，MV/深度相对颜色可能落后 0–4 帧 | `bridge_4x_4k_pbz.txt`：`fwd=[18446744073709551615 ×4]`、104 次报告 slot 全是 3、`wait=0.0ms` |
| 2 | **协议 v5（producer 发布 `ready`）没法用**：发布它必须在 producer 侧轮询 `SHARED_CROSS_ADAPTER` fence，每帧 4 次就把游戏渲染线程拖垮（`game` 33→60 ms、我们自己的循环 30→15 fps、帧数折半、闪烁）⇒ **轮询已删除，v5 的死代码已清掉**，回到轮转选槽 | 第七轮回归修复前后的日志对比 |
| 3 | **PBZ 的 MV 流本身是垃圾**：3% NaN、成片 ±40000、y 恒定 −54，与光流真值相关性 ≈ 0 | `temp/inspect_dump.py` / `temp/compare_mv_of.py` 对 dump 的分析 |
| 4 | **MV 是死输入**（最重要）：`MV=0` / `MV=100000 px` / 游戏真 MV（CP2077 上 corr 0.57，已证真实）三者画面**完全一致**，加 `HIGH_RES_MV` 也无变化 ⇒ `libxess_fg 1.3.1` 的模型**自己估运动**，不读 tag 进来的 MV | 第五/六轮的决定性对照实验 |
| 5 | **深度是唯一被消费的输入**，且：越新越好（延迟 1 帧更差）、分辨率不敏感（480×270 更差；2562×1441 与 3840×2160 无差别）、方向不能反（`--fg-depth-inv` 更差）、不能省（`--no-depth` 更差） | 深度专项五组对照 |
| 6 | 附带收益：**不发 MV** 省一个全屏 pass + 每帧 14.7 MB 跨卡写入，`gpu` 从 ~20–22 降到 **10.5–21.5**；于是 **3x@40 变得可行**（延迟更低、伪影更少） | 定稿配方实测 |

**由此定稿**（细节见 `FINAL_CONFIG.md` §2.1.2 / §2.1.3）：

```bat
rem PBZ 4x@30（120 fps）
bridge.exe --game PBZ.exe --den 1 --pool 4 --interp 3 --unlock-mfg --fg-ui none ^
           --fg-proj-rz --fg-fov 58.7 --no-mv --fg-jitter-zero
rem PBZ 3x@40（~75 fps，伪影明显更少）
bridge.exe --game PBZ.exe --den 1 --pool 4 --interp 2 --unlock-mfg --fg-ui none ^
           --fg-proj-rz --fg-fov 58.7 --no-mv --fg-jitter-zero
```

**两个必须避免的坑**（都踩过、都已修）：

1. 不要轮询 `SHARED_CROSS_ADAPTER` fence 的 `GetCompletedValue()`（见结论 2）；
2. 不要为了"深度延迟一帧"去读**较旧的共享槽**：那是 producer 正在写的内存，两块卡抢同一片内存双方都半速。
   要延迟只能在**本地**做（`g_depth_ring` + `--depth-lag`，实测 lag1 更差，默认 0）。

**代码现状**（2026-11-07 清理）：

- `--mv-newest`、`wait_slot_v5()`、`slot_ready()`、`g_ready_ever`、`g_last_used_id` **已删除**；
- `FrameShared.ready` 字段**保留**（add-on 与 bridge 的共享内存布局必须一致，版本号不动），只是没人读；
- `--fg-of` 全家（含 `--fg-of-units/sign/scale/guide/deadzone/depth-k/const/selftest`）
  用 **`MGPU_FG_OF` 宏隔离，默认 0 = 不编译**。`--fg-of` 现在只会打印一行
  `ignored: not compiled in`。要复验请 `/DMGPU_FG_OF=1` 重编（`third_party/ffx` 的头文件与 lib 都还在）；
- `--fg-hi-res-mv` / `--fg-jittered-mv` **已删除**（MV 是死输入，两个 init flag 对画面无影响）。

### 8.6.2 实验档案：最初的发现（证据）

问题：喂给 XeFG 的 颜色 / 深度 / 运动矢量，是不是同一帧的？

**结论：不是，而且差多少完全不受控。** 证据全部来自现有日志（`bridge_4x_4k_pbz.txt`、`bridge_4x_pbz_1440.txt`）：

```
[bridge] diag: frame_published=3793  fwd=[18446744073709551615 ×4]   ← 消费端读不到跨卡 fence 值
frame 1   slot 3 ...   frame 41  slot 3 ...   （104 次报告，slot 全是 3）
wait=0.0ms（106/106）   timeouts=0（104/104）
```

机制：

1. `--mv-newest` 的实现是"扫 4 个槽，取 `GetCompletedValue()` 最大的"（`bridge.cpp` 选槽那段）。
   跨适配器 fence 在消费端（6750 GRE）**永远返回 `UINT64_MAX`**，`>=` 比较让循环**总是选最后一个槽 = slot 3**。
2. `wait_slot()` 用 `SetEventOnCompletion(期望值)`，而 fence 的可见值是 `UINT64_MAX ≥ 1` ⇒ **立刻返回**，`wait=0`、`timeouts=0` 由此而来。
3. ⇒ **bridge 对 MV/深度完全没有同步**，只是读"slot 3 现在的内容"。producer 是 4 槽轮转、每 4 个游戏帧写一次 slot 3
   ⇒ MV/深度相对颜色的偏差是 **0～4 帧（30fps 下 0～133 ms，平均 ~2 帧）**，且可能读到 producer 正在写的半成品（撕裂）。
   颜色那一侧是**真的**同步的（`g_fence11`/`g_fence12` 同适配器，`copy=0.0~0.5ms` 是真实等待；WGC 帧 `age=2.5~23ms` ≈ 0～1 帧）。

为什么之前没发现：镜头慢时 2 帧前的 MV ≈ 当前 MV，视觉上看不出来；只有运动物体边缘/遮挡处才露。
**这也意味着过去所有"真 MV 反而糊边""深度调不出干净结果"的结论都要重审**——很可能治的是症状。
（另外 `on_reshade_present` 里 `ctl->frames[slot]` 的字段是在 `Signal` **之后**才写的，注释说是先写，属同类竞态，一并待修。）

### 定量验证工具（已实现，等你跑一次）

bridge 新增 `--dump N` / `--dump-after N` / `--dump-dir <dir>`：把**真正 tag 给 XeFG 的那几帧**原样导出
（`--dump-after` 默认 60，跳过刚启动时的加载画面；dump 脚本里设成 300 ≈ 起跑 10 秒后开始抓）

```
<STAGE_DIR>\dump\f00_colour.raw   RGBA16F（WGC 抓到的颜色）
<STAGE_DIR>\dump\f00_depth.raw    R32_FLOAT（我们 tag 的深度）
<STAGE_DIR>\dump\f00_mv.raw       R16G16_FLOAT（我们 tag 的 MV）
<STAGE_DIR>\dump\f00_meta.txt     slot / producer_frame_id / jitter / render / mv_scale / flags
```
每个 `.raw` 头是 4×uint32 `{w, h, dxgiFormat, rowPitch}`（行已去 padding）。dump 会等 GPU，**这 5 帧的 gpu/show 数字无意义**。

分析（在 WSL 里，用 miniconda 的 python，系统 python 没 numpy）：

```bash
./scripts/recipes/run_dump.bat          # 先在 Windows 侧跑这个（PBZ 4K + 真 MV + --dump 5）
./scripts/analyse_dump.sh <DUMP_DIR>
```

脚本做四件事：
1. 用块平均 luma 的 SSD 搜索求**相邻帧之间的真实位移**（全分辨率像素）；
2. 把 dump 出来的 MV 转成像素，**分别和 `(i-1→i)` 与 `(i→i+1)` 比**，谁更近就说明 MV 属于哪一帧；
3. 同样的方法给出**深度图自身的位移**（顺便验证深度值是不是真的在动）；
4. 打印 MV 统计（零向量比例）、深度值域（判断 reverse-Z 还是标准 Z）。

> **跑之前必须注意**：画面里要有**速度会变**的运动（走路/转头、忽快忽慢）。
> 匀速运动时"差一帧"和"差零帧"预测完全相同，脚本会打印 `inconclusive`。
>
> 脚本自身已用合成数据验证过：`temp/make_fake_dump.py <dir> correct|ahead` 生成"同帧/MV 领先一帧"两组假数据，
> 分析结果分别输出 `SAME FRAME (correct)` 和 `MV IS ONE FRAME AHEAD (mismatch)` ✓

### 修法（协议 v5，下一步）

1. `FrameShared._pad` → `uint64_t ready`；add-on 在 `on_begin_effects` 里**轮询自己的 fence**（producer 侧读值是正常的），
   把"该槽 GPU 已写完"发布到共享内存；顺带把 `frames[slot]` 的写入挪到 `Signal` 之前。
2. bridge 不再挑"fence 值最大"，而是挑**已完成槽里 `producer_frame_id` 最新的那个**，用共享内存自旋代替 fence 等待。
3. 加 `--mv-lag K` 做对齐 A/B（预期 0 或 1）。
4. 之后单独校**投影矩阵/near-far**：深度值是真的，但 XeFG 用深度要靠投影矩阵重投影，而那个矩阵是我们按 FOV 58.7 猜的。

### 第一次真机 dump 的结果 —— 又挖出一个更严重的 bug（2026-11-07 00:0x）

5 帧 4K dump 拿到后（`<STAGE_DIR>\dump\`，bridge 起跑 10 秒、PBZ 4K + 真 MV），`temp/inspect_dump.py` 的结果：

| 流 | 观察到的内容 | 判断 |
|---|---|---|
| **colour** | RGBA16F 3840×2160，相邻帧 70% 像素变化 >0.02，无 NaN | ✅ 正常 |
| **depth** | R32_TYPELESS 2562×1441，min 0 / median 0.012 / max 0.042，无 NaN，块匹配能得到 **12–48 px 的平滑位移** | ✅ 正常（且是**真的在动**） |
| **MV** | R16G16_FLOAT 2562×1441，**3% NaN**，x 有 47% 落在 ±1 但另有成片 ±40000 的离群值，**y 几乎恒定 −54（从不接近 0）** | ❌ **垃圾数据** |

**根因**：depth 一直走的是 **shader pass**（`ds_run`：SRV 采样 + 画到本地目标再拷），而 MV 走的是**直接 `CopyResource`**
（把游戏那张 swizzled 的 MV 贴图拷进行主序的跨卡槽）。`CopyResource` 要求源/目标 desc 完全一致，
UE5 那张 MV 资源只要有一点不同（array size / mip / layout），D3D12 就**静默失败**，槽里留下的是**未初始化堆内存** ——
这正好解释了"3% NaN + 成片 ±40000 + y 恒定负值"。

**影响面：** UE5 上所有"真 MV 更糊/更闪"的结论都要作废重测（§0.1 的 `--no-mv` 配方很可能是被这个 bug 逼出来的）；
CP2077 那条一直表现正常，说明那边的 MV 资源能被 CopyResource 接受（尺寸也正好 2560×1440）。

**已修（add-on `f0195f71…`）：** `on_begin_effects` 里 MV 也优先走 pass（`g_ds_on || g_mv_pass || g_ds_ready`），
失败才退回 `CopyResource`/`CopyRegion`。add-on 日志会出现 `[mv] convert pass 2562x1441 -> ...`（不再有 `[mv] CopyResource`）。

**同时修了 dump 的间距问题（bridge `d9ba117b…`）：** 第一版 dump 在帧循环里等 GPU，
导致相邻两帧相隔 **5–6 个游戏帧**（`producer_frame_id` 1354→1358→1363→1369→1375），
这种间距没法判断"差一帧"。现在改成：**拷贝录进当帧的命令队列**（readback ring），5 帧全部录完后再统一写盘 ——
相邻帧恢复成 1 个游戏帧，±1 帧的判据才成立。

> 复跑步骤：重启游戏（add-on 换过）→ `run_mgpu_pbz_dump.bat`（画面里要有**变速**的运动）→
> `./scripts/analyse_dump.sh <DUMP_DIR>`。
> 判据：MV 的 `mean|mv|` 应该是**个位数到几十像素**、零向量占多数、没有 NaN；verdict 出 `SAME FRAME` 就说明配对正确。

### 第二次 dump（MV 改走 pass 之后）—— 结论：MV 流里根本没有运动信息

add-on 日志已确认走的是 pass（`[mv] convert pass 2562x1441 -> 2562x1441, tracked state = 0xC0`，
不再有 `[mv] CopyResource`），帧间距也从 5–6 帧缩到 **+1/+2 帧**（`producer_frame_id` 1155→1157→1158→1160→1161），
但 MV 的数据依旧不可用，而且这次做了**定量对照**：

1. `temp/inspect_dump.py`：MV 仍然 **3% NaN**、19% 像素 |x|>1000、y 中位数 −56 且**随行号线性变化**（斜率 −0.02/行）；
2. 用 **FidelityFX 光流**（`temp/ffx_of_test.cpp` 新增 `dump` 模式，直接吃 dump 出来的两帧真实画面）算出真值：
   `mean=(3.06, −20.61) px`、`p99 |x| = 172 px`；
3. `temp/compare_mv_of.py` 把 MV（块中值，多种缩放 k = 1 / 3840 / 1/64 / 1/256）与这条真值相关：
   **corr = −0.005 … −0.04，全部为零**；
4. 结构相关（|mv| vs |flow|）同样是 **0.000 ± 0.014**；而 MV 自己帧间相关还有 0.45–0.58
   （说明它在变，但变的东西跟画面无关）。

⇒ **结论：UE5 交给 NGX 的那张 "MotionVectors" 资源里不是运动矢量**（或我们读到的不是它）。
无论怎么缩放都对不上真值，所以"真 MV 更糊/更闪"的旧结论全部作废；`--no-mv` 是当时唯一正确选择。
深度那一路反而是**干净且真的在动**（行中值 0.0017→0.0248，块匹配得到平滑位移，无 NaN）。

⇒ **方案确定：MV 换成我们自己的 FFX 光流**（4K 只需 1.3–1.5 ms，实测见 §8.5），
   游戏 MV 这条流直接不信；深度保留（顺便按 §8.6 的 v5 修槽位同步，解决 0–4 帧的陈旧问题）。

> 本次运行用户反馈"几次掉帧 + 画面闪烁"：掉帧来自 dump 每帧多拷 132 MB + MV 改走 pass；
> 闪烁则是**用垃圾 MV 插值**的必然结果（`--no-mv` 时不闪）。

### 里程碑 3：bridge 集成 FFX 光流（已实现，2026-11-07）

新增 `--fg-of`（bridge `a19d644c…`）：

- 用我们**已有的捕获颜色**（`g_shared12`，scRGB FP16，4K）在 6750 GRE 上算光流，
  输出 `480×270 R16G16_SINT`（渲染/8，单位=像素），再用一个 compute pass（`k_of_hlsl`，
  8×8 线程组）乘 `1/captureSize` 变成 **screen-UV** 的 `R16G16_FLOAT`，直接 tag 给 XeFG；
- **游戏那条 MV 流不再拷贝**（`copy_inputs_local` 里跳过），省掉 16–33 MB/帧跨卡流量；
  深度照旧（真实、干净）；
- 依赖：`third_party/ffx/lib/{ffx_opticalflow_x64,ffx_backend_dx12_x64}.lib`（已入库），
  头文件用 `third_party/ffx/include`，`src/bridge/build.bat` 已加好 include/lib 路径；
- 日志：`--fg-of: FidelityFX optical flow at 3840x2160, vector field 480x270`，
  breakdown 里多了 **`of=`** 一项（预期 1–2 ms）；
- **`--fg-of-selftest`**（需 `/DMGPU_FG_OF=1` 重编）：不需要开游戏，直接验证 FFX 建链（接口→上下文→3 次 dispatch→UV 转换 pass）：
  `<TOOLS_DIR>\bridge.exe --fg-of-selftest` 应打印 `3 dispatches + convert OK` / `OK (480x270 field)`。
  调试期间的两次翻车都栽在这里，所以这个自检保留；
- 踩过的三个坑（前两个已修，第三个是第二轮踩到的）：
  1. `of_init_once()` 结尾漏了 `g_of_ready = true;` ⇒ `of_record()` 每帧直接返回 false（表现为"没有 MV、xN=1.00"）；
  2. scratch buffer 用 `assign(n, 0)` 显式清零（`ffxGetInterfaceDX12` 会检查其 `refCount` 是否为 0）；
  3. **MV 必须和 depth 同尺寸**：FidelityFX 的场是 `ceil(size/8)`（480×270），直接 tag 上去 XeFG 每帧
     `ERR_INVALID_ARGUMENT`、`presented=1`、`xN=1.00`（日志里 `of=0.3ms` 说明光流本身是好的）。
     所以 UV 转换 pass 现在**顺手做双线性上采样**到 depth 流的尺寸（PBZ 是 2562×1441）再 tag。
     代价：转换 pass 从 480×270 变成 2562×1441 个线程，仍然 <0.3 ms。
- 已知：FFX 前 6 帧故意返回零向量（`FrameIndex() <= 5` 当成镜头切换），所以**开机 ~200 ms 内
  MV 是零**，之后才生效。

A/B 用两条命令（都需要游戏在跑、DLSS on、RTSS 30）：

```bash
<TOOLS_DIR>\run_mgpu_4x_4k_pbz.bat      # 基线：--no-mv（完全没有 MV）
<TOOLS_DIR>\run_mgpu_4x_4k_pbz_of.bat   # 新：--fg-of（本地光流 MV）
```

看三件事：`of=` 是否 1–2 ms、`gpu` 是否比基线低（少搬 16–33 MB）、
以及**运动物体边缘/近景是否不再糊或闪**（老配方 `--no-mv` 的问题是"硬/拖"，
垃圾 MV 的问题是"闪/糊边"，自己算的 MV 应该两者都没有）。

### 伪影第一轮（2026-11-07）：FG 通了，但画面比 `--no-mv` 更花

数据（`bridge_4x_4k_pbz_of.txt`）：`presented=4 / xN=4.00 / SUCCESS / show 109–120 / of=0.3–0.9ms`，
`gpu` 11.6–24.9 ms —— **光流这条路是通的**，每帧只花 0.3 ms，比搬 MV 便宜得多。

**能不能拿到"原生 2K MV"：不能。** FFX 光流的矢量场是 `输入/8`，而且 8 是**写死的**：
`ffx_opticalflow.cpp: uint32_t opticalFlowBlockSize = 8;`，shader 里更是 `iPxPos >> 3u`、`blockId * 8`。
`FFX_ASSERT(opticalFlowBlockSize >= 4)` 说明理论最小是 4（4K → 960×540），但要同时改 C++ 和两个 shader 并重编，
收益有限。所以路线是**把 1/8 的场"喂"得更聪明**，而不是追求原生分辨率。

**这一轮的三处改动（bridge `a490e900b921`）**：
1. 上采样从"双线性"改成 **颜色引导的加权上采样**：4 个 MV 相邻 texel 按"对应位置的颜色是否与本像素相近"加权，
   边缘处只吃同侧那一个 —— 双线性会把运动物体的矢量糊到背景上，正是额外鬼影的来源；
2. 新增 `--fg-of-guide K`（K=0 退回纯双线性，默认 48）；
3. 新增 `--fg-of-sign ±1` / `--fg-of-scale K`（MV 约定 A/B，避免每次改代码重编）。

三条对照脚本（都在 `<TOOLS_DIR>\`）：

```
run_mgpu_4x_4k_pbz_of.bat        颜色引导上采样 + sign +1（新默认）
run_mgpu_4x_4k_pbz_of_plain.bat  纯双线性（= 上一轮看到的花画面）
run_mgpu_4x_4k_pbz_of_flip.bat   颜色引导 + sign -1（验证方向约定）
```

**如果三条都还是花**：下一个最大的嫌疑不再是 MV 而是**深度陈旧**（§8.6 的槽位同步 bug：depth 可能落后 0–4 个游戏帧）。
有真 MV 之后，模型"相信"运动、再用 wrong-frame 深度做遮挡判断，错得比 `--no-mv` 更明显 —— 这也是为什么
`--no-mv` 看起来更干净。真 MV + 新鲜深度才是完整解，所以 v5 同步该做了。

### 伪影第二轮：把 depth 降到光流场的原生尺寸（用户提的思路）

矢量化光流没法给原生 2K（见上），那就**反过来**：把 depth 也降到 `480×270`，
让两条流都**原生**、**同尺寸**、且**零重采样**。add-on 本来就能做这件事，
`[capture] RenderWidth/Height` 直接决定流尺寸，`ds_run` 会把游戏深度缩放到这个尺寸：

```ini
[capture]
RenderWidth=480
RenderHeight=270
```

- 4K 下光流场 = `ceil(3840/8) × ceil(2160/8)` = **480×270**，和 ini 完全对上 ⇒ bridge 侧的上采样退化成 1:1；
- add-on 日志应为 `shared: mv at 480x270 (ini, format 34), depth source 2564x1444 -> stream R32 (480x270) via convert pass`；
- bridge 日志应为 `MV 480x270, depth 480x270 (stream size)` + `field 480x270 -> tagged at 480x270`；
- 附带收益：XeFG 的 MV/depth 预处理量降到 1/64（这部分本来按流分辨率算），`gpu` 应该更低；
- 风险：XeFG 可能在这么小的 MV/depth 上判 disocclusion 不准（或者干脆拒绝，报 `ERR_INVALID_ARGUMENT`）；
  若被拒绝，退一档用 `RenderWidth=960 RenderHeight=540`（4K/4，光流场只需 2× 上采样）。
- **注意**：ini 是全局的，`--no-mv` 基线现在也会用 480×270 的两条流，基线数字会变，别直接和旧日志比。

脚本：`<TOOLS_DIR>\run_mgpu_4x_4k_pbz_of_native.bat`（改 ini 后**必须重启游戏**）。
恢复 ini：`game_capture.ini.ofnative_bak`。

### 伪影第三轮：单位/语义（2026-11-07，从 XeSS-FG 头文件定位）

native 尺寸那版（480×270 两条流、零重采样）数据完全正常：

```
MV 480x270, depth 480x270 ; field 480x270 -> tagged at 480x270
presented=4 / xN=4.00 / SUCCESS / show 116~122 / of=0.5ms
gpu=14.4~17.9ms      ← 比 2562×1441 那版的 22ms 低（XeFG 的预处理量降到 1/64）
```

所以尺寸、配对、成本都不是问题。翻 `third_party/xess/inc/xess_fg/xefg_swapchain.h` 找到根因线索：

```c
XEFG_SWAPCHAIN_INIT_FLAG_USE_NDC_VELOCITY = 1 << 3,   // "Use velocity in NDC"
...
float motionVectorScaleX;  // 我们在 frame constants 里给的是 1.0
```

- 我们初始化传的是 `INIT_FLAG_NONE` ⇒ **默认不是 NDC**；
- 而我们一直喂的是 **screen-UV（0~1 量级）** 的 MV：
  - 若 XeFG 按**像素**解释 ⇒ 20 px 的运动被当成 0.005 px（≈没动，接近 `--no-mv`）；
  - 若按 **NDC** 解释 ⇒ UV 正好是 NDC 的一半（NDC 全幅 = 2）⇒ **运动只走一半** ⇒ 处处半程重影，正是"花"。

新增 `--fg-of-units px|uv|ndc`（**默认 px**；ndc 模式会自动把 Y 取反，因为 NDC 向上为正）：

```
run_mgpu_4x_4k_pbz_of_native.bat   ← px（默认，应最接近正确）
run_mgpu_4x_4k_pbz_of_ndc.bat      ← ndc（= 2×UV，Y 反向）
run_mgpu_4x_4k_pbz_of_uv.bat       ← uv（= 之前那版的行为）
```

判定：三条里哪条最干净，单位就定了；之后如果仍有余伪影，再叠加 `--fg-of-sign -1` 和 `--fg-of-guide`。
另外 `--fg-of-scale K` 仍可做连续微调（例如 px 模式下 K=0.5 试"像素但半幅"）。

### 伪影第四轮：深度方向（`INVERTED_DEPTH` 从来没测过）

实测：`--fg-of-units px` ≈ `uv`，而 `ndc` 明显更差 ⇒ **单位不是主因**（px 至少"近似正确"）。
翻 `docs/findings/HANDOFF.md` §8-38 那句一直没做的 TODO："验证 UE5 反向 Z 是否需要 `--fg-depth-inv`" ——
而 **19 个 bat 里 0 个用过它**：

```c
XEFG_SWAPCHAIN_INIT_FLAG_INVERTED_DEPTH = 1 << 0,
ip.initFlags = g_fg_depth_inv ? ..._INVERTED_DEPTH : ..._NONE;   // 实际一直走 NONE
```

我们喂的是 UE5 的**反向 Z**（近=1、远≈0，dump 实测 median 0.012 / max 0.08），却告诉 XeFG"这是普通深度"。
遮挡判定的两个方向因此是反的 ⇒ **与 MV 无关的一阶错误**，正好解释"px/uv 都花、ndc 更花"。

两条对照（都基于当前 native ini，无需重启游戏）：

```
run_mgpu_4x_4k_pbz_of_depthinv.bat   --fg-of --fg-depth-inv   ← 只改深度方向
run_mgpu_4x_4k_pbz_of_nodepth.bat    --fg-of --no-depth       ← 完全不用深度（判据）
```

- 若 `depthinv` 明显干净 ⇒ 找到根因，把它设为默认（PBZ/CP2077 都是反向 Z）；
- 若 `nodepth` 明显干净而 `depthinv` 不 ⇒ 深度这条路（1/8 缩放 + 0–4 帧陈旧 + 猜的投影矩阵）需要重做，先做 v5 同步；
- 若三条都花 ⇒ 才轮到"提高光流场精度"（blockSize 8→4，960×540）。

### 第五轮：先验证"我们的 MV 到底被不被消费"（决定性试验）

CP2077 的 dump 标定结果（`<STAGE_DIR>\dump_cp\`，用 FFX 光流当基准）：

```
mv_scale = 2560, 1440（游戏报渲染尺寸）
MV 与真实运动的相关系数 corr = +0.565 / +0.532   ← CP2077 的 MV 是真的！
量级上 MV_raw × 3840 ≈ 运动像素 ⇒ XeFG 认可的输入大致是 UV 量级
```

⇒ 我们在 PBZ 上给的 `uv` 版本**单位可能就是对的**，画面却依然花 ⇒ 说明问题不在数值，
而在"**我们 tag 的东西到底有没有被 XeFG 消费**"，或者被消费的方式（分辨率/预处理）与我们设想的不同。

因此新增 `--fg-of-const X Y`（px）：把整帧 MV 强行写成同一个向量，用来做**消费测试**：

```
run_mgpu_4x_4k_pbz_of_const_uv.bat   --fg-of-const 200 0 --fg-of-units uv   (写入 0.052)
run_mgpu_4x_4k_pbz_of_const_px.bat   --fg-of-const 200 0 --fg-of-units px   (写入 200)
run_mgpu_4x_4k_pbz_of_zero.bat       --fg-of-scale 0（MV 全 0，对照）
```

判据（画面整体应该出现**水平方向的半程重影**，位移约 100 px）：

| 现象 | 结论 |
|---|---|
| `const_uv` 有约 100 px 的横向重影、`const_px` 几乎无变化 | XeFG 按 **UV** 解释 ⇒ 单位用 `uv` |
| `const_px` 有约 100 px 横向重影、`const_uv` 几乎无变化 | 按**像素**解释 ⇒ 单位用 `px` |
| 两条都几乎无变化（和 `zero` 一样） | **我们的 MV 根本没被消费** ⇒ 问题在"资源是否被接受"，与数值无关 |
| 两条都是满屏横扫/画面整体错位一大块 | 数值被放大了 ⇒ 结合符号再定 |

### 第六轮（结论）：**MV 是死输入** —— 整个 MV 方向的工作都不影响画面

决定性对照（CP2077，同一个场景，同一 build）：

```
基线（游戏自己的真 MV，corr +0.57 已验证是真的）  ≈  ZERO（MV=0）  ≈  HUGE（MV=100000 px）
再叠加 --fg-hi-res-mv（init flag HIGH_RES_MV）  →  依然无变化
而 --no-depth 明显更差                                  →  深度是被消费的
```

⇒ **XeFG（libxess_fg 1.3.1 这个 build）不消费 tag 进来的 MV 资源**；库里那条
`"Missing resource of type XEFG_RES_MOTION_VECTOR."` 校验也从没被触发（说明资源被接受、只是没用上）。

**这意味着：**

1. 游戏 MV 的跨卡搬运、PBZ 垃圾 MV 的定位、FFX 光流集成、以及所有单位/符号/上采样 A/B —— **对画面零影响**
   （虽然"垃圾 MV"这个发现本身是真的，只是它本来就没进模型）；
2. 运动物体边缘的重影/拖影 **不是 MV 造成的**，是这个模型自己的运动估计质量；
3. `gpu` 的差异（22 → 14~18）全部来自 **depth 流的尺寸/搬运**，与 MV 无关；
4. 之前"我们光流 ≈ 基线"的结果也顺带解释了：因为都不影响输出。

**因此剩下的唯一杠杆是深度**，具体两条：

- **分辨率**：480×270 的粗深度是白降价（实验证明没有换来画质），已把 PBZ 的
  `game_capture.ini` 从 `RenderWidth=480/RenderHeight=270` 恢复成原样（深度回到 2562×1441）；
- **时效性**：`--mv-newest` 实际退化成"永远 slot 3"（跨卡 fence 在消费端返回 UINT64_MAX），
  producer 每 4 帧才刷新一次 slot 3 ⇒ 深度可能落后 **0–4 帧（0–133 ms）**。
  临时缓解：`run_mgpu_4x_4k_pbz_depthonly.bat`（去掉 `--mv-newest` → 轮转按序读槽，深度逐帧推进，且不发 MV）；
  根治：协议 v5（见下）。

**协议 v5（下一步实现）**：`FrameShared._pad` → `uint64_t ready`；add-on 在 `on_begin_effects`
里轮询自己的 fence（producer 侧读值是正常的），把"该槽 GPU 已写完"发布出来；bridge 按
`frames[].frame_id` 选**最新已完成**的槽，用共享内存自旋等待（不再依赖跨卡 fence）。
顺带把 `frames[slot]` 的写入挪到 `Signal` 之前（现在写在 Signal 之后，属同类竞态）。

**给以后的提醒**：不要再在 MV 上投入任何时间，除非换库版本并先用
`--fg-of-const 100000 0` 复验"MV 是否被消费"。

### 协议 v5：槽位同步（已实现，2026-11-07）

MV 被证明是死输入后，**深度是唯一被消费的输入**，而深度的时效性一直是坏的：
`--mv-newest` 实际退化成"永远 slot 3"（跨卡 fence 在消费端返回 `UINT64_MAX`），
而 producer 每 4 帧才刷新一次 slot 3 ⇒ 深度可能落后 **0–4 帧（0–133 ms）**。

v5 的做法（**add-on 与 bridge 必须同版本，`kSharedVersion` 4 → 5**）：

- `FrameShared._pad` → `uint64_t ready`：producer 在自己的 **fence 上轮询**（producer 侧读值是正常的），
  确认某槽的 GPU 拷贝真的完成后，用 `InterlockedExchange64` 把该值发布出来；bridge 不再自己碰跨卡 fence；
- `on_reshade_present` 里把 `frames[slot]` 的字段**挪到 Signal 之前**（原来是 Signal 之后写，与消费者抢跑），
  并在改写期间先把 `ready` 清 0；
- bridge 用 `wait_slot_v5()`：扫所有槽，取 **`ready != 0` 且 `frame_id` 最新、且我们还没用过**的那个；
  等不到新的（最多 `--wait-ms`，默认 20）就退回"已知完成的最新槽"；
- `--mv-newest` 后来**整个删除了**（2026-11-07 清理）：它本来就退化成"永远 slot 3"，
  而 v5 的 `ready` 发布路径因为 fence 轮询被否掉，见 §8.6.1。

构建产物：bridge `7bf824d0da05`、add-on `a7b6aad12dec`（都已部署到 `<TOOLS_DIR>` 与 PBZ）。
自检通过：`--fg-of-selftest: 3 dispatches + convert OK`。

**下一次验证**：重启 PBZ（add-on 换了）→ 跑 `run_mgpu_4x_4k_pbz_depthonly.bat`
（深度单独发力：不发 MV、按 v5 选最新完成槽）。日志新增：`timeouts=` 应该接近 0，
`LATENCY` 与 `show` 应保持 ~118–122；观感重点看**运动物体边缘的拖影是否变少**。

### 第七轮（定稿）：PBZ 的新基线 = 「不发 MV + 最新深度」

回归修复后实测：

| 配置 | 结果 |
|---|---|
| `run_mgpu_4x_4k_pbz.bat`（基线，含跨卡 fence 轮询 bug） | 修复前 game 33→60 ms、帧数折半、闪烁；修复后恢复 |
| **`run_mgpu_4x_4k_pbz_depthonly.bat`（不发 MV + 最新深度 + 本地环 lag0）** | **= 之前最好的一版**，`gpu 10.5–21.5`、`show 118–122`、`skip 0` |
| `run_mgpu_4x_4k_pbz_depthlag1.bat`（本地环延迟 1 帧） | **伪影更严重** ⇒ 深度越新越好，`--depth-lag` 默认 0 |

**因此 PBZ 的推荐配方改为**：

```bat
bridge.exe --game PBZ.exe --den 1 --pool 4 --interp 3 --unlock-mfg --fg-ui none ^
           --fg-proj-rz --fg-fov 58.7 --no-mv
```

（对应 `<TOOLS_DIR>\run_mgpu_4x_4k_pbz_depthonly.bat`；`--mv-newest` 已无意义，
bridge 在 producer 不发布 `ready` 时会立刻回退到轮转选槽。）

### 两个必须记住的坑

1. **不要轮询跨适配器的共享 fence**：`GetCompletedValue()` 在 `SHARED_CROSS_ADAPTER` fence 上走驱动同步路径，
   每帧 4 次就能把游戏的渲染线程拖垮（`game` 33→60 ms、我们自己的循环 30→15 fps、画面闪烁、`skip` 暴涨）。
   协议 v5 那次"发布 ready"的实现就栽在这里，**已删除**；共享槽同步暂时回到轮转读取。
2. **不要读"较旧的共享槽"来制造延迟**：那正是 producer 下一帧要写的那块内存，两块卡同时读写同一片跨卡内存，
   双方都会降到半速。要做延迟必须在**本地**做（`g_depth_ring`，每帧拷最新深度进本地环再取 N 帧前那份），
   实测 lag1 反而更差，所以默认 0。

### 现在的 PBZ 能力上限

- MV 是死输入（模型自己估运动），深度是唯一有效输入，且"最新"最好；
- 深度流已经是 ~1:1（2564×1444 → 2562×1441，重采样几乎无影响），所以分辨率和滤波都不是瓶颈；
- **结论：运动物体边缘的拖影就是这个模型在这块卡上的能力上限**，可调的旋钮已经用完。
- 附带收获：不发 MV 让 `gpu` 从 ~20–22 降到 **10.5–21.5** ⇒ 有富余预算，可以试 **3x@40**（延迟更低）。

| 脚本 | 锁帧 | 插值→上屏 | `gpu` p50 | `total`/预算 | `show` | `skip` | `click→photons` |
|---|---|---|---|---|---|---|---|
| `run_mgpu_4x_4k_pbz_depthonly.bat` ⭐**PBZ 定稿** | 30 | 3840×2160 | **10.5–21.5** | — | **118–122** | 0 | ≈ 74（4x@30） |
| `run_mgpu_pbz_3x40.bat` ⏳**待测**（40fps 输入） | 40 | 3840×2160 | ≈15–18（推算） | — | ? | ? | ≈ 55? |
| `run_mgpu_4x_4k_pbz_depthlag1.bat` | 30 | 3840×2160 | — | — | — | — | 深度延迟 1 帧，**更差**，已否决 |

> PBZ 的定稿配方 = `--den 1 --pool 4 --interp 3 --unlock-mfg --fg-ui none --fg-proj-rz --fg-fov 58.7 --no-mv`
> （**不发 MV**：MV 是该 build 的死输入，省下 16–33 MB/帧，`gpu` 从 ~20 → 10.5–21.5）。
> 8.6 那十轮实验的结论、以及两个必须避免的坑（跨卡 fence 轮询、读旧共享槽），都记在 `HANDOFF.md` §8.6。

### 第八轮：从 OptiScaler 找到 XeFG 的真正 MV 约定（`third_party/optiscaler`）

`OptiScaler/shaders/camera_motion/CM_Common.h` 里的 shader 把约定写得一字不差：

```hlsl
// Velocity in UV units as current - previous, alpha 0 where the game wrote no velocity.
// Output in pixels as previous - current, ...
float2 size = float2(Width, Height);          // = 速度纹理自己的尺寸
if (velocity.a != 0.0f) { Output[pixel] = -velocity.xy * size; }      // 游戏 MV 的换算
...
Output[pixel] = (previousUv - uv) * size;                             // 相机运动的换算
```

⇒ **XeFG 要的是：像素单位（按 MV 纹理自身分辨率）+ 符号 (previous − current)**。

我们的 FFX 场是"像素、按**捕获分辨率**、符号 (current − previous)"，所以正确组合是
**× (mvSize / captureSize) 再取负** —— 这个组合之前**从未测过**（此前 sign 测试用的是 UV 单位，
像素测试用的是未取负的场）。

已实现为默认单位模式 `mvpx`（`--fg-of-units mvpx`，`ux = -outW/fullW, uy = -outH/fullH`），
bridge `d0fa70a4b9ba`，日志会打印 `units=mvpx sign=1`。

**另外 OptiScaler 还有更值得借鉴的实现**：`OptiScaler/shaders/camera_motion/CM_Dx12` 用
**深度 + 重投影矩阵**逐像素生成相机运动矢量（`Reprojection = inverse(curViewProj) * prevViewProj`），
只在游戏自己不写 MV 的地方填（用 alpha==0 判定）。它对 "The Witcher 3" 这种 XeFG 输入缺失的游戏是官方解法，
对我们同样是**逐像素精确**（远好于 1/8 的块匹配），备选方案已确认可行。
`OptiScaler/inputs/FG/XeFG_Inputs_Dx12.cpp` 里还有 `FrameTimeMs()` 处理"游戏把帧时间当秒传"的怪癖。


### 第九轮：深度引导上采样（`--fg-of-depth-k`，默认 40）

`mvpx` 单位定下来后，残留伪影"窄了一些但依旧存在" —— 剩下的来源就是**场的空间分辨率**（1/8 块）：
块边界处物体与背景的矢量被混在一起。之前用**颜色**做引导上采样，低对比区域靠不住；
现在改用**深度**做引导（它和 MV 同尺寸、逐像素，而且本来就是 XeFG 唯一消费的输入）：

- 上采样时 4 个相邻 texel 的权重乘以 `1/(1 + 相对深度差 × depthK)`，相对差 = `|dc-dt| / max(dc,1e-4)`
  （尺度无关，UE5 反向 Z 和标准 Z 都能用）；
- 描述符布局改成 `[0]场 [1]颜色 [2..5]深度环 [6]MV UAV`，深度 SRV **每个环纹理只建一次**，不在飞行中改写堆；
- OF pass 在 `copy_inputs_local` 之前执行，所以首帧深度 SRV 还没绑定时把 `depthK` 强制为 0（否则读未绑定描述符会掉设备）；
  引导用的深度是**上一帧实际 tag 的那一份**（`g_of_depth_desc`），永远是已完成的数据。

旋钮：`--fg-of-depth-k 0` 退回"颜色引导"；`40` 是默认；更大 = 更严格地只吃同深度面。

## 9. 新对话的第一句建议

> "先读 `docs/findings/FINAL_CONFIG.md` §2.1.2 / §2.1.3 和 `docs/findings/HANDOFF.md` §8.6.1（现状结论），
> 需要历史细节再看 §8.6.2 起的实验档案。**MV 是死输入**，不要再在 MV/光流上投入。
> 2026-11-07 的整理轮已完成：脚本收敛（归档目录已在 2026-11-08 移除）、`--fg-of` 宏隔离、
> `third_party` 1.4 GB → 5 MB、`git init` + 首次提交、文档拆成"配方 + 档案"。
> 下一步候选：
> ① 3x 想上 120 fps ⇒ 只能压**游戏**帧时间（DLSS Balanced/Performance），FG 侧没空间；
> ② 换 `libxess_fg`（>1.3.1）时先跑 `--fg-of-const 100000 0`（需 `MGPU_FG_OF=1` 重编）复验 MV 是否重新被消费；
> ③ `--down 1` 在 4K 面板上的观感（`run_mgpu_4x_4k_hq.bat` 已归档，参数是 `--scale 0.75 --down 1`）。"

## 10. 2026-11-07 晚：三条现场反馈（详见 `FINAL_CONFIG.md` §9.20）

| 反馈 | 处理 | 怎么验证 |
|---|---|---|
| 热键启动后按第二次关不掉 | **已定位（实测日志）**：热键/开关都对，是 `bridge` 不理 `WM_CLOSE`——`return 0` 卡在 D3D 静态析构（实测 2.2 s 才被强杀）。修：`hard_exit()`（flush 日志后 `TerminateProcess(self)`）、`WM_CLOSE` 在两个窗口过程里**就地**退出、UI 阶梯收紧到 0.5+0.3 s、UI 的 `WM_QUIT` 改用真实 tid | 按一次启动、再按一次：`mgpu_ui.log` 里 `stop: WM_CLOSE -> N window(s)` 之后应立即 `exited=1`（不再出现 `TerminateProcess`）；`bin\bridge_ui.txt` 里应出现 `[bridge] WM_CLOSE ... - exiting now` |
| 异常时叠加层赖在桌面最上层 | `assert_topmost()` 里的焦点守卫：前台是**我们自己的窗口** → `focus_game()` 交还游戏；前台是**第三方窗口** 连续 5 s → 退出（`--exit-on-focus-loss N`，0=永不）；`!IsWindow(game)` → 立刻退出 | 开着 FG 切到浏览器，5 秒内 `bridge` 自行退出，日志 `another window has been in front for 5s - exiting` |
| 影之刃零菜单点不动（攻击正常） | ① 焦点守卫（游戏以为自己"未激活"时会丢菜单点击，而 raw input 的攻击照旧）② **点击探针** `click probe`（`WindowFromPoint` 会跳过 `WS_EX_TRANSPARENT`，正好等价于"这一下点击会落到谁身上"）③ `--overlay-layered` A/B（UI："叠加层 / 分层窗口 (A/B)"） | 看日志 `click probe` 行：`(ok)` ⇒ 点击确实到了游戏，问题在游戏的激活状态；`<-- NOT the game` ⇒ 我们的某个窗口吃了点击，勾上 A/B 再试 |

`mgpu_ui.log`（`bin` 目录）里的关键行：
`ui started, hotkey index=0` / `WM_HOTKEY id=1` / `hotkey/button: running=…` /
`start: pid=… cmd=…` / `stop: WM_CLOSE -> N window(s)` / `stop: TerminateProcess`。

`bridge_ui.txt`（`bin` 目录，每次启动覆盖）是 **bridge 的完整 stdout**：
`escape hotkey ... registered=` / `borderless: ...` / `zorder:` / `click probe:` /
`WARNING overlay received ...` / `WM_CLOSE ... - exiting now`。看叠加层/点击问题只看这个文件。


## 11. 输入探针（2026-11-07 晚，**已移除**）

菜单点击的排查曾用 "subclass 游戏窗口 + `WH_MOUSE_LL` 钩子 + 计数器" 把测量点搬进游戏进程，
结论是 **`--overlay-layered` 即解**（现已定为默认），所以整套探针在当晚就从 `game_capture.cpp`
删掉了（连带 `user32.lib`）。判定表仍留在 `FINAL_CONFIG.md` §9.23 供查。

现在 add-on 的日志也是**默认关**的：`[capture] Log=1` 或环境变量 `MGPU_CAPTURE_LOG=1` 才写
`game_capture.log`，正常游玩不在游戏目录留任何文件。
