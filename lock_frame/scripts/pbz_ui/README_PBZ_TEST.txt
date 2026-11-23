PBZ UI 支撑补丁测试（UiPatch=1 + mode 4）
==========================================

基线：`UiPatch=1` + 消费端每通道 32 档 LUT
已知残差：干净像素亮度均值 +0.003、中位 +0.022；RGB MAE 0.032–0.041；色度比 0.894×。

点击顺序
--------
0. 完全退出 PBZ 和旧的 lock_frame.exe。
1. 双击 09a_patch_on.bat：把 patch 基线写进游戏目录 fakerFG.ini
   （UiPatch=1 / UiStream=1 / UiSource=Auto / UiFullRes=1 / UiCodec=1 /
    UiSparseCodec=1 / UiAlpha8=0 / UiPatchWhiteNits=80 / UiSynthDump=0）。
   游戏会重读 ini，所以必须先退出游戏。
2. 双击 00_check.bat（可选）：确认 dxgi/fakerFG/nvngx 字节一致、ini 有 UiPatch=1。
3. 启动 PBZ.exe，读档：
   - DLSS = ON（不是 TAA/TSR；PBZ 内部渲染约 2562x1441 -> 4K）
   - DLSS Frame Generation = ON（否则没有 HUDless/UI tag）
   - HDR = ON（patch 只支持 R10/HDR10；SDR 下 producer 会拒绝并保持 stream 3 off）
   - 锁 40 fps（RTSS 或游戏内上限），目标 3x = 120 fps
   - 画面里要有 HUD 文字/小地图/半透明面板
4. 双击 09_run_3x_patch_mode4.bat，跑 30–60 秒，然后 Ctrl+Alt+End。
5. 双击 06_collect.bat 收集。

产物
----
ui_test\logs\run_patch_<时间>.log
ui_test\logs\tag_patch_<时间>\tag_00..02_backbuffer.bmp   ← 消费端 present（含 UI）
ui_test\logs\tag_patch_<时间>\tag_00..02_hudless.bmp      ← 交给 XeFG 的 H'（LUT 后）
ui_test\logs\tag_patch_<时间>\tag_00..02_patch.bmp        ← 重建素材（LUT 前，偏亮正常）
ui_test\logs\tag_patch_<时间>\tag_00..02_alpha.bmp        ← UI 掩码（白=UI）
ui_test\logs\tag_patch_<时间>\tag_00..02_meta.txt
ui_test\results\<时间>\producer_synth_dump\               ← 仅 UiSynthDump=1 时
ui_test\results\<时间>\summary.txt

日志判据（consumer）
--------------------
- 必须有：`ui-unblend: HUDless rebuilt from ... HUDless patch (source=patch)`
- 必须有：`ui-patch: dynamic gain ready (clean-pixel histogram, 32 bins)`
- 应有：`ui-patch: LUT16 R=... G=... B=... confidence=...`（置信度 >0.9 最好）
- 不应出现：`ui: mode 5`、`FAILED`、`Resource pointer is null`
- 若出现 `UiPatch=1 needs P/H with declared states and an R10 transport`：
  检查 HDR 是否开、DLSS FG 是否真的在 evaluate。

日志判据（producer，游戏目录）
-----------------------------
`PBZ_mfg_producer.log`：
- `producer: UiPatch=1 - stream 3 carries the HUDless scene under the UI support`
- `producer: UI source = HUDless patch under UI (mode 4...)`
- `fr: coded ui ... MB/frame`

回退
----
- 运行 02_ui_off.bat 或把 fakerFG.ini 的 UiPatch 改回 0，重启游戏。
