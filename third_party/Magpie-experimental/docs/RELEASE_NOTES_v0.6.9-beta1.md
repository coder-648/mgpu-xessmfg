# Magpie Experimental v0.6.9 Beta 1

本地测试版，基于 `experimental` 主线整合全部七个 0.6.9 功能分支。

## 功能更新

### 界面与操作

- **快捷键清除**：工具栏快捷键右侧新增垃圾桶按钮，全部八项快捷键的编辑弹窗支持“保存／清除／取消”。清除后显示“未设置”，立即解除绑定并跨重启保留；重新设置仍检查快捷键冲突。
- **工具栏拖拽**：通过专用拖拽柄移动工具栏，支持顶部／底部停靠及沿边缘左右移动，拖动时显示蓝色停靠提示。默认配置和各应用配置分别保存窗口／全屏模式的上下位置；横向位置仅在本次运行保留，新运行居中。
- **光标刷新策略**：默认配置与应用配置新增“优先随原始内容帧更新”和“保持光标最低刷新率”。原始帧优先默认关闭，最低刷新率默认开启、60 FPS，可自行调整；关闭补充刷新后可严格随原始内容帧更新。
- **主页常驻选项**：重复帧过滤与开发者选项入口常驻主页。关闭开发者模式不再重置过滤设置，FG 效果参数继续覆盖主页全局过滤设置。

### 效果器与处理管线

- **五个 xBR 效果**：新增 xBR 2x、3x、4x、NoBlend 3x 和 Hybrid 2x，适用于像素画和低分辨率 2D 画面；保留各算法的授权说明。
- **重复帧处理优化**：重复输入复用已有结果，通过输入／输出修订传播避免重复执行后续效果；兼顾同帧调参、HDR 和捕获尺寸变化，保留已接受捕获帧的时间与身份信息。
- **DLSSNR Multi Pass 优化**：共用引导、外围资源和 D3D12 调度。SDR 输入分辨率调整路径只在入口降采样一次，多层在推理尺寸串联，出口统一控制并重建总残差；各层 NR 参数和模型历史仍独立。
- **DLSSNR 细节控制**：统一使用总残差控制，旧配置保留参数数值并自动迁移。总强度支持 0–2；新增色相变化保护、暗部保护、高光保护、局部失控压缩、大范围／细节修正强度、色度时域稳定及诊断视图。中性频率设置跳过额外分解，纯后处理调参复用 NR 结果。

## 使用与验证边界

- 多层低分辨率串联与统一总残差会改变旧版本的画面行为；旧非默认颜色参数迁移后可能呈现不同观感。
- 已有测试在极小 DLSSNR 推理尺寸（例如 32×18）观察到设备挂起，旧单层管线也可复现；模型最小尺寸边界尚未确定。详见 [DLSSNR 测试记录](experimental/reviews/20261001-v0.6.9-dlssnr-overhead.md)。
- 光标最低刷新率是调度目标，不是最高帧率，也不表示所有场景均达到该值。
- CPU／软件 WARP 自动回归、完整 Release x64 构建与包校验分别记录。真实 NVIDIA GPU 的 NGX 多层调度、性能／画质，以及原生 UI、实际热键、工具栏拖拽和多显示器验收仍需实测。
- 暂停的 `feature/vrr-presentation-paused` 不属于 0.6.9 功能范围，本包未纳入该分支。

---

# Magpie Experimental v0.6.9 Beta 1

Local test release integrating all seven 0.6.9 feature branches into `experimental`.

## Updates

### Interface and controls

- **Clear shortcuts**: toolbar shortcut rows gain a trash button, and all eight shortcut dialogs offer Save / Clear / Cancel. Cleared bindings immediately become “Not set” and remain unset after restarting; rebinding retains conflict checks.
- **Draggable toolbar**: use the dedicated handle to move the toolbar along the top or bottom edge, with blue docking guides. Default and application profiles save top/bottom docking separately for windowed and fullscreen modes; horizontal position lasts for the current session and starts centered next time.
- **Cursor refresh policy**: default and application profiles gain “Prefer original content frames” and “Maintain minimum cursor refresh rate.” Original-frame preference defaults off; minimum refresh defaults on at an editable 60 FPS. Disable supplemental refresh to update strictly with original content frames.
- **Permanent home options**: duplicate-frame filtering and the developer-options entry remain visible on Home. Disabling developer mode no longer resets filtering, and FG effect parameters continue to override the global Home setting.

### Effects and processing

- **Five xBR effects**: xBR 2x, 3x, 4x, NoBlend 3x and Hybrid 2x for pixel art and low-resolution 2D content, with the original license notices retained.
- **Duplicate-frame optimization**: reuse existing results for duplicate inputs and propagate input/output revisions to avoid repeated downstream work. Same-frame parameter updates, HDR and capture resizing retain the accepted capture identity and timing.
- **DLSSNR Multi Pass optimization**: share guidance, surrounding resources and D3D12 scheduling. The SDR input-resolution-adjustment path downsamples once at entry, chains passes at inference resolution, and controls/reconstructs the total residual once at exit; NR parameters and model history remain independent per pass.
- **DLSSNR detail controls**: all configurations use unified total-residual controls; existing numeric values are preserved during automatic migration. Total strength supports 0–2. New controls include hue-change protection, dark/highlight protection, local correction compression, broad/detail correction strength, chroma temporal stabilization and diagnostic views. Neutral frequency settings skip the extra decomposition; postprocessing-only changes reuse NR results.

## Compatibility and validation

- Low-resolution multipass chaining and unified total-residual processing change the image behavior of previous versions. Existing nondefault color settings may look different after migration.
- Existing tests observed a device hang at very small DLSSNR inference sizes (for example, 32×18), also reproducible on the earlier single-pass pipeline. The model's minimum supported size remains undetermined; see the [DLSSNR test record](experimental/reviews/20261001-v0.6.9-dlssnr-overhead.md).
- Minimum cursor refresh is a scheduling target, not a maximum frame rate or a measured guarantee in every scenario.
- CPU/software-WARP regression checks, the full Release x64 build and package verification are recorded separately. Real NVIDIA NGX multipass scheduling, performance/image quality, native UI, system hotkeys, toolbar dragging and multi-monitor acceptance still require testing.
- The paused `feature/vrr-presentation-paused` branch is outside the 0.6.9 feature scope and is excluded.
