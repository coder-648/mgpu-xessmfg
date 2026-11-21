CP2077 XeSS-FG 测试脚本 —— 最后两个入口（2026-11-22）
====================================================

已确定：XeFG 执行、无损传输、配对算法都没问题；模型选 keep2x（model 18，所有档位）。
剩下两个入口（都在 4x 上测，边缘伪影最明显）：

  A. 颜色入口：WGC 抓的颜色 vs 游戏自己的后缓冲
  B. 输入质量：MV 来源（DLSSG 车道 vs Upscaler 车道）、以及 keep2x 到底用不用深度

脚本与步骤
----------
基线：ab_24_keep2x_4x（WGC + DLSSG 车道 + 深度）

  1) ab_40_keep2x_4x_tearing.bat        （无需重启）呈现路径 ALLOW_TEARING
  2) ab_43_keep2x_4x_nodepth.bat        （无需重启）keep2x 下不用深度

  3) 颜色入口（需要重启游戏）：
     游戏目录双击 use_colour_v30.bat   -> 完全退出游戏 -> 重新进游戏读档
     点 ab_41_keep2x_4x_colour_shared.bat
     测完：游戏目录双击 use_wgc_v30.bat -> 重启游戏

  4) MV 入口（需要重启游戏）：
     游戏目录双击 use_upscaler_mv.bat  -> 完全退出游戏 -> 重新进游戏读档
     点 ab_42_keep2x_4x_upscaler_mv.bat
     测完：游戏目录双击 use_dlssg_mv.bat -> 重启游戏

（use_colour_v30 / use_wgc_v30 / use_upscaler_mv / use_dlssg_mv 都在游戏目录，自动备份 fakerFG.ini）

判据（每条只看边缘伪影 + 日志里的对应行）
----------------------------------------
- 40 vs 24：呈现路径影响吗？（ALLOW_TEARING vs DWM 合成）
- 41 vs 24：颜色换成游戏后缓冲后边缘是否变化？
    * 变干净 -> WGC 的颜色（合成/缩放/时延）是入口；后续可评估常驻 colour stream 的代价（渲染卡约 +6%）。
    * 没变化 -> 颜色入口排除，焦点转到 MV。
    日志应有：[lock] --fg-colour shared: colour comes from the add-on's stream[0]，bus 里出现 colour 字节。
- 42 vs 24：MV 换成 Upscaler 车道后是否变化？
    * 变化 -> MV 来源是入口；可进一步比对两条车道的 MV 尺寸/单位（日志 producer semantics / valid=WxH）。
    * 没变化 -> MV 来源不是主因，剩下 MV 与颜色的"时间对齐/质量"需要更强的测量（残差探针）。
- 43 vs 24：无深度时边缘是否变化？
    * 不变 -> model 18 不读深度，可以永久省掉 7.9 MB/frame（并且少一次 tag/upsample）。
    * 变差 -> keep2x 也用深度，保留。

跑完点 ab_90_summary.bat，把 ab_logs + summary.txt 发回来。

补充：这一轮不要混入别的改动（--fg-gate / --interp-start / --mfg-replace-all / --fg-debug-* 都不要加）。
