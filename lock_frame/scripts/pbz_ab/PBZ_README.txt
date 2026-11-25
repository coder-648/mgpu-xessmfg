PBZ（影之刃零）人物边缘伪影 —— 测试脚本
========================================

背景（2026-11-25 实测，3x，新构建）
----------------------------------
- 屏幕边缘伪影基本消失（viewMatrix 生效，和 CP2077 一致）；
- 人物模型边缘仍有残留；
- 显式 `--fg-of`（AMD 光流）后**表现相同** ⇒ **“PBZ MV 不可用”的旧结论撤回**，
  残留不在 MV 来源。

所以下面按“模型相位 → 原生网格 → 符号/抖动”的顺序做对照，每次都只动一个变量。

测试顺序（同一处、同样转速；锁帧按脚本提示）
--------------------------------------------
1) pbz_14_game_mv_2x.bat      2x，t=0.5，游戏 MV（模型相位对照）
2) pbz_10_game_mv_3x.bat      3x，游戏 MV（基准）
3) pbz_18_port_3x.bat         3x，port model 15（需要 xefg_programs\）
4) pbz_20_render_grid_3x.bat  3x，原生 render 网格（--no-upsample，需先把 depth 设 100 并重启）
5) pbz_16_mv_flip_3x.bat      3x，--mv-flip（符号对照）
6) pbz_17_jitter0_3x.bat      3x，--fg-jitter-zero（抖动契约对照）
7) pbz_11_of_3x.bat           3x，--fg-of（MV 来源对照；已知人物边缘相同）

判读
----
- 14（2x）人物边缘干净、10（3x）不干净 ⇒ 残留是 **keep2x 在 t=1/3,2/3** 的模型限制；
  再看 18（port）是否更好。若 port 更好，3x 可显式用 port（4x 不要，历史实测更差）。
- 14 也不干净 ⇒ 不是 t 相位；看 20（原生 render 网格）与 16/17。
- 20 的日志应出现 `xefg-in: mv=2562x1441 depth=2562x1441`，且**没有**
  `--upsample: MV/depth rebuilt ... to 3840x2160`；如果仍有 rebuilt 行，说明 ini 没到 100%
  或游戏没重启。
- 16/17 只作对照，不要作为默认（CP2077 上两者都更差）。

每个脚本的日志在 pbz_ab\logs\；跑完把这些日志 + 当次 `PBZ_fakerFG.log` /
`PBZ_mfg_producer.log` 的尾部发回来。`pbz_00_check.bat` 会汇总二进制/API/最近几次运行。

注意
----
- `pbz__env.bat` 用 `--game PBZ.exe` 找窗口；游戏必须已在运行，且不能已有 lock_frame.exe。
- `--fg-of` 仍是 MV 来源的 A/B，但**不再作为 PBZ 的默认修复**。
- 20 需要把 `fakerFG.ini` 写到：
      CodecStreamPercent=100 / MvCodecPercent=100 / DepthCodecPercent=100
  （UI：MV/深度传输比例 = 100%，深度码流 = 100%），然后**完全重启游戏**。
