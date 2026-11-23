CP2077 UI composition 测试（DLSSG-only / 全分辨率 UI）
=====================================================

目标：渲染卡从 (P=Backbuffer, H=HUDless) 合成全分辨率 UI，经无损 UI 码流
（8 槽）送到消费卡；消费卡用 WGC 的 Backbuffer 减去 UI，重建 HUDless 交给
XeFG（mode 4 / mode 5）。这一轮只验证这条链路，不要混入别的开关。

当前稳定基线（2026-11-23，1/2 已验证）：
  UiSparseCodec=1, UiTagSynth=0, UiSnapshotPQ=0
  coverage 6–8%，fr: coded ui 1.36–1.55 MB，成本约 103%。
  本轮完整记录见 docs/design/UI_ITERATION_2026-11-23_SPARSE.md。

当前部署状态：已执行 01_ui_on.bat（UiLinear=1），直接启动游戏即可；若之后
改过 fakerFG.ini，重新运行 01 / 01b / 02 并重启游戏。

定稿架构（1.3.0+ / v34，mode 4-only）：stream 3 / mode 5 关闭；stream 4
（`HudlessStream=1`）只放「被 UI 遮挡的画面」的支撑域 patch。消费端还原干净 HUDless，
只 tag H' 走 mode 4。`UiPatch=0` 可回退。`UiDilate=0`、`UiAlpha8=0`（patch 强制 R10）。

目录
----
  E:\SteamLibrary\steamapps\common\Cyberpunk 2077\bin\x64\
    lock_frame.exe           本次构建的 consumer
    fakerFG.dll / dxgi.dll   本次构建的记录层（两份必须相同）
    nvngx_dlssg.dll          同一份 fakerFG.dll（游戏原始 DLL 已改名）
    xess_mfg_producer.dll    本次构建的 producer
    fakerFG.ini              producer 配置（每个游戏会话只读一次）
    ui_test\                 本测试脚本、日志、BMP、results

测试前准备
----------
1. 完全退出游戏和旧的 lock_frame.exe（Ctrl+Alt+End）。
2. 运行 00_check.bat，确认全部 [ok]。
3. 运行 01_ui_on.bat（默认 UiLinear=1）；它会备份 fakerFG.ini。
4. 重新启动游戏，读取存档，保持 DLSS Frame Generation = ON。
5. 把游戏锁在 40 fps（游戏内上限或 RTSS）。目标输出 120 fps（3x）。

脚本顺序
--------
  00_check.bat               文件/配置/进程检查
  01_ui_on.bat               patch 模式：UiStream=0, UiSource=Off, UiPatch=1,
                             HudlessStream=1, UiFullRes=1, UiSlots=2
  09_run_3x_patch_mode4.bat  patch：只传「UI 遮挡域里的 HUDless」，消费端
                             合成 H' 后只 tag HUDless（mode 4）
  03_run_3x_mode4.bat        patch/mode 4（当前脚本走 COMMON = patch）
  04_run_3x_mode5.bat        **已封死**：即使运行也不会 tag UI；不再使用
  01b_ui_on_linear0.bat      A/B：UiLinear=0（旧 PQ 码值减法）
  02_ui_off.bat              A/B 基线：UiStream=0 / UiSource=Off
  05_run_3x_ui_off.bat       基线运行
  06_collect.bat             收集日志+BMP+summary.txt 到 results\<时间戳>\

每一步跑 30–60 秒，场景里要有 HUD 文字/小地图。按 Ctrl+Alt+End 结束
consumer（不要直接关游戏）。03/04 会在启动后第 240 帧导出 5 帧 BMP，分别放在
  ui_test\logs\tag_m4_<时间戳>\
  ui_test\logs\tag_m5_<时间戳>\
每个目录有 tag_00..04_{backbuffer,hudless,ui}.bmp 与 meta.txt。

A/B 组合
--------
  A1  UiLinear=1 + mode 4    （推荐主路径）
  A2  UiLinear=1 + mode 5
  B1  UiLinear=0 + mode 4    （需要 01b + 重启游戏）
  B2  UiLinear=0 + mode 5
  C   UiStream=0 + 05        （基线；需要 02 + 重启游戏）
  D   UiPatch=1 + 09         （v33 支撑补丁；producer 应打印
                              `UI source = HUDless patch under UI (mode 4)`，
                              consumer 打印 `ui-unblend: ... HUDless patch`，
                              不应出现 `ui: mode 5`）

每组都保持同一个场景、同一个 40 fps 锁。换 UiLinear/UiStream 后必须重启游戏，
只换 mode 4/5 可以直接重跑 consumer。

日志判据（consumer：logs\run_m4_*.log / run_m5_*.log）
-----------------------------------------------------
- `ui-unblend: HUDless rebuilt from 3840x2160 present + 3840x2160 ...`
  说明 unblend 路径真的启用，且 UI 是全分辨率。
- `ui: mode 4`；**不应出现 `ui: mode 5`**（patch 模式固定 mode 4）
- `pair: exact=... no_slot=...`：命中率。exact 应远大于 no_slot。
- `cadence: ... input=25.0ms = 3.00 vblanks -> interp=2 (avg 3.00x, 120.0 fps out)`
  40 fps 输入、3x、120 Hz。
- `frame ... presented=3 ... present=Nms`：每个真实帧是否稳定出 3 帧；
  `present` 不超过 1 个 vblank（8.3 ms）太多；持续 >9–10 ms 说明预算紧。
- `out: presents=120 expected=120 ...`：输出帧数是否被吞。
- `bus: mv ... + depth ... = ... MB/frame`：跨卡字节；UI 走码流后额外字节在
  producer 的 `fr: coded ui` 行。
- `headroom:` 若出现，说明输入+拷贝占了本帧 25% 以上或 >48 MB/frame。
- `LATENCY: compose->present ...`：只记录，不参与判断。

producer（游戏目录 Cyberpunk2077_mfg_producer.log）
---------------------------------------------------
- `producer: UI source = ...` 与 `producer: UI 3840x2160 fmt 10 -> 3840x2160 ...`
  旧半分辨率路径会显示 1920x1080；本轮必须是 3840x2160。
- `ui-synth: target 3840x2160 ...`、`ui-synth: coverage ...`：
  覆盖率（m>0.5 / m>0.01）；覆盖异常时检查 tag census。
- `ui-synth: using true UIAlpha ...`（游戏有 UIAlpha 时）
- `fr: coded ui  ... MB/frame`：UI 码流实际字节；`UiCodec=auto` 时应该是
  packed（明显小于 raw 33 MB）。
- `fr: transport plan: ... ui=codec`：确认 stream 3 选了码流而不是纹理槽。

BMP 判读
--------
- `tag_XX_backbuffer.bmp`：上屏画面（含 UI）。
- `tag_XX_hudless.bmp`：应干净无 HUD；有残留说明 unblend/mask 不对。
- `tag_XX_ui.bmp`：重建用的 UI；边缘应贴合 HUD，而不是整屏。
- `tag_XX_patch.bmp`：**v33 patch 模式的重建素材**（解码后的 HUDless 补丁）。
  支撑外的 32x8 tile 在 codec 头里是 mode=3，解码为 0，所以 BMP 里是黑块；
  这些黑块正是消费端从 present 拷贝的区域。把 patch.bmp 与 present 对齐看，
  支撑应该覆盖所有 HUD；hudless.bmp 应与真实 HUDless 逐像素一致。
- `meta.txt`：source / mode5 / derived / linear / patch / ui_quality_bp / bmp 是否写出。
  `patch 1` = v33 支撑补丁；`mode5 0` = 只 tag HUDless（mode 4）；
  `reconstruct_bmp 1` = 第三张图（patch.bmp）已写出。

结果收集
--------
运行 06_collect.bat，把 `ui_test\results\<时间戳>\` 整个目录发回即可。
summary.txt 已抽取上面这些关键行，并带每个 tag 目录前 3 帧的 meta。

原生基线（整个系统 vs 原生 2077）
-----------------------------------
`10/11` 的 off 仍带 MV/depth + marker，是“UI 增量”，不是无系统基线。
要测整个系统成本：

1. 退出游戏和 consumer，运行 `20_cost_native_off.bat`：
   - 移除 dxgi/fakerFG/nvngx_dlssg/xess_mfg_producer 代理；
   - 恢复游戏原始 `nvngx_dlssg.dll`；
   - 启动游戏，DLSS Frame Generation ON，40 fps；
   - `12_gpu_sample.bat native 60`。
2. 退出游戏，运行 `21_cost_restore_system.bat` 恢复本系统。
3. 再按 `11_cost_ui_on` 的流程测 `on`。

对比：

```text
on - native  = 整个系统给渲染卡的额外成本
on - off     = 纯 UI 合成增量
off - native = 基础截获/传输 + marker + MV/depth codec
```

注意：原生侧如果开 DLSS-G，NVIDIA 自己的 FG 也在渲染卡上跑；这个差值是
“原生 DLSS-G vs 本系统”的端到端差异，不是单一 UI 模块的差值。

GPU 成本 A/B（只测渲染卡额外开销）
-----------------------------------
先确认 `HudlessStream=1`（v34 patch 默认），这样 UI 开时 stream 4 是支撑域 patch。

1. 关闭游戏，运行 `10_cost_ui_off.bat`，重启游戏读同一个存档，锁 40 fps。
   运行 `05_run_3x_ui_off.bat`，同时开 `12_gpu_sample.bat off 60`。
2. 关闭游戏，运行 `11_cost_ui_on.bat`，重启游戏读同一个存档。
   运行 `03_run_3x_mode4.bat`，同时开 `12_gpu_sample.bat on 60`。
3. 对比两个 CSV：
   `powershell -File gpu_summary.ps1 -Csv <csv>`。

关闭侧检查日志：

```text
faker: sl: HUDless staging OFF (UiStream=0 HudlessStream=0)
producer: 没有 UI source / ui-synth / HUDless stream / downscale: hudless
consumer: 没有 ui-unblend: ... rebuilt
```

Marker 是配对用的，开/关都会画，不属于 UI 合成成本。

注意
----
- `--tag-dump` 是全分辨率读回，5 帧约 0.5 GB；脚本固定 5 帧，不要随意加大。
- 改 fakerFG.ini 后必须重启游戏；producer 每个会话只读一次。
- 出现花屏/黑屏时先看 `ui: mode 5`、`ui-synth: ... skipped`、
  `ui-unblend: ...` 的拒绝原因，不要先改参数。
- 本轮不要加 `--fg-gate`、`--interp-start`、`--mfg-replace-all`、`--fg-debug-*`。
