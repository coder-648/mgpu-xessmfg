# 下一轮计划（2026-11-26 起）

## 0. 2026-11-25 追加（consumer 自身链路修正，已实机回归）

`SELF_AUDIT_2026-11-25.md` 的 A/C/D/E/F 已落地并重新构建（源码级，版本号仍
`1.3.1-local.camvm`，二进制 md5 与已验证包不同）。CP2077 4K / 40 fps / 3x keep2x
三次 run（基线 / UI patch / `--d3d-debug`）结果见审查文档 §8：

- A/C/E 无回归：`pair: exact=40 no_slot=0` 全程、`xefg-in` MV/depth 都是 3840x2160、
  `wait=0.0ms`、`pf:` 0 行、`xN=3.00`、`slow/timeouts=0`、`out 120/120`；
- D 已用 debug layer 验证：1045 条消息里 0 条 barrier/state/invalid-call
  （其余是 XeFG 的 `CheckFeatureSupport` 与 6 条 upload buffer `GENERIC_READ` 提示）；
- F 未被执行到（本轮没有 `--lock/--jbuf`）。

新发现（详见 §3 Q / §4 R）：

- **Q**：把 `lock_frame.exe` 放在游戏目录会让 consumer 进程也加载 fakerFG
  （第二个 XeFG context + XeLL + Present 钩子）。后续 A/B 统一用
  `C:\temp\lock_frame_audit\` + `audit_out_3x_keep2x.bat`（发布包布局）。
- **R**：consumer 退出后 `request_mask` 残留，producer 继续每帧编码/拷贝两路流，
  直到游戏重启。建议 producer 用 `kConsumerPresentEvent` 是否仍存在来判活。

仍未做：G/H/I（显存 / SDR / UI 组合边界）、K（producer copy ring）、L（LUT 间隔）、
M（marker 日志量）、O（>3840 渲染的纹理档静默跳过）、P（`--fg-colour shared` 失效路径）、
F（`--lock`+`--jbuf` 泄漏的实机验证）。

### 2026-11-25 晚追加（B/J/R，已实现并实机确认）

实测（21:39–21:43，4K/40/3x，三次 run，详见审查文档 §10）：

- **J** 映射 1773.6 → **1017.6 MB**（-43%）；
- **B** `reclaimed oldest slot` 0 次，`no safe slot` 只在 run 开始/结束，稳态零丢帧；
- **R** 退出后 3 s 宽限 + 5 s 一次保活，不再连续编码（更长的空闲窗口待补测）；
- 回归全绿：`exact=40 no_slot=0`、`xN=3.00`、`out 120/120`、`wait=0`、`slow/timeouts=0`；
  debug 层 0 条 barrier 校验。

新增待办：
- `record: no slot for seq=...` 在退出窗口最多刷 300 行（本轮 76 行），建议降频；
- `TransportSlots=16` 在 debug 层下 `lag max=13`（watermark 上限 14），
  若某标题稳态出现 `no safe slot` / `pair no_slot`，调到 24/32；
- **Q 的干净布局仍未实测**：本轮还是从游戏目录启动的 consumer
  （`lock_frame_fakerFG.log` 有 21:41:39 / 21:42:16 / 21:43:16 三个 pid）。
  下次至少跑一次 `audit_out_3x_keep2x.bat`。

实现说明：
- **B** `claim_slot()` 只覆盖安全槽（`fid==0` 或 `fid+2 <= colour_seq`），其余丢帧；
  日志由 `reclaimed oldest slot` 变为 `reclaimed SAFE slot` / `no safe slot to reuse`。
- **J** `TransportSlots=16`（`fakerFG.ini`，4..32）。
- **R** producer 用 `consumer_session` 高 32 位 pid 判活：3 s 宽限后停止拷贝，
  每 5 s 一次保活发布。
- 部署：`xess_mfg_producer.dll`（**需重启游戏**）、`lock_frame.exe`
  （游戏目录 + `C:\temp\lock_frame_audit\`）。md5 每 rebuild 都变，
  本轮值见 `audit_logs\audit_deploy_info.txt`。

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
