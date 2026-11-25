# 下一轮计划（2026-11-26 起）

## 当前基线

- CP2077：`keep2x` + 3x 仍是默认画质配置；4x 只作对照，不推荐。
- port 路径已验证**真实生效**（model 15 → 16），但 CP2077 3x 实测不如 keep2x：
  - port `pair lag` 3.30–3.60 帧，keep2x 0.03–0.05；
  - port GPU 出现 8.5/10.6 ms 尖峰，vblanks 有 146；
  - 两者均无 tag/MISMATCH 契约错误。
- 最新部署：`compat14`；port 资源 `xefg_programs` 已放在游戏目录。
- 第二轮链路改动已编译：fakerFG / producer / consumer 均为 API v4（fakerFG 记录侧）。

## P0：先做回归，不再扩大改动面

1. 用 `compat_3x_default.bat` 再跑一次，确认 compat14 后：
   - faker 日志不再出现 Capture 模式的 `FgInput=Upscaler` / `1X` 误报；
   - `pair exact=40 no_slot=0`，无 `MISMATCH` / HUDless 尺寸/格式失败；
   - 3x 输出多数窗口 120.0 fps，输入 38–40 fps 的 `NOT-EXACT` 不再被误判为回归。
2. 起 `lock_frame.exe` 的时机提前到游戏加载前，减少 `producer skip=...` 空窗；
   或把启动等待写进脚本，避免把“消费者未连接”误判成 no_slot。
3. 在 HUDless 成功 tag 的位置加一次性日志（`mode 4/H'` + 尺寸 + patch/fresh），
   只加日志，不改行为；用来确认 patch-only UI 路径每帧真的 tag 了。

## P1：验证本轮新增但没有被 CP2077 覆盖的路径

CP2077 走 `slSetTag` + PCL present-staged，`eval token=0`，因此以下代码本轮未实测：

- P1-A `MgpuFgFrame.stream_cmd[stream]`：需要一个会走 Evaluate 的标题，或合成测试；
- P1-B `StrictFrameToken`：需要 `slSetTagForFrame` 与 Evaluate 同 token 的标题；
- P1-C 高分辨率 MV activation：需要 render≠display 且声明 dilated 的标题；
- P1-E `FGOnlyAcceptFirstHudless`：需要一帧多 HUDless 的标题；
- P1-F UI premultiplied：需要真正 tag UIColorAndAlpha 的标题（当前 patch-only 不 tag UI）。

建议先用 PBZ / Wuchang 做回归，再根据日志决定是否补 synthetic test。

## P1：仍待决策的结构项

1. **Bridge per-resource command list**：本地 XeFG 已按资源 cmd list；bridge transport 仍按
   `record.cmd_list` 提交。跨 list 没有 producer fence，不能直接沿用；需要 transport 端
   增加按资源 fence/独立 list，或继续接受这一安全边界。
2. **P2-A late init flags**：premultiplied/inverted/jittered 在 context 创建时冻结。
   不在运行中重建（会触发旧 facade 生命周期问题）；如果后续必须支持晚到 UI flag，
   先设计 generation-scoped facade，再做重建。
3. **CameraMotionFill**：Witcher 3 需要 RGBA velocity alpha 协议；当前 RG16 transport
   无法填 camera-only 像素。
4. **UIAlpha 单独平面 / UI premultiplied 的完整语义**：patch-only 之外再考虑。
5. **KCD2 waitable 精确不消费**：本轮已换成真实 `FRAME_LATENCY_WAITABLE_OBJECT`，
   仍需实机确认 SL 不再二次消费。

## P2：性能/体验

- 观察 port 实验的 3–4 帧 color lag：若未来还要 port，先解决 consumer present 延迟，
  否则即使模型边缘改善，时间错位仍会盖过收益。
- `producer copywait max=16 ms` 只在启动窗口出现；后续若持续，检查 DMA/COPY queue。
- 3x 的 38.5 fps 输入不追求 4x；若需要严格 120，先把游戏锁 40 fps。
- CP2077 当前 bus 约 45.7 MB/frame（MV 31.6 + depth 14.1）；降低 StreamPercent 前
  先做同场景 A/B，不要为了数字牺牲边缘。

## 不要做

- 不要把 `--mfg-model=port` 改成默认；port 仅保留实验脚本。
- 不要为了 `NOT-EXACT` 切 4x；4x 边缘模糊带更宽。
- 不要在未验证 `stream_cmd` / strict token 的标题上直接改 bridge 行为。
