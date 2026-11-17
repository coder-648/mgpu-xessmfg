# lock_frame 1.0.0 发布说明

**开源发布（GPL-3.0）· Shared v28 · 1.0.0**

## 亮点

- 固定倍率 **2x / 3x / 4x**，用户按面板刷新率选择；输入锁帧值由 UI 计算。
- fakerFG 输入源可选：**DLSSG（推荐） / NVNGX / Upscaler / FSR**。
- 颜色走 WGC，MV/depth 跨卡 150% 传输，消费端联合重建；渲染卡不再支付 4K
  颜色拷贝。
- marker 精确配对；失败时 1X，不猜槽；健康状态机 + 光流降级。
- 环境自检：版本/协议、GPU 数量、XeSS 运行时、游戏配置四项直接显示在 UI。

## 使用前提

- 双 GPU：游戏渲染卡 + 帧生成卡（或至少两块可用适配器）。
- 游戏无边框/窗口化全屏，开启对应 FG 输入 API。
- 120 Hz 面板推荐：2x 锁 60 fps，3x 锁 40 fps，4x 锁 30 fps。
- 需要自备 Intel XeSS 运行时 `libxess_fg.dll` / `libxell.dll`（正式包默认不内置）。
- 不要在带反作弊的在线游戏中使用。

## 已知限制

- 3x/4x 依赖 `--unlock-mfg` 的字节补丁，只对实测过的 `libxess_fg`/`libxell`
  版本有效；补丁失败会回退到可用倍率并打日志。
- 非整除刷新率（如 165 Hz）不承诺完美节奏。
- `NVNGX` / `Upscaler` / `FSR` 输入源为实验性，需要按游戏验证。
- 低帧自适应默认关闭，开启后可能产生轻微 flicker/ghosting。
- 当前包未签名；请用 `SHA256SUMS.txt` 校验。

## 验证指标（4x / 3x 基线，旧称 30x4 / 40x3）

- 4x 长跑：精确配对约 **99.2%**，输出约 **99.77%**，稳态 `show=118~120`。
- 3x 稳态：`exact=40 no_slot=0`，`presents=120/120`。

## 许可

本项目以 GPL-3.0 发布。对应源码见同版本 `lock_frame-1.0.0-src.zip` 或仓库
地址。Intel XeSS、NVIDIA DLSS、FidelityFX、OptiScaler 等第三方组件的许可见
`NOTICE` 与 `LICENSE`。
