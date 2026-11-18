# lock_frame 1.1.0 发布说明

**开源发布（GPL-3.0）· Shared v28 · 1.1.0**

## 本版重点：帧节奏控制重做（v29 + v30）

- **测速改用游戏自己的时钟**：`frameRenderTime` 与节奏判定改用 producer 发布的
  `FrameShared.frame_qpc_ms`（1 ms 分辨率）。WGC 的 `SystemRelativeTime` 被量化到整
  vblank——39 fps 和 42 fps 都读成 25 ms，这是旧版"每批尾部小停顿"的根因。
- **`--fg-rt-mode auto` 改成连续量**（不再是 quant/raw 二选一）：
  `target = max(m·vblank, 中位数)`，2% 死区，**每批最多 +0.5 ms、恢复时立即回落**。
  40 fps → 25.0 ms（1 张/vblank）；进入 33 fps 场景 → 约 0.35 s 平滑爬到 31 ms；
  回到 40 → 立即 25.0。**场景切换不再有模式跳变。**
- **默认开启时隙抽帧门**：输入高出目标 2% 以上时按网格丢帧（漏桶，均匀分布，
  丢在颜色拷贝之前）。2% 以内不丢——39–40 fps 的工作点不再被误伤。
- **OSD / 报告**：`in fps`、`game ms` 改用游戏时钟；每个统计窗口新增
  `game clock: in … target … kept/dropped …` 与 `in(vb)`（0.5 vblank 分桶）。

## UI 变更

- **移除"低帧自适应"开关**（旧算法）：它会传 `--interp-cadence`，让时隙抽帧门
  让位并每批换倍率（3x/4x 闪烁/重影）。现在界面只暴露固定倍率 **2x / 3x / 4x**，
  新算法即默认路径。`--interp-cadence` 仅保留为命令行 A/B。
- "MV/深度重建"一行改为按 `fakerFG.ini` 的 `StreamPercent` 显示（不再写死 150%）。

## 修复

- `presentation is not vblank-paced` 误报：现在只有显示帧率也明显低于批次应产出才报。
- `cadence:` 日志里的假数据：`interp=0`（3x 显示成 1X）与 `out=120`（实际 112）已修正。

## 实测（Wuchang: Fallen Feathers，4K，120 Hz，3x）

- 输入 32.8–40.1 fps，39 个统计窗口：`rt ≥ m·vblank` 与 `rt ≤ 输入+1 ms`
  **全部成立（0 次超额请求）**，`dropped=0`，运行中 0 次配对失败。
- 40 fps 场景稳定 120 fps；33 fps 场景 97–107 fps（= 3 × 输入）。
- 主观：**"整体稍慢但是顺"**（用户确认）。

## 使用前提（同 1.0.0）

- 双 GPU；游戏无边框/窗口化全屏并开启对应 FG 输入 API。
- 120 Hz 面板：2x 锁 60 / 3x 锁 40 / 4x 锁 30。
- 自备 Intel `libxess_fg.dll` / `libxell.dll`（正式包默认不内置）。
- 不要在带反作弊的在线游戏中使用。

## 已知限制

- 3x/4x 依赖 `--unlock-mfg` 的字节补丁，只对实测过的 `libxess_fg`/`libxell` 版本有效。
- 非整除刷新率（如 165 Hz）不承诺完美节奏。
- 输入持续低于目标时，输出 = 倍率 × 真实输入（例如 33 fps + 3x ≈ 100 fps），
  慢场景的"稍慢"是算术结果；要消除只能 VRR 或提高 GPU 余量。
- **不要用 `--slack` / `--slack-auto`**：实测更差（hold 会让两批背靠背交给 XeFG）。
- 当前包未签名；请用 `SHA256SUMS.txt` 校验。

## 升级说明

本版 **consumer 与游戏侧 DLL 都要更新**（`fakerFG.dll` / `dxgi.dll` /
`xess_mfg_producer.dll` / `lock_frame.exe` / `lock_frame_ui.exe`），协议仍是 v28。
`fakerFG.ini` 不需要改；UI 的 `lock_frame_ui.ini` 里旧的 `cadence` 键会被忽略。

## 许可

GPL-3.0。对应源码见同版本 `lock_frame-1.1.0-src.zip`。Intel XeSS、NVIDIA DLSS、
FidelityFX、OptiScaler 等第三方组件的许可见 `NOTICE` 与 `LICENSE`。
