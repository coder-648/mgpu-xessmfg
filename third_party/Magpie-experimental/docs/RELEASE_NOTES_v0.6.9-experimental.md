# Magpie Experimental v0.6.9

本版更新了帧率与刷新设置、工具栏操作、DLSSNR 细节控制和效果器说明，并新增五个 xBR 效果。

## 功能更新

### Magpie 本体易用性更新

#### 帧率与刷新

- 默认配置和应用配置现在统一使用 **“帧率与刷新”**，可在同一处调整内容帧率、节奏控制、光标刷新和空闲重绘。主页提供默认配置的摘要与编辑入口。
- 内容帧率支持 **跟随源／自动／自定义**，节奏控制支持 **Front Edge／Async／NVIDIA Reflex**。自动模式根据显示器刷新率和补帧倍率选择基础目标；跟随源模式按源内容的更新节奏处理。
- 光标刷新支持 **响应优先／仅随原始内容帧／原始帧优先并补充**。补充刷新率可自动跟随显示器，也可自定义最低目标；光标状态变化时按设置补充刷新。
- 内容、光标和空闲重绘的手动值统一为 **15–360 FPS、整数、步进 1**。新配置和恢复默认使用以下设置，已有有效设置会保留。

| 设置 | 新配置默认值 |
| --- | --- |
| 内容帧率 | 自定义 60 FPS |
| 节奏控制 | Front Edge |
| 光标刷新 | 原始帧优先并补充，最低 60 FPS |
| 空闲持续重绘 | 开启，30 FPS |

- 空闲重绘位于默认折叠的高级选项中，实际帧率受内容上限限制；开启补帧后暂停空闲重绘，但保留设置。
- 改进 Reflex 的节奏控制，以及 Front Edge 模式下低内容帧率时的光标补充刷新。
- 设置自动保存，重新启用缩放后生效；缩放时也可在参数面板中调整并选择“保存并重启”。新建应用配置复制默认配置或所选模板，之后可独立调整。

#### 效果组操作

- 恢复列表下方的 **“＋新建效果组”** 按钮。
- 右上角 **“其他选项”** 集中提供导入、导出、配置文件夹和重置，按钮的文字与留白更协调。
- “配置文件夹”可直接打开当前使用的设置目录；重置仍需确认。

#### 清除快捷键

- 快捷键设置增加清除按钮，编辑弹窗支持保存、清除和取消。A
- 清除后显示“未设置”，重启后仍保持；需要时可重新设置。修复快捷键相关的启动问题。

#### 工具栏拖动与显示

- 按住工具栏 **空白或 FPS 区域** 即可拖动，可停靠在顶部或底部，也可沿边缘左右移动。
- 靠近画面水平中心时自动吸附，并显示对齐提示。
- 上下停靠位置按配置和窗口／全屏模式保存；左右位置仅在本次运行保留，下次重新居中。
- 调整按钮、图标和 FPS 的对齐；底部停靠时，菜单和提示向画面内展开。

#### 参数窗口与主页

- 效果器参数窗口可使用主窗口之外的屏幕空间，内容较多时可滚动，减少拥挤和遮挡。
- DLSSNR 按细节控制、进阶调整和各 Pass 分组显示。进阶调整默认隐藏，收起后仍保留参数与效果；悬停效果名称可查看简短说明。
- 全局重复帧检测移至 **帧率与刷新 → 高级选项**；补帧效果的重复帧过滤仍在各自参数中设置。开发者选项入口保留在主页。

#### 效果器选择说明

- 更新全部 **162 项内置效果器**的中英文说明，帮助了解用途、适合素材、变体差异、效果链位置和使用限制。
- 分类和推荐标签更清楚，旧配置与搜索名称保持兼容。推荐标签帮助按用途选择效果。

#### 虚拟显示环境下的显卡选择

- 修正 Sunshine 等虚拟显示环境中误选显示设备进行渲染的问题。

### 效果器更新

#### 五个 xBR 效果

新增 **xBR 2x、xBR 3x、xBR 4x、xBR NoBlend 3x、xBR Hybrid 2x**，适合像素画和低分辨率 2D 内容，可按所需倍率和边缘风格选择。

#### DLSSNR Multi Pass

- 改进多 Pass 与输入分辨率调整的处理，减少重复工作；仍支持 **1／2／3 次处理**，默认 1 次，各 Pass 可独立调整 NR 参数。
- 细节控制统一作用于全部 Pass 完成后的整体修正，调整多层效果时更容易控制最终观感。

#### DLSSNR 细节控制

- 简化颜色与细节参数，统一提供总强度、色度强度、明度总强度、阴影／结构强度和高光／辉光强度。
- 进阶调整提供色相／暗部／高光保护、过度修正抑制、低频／高频修正强度和诊断视图。默认隐藏，展开、收起均保留参数。

| 参数 | 范围 | 默认值与调整方式 |
| --- | --- | --- |
| 五项基础强度 | 0–2 | 默认 1，步进 0.05；总强度为 0 时关闭修正叠加 |
| 保护与过度修正抑制 | 0–1 | 默认 0，步进 0.05；0 为关闭 |
| 低频／高频修正强度 | 0–2 | 默认 1，步进 0.05 |
| 输入分辨率调整 | 25–100% | 默认关闭、100%，百分比步进 1；关闭时使用原输入尺寸 |
| 诊断视图 | 下拉选择 | 默认显示最终图像 |

- 保留抗闪烁功能，旧参数会自动迁移。新的多层处理和细节控制可能改变旧版观感，升级后可按实际画面重新调整。

#### 重复帧处理与帧率过滤

- 减少重复画面的重复处理，并改进 HDR 下的重复帧处理。
- FrameRate Filter 的跟随选项改为 **“跟随内容帧率目标”**，已有设置继续兼容。

## 应该下载哪个文件？

**所有用户都下载完整主包 `Magpie-Experimental-x64.zip`。** 无论从旧版或本地 Beta 升级，还是首次安装，步骤相同：

1. 升级前备份设置，并从托盘完全退出 Magpie。
2. 将 ZIP **完整解压到新目录**，再运行其中的 `Magpie.exe`。
3. 保留原程序与配置备份，以便需要时回退。

普通安装读取 `%LOCALAPPDATA%\Magpie\config\v4e\config.json`；没有 v4e 时会导入旧 v4 设置。便携用户可将原设置复制到新程序目录的 `config\v4e\config.json`。

### 附件的作用与使用

| 附件 | 用途与使用方法 |
| --- | --- |
| `Magpie-Experimental-x64.zip` | **必选完整主包**，包含程序、界面资源、效果器与运行组件。完整解压后使用。 |
| `DLSSNR-DLL-Options-310.8.0.0.zip` | **可选 DLSSNR DLL 包**，提供 NVIDIA 官方版、RTX 40/50 社区兼容版与 SF-v2。需要切换 NR DLL 时下载；完全退出 Magpie，备份现有 DLL，按包内中英说明选择一个版本放到 `Magpie.exe` 旁。 |
| `NGX_OTA_Switch.bat` | **可选 NGX OTA 设置工具**，在需要查看或切换 NGX OTA 设置时使用。相关操作需管理员权限并影响系统级设置；使用 **Restore default** 恢复设置。 |

## 使用提示

- 光标最低刷新是补充目标，实际速率受显示器与当前效果组合限制。空闲重绘用于刷新已有画面，可能增加功耗。
- DLSSNR 输入分辨率调得过低可能导致异常；遇到问题先关闭输入分辨率调整，或提高推理尺寸。
- 时域超分和补帧依赖捕获画面估算运动，快速运动、遮挡和界面区域可能出现拖影或细节不稳定，请按实际场景选择效果与参数。
- 反馈问题时请附上日志与复现步骤。需要回退时使用旧完整包及对应配置备份。

Contributor: [liaanj](https://github.com/liaanj) 提供虚拟显示环境下的显卡筛选修正（PR #62）。

---

# Magpie Experimental v0.6.9 User Guide

This release updates frame-rate and refresh settings, toolbar controls, DLSSNR detail controls and effect descriptions, and adds five xBR effects.

## Updates

### Magpie usability updates

#### Frame rate and refresh

- Default and application profiles now share **Frame rate and refresh** settings for content rate, pacing, cursor refresh and idle redraw. Home provides a summary and an edit entry for the default profile.
- Content modes are **Follow source / Automatic / Custom**; pacing options are **Front Edge / Async / NVIDIA Reflex**. Automatic chooses a base target from the display refresh rate and frame-generation multiplier. Follow source processes content according to its update cadence.
- Cursor modes are **Response first / Original content frames only / Prefer original frames with supplementation**. Supplemental refresh can follow the display automatically or use a custom minimum. Cursor changes use the selected supplemental refresh policy.
- Manual content, cursor and idle rates use **15–360 FPS**, integer values and 1-FPS steps. New profiles and restored defaults use the settings below; valid existing settings are retained.

| Setting | New-profile default |
| --- | --- |
| Content rate | Custom, 60 FPS |
| Pacing | Front Edge |
| Cursor refresh | Prefer original frames with supplementation, minimum 60 FPS |
| Continuous idle redraw | Enabled, 30 FPS |

- Idle redraw is in the initially collapsed advanced options. Its effective rate is limited by the content ceiling. Frame generation pauses idle redraw while retaining its settings.
- Improve Reflex pacing and supplemental cursor refresh at low content rates in Front Edge mode.
- Settings save automatically and apply after restarting scaling. During scaling, you can also edit them in the parameter panel and choose Save and restart. New application profiles copy the default profile or a selected template, then remain independently adjustable.

#### Effect-group actions

- Restore **+ New effect group** below the list.
- **Other options** in the header provides Import, Export, Configuration folder and Reset. Its label and spacing better match surrounding controls.
- Configuration folder opens the active settings directory. Reset still requires confirmation.

#### Clear shortcuts

- Shortcut settings gain a clear button, and edit dialogs offer Save, Clear and Cancel.
- Cleared shortcuts display Not set and remain cleared after restarting. You can assign a new shortcut when needed. Fix shortcut-related startup issues.

#### Toolbar dragging and display

- Drag from the toolbar's **blank or FPS areas** to dock at the top or bottom, or move horizontally along the edge.
- The toolbar snaps near the horizontal center and shows an alignment guide.
- Vertical docking is saved per profile and windowed/fullscreen mode. Horizontal placement lasts for the current run and starts centered next time.
- Improve button, icon and FPS alignment. Menus and tooltips open toward the picture when the toolbar is docked at the bottom.

#### Parameter windows and Home

- Effect parameter windows can use screen space outside the main window and scroll when needed, reducing crowding and overlap.
- DLSSNR groups controls by Detail Control, Advanced Adjustments and individual passes. Advanced starts hidden; collapsing it retains values and processing. Hover over the effect name for a brief description.
- Global duplicate-frame detection moves to **Frame rate and refresh → Advanced**. Frame-generation effects retain their own duplicate-filter settings. Developer options remain on Home.

#### Effect-selection guidance

- Update Chinese and English descriptions for all **162 built-in effects**, explaining purpose, suitable content, variants, chain placement and limitations.
- Clearer categories and recommendation labels help you choose effects. Existing configuration and search names remain compatible. Recommendation labels help you choose effects by purpose.

#### GPU selection with virtual displays

- Fix selection of display-only devices for rendering in Sunshine and similar virtual-display environments.

### Effect updates

#### Five xBR effects

Add **xBR 2x, xBR 3x, xBR 4x, xBR NoBlend 3x and xBR Hybrid 2x** for pixel art and low-resolution 2D content. Choose according to your desired scale and edge style.

#### DLSSNR Multi Pass

- Improve multi-pass and input-resolution processing to reduce repeated work. **1 / 2 / 3 passes** remain available, defaulting to 1, with independent NR settings for each pass.
- Detail controls affect the overall correction after all passes, making the final appearance easier to adjust across multiple passes.

#### DLSSNR detail controls

- Simplify color and detail settings with Overall, Chroma, Overall Lightness, Shadow/Structure and Highlight/Glow Strength controls.
- Advanced Adjustments offers hue/shadow/highlight protection, overcorrection suppression, low/high-frequency strength and diagnostic views. It starts hidden; expanding or collapsing it retains values.

| Parameter | Range | Default and adjustment |
| --- | --- | --- |
| Five basic strengths | 0–2 | Default 1, step 0.05; Overall 0 disables correction blending |
| Protection and overcorrection suppression | 0–1 | Default 0, step 0.05; 0 disables it |
| Low/high-frequency strength | 0–2 | Default 1, step 0.05 |
| Input resolution adjustment | 25–100% | Disabled by default, 100%, percentage step 1; Off uses the original input size |
| Diagnostic View | Selection | Final image by default |

- Anti-flicker remains available, and existing parameters migrate automatically. New multi-pass processing and detail controls may change the previous appearance; adjust them with actual content after upgrading.

#### Duplicate-frame processing and rate filtering

- Reduce repeated processing of identical frames and improve duplicate-frame handling with HDR.
- FrameRate Filter's inherited target is now labelled **Follow content frame rate target**, retaining compatibility with existing settings.

## Which file should I download?

**All users should download the complete `Magpie-Experimental-x64.zip`.** The steps are the same whether upgrading from an older release or local Beta, or installing for the first time:

1. When upgrading, back up your settings and fully exit Magpie from the system tray.
2. **Extract the entire ZIP into a new folder**, then run its `Magpie.exe`.
3. Keep the original application and settings backup for rollback.

Normal installations use `%LOCALAPPDATA%\Magpie\config\v4e\config.json`, importing legacy v4 settings if v4e is absent. Portable users can copy their existing settings to `config\v4e\config.json` in the new program folder.

### Assets: purpose and instructions

| Asset | Purpose and instructions |
| --- | --- |
| `Magpie-Experimental-x64.zip` | **Required complete package**, with the application, UI resources, effects and runtime components. Extract it fully before use. |
| `DLSSNR-DLL-Options-310.8.0.0.zip` | **Optional DLSSNR DLL package** with official NVIDIA, community RTX 40/50-compatible and SF-v2 variants. Download only to switch NR DLLs. Fully exit Magpie, back up the existing DLL, and follow the Chinese/English instructions to place one variant beside `Magpie.exe`. |
| `NGX_OTA_Switch.bat` | **Optional NGX OTA settings tool** for use when you want to inspect or change NGX OTA settings. Relevant operations require administrator privileges and affect system-wide settings. Use **Restore default** to restore settings. |

## Usage tips

- A cursor minimum is a supplementation target constrained by the display and current effect combination. Idle redraw refreshes the existing picture and may increase power consumption.
- Setting DLSSNR input resolution too low may cause problems. Disable input-resolution adjustment or increase the inference size if this occurs.
- Temporal upscaling and frame generation estimate motion from captured images. Fast motion, occlusion and UI regions may show ghosting or unstable detail; choose effects and parameters for the actual scene.
- Include logs and reproduction steps when reporting a problem. Roll back with the previous complete package and its matching settings backup.

Contributor: [liaanj](https://github.com/liaanj) contributed the GPU filtering fix for virtual-display environments (PR #62).
