CP2077 输入契约补齐（相机 / view / 投影 / rect / extent）—— 测试脚本
====================================================================
日期：2026-11-25

这一轮只改“喂给 XeFG 的帧常量/资源描述”，不改调度、模型、UI、marker。
新增/变更的默认值：
  * viewMatrix    = 由 sl::Constants.cameraRight/Up/Fwd/Pos 构造（原来恒为单位阵）
  * near/far/FOV  = producer 端按 OptiScaler 规则从 cameraViewToClip 反算并发布
  * projection    = 游戏自己的 view_to_clip（缺矩阵时退回 LH camera，再退回旧近似）
  * BACKBUFFER    = 每帧 tag 插值矩形（{0,0,draw_w,draw_h}）
  * HUDless extent= clean stream 4 用真实流尺寸（H' 合成路径仍是链尺寸）
  * present-staged 车道的 mvecScale 补上 ×MV extent

脚本顺序（每次只动一个变量；同一处、同样转速/视角，锁帧按脚本提示）
--------------------------------------------------------------------
  1) ab_50_camvm_default_4x.bat   4x keep2x，新默认（游戏矩阵 + camera view）
  2) ab_51_camvm_rz_4x.bat        4x keep2x，旧投影（--fg-proj-rz）
  3) ab_52_camvm_noproj_4x.bat    4x keep2x，identity 投影（--fg-no-proj，负对照）
  4) ab_53_camvm_default_3x.bat   3x keep2x，新默认（锁 40）
  5) ab_54_camvm_default_2x.bat   2x keep2x，新默认（锁 60）
  6) ab_55_camvm_rz_2x.bat        2x keep2x，旧投影（锁 60）

前置条件
--------
  * 游戏：4K DLAA、DLSS Frame Generation ON、无边框/窗口化全屏；
  * fakerFG.ini 保持你当前配置（FgInput=DLSSG / FgOutput=Capture / MvCodec=auto /
    DepthCodecPercent=150 / UiPatch=1 / HudlessStream=1）；
  * 每个脚本会先做 ab__env.bat 预检（游戏必须已在运行、不能已有 lock_frame.exe）；
  * 结束时 Ctrl+Alt+End（或关窗口）。

怎么判读
--------
  * 50 的日志必须出现：
        proj=game view-to-clip
        view=camera
        camera=1
    （偶发 `(cached)` 正常；一直 cached 说明带 sl::Constants 的记录稀疏）
  * 若 50 出现 `view=identity` → 相机基向量没到，先把这份日志发回来。
  * 50 vs 51：画面是否变化？
        - 完全一样 → model 18 不读投影（与历史结论一致），这项可收束；
        - 有变化   → 投影被消费，记下哪一版边缘更干净。
  * 50 vs 52：identity 通常会明显不同；若三者（50/51/52）几乎一样，投影路径可排除。
  * 53 / 54：确认 3x/2x 下节奏没有被新 view/projection 扰动（2x 是已知能和单卡对齐的档）。
  * 每次日志里顺手确认：
        near/far: ... (camera=1 matrix=1 ...)
        initFlags=0x1 (inverted_depth=1 ...)

跑完
----
  双击 ab_99_camvm_check.bat（不需要游戏在跑）。它会检查：
    - 三份代理 DLL 是否同一份；
    - fakerFG 是否为 v3（`api ready (version 3, record 640 bytes)`）；
    - producer 是否 attach 到 `api v3`；
    - 最近一次会话的 `xefg-in` / `near/far` / `initFlags` 是否符合预期。
  把以下东西发回来：
    ab_logs\ab_50*.log .. ab_55*.log
    游戏目录下 lock_frame_fakerFG.log 的最后 ~200 行
    ab_99_camvm_check.bat 的输出

回滚
----
  游戏和 consumer 全关后，把游戏目录下
    fakerFG.dll.bak_camvm_20261125_071718
    dxgi.dll.bak_camvm_20261125_071718
    nvngx_dlssg.dll.bak_camvm_20261125_071718
    xess_mfg_producer.dll.bak_camvm_20261125_071718
    lock_frame.exe.bak_camvm_20261125_071718
    lock_frame_ui.exe.bak_camvm_20261125_071718
  分别拷回原名即可。

注意
----
  * `--fg-use-matrix` 现在是历史别名（游戏矩阵已是默认）；A/B 请用 --fg-proj-rz / --fg-no-proj。
  * `--fg-far-finite` 只影响“没有新鲜游戏矩阵”的退回路径。
  * 这一轮不要叠加 --fg-gate / --interp-start / --mfg-replace-all / --fg-debug-*。
