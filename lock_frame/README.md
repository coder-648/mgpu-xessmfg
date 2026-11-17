# lock_frame —— 固定倍率 2x / 3x / 4x（WGC 颜色 + marker 精确配对）

这是一个**完全独立**的实现目录：`src/`、`scripts/` 下的任何文件都没有被修改。
文档索引见 [`docs/README.md`](docs/README.md)。

源码基于仓库里原有的 WGC 消费者实现（`src/bridge`）与当前 consumer 的
marker 精确配对逻辑（`src/xess_mfg_consumer` + `src/mgpu_pair_core.h`）复制、
裁剪、合并而来。

## 它解决什么问题

v23 把颜色变成 producer 的 stream 0：渲染卡每帧多一次 4K 颜色复制，约 **6%**。
锁帧版把颜色放回 **WGC**（DWM 本来就要把画面搬到输出卡，复用这笔传输），
producer 只传 MV + depth，默认 `StreamPercent=150`（每维约 2/3，字节 4/9）；
消费端默认 `--upsample`，把 MV/depth 联合重建到输入画面尺寸后再交给 XeFG。
这样渲染卡的预算主要留给游戏本身。

为什么是固定倍率：WGC 的到达时间被 DWM 量化到 2/3/4 vblank，动态逐帧的
相位累加器无法在这种输入上稳定工作。锁帧把输入节拍固定下来，再由用户选择
倍率：

| 模式 | 输入（120 Hz 面板） | 插帧 | 输出 |
|---|---|---|---|
| `2x` | 60 fps | 1 张 | 120 fps |
| `3x` | 40 fps | 2 张 | 120 fps |
| `4x` | 30 fps | 3 张 | 120 fps |

三个模式都在 `lock_frame.exe` 里用 `--mode 2x` / `--mode 3x` / `--mode 4x`
选择；旧的 `--mode 30x4` / `--mode 40x3` 保留为 `4x` / `3x` 的别名。
UI 提供这三个固定倍率，并显示当前面板刷新率对应的建议输入锁帧值。

其他刷新率同理：输入 = 面板刷新率 / 倍率。不能整除时 UI 会提示，而不是
静默给出错误节奏。

> 关于 "scar"：最终版的 WGC 设计里，16x16 scar 图块已经不再走共享内存
> （v17 之后 marker 是自包含的 32-bit code，图块只用于"补掉画面四角"）。
> 锁帧版沿用这个定版：身份来自 marker，四角由 convert pass 用内侧邻居回填，
> 不再有额外的 scar 流。MV/depth 仍按发布槽与 marker 的 frame_id 精确配对。

## 数据流

```
游戏（渲染卡）
  fakerFG：Evaluate 记录 MV/depth；present 前在画面四角画 32-bit marker
      │
      ├─ xess_mfg_producer：只发布 MV + depth（StreamPercent=150）
      │      FrameShared.frame_id = fakerFG 的 seq
      ▼
共享内存（32 槽，v28）

消费卡 lock_frame.exe
  WGC 抓窗口颜色（scRGB/HDR 原样）
      ├─ 解四角 marker -> seq12 + low12(ms) + CRC-8
      ├─ 与 frames[].frame_id / frame_qpc_ms 精确匹配（mgpu_pair_core）
      ├─ 命中：拷该槽 MV/depth -> 转换（顺带把四角 marker 用内侧邻居补掉）
      │        -> XeFG tag -> Present
      └─ 未命中：只上屏真实帧（1X），不打 tag，也绝不猜槽
```

## 目录

```
lock_frame/
  lock_frame.cpp          消费端入口 + 模块索引
  modules/*.inc           消费端实现（WGC、配对、转换、XeFG、OSD、锁帧）
  shaders/                upsample_joint.hlsl
  producer/               xess_mfg_producer 的独立副本（ColourStream=0, StreamPercent=150）
  faker_fg/               fakerFG 的独立副本（重新启用四角 marker 绘制）
  ui/mgpu_ui.cpp          固定倍率 + fakerFG 输入源前端（构建为 lock_frame_ui.exe）
  fakerFG.ini             游戏目录用配置（默认 FgInput=DLSSG、颜色关闭、StreamPercent=150）
  deploy_game.bat         把游戏侧文件装进游戏目录（备份/别名一次完成）
  collect_diagnostics.bat 收集日志/系统/显卡信息为 zip（不联网）
  run_2x.bat              无 UI 直接启动（2x）
  run_3x.bat              无 UI 直接启动（3x）
  run_4x.bat              无 UI 直接启动（4x）
  run_30x4.bat            旧别名 -> run_4x.bat
  run_40x3.bat            旧别名 -> run_3x.bat
  version.h               产品版本号（发布脚本与 UI 共用）
  build.sh                统一构建脚本
  build.bat               消费端 cl 构建（由 build.sh 调用）
  release.sh              正式版打包（manifest / SHA256 / GPL 源码包）
  docs/                   规划、发布说明、测试步骤、PBZ 测试配置
  third_party/nlohmann/   vendored JSON 头文件（MIT）
```

## 构建

在 WSL 中：

```bash
./lock_frame/build.sh all      # producer / fakerFG / ui / consumer
# 或分别构建
./lock_frame/build.sh consumer
./lock_frame/build.sh producer
./lock_frame/build.sh faker
./lock_frame/build.sh ui
```

产物写入仓库的 `build/`：

- `build/lock_frame.exe`
- `build/lock_frame_ui.exe`
- `build/fakerFG.dll`
- `build/xess_mfg_producer.dll`

可覆盖的环境变量：

```bash
MGPU_STAGE=/mnt/c/temp/mgpu      # 中间文件目录
MGPU_FXC="/path/to/fxc.exe"      # Windows SDK fxc
MGPU_VSDIR="C:\\...\\BuildTools" # Windows 侧 VS 安装目录
```

## 发布打包

```bash
BUILD=1 ./lock_frame/release.sh          # 先全量构建，再打包
VERSION=1.0.0 ./lock_frame/release.sh    # 覆盖 version.h 的版本号
WITH_XESS=1 ./lock_frame/release.sh      # 可选：把 Intel 运行时一起打包
WITH_THIRD_PARTY=1 ./lock_frame/release.sh  # 可选：源码包放入本地 third_party 副本
```

产物：

- `dist/lock_frame-<ver>.zip`：运行包（默认不含 Intel DLL）；
- `dist/lock_frame-<ver>-src.zip`：GPL-3.0 对应源码；
- `manifest.json` + `SHA256SUMS.txt`：版本、协议、文件校验值。

打包脚本会调用 `lock_frame/scripts/check_release.py` 校验必需文件、禁止的
`.pdb/.map/.log`、`fakerFG.ini` 默认键和全部 SHA-256。

## 部署

1. 把整个目录放到可写位置，运行：
   ```bat
   deploy_game.bat "F:\...\游戏 exe 目录"
   ```
   它会备份/改名并把 `fakerFG.dll`、`dxgi.dll`、`nvngx_dlssg.dll`、
   `xess_mfg_producer.dll` 装好；`fakerFG.ini` 只在缺失时放入模板。
2. 把 `lock_frame.exe`、`lock_frame_ui.exe`、`libxess_fg.dll`、`libxell.dll`
   放在**同一个目录**（发布包目录），不要放进游戏目录。
3. 游戏内锁帧：2x 锁 60 fps，3x 锁 40 fps，4x 锁 30 fps（120 Hz 面板）。
   然后从 UI 选倍率启动，或直接运行对应的 `run_*.bat`。
4. 面板默认按 120 Hz 计算；其他刷新率下输入 = 面板刷新率 / 倍率。不能整除时
   UI 会提示，不会静默给错误节奏。

### fakerFG 输入源

UI 的“游戏侧配置 → fakerFG 输入源”会写入游戏目录的 `fakerFG.ini`：

| 选项 | `FgInput=` | 适用 |
|---|---|---|
| DLSSG（推荐） | `DLSSG` | 走 Streamline `slSetTag/slEvaluateFeature`，已在 RED / UE5 / UE4 实测 |
| NVNGX | `NVNGX` | 游戏直接使用 `NVSDK_NGX_DLSSG` 参数块 |
| Upscaler | `Upscaler` | 从 DLSS-SR 参数块取 MV/depth |
| FSR | `FSR` | AMD FFX 3.1 `FrameGenerationPrepare` 车道 |

写之前会在同目录备份为 `fakerFG.ini.bak_<tick>`；写入后需要**重启游戏**生效。
如果游戏目录没有 `fakerFG.ini`，UI 只提示，不会擅自创建半份配置。

## 日志判读

每个 `--report 40` 窗口会打一行：

```
[lock] pair: exact=40 no_slot=0 ms_fail=0 ambiguous=0 unreadable=0 dup=0 (total=40) lag(avg=0.10 max=1)
```

- `exact`：marker 与发布精确匹配并打了 tag 的帧数（目标 = total）。
- `no_slot` / `unreadable`：颜色到达时发布已被覆盖，或四角 marker 被 HUD 遮挡/
  DWM 缩放坏掉。两者都会让该帧 1X，而不是错配。
- `dup`：marker 的 **seq12** 与上一帧相同（同一帧身份被重复交付，例如
  loading/menu/pause 时 fakerFG 仍按 120 Hz 重画 marker）而被直接丢弃的帧。
  只比较 seq12，不比较 ms12 —— ms12 每帧都会变，比较完整 payload 会把
  这类帧误当成新输入（实测 733 帧同 seq 全部 no_slot → 1X）。
- `lag`：已验证发布比最新发布落后几个槽；`max + 2 < kMaxSlots(32)` 才安全。
- marker 的每帧解码/CRC 失败会单独打一行（最多 4 次），用于判断是"没画"还是
  "画了但读不出"。

A/B 文件（放在 exe 旁边）：

- `marker_repair_off.on`：不补四角 marker（会看到画面四角的小方块）。
- 游戏目录的 `marker_disable.on`：fakerFG 不画 marker（配对必然失败，仅排障）。

## v28（2026-11-17，正式版基线）固定倍率 + 输入源

- 共享协议 `kSharedVersion = 28`，`kMaxSlots = 32`；控制块与纹理数据区分离。
- UI 从两个锁帧模式改为 **2x / 3x / 4x** 三个固定倍率；旧 `30x4` / `40x3`
  仍可作为命令行别名。
- UI 新增“fakerFG 输入源”，写入游戏目录 `fakerFG.ini` 的 `FgInput`
  （DLSSG / NVNGX / Upscaler / FSR），写入前自动备份。
- 2x 不需要 `--unlock-mfg`；3x/4x 仍由 UI 发 `--unlock-mfg`。
- `README` 的槽位说明与代码对齐为 32；`StreamPercent` 默认 150。

## v26（2026-11-15，历史）槽位 8 → 16

- `kMaxSlots` 8 → **16**（协议 v26）。30x4 实测偶发 `lag max=6`，8 槽只剩 1 格
  余量；16 槽给 WGC 抖动和长帧留足空间。MV/depth only 的共享内存：4K 容量约
  **1.06 GB**，1440p 容量约 **472 MB**。
- `g_fwd_expect[]` 现在为所有 16 个槽初始化成 1；之前只写了前 4 个，多出来的
  槽第一次 wait 会用 0（立即返回，不等发布）。

## v25（2026-11-15）40x3 修正 + 槽位 8

- `--mode` 现在会置 `g_fg_interp_set`。之前 40x3 只设了 `g_fg_interp=2`，随后
  `--unlock-mfg` 因为该标志为 false 按默认目标 3 打补丁，并把 `g_fg_interp`
  覆盖回 3 —— 这就是 40x3 一直跑成 4x 的直接原因。
- `kMaxSlots` 6 → 8：40x3 实测 `lag avg≈4 max=5`（同样颜色延迟在 40 fps 下
  对应更多发布），6 槽会丢 5–8%。8 = 5 + 2 安全 + 1。
- 上采样目标改为**输入画面尺寸**（capture/present 尺寸，`g_full_w/h`），不再用
  游戏内部 render 尺寸；1440p 渲染 → 4K 输入时也会重建到 4K。

## v24（2026-11-15）针对实测命中率的修正

- `kMaxSlots` 4 → 6：实测 WGC 颜色落后 1–3 个发布（`pair: lag max=3`），
  4 槽在 40x3 下没有余量，偶尔一个额外发布就会 `no_slot`。6 槽 = 3 + 2 安全
  + 1，共享内存约 80 MB → 120 MB（StreamPercent=150）。
- 重复帧按 **seq12** 去重（不是完整 payload）：loading/menu/pause 时
  fakerFG 以 120 Hz 重画同一帧 marker，ms12 每帧都变；旧比较会把这些帧
  全部当作新输入，于是整段 `no_slot` + 1X。
- 超过 250 ms 没有新发布时隐藏覆盖窗口，让游戏自己的菜单/加载画面露出来；
  第一条新发布到达时再显示，避免去重后画面冻结。

## MV/depth 150% + 消费端上采样

- 跨卡只传 150%（每个维度约 2/3），精确度比 200% 高，带宽仍远低于 1:1。
- `lock_frame.exe` 默认开启 `--upsample`：在消费卡上用 `shaders/upsample_joint.hlsl`
  把 MV/depth 联合重建到 `frames[].render_w/h`（输入画面尺寸），XeFG 看到的是
  重建后的成对资源，而不是 150% 的小图。
- 日志：`[lock] --upsample: ON by default ...`，以及一次性
  `--upsample: MV/depth rebuilt from ... to ... on this GPU`。
- 如果 `--no-depth` 或 `--depth-lag > 0`，重建 pass 会跳过（没有成对输入）。

## 无 MV 标题（Wuchang / UE5 等）

- 这类游戏只发布 depth（`active_mask` 没有 `kRequestMv`），颜色 ↔ MV/depth 的配对仍然
  由 marker 完成，MV 改由消费端 FidelityFX 光流生成。
- fakerFG 现在允许 **depth-only publication**：`slEvaluateFeature` 的 render 尺寸在
  MV 缺失时回退到 depth，`ePresentStart` 的 staged 发布也不再要求 MV 非空。没有这条，
  producer 会以 render=0 初始化/更新，最终一个发布都没有，消费端只能 `no_slot`。
- producer 的 downscale pass 现在按"源是否真的存在"决定要不要缩放：`StreamPercent=150`
  不会再因为 MV 流为空就 `scale_dispatch(null)` 返回失败并把整帧丢掉（这是 depth-only
  时最常见的"一个发布都没有"的原因）。
- 消费端用 `active_mask & kRequestMv`（而不是 `ProducerInfo.input_flags` 或"槽已放置"）
  判断是否真有 MV；没有就自动走光流，并在配对失败时用"当前颜色 + 光流 + 上一有效
  depth"降级，不再直接 1X。
- 光流 MV 的 `motionVectorScale` 使用**实际 tag 的 OF 纹理尺寸**（`g_of_out_w/h`），
  与 depth stream 尺寸严格一致；尺寸变了会拒绝 tag 并打日志，而不是喂给 XeFG 一对
  尺寸不匹配的输入。

排障时看这几行：

```text
sl: depth-only publication (no MV tagged) render=... depth=...
shared: content is now mv ... depth ... render ...
[lock] mv: auto -> optical flow ... (active_mask=0x.. mv slot ... (no MV data))
[lock] --fg-of: FidelityFX optical flow at ... -> tagged at ...
[lock] pair: exact=.. no_slot=0 unreadable=0 ...
[lock] ... -> 1X (...) [pair=1 producer_mv=0 of=1/1]
```

最后一行如果出现 `of=0/0`，说明光流没有初始化/没有跑（检查 depth stream 是否
`>= 256`、`g_of_out_w/h` 是否与 depth stream 一致）；如果 `producer_mv=0` 且
`of=1/1` 仍然是 1X，则配对 `pair` 字段会是 0。

## 已知边界

- WGC 颜色是 scRGB FP16；sink 按 FP16 建立。HDR10 输出由 convert pass 编码，
  SDR 用 `--sdr`。
- 四角 marker 的回填在 convert pass 内完成；缩放输出（`--scale` / `--den 2`）
  时只有 1:1 主路径会替换（锁帧版默认 1:1）。
- marker 的 ms12 用低 12 位毫秒，配对容差 120 ms；seq12 回绕周期约 136 s
  （30 fps），32 个槽的快照不会产生歧义。
- 固定倍率默认按 120 Hz 面板设计；其他刷新率按“输入 = 刷新率 / 倍率”推导。
  不能整除时会产生不整的输出节奏，UI 会提示；首版不承诺 165 Hz 这类非整除刷新率。
- 2x 使用原版 `libxess_fg` 即可；3x/4x 依赖 `--unlock-mfg` 的字节补丁，只对
  实测过的 `libxess_fg` / `libxell` 版本成立，失败时会回退并打日志。
- 输入源中 DLSSG 为正式支持；NVNGX / Upscaler / FSR 仍属实验性，需要按游戏验证。
- 本版强制关闭"按时间猜槽"的预拷/发布 FIFO/StreamPercent 自动控制器，
  这些 A/B 参数在命令行仍能解析，但不再改变行为。

## 第三方项目与致谢

本目录的实现依赖或参考了以下项目；版权归各自作者，详见根目录 `NOTICE` 与
`docs/ops/LEGAL.md`：

| 项目 | 用途 | 许可 |
|---|---|---|
| OptiScaler | DLSS-G/XeFG 输入车道、交换链接管、Dynamic MFG 参考 | GPL-3.0 |
| AMD FidelityFX SDK | 光流 / FFX API | MIT / AMD 许可 |
| Universal MFG Unlocker | `--unlock-mfg` 偏移与改法 | MIT |
| nlohmann/json | Streamline 插件清单解析 | MIT |
| MinHook | fakerFG 模块/加载器钩子 | BSD-2-Clause |
| ReShade | 旧 add-on 宿主（可选） | BSD-3-Clause |
| Intel XeSS SDK | XeSS-FG / XeLL 运行时与头文件 | Intel Simplified Software License |
| NVIDIA DLSS SDK / Streamline | DLSS-G / Streamline 输入契约 | NVIDIA 许可（专有） |
| Magpie | FG pacing 参考 | GPL-3.0 |
| dlssg_for_sm86 | DLSS-G 后端研究参考 | GPL-3.0（NVIDIA 组件不转授权） |
| Microsoft Windows SDK / DirectX 12 | 平台接口 | Microsoft 许可 |

正式包默认不分发 Intel 运行时；`--unlock-mfg` 的风险见 `docs/RELEASE_NOTES_1.0.0.md`。

