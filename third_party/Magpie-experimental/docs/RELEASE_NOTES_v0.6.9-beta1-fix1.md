# Magpie Experimental 0.6.9 Beta1 fix1

部署到现有 Beta1 目录，保留原 Beta1 恢复副本。

- 所有效果器参数浮层允许超出主窗口，按所在显示器工作区计算可用尺寸；保留可读列宽，空间不足时滚动。
- DLSSNR 配置页采用“细节控制｜进阶调整｜Pass 1｜启用的后续 Pass”。进阶调整默认隐藏，开关放在细节控制中；诊断视图归入进阶调整。收起保留参数数值和效果。
- 参数说明改为名称悬停提示，包含实际作用、范围、默认值和步进，补齐英文、简中、繁中。运行时参数面板保留纵向分组。
- 统一残差修正算法，移除旧颜色模式选择和运行分支。旧配置自动迁移并保留合法参数数值；非默认颜色参数的观感可能变化。
- 工具栏按钮统一尺寸、图标居中，修正 FPS 对齐及小窗口下的圆角缩放；底部菜单和提示向输出区域内展开。
- 顶部／底部拖拽增加左右各 12 个实际像素的水平居中吸附。吸附时显示贯穿输出区域的青色 2px 竖线，松手、离开或取消立即消失。
- 保留原有窗口／全屏上下停靠保存规则；横向位置仍只在本次运行保留。

验证包括配置与显隐、残差／时域 WARP、实际 Segoe 图标字体的 ImGui 几何、居中吸附及输入回归。完整构建、真实启动和 NVIDIA GPU 管线结果见配套部署审核记录。多显示器浮层摆放、实际游戏画面与完整鼠标视觉验收按实际证据记录，不由无窗口测试替代。

---

All effect parameter popups can extend beyond the main window and size against the monitor work area. DLSSNR adds a hidden-by-default Advanced Adjustments group containing diagnostics, compact names with localized hover help, and one residual algorithm with preserved numeric settings. The runtime panel keeps vertical groups.

Toolbar buttons use uniform frames and centered icons/FPS. Top and bottom dragging snaps to the output center within 12 physical pixels; a 2-pixel cyan guide appears only while the drag is snapped. Existing dock persistence and session-only horizontal positions are retained.

Existing nondefault color settings may look different after migration. Automated layout/shader/input tests do not replace native multi-monitor popup or real-content visual acceptance.
