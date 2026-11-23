CP2077 v34 UI 支撑补丁（mode 4-only）

视觉 dump：运行 `13_dump_patch.bat`（游戏已启动、consumer 未运行）会在 ui_test\logs\tag_visual_* 生成 tag_XX_backbuffer/hudless/patch/ui/meta。（只传被 UI 遮挡的画面，固定 XeFG mode 4）
================================================================

这是一条实验路径：渲染卡把 HUDless 在 UI 支撑域上的切片传过来，消费端
合成 H' = present（支撑外）/ patch（支撑内），只 tag HUDless，XeFG 走
BACKBUFFER_HUDLESS（mode 4）。stream 3 / mode 5 已关闭；没有 UI 平面、没有 alpha、没有 (P-U)/(1-a)。

点击顺序
--------
0. 完全退出游戏和旧的 lock_frame.exe（旧消费者用 Ctrl+Alt+End 停）。
   当前 fakerFG.ini 已经写好 UiPatch=1；如果要重新应用，先跑 09a_patch_on.bat
   再启动游戏（producer 每个会话只读一次 ini）。

1. 双击 00_check.bat
   期望看到 UiPatch=1，且 dxgi/fakerFG/nvngx 三个文件字节一致。

2. 启动 game 目录里的 Cyberpunk2077.exe，读一个存档：
   - DLSS Frame Generation = ON
   - 游戏/RTSS 锁 40 fps（目标 3x = 120 fps）
   - 画面里要有 HUD 文字、小地图或半透明面板

3. 双击 09_run_3x_patch_mode4.bat，跑 30–60 秒，然后 Ctrl+Alt+End 停止。

4. 双击 06_collect.bat 收集日志/BMP/summary。

产物
----
ui_test\logs\run_patch_<时间>.log
ui_test\logs\tag_patch_<时间>\tag_00..02_backbuffer.bmp   ← 上屏画面（含 UI）
ui_test\logs\tag_patch_<时间>\tag_00..02_hudless.bmp      ← 交给 XeFG 的 H'
ui_test\logs\tag_patch_<时间>\tag_00..02_patch.bmp        ← 重建素材（HUDless 补丁，加 LUT 前）
ui_test\logs\tag_patch_<时间>\tag_00..02_alpha.bmp        ← 逐像素 UI 掩码（patch alpha）
ui_test\logs\tag_patch_<时间>\tag_00..02_meta.txt         ← patch 1 / mode5 0 / alpha_bmp 1
ui_test\results\<时间>\producer_synth_dump\               ← producer 侧 P/H/patch 的 BMP

看 BMP 的方法
-------------
- backbuffer：应该就是游戏画面（含 HUD）。
- hudless：应该干净无 HUD；UI 区域内的场景来自 patch。
- patch：只保留 HUD 支撑域内的场景；支撑外的 32x8 tile 在 codec 头里是
  mode=3，解码为 0，所以在 BMP 里是黑块。黑块=消费端从 present 拷贝的区域。
  把 patch 和 backbuffer 叠看：支撑应覆盖所有 HUD（宁多勿少），不能漏掉文字。

日志判据（consumer）
--------------------
- 必须有：`ui-unblend: HUDless rebuilt from ... HUDless patch (source=patch)`
- 必须有：`hudless: ...` 或 tag dump 的 `patch=1`
- 期望：**始终只 tag HUDless，XeFG 走 mode 4**；不应出现 `ui: mode 5`
- 期望：`pair: exact=... no_slot=0`，presents 不丢帧

日志判据（producer，游戏目录 Cyberpunk2077_mfg_producer.log）
-----------------------------------------------------------
- `producer: UiPatch=1 - ... stream 3 is sealed off, consumer tags H' only (mode 4)`
- `producer: UiPatchWhiteNits=80.0 (scRGB 1.0 = nits)`（传输编码；消费端自动测增益）
- consumer: `ui-patch: 3D LUT 33^3 ready (H code -> WGC code, tetrahedral)`
- `fr: coded ui ... MB/frame`：patch 字节量（4K 典型约 2.2 MB/帧）
- 若出现 `UiPatch=1 needs P/H ... R10 transport`：检查是否 HDR/R10、是否游戏 FG 已开

## 基线（2026-11-24 定稿）

本链路以 **`UiPatch=1` + 消费端 33³ 3D LUT（四面体插值）** 为基线：
- 3D LUT 只用 `alpha=0` 干净像素配对 H/P，GPU 内建表，无回读；
- `UiPatch=1` 是 1.3.0+ 发布默认；`UiPatch=0` 仅用于回退 A/B。`UiSynthDump=0` 是基线，诊断时才设 1；
- stream 3 / mode 5 关闭，避免额外 UI 编码/解码。

回退
----
- 运行 02_ui_off.bat 或把 fakerFG.ini 的 UiPatch 改回 0，重启游戏。
- 或运行 scripts 目录里的 09a_patch_on.bat 重新应用。
