# 最终配置与踩坑清单

> **文档已重组（2026-11-08）**：本文件现在位于 `docs/findings/`，作为
> **历史附录**保留（踩坑清单 §5 + 实验档案 §9）。
> 当前结论请先看 [`CONCLUSIONS.md`](CONCLUSIONS.md)（结论先行）与
> [`QUALITY_VARIABLES.md`](QUALITY_VARIABLES.md)（画质变量表）；
> 怎么装、怎么调参数请看 `docs/ops/`（BUILD / DEPLOY / CONFIG / UI）。
> 本文里 2026-11-07 之前的判断，凡与上述两篇冲突的，以那两篇为准。

> 这份文档是**唯一需要读的运维文档**：怎么装、怎么跑、每个开关干什么、坏了看哪个数字。
> 实验过程与证据链在 `docs/findings/milestones/M4_WGC_RESULTS.md`（§1–10），本文只保留结论。
> **本轮（MFG 解锁 / 4K+VSR）的改动与下一步：`docs/findings/HANDOFF.md`。**
> 最新实测：2026-11-06，Windows 11 / RTX 3080 Ti（游戏）+ RX 6750 GRE（bridge）/ 5120×2880@120Hz HDR10。

**一句话**：跨厂商双卡帧生成链路已完整跑通——5K 原生 @60fps、1440p 插值 @80fps、
1440p 插值 + 4x 帧生成 @120fps 三套配置都可用，
颜色正确、长跑稳定（4000+ 帧无 TDR）、延迟优化过。核心难点只有两条：**XeFG 靠 XeLL 标记推断节奏**、
**WGC 帧池必须排空取最新**。这两条修好后，系统性卡顿归零。

---

## 1. 架构（为什么必须是两个进程）

```
游戏进程 (RTX 3080 Ti)                    bridge 进程 (RX 6750 GRE)
┌──────────────────────────┐              ┌────────────────────────────────┐
│ Cyberpunk 2077           │              │ WGC 抓颜色 (FP16 scRGB 5120x2880)│
│  └ ReShade 6.8 + addon   │  共享内存     │   ↓ 5K→HDR10 转换 pass          │
│     └ 钩 NGX Evaluate    │──MV/深度────▶│   ↓ XeSS-FG 2x 插值             │
│        取 MV / 深度 / jitter│  文件映射堆  │   ↓ 覆盖层窗口上屏（HDR10）      │
└──────────────────────────┘              └────────────────────────────────┘
```

- **颜色**：不走共享内存。WGC 直接在本机 DWM 已经跨卡搬过一次的画面上取 FP16 scRGB，
  所以 5K 下 118 MB/帧的 FP16 对我们**完全免费**（`docs/findings/milestones/M4_WGC_RESULTS.md` §3）。
- **MV / 深度**：走共享内存（2560×1440，MV `R16G16_FLOAT` 14 MB + 深度 `R32_FLOAT` 14 MB，
  共 28 MB/帧，≈1.5 ms），由 add-on 从 NGX 钩子里取。
- **必须两进程**：ReShade 的 D3D12 钩子是进程全局的，在游戏进程里建第二厂商设备会死锁
  （实测，见 `docs/findings/milestones/M3.2_RESULTS.md`）。拆开还带来一个额外好处：AMD 侧崩溃不会带走游戏。

---

## 2. 部署与启动顺序

### 2.1 一次性部署

| 文件 | 放到哪里 | 来源 |
|---|---|---|
| `game_capture.addon64` | 游戏 `bin/x64/`（与 `Cyberpunk2077.exe` 同级） | `./scripts/build_addon.sh src/game_capture game_capture` → `build/`。**必须与 `bridge.exe` 同一次构建**：共享内存协议现为 **v10**（4 槽 + 生产端身份/相机 + MV 请求位 + 内容尺寸代次 + **per-slot ACK 信用窗口 / seqlock / 队列水位**）。版本不符时 bridge 会直接打 `shared block version N, this bridge speaks 10 - rebuild/redeploy BOTH sides` 并退出，不会等 120 秒也不会读歪 |
| `game_capture.ini` | 同上（与 add-on 同目录，**必须改成 `Color=0`**，见下） | 手写，1 行 |
| `bridge.exe` | `<TOOLS_DIR>\` | `./scripts/build_bridge.sh` → `build/` |
| `libxess_fg.dll` / `libxell.dll` | **与 `bridge.exe` 同目录** | `third_party/xess/bin/`（`build_bridge.sh` 会自动拷）。MFG 靠 `--unlock-mfg`，不用换 DLL，见 `docs/findings/MFG.md` |
| VC++ 运行库 14.40.33810+ | 系统 | 缺了 `bridge.exe` 直接起不来 |

`game_capture.ini`（最终配置）：

```ini
[capture]
Color=0
; 可选，一般不用写：只有当 NGX 报的渲染尺寸不对时才手动指定 MV/深度尺寸
; RenderWidth=3840
; RenderHeight=2160
; MV/深度是否先降采样再写共享内存：100 = 不降（默认，推荐）
; DLAA 这种 2160p 渲染的场合可以写 150（2160p→1440p，省 ~3ms 跨卡搬运），代价是渲染卡多一个小 pass
StreamPercent=100
```

> **MV/深度流的尺寸会跟着渲染分辨率自动走**（`HANDOFF` §8-24）：
> add-on 取 NGX 每帧上报的 `Width/Height`，所以 DLSS Quality(4K→1440p)、Balanced(~1250p)、
> Performance(1080p)、DLAA(2160p) 各自对应自己的流尺寸，不用改配置、不用重编。
> 但**换档位必须重启游戏**（共享块每个进程只建一次）；中途换会打 WARNING。
> `StreamPercent` 是在此之上的可选缩放因子，`100` 表示原样透传。

> **MV/深度流的尺寸跟随 DLSS 档位**（`docs/findings/FINAL_CONFIG.md` §2.1 / `HANDOFF` §8-24）：
> 4K + **DLSS Quality** 时游戏内部渲染 1440p → 流是 2560×1440；
> 4K + **DLAA** 时内部渲染 2160p → 流是 3840×2160。add-on 现在直接取 NGX 上报的渲染尺寸，
> 所以换档位**不需要重新编译**，但**必须重启游戏**（共享块每个进程只建一次）。
> add-on 日志会打 `shared: mv/depth at 3840x2160 (NGX render size)`，换档位忘了重启会打 WARNING。

**为什么 `Color=0`**：颜色现在由 bridge 侧 WGC 抓取，共享内存里的 colour 流已经没人消费
（`bridge.cpp` 只用 `streams[0]` 读宽高，见 `open_shared()`）。开着它等于让游戏每帧多做一次
56 MB 的 `CopyResource`，纯粹是在游戏的关键路径上白花时间。add-on 日志会打印
`config: Color=0 (path ...)`，可据此确认生效。

> ReShade 自身：6.8.0.2155，`dxgi.dll` 方式加载，游戏内**必须开 DLSS**（NGX 钩子挂在
> 驱动目录的 `_nvngx.dll` 上，不开 DLSS 就没有 MV/深度）。
> 游戏必须**无边框/窗口化**（独占全屏 WGC 抓不到）。

**2.1.1 第二款游戏（任意 ReShade+DLSS 游戏，例如虚幻 5 的 `PBZ.exe`）**

```bash
./scripts/deploy_game.sh "<GAME_DIR>" Game.exe
```
做的三件事（都可回退）：把 ReShade 6.8 拷成该目录的 `dxgi.dll`、拷 `game_capture.addon64`、
写 `game_capture.ini`（`Color=0` / `StreamPercent=100`）。然后：

1. 游戏内：**无边框/窗口化**（独占全屏 WGC 抓不到）、**开 DLSS**（TSR/FSR 不行）、
   **关掉游戏自带的帧生成**（DLSS-G / XeSS-FG 会和我们抢交换链）；
2. RTSS 给这个 exe 单独建 profile 并锁 30；
3. 跑 `run_mgpu_4x_4k_pbz.bat`（同一套参数 + `--game PBZ.exe`）。

bridge 用 `--game <exe>` 找窗口（默认 `Cyberpunk2077.exe`），add-on 依旧按该游戏上报的渲染分辨率
自动定 MV/深度尺寸，所以换游戏不用改配置。首次启动还要注意 ReShade 自己的引导浮层（按 Home 关掉）。

> **必须有至少一个启用的 ReShade 技术**（`HANDOFF` §8-31）：add-on 的每帧工作挂在
> `reshade_begin_effects` 上，而 ReShade **没有启用任何效果时不会触发这个事件** ——
> 表现就是 bridge 一直停在 `waiting for 'Local\mgpu_mailbox_v1'`。
> `deploy_game.sh` 会自动装 `reshade-shaders\Shaders\mgpu_depth_tap.fx`、写
> `ReShadePreset.ini`（启用 `MGPU_DepthTap`）、并把 `ReShade.ini` 的
> `EffectSearchPaths` 指到 `.\reshade-shaders\Shaders\**`。这个特效**不改画面**（渲染到私有目标）。
> 注意 ReShade 会在退出时自己重写 `ReShade.ini`，所以这些文件要在**游戏关闭时**修改。

### 2.2 每次运行（顺序不能反）

```
1) 启动游戏，进入游戏世界，确认帧率稳定（RTSS 锁帧见 §3）
2) 双击 run_mgpu.bat（或 run_mgpu_5k.bat）
3) 出现无边框覆盖窗口即为成功；游戏窗口仍是前台、输入照常
4) 停止：关掉那个控制台窗口 / Ctrl+C
```

bridge 启动时会找 `Cyberpunk2077.exe` 的窗口，**找不到就直接退出**（`return 2`），
所以必须先有游戏。

### 2.3 脚本位置

**2026-11-07 收敛过一轮**：`scripts/` 现在只留 21 个 `run_*.bat` + 3 个 `deploy_*.bat` + `probe_fg.bat`；
其余 38 个一次性实验脚本已移除（参数与结论见 `ARCHIVED_EXPERIMENTS.md`），
`<TOOLS_DIR>\archive\` 里也归档了旧的 `bridge.exe.prev*`、60 个旧日志和实验脚本。

| 脚本 | 用途 | 部署位置（同名） |
|---|---|---|
| `run_mgpu.bat` | CP2077 性能配置（`--den 2`，1440p 插值） | `<TOOLS_DIR>\` |
| `run_mgpu_5k.bat` | CP2077 画质配置（原生 5K） | 同上 |
| `run_mgpu_5k_mfg.bat` | 5K + 4x（实测撑不住，留作对照） | 同上 |
| `run_mgpu_4x.bat` | CP2077 MFG 4x 基线（1440p 插值） | 同上 |
| `run_mgpu_4x_c.bat` | CP2077 4x + `--fg-ui none` ⭐1440p 日常 | 同上 |
| `run_mgpu_3x_40.bat` | CP2077 3x@40（低延迟，~120 fps） | 同上 |
| `run_mgpu_4x_4k.bat` | CP2077 4x + 4K 面板/VSR（`--scale 0.75`）⭐4K 日常 | 同上 |
| `run_mgpu_4x_4k_native.bat` | CP2077 4x @ 原生 4K（无 VSR、无缩放） | 同上 |
| `run_mgpu_cp2077_dump.bat` | CP2077 + `--dump 5`（导出 colour/depth/MV） | 同上 |
| **`run_mgpu_4x_4k_pbz_depthonly.bat`** | **PBZ 定稿 4x@30（120 fps）** | 同上 |
| **`run_mgpu_pbz_3x40.bat`** | **PBZ 3x@40（~75 fps，伪影更少）** | 同上 |
| `run_mgpu_pbz_dump.bat` | PBZ + `--dump 5` | 同上 |
| `run_mgpu_pbz_dyn.bat` | PBZ **动态帧率**（RTSS 不锁）+ `--interp-auto --fg-rt-mode quant`（§9.14） | 同上 |
| `run_mgpu_pbz_dyn_rt.bat` | 同上但只改帧时间喂法（`--fg-rt-mode median`），用来隔离贡献 | 同上 |
| `run_mgpu_pbz_dyn_ema.bat` | 同上的对照组（`--fg-rt-mode ema`，历史行为） | 同上 |
| `run_mgpu_pbz_dyn_vrr.bat` | median + `--tearing`（系统 VRR 已开时用） | 同上 |
| **`run_mgpu_pbz_120_5x24.bat`** | **RTSS 24 + 5x = 精确 120 fps（1 vblank/帧）**，无 VRR 的正解（§9.15） | 同上 |
| **`run_mgpu_pbz_dynmfg.bat`** | **OptiScaler 式 Dynamic MFG（`--dyn-mfg`，RTSS 关）**：不锁帧、自动 1x..6x、够快就关掉 FG（§9.17） | 同上 |
| **`run_mgpu_pbz_dynmfg2.bat`** | 同上但**给循环留余量**：上限 4x + `--fg-target 110 --fg-round 0`（30 fps 输入时选 3x=90，不丢帧） | 同上 |
| **`run_mgpu_pbz_3x_slack1.bat`** | **固定 3x + `--slack 1`**：去掉控制器降/升档的顿挫，并吸收渲染卡满载导致的迟到帧（§9.17 第四次实测） | 同上 |
| ~~`run_mgpu_pbz_cadence.bat`~~ | ❌ **已实测失败**，改名归档到 `_FAILED_run_mgpu_pbz_cadence.bat`（§9.18） | — |
| `run_mgpu_pbz_100_4x25.bat` | 桌面 100 Hz + RTSS 25 + 4x = 精确 100 fps | 同上 |
| `probe_fg.bat` | DLL 能力自检（`--fg-probe`） | 同上 |
| `deploy_capture.bat` / `deploy_m2.bat` / `deploy_m3.bat` | 一次性部署 add-on | — |

> **所有 `run_mgpu*.bat` 都带 `--no-mv`**（2026-11-07 统一）：两个游戏的 `game_capture.ini`
> 都是 `SkipMv=1`，MV 流根本不生产，不加 `--no-mv` 只会去读一条不再更新的旧流
> （画面无影响，但语义混乱）。

改完后把仓库里的版本拷到 `<TOOLS_DIR>\` 覆盖。日志就写在脚本所在目录
（`bridge_run.txt` / `bridge_5k.txt` / `bridge_4x_c.txt` / `bridge_4x_4k.txt` / `bridge_3x40.txt`），
**同名日志每次运行都会被覆盖**，关键会话先另存（`docs/findings/HANDOFF.md` §8-16）。

> 升级 `bridge.exe` 前先留一份旧的：`copy bridge.exe bridge.exe.bak`（`bin\archive\exe_prev\` 里已经有 13 份）。

---

## 2.1.2 PBZ / UE5 的定稿配方（2026-11-07，十轮对照实验之后）

```bat
bridge.exe --game PBZ.exe --den 1 --pool 4 --interp 3 --unlock-mfg --fg-ui none ^
           --fg-proj-rz --fg-fov 58.7 --no-mv
```

对应 `<TOOLS_DIR>\run_mgpu_4x_4k_pbz_depthonly.bat`：
实测 `gpu 10.5–21.5`、`show 118–122`、`skip 0`、`click→photons ≈ 74 ms`。

配套 `game_capture.ini`（PBZ）：

```ini
[capture]
Color=0
StreamPercent=100
StreamAlign=1
SkipMv=0          ; 0 = 由 bridge/UI 决定（v7 请求位）；1 = 永远不产出
DepthConvert=1    ; 同帧的真实深度，经 ds_run pass
MvNormalize=0
MvFlip=0
Input=auto        ; 输入源：auto / ngx / ffx / xess / depth（M6.3）
```

> **`Input=auto`**：add-on 现在按"游戏真的加载了哪个超分库"来选输入源
> （`ngx` = DLSS、`ffx` = FSR、`xess` = XeSS、`depth` = 只有 ReShade 深度），
> 选定后**本进程内固定**（流尺寸只建一次）。日志里会有一行
> `input source: ngx (api 1) - pinned for this process`；一个都还没等到时每 120 帧打
> `no upscaler input yet (waiting for ngx)`。想强制某个源就改这一行（`auto` 是默认）。

> **`SkipMv` 与协议 v7 的请求位（2026-11-07 晚）**：`SkipMv=0` 表示"看 bridge 要不要"，
> `SkipMv=1` 是"永远不产出"的硬开关。bridge 每帧往共享块写 `consumer_request`：
> 带 `--no-mv` 时请求 0，不带时请求 `kRequestMv`，add-on 只有被请求时才转换/搬运 MV
> （一次全屏 pass + 每帧 ~14.7 MB）。所以"关着 MV"时一分钱都不花，UI 里切"运动矢量"
> 立刻生效（下一次启动 bridge；add-on 侧不用重编、不用改 ini）。
> `docs/ops/UI.md` 的 `运动矢量` 行：关闭 / 游戏 MV / 游戏 MV(反向)（→ `--no-mv` / 无 / `--mv-flip`）。
> **实测**：`[mv] the consumer asked for motion vectors - stream enabled` 后新开 MV 流，
> 不请求时无该 pass。

**结论（全部用对照实验验证，细节见 `HANDOFF.md` §8.6）：**

1. **运动矢量：画面看不到差别，但 XeFG 确实在读它（2026-11-08 修正）**。
   早先的结论是"`MV=0` / `MV=100000 px` / 真 MV 三者画面完全一致 ⇒ 死输入"；
   **成本证据推翻了后半句**。用新增的 `--mv-shadow`（照常拷真 MV、但只喂零化 dummy）做三组对照：

   | 运行 | `gpu` 中位 | `show` | `skip` |
   |---|---|---|---|
   | A `--no-mv`（不拷、喂零） | 11.3 ms | 118 | 20 |
   | B `--no-mv --mv-shadow`（**拷真 MV**、喂零） | 13.2 ms | 118 | 16 |
   | C 真 MV（拷 + 喂真） | **20.6 ms** | **110** | **77** |

   ⇒ **跨卡拷贝只值 +1.9 ms**（和同尺寸深度的 +2.1 ms 一致），而 **C−B = +7.4 ms 是 XeFG 自己的
   数据相关工作量**：同样的拷贝、同样的 pass，只有"喂真值 vs 喂零"不同。**所以它读 MV**。
   剩余未知：**读了是否让画质更好**（旧的眼测说看不出差别，但那时没有控制拷贝成本，也可能只是
   3x 下差异不明显）。
   **对策**：默认仍 `--no-mv`（3x@4K 下 `gpu` 20.6 ms 顶到 25 ms 预算，`skip` 会 ×5）；要用 MV 就得
   腾出预算（降到 2x、或降分辨率、或换更强的消费卡），并用 `--mv-shadow` 复现本实验。
   > **怎么复验**：UI「帧生成」卡片把 **运动矢量** 选成 `游戏 MV`（= 去掉 `--no-mv`，bridge 通过
   > 协议 v7 请求），add-on 日志会出现 `[mv] the consumer asked for motion vectors - stream enabled`；
   > 再切回 `关闭` 做 A/B。要区分"拷贝成本"与"XeFG 消费"，用 `--no-mv --mv-shadow` 做中间组。
   > 代价：打开时多一次全屏 pass + 每帧 ~14.7 MB 跨卡写 + **7.4 ms 消费端 GPU**（关着时零成本）。
2. **深度是被确认消费的两个输入之一（另一个是 MV）**，并且：越新越好（延迟 1 帧更差）；分辨率不敏感（480×270 更差，
   2562×1441 与 3840×2160 **无差别**，所以用渲染分辨率省带宽）；方向不能反（`--fg-depth-inv` 更差）；
   不能省（`--no-depth` 更差）。
3. **jitter：一律用游戏自己的（`--fg-jitter-zero` 已弃用，2026-11-08）**。
   彩色确实是 DLSS 已解析输出（没有 jitter），但游戏的运动矢量是在**抖动后的画面**上算出来的 ——
   XeFG 需要 jitter 才能把当前帧和历史帧对齐。实测（PBZ 2x@40，真 MV）：
   `--fg-jitter-zero` 传到 XeFG 的 `(0,0)` → **画面闪烁 + 模型边缘伪影**；
   传游戏的 `(0.0625, -0.4630)` → **闪烁和伪影都减少**（`gpu`/`show` 同级，纯画质差异）。
   ⇒ **`--fg-jitter-zero` 弃用**：bridge 仍解析它（老脚本不会报错），但只打印一条弃用提示、
   不再生效；UI 也不再发这个参数。**永远传游戏的 jitter。**
4. **残留的运动物体边缘细伪影 = 该模型在这块卡上的能力边界**：MV 单位/符号/分辨率、深度分辨率/方向/新鲜度、
   jitter、逐像素细化、深度引导上采样，全部试过，无进一步改善。

**两个必须避免的坑**（都踩过、都已修）：

- 不要轮询 `SHARED_CROSS_ADAPTER` fence 的 `GetCompletedValue()`：每帧 4 次就能把游戏渲染线程拖垮
  （`game` 33→60 ms、帧数折半、闪烁）；
- 不要为了"深度延迟一帧"去读**较旧的共享槽**：那是 producer 正在写的内存，两块卡抢同一片内存双方都半速。

**留存的工具**：`--dump N`（导出 colour/depth/MV 原始数据）、`scripts/analyse_dump.py`（`.sh` 是
numpy 包装）、`temp/inspect_dump.py`、`temp/compare_mv_of.py`、`temp/ffx_of_test.cpp`（FFX 光流测试台，
含 `dump` 模式）、`--fg-debug-only-interp` / `--fg-debug-tag-interp`（XeFG 调试视图）。

> `--fg-of` 与 `--fg-of-selftest` **默认不编译**（`MGPU_FG_OF=0`，见 §4.7 与 `HANDOFF.md` §8.6.1）。
> 要用它们复验"新 `libxess_fg` 是否重新消费 MV"：`src/bridge/build.bat` 的 `cl` 行加 `/DMGPU_FG_OF=1`
> 重编即可，`third_party/ffx` 的头文件与两个 `.lib` 都还在。

## 2.1.3 PBZ 的第二套：3x（`--interp 2`）—— 已实测

`run_mgpu_pbz_3x40.bat`：

```bat
bridge.exe --game PBZ.exe --den 1 --pool 4 --interp 2 --unlock-mfg --fg-ui none ^
           --fg-proj-rz --fg-fov 58.7 --no-mv
```

**实测（2026-11-07，`bridge_pbz_3x40.txt`）**：

| 指标 | 数值 |
|---|---|
| `xN` | **3.00** |
| `game` | **39.6–43.0 ms**（≈24 fps —— 游戏在 4K DLSS Quality 下是瓶颈，RTSS 40 根本没压到） |
| `gpu` | **4.3–8.1 ms** |
| `skip` | **0** |
| `show` | 73–76（= 24 fps × 3） |
| `LATENCY` | compose→present **5–12 ms**，+scanout ⇒ **5–20 ms** |

**用户实测体感**：① 运动物体边缘伪影**明显改善**；② 即使输入跑不满 40 Hz 也**不卡顿**
（输入节奏比 RTSS 锁 30 更平滑，XeFG 只合成 2 张时每张的时间间隔更短）。

**取舍**：

| 配置 | 输入 | 输出 | 伪影 | `click→photons` |
|---|---|---|---|---|
| **4x@30**（`run_mgpu_4x_4k_pbz_depthonly.bat`） | 30 fps | **118–122 fps** | 较多 | ≈74 ms |
| **3x**（`run_mgpu_pbz_3x40.bat`） | 24 fps（游戏瓶颈） | 73–76 fps | **明显更少** | 更低 |

⇒ 想要 **120 fps 就留 4x@30**；想要**更干净的画面**就用 3x（代价是输出帧率只有 ~75，
因为 4K DLSS Quality 下游戏本身只有 ~24 fps）。若想在 3x 下也冲 120，需要把**游戏自己**的
帧时间压到 25 ms 以内（例如 DLSS 改 Balanced/Performance），那不是 FG 侧能解决的。

## 3. 三套可用配置

> 2026-11-07：下表"核心开关"里的 `--mv-newest` 已删除，脚本里现在换成 `--no-mv`（见 §2.3）。
> 数字是删除前的实测，~~MV 不影响画面也不影响这些指标~~ —— **该结论已被推翻**：MV 确实被消费，见 `CONCLUSIONS.md` §2。

| 用途 | 脚本 | 核心开关 | 游戏锁帧 (RTSS) | 输出 | 画质 |
|---|---|---|---|---|---|
| **性能** ⭐ | `run_mgpu.bat` | `--den 2 --pool 4` | **40 fps** | **76–80 fps** | 1440p 插值 + 1.33× 放大 |
| **画质** ⭐ | `run_mgpu_5k.bat` | `--den 1 --pool 4 --wait-ms 0` | **30 fps** | **58–61 fps** | **原生 5K，零放大** |
| 最顺滑（基线） | `run_mgpu_4x.bat` | `--den 2 --pool 4 --interp 3 --unlock-mfg` | **30 fps** | **117–124 fps** | 1440p 插值（4x 帧生成） |
| **最顺滑** ⭐ | `run_mgpu_4x_c.bat` | 上面 + `--fg-ui none` | **30 fps** | **118–120 fps** | 同上（`--fg-ui none` 零风险，日常用这个） |
| **最顺滑 + 低延迟** ⭐ | `run_mgpu_3x_40.bat` | `--den 2 --interp 2 --unlock-mfg --fg-ui none` | **40 fps** | **~120 fps** | 同上；`click→photons` 比 4x@30 少约 20 ms（§6.3.1） |
| **4K 面板 + VSR** ⭐ | `run_mgpu_4x_4k.bat` | `--scale 0.75 --interp 3 --unlock-mfg --fg-ui none` | **30 fps** | **~120 fps** | 5K 抓取 → **我们在 FG 前下采样到 4K** 再插值；细节接近 5K，`gpu` 只有 5K 的 56%（§9.7） |

> 锁帧用 RTSS（`Cyberpunk 2077` profile）。锁 40 而不是锁 60：XeSS-FG 每次 `Present`
> 内部要对齐垂直同步（约 12–15 ms），40fps 的 25 ms 预算刚好容得下 `present + FG`；
> 锁 30 是为了 33.3 ms 的预算容得下多帧生成（4x 在 1440p 下每真实帧要 ~25–32 ms）。
>
> **5K 不能上 4x**：5K 每生成一帧约 13 ms，3 帧 ≈40 ms > 33.3 ms 预算，实测掉到
> `19fps × 4 ≈ 80fps`（`skip=19–26`）。5K 的甜点就是 2x。详见 `docs/findings/MFG.md` §3.1。

### 3.1 实测数据

| 指标 | 1440p 模式 (2x) | 5K 模式 (2x) | 1440p 模式 (4x) |
|---|---|---|---|
| `gpu` | 3.7–5.3 ms | 4–20 ms | p50 **17.3** / max 24.3 ms |
| `present` | 3.6–10.7 ms | 12.7–15.1 ms | p50 13.7 ms |
| `total` | 7.5–17 ms | 16–33 ms | p50 **31.8**（预算 33.3）/ max 35.0 |
| `load` | 29–75% | 58–100%（峰值贴顶但可接受） | p50 93% / max 104% |
| `skip`（**真丢帧**） | **0** | **0–2** | 84/99 窗口为 0，其余 1–3 |
| `HITCH` | 3 次 / 103 窗口 | 1 次 / 30 窗口 | **1 次 / 99 窗口** |
| `xN` / `xNmiss` | 2.00 / 0 | 2.00 / 0 | **4.00 / 0**（98/99 窗口） |
| `age`（WGC 延迟） | 3.9–6.3 ms | 10–14 ms | p50 8.0 ms |
| 上屏 | 76–80 fps | 58–61 fps | **p50 120**（108–123） |
| 长跑稳定性 | 4000+ 帧无 TDR | ✅ | 3921 帧 / 0 报错 |

> 4x 那一列数据来自 99 个统计窗口（约 130 秒）。`skip` 的 15 个非零窗口里，
> **10 个是 `age ≥ 10 ms` 的到达抖动**、7 个 `gpu < 12 ms`（GPU 有余量），
> 只有 3 个是真的贴预算 → 说明这一档的丢帧主因在 **WGC/DWM 侧**，不是算力。
> 排查脚本：`scripts/logstats.py`、`scripts/logdeep.py`、`scripts/skipcause.py`。

### 3.2 为什么不是 80fps 以外的数字

- **80fps on 120Hz = 3:2 抖动**（8.33 / 16.7 ms 交替）。这是物理事实，不是 bug。
  解法只有两个：开 VRR，或让输出锁在 30/60 这种整数比。介意就用 5K 配置（60fps 一半帧率）。
- **4x 需要 `--unlock-mfg`**：官方 `libxess_fg.dll` 在非 Intel GPU 上把插值帧数夹到 1（2x），
  但那是这个 DLL 里 4 个字节 + `libxell` 里 1 个字节的检查；`--unlock-mfg` 改掉后**实测上报 3（4x）**。
  机制与代价见 `docs/findings/MFG.md`。

---

## 4. 开关全集

`bridge.exe` 的参数是**子串匹配**（`wcsstr(GetCommandLineW(), ...)`），所以 `--den 2` 必须带空格写全；
参数写错不会报错，只是被忽略，**启动时那行 `mode:` 日志才是真相**：

```
[bridge] mode: fg=xess-fg den=2 scale=0.00 buffers=2 pool=4 latency=1 tearing=0 xell=no-sleep(mode 1) inputs=on sharp=0.00 colour=0 interp=0
```

### 4.1 输出 / 插值

| 开关 | 默认 | 范围 | 作用 | 建议 |
|---|---|---|---|---|
| `--den N` | `1` | 1–8 | 输出（及插值）分辨率 = 抓取尺寸 / N | 5K 用 `1`，性能配置用 `2` |
| `--scale F` | 关闭 | 0.25–1.0 | 输出 = 抓取尺寸 × F（优先于 `--den`）。**4K 面板用 `0.75`**（5K×0.75 = 3840×2160，等于把下采样放到帧生成之前），见 §9.7 | 日常 `0.75`；5K 想留余量 `0.95` |
| `--half` | — | — | 等价于 `--den 2`（老开关，保留兼容） | 用 `--den 2` |
| `--sharp F` | `0` | 0–2 | 放大时的 unsharp mask | 降分辨率时 `0.5`；原生 5K 保持 0。**用了 `--down 1..3` 就保持 0**（真低通之后再锐化只会放大振铃） |
| `--down N` | `0` | 0–3 | 缩放核：`0` = **单次双线性**（旧行为，等价于点采样）、`1` = Catmull-Rom（support 2）、`2` = Mitchell-Netravali B=C=1/3（无振铃）、`3` = Lanczos-3（support 3，最锐）。四个都是按比例拉伸的真低通核，在**每个真实帧**上跑一次（不是每个插值帧） | 4K 面板（`--scale 0.75`）用 **`1`**；`--den 2` 是精确 2:1，双线性本来就是盒式平均，差别很小 |
| `--fg warp` | off | — | 用自研 MV 插帧替换 XeSS-FG（强制 `--den 1 --scale 0`） | 备选，见 §4.4 |

> `--den` / `--scale` 只影响**呈现与插值的尺寸**。WGC 永远抓全尺寸（5120×2880），
> 缩小是在我们自己的 pass 里降采样——**千万不要**试图用更小的 WGC 帧池尺寸来省算力，
> 那是裁剪（只给左上角一块），不是缩放（§5.5）。

> **`--down` 到底改善了什么**（CPU 侧等效实现 `temp/check_down.py` 可复算）：
> 5K→4K 是 0.75 倍，比整数倍更糟——单点双线性只把 1.33×1.33 的输入压成 1 个像素，
> 相当于"点采样 + 一点平均"，高频直接折成摩尔纹；`--sharpen` 只是把糊掉的边缘重新拉硬，
> 顺便把走样也放大。用棋盘格（Nyquist）输入量化的结果：
>
> | 核 | 一个轴上的抽头（0.75 倍） | Nyquist 残留幅度 | 阶跃过冲 |
> |---|---|---|---|
> | 双线性（旧） | 1（4 texel） | **67%**（0.167–0.833） | 0 |
> | `--down 1` Catmull-Rom | 7 | **41%** | +0.5% |
> | `--down 2` Mitchell | 7 | 41% | +0.3% |
> | `--down 3` Lanczos-3 | 9 | **31%** | +1.1% |
>
> 代价：这个 pass 每个**真实帧**只跑一次（4x 时 30 次/秒），4K 下 7×7=49 次 `Load`，
> 约 4 亿次/帧 ≈ 12 Gtexel/s，对 6750 GRE 的 TMU（理论 ~400 Gtexel/s）是个零头。
> 若 `gpu` 涨超过 ~0.5 ms，说明踩到了别的瓶颈（多半是 L2 复用被打断），先看 `--down 2`。
> **抓取尺寸 == 输出尺寸时（原生 4K：`--den 1 --scale 0`）核会被自动跳过**，走单点双线性，
> 日志打 `resize filter: not needed ... is 1:1` —— 那种链路一次缩放都没有，`--down` 不起作用。

### 4.2 时序 / 延迟（这几条最容易被误调）

| 开关 | 默认 | 范围 | 作用 | 建议 |
|---|---|---|---|---|
| `--pool N` | `2` | 2–8 | WGC 帧池深度 | **`4`**（处理节奏要留抖动余量；不影响延迟，因为排空取最新） |
| `--mv-newest` | ~~off~~ | — | **已删除（2026-11-07）**：跨卡 fence 在消费端读不到值，这个开关退化成"永远选最后一个槽"；发布 `ready` 又要轮询 fence 把游戏拖垮。现在固定轮转选槽（`HANDOFF.md` §8.6.1） | 不要再用 |
| `--wait-ms N` | `20` | 0–200 | 等 producer 发布 MV 的超时 | 5K 用 **`0`**；1440p 保持默认 |
| `--buffers N` | `2` | 2–4 | 交换链缓冲数 | 默认 `2`（加到 3 实测无收益） |
| `--latency N` | `1` | 1–4 | DXGI 最大帧延迟 | 默认 `1` |
| `--no-throttle` | off | — | 不等上一帧 GPU 完成（队列深度 >1） | ❌ 实测更差，别用 |
| `--pace HZ` / `--pace auto` | 关闭 | 10–240 Hz | 定点呈现 | ❌ **不要用**（§5.3） |
| `--slack N` | `0` | 0–3 | 最多扣住 N+1 个抓取帧、**处理最旧的**：把"晚一帧就丢"换成"晚一帧就多等一帧"。`0` = 排空取最新（零延迟） | 只在"尾部超预算"时有用（判据见 §9.8）；代价最多 +N 个输入间隔的延迟 |
| `--fg-rt-mode ema\|raw\|median\|quant` | `quant` | — | 喂给 XeFG 的 `frameRenderTime` 用哪种估计：指数平均 / 上一帧原始值 / 16 帧中位数 / 中位数吸附到整数 vblank。动态帧率下 `ema`（≈330 ms 尾巴）会说谎 | **默认 `quant`**（整拍）；动态帧率另见 §9.14 |
| `--fg-rt-spike 0\|1` | **`1`** | — | **单帧输入尖峰**处理：上一帧间隔 > 1.35× 计划（快速转视角、流送卡顿）时，把这一帧的**真实长度**（吸附整 vblank，封顶 3× 计划）喂给 XeFG，让它把 2 帧摊在更长的间隔里，而不是排在 2 个 vblank 里留 2 个空刷新。日志 `rt spike:` / `rt spikes: N`。`0` = 关闭（A/B 用）。见 `PERF_BASELINE.md` §3.13 |
| `--interp-auto [floor\|round]` | 关 | — | 每 40 帧按中位数算 `N+1 ≈ T_in/vblank`，运行时调 `SetNumInterpolatedFrames`，让输出回到整数 vblank 上（连续两个窗口稳定才改，改一次≈2.7 s） | 动态帧率、或想让输出"永远贴 refresh"时用；**会换模型 ⇒ 改档时有一次顿挫**（§9.14） |
| `--lock N` | 关 | 5–1000 | 在 bridge 里按周期**丢弃**抓取帧，使转发给 XeFG 的输入严格为 N fps（不需要 RTSS） | 只在「游戏 fps / N」是**整数**时才均匀（§9.16）；否则会出现 2T 空档，比不锁更差 |
| `--lock-auto` | 关 | — | 每窗口求解 `(N, 倍数)`：`N*(interp+1)` 整除刷新率且游戏能维持 N，然后自动锁 + 改倍数（2 窗口迟滞） | 游戏比目标快整数倍时最有用；26 fps@120 Hz 会明确拒绝（§9.16） |
| `--list-modes` | — | — | 打印当前分辨率支持的刷新率，以及每个倍数的精确输入 fps，然后退出 | 换分辨率/面板、或决定 RTSS 锁多少时先跑它（§9.15） |
| `--dyn-mfg` | 关 | — | **OptiScaler 的 Dynamic MFG（原版控制器）**：每真实帧按"presented fps vs 目标 + 学到的每帧成本"选 1x..6x，`1x` = 直接关掉 FG；改档有 hold/验证/退避 | 不锁帧、不浪费 GPU 时**最接近目标帧率**的通用方案（§9.17）；要整 vblank 仍需 VRR 或 §9.15 |
| `--fg-target N` | 刷新率-4 | 10–1000 | `--dyn-mfg` 的目标上屏帧率（120 Hz 默认 116，与 OptiScaler 默认一致） | 想更省就设低一点 |
| `--fg-round 0/1/2` | `1` | — | `--dyn-mfg` 的取整：`0` at most（不超目标）、`1` nearest、`2` at least | 与上游 `RoundingDMFG` 同义 |
| `--fg-allow-1x off` | 允许 | — | 禁止 `--dyn-mfg` 关掉帧生成（最低 2x） | 与上游 `DynamicAllow1X=false` 同义 |
| `--interp-cadence` | 关 | — | **变倍率生成**：按两个真实帧之间经过的 vblank 数逐帧设 `SetNumInterpolatedFrames` | ✅ **2026-11-08 复测有效**（空刷新 2.8–3.1% vs 纯 2x 的 5.0–6.7%）。⚠️ 2026-11-07 的"失败"结论是**到达间隔被墙钟偏差高估 20%** 造成的（见 `PERF_BASELINE.md` §3.7/§3.14），已撤回。必须配 `--interp 2` 才会打 MFG 补丁（`--interp 1` 时补丁跳过，倍率上限停在 1） |
| `--report N` | `40` | 5–400 | 统计窗口长度（帧） | 调小=更细，调大=更稳 |

### 4.3 XeLL（低延迟库）

| 开关 | 默认 | 实际效果 | 建议 |
|---|---|---|---|
| `--xell` / `--no-xell` | 日志显示 `no-sleep` | **目前只改启动那行 `mode:` 日志的文字**，不影响行为（`g_no_xell` 只在打印处被引用）。`xellSleep()` 每帧都会调，但它在 AMD 上是空实现 | 忽略 |
| `--xell-mode N` | `1` | 真正生效：传给 `xellSetSleepMode` 的 `bLowLatencyMode` | 只能是 `1`：设 0 会让 `SetEnabled` 返回 `ERR_LATENCY_REDUCTION_UNSUPPORTED (-15)` |

> **重要**：每帧的三条 XeLL 标记（`SIMULATION_START/END`、`RENDERSUBMIT`、`PRESENT`）
> **永远都发**，不受任何开关影响。这是我们踩过的最贵的坑（§5.1）。
>
> `--xell` / `--no-xell` 是个**半成品开关**（`g_no_xell` 没接进真正调用点），
> 别用它做实验，会得出错误结论；想验证睡眠有无影响，直接看 `xell=` 那一项的耗时。


### 4.4 自研 warp 插帧（`--fg warp`）调参

| 开关 | 默认 | 范围 | 作用 |
|---|---|---|---|
| `--warp-conf F` | `90` | 0–4000 | 前后向 MV 一致性灵敏度；越大越严格 |
| `--warp-search F` | `2` | 0–16 | 置信度不足时的局部块匹配半径（MV 纹素）；0 关闭 |
| `--warp-clamp F` | `1` | 0–1 | 用当前帧邻域夹紧插值结果；0 关闭 |

### 4.5 诊断用（不要留在日常配置里）

| 开关 | 作用 |
|---|---|
| `--no-fg` | 只建代理交换链、不启用 FG。用来把"颜色/显示问题"和"插帧问题"分开 |
| `--no-inputs` | 不导入跨适配器堆（不 tag MV/深度）。用来确认问题是否来自输入。**2026-11-06 实测 4K/DLAA：`gpu` 25.2 → 21.6 ms**，即 3.6 ms 全在跨卡搬运上 |
| `--no-depth` | 只跳过深度流（MV 仍然是真的）。深度占一半搬运量（2160p R32 = 33 MB/帧），只影响遮挡边界的 disocclusion；先当"省 ~1.8 ms"的折中试。**深度是唯一被消费的输入** ⇒ 这是仅剩的"省带宽但掉质量"开关 |
| `--sdr` | 颜色对照：不做 HDR10 编码，直接 scRGB→SDR 输出 |
| `--no-primaries` | 颜色对照：保留 PQ 但跳过 BT.2020 primaries 矩阵 |
| `--tearing` | `DXGI_PRESENT_ALLOW_TEARING`。实测不解除 `present` 阻塞 |
| `--mv-mul F` / `--mv-flip` | 强制 MV 缩放 / 翻转符号（游戏 MV 约定不同时用；CP2077 不需要） |
| `--log-flush` | 每行日志立即 `fflush`（默认按 200 ms 节流）。排查崩溃时用：进程被杀时缓冲区里那几行可能还没落盘（`PERF_BASELINE.md` §1） |

### 4.6 多帧生成（MFG，3x–6x）

| 开关 | 默认 | 作用 |
|---|---|---|
| `--unlock-mfg` | off | **解锁非 Intel 卡的 MFG**：给本进程里的 `libxess_fg.dll` / `libxell.dll` 各打几个字节的补丁（不改磁盘文件）。倍数取 `--interp N`；**没给 `--interp` 时默认 3（4x）**，给了 `--interp 1` 就真的只跑 2x（2026-11-06 修，之前会被默认值覆盖成 4x） |
| `--interp N` | `0` | 每个真实帧插 N 帧：`1`=2x、`2`=3x、`3`=4x、`4`=5x、`5`=6x。`0` = DLL 上限（=原来的行为）。`--interp 1` 不需要 `--unlock-mfg`（2x 本来就能跑），要 3x 以上才需要 |
| `--fg-lib <path>` | 同目录 | 指定 `libxess_fg.dll`（支持带引号的空格路径） |
| `--fg-probe` | — | 报告 DLL 路径/版本/`maxSupportedInterpolations` 后退出，**不需要游戏**；配合 `--unlock-mfg` 可直接验证解锁是否生效 |

**本机实测**（RX 6750 GRE + 官方 1.3.1.78，`--fg-probe`）：

```
不加 --unlock-mfg:  maxSupportedInterpolations 1  ->  2x output   VERDICT 2x only
加   --unlock-mfg:  maxSupportedInterpolations 3  ->  4x output   VERDICT MFG available
```

原理（4 处 `libxess_fg` + 1 处 `libxell` 的立即数/跳转）、代价（每帧插值都复用 2x 模型）
与风险，见 `docs/findings/MFG.md`。

```bat
<TOOLS_DIR>\probe_fg.bat                                  :: 同目录 DLL
<TOOLS_DIR>\run_mgpu_5k_mfg.bat                           :: 5K 锁 30 + 4x
```

### 4.7 XeFG 调优开关（对齐 OptiScaler 的同类选项）

> **2026-11-07 清理**：`--fg-of*`（光流，含 `units/sign/scale/guide/deadzone/depth-k/const/selftest`）、
> `--fg-hi-res-mv`、`--fg-jittered-mv` 都已从默认构建里移除（当时的理由是"MV 是死输入"，已被推翻，见 `CONCLUSIONS.md` §2）
> （§2.1.2 结论 1）。光流那一整套代码还在，但被 `MGPU_FG_OF` 宏关掉（默认 0），
> 需要复验时 `/DMGPU_FG_OF=1` 重编。详见 `HANDOFF.md` §8.6.1。

| 开关 | 默认 | OptiScaler 对应 | 作用 |
|---|---|---|---|
| `--fg-rt 0` | 关（用游戏帧间隔） | `[FrameGen] FTInput=2`（"only for XeFG"） | `frameRenderTime` 传 0。改的是逐帧节奏，收益/风险都未被证实（见 `docs/findings/MFG.md` §3.3），**不建议使用** |
| `--fg-ui none` | `AUTO` | `[XeFG] UIComposition=false` | `uiMode = UI_MODE_NONE`，不再探测我们从不提供的 UI 纹理。三次 A/B 显示差异在噪声内（`gpu` sd≈5.5 ms），但**零风险**（初始化期静态开关、我们本就不提供 UI 纹理），故保留 |
| `--fg-scene F` | 关（库内默认 0.7） | MFG-Unlocker 的 scene-decision detour | `xefgSwapChainSetSceneChangeThreshold(F)`；调大=更少"大角度转视角时插值帧变复制帧" |
| `--fg-depth-inv` | 关 | `[XeFG] DepthInverted` | 置 `INVERTED_DEPTH`（逆向/无限远 Z 深度）。**CP2077 是否适用未验证**，开了要立刻看画面 |

开任意一个都会在启动日志多打一行 `XeFG tuning: rt=.. ui=.. scene=.. depthInv=..`，
方便确认真的生效。

### 4.8 HDR 亮度

`sdrWhiteNits` 硬编码 **80**（scRGB 定义：1.0 = 80 nits），在转换 pass 的常量里
（`bridge.cpp` 的 `conv_constants()`，`cc[0] = 80.0f`）。觉得整体偏暗/偏亮就改这一个数，需要重新编译。

### 4.9 副卡选择（`--gpu` / `--producer-luid`，2026-11-07 新增）

旧版把副卡写死成"AMD 那张卡"（`VendorId == 0x1002`），非 A 卡直接 `no AMD adapter` 退出。
现在副卡由"能不能承载我们的交换链"决定，厂商判断只保留一处（**Intel 副卡不打 `--unlock-mfg`**）。
背景与后续里程碑见 `docs/design/VENDOR_COMPAT_PLAN.md`。

| 开关 | 默认 | 作用 |
|---|---|---|
| `--gpu <spec>` | 自动 | 强制副卡。支持 `0`/`1`（适配器序号）、`nv`/`amd`/`intel`、`10de`/`1002`/`8086`/`0x10de`、`luid:HI:LO`、描述子串（`6750`、`3080`） |
| `--producer-luid HI:LO` | 关 | 告诉 bridge 游戏渲染在哪块卡上，自动选择"另一块"。协议 v6 之后由 add-on 自动填，现在可手填 |

自动选择的顺序（日志会逐条列出适配器，并打印最终 `consumer adapter: ... - <理由>`）：

1. `--gpu` 显式指定；
2. **覆盖游戏窗口所在显示器的适配器** —— 这是硬约束，选错会 `CreateSwapChainForHwnd` 返回 `E_ACCESSDENIED`；
3. 不是生产端那块卡（需要 `--producer-luid`）；
4. 非软件适配器里专用显存最大的那块。

`--fg-probe` 现在会先打印适配器表、再打印 `adapter` 行（名字/厂商/device/LUID）与
`maxSupportedInterpolations (stock)`（与 `--unlock-mfg` 一起用时，能看到**打补丁前后**两个值）。

**实测（2026-11-07，本机 3080 Ti + 6750 GRE）**：

```
bridge.exe --fg-probe --gpu 10de
  consumer adapter: 'NVIDIA GeForce RTX 3080 Ti' (NVIDIA vendor=10DE) - --gpu
  probe: CreateContext SUCCESS (0)        <- XeFG 在 N 卡上建上下文成功
  probe: maxSupportedInterpolations 1 -> 2x

bridge.exe --fg-probe --gpu 10de --unlock-mfg --interp 3
  probe: maxSupportedInterpolations (stock) 1 -> 2x
  --unlock-mfg: all 6 sites patched and read back OK
  probe: maxSupportedInterpolations 3 -> 4x output      <- N 卡副卡同样能解锁
  probe: VERDICT MFG available, up to 4x

bridge.exe --fg-probe --producer-luid 0:10392
  consumer adapter: 'NVIDIA GeForce RTX 3080 Ti' (NVIDIA vendor=10DE) - not the producer
```

所以"N 卡当副卡"的**能力**已经确认（离"整链跑通"还差一次带游戏的实跑，见
`docs/design/VENDOR_COMPAT_PLAN.md` §4.3 组合 3）。

**协议 v6（2026-11-07，与上面同轮）**：共享块里多了"生产端是谁、输入是什么"：

| 字段 | 含义 | bridge 怎么用 |
|---|---|---|
| `producer.device_luid` | 游戏那块卡的 LUID | 选副卡时自动"避开生产端"（`--producer-luid` 可以退休）、同卡时明确告警 |
| `producer.input_api` | `ngx` / `ffx` / `xess` / `reshade-depth` | 只在日志里报（`api=ngx/dlss`），M6.3 起由它决定输入源 |
| `producer.input_flags` | 反向深度 / MV 在 NDC / 相机有效… | `kInputFlagInvertedDepth` 会**自动**置 `INVERTED_DEPTH`（等价 `--fg-depth-inv`）；`kInputFlagHasCamera` 让投影改用真相机 |
| `producer.render/output_w/h` | 超分输入/输出尺寸 | 日志 |
| `frames[].near/far/vfov` | 每帧相机（NGX 没有，填 0） | 有值时投影不再依赖 `--fg-fov`。**FSR 已实测**（PBZ：`camera=producer fov=46.7deg zn=10.00`）；反向 Z 游戏的 near/far 是**反着填**的，add-on 会先交换（见 §5.11） |
| `frames[].frame_time_ms` | 游戏自己的 present 间隔 | 一次性交叉校验（差 >35% 打一行提示；`frameRenderTime` 仍用我们的估算，§5.4） |
| `consumer_request`（v7） | 消费端→生产端 | bridge 每帧写；带 `--no-mv` 时 0 = add-on 完全不产出 MV 流 |
| `generation`（v8/v9） | 内容尺寸代次 | **切超分模式/档位不用重启，也不用任何握手**：add-on 只把新的内容矩形写进 `streams[i].width/height` 并把 `generation` 加一（`shared: content size is now AxB (gen N) - slots stay 3840x2160, nothing rebuilt`）；bridge 看到 `generation` 变化就重读尺寸、按需重建自己的**本地**副本（`producer published gen N: mv AxB`）。槽位/映射/堆/fence 全程不动，双端互不等待 |

新增/变化的日志（排查时按这个找）：

```
[bridge] shared block present: producer pid=.. luid=.. api=ngx/dlss flags=0x0 render=2560x1440 output=5120x2880
[bridge] frame constants: jitter=(..) proj=.. camera=synthetic (--fg-fov 58.7, --fg-zn 10.00) aspect=.. game_dt=33.20ms
```

> v6 要求 **add-on 与 bridge 同时重编/重部署**；只换一边会看到
> `shared block version 5, this bridge speaks 8 - rebuild/redeploy BOTH sides`。

---

## 5. 十条核心结论（踩坑清单，按价值排序）

每一条都是**花了很久才定位**的，改配置前先读一遍。

### 5.1 XeLL 的"标记"是 XeFG 的时钟，不是"睡眠" ⭐ 最贵

- **现象**：帧率数字完美（`xN=2.00`、输出 80fps），但**看着卡**，系统性顿挫。
- **根因**：我们把"跳过每帧 XeLL 调用"实现成了**同时跳过标记**，还设成默认。
  XeFG 靠每帧的 `SIMULATION_START/END`、`RENDERSUBMIT`、`PRESENT` 标记推断节奏；
  标记一缺，它只能退回最保守的呈现行为。
- **事实**：`xellSleep()` 在 AMD 上是**空实现**（`libxell.dll` 里明写 `No sleep algorithm available`），
  所以它本来就不该被当作必需项。参考 OptiScaler：`markers_only = true` + `forced_mode = true`。
- **做法**：**睡眠随便，标记必须发**（`bridge.cpp` 里标记调用不受 `g_no_xell` 影响）。
  每帧 `frame id == present id`。

### 5.2 WGC 帧池必须"排空取最新帧"

- **现象**：输入只有 37–39fps（应该是 40），`gpu` 5.9ms，输出到不了 80。
- **根因**：`TryGetNextFrame` 返回**最旧**的帧。配 `--pool 4` 就等于把池子深度变成延迟
  （最多 +100 ms），而且池子一直满 → WGC 自己丢帧。
- **做法**：一次 `TryGetNextFrame` 循环**排空**整个池子，只留最后一帧。
  改完：`gpu` 5.9→2.1 ms，输入回到 40，输出稳定 76–80。多出来的帧计入 `skip`。

### 5.3 不要自己配速（`--pace` 关闭）

- **现象**：`gpu` 2→17 ms，`load` 89%，掉到 50Hz。
- **根因**：XeSS-FG 已按标记自配速，且 `Present()` 内部要等约 15 ms 垂直同步。
  我们再钉一次时间 = 两者串行。
- **做法**：不传 `--pace`。`--pace auto` 也一样别用。

### 5.4 `frameRenderTime` 必须喂"游戏自己的帧间隔"

- **现象**：5K 下 `x2` 时不时掉到 1.0（插帧被跳过）。
- **根因**：之前喂的是**我们主循环**的间隔（约 30 ms，里面含 `Present` 的 15 ms 阻塞）。
  XeFG 以为没有余力 → 跳过插值。
- **做法**：在 WGC 出帧处单独测游戏帧间隔（EMA），喂给它。改完 5K 也稳 `xN=2.00`。

### 5.5 WGC 缩小帧池是"裁剪"不是"缩放"

- **现象**：画面像"严重拉伸"，其实只显示了左上角那一块内容。
- **根因**：把帧池设成 2560×1440，WGC 只给你窗口左上角的 2560×1440。
- **做法**：抓全尺寸（5120×2880），在**我们自己的 pass** 里降采样（`--den` / `--scale`）。

### 5.6 `InitFromSwapChainDesc` 不保留 `DXGI_SCALING_STRETCH`

- **现象**：非原生分辨率（1440p 模式）内容贴在左上角，不缩放。
- **做法**：非原生分辨率必须自己 `CreateSwapChainForHwnd`（带 `DXGI_SCALING_STRETCH`），
  再 `InitFromSwapChain` 移交所有权（记得 `Detach()`）。

### 5.7 `--wait-ms 0` 打破了"越慢越等"的死循环

- **现象**：5K 下 `skip` 11–17 帧、`wait` 5–8 ms。
- **根因**：我们慢 → 等游戏发布 MV 5–8 ms → 更慢 → 等得更久。
- **做法**：`--wait-ms 0`。`skip` → 0–2。

### 5.8 WGC 的"+1 帧"其实只有 3–6 ms，别去钩 Present

- **实测**：`age=` 直接量"拿到这帧时它已经多旧"：3.2 / 4.7 / 5.9 / 8.3 ms。
  因为 DWM 在 120 Hz 合成节拍上取用画面（最多 8.3 ms），不是整整一个游戏帧（25 ms）。
- **推论**：在游戏进程内钩 `Present`、自己拷后备缓冲，只省约 4 ms，
  却要把 59 MB/帧跨卡传（PCIe 点对点 = 已知的 NVIDIA TDR 来源）。**不值得，不要做。**

### 5.9 `Sleep(0)` 会冲过节拍最多 15.6 ms

- **根因**：Windows 默认定时器精度 15.6 ms。
- **做法**：`winmm` 的 `timeBeginPeriod(1)`（`enable_fine_timer()` 里动态加载，不需要改链接）。
  `--pace` 路径里剩下的最后 2 ms 才允许 spin。

### 5.10 80fps on 120Hz = 3:2 抖动，这是物理事实

8.33 / 16.7 ms 交替。解法：开 VRR，或者锁 30/60 这类整数比。不是代码能修的。

### 5.11 资源状态必须"按资源"记，不能只记最后一个（2026-11-07 崩溃的根因）

- **现象**：PBZ 从 DLSS 切到 FSR，进游戏 ~13 秒崩溃；`ReShade.log` 只有
  `Failed to close immediate command list with error code E_INVALIDARG`。
- **根因**：add-on 只保存"最后看到的那个资源 + 它的状态"（`g_depth_ptr/g_depth_state`）。
  DLSS 每帧交给 NGX 同一个深度纹理，所以一直够用；**FSR 是两个深度纹理交替**，
  指针一变我们就把它当成新资源、把状态重置成猜的值，随后对"其实是别的状态"的资源
  发 `ResourceBarrier` → `Close()` 返回 `E_INVALIDARG` → 游戏崩。
- **做法**：`game_capture.cpp` 换成 **8 条的固定表**（`handle/hint/state/seen/last_used`，
  无锁、`on_barrier` 按 handle 线性查找更新），并且：
  - **按最近使用淘汰**：表满时踢最久没被报告的（切换超分/档位会换新纹理）；
  - **闲置 >2 帧即作废状态**：只作废成"未知"（**不是**退回 hint），四条拷贝路径
    （`record_copy_tracked` / `record_downsample` / `record_copy_region` /
    `record_depth_convert`）统一按三态处理：`未知=不许碰`、`观测=真实状态`、
    `本帧刚报告=hint`。防止"旧纹理已释放、地址被新分配复用"时把新资源当成旧状态的（ABA）。
  - **不在表里就不发 barrier**：宁可丢一帧，也不发一个来路不明的 transition。
  **不要用 map**：当年无界 map + 全局锁在关卡加载的
  几万次 barrier 上把渲染线程饿死过（冻结、无 TDR、无报错）。
- **只有"切换算法模式"会崩、"切档位"不崩**（2026-11-07 深夜定位）：档位切换时同一个超分库
  还在被调用，每帧都有**新**纹理；模式切换（DLSS→FSR）后旧库**不再被调用、纹理被销毁**，
  而 `Source::poll()` 当时"只要曾经有过就永远返回 true"（serial 冻结也不管），于是我们反复
  用同一帧的旧纹理去拷/发 barrier。修法两条：
  - `poll()` 加 **serial 闸门**（`in_ngx.cpp` / `in_ffx.cpp`）：serial 没前进就是"没有新帧"；
  - 当前源连续 **90 帧**没新帧 → `input source … went quiet … re-arming` 自动释放，
    注册表在新 API 出数据时立刻接管。
  实测：来回切超分模式多次不再崩。
- **配套坑**：反向 Z 游戏（`DEPTH_INVERTED`）把相机距离**反着填**——`cameraNear=+inf`、
  `cameraFar=10`（真正的近平面）。`in_ffx.cpp` 先交换再判断，bridge 在 `far` 非有限时
  强制用 reversed-Z 形式（有限形式会除 inf 得到 NaN）。

---

### 5.12 消费者侧不能"一发现尺寸变了就释放本地副本"（XeFG 还握着裸指针）

- **现象**：协议 v9 上线后第一次切内容尺寸，bridge 立刻打出
  `local input copy FAILED hr=0x887A0005`（`DXGI_ERROR_DEVICE_REMOVED`）+
  `[xefg error] Resource pointer is null` + `Missing resource of type XEFG_RES_MOTION_VECTOR`，
  之后整条链全黑（游戏侧完全正常，`gen` 一直涨）。
- **根因**：XeFG 把上一帧我们 tag 进去的资源**裸指针**留在自己的队列里；
  bridge 在同一时刻 `Reset()` 了 `g_mv_local/g_depth_local/g_depth_ring` → GPU 侧的
  XeFG 解引用已释放的显存 → 消费者设备被移除（同时伴随一次 ~3 s 的 TDR）。
- **做法**：**retire 队列**（`retire_input()` → `g_retired_inputs`），旧副本保留
  `kRetirePresents=16` 帧后才真正释放；`collect_retired_inputs()` 每帧清理。
  这样不需要任何 fence 握手，也不影响解耦（纯消费者内部记账）。

### 5.13 信用窗口的"消费者还在不在"必须看 ACK 是否推进，不能看"有没有 ACK 过"

- **现象**：v10 第一版上线后，bridge 一退出游戏就掉帧（add-on `stats` 里 `ov` 疯涨，实测 **39% 的发布被丢**，`q` 卡在 3/4）。
- **根因**：判断"有没有消费者"用的是 `any_ack == 0`，但 `consumed[]` 在 bridge 退出后**保留旧值**，
  条件永远不成立 ⇒ 生产端以为每个槽都还被占着，于是永久丢帧。
- **做法**：`update_credit_state()` 看 **ACK 和是否还在变**：连续 120 帧不变 = 消费者已走，打印
  `credit window OFF (falling back to round-robin)` 并恢复旧的轮转行为；一旦 ACK 又开始推进，
  立刻回到 `credit window ON`。实测：无消费者时 `ov` 冻结在 115，有消费者时 55 秒只丢 3 次（0.05%）。

### 5.14 作用域守卫（RAII）带引用成员，把彩色纹理在用到之前就释放了 —— "全黑但一切正常"

- **现象**：起 bridge 后**整屏黑**，但 `presented=3 fg=1 SUCCESS`、`show=119`、`xN=3.00`、
  add-on `record=0.03ms / skipped=0`、`input geometry` 全部正常；OSD 照常显示数据。
  换目标（Notepad）、`--no-fg`、`--no-sdr/SDR`、去掉 `--overlay-layered`、`--tearing` 全黑。
- **定位过程**（这套办法以后直接复用）：
  1. **GDI 截屏**判黑白（`CopyFromScreen` + 统计亮度）：起 bridge 时均值 0.5，不起时 154 → 客观确认黑；
  2. 给 bridge 加 **`--probe-colour N`**：把共享彩色纹理的 64×36 个采样点回读统计，
     得到 `0/2304 samples non-zero <== BLACK CAPTURE` → 问题在**采集侧**，不在上屏侧；
  3. 在 push / pop / dispatch 三处打点，发现 `pop` 时 `texture=0x…` 有效，
     `dispatch`（同一帧、十行之后）时已经是 `nullptr`。
- **根因**：`--jbuf` 的回收写成作用域守卫：
  ```cpp
  struct JbufReturn { ComPtr<ID3D11Texture2D>* t; decltype(jbuf_give_back)& give;
                      ~JbufReturn() { give(*t); } } jbuf_return{&texture, jbuf_give_back};
  ```
  它在**记录命令之前**就把 `texture` 释放了 → `run_frame(nullptr, 非复用帧)` →
  `CopyResource` 被跳过（我加的判空检查打了 `colour11 is NULL`）→ sink 永远保持初始黑色。
- **做法**：删掉守卫，改成 `run_frame(...)` 之后**显式** `jbuf_give_back(texture)`。
  实测：`colour copy: src 3840x2160 fmt 10 -> sink 3840x2160 fmt 10 (match)`，
  `probe: 2304/2304 samples non-zero (100.0%) ok`，截屏均值回到 115/129/110。
- **教训**：所有计数器都对得上、只有画面黑时，先量"**我们拿到的像素**"而不是猜链路。
  `--probe-colour N` 现在是常备诊断项。

> **MV 三项验证（2026-11-08 完成）**：① 被消费 —— `--mv-shadow` 三组对照，喂真值比喂零
> 多花 **7.4 ms**；② 方向正确 —— `--mv-flip` 反向**明显更差**；③ 有画质收益 —— 2x 下开 MV
> 模型边缘伪影减少。配套必须**透传游戏 jitter**（传 0 会闪烁）。
>
> **画质优先配方（2026-11-08 实测）**：**2x + 游戏 MV + 透传游戏 jitter**。
> 相对 3x：模型边缘伪影明显减少；相对 2x+关 MV：边缘伪影进一步减少；
> 相对 2x+MV+`--fg-jitter-zero`：闪烁消失。代价是 2x@40 只有 80 fps 输出（`xN=2.00`），
> 120 Hz 面板下不是整拍 —— 想同时要整拍，就把游戏锁到 60 fps（2×60=120）。

### 5.15 3x 的"人物边缘拖影"来自 MFG 补丁，不是 MV/深度/配对（别再往那边找）

- **现象**：**转动视角时人物模型边缘有拖影**（静态时不明显）。
- **排除过程（2026-11-08，PBZ 1080p，全部实测）**：
  | 变量 | 结果 |
  |---|---|
  | 真 MV vs 零化 MV（`--mv-shadow` 已证 XeFG 会消费 MV，+7.4 ms） | 拖影**不变** |
  | `--depth-lag 1` / `--no-depth` / 现状 三组 | 拖影**基本一致** |
  | 只把倍率 3x → 2x | 拖影**明显改善** |
- **根因**：这个 `libxess_fg 1.3.1` **没有真正的 MFG 模型**（`docs/findings/MFG.md`），3x 是 `--unlock-mfg`
  放行后"把 2x 模型用两遍"：bridge 启动日志自己就写着
  `every generated frame uses the 2x model ... so quality is not 2x quality`。
  第二个插值帧的误差在快速转动时被放大，正好表现为模型边缘拖影。
- **结论/选择**：
  - **要运动画质** → 用 **2x**，并且把输入锁到 **60 fps**（2×60=120，正好整拍）；
    实测 2x@40 只有 80 fps 输出，在 120 Hz 上会有 3:2 抖动（§5.10）。
  - **要满 120 fps 且能接受拖影** → 继续 3x@40（当前定稿配方）。
  - 与 `--no-mv` 无关：开 MV 只多花 7.4 ms 消费端 GPU，**不会改善这个拖影**。

### 5.16 合成投影（FOV/near/far）对画质没有任何影响 —— 别再猜 FOV 了

- **实测（2026-11-08，PBZ 2x + 真 MV，1080p）**：`--fg-fov 30` / `58.7` / `120` 三组
  （日志确认 `camera=synthetic (--fg-fov 30.0 / 58.7 / 120.0)` 都生效、`xN=2.00`）
  **画面/伪影完全一致**。
- 结合上一条（`--no-depth` 画质不变）⇒ **这个 `libxess_fg 1.3.1` 的 FG 只用 MV 做重投影，
  不读深度、也不读我们喂的投影矩阵**。
- 含义：
  - `--fg-fov` / `--fg-zn` / `--fg-proj-rz` 对 **XeFG 帧生成画质** 无意义（DLSS 档不用再猜 FOV；
    FSR 档虽然能拿到真相机，但对 FG 也没用）；
  - 它们**仍然影响我们自己的路径**（`--fg-warp` 的双帧 warp、`--fg-of` 的光流），那些代码真的用投影/MV；
  - 想真正改善画质的旋钮只剩两个：**MV 开/关** 与 **jitter 怎么传**（见 §2.1.2 第 1/3 条）。

### 5.17 3x/4x + 原生 MV 的闪烁是 MFG 补丁的固有限制（丢帧可以治，闪烁不能）

**先纠正上一版的说法**：最初以为闪烁只是"预算不够丢帧"，实测证明**两件事是分开的**。

| 运行（3x/4x + 原生 MV @4K） | `xN` | `gpu` | `show` | `skip` | `slow` | 观感 |
|---|---|---|---|---|---|---|
| 3x，默认（实测间隔 25 ms） | 3.00 | 16.9 | 115 | 19 | 17 | drop 多 + **闪烁** |
| 3x，`--input-ms 33` | 3.00 | 13.8 | 118 | **13** | **0** | **无 drop** + **仍闪烁** |
| 4x@30，`--lock 30` | 4.00 | 25.6 | 118 | 168 | 46 | drop 多 + **闪烁** |

- **丢帧（`skip`/`slow`）可以治**：`--input-ms 33` 让节拍器知道每帧有 4 个 vblank 的余量，
  `slow` 17→0、`skip` 19→13，延迟不变（§5.18）。
- **闪烁不能治**：三组丢帧数差 10 倍，闪烁程度**一样**。它来自 `3x/4x + 原生 MV` 这个组合本身 ——
  这个 DLL 没有真正的 MFG 模型（`docs/findings/MFG.md`），3x 是"2x 模型跑两遍"，第二个生成帧拿同一份 MV
  去算另一个时间分数，重投影不自洽。4x（模型跑三遍）更差。
- **唯一干净带 MV 的配置是 2x**：PBZ 2x@40 实测"伪影减少 + 无闪烁"（配套必须透传游戏 jitter，§2.1.2 第 3 条）。


### 5.18 `--input-ms X`：把"输入间隔"当成 X ms（让节拍器别过度丢帧）

节拍器用的是**实测到达间隔** `g_arrival_ms_ema`（40 fps → 25 ms）。这个值既决定 `slow`/`load`
的判定，也作为 `frameRenderTime` 喂给 XeFG。真给它多一点余量，XeFG 就不再因为"挤不下 3 个
vblank"而丢掉生成帧。

| 3x + 原生 MV @4K | `gpu` med | `total` | `show` | `skip` | `slow` | compose→photons | click→photons |
|---|---|---|---|---|---|---|---|
| 默认（实测 25 ms） | 17.5 | 22.8 | 116 | 107 | **120** | 27.5 | 54.0 |
| **`--input-ms 33`** | **11.0** | **15.9** | **119** | **14** | **0** | 27.8 | 53.8 |

**延迟一分没涨**，`skip`/`slow` 却几乎归零 —— 纯赚。**但它不会凭空造出 GPU 时间**：
如果工作量真的需要 27 ms 而槽位只有 25 ms，帧还是会晚，只是不再被计成 `slow`。

**两种用法**：① 游戏帧节奏不稳（22–28 ms 抖动）时当成稳定值；② 给 XeFG 节拍器留余量。
**真正"变大"输入间隔**（少用真实帧、每帧更多时间）用现成的 `--lock N`：例如
`--interp 3 --lock 30` = 4x@30 = 120 fps 输出、真实 33.3 ms 间隔。

### 5.18.1 `--input-ms` 的真实作用：它是"容忍迟到"的旋钮（3x 建议 33.3）

用户实测（PBZ，**3x + 关 MV**，4K）：把输入间隔从"自动（实测 25 ms）"改成 **33.3 ms**，
**画面明显更平滑流畅**；数据支持：`slow` 17→**0**、`skip` 19→**13**，延迟不变。

**为什么理论值反而更差**：3 vblank = 25.0 ms 意味着"每帧必须刚好卡满 3 个 vblank"，而我们自己的
`throttle + present` 偶尔会超一点点，XeFG 就判"挤不下"并丢掉一个生成帧。
告诉它"有 4 个 vblank"后，它容忍这点迟到、不再扔帧 —— 观感更顺，而**实测延迟并没有变差**。

**推荐值**：`--input-ms` ≈ (interp + 2) × vblank。120 Hz 下即 8.33 ms × 倍数：

| 倍数 | 理论间隔 | 建议 `--input-ms` |
|---|---|---|
| 2x（interp 1） | 25.0 ms | 33.3 ms |
| 3x（interp 2） | 25.0 ms | **33.3 ms** ← 实测最顺 |
| 4x@30（interp 3） | 33.3 ms | 41.7 ms |

**再松一档也不变差**（3x 关 MV @4K 实测）：`--input-ms 33.0` → `slow=0`/`skip=7`/click 48.8 ms；
`--input-ms 41.7` → `slow=0`/`skip=6`/click **46.0 ms**（差异在噪声内）。

**UI 默认已经是"自动 = (倍数 + 2) × vblank"**（120 Hz 下 2x/3x → 33.3 ms，4x → 41.7 ms），
也可以手动选固定档（16.7 / 20 / 25 / 33 / 41.7 ms）。

### 5.19 AMD 光流（`--fg-of`）恢复启用：数据来源与成本（2026-11-08）

早先因为"MV 是死输入"的结论，整条光流被 `#if MGPU_FG_OF 0` 编掉了；该结论已被推翻（§2.1.2 第 1 条），
所以**重新开启**（`src/bridge/build.bat` 加 `/DMGPU_FG_OF=1`），并作为 UI 的第 4 个 MV 选项。

**它到底用什么数据（与 OptiScaler 那套不同）**：

| | OptiScaler 的 FG 输入方案 | 我们的 `--fg-of` |
|---|---|---|
| 运动估计 | 重投影矩阵 + 深度 | **只用彩色**：`FfxOpticalflowContextDispatch(g_shared12)` |
| 重投影矩阵 | 需要 | **完全不读**（不读相机、不读 `--fg-fov`） |
| 深度 | 参与重投影 | **只**作为边缘引导上采样的权重（`--fg-of-depth-k`，默认 40），来源是**本地深度 ring**＝**游戏原生深度** |

**成本实测（PBZ 4K，2x）**：`of=0.3 ms`（流场 480×270 → 上采样到 2228×1253 与深度流匹配），
对比"拷游戏 MV"在消费端 +7.4 ms —— 光流便宜一个数量级。

**什么时候值得用**：不只"游戏不给 MV"时 —— **3x 下它就是首选**（见下）。

**3x + AMD 光流实测（PBZ 4K，2026-11-08）**：

| 运行（3x、`--input-ms 41.7`） | `gpu` | `of` | `show` | `skip` | `slow` | click→photons |
|---|---|---|---|---|---|---|
| 关 MV | 6.3 | — | 118 | 6 | 0 | 51.4 ms |
| **AMD 光流** | 9.0 | **0.3** | **119** | **5** | 0 | **44.5 ms** |

观感：**比关 MV 更干净，且完全没有 3x+游戏MV 的闪烁**。
原因：光流是从**已解析的彩色**里估矢量，与彩色同源；而游戏的 MV 是在**抖动后的画面**上算的，
3x 下第二个生成帧复用它时那个亚像素错位被放大成闪烁。

**选择规则（最终形态）**

| 场景 | MV 来源 |
|---|---|
| **3x（120 fps 整拍，4K）** | **AMD 光流**（`--fg-of`）← 首选 |
| 2x + 游戏 60 fps | 游戏 MV（同帧、无估计误差） |
| 游戏用 TSR / 原生 TAA / XeSS 无深度 | AMD 光流（唯一来源） |
| 追求最低消费端开销 | 关 MV（但快转时模型边缘更脏） |
**自检**：`bridge.exe --fg-of-selftest` → `FidelityFX optical flow at 3840x2160, field 480x270 ... OK`。

## 6. 日志判据

### 6.1 日志在哪

| 日志 | 位置 | 内容 |
|---|---|---|
| bridge（性能） | `<TOOLS_DIR>\bridge_run.txt` | 启动参数、每 40 帧两行统计、`HITCH`、`[xefg ...]` |
| bridge（5K） | `<TOOLS_DIR>\bridge_5k.txt` | 同上 |
| 游戏侧 add-on | 游戏 `bin/x64/game_capture.log`（**默认不写**，`[capture] Log=1` 才生成） | `config: Color=`、NGX 钩子、共享内存就绪、每帧统计 |

### 6.2 每 40 帧打印的几行

```
[bridge] frame 841  slot 0  jitter=(0.245, -0.430)  (34.5 fps avg, 39.8 recent)  presented=2 fg=1 SUCCESS  hittest=0 fgwin=1 present=0ms bb=2560x1440 show=80
[bridge]   breakdown: capture=0.0ms work=0.5ms present=3.6ms gpu=2.1ms wait=0.0ms copy=0.4ms mvs=1.5ms age=4.7ms xell=0.0ms fg=5.2ms total=7.3ms timeouts=0 mode=0  xN=2.00  game=25.1ms  jitter=24.9..25.3ms  wgc_drops=0  load=29%  slow=0  pace=0.0ms repres=0 skip=0  worst(gpu 5.3/pres 10.7/...) xNmiss=0
```

| 字段 | 含义 | 正常值 |
|---|---|---|
| `recent` | 40 帧窗口内的**瞬时**输入帧率（`avg` 是累计值，启动阶段会拖低它，别信 `avg`） | 贴着锁帧值 |
| `game` | WGC 帧到达间隔 = **我们拿到帧的间隔**，不是游戏自己的帧率 | 锁 30 时 33.0–34.0 ms。**注意**：bridge 自己成为瓶颈时（GPU 满、`present` 阻塞），这里显示的是**我们的消费速率**，会被误读成"游戏只有 13fps"；判据见下面 §6.2.1 |
| `presented` / `xN` | 每次 Present 实际输出的帧数 / 窗口平均值 | `2` / `2.00` |
| `fg` + `SUCCESS` | FG 是否启用 + `frameGenResult` | `1 SUCCESS` |
| `gpu` | 等上一帧 GPU 完成的耗时 = **真实 GPU 单帧**（1440p 3–5 ms / 5K 4–20 ms） | 越低越好 |
| `present` | `Present()` 阻塞（= XeSS-FG 内部对齐垂直同步） | 0–16 ms，**不是 bug** |
| `total` | 单帧主循环总耗时 | 7–17 / 16–33 ms |
| `wait` | 等 producer 发布 MV（`--wait-ms` 超时） | 0（用了 `--wait-ms 0`） |
| `mvs` | 从共享内存拷 MV/深度到本地 committed 资源 | ≈1.5 ms |
| `age` | 拿到 WGC 帧时它已多旧 = DWM/WGC 固有延迟 | 4–6 / 10–14 ms |
| **`skip`** | **唯一可信的真丢帧指标**（池子里被新帧覆盖掉的 + pacing 掉的） | 1440p `0`，5K `0–2` |
| `wgc_drops` | 按时间戳判定的可疑丢帧（间隔 >1.6×EMA） | 0 |
| `slow` | GPU 单帧没能装进输入间隔的帧数（"超预算"计数，比 `load` 更实） | 0 |
| `load` | `(gpu + present) / 游戏帧间隔` | 峰值可能贴顶，**不作为判据** |
| `xNmiss` | 窗口内出帧数低于配置倍数的次数（FG 降级） | `0` |
| `interp`（mode 行） | 请求的插值帧数（0 = DLL 上限） | `0` 或你要的倍数 |
| `maxSupportedInterpolations`（启动行） | **这个 DLL 在这台机器上允许的上限**；1 = 只有 2x | 1（官方 DLL）/ 3（MFG DLL） |
| `timeouts` | `--wait-ms` 超时次数。`--wait-ms 0` 下它**与帧数同量级是正常的**（表示那一刻还没等到新 MV，复用了上一槽；`wait` 仍是 0） | `--wait-ms`>0 时 `0`；`--wait-ms 0` 时可到 ~100/窗口 |
| `mode` | 颜色模式（0 = scRGB→BT.2020 PQ，1 = `--no-primaries`，2 = `--sdr`） | 0 |
| `HITCH` | 单帧异常（`gap`/`gpu`/`pres` 超过期望间距 2 倍的即时打印） | 偶发，见 §9 |
| `LATENCY`（两行） | 端到端延迟拆解：`compose->photons` 与 `est. click->photons`，详见 §6.3 | 4x@30 约 40–48 / ≥73 ms |

### 6.2.1 `game=` 到底在说什么（很容易误读）

`game=` 是 **WGC 帧送达到我们手里的间隔**（`g_arrival_ms_ema`），取的是"我们上一次拿帧到这一次拿帧"的时长。
所以它同时受两头限制：

```
game ≈ max( 游戏真实帧间隔 , 我们自己的消费间隔 )
```

bridge 一旦成为瓶颈（`gpu` 顶到间隔、`present` 阻塞占掉大半），这个数就会等于**我们的消费速率**，
看起来像"游戏只有 13–15 fps"，其实游戏可能跑到 30+ fps，只是我们把帧挤掉了。

判别顺序（2026-11-06 在 59 Hz 面板上踩过）：

| 现象 | 说明 |
|---|---|
| `game=66ms` + `present` 占 `total` 的 40% 以上 + `load≈98%` | **我们自己/呈现端在限速**，不是游戏慢 |
| 面板 59/60 Hz、`xN=4.00`、输入 ~15 fps | 数学上必然是 `refresh/(xN) = 59/4 ≈ 14.8`；4x 在 60 Hz 面板上只能换来"输入被砍到 1/4" |
| `game=33ms`、`load<90%`、`skip≈0` | 这才是游戏真的在 30 fps，链路健康 |

对应动作：面板不到 120 Hz 就把 `--interp` 降到 1（2x），让 `30×2=60` 正好填满 60 Hz；
或者先把显示模式/线材（DSC、HDMI 2.1）搞定再上 4x。bridge 会在**面板 <100 Hz 且 Present
占掉大半时间**时打一行 `note: Present blocks ... so the display side paces the loop.`
来点明这件事（120 Hz 的正常 4x 配置不会打 —— 那里输入本来就该是 30 fps）。

### 6.3 延迟：怎么读，怎么"真测"

每个统计窗口现在多打两行（数据全部来自我们已有的时间戳，**不挂钩 Present**，所以不扰动呈现）：

```
[bridge] LATENCY: compose->present 39.9ms (game->wgc 8.4 + bridge 31.5) | +scanout 0..8.3 => **compose->photons 39.9..48.2ms**
[bridge]          + game input->compose >= 33.3ms (1 input frame, not measurable here) => **est. click->photons >= 73.2ms**
```

| 项 | 含义 | 来源 |
|---|---|---|
| `game->wgc` | 游戏合成完 → 我们拿到这一帧 | WGC 的 `SystemRelativeTime` 与本地时钟之差（= 原来的 `age`） |
| `bridge` | 我们这帧的总耗时（含 `Present` 等 vblank） | `total` |
| `+scanout` | 批次里最新那帧到像素发光，0…一个刷新周期 | 由 `display mode` 的刷新率算 |
| `game input->compose` | 游戏自己的"点击→画面合成" | **测不到**（需要点击时间戳），只能给下限：≥1 个输入帧 |

**为什么不能在软件里测"真正的点击→像素"**：我们只能看到"帧已经合成好之后"的时刻；
点击发生在游戏输入线程里，没有时间戳传出来。所以任何软件数字都是"合成→像素"或它的代理。

**要真测，只有两条路**：

| 方法 | 能测到什么 | 备注 |
|---|---|---|
| **LDAT / 光电二极管 + 高速相机**（鼠标点击与屏幕发光做硬件对齐） | 真正的 click→photon | 唯一"真实"的方法；对我们的覆盖层输出同样有效（它只看屏幕上的光） |
| NVIDIA Reflex Analyzer | click→photon | **不适用本方案**：它依赖游戏自己的交换链（Reflex）上报画面，我们上屏走的是 bridge 的交换链，它看不到 |
| FLM / 各类 Present 打桩工具 | 队列内延迟的近似 | **会改变被测系统**（见 `docs/findings/MFG.md` §3.3）：`present` 掉到 ~2ms、4 帧成串推，测出的数和体感都不再是日常状态 |

所以日常判断用日志里的 `LATENCY` 行（零扰动），需要验收时用 LDAT 做一次硬件对照。

### 6.3.1 实测与"哪一块能优化"

**4x@1440p（锁 30）实测（54 窗口，2026-11-06 10:54）**：

| 指标 | p50 | mean | min | max |
|---|---|---|---|---|
| `game->wgc` | 9.2 | 11.2 | 4.5 | 98.0（首窗） |
| `bridge` | 31.4 | 28.4 | 10.2 | 36.1 |
| compose→photons | **39.8–48.0** | 39.6–47.9 | 26.0–34.3 | 108–117（首窗） |
| est. click→photons（下限） | **73.2** | 73.3 | 59.9 | 134.3 |

这组数字与上一轮纯由 `age + total` 推算的 39.9 / 73.2 **几乎完全一致**，说明模型可信。

延迟的四大块，以及谁该被优化：

```
est. click->photons ≈  游戏输入采样(≥1 输入帧)  +  game->wgc  +  bridge  +  scanout
4x@1440p 锁30        ≈  33.3                    +  9.2        +  31.4    +  0..8.3 = 73..82 ms
3x@1440p 锁40        ≈  25.0                    +  ~7         +  ~20     +  0..8.3 = 52..60 ms  ← 同样 120fps
1440p 2x 锁40        ≈  25.0                    +  ~5         +  ~11     +  0..8.3 = 41..49 ms  （80fps）
```

- `bridge` 那 31.4 ms **绝大部分是必须的**：4 帧要在 33.3 ms 里铺满，`present` 等的就是 vblank；
- 真正能减的是**第一项（输入帧率）**。而 40fps 输入时 **3x 正好 = 120fps**（40×3），
  节拍同为 1:1，延迟却少一个输入帧间隔（约 −20 ms）→ `run_mgpu_3x_40.bat`；
- `game->wgc` 由 DWM 决定，不值得动（`docs/findings/MFG.md` §3.3）。

### 6.3.2 长跑实测（4x@4K/30，167 窗口 / 6641 帧 / ~220 s）

| 环节 | p50 | p90 | p99 | max |
|---|---|---|---|---|
| `game->wgc`（DWM hop） | 11.8 | 17.3 | 38.9 | 106.1 |
| `bridge`（我们整帧） | 31.2 | 34.0 | 36.7 | 47.3 |
| **compose→present** | **43.0** | 49.7 | 86.2 | 116.6 |
| compose→photons | 43.0–51.4 | 49.7–58.1 | 86.2–94.5 | — |
| **click→photons（下限）** | **77.4** | 84.7 | **137.3** | 142.6 |

**关键结构性结论**（这条比任何单项数字都重要）：

```
compose→present ≈ game->wgc + 输入间隔     43.0 ≈ 11.8 + 33.3 - 2
```

因为 `total` 会被 vblank 配速**饱和到输入间隔**，所以：

- **只要"装得下"，延迟就由「输入间隔 + DWM hop」决定，与插值分辨率无关**
  （1440p 4x 的 compose→present 是 40.5，4K 是 43.0 —— 差的就是 `age` 那 2 ms）；
- 想降延迟只有三条路：**① 提高输入帧率**（33.3→25 ⇒ −8.3 ms）、
  **② 降倍数**（4x→3x 在 120Hz 上少一个 vblank 槽 ⇒ −8.3 ms）、③ 降 DWM hop（不可控）。

两组实测正好验证了 ①+②：`3x@4K@40` 的 `click→photons` 是 **64.9**，比 `4x@4K@30` 的 77.4
低 **12.5 ms**（≈ 8.3 + 4）。代价是 4K 装不下 3x@40（106 fps）；若把插值降到 1440p 就能装下，
预计 `click→photons ≈ 55–58 ms` 且 120 fps —— 这是目前唯一还没试的低延迟组合。

**尾部**：`p99 86.2` vs `p50 43.0`，来源是 `age` 的尖峰（p99 38.9、max 106），
即 DWM/WGC 的偶发停顿，不是我们链路，也不值得追。


### 6.4 排查顺序（照这个顺序看，不要乱看）

```
1. mode 行        参数对不对？den/scale/pool 是不是你以为的
2. recent         输入帧率贴不贴锁帧值 → 不贴 = RTSS 没生效 / WGC 在丢帧
3. xN / xNmiss    插帧有没有降级 → 降级 = frameRenderTime 或标记出问题
4. skip           真的丢帧吗 → skip>5 说明预算不够（降 --scale 或降锁帧）
5. gpu / present  预算花在哪 → gpu 大 = FG 模型算力，present 大 = 库内 vsync 对齐
6. age / LATENCY  颜色链路与端到端延迟 → age 是固有项（§6.3.2），别在这里优化（§5.8）
```

**判据结论**：`load` 贴顶不算问题，`skip` 才是。`present` 15 ms 也不是问题。

---

## 7. 故障排查速查

| 症状 | 最可能的原因 | 动作 |
|---|---|---|
| `[bridge] game window not found`（退出码 2） | 游戏没开 / 不是窗口化 | 先进游戏世界再启动 bridge |
| 画面全黑 / 只有覆盖窗口 | 覆盖窗口没拿到画面 / 焦点问题 | 看 `show=` 是否为 0；确认游戏是**无边框窗口** |
| 画面发灰、一帧正常一帧错 | 只写 `GetBuffer(0)`，第二块缓冲是旧帧 | 必须用 `GetCurrentBackBufferIndex()` + 每 buffer 独立状态（已修） |
| 内容贴在左上角、不缩放 | 交换链缺 `DXGI_SCALING_STRETCH` | 见 §5.6 |
| **画面整体被拉抻/缩放**（4K 桌面却像 5K 缩下来） | 抓取尺寸 != 窗口尺寸：共享块里 `streams[0]` 的尺寸是 add-on 的**固定占位值 5120×2880**，`Color=0` 时它根本不分配 | 已修：`bridge` 现在以 **WGC item 尺寸**为准（日志 `capture size taken from the window: 3840x2160 (shared block declared 5120x2880)`）；再看到 `WARNING: present size ... != window client ...` 就是 `--den/--scale` 与桌面没对齐 |
| `capture pool` 比 `capture item` 大 | 同上（池是按占位尺寸建的） | 同上，修好后两者必然相等 |
| `show` 卡在 58–60、`display mode: ... @ 59 Hz` | **面板在 59/60 Hz**，4x@30 要 120 Hz 才有意义 | 把显示模式改成 3840×2160@120（4K HDR 120 需要 DP1.4+DSC 或 HDMI2.1）。bridge 现在会打 `WARNING: panel runs at 59 Hz ...` |
| `[xefg error] Backbuffer format is not valid. Found 40.` | 40 = `R16G16B16A16_FLOAT`，**XeSS-FG 不支持 FP16 / scRGB** | 交换链必须 `R10G10B10A2_UNORM` + `RGB_FULL_G2084_NONE_P2020`（HDR10） |
| `Failed to allocate memory for a copy of DEPTH` + 缺 `MOTION_VECTOR` | 直接把跨适配器 placed 资源 tag 给 FG | 先拷到本地 committed（`DEFAULT` heap、`LAYOUT_UNKNOWN`）再 tag |
| `local input copy FAILED hr=0x80070057` | 复制源 desc 带着 `LAYOUT_ROW_MAJOR` | 本地副本改 `LAYOUT_UNKNOWN`（§同上） |
| `InitFromSwapChainDesc: ERR_INVALID_ARGUMENT (-4)` | 进程非 DPI-aware，客户区被虚拟化 | `SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2)`（已在 `main()` 第一行） |
| `CreateSwapChainForHwnd: E_ACCESSDENIED` | 本机不接受在自建窗口类/顶层弹窗上建交换链 | 先用普通类（`STATIC`）窗口建，建好再改无边框 + topmost |
| `SetEnabled: -15` / `ERR_LATENCY_REDUCTION_UNSUPPORTED` | `bLowLatencyMode=0`，或 `SetLatencyReduction` 调在 `InitFromSwapChain*` 之后 | 保持 `--xell-mode 1`；`SetLatencyReduction` 必须在初始化**之前** |
| 帧率数字漂亮但看着卡 | XeLL 标记没发（§5.1） | 检查 `xell=on(mode 1)` 与标记调用未被跳过 |
| 输出只有 50–60Hz、`gpu` 十几 ms | 加了 `--pace`（§5.3） | 去掉 `--pace` |
| 输入到不了锁帧值、`skip` 很大 | 帧池取了最旧帧（§5.2） / `--wait-ms` 太大 | 排空取最新；5K 用 `--wait-ms 0` |
| `xN` 反复掉到 1.0 | `frameRenderTime` 喂的是主循环间隔（§5.4） | 用 WGC 出帧处测的游戏帧间隔 |
| 鼠标不动 / 点不到游戏 | 覆盖窗口抢了焦点 | `WS_EX_TRANSPARENT｜NOACTIVATE｜TOOLWINDOW` + `WM_NCHITTEST→HTTRANSPARENT`（已修） |
| 任务视图/Alt+Tab 出现两个窗口 | 镜像窗口是独立顶层窗口 | 加 `WS_EX_TOOLWINDOW`（已修） |
| 白屏 + "程序无响应" | 主循环没有消息泵 | 每帧 `PeekMessage`/`DispatchMessage`（已修） |
| 5K 偶发掉帧、`load` 贴顶 | 余量接近零 | §9.2 的缓解清单 |
| 日志说 `maxSupportedInterpolations = 1` / `MFG unavailable` | 没加 `--unlock-mfg`（官方 DLL 在非 Intel 卡上只给 1 个插值帧） | 加 `--unlock-mfg`；先用 `probe_fg.bat --unlock-mfg` 确认变成 3 |
| `--unlock-mfg FAILED: <site>_unexpected_bytes` | DLL 版本换了，字节表对不上（有意保护，不会半改） | 用 `temp/check_sites.py` 重新比对，或换回 1.3.1.78 家族的 DLL |
| `--interp 3` 没效果、`xN=2.00` | 同上（没解锁），或 `libxell.dll` 太旧（`[xefg warn]` 会写 "falling back to 2x"） | 加 `--unlock-mfg` |
| `[xefg error] Network model does not support requested multi frame generation` | 这个 DLL 的模型不支持该倍数 | 降 `--interp` |
| `libxess_fg.dll NOT loaded: ...`（退出码 11） | DLL 不在 exe 目录 / `--fg-lib` 路径错 / 缺依赖 | 用 `--fg-probe` 单跑一次看具体缺什么 |
| MFG 下 `present` 变长、`skip` 变多 | 出帧数超过面板/算力（如 40fps×4x=160） | 降输入帧率（锁 30）或降 `--interp` |
| 计数器 120 但画面抖，`present` 只有 1–3ms | 外部工具在 Present 上打桩（FLM/RTSS/Steam overlay），或覆盖层被遮挡 | 关掉该工具；日志会打 `WARNING: presentation is not vblank-paced` |

### 7.1 装机时最容易忘的三件事

1. `libxess_fg.dll` + `libxell.dll` 必须**和 `bridge.exe` 同目录**（不是系统目录）。
2. `game_capture.ini` 里 `Color=0`（§2.1），并确认 `game_capture.log` 打印的是 `Color=0`。
3. 两个 `bridge` 不要同时跑（会抢同一个共享内存 + 同一个覆盖窗口）。

---

## 8. 不要做的事（已用实测排除的路）

| 看起来合理但其实有害 | 后果 |
|---|---|
| `--pace HZ` / `--pace auto` | 和 XeSS-FG 内部配速串行，`gpu` 2→17 ms，掉到 50Hz |
| `--no-throttle` | 队列深度 >1，延迟变差，实测整体更差 |
| 用更小的 WGC 帧池尺寸省算力 | 帧池是**裁剪**不是缩放，画面只剩左上角 |
| 钩游戏进程的 Present 自己拷后备缓冲 | 只省约 4 ms，却引入 PCIe 点对点（已知 TDR 源） |
| `--buffers 3` 治 5K 掉帧 | 实测 29.5→30.3 fps，无差别 |
| `--xell` / `--no-xell` 反复调 | 只改日志文字，不改行为（§4.3）。拿它做"睡眠有没有影响"的对照实验会得出错误结论 |
| `--sdr` / `--no-primaries` 日常使用 | 只是颜色诊断开关，会丢掉 HDR10 |
| 拿 `--interp 3` 直接套在没有解锁的 DLL 上 | 官方 DLL 在 AMD 上只有 1 个插值帧，`--interp` 会被夹回去（日志会说明），看起来"没效果"。要 4x 需要 `--unlock-mfg` |
| 在挂着 FLM / RTSS 等 Present 打桩工具时评价性能 | 计数器仍显示 120，但帧被成串推送 → 画面抖；日志会打 `WARNING: presentation is not vblank-paced`（见 `docs/findings/MFG.md` §3.3） |
| `run_bridge_xelldiag.bat` | 设 `XELL_FORCE_LOGS_TO_OUTPUT` 后帧率崩到个位数。**仅诊断，勿用** |
| `run_bridge_crossvendor.bat` | 曾一次性触发 9 次 NVIDIA 驱动复位。**仅诊断，勿用** |

> 最后两个脚本是历史实验产物，只建议在排查期临时用，并且**先备份存档**。

---

## 9. 遗留任务与改进方向

### 9.1 已闭环

架构、颜色链路（HDR10 正确）、插帧（`xN=2.00` 稳定）、三套配置固化、长跑稳定（4000+ 帧无 TDR）。

### 9.1.1 MFG 已打通并实测（`--unlock-mfg`）

官方 DLL 里 MFG 的代码是全的，卡住它的是 4 处 `libxess_fg` + 1 处 `libxell` 的字节检查。
`--unlock-mfg` 改掉它们（只改本进程内存，不动磁盘文件），实测
`maxSupportedInterpolations` **1 → 3**、`xN=4.00 xNmiss=0`、HITCH 0。

- **1440p 插值 + 4x = 117–124 fps**（30fps 输入）→ `run_mgpu_4x.bat` ⭐
- **5K + 4x 不可行**：每生成一帧 13 ms × 3 ≈ 40 ms > 33.3 ms 预算 → 实测 19fps×4 ≈ 80fps
- 5K 的甜点仍是 2x（58–61 fps）

原理、字节表、代价（每帧插值复用 2x 模型）见 `docs/findings/MFG.md`。

### 9.2 5K 模式余量接近零

重负载场景可能掉帧。**按优先级**试，一次只改一个，改完看 `skip`（不是 `load`）：

| 顺序 | 动作 | 代价 |
|---|---|---|
| 1 | `--buffers 3` | 无（但 1440p 下实测无收益，5K 未系统验证） |
| 2 | `--latency 2` | 延迟 +1 帧 |
| 3 | `--scale 0.95` | 轻微放大，几乎看不出 |
| 4 | 锁帧降到 28–29（RTSS 支持小数） | 输出略降 |

### 9.3 偶发单帧打嗝（长跑约 1 次，最大 176 ms）

DWM/WGC 固有。探索方向：禁用 DWM 批处理、排查抢焦点的窗口（浏览器/更新弹窗）。
日志里对应一条 `HITCH gap=176ms`。

### 9.4 延迟仍有 ~60–70 ms

构成（40fps 下每项约 25 ms）：WGC 抓取 1 帧 → XeFG 模型 ~13–25 ms → GPU 队列 1 帧。
**还没系统测过的组合**：`--buffers 3 --latency 2`、去掉 GPU 队列那一帧（需要重新设计
`gpu_throttle()` 的同步点）。测的时候以 `skip` 不恶化为前提。

### 9.5 可选：`--fg warp` 的插值质量

目前是"引擎 MV + 置信度不足时 8 邻域块匹配"。可提升方向：深度遮挡检测、
更优的像素搜索（多尺度 / 金字塔）。只有在"5K + 80fps"变成硬需求时才值得做。

### 9.6 代码清理（2026-11-07 已做 + 剩下的）

**已做**（见 `HANDOFF.md` §8.6.1 "代码现状"）：

- 删掉协议 v5 的死代码：`wait_slot_v5()` / `slot_ready()` / `g_ready_ever` /
  `g_last_used_id` / `--mv-newest`；
- 删掉 `--fg-hi-res-mv` / `--fg-jittered-mv`（当时按"MV 是死输入"删的；该结论已被推翻，现在 MV 由 `--no-mv` / 默认 / `--fg-of` 三态控制）；
- `--fg-of` 全家用 `MGPU_FG_OF` 宏隔离（默认 0 = 不编译，`bridge.exe` 427 KB → 137 KB）；
- 脚本收敛到 `scripts/` 12 个 `run_*.bat`，其余归档（2026-11-08 已移除，见 `ARCHIVED_EXPERIMENTS.md`）；
- `third_party` 从 1.4 GB 降到 5 MB 入库（FFX 只留头文件 + 两个 lib，OptiScaler 只留 3 个参考文件）。

**还剩（低风险，但会改变行为的先别动）**：

| 位置 | 问题 | 建议 |
|---|---|---|
| `bridge.cpp` 参数解析 | `--xell` 的判断写了两遍（重复行） | 删掉重复行 |
| `bridge.cpp` `g_no_xell` | 只被日志引用，没接进 `xellSleep` 调用 | 要么接上（**会改变默认行为，需重测**），要么整个删掉 |
| `--half` | 与 `--den 2` 重复的旧开关 | 保留兼容即可 |
| `FrameShared.ready` / `mv_scale_x/y` | 协议 v5 的字段现在没人读 | **保留**：改布局要两边同时重编并升版本号，得不偿失 |
| `run_bridge_*.bat` / `run_wgc*.bat` / `run_lsfg.bat` | 危险/过期脚本 | 已移入 `<TOOLS_DIR>\archive\scripts\` |

### 9.7 4K 面板 + VSR（下采样放在帧生成之前）

用户环境是 **4K 面板**，靠 VSR 让游戏按 5K 渲染以获得超采样细节。此时：

```
游戏渲染 5K → DLSS 输出 5K → DWM 合成 5K → 驱动下采样到 4K 面板
```

副卡其实只需要吞吐 4K。`--scale 0.75`（5K × 0.75 = **3840×2160**）就是把这次下采样
搬到我们自己的转换 pass 里、放到**帧生成之前**：

```
WGC 5120x2880 → [我们的 pass: 下采样 + HDR10] → XeFG @ 3840x2160 → 上屏 3840x2160
```

收益（像素数 8.29/14.75 = 0.56）：

| 配置 | 每真实帧 `gpu`（4x 实测/推算） | 33.3ms 预算 |
|---|---|---|
| 5K 插值（`--den 1`） | 39–44 ms（实测） | ❌ 超预算 |
| **4K 插值（`--scale 0.75`）** | **23.4 ms（实测 p50）/ 27.7 max** | ⚠️ 贴边（`present` 6.7 → total 31.3/33.3，78 窗口里 42 个丢过 1 帧） |
| 1440p 插值（`--den 2`） | 15–17 ms（实测） | ✅ 零丢帧，但细节少 2.25× |

**4K@4x 实测（78 窗口，2026-11-06 11:05，`bridge_4x_4k.txt`）**：

| 指标 | p50 | 说明 |
|---|---|---|
| `gpu` | 23.4（max 27.7） | 与推算的 22–25 吻合 |
| `present` | 6.7（1440p 是 14.8） | GPU 更忙 → 等 vblank 的时间自然变少 |
| `total` | 31.3（max 34.0） | 预算 33.3，贴边 |
| `show` | 119 | 满刷新率 |
| `HITCH` | **0 / 78** | |
| `skip` | **0.7 均值，42/78 窗口 >0** | 归因：17 个窗口 `total>31`（真贴预算）、26 个 `age≥10`、仅 2 个 GPU 有余量 |

结论：**4K@4x 跑得动，但是"贴边"**——54% 的窗口会丢 1 帧（约 1.7% 的输入帧）。
想要 4K 细节**又**零丢帧，下一个该试的是 **3x + 锁 40**：
预算 25 ms，`gpu` 约 16 ms（2/3 的生成量），`40×3 = 120` 依旧是 1:1 节拍，
而且延迟比 4x@30 低约 20 ms → 但 **4K 装不下**（实测 106 fps，见 §9.11），
所以这条路只在 1440p 插值时才有意义（`run_mgpu_3x_40.bat`，尚未实测）。


另外，**覆盖层窗口现在按"显示器分辨率"而不是"抓取分辨率"来定尺寸**（`overlay_size()`）：
桌面是 4K 时，4K 输出正好 1:1 上屏，不再出现
`4K 交换链 → DXGI 拉伸到 5K 桌面 → 驱动再下采样到 4K` 的**双重缩放**。
启动日志会打印：

```
[bridge] overlay fits the monitor 3840x2160 (capture 5120x2880)
[bridge]   captured 5120x2880 will be resampled once by us to the output size; ...
[bridge] FG swap chain: 3840x2160 ...
```

> 若桌面仍停在 5K，窗口就是 5120×2880，4K 输出会被 DXGI 拉伸 —— 那就还是双重缩放。
> 要拿到干净链路，把**桌面分辨率设成 3840×2160 @120**（VSR 只留给游戏分辨率）。

**另一条更省的路：干脆不要 VSR。** 桌面 4K + 游戏 4K（DLSS **Quality**，内部渲染 1440p）
时，WGC 抓的就是 3840×2160，我们能给的缩放次数是 **0**：

```
WGC 3840x2160 → [我们的 pass：只做 HDR10，1:1] → XeFG @ 3840x2160 → 上屏 1:1
```

比 `--scale 0.75` 少一次 5K 读（118 MB/帧 → 63 MB/帧的 FP16）、少一整个缩放 pass，
多出来的预算就是 `gpu` 余量；而且 `--down` 在这种 1:1 场景会被自动跳过
（日志打 `resize filter: not needed - 3840x2160 -> 3840x2160 is 1:1`）。
脚本：`run_mgpu_4x_4k_native.bat`。

**画质：这一步默认还是单点双线性（`--down 0`），0.75 倍下是白送的走样。**
加 `--down 1`（Catmull-Rom）能砍掉约 2/5 的 Nyquist 残留、3 倍像素的超采样细节才真正落到
4K 上，代价是 7×7 抽头 × 每个真实帧一次，实测预计 <0.5 ms `gpu`（§4.1 有量化表）。
`run_mgpu_4x_4k_hq.bat` 就是这个配置；`--down 3`（Lanczos-3）更锐但边缘有振铃，
适合静态截图对比，日常不建议。**用了 `--down` 就把 `--sharp` 关掉。**

### 9.8 XeFG 的成本模型（实测反推，4K）

用同一分辨率下的两次实测（`bridge_4x_4k.txt` 4x = 23.4 ms、`bridge_3x_4k.txt` 3x = 20.6 ms）反解：

```
每多生成一帧 ≈ 2.8 ms
固定成本       ≈ 15.0 ms     ← 真实帧 + 我们的 5K→4K 下采样 pass + XeFG 基础处理

4x@4K = 15.0 + 3×2.8 = 23.4 ms   (实测 23.4 ✓)
3x@4K = 15.0 + 2×2.8 = 20.6 ms   (实测 20.6 ✓)
2x@4K = 15.0 + 1×2.8 = 17.8 ms   (推算)
```

同一模型套 5K（像素 ×1.78）：固定 ≈26.7、生成 ≈5.0 → `4x@5K ≈ 41.7 ms`，与实测 39–44 吻合。

**三条实测的分布对照（同一份日志口径）**：

| 配置 | 预算 | `total` p50 / p90 / p99 | 超预算窗口 | `gpu` p50 / p99 | `skip` |
|---|---|---|---|---|---|
| 4x@1440p / 30 | 33.3 | 31.7 / 33.2 / 36.1 | 1（2%） | **16.4 / 24.5** | 0.3 |
| 4x@4K / 30 | 33.3 | 31.4 / 33.2 / 34.0 | 6（8%） | **23.4 / 27.7** | 0.7 |
| 3x@4K / 40 | 25.0 | 25.4 / 28.7 / 36.9 | **21（58%）** | **20.6 / 31.8** | **5.3** |

**关键修正：判据看 `gpu`，不要看 `total`。**

`total` 在健康时会**饱和到输入间隔**——因为 `present` 会把剩下的时间用来对齐 vblank
（4x@1440p 的 `gpu` 只有 16.4，`total` 却是 31.7，就是这个原因）。
所以 `total ≈ 间隔` 不代表"顶格"，它只是"已经按时排队了"。

**实测标定出来的稳定性判据**：

```
稳定条件：  gpu_p99  <=  输入间隔 - 3 ms
规划公式：  gpu_p50 + 8 ms  <=  输入间隔      （8 ms ≈ 实测的 p99-p50 抖动）
```

代入三组数据（全部吻合）：

| 配置 | `gpu` p99 | 判据线 | 判定 | 实测 skip |
|---|---|---|---|---|
| 4x@1440p / 30 | 24.5 | 30.3 | ✅ | 0.3 |
| 4x@4K / 30 | 27.7 | 30.3 | ✅ | 0.7 |
| 3x@4K / 40 | **31.8** | **22.0** | ❌ | 5.3 |

为什么"只超一点点"会被放大：超预算不是偶发，而是**过半的帧都超**（4K@40fps 是 58%），
WGC 池（`--pool 4`）只能吸收几帧的突发，持续欠账就变成每窗口丢 5 帧。

**由此得到的倍数上限（4K 成本模型 + 上述判据）**：

| 目标 | 可行倍数 | 说明 |
|---|---|---|
| 4K · 120Hz | **只有 4x @30** | `gpu_p50 23.4 + 8 = 31.4 ≤ 33.3` ✅；3x@40 需要 28.6 > 25 ❌ |
| 1440p · 120Hz | **4x @30 或 3x @40** | 3x@1440 预计 `gpu≈14 + 8 = 22 ≤ 25` ✅（低延迟版） |
| 5K · 120Hz | 不可能 | 4x@5K `gpu≈42 > 33.3`；只有 2x@30（60fps）可行 |


### 9.9 `--slack N`：用延迟换时间窗口（实测无效，但机制保留）

`--slack N` 扣住最多 N+1 个抓取帧、**处理最旧的**，把"晚一帧就丢掉这个输入帧"换成
"晚一帧就多等一帧"。**实测（3x@4K/40，2026-11-06 11:35，`bridge_3x_4k_slack.txt`）：**

| 指标 p50 | `--slack 0` | `--slack 1` | 变化 |
|---|---|---|---|
| `gpu` | 20.6 | **23.6** | **+3.0** ⚠️ |
| `total` | 25.4 | 27.8 | +2.4 |
| `skip` 均值 | 5.3 | 4.6 | −0.7（几乎没变） |
| `show` | 106 | 107 | +1（等于没救） |
| `compose→photons` | 36.6 | **73.3** | **+36.7** ❌ |
| `click→photons` | 64.9 | **107.5** | **+42.6** ❌ |
| `HITCH` | 3 | 0 | −3（唯一的好处） |

**结论：纯亏。**花 37 ms 延迟只换回 0.7 帧/窗口，输出还是 107 fps。

为什么：

1. 这个配置是**持续型**赤字，不是尾部型。判据 `gpu_p50 + 8 = 28.6 > 25.0` 早就算出"slack 没用"。
   （我上一轮同时给了两条判据，这条才是对的——`gpu_p50 + 8 > 间隔` ⇒ 加缓冲无效。）
2. 更关键：**`slack` 自己把 `gpu` 抬高了 3 ms**。推测是扣住抓取帧会延长 WGC 缓冲区的存活时间
   （池里那张纹理不能回收），反而增加了拷贝/分配的耦合开销。这抵消了它本来能吸收的抖动。

判据（最终形态）：

```
processing ≈ gpu + 1.5ms(我们的 CPU)
若 processing_p50 已接近/超过间隔  → slack 只会把丢帧换成更大的延迟
若 processing_p50 明显低于间隔且 p99 超间隔（尖峰型）→ 可以用 slack 1
```

实现上必须改共享内存：MV/深度的槽位从 **2 → 4**（`kSharedVersion = 4`），否则扣住 1–2 帧后
颜色帧与 MV 会错位配对（→ 鬼影）。因此 **add-on 和 bridge 必须同版部署**，版本不符会直接拒绝通信。

顺带验证到的两个收获（保留）：

- **v4 协议正常工作**：`shared ready: ... size=112.6 MB slots=4`，`xN=3.00`、`HITCH 0`、无报错；
- **内存反而更省**：`Color=0` 时 add-on 不再分配颜色流（4 槽会白占 224 MB），
  共享块从原来的 ~168 MB 降到 **112.6 MB**。



### 9.11 两轮失败实验：`--pace 30` 与 `--scale 0.72`（4K@4x）

基线 = `run_mgpu_4x_4k.bat`（11:53，49 窗口）。三轮对比：

| 指标 p50 | **基线** | `--pace 30`（18 窗） | `--scale 0.72`（29 窗） |
|---|---|---|---|
| `gpu` | 21.2 | 22.5 | **21.5（几乎没降）** |
| `present` | 8.4 | 7.8 | 8.9 |
| `total` | **29.6** | 32.6 | 32.2 |
| `load` | 86% | 76% | 93% |
| `skip` 均值 | **0.9** | **5.1** ❌ | 0.9 |
| `show` 中位 / 最低 | 117 / 109 | 118 / 111 | 118 / **103** |
| `HITCH` | **1** | **9** ❌ | 4 |

**`--pace 30` 失败**：9 次 HITCH 全是 `gap≈48–53 ms`（6 个 vblank 槽），也就是节拍器**漏拍**。
原因：我们的 `total` 29.6 ms 距 33.3 ms 的节拍只有 3.7 ms，配速模式对抖动零容忍——
漏一次拍就变成 50 ms 的空档，同时固定节拍的排空还丢了 5.1 帧/窗口。
> 这印证了 §5.3 的老结论，而且现在是在"算力并不紧张"的 4K 4x 上再次确认：**`--pace` 不要用**。

**`--scale 0.72` 失败**：像素少 7%，`gpu` 却纹丝不动（21.2 → 21.5）。
说明 4K 这一档的 `gpu` 里**固定成本占绝对多数**（我们的 5K→4K 转换 + XeFG 基础处理 + 拷贝），
降分辨率在这个区间几乎没有收益 → **别再用"降一点分辨率"换余量**。

**那么"稳定 120 无波动"到底卡在哪？** 三轮都指向同一个地方：输入到达被量化到 120Hz 网格。

```
jitter（每窗口内到达间隔 min..max）: 16.7..50.0ms / 16.7..58.3ms / 25.0..50.0ms
（30 fps 本该是干净的 33.3 = 4 个 vblank 槽）
```

30fps 内容 × 120Hz 面板本该完美整除，但 **RTSS 的限帧精度是 ±1 个面板周期（±8.3 ms）**，
实际到达变成 25 / 41.7 / 50 ms ⇒ 2 / 5 / 6 个槽。XeFG 每批要吐 4 帧，
**2 槽的批次物理上装不下** —— 于是 `show` 在 109–123 之间摆。

这是消费端修不了的：我们只是在消费 DWM handed 给我们的帧。
**唯一还没试的杠杆在限帧器那一侧**（见 §9.12）。


### 9.12 还剩的唯一杠杆：限帧器（不需要改代码）

判据只有一条：**日志里的到达间隔变成 `jitter=33.3..33.3ms`**（每个窗口的 min..max 都是 33.3）。
到达一旦对齐到面板网格，`show` 会自动变成 120 恒定，不需要别的改动。

可以按顺序试（都在游戏/RTSS 侧，1 分钟一次）：

| # | 做法 | 为什么可能有用 |
|---|---|---|
| 1 | RTSS 限帧模式改成 **front edge sync** 或 **back edge sync** | 让限帧以 vblank 为基准，而不是自由运行；默认 async 会漂 ±1 个面板周期 |
| 2 | 关掉 RTSS，改用**游戏内建限帧**（CP2077 有 FPS 上限选项） | 游戏自己的 present 更贴近面板节拍 |
| 3 | RTSS 锁 **29.97**（NTSC 30） | 改变相位，有时能避开最坏的分桶 |
| 4 | RTSS 开 "wait for V-Sync" 类同步选项（若驱动允许） | 直接把 present 钉到 vblank |

验证方法：跑任意 4K 配置 30 秒，然后

```bash
grep -o 'jitter=[0-9.]*\.\.[0-9.]*ms' bridge_4x_4k.txt | sort | uniq -c | sort -rn | head
```

- 看到 `33.3..33.3ms` 占多数 ⇒ 到达已对齐，`show` 应该稳定在 120；
- 仍是 `16.7..50.0ms` 这类 ⇒ 限帧器还在自由运行，换一种模式再试。

> 这一项修好之前，`show 109–123 / 中位 118` 就是这条链路的实际上限。
> 注意它是**速率波动**而不是"卡"——每帧都按时出，只是某一拍少/多一帧。

### 9.13 4K 上的可选配置（按实测排）

| 配置 | `gpu` | `total`/预算 | `show` | `skip` | 结论 |
|---|---|---|---|---|---|
| **4x @30 · 4K**（`run_mgpu_4x_4k.bat`） | 21.2–23.4 | 29.6–31.3/33.3 | **118** | 0.7–0.9 | ✅ **当前 4K 最优** |
| 4x @30 · 4K + `--pace 30`（`_FAILED_`） | 22.5 | 32.6/33.3 | 118（111–122） | **5.1** ❌ | ❌ 漏拍 9 次 HITCH（§9.11） |
| 4x @30 · 4K + `--scale 0.72`（`_FAILED_`） | 21.5 | 32.2/33.3 | 118（**103**） | 0.9 | ❌ `gpu` 没降，无收益（§9.11） |
| 3x @40 · 4K（`_FAILED_`） | 20.6 | **25.4/25.0** | 106 ❌ | **5.3** | ❌ 超预算 |
| 3x @40 · 4K + `--slack 1`（`_FAILED_`） | 23.6 | 27.8/25.0 | 107 ❌ | 4.6 | ❌ 用 +37 ms 延迟换 0.7 帧（§9.9） |
| 4x @30 · 1440p（`run_mgpu_4x_c.bat`） | 16.3 | 31.4/33.3 | 120 | 0.3 | ✅ 零丢帧但少 2.25× 像素 |
| 3x @40 · 1440p | ≈14.3（推算） | ≈20/25 | ~120 | 0 | 低延迟候选（`click→photons ≈55 ms`），未测 |

### 9.14 动态帧率下"不卡但不平滑"（2026-11-07 新增两个旋钮）

**症状**：不锁帧（或锁不住）时，帧率不低、也没有卡顿，但运动物体"走得不匀"——
像是每几步轻微跳一下。

**根因是算术，不是 MV/深度**：XeSS-FG 把生成的帧塞进面板的 vblank 里，
所以只有当 `T_in / (interp+1)` 是**整数个 vblank** 时，画面才真正均匀：

| 输入（120 Hz 面板） | 配置 | 输出间隔 | vblank 数 | 观感 |
|---|---|---|---|---|
| 锁 30.00 fps | 4x | 8.33 ms | **1.00** | ✅ 完全均匀 |
| 锁 40.00 fps | 3x | 8.33 ms | **1.00** | ✅ 完全均匀 |
| 动态 ≈38 fps | 4x | 6.6 ms | 1.26 | ⚠️ 1,1,1,2 交替 |
| 动态 ≈28 fps | 4x | 8.9 ms | 1.07 | ⚠️ 每 ~14 帧多占 1 个 vblank |
| 动态 ≈25 fps | 4x | 6.25 ms | 0.75 | ❌ **过订**：pacer 只能丢/延 |

这就是 §5.10 那条"物理事实"的动态版。下面是**三个**可用手段（按性价比）：

**① VRR（零代码，根治）**：面板开 FreeSync/VRR，并且 Present 带
`DXGI_PRESENT_ALLOW_TEARING`（我们的 `--tearing`）。VRR 下刷新率跟着输出走，
"整数个 vblank"这条约束消失 ⇒ 任意动态帧率都均匀。**动态帧率 + FG 的行业标准组合就是
FG + VRR**，其余手段都只是逼近。注意：不开 VRR 却开 tearing 只会撕。

**② `--interp-auto [floor|round]`（自适应插值帧数）**：每 40 帧窗口用输入间隔的
**中位数**算 `N+1 ≈ T_in / vblank`，让输出重新落在整数 vblank 上（120 Hz 下
T≈33ms→4x、25ms→3x、20ms→2x）。要点：
- 变化必须稀疏：XeSS-FG 文档说 `xefgSwapChainSetNumInterpolatedFrames`
  "may be expensive（可能换模型、触发 shader 编译）"，所以实现里**连续两个窗口
  稳定**才改一次（≈2.7 s），改完会有一次可感知的顿挫；
- `floor`（默认）永不超订，pacer 宁可变长也不丢帧；`round` 更接近理想比例但可能超订；
- OptiScaler 的同类实现（`XeFG_Inputs_Dx12.cpp` 的 `PassApply`）也是运行时调
  `SetNumInterpolatedFrames`，只是它由**用户设置**驱动，我们由**节奏**驱动。

**③ `--fg-rt-mode ema|raw|median|quant`（喂对 `frameRenderTime`）**：
XeSS-FG 开发者指南原文——"On non-Intel GPUs, XeSS-FG can use `frameRenderTime` to
sanity-check presentation pacing"。我们历史上喂的是 EMA(α=0.1)，30 fps 下尾巴约
**330 ms**，动态帧率时必然给出"上一段时间的平均值"，pacer 的 sanity check 和真实
节奏打架。四档：
| 模式 | 喂什么 | 说明 |
|---|---|---|
| `ema`（默认） | 历史行为，指数平均 | 稳态最稳，动态时最钝 |
| `raw` | 上一帧的原始间隔 | OptiScaler 的做法（它只额外做 s/ms 量纲自适应）；最灵敏，也最抖 |
| `median` | 最近 16 个间隔的中位数 | **推荐先试**：WGC 在合成时刻交付，35 fps@120 Hz 会以 25/33/25 ms 到达，中位数直接忽略这种 ±1 vblank 量化 |
| `quant` | 上者再吸附到整数个 vblank | 告诉 pacer 一个"物理上做得到"的节奏 |

日志每 40 帧多一行（只在启用时出现）：

```
[bridge]   cadence: rt=33.3ms (quant) input=33.2ms = 3.99 vblanks -> interp=3 (4x, 120.5 fps out) refresh=120Hz
```

**④ 辅助**：`--slack 1`（+1 输入帧延迟）把"晚一帧就丢"换成"晚一帧就多等一帧"，
在动态帧率下能让输入节奏连续（§9.9 的实测是在"锁 40 + 超预算"的坏场景下，别照搬结论）。

**A/B 脚本**（同一场景、同一段运动，RTSS **不锁**）：

```
run_mgpu_pbz_dyn.bat        --interp-auto + --fg-rt-mode quant
run_mgpu_pbz_dyn_rt.bat     只有 --fg-rt-mode median（隔离 ③ 的贡献）
run_mgpu_pbz_dyn_ema.bat    只有 --fg-rt-mode ema（② 的对照组，历史行为）
run_mgpu_pbz_dyn_vrr.bat    median + --tearing（VRR 已在系统里打开时用）
run_mgpu_pbz_3x40.bat       旧基线：固定 3x（原本应该锁 40 fps）
```

#### 首轮实测（2026-11-07，`bridge_pbz_dyn*.txt`，PBZ 4K，120 Hz，RTSS 未锁）

| | A `3x40`（固定 3x） | B `dyn_rt`（median，4x） | C `dyn`（auto+quant） |
|---|---|---|---|
| 窗口数 | 20 | 16 | 16 |
| `game` 中位 / 范围 | **38.1** / 26.0–45.9 | **38.0** / 26.0–41.1 | **40.2** / 26.0–47.2 |
| 插值 | 2（恒定） | 3（恒定） | 3，**运行时改档 4 次**（3x↔4x） |
| `xN` 中位 | 3.00 | 4.00 | 4.00 |
| `show` 中位 / 最小 / 最大 | 79 / 65 / 98 | **105 / 97 / 123** | 98 / **66** / 109 |
| `gpu` / `total` 中位 | 5.7 / 3.5 ms | 7.6 / 7.7 ms | 9.8 / 8.6 ms |
| `skip` 合计（~800 帧） | 5 | 6 | 5 |
| `xNmiss` | 1 | 1 | 1 |
| `wgc_drops` | 13 | 8 | 9 |
| `jitter` 范围 | 16.7–283 ms | 16.7–267 ms | 33.3–283 ms |

**四条结论：**

1. **游戏本身只有 25–26 fps**（`game` 中位 38–40 ms），三条都一样 —— 连"锁 40"的 A 也只有 26 fps，
   RTSS 没压住 / 压不动。于是 4x → 104 fps（**1.15 vblank/帧**，每 ~7 帧多占一个）、
   3x → 78 fps（**1.54 vblank/帧**，1,2 交替，肉眼最容易看出）。**这台机器上 4x 永远比 3x 均匀。**
2. `cadence:` 里的 vblanks 序列是 **3.12 / 3.72 / 3.84 / 5.52 / 5.64 —— 没有一个整数**。
   输入在 26/32/47 ms 三档之间跳，任何整数 N 都不可能整。⇒ **这就是要 VRR 的判据**，
   不是"再调一个参数"能解决的。
3. **`--interp-auto` 的 `floor` 策略被数据否掉**：32 ms（3.84 vblanks）时 floor 选 3x
   （93.8 fps，误差 0.84 vblank），round 选 4x（125 fps，过订 4%，误差 0.16）——
   `show` 从 105 掉到 98/66 就是它换档造成的，还额外付了 4 次换模型的顿挫。
   **已改为默认 `round`**，并加了"误差改善不足 0.15 vblank 就不换"的门限；
   被 DLL 上限截断时打一行
   `--interp-auto: input 47.0ms wants 6x … but this DLL caps at 4x`。
4. **B（median）是三条里最好的**（`show` 105、`gpu` 仅 7.6 ms、`skip` 0.6%），
   但缺 `ema` 同条件对照，归因待定 → 已补 `run_mgpu_pbz_dyn_ema.bat`。

**所以现在的建议**：这台机器（26 fps 输入）先不要用 `--interp-auto`；
用 **`--fg-rt-mode median`**，然后走 §9.15 的精确节奏矩阵（**5x 是解锁得到的，不需要 VRR**）。

### 9.15 无 VRR 的精确节奏矩阵（2026-11-07，`--unlock-mfg` 实测到 6x）

**先纠正一个一直以来的误判**：脚本里长期写着 `--interp 3`，于是 `maxSupportedInterpolations`
一直是 3（4x）。`src/xefg_unlock.h` 的补丁其实**把上报值改成你请求的值**，实测：

```
--interp 3 → maxSupportedInterpolations 3 → 4x
--interp 4 → maxSupportedInterpolations 4 → 5x     ← 可用
--interp 5 → maxSupportedInterpolations 5 → 6x     ← 可用
```

于是 120 Hz 有**三个**精确解（输出 = 输入 × (interp+1)，必须整除刷新率）：

| 输入 | 倍数 | 输出 | vblank/帧 | 需要游戏能跑到 |
|---|---|---|---|---|
| 30.00 fps | 4x（`--interp 3`） | 120.0 | **1.00** | ≥30 fps（≤33.3 ms） |
| **24.00 fps** | **5x（`--interp 4`）** | **120.0** | **1.00** | ≥24 fps（≤41.7 ms）← 实测 26–35 fps，稳 |
| 20.00 fps | 6x（`--interp 5`） | 120.0 | **1.00** | ≥20 fps（≤50 ms） |
| 25.00 fps | 4x | 100.0 | 1.00 | 需把桌面切到 **100 Hz**（`--list-modes` 确认支持） |

脚本：`run_mgpu_pbz_120_5x24.bat`（RTSS **24** + `--interp 4`）、
`run_mgpu_pbz_100_4x25.bat`（桌面 100 Hz + RTSS 25）、
`run_mgpu_4x_4k_pbz_depthonly.bat`（RTSS 30 + 4x，游戏需 ≥30 fps）。

代价与风险：**5x/6x 的每个生成帧都用 2x 模型**（MFG 模型不在这份 DLL 里），
所以倍数越高，生成帧的质量上限越低、GPU 时间也越高（4K 下 4x 实测 `gpu` 5.7–10.5 ms，
5x 约 +25%）。先用 5x@24 看观感，不行再退回 4x@30（能跑到 30 的话）。

**`--list-modes`**（新增）：打印这台机器在当前分辨率下真正支持的刷新率，并给出
每个倍数的精确输入值 —— 换分辨率/换面板时先用它算，再决定锁多少。

### 9.16 消费者侧锁帧为什么不行（`--lock` / `--lock-auto` 的真实边界）

新增了两个开关，但**先看清它们的边界**，否则会得出错误结论：

- `--lock N`：在 bridge 里按周期**丢弃**到的抓取帧，使转发给 XeFG 的输入严格为 N fps；
- `--lock-auto`：每窗口按「中位数输入 fps + 刷新率」求解 `(N, 倍数)`，要求同时满足
  ① `N*(interp+1)` 整除刷新率，② 游戏能维持 N。

**关键约束（实测推演 + 代码里已强制）**：我们只能丢帧，不能"延后/重排"帧，
所以丢一帧会把一个 T 间隔变成两个 2T。只有当 `游戏fps / 锁定fps` 是**整数**时，
丢弃才是均匀的（每 2/3/4 帧丢 1 帧）。若比例非整数（26 → 24 = 每 13 帧丢 1 帧），
就会出现"每 13 帧一个 2T 空档"，**比原来的 1.15 vblank 抖动更难看**。
所以 `--lock-auto` 对 26 fps 输入会明确拒绝，并打印：

```
--lock-auto: input 26.0 fps at 120 Hz has no exact cadence with up to 6x - falling back to the unlocked rate
```

也就是说：**要达到精确节奏，锁帧必须发生在"源头"**（RTSS / 游戏内建限制器，它们延迟游戏的
Present，使游戏自己按固定节奏产出），或者让显示跟着走（VRR）。这也解释了为什么
OptiScaler 那条路（自己按序呈现）同样救不了这件事：它也不能凭空造出 vblank。
`--lock` 真正有用的场景是**游戏比目标快整数倍**时（例如游戏 90 fps → 锁 30 + 4x = 120，
丢弃比 3:1，间隔完全均匀）。

### 9.17 `--dyn-mfg`：OptiScaler 式 **Dynamic MFG**（原版控制器移植）

OptiScaler 里有现成的、比我这几个尝试都成熟的实现（用户提示后从
`Downloads/OptiScaler-master.zip` 里核对过）：

| 文件 | 作用 |
|---|---|
| `OptiScaler.ini` §FrameGen | `DynamicMFG` / `FramerateTargetDMFG` / `RoundingDMFG` / `DynamicAllow1X` / `TargetAsCapDMFG` |
| `framegen/xefg/XeFG_DynamicMFG.{h,cpp}` | 顶层调度：目标 fps、窗口、1X、焦点检测 |
| `framegen/xefg/XeFG_DynamicCount.{h,cpp}` | 选倍率控制器：hold 时长、迟滞、**学到的每帧成本**、改完后的验证窗口、失败退避 |
| `framegen/xefg/XeFG_DynamicCap.{h,cpp}` | `TargetAsCapDMFG` 变体（"最低倍率保住上限"） |

它的三个关键设计与我们的失败点正好互补：

1. **不是每帧改 count**：`if (_framesToInterpolate != targetCount)` 才调，其余时间是
   hold 计时 + 迟滞 + 退避（`kMinUpGapMs` / `kHoldUpMs` / `_upBlockedUntilMs`）。
   我那个 `--interp-cadence` 每帧改，正好踩在它刻意避开的地方（§9.18）。
2. **测"游戏在 present 路径里等了多久"**：`InputCommon::take_sleep_ms() + presentWaitMs`
   （它自己的限帧器 + 输出侧等待）+ `state.lastFGFrameTime`，而不是消费者循环的节流。
3. **允许 1X = 直接关掉帧生成**（`result.generationOff`），倍率集合是 `{关, 2x…6x}`。

**但它自己的默认目标写得很明白**：

```cpp
// Just under the refresh rate (116 at 120 Hz), where a VRR display's frame cap sits
return refresh > 0.0f ? std::floor(refresh - refresh * refresh / 3600.0f) : 0.0f;
```

⇒ **它瞄的是 VRR 显示器的帧率帽**，不是"每帧整 vblank"。所以它对
"26 fps 输入 + 120 Hz + 无 VRR"这个组合也无能为力（4x=104 或 5x=130，没有第三种），
它优化的是**用最省的倍率把上屏帧率顶到目标**。

#### 第一版（精简，已废弃）：`--dyn-mfg`

```bat
bridge.exe --game PBZ.exe --den 1 --pool 4 --interp 5 --dyn-mfg --unlock-mfg ...
           --fg-target 116        ; 目标上屏帧率
           --fg-round 1           ; 0 at most / 1 nearest（默认）/ 2 at least
```

逻辑（`fg_dyn_mfg_tick`，文档注释里有完整推导）：

```
target = --fg-target 或 刷新率-4
cap    = 1000 / 中位数输入间隔        # 我们测得到的输入 fps
count  = floor(target / cap)         # "at most the target"，夹在 0..6
count == 0  ->  1X：SetEnabled(fg, 0)，保留 context 与历史
改档条件：新答案连续两个窗口稳定，且距上次改档 ≥ --fg-dyn-hold
```

**它带来的实际好处**（这些是固定倍率做不到的）：

| 游戏实测 | 固定 4x | `--dyn-mfg` |
|---|---|---|
| 40 fps（25 ms） | 160 fps（>120，pacer 丢 25%） | **3x = 120** ✅ 正好整拍 |
| 60 fps | 240 fps（丢一半） | **2x = 120** ✅ 正好整拍 |
| 90 fps | 360 fps | **1X = 关掉 FG**，90 fps 零伪影零开销 |
| 26 fps | 104 fps（1.15 vb） | 4x = 104（一样，见下） |

**它仍然解决不了的**：26 fps 输入时 120 Hz 上只有 4x=104 与 5x=130 两个选择，
残余 1.15 vblank 抖动在。这是"没有 VRR"的物理边界，不是实现问题。

脚本：`run_mgpu_pbz_dynmfg.bat`（**RTSS 关掉**）。日志判据：

```
dyn-mfg: input 39.0 fps, target 116 -> 3x (117 fps predicted), was 4x (window)
dyn-mfg: 1X (frame generation off): SUCCESS (0)
```

#### 首次实测（2026-11-07）：6x 起步 → 崩，已修

`bridge_pbz_dynmfg.txt`（脚本当时带 `--interp 5`，而我把 `--interp` 同时当成了**初始倍率**）：

```
gpu=44-49ms  present=22-31ms  total=68-73ms  game=69-72ms  xN=6.00  skip=69-74  show=82-88  load=97-100%
```

6x 需要 6 个 vblank = 50 ms/真实帧，26 fps 输入要求 156 fps > 120 ⇒ pacer 过订 ⇒ 我们等 GPU
等到 45 ms ⇒ 循环塌到 13 fps ⇒ **每 40 帧窗口丢掉 70 帧**（就是"卡顿+闪烁"）。

而且控制器**没有自救**：它算 `floor(116 / 12.8) = 9` → 夹到上限 6 = 当前值 ⇒ 永远不降。
这正是 OptiScaler 用"学到的每帧成本 + 改完验证 + 失败退避"防的坑，我那版把它简化掉了。

补了两条硬约束（`fg_dyn_mfg_tick`）：

1. **起步就是 4x**：`--interp` 现在只当**上限**（解锁把 6x 打进去），初始倍率固定 3（4x）——
   已经被实测证明在 4K 上可行的值。控制器只在**有证据**时往上爬。
2. **失速守卫**：`gpu + present > 0.75 × T_in` 且当前 >2x ⇒ 强制降一档，并且 **5 秒内禁止升档**。
   这条不信任"测量到的输入 fps"（它会被生成开销污染），直接看循环自己的预算。
   日志会打：`dyn-mfg: 6x cannot keep up (gpu 45.0 + present 23.0 of 49.0 ms) -> 5x`。

参考实现里还有两个我们**没有**移植的部分（如果以后还要继续做）：**学到的每帧成本**（用它预测
"再加一帧会不会超预算"，而不是只看当前 fps）和 **改完后的验证窗口**（改档后 2 秒内比较实际收益，
不达标就回退并封锁该步 10 秒）。在 4K/26 fps 这个点上它们不影响结论，但在帧率剧烈波动的场景里
是"不抖"的关键。

#### 我们移植的是**原版控制器**（2026-11-07，`src/xefg_dynamic_count.h`）

前两次是"精简版"（`floor(target/输入fps)` + 两条守卫），都被实测否掉
（§9.18 的 2x↔3x 极限环）。现在改成 **OptiScaler 的原版状态机**：

```
src/xefg_dynamic_count.h   ← 819 行，逐字节拷贝自
                              OptiScaler/framegen/xefg/XeFG_DynamicCount.h
                              （`tail -n +30 | diff` 验证过与上游完全一致，MIT）
src/bridge/bridge.cpp      ← fg_dyn_mfg_frame()：每真实帧填一个 Sample、应用返回值
```

它零外部依赖（不引 Config/State/日志），输入一个 `Sample`、输出"该用几张插值帧"，所以能原样拷。
我们只需按语义喂数：

| 字段 | 我们喂什么 |
|---|---|
| `nowMs` | `qpc_ms()` |
| `frameMs` | 游戏自己的帧间隔（**原始**上一帧到达间隔，控制器内部自己做 EMA，>250 ms 视为 hitch 丢弃） |
| `waitMs` | 我们 `Present()` 阻塞的时长（"应用在 present 路径里等了多久"） |
| `targetFps` | `--fg-target`，默认 刷新率-4（120 Hz → 116，与上游一致） |
| `refreshFps` | `g_refresh_hz` |
| `limitFps` | `0`（我们没有进程内限帧器） |
| `maxCount` | `g_fg_interp_max`（解锁上限，最多 5 = 6x） |
| `currentCount` | `g_fg_interp_actual`；1X 时为 0 |
| `rounding` | `--fg-round 0/1/2` = at most / nearest（默认）/ at least |
| `minCount` | `--fg-allow-1x off` 时禁止 1X（最低 2x，默认允许 1X） |

**它比我的精简版多的东西**（也是这次能指望它不抖的原因）：
- **学到的每帧成本**（`_genMs` / `_offMs`，以及无 cap 时从整帧时间反推的 `_freeGenMs`）——
  用"再加一帧预计能多快"来判断，而不是相信被生成开销污染的输入 fps；
- **每一步的独立 hold**（升 0.3 s / 降 1 s）+ **改档后 1.5 s 验证窗口**（没兑现就 undo）；
- **失败退避**：某一步被否就按 3 s 起、翻倍到 60 s 封锁，且"负载变轻"才提前解封；
- **硬规则**：每个生成帧必须挣到"自由帧能带来速率的 30%"，否则不升；
- **cap 识别**：presented 率被 V-Sync/外部限帧器顶住时走另一套规则（`Capped()` / `hidden`）。

**没移植的部分**：`XeFG_DynamicCap.h`（259 行，"TargetAsCap" 变体，需要 `TargetAsCapDMFG=true` 才用）
和上游的 `_cap` 分支。我们固定用默认的 `RoundingDMFG=1`（nearest）路径。

日志（每 5 秒一行状态，改档时额外一行）：

```
dyn-mfg: 4x held: presented 104.2 fps, target 116.0, lower 78.1, wait 8%, frame cost 2.31 ms, back-off 3 s, uncapped
dyn-mfg: 4x -> 5x (up): presented 118.4 fps, target 116.0, ...
dyn-mfg: 1X (frame generation off): SUCCESS (0)
```

> 判据仍然只有一句：`skip` 接近 0、`show` 贴着 target、`dyn-mfg:` 行**稀而合理**（不是每 2 秒一条）。
> 如果它还在 2x/3x 之间来回，那说明我们喂的 `waitMs`（我们的 Present 阻塞）与上游语义
> （它自己的限帧器 sleep + present）差得太远 —— 那时候要看 `wait %` 那一项。

#### 第三次实测（原版控制器，2026-11-07 05:21）：控制器是对的，卡在**占空比**

`bridge_pbz_dynmfg.txt`（`--interp 5` 上限 6x + `--fg-target 116`），51 个窗口：

```
game   med 33.0 ms (30.0 fps!)  min 24.2  max 36.6
gpu    med 21.1 ms                       <- 在等 pacer 排空一批 4 帧
total  med 31.5 ms / 输入间隔 33.0 ms     <- 零余量
load   med 87%  max 101%
skip   350 / 约 2040 帧 = 17%            <- 每 6 个输入帧丢 1 个
worst gap  med 44.6 ms  max 57.7 ms      >33 ms 出现在 50/51 个窗口
show   med 118   xN med 4.0
```

**控制器本身工作正常**（这是它第一次表现正确）：`up / down / undo / capped / back-off 3 s→12 s`
全部按上游逻辑，50 个窗口里只改档 7 次（上一版是 28 次）。

**卡顿来源不是它，是 `skip=17%`**：这次游戏是 **30.0 fps**，

```
4x × 30.0 = 120.0 fps = 刷新率  ⇒ 占空比 100%
```

pacer 要把整批 4 帧排进 33.3 ms，输入间隔也是 33.3 ms ⇒ 我们这条循环没有任何余量，
任何抖动都会让 WGC 池丢帧（丢 1 帧 = 真实内容间凭空少 33 ms，观感就是"卡"）。
对照：游戏 26 fps 时 `4x = 104 fps`，占空比 87%，`skip` 只有 0–2%。

**两个推论**：

1. `--interp 5`（按 6x 初始化上下文）可能是额外的开销来源：这次 `gpu` 21.1 ms，
   而所有按 4x 初始化的 4x 运行是 7.6–9.8 ms。未做同条件对照，先按"降回 4x 上限"处理。
2. 想不丢帧就得**放弃 100% 占空比**：`--fg-target 110 --fg-round 0`（at most）在
   30 fps 输入时选 **3x = 90**（占空比 75%），代价是 90 fps/120 Hz 的 1.33 vblank 轻微不匀。

对应的调好脚本：**`run_mgpu_pbz_dynmfg2.bat`**（`--interp 3 --fg-target 110 --fg-round 0`）。
判据：`skip` 从 ~7/窗口 降到 0–2，`worst gap` 从 45–58 ms 降到 33 ms 以内，`show` 落在 90–110。

#### 第四次实测（dynmfg2，2026-11-07 05:31）：占空比解决了，露出**输入侧的迟到帧**

`bridge_pbz_dynmfg2.txt`（`--interp 3 --fg-target 110 --fg-round 0`），83 个窗口，与第三次对比：

| 指标 | 第三次（上限 6x / target 116） | 第四次（上限 4x / target 110 at-most） |
|---|---|---|
| `xN` | 4.0（2x↔4x 跳） | **3.0（稳在 3x）** |
| `skip` | 350（17%） | **89（2.7%）** |
| `total` | 31.5 ms | **13.9 ms** |
| `load` | 87%（max 101%） | **62%** |
| `gpu` | 21.1 ms | **10.5 ms** |
| `show` | 118 | 117（min 81） |

⇒ 两条都得到验证：**上限 6x 初始化多花约 2 倍 GPU 时间**；**target/at-most 把占空比从 100% 降到 62%**，丢帧从 17% 降到 2.7%。

**但还剩两个问题：**

1. **控制器仍有 3 轮 "3x→2x→undo"**。它的 `lower`（少一档的预期）按
   `count × 1000/(frameMs − waitMs)` 估出 94–105 fps，实测只有 67–73。
   原因：3x 的 presented 已经被刷新率顶在 117–120，**再降一档并不会释放 1/3 的速率**，
   而是直接掉到 `2 × 真实帧率`。每轮 undo 都是一次顿挫 ⇒ 用固定 3x 消掉它。
2. **输入侧开始"迟到"** —— 这就是渲染卡 100% 的代价（见下）。

**渲染卡满载的影响（用户提问，日志已证实）**：

```
age (compose->我们收到)   max 121.9 ms，有一窗 392.8 ms
arrival jitter           8.3 .. 333.3 ms
mvs (深度拷贝)            单窗最坏 13.2 ms
wgc_drops                176 次（约 5% 的帧晚了一个多间隔）
worst present gap        中位 42.8 ms，>33 ms 出现在 74/83 窗口
```

机制：
- add-on 的命令（深度拷贝 `ds_run`）**跑在游戏的渲染队列上**，渲染卡满载时排在游戏 workload
  后面 ⇒ `mvs` 尖到 13 ms；
- **WGC/DWM 的合成拷贝也被挤** ⇒ 帧到我们手上时已经很旧（`age` 最坏 393 ms）；
- 于是到达间隔在 23–33 ms 里夹着 43/57/333 ms 的空档。

**FG 只能把给它的每一个真实帧放大 N 倍，补不了"迟到或缺失的真实帧"** —— 那个 43 ms 空档
就是观感上的"卡一下"。这一层不在 FG 侧，对策只有两个：吸收它（`--slack 1`）或让它不出现
（别让渲染卡 100%：降设置/RTSS/VRR）。

对应脚本：**`run_mgpu_pbz_3x_slack1.bat`**（固定 3x + `--slack 1`）。
判据：`worst gap` 降到 33 ms 以内、`skip` 0–1、启动后**不再出现** `SetNumInterpolatedFrames`。
若 gap 仍在 40 ms 以上 ⇒ 那就是游戏自己 40+ ms 的帧（渲染卡 100%），FG 侧无解。

#### 附录：精简版（第一版）的实测记录（已废弃，留作对照）

101 个窗口的统计（`bridge_pbz_dynmfg.txt`）：

```
game  med 26.9ms  min 23.6  max 69.7     <- 输入帧时间在 24~70ms 之间摆（4 倍！）
gpu   med  6.7    max 47.2               <- 47.2 是开局那次 6x
skip  med  0      max 67                 <- 同样只在 6x 期间
xN    med  3.0    min 1  max 6
改档相关日志 28 行                        <- 2x↔3x 每 ~2 秒来回跳（极限环）
```

三个 bug（已修，提交 `f8a2624` 之后）：

1. **起步仍是 6x**：`fg_apply_interp("after SetEnabled")` 用的是 `--interp 5`，把我设的
   4x 覆盖掉了 → 又出现一次 `gpu 47.2 / skip 67`。修法：`fg_apply_interp` 在 `--dyn-mfg`
   下强制用 4x 起步。
2. **失速守卫每次降 2 挡**（日志写"→3x"，实际 `interp=1` 即 2x）：`down` 差 1，且单窗口就可能误触。
   修法：一次只降一挡，且**连续两个窗口**满足才动作。
3. **公式不看刷新率**：游戏偶尔冲到 62 fps（`T_in=16 ms`）时它要 3x（=186 fps ≫ 120），
   必然过订，守卫再打回 2x，下一窗口公式又要 3x —— 极限环。修法：`want` 额外夹
   `floor(刷新率 / cap)`。

**规律（这条比 bug 更重要）**：这个场景**不适合任何"目标帧率"控制器**。输入帧时间在
**24–70 ms（4 倍）**之间摆，任何基于"当前 fps 除以目标"的控制器都只能追着它跑；
OptiScaler 之所以稳，靠的是 `XeFG_DynamicCount` 那几百行（按毫秒的 hold、学到的每帧成本、
改档后验证窗口、失败封锁），而且它的目标本身就是 VRR 的帧率帽。
**结论：PBZ 4K 这种 4 倍波动的负载下，固定 4x 是最稳的实际选择；`--dyn-mfg` 留给帧率较稳、
且明显高于目标的场景（例如 60 fps 的游戏想要 120 fps 输出）。**

> 与 §9.14 的 `--interp-auto` 关系：`--interp-auto` 追求"整 vblank"（需要整除才成立），
> `--dyn-mfg` 追求"最省地贴住目标帧率"（OptiScaler 的取向，也是更通用的一层）。
> 两者都是默认关，按场景选一个，不要同时开。

### 9.18 `--interp-cadence`（变倍率）—— **已实测，结论：失败**（2026-11-07）

> ## ⚠️ 2026-11-08 更正：这个结论已**撤回**
>
> 失败的真实原因不是"逐帧改倍率与 pacer 冲突"，而是**到达间隔被量错了**：
> 当时 `input=` 用的是 `GetTickCount64` 的墙钟差值，同毫秒内的两帧会丢一个样本，
> 中位数被推高约 20%（真实 25 ms 报成 31 ms，见 `PERF_BASELINE.md` §3.7）。
> 逐帧倍率**对间隔估计最敏感** —— 间隔偏 20%，它就会每帧要求错误的帧数，
> 于是输入崩到 63–78 ms。
>
> 改用 WGC 帧自带的 `SystemRelativeTime` 之后复测（2x@60，同场景，各 45 s）：
>
> | 配置 | 空刷新 | `arr>2vb` | `xN` | `gpu` |
> |---|---|---|---|---|
> | 纯 2x（对照） | 5.0–6.7% | 6.3–7.4% | 2.00 | 2.7–4.0 ms |
> | **`--interp 2 --interp-cadence`** | **2.8–3.1%** | 8.6–8.9% | 2.00–2.35 | **1.7–2.3 ms** |
>
> 即：空刷新**减半**，`gpu` 还更低，`skip=0`。它大部分时间仍是 2x，
> 只在晚到的帧上多要 0.2–0.35 帧 —— 正是想要的行为。
> 前提：**必须配 `--interp 2`**，否则 `--interp 1` 时 MFG 补丁被跳过、倍率上限停在 1。
>
> 另外修掉一个把这事看得更糟的日志 bug：cadence 模式下 `avg 1.00x / 60.0 fps out`
> 的算法漏了 `+1`（`g_cad_hist[k]` 是"要 k 个生成帧"，输出帧数是 k+1），
> 现在会正确显示 `avg 2.00x / 120.0 fps`。
>
> 下面的原始记录保留，作为"测量错会怎样误导结论"的案例。

想解决的问题是对的：§9.15/§9.16 都还在"常数倍率 + 锁帧"的框里，而锁帧的代价是
**UE5 帧时间波动 25%，锁低帧等于把 GPU 余量长期扔掉**。所以试了把倍率做成**每帧的函数**：


```
phase += T_in / vblank          # 这一帧之前过去了几个 vblank（带余数）
k      = round(phase)           # 这一帧总共要输出 k 张
phase -= k                      # 余数留到下一帧（Bresenham，长期平均精确）
interp = clamp(k - 1, 1, 6)
```

理论上 26 fps（38.5 ms = 4.62 vb）应该交替 5x/5x/4x/5x（平均 4.62x = 120 fps），
35 fps 交替 4x/3x。**实测是灾难**（`bridge_pbz_cadence.txt`，脚本已改名
`_FAILED_run_mgpu_pbz_cadence.bat`）：

```
cadence: input=26.0ms = 3.12 vblanks -> interp=2 (76.9 fps out = 1.56 vblank/frame)
cadence: input=78.0ms = 9.36 vblanks -> interp=5 (63.1 fps out = 1.90 vblank/frame)
cadence: input=63.0ms = 7.56 vblanks -> interp=5 (79.4 fps out = 1.51 vblank/frame)
```

输入从 **26 ms 崩到 63–78 ms**（13–16 fps），输出只有 **63–79 fps**，`1.00 vblank/frame`
一次都没达到。三条原因，前两条是我的实现 bug，第三条是这条路线的死因：

1. **正反馈死循环（bug）**：相位累加器吃的是 `g_arr_last_raw`，即"我们从 WGC 池里取走帧"的
   间隔；而 `gpu_throttle()` 每帧都等上一帧的 GPU（此刻上一帧含 5–6 张生成帧 ≈ 42–50 ms 的
   vblank 排队）。于是它测到的是**自己的节流**，不是游戏的帧时间：节流 → 间隔变大 →
   要求更多生成帧 → 更慢。应该改用帧自带的合成时间戳（`SystemRelativeTime`，算 `age` 时已经在读）。
2. **初始倍率放到 6x（bug）**：`--interp 5` 让 init 就是 6x，26 fps × 6 = 156 > 120，
   pacer 一开始就过订，直接进入上面那条链。
3. **逐帧改 count 与 pacer 的前提冲突（死因）**：`xefgSwapChainSetNumInterpolatedFrames`
   文档只说"may be expensive"，从没承诺逐帧安全；pacer 的批次记账与历史（4→5→4 反复）
   建立在 count 稳定的前提上。即使修好 1、2，这条也只剩"能用但会闪/花"的可能性。

**结论：这条路在这份 `libxess_fg 1.3.1` 上关掉了。** 代码保留（`--interp-cadence`，
默认关，启用时会打一行 WARNING 指向本节），但**不要在日常配置里用**。
`vblank/frame` 要么靠 VRR，要么靠 §9.15 的常数倍率 + 源头锁帧；
在没有 VRR、又不肯锁帧的前提下，**"不浪费 GPU"和"完全均匀"这两个目标无法同时满足** ——
这是 pacer 的物理，不是参数问题。

**为什么 OptiScaler 能做得更细**：它绕过了代理交换链的自动 pacing——自己调
`SetFrameTimeDelta` / `SetCameraValues` / `TagFrameResource`，并**自己按序呈现**生成的帧
（头文件里那句 "when the application does not choose to be responsible for the in-order
presentation of the generated frames" 就是这个意思）。那需要驱动 `libxess_fg` 的内部
C++ 接口 + 自建 present 线程，工作量与风险都远超本轮；我们的公开接口只有代理交换链，
能动的就是"生成几张"（②）和"告诉它什么节奏"（③）。

---

### 9.19 定稿：**锁帧 + 固定倍率**（2026-11-07，放弃动态倍率）

四轮动态倍率实验（§9.14 / §9.17 / §9.18）之后的结论：在这台机器上
**"固定倍率 + RTSS 锁帧" 是唯一稳定且可预期的方案**，动态倍率（`--interp-auto`、
`--interp-cadence`、`--dyn-mfg`）全部默认关闭，仅作研究留存。

| 场景 | 锁帧 | 倍率 | 上屏 | 依据 |
|---|---|---|---|---|
| PBZ 4K，想 120 | **24 fps** | **5x**（`--interp 4`） | 120.0 精确 | 游戏 26–38 ms，24 fps 有余量；5x 在 AMD 侧 gpu≈15 ms ≪ 41.7 ms 预算 |
| PBZ 4K，最省 GPU | 30 fps | 3x（`--interp 2`） | 90 | 占空比 75%，伪影最少 |
| CP2077 4K/VSR | 30 fps | 4x | 120 | §2.1.2 |
| 不想锁帧 | — | 4x | 104–130 | 会随游戏帧时间在 1.15–1.4 vblank 间漂移，只能接受 |

**为什么不再追求动态**：输入帧时间在 24–70 ms 之间摆（4 倍），任何目标帧率控制器
都会追着它跑；而"整拍"要求输入是刷新的整数分之一 —— 这是算术，不是调参。
锁帧把这两件事一起解决（输入既稳定又整拍），代价只是要手动设 RTSS。

配套：`src/mgpu_ui`（`docs/ops/UI.md`）—— 选窗口、一键开关（全局热键）、改参数、看实时数据。

**叠加窗口的输入穿透（2026-11-07 修）**：`bridge` 有三条建交换链的路径，其中"普通窗口"
兜底路径原先用的是 `WS_OVERLAPPEDWINDOW` 且没有 `WS_EX_TRANSPARENT / WS_EX_NOACTIVATE`，
在某些 UE5 游戏里会**吃掉鼠标与键盘**（叠加窗口是独立进程，`HTTRANSPARENT` 跨进程不生效）。
现在兜底窗口也带全套 `TOPMOST|TRANSPARENT|NOACTIVATE|TOOLWINDOW` + `WS_POPUP`，并且
`make_borderless()` 会作用在**真正持有交换链的那个窗口**上，三条路径都是点击穿透、永不抢焦点。

### 9.20 三条实测反馈的修法（2026-11-07 晚）

用户报了三件事：热键按第二次关不掉、异常时置顶窗口赖在桌面上不走、影之刃零菜单仍然点不动。
前两件是可靠性问题，第三件加了**探针**而不是继续猜。

**1）bridge 自己也有逃生热键：`Ctrl+Alt+End`**

`bridge` 主循环里 `RegisterHotKey(nullptr, 'MG', MOD_CONTROL|MOD_ALT, VK_END)`，
命中就 `return 0` 退出。这条不依赖 `mgpu_ui.exe`：UI 崩了、热键被抢、UI 被关掉，
叠加窗口都能拆掉。日志第一行会写 `escape hotkey Ctrl+Alt+End registered=1`。

**2）失去前台就退出（治"赖在桌面最上层"）**

`assert_topmost()` 每 400 ms 跑一次，顺带做焦点守卫：

| 前台窗口 | 处理 |
|---|---|
| 我们自己的窗口（叠加层 / 交换链窗口） | `focus_game()` 交还给游戏 —— UE5 认为自己"未激活"时会**丢掉菜单点击**（攻击走 raw input 照样有反应，这正好对上"能打、点不动菜单"） |
| 游戏窗口 | 正常，计时清零 |
| 第三方窗口 | 连续 `--exit-on-focus-loss N` 秒（默认 5，0=永不）→ `PostQuitMessage` 退出 |

另外 `!IsWindow(g_game_wnd)` → 立刻退出，游戏崩了不会留一个幽灵窗口。

**3）点击探针 `click probe`（`WindowFromPoint` 会跳过 `WS_EX_TRANSPARENT`）**

```
[bridge] click probe: cursor 1920,1080 under=... pid=... root=... game=...
         (ok)  或  <-- NOT the game
```
`root != game` 说明这一下点击**在 OS 层就没落到游戏上**（我们的某个窗口吃了）；
`root == game` 说明点击落到了游戏，问题在游戏内部（多半就是第 2 条的前台/激活状态）。
探针在光标落在游戏矩形内时**每 3 s 采样一次、最多 12 次**，不刷屏。
另有一条铁证级日志：叠加窗口只要收到左键/右键就记

```
[bridge] WARNING overlay received a left click - the click was NOT passed through
```

**4）`--overlay-layered`：A/B 开关**

给叠加窗口补 `WS_EX_LAYERED` + `SetLayeredWindowAttributes(0, 255, LWA_ALPHA)`（全不透明）。
个别标题只对 layered 窗口做穿透判定。UI 里"叠加层 / 分层窗口 (A/B)"就是它，勾上后命令行
自动多一个 `--overlay-layered`。

**5）UI 侧的停不掉**：`stop_bridge()` 改成三级阶梯 + 全量日志到 `bin\mgpu_ui.log`

```
stop: running=1 pid=12345 handle=...
stop: WM_CLOSE -> 2 window(s)
stop: WM_QUIT posted, exited=1          (或 stop: TerminateProcess)
```
没有 pid 时还会用 Toolhelp 扫一遍 `bridge.exe` 清干净。热键按下也记一行
（`WM_HOTKEY id=1`），这样"按了没反应"和"按了在关但关不掉"一眼能分开。

---

### 9.21 实测日志定位：`bridge` 不理 `WM_CLOSE`（2026-11-07 20:0x）

第一次跑完拿到的 `bin\mgpu_ui.log` 把"关不掉"说得一清二楚：

```
07:53:26.997  WM_HOTKEY id=1
07:53:26.997  hotkey/button: running=0 child_pid=0
07:53:27.037  start: pid=3684 cmd="...bridge.exe" --hwnd 0x403FC ...
07:54:11.886  stop: running=1 pid=3684 handle=...02D4
07:54:11.887  stop: WM_CLOSE -> 3 window(s)
07:54:13.909  stop: WM_QUIT posted, exited=0
07:54:14.043  stop: TerminateProcess            ← 2.2 秒后才真的死
```

结论：热键、toggle、命令行全对；**是 `bridge` 自己不肯走**。

两个原因：

1. `main` 里的 `return 0` 会跑**静态析构 + D3D12/COM/XeFG 拆除**，那一步会卡住
   （实测 ≥2 s，`exited=0`）。叠加窗口正在拆的时候没有任何值得等的东西。
2. UI 的第二级用的是 `PostThreadMessageW(pid, WM_QUIT, ...)` ——
   这个 API 要的是**线程 id**，传进程 id 会静默失败（日志里 `exited=0` 就是这么来的）。

修法：

| 位置 | 改成 |
|---|---|
| `hard_exit(why)` | `logf_` + `fflush` + `TerminateProcess(GetCurrentProcess(), 0)`，不走析构 |
| `wnd_proc` / `overlay_proc` 的 `WM_CLOSE` | **就地** `hard_exit("WM_CLOSE ...")` —— 不再指望"销毁窗口 → WM_DESTROY → PostQuitMessage → 帧循环恰好正在 pump"这条链 |
| 帧循环 pump 的 `WM_QUIT` | `hard_exit("WM_QUIT (launcher request)")` |
| 逃生热键 `Ctrl+Alt+End` | `hard_exit("escape hotkey ...")` |
| UI 停止阶梯 | 优雅等待 1200+800 ms → **500+300 ms**，最坏 0.8 s 内一定死 |
| UI 的 `WM_QUIT` | 枚举到该进程的真实窗口线程 id 再 `PostThreadMessageW(tid, ...)` |

**另一个必须记的坑**：UI 启动 `bridge` 时把 stdout 接到了匿名管道，
所以 `bridge` 的日志（`click probe`、`WARNING overlay received ...`）**根本没落到任何文件**。
现在 `reader_thread` 顺手把管道内容写成 `bin\bridge_ui.txt`（每次启动覆盖）。
没有这个文件，第 3 条（菜单点不动）永远是瞎猜。

---

### 9.22 菜单点击：OS 层已证明点击到了游戏（2026-11-07 08:0x，`bridge_ui_*.txt`）

有了 `bridge_ui.txt` 之后第一次拿到干净数据（整局 `fgwin=1`、探针全 `(ok)`、无 WARNING）：

```
click probe: cursor 1854,1894 under=…403FC pid=19028 root=…403FC game=…403FC  (ok)
click probe: cursor 2013,1874 under=…403FC pid=19028 root=…403FC game=…403FC  (ok)
frame  41 … hittest=5    fgwin=1      ← 游戏一直是前台
frame 161 … hittest=16   fgwin=1
（整局没有任何）WARNING overlay received a left click
```

| 已排除 | 证据 |
|---|---|
| 我们的窗口吃点击 | 无 `WARNING overlay received`；`hittest` 只增不见点击 |
| 点击落不到游戏 | `WindowFromPoint` 的 `root` 恒等于 `game`（该 API 会跳过 `WS_EX_TRANSPARENT`，等价于真实派发结果） |
| 游戏被判"未激活" | `fgwin=1`（`GetForegroundWindow()==game`）持续成立 |

结论：**OS 已经把点击交给游戏窗口了，剩下的问题在游戏内部。**
已知"攻击有效"是因为攻击走 **raw input**（WM_INPUT 与窗口消息路由无关），
而 Slate/UI 菜单走的是 **WM_\* 消息**——所以症状恰好是"能打、点不动菜单"。

顺带记两个观察：
- 游戏自己的窗口是 `WS_EX_TOPMOST`（`style=0x94000000 ex=0x00040008`），我们只是把它压到叠加层下面一层；
- z-order dump 顶部 8 个窗口全是 **AMD 驱动的隐藏窗口**（`pid=23224`，`AMDDVROVERLAYWINDOWCLASS`/`IME`/`GDI+ Hook`），可见 AMD 侧也在链路里，但它们 `vis=0`。

**新增 A/B：`--relay-input`（UI："叠加层 → 输入转发 (A/B)"）**

叠加层不再用 `WS_EX_TRANSPARENT`（于是它真的收到鼠标消息），然后在窗口过程里把
鼠标消息**按游戏客户区坐标**转发给游戏窗口：

```cpp
POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
ClientToScreen(h, &pt);          // 我们的窗口就是整屏 0,0
ScreenToClient(g_game_wnd, &pt);
PostMessageW(g_game_wnd, msg, wp, MAKELPARAM(pt.x, pt.y));   // 移动限流 8ms
```
按键/滚轮/移动都转发，日志打 `relay #1: msg=0x0201 -> game at x,y`（前 4 次）。
`WS_EX_NOACTIVATE` 仍保留，所以键盘焦点不变、raw input 也不受影响 —— 这是"游戏 UI 只认
窗口消息"这一种情况的通用解法。

**判定表**（下次跑完看 `bin\bridge_ui_HHMMSS.txt`，名字记在 `mgpu_ui.log` 里）：

| 现象 | 说明 |
|---|---|
| 勾上"输入转发"后菜单能点了 + 日志有 `relay #1..4` | 游戏 UI 只认窗口消息，这条就是正解（默认打开它） |
| 勾上后菜单还是点不动 | 游戏 UI 既不吃窗口消息也不信任光标位置（可能按 raw input 自己算光标），下一步只能查它是否在**独占全屏/特殊输入模式**下才响应 |
| 勾上后鼠标转向变卡/重复响应 | 说明游戏两种消息都收，必须只保留一种；那就退回不加 `--relay-input` |

**UI 侧踩到的坑（同日修复）**：这两个开关一开始"点了没反应"，根因不在互斥逻辑，而在
`mgpu_ui.exe` 的行命中判定只比较 y、不比较 x —— 左列"启动命令"那条 106px 高的行
（`rc=288,418-701,524`）在 y 上与整张"叠加层"卡片重叠，于是右列的点击全被它吃掉，
走进 `switch` 的 `default: return`。修法：`on_click` 遍历时补一行
`if (mx < rc.left || mx >= rc.right) { continue; }`，并把开关的可点框从 40×17 放宽到 68×25。
互斥也顺手做成对称：开"分层窗口"会关掉"输入转发"，反之亦然；两个都关 = 默认叠加层。

**同日第二个 bug：退出 FG 后游戏一直贴在桌面最上层。** `assert_topmost()` 每 400 ms 把游戏窗口
用 `SetWindowPos(game, ovl, …)` 插到自己的 topmost 叠加层**下面**，而 Windows 的规则是
"窗口一旦被放到 topmost 窗口之上，它自己也会变成 topmost"，于是游戏拿到 `WS_EX_TOPMOST`
并**永久保留**（这个位不会因为进程退出而消失）。修法两条：

1. `bridge.exe`：`restore_game_zorder()` 在 `hard_exit()` 里调用（WM_CLOSE / WM_QUIT /
   `Ctrl+Alt+End` / 失去前台自动退出都走这里），清 `WS_EX_TOPMOST` + `SetWindowPos(HWND_NOTOPMOST)`，
   日志打 `cleared WS_EX_TOPMOST on the game window`；
2. `mgpu_ui.exe`：`stop_bridge()` 末尾的 `clear_target_topmost()` 是兜底 ——
   三级阶梯最后一步是 `TerminateProcess`（bridge 没法自救）、或者用户直接任务管理器杀掉、
   或者 bridge 崩了，UI 会在停止后对 `--hwnd` 那个窗口做同样的清理。

另外 bridge 启动时若发现游戏窗口 `topmost=1`，日志会提示这可能是上一次运行没还原的遗留状态。
验证脚本：`<STAGE_DIR>\\zorder_test.ps1`（游戏运行时执行，打印 before/running/after 三次
`WS_EX_TOPMOST` 与窗口矩形）。

**定稿**：`--overlay-layered` 已设为 UI 默认（`Profile::overlay_layered = true`）——
影之刃零实测"开=菜单能点，关=点不动"，所以新配置档一上来就带着它。
`--relay-input` 仍是备选（勾上会自动关掉分层，两者互斥，两个都关 = 原始叠加层）。



---

### 9.23 输入探针：把测量点搬到游戏进程里（2026-11-07，影之刃零 / 伍长）

> **已移除（2026-11-07 晚）**：结论已经拿到（分层窗口即解），探针本身是纯粹的调试脚手架，
> 所以子类化、`WH_MOUSE_LL` 钩子、计数器、几何/DPI dump、`[input]` 记录连同 `user32.lib`
> 一起从 `game_capture.cpp` 删掉了。要复查时看 git 历史（本条下面的代码片段仍在）。

第二个 UE5 游戏（`Wuchang Fallen Feathers`，`Project_Plague-Win64-Shipping.exe`）复现同样的症状，
外部证据依旧干净：

```
[bridge] game window style=0x14000000 ex=0x00040008 topmost=1 rect=(0,0)-(3840,2160)
[bridge] click probe: cursor 1987,1013 under=…80B7E pid=19988 root=…80B7E game=…80B7E  (ok)
frame 41 … hittest=63  fgwin=1        ← 游戏是前台
（整局）没有 WARNING overlay received a left click
```

所以"谁的窗口吃了点击/谁是前台"这两条已经被彻底排除。剩下的可能性只有两种，而它们
**只能从进程内部区分**：

| 假设 | 进程内的判别 |
|---|---|
| 游戏**收到了** `WM_LBUTTONDOWN`，但自己丢掉了 | 日志里出现 `[input] first WM_LBUTTONDOWN ... the click DID arrive` |
| 游戏**没收到**（OS 没派发） | 完全没有那条行，`down=0` |
| 游戏被别的窗口"抢了激活"，UE 在非激活状态丢弃菜单输入 | `[input] WM_ACTIVATE WA_INACTIVE` / `WM_ACTIVATEAPP FALSE` / `active != game` |

`game_capture.addon64` 就在游戏进程里，所以直接做：`init_swapchain` 里拿到游戏窗口
（`swapchain->get_hwnd()`），`SetWindowLongPtrW(GWLP_WNDPROC)` 挂一层探针，
消息原样 `CallWindowProc` 透传；每个消息**只记第一次**（+ 计数器），每 240 帧打一行汇总：

```
[input] subclass installed: hwnd 0x… cls=UnrealWindow
[input] first WM_MOUSEMOVE client=(812,455) keys=0x0
[input] first WM_LBUTTONDOWN client=(1987,1013)  <== the click DID arrive
[input] WM_ACTIVATEAPP TRUE (active)
[input] fg=0x… game=0x… | active=0x… focus=0x… capture=0x…  move=… down=… raw=… act=… app=…
```

`active/focus/capture` 来自 `GetGUIThreadInfo(窗口线程)` —— 如果 `active != game`（日志会标
`<== ACTIVE IS NOT THE GAME`），那就是"窗口管理器层面游戏没有被激活"，和 UE 丢菜单输入完全吻合。

~~开关：`game_capture.ini` 里 `[capture] LogInput=0`（默认 1）。需要 `user32.lib`，链接行已加。~~
（探针已删；现在日志本身默认关，`[capture] Log=1` 或环境变量 `MGPU_CAPTURE_LOG=1` 才写文件。）

---

### 9.24 单行 OSD（2026-11-07）

左上角那一大块**不是我们的代码画的**，是 RTSS 7.3.7 的全局 OSD：

```
<RTSS_DIR> Statistics Server\Profiles\Global
[OSD] EnableOSD=1  ZoomRatio=2  PositionX=1  PositionY=42  BaseColor=00FF8000
```

`ZoomRatio=2` + 橙色，所以又大又显眼。现在：

- RTSS 全局 OSD 关掉（`EnableOSD=0`；原文件备份在 `<STAGE_DIR>\rtss_Global_backup.cfg`，
  `[Framerate]` 那些限帧设置一个字没动 —— 锁帧仍然归 RTSS）；
- `bridge.exe` 自己画一行，内容全是这条流水线真正有的数：

```
120 fps   x5   in 24.0 fps   gpu 7.9 ms   game 41.2 ms   lock 24   drop 3
```

| 字段 | 来源 |
|---|---|
| `fps` | `g_show_fps` = loop fps × XeSS 每次 Present 出的帧数 |
| `xN` | `g_fg_interp_actual + 1`（XeSS 真正应用的倍率） |
| `in` | `1000 / g_arrival_ms_ema`（游戏输入帧率） |
| `gpu` | `g_ms_throttle`（等上一帧 GPU 完成的时间） |
| `game` | `g_arrival_ms_ema`（游戏帧间隔） |
| `lock` / `drop` | 有 `--lock` 或真丢过帧时才出现（`g_lock_fps` / `g_lock_drops + g_pace_skips`） |

实现（`src/bridge/osd_panel.h`，`bridge` 与 `osd_test` 共用）：

- GDI 文本 → 32bpp DIB（**预乘 alpha**）→ `UpdateLayeredWindow`；400 ms 一次，跟 `assert_topmost()` 同一节拍；
- 窗口 `WS_EX_LAYERED|TRANSPARENT|NOACTIVATE|TOOLWINDOW|TOPMOST`，点穿透、不抢焦点、不进帧生成路径；
- 位置：游戏客户区左上角内缩 8 px（按窗口 DPI 缩放，13 px 字体）。

两个必踩的坑：

1. **不要**再调 `SetLayeredWindowAttributes`：它把窗口切成"整体 alpha"模式，
   `UpdateLayeredWindow` 的 per-pixel alpha 就直接不显示（第一版就是这样，屏幕上一片空）；
2. GDI 画字只写 RGB、**不动 alpha 字节**，所以画完要做一遍后处理：
   覆盖率 `v = max(R,G,B)` → `RGB = v`、`A = v + A0*(255-v)/255`（预乘白色）。

UI 开关：`叠加层 → 单行 OSD`（默认开），关掉时命令行加 `--no-osd`。
排版自测：`<STAGE_DIR>\osd\osd_test.exe`（在假窗口上渲染 4 行样例，不开游戏就能看效果）。

---

## 10. 相关文档



| 文档 | 内容 |
|---|---|
| `docs/findings/milestones/M4_WGC_RESULTS.md` | WGC + HDR + XeSS-FG 的全部实验记录与证据（§1–10），本文的所有数据都出自这里 |
| `docs/findings/HANDOFF.md` §8.6 | MV/深度/颜色同步问题的**结论（8.6.1）**与十轮实验档案（8.6.2 起） |
| `docs/ops/UI.md` | **`mgpu_ui.exe`**：选窗口 / 全局热键开关 / 参数面板 / 实时数据（Lossless-Scaling 式前端） |
| `docs/findings/OPTISCALER_NOTES.md` | XeFG 的 MV 约定、相机运动重投影、`D3D12TagFrameResource` 习惯（原文抽在 `third_party/optiscaler/`） |
| `ARCHIVED_EXPERIMENTS.md` | 60 个已移除实验脚本各自在验证什么 |
| `docs/findings/MFG.md` | **多帧生成（3x/4x）**：DLL 里的三道闸门、OptiScaler 的做法、我们的开关与验证方法 |
| `docs/findings/XESS_FG_NOTES.md` | XeSS-FG 的硬性约束（2x 上限的来历、XeLL 依赖、代理交换链形态） |
| `docs/findings/milestones/M2_RESULTS.md` | MV/深度/jitter 的实测规格；§4 说明为什么相机矩阵只能用近似值 |
| `docs/findings/milestones/M3.2_RESULTS.md` | 为什么必须两个进程（进程内建第二厂商设备会死锁） |
| `docs/findings/ENV_REPORT.md` | 硬件/驱动/运行库探测结果 |
| `docs/findings/TOOLING_NOTES.md` | 命令挂起的原因与规避规则（`run_commands` 必须单行，长命令走 `rcmd.sh`） |
| `src/mgpu_shared.h` | 共享内存 v3 布局（含 `mv_scale_x/y`） |
| `effects/mgpu_depth_tap.fx` | 深度 tap shader（历史路径，最终配置不再需要） |

### 构建命令

```bash
./scripts/build_bridge.sh                      # -> build/bridge.exe（已带 libxess_fg/libxell）
./scripts/build_addon.sh src/game_capture game_capture   # -> build/game_capture.addon64
```

> 命令必须**单行**（换行会让 Cline 的 shell integration 丢失结束标记），长命令用
> `./scripts/rcmd.sh <名字> '<命令>'`，详见 `docs/findings/TOOLING_NOTES.md`。


（`bridge.cpp`：`const float cc[4] = { 80.0f, ... }`）。觉得整体偏暗/偏亮就改这一个数，需要重新编译。
