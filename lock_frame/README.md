# lock_frame —— 锁帧 30x4 / 40x3（WGC 颜色 + marker 精确配对）

这是一个**完全独立**的实现目录：`src/`、`scripts/` 下的任何文件都没有被修改。
源码基于仓库里原有的 WGC 消费者实现（`src/bridge`）与当前 consumer 的
marker 精确配对逻辑（`src/xess_mfg_consumer` + `src/mgpu_pair_core.h`）复制、
裁剪、合并而来。

## 它解决什么问题

v23 把颜色变成 producer 的 stream 0：渲染卡每帧多一次 4K 颜色复制，约 **6%**。
锁帧版把颜色放回 **WGC**（DWM 本来就要把画面搬到输出卡，复用这笔传输），
producer 只传 MV + depth，默认 `StreamPercent=150`（每维约 2/3，字节 4/9）；
消费端默认 `--upsample`，把 MV/depth 联合重建到输入画面尺寸后再交给 XeFG。
这样渲染卡的预算主要留给游戏本身。

为什么只保留两个模式：WGC 的到达时间被 DWM 量化到 2/3/4 vblank，动态逐帧的
相位累加器无法在这种输入上稳定工作。锁帧把输入节拍固定下来：

| 模式 | 输入 | 插帧 | 输出（120 Hz 面板） |
|---|---|---|---|
| `30x4` | 30 fps | 3 张（4x） | 120 fps |
| `40x3` | 40 fps | 2 张（3x） | 120 fps |

两个模式都在 `lock_frame.exe` 里用 `--mode 30x4` / `--mode 40x3` 选择，
UI 也只提供这两个选项。

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
共享内存（16 槽，v26）

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
  ui/mgpu_ui.cpp          精简为两个锁帧选项的前端（构建为 lock_frame_ui.exe）
  fakerFG.ini             游戏目录用配置（颜色关闭、StreamPercent=150）
  run_30x4.bat            无 UI 直接启动（30x4）
  run_40x3.bat            无 UI 直接启动（40x3）
  build.sh                统一构建脚本
  build.bat               消费端 cl 构建（由 build.sh 调用）
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

## 部署（自用）

1. 把 `fakerFG.dll`、`xess_mfg_producer.dll` 按原有注入方式装进游戏
   （`dxgi.dll` / `nvngx_dlssg.dll` 等名字映射与现有版本一致）。
2. 把本目录的 `fakerFG.ini` 放到**游戏 exe 旁边**。
   它明确写上 `ColourStream=0` 与 `StreamPercent=150`。
3. 把 `lock_frame.exe`、`lock_frame_ui.exe`、`libxess_fg.dll`、`libxell.dll`
   放到同一个目录。
4. 游戏内锁 30 fps（用 30x4）或 40 fps（用 40x3），然后从 UI 选模式启动，
   或直接运行对应的 `run_*.bat`。
5. 面板需为 120 Hz（两个模式都是 120 fps 输出）。其它刷新率请用
   `--interp/--lock` 手动组合。

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
- `lag`：已验证发布比最新发布落后几个槽；`max + 2 < kMaxSlots(16)` 才安全。
- marker 的每帧解码/CRC 失败会单独打一行（最多 4 次），用于判断是"没画"还是
  "画了但读不出"。

A/B 文件（放在 exe 旁边）：

- `marker_repair_off.on`：不补四角 marker（会看到画面四角的小方块）。
- 游戏目录的 `marker_disable.on`：fakerFG 不画 marker（配对必然失败，仅排障）。

## v26（2026-11-15）槽位默认 16

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

## 已知边界

- WGC 颜色是 scRGB FP16；sink 按 FP16 建立。HDR10 输出由 convert pass 编码，
  SDR 用 `--sdr`。
- 四角 marker 的回填在 convert pass 内完成；缩放输出（`--scale` / `--den 2`）
  时只有 1:1 主路径会替换（锁帧版默认 1:1）。
- marker 的 ms12 用低 12 位毫秒，配对容差 120 ms；seq12 回绕周期约 136 s
  （30 fps），4 个槽的快照不会产生歧义。
- 本版强制关闭"按时间猜槽"的预拷/发布 FIFO/StreamPercent 自动控制器，
  这些 A/B 参数在命令行仍能解析，但不再改变行为。
