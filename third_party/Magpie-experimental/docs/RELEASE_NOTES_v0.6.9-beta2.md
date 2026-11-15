# Magpie Experimental v0.6.9 Beta2

2026-10-02 本地测试版。以下为相对 0.6.8 的 **0.6.9 累计更新清单**，包含全部 11 个功能分支以及 Beta1 fix1、fix2、fix3 的修正。Beta2 新增重点是统一帧率与刷新、效果组页面恢复、仅显示适配器筛选和 Reflex 标记边界。

## 界面、操作与配置

### 1. 统一“帧率与刷新”

- 默认配置和应用配置统一编辑内容帧率、节奏控制、光标刷新和空闲持续重绘；主页显示默认配置摘要及编辑入口，原光标区不再重复提供刷新编辑入口。
- 内容帧率支持 **跟随源／自动／自定义**。节奏方式支持 **Front Edge／Async／NVIDIA Reflex**，位于内容选择下方、基础数值上方；跟随源时禁用固定目标节奏并保留选择。自动按显示刷新率和 FG 倍率计算基础目标。
- 光标支持 **响应优先／仅随原始内容帧／原始帧优先并补充**。补充刷新率支持自动跟随显示器或自定义最低目标；静止光标不持续触发空呈现，成功发布变化状态才推进刷新期限。
- 高级区默认折叠，提供空闲持续重绘及数值。有效空闲率受内容上限限制，FG 期间停用后端空闲重绘，但保留配置，光标补充仍独立工作。
- 新配置及恢复默认：**内容自定义 60 FPS、Front Edge、原始帧优先并补充、光标自定义最低 60 FPS、空闲重绘启用 30 FPS**。实时面板与配置页的内容、光标、空闲三个自定义值统一为 **15–360 FPS**，整数、步进 1；自动目标和旧配置小数继续兼容，只在手动编辑时按新范围取整数。
- 刷新设置的六项下拉框和三组“数值＋FPS”统一为 225 DIP 输入列，三项数值提供行内步进按钮；窄窗口统一换行，条件提示沿用次级文字样式。
- 修改自动保存，重新启用缩放后生效；运行时参数面板使用同一设置，可保存并重启。新建应用配置复制当前默认配置或选定模板，之后各自独立保存。
- 旧全局空闲率、同步模式、独立上限和四种旧光标组合按有效旧行为迁移；不会用新默认值覆盖合法旧选择。保存值与运行时有效值分开，隐藏控件保留数值，恢复默认仅影响当前配置。
- 运行时保存检查配置身份、窗口、运行会话及字段冲突，防止旧回调写入其他配置。配置恢复规则升级，检查字段类型、枚举和范围。
- FrameRate Filter 的跟随项改为“跟随内容帧率目标”，保留原效果 ID、旧数值和自定义限制兼容。

### 2. 效果组页面

- 恢复整个列表下方右侧的 **“＋新建效果组”**，修复负边距与零尺寸布局过滤组合导致按钮消失的问题。
- 标题右侧直接显示 **导入、导出、配置文件夹、重置**，取代更多菜单；18 种支持语言均提供短文字按钮标签。
- 四个操作按钮沿用效果组／效果器普通按钮的中性填充、圆角与交互反馈，边框宽度为 0。
- 配置文件夹打开当前实际生效目录，支持普通与便携模式；失败提示包含实际路径。导入、导出、新建和重置沿用原流程，重置仍先确认。

### 3. 快捷键清除

- 工具栏快捷键行增加清除按钮，全部八项快捷键编辑弹窗支持保存／清除／取消。
- 清除后显示“未设置”、立即解除绑定，重启后仍保持；重新设置继续检查冲突。明确的空绑定与缺失配置字段分别处理。

### 4. 工具栏拖动与样式

- 按住工具栏 **空白或 FPS 区域直接拖动**；功能按钮保持点击操作。最终版本已移除专用拖动按钮。
- 支持顶部／底部停靠及沿边缘左右移动，拖动时显示停靠提示。默认与应用配置分别保存窗口／全屏上下位置；横向位置仅在本次运行保留，新运行居中。
- 距水平中心左右各 **12 个实际像素**内吸附，吸附拖动期间显示 **2 实际像素**青色竖线；松手、离开或取消立即消失。
- 统一按钮尺寸、图标居中和小窗口圆角；底部菜单与提示向画面内展开。FPS 按整个工具栏水平居中，并修复继承按钮基线导致的垂直下偏。

### 5. 参数浮层与主页选项

- 所有效果器参数浮层可超出主窗口，按显示器工作区确定尺寸，保留可读列宽，空间不足时滚动。
- DLSSNR 配置页按“细节控制｜进阶调整｜Pass 1｜启用的后续 Pass”排列；进阶组默认隐藏，收起保留数值和效果，诊断视图归入进阶组。
- 修复隐藏列和参数行留下的额外间距。DLSSNR 名称悬停提供简短用途说明，补齐英文、简中、繁中；运行时面板仍采用纵向分组。
- 重复帧检测移至 **帧率与刷新 → 高级选项**，沿用全局设置；开发者选项入口常驻主页，FG 检测使用效果器参数。刷新设置说明统一为简短陈述，移除重复保存提示。
- 修复快捷键清除按钮引用不存在的 XAML 样式导致的启动失败，并补充异常详情日志。

## 效果器、处理与兼容性

### 6. 五个 xBR 效果

新增 **xBR 2x、xBR 3x、xBR 4x、xBR NoBlend 3x、xBR Hybrid 2x**，面向像素画与低分辨率 2D 内容，保留原算法及相关许可说明。LV3、MLV4、Super-xBR 等变体未纳入本版。

### 7. 重复帧与效果缓存

- 重复输入复用已有输出，以输入／输出修订传播避免重复执行后续效果；动态效果、同帧调参、HDR、尺寸变化和资源重建沿用相应失效规则。
- 保留实际接受捕获帧的身份和时间；HDR 去重提前到重复转换工作之前，避免复用路径推进不属于新内容的时域历史。

### 8. DLSSNR Multi Pass

- SDR 输入分辨率调整路径只在入口降采样一次，各 Pass 在推理尺寸串联，出口一次重建并控制 **最终输出减初始输入的总残差**。
- 共用引导、外围资源和 D3D12 调度，集中入口／出口同步；各 Pass 的 NR 参数、Feature 和模型历史仍独立。
- 只改残差后处理时复用 NR 输出；修改某个 Pass 时从该 Pass 开始重算后缀；中性频率设置跳过额外频率分解。

### 9. DLSSNR 统一细节控制

- 使用统一残差算法，移除旧颜色模式选择和运行分支；有效旧参数数值自动保留并迁移。
- 基础名称统一为总强度、色度强度、明度总强度、阴影／结构强度、高光／辉光强度；进阶提供色相／暗部／高光保护、过度修正抑制、低频／高频修正强度及诊断视图。

| 参数 | 范围 | 默认 | 步进及关闭行为 |
| --- | --- | --- | --- |
| 上述五项基础强度 | 0–2 | 1 | 0.05；总强度 0 不叠加残差修正 |
| 色相、暗部、高光保护与过度修正抑制 | 0–1 | 0 | 0.05；0 关闭对应保护／抑制 |
| 低频、高频修正强度 | 0–2 | 1 | 0.05；两者为 1 时跳过额外分解 |
| 进阶调整 | 关／开 | 关 | 只控制界面显隐，不重置参数 |
| 诊断视图 | 最终图、原始／受控总残差、明度／色度变化、保护权重、色域／压缩比例 | 最终图 | 下拉选择 |
| 输入分辨率调整 | 关／开；25–100% | 关；100% | 百分比步进 1；关闭时使用原输入尺寸 |

- 移除额外色度时域稳定的公开参数，固定为 0 并清理旧字段；保留抗闪烁功能及其明暗、颜色处理。
- 多层低分辨率串联及统一残差会改变旧版画面行为，尤其是旧非默认颜色参数；不能假定迁移后观感完全相同。

### 10. Reflex 标记与内容／光标调度

- 捕获等待不提前打开 Render 区间；实际纹理复制、共享资源打开或效果重算前才开启。被拒绝候选、捕获中断和失败路径及时关闭区间。
- 前端先取得交换链容量，再开启 Render 并复制基础、参考和运动纹理；容量不足返回消息循环，不先复制。保留候选和实际呈现 ID 的对应关系。
- DLSS FG 暂存输入的期限等待移出 Render 区间；NR 运动历史区分合法捕获 ID 间隙与真正遗漏有效输入，避免误重置。
- 普通 Front Edge 的光标／工具栏旧图呈现不推进内容时钟；内容间隙可复用已成功呈现背景发布变化光标，不额外运行效果链或送入 FG 基础帧。
- 单一限速责任方、Reflex 失败回退、XeLL 接管、FG FIFO、容量与资源背压规则继续保留。这些修正不代表已经测得 FPS 或延迟收益。

### 11. 排除仅显示的间接适配器

- 接入 PR #62：从界面列表、已保存显卡选择、ID 重匹配及自动选择路径排除查询成功且 **IndirectDisplayDevice=1、RenderSupported=0** 的条目，处理 Sunshine／虚拟显示环境中选错相同 vendor/device ID 适配器的问题。
- 保留可渲染的间接适配器；查询失败记录诊断并保留候选、正常设备创建及 WARP 回退。查询句柄各退出路径均关闭。
- 默认与已有应用配置按既有 ID 匹配规则重新定位保留的实体卡；保留原始 DXGI 索引。

## 安装、验证与已知边界

- 完整解压 **Magpie-Experimental-x64.zip** 到新目录，完全退出旧 Magpie 后运行新目录的 `Magpie.exe`。本地部署目录为 `release/v0.6.9-beta2/Magpie-Experimental-x64/`，Beta1 保留供恢复。
- 普通配置使用 `%LOCALAPPDATA%\Magpie\config\v4e\config.json`；便携配置使用程序目录内 `config\v4e\config.json`。启动前备份现有配置，回退时使用对应旧包和配置快照。
- 构建、自动回归、资源、包哈希及真实启动结果见 [Beta2 整合与部署记录](experimental/reviews/20261002-v0.6.9-beta2-integration.md)；功能存在与运行验收分别记录。
- 光标最低刷新是补充目标，实际速率受显示与呈现通道约束；空闲重绘可能增加功耗，不提高源应用真实帧率。DLSS FG 共用呈现通道，XeSS 使用独立叠加层。
- 历史测试在极小 DLSSNR 推理尺寸（如 32×18）观察到设备挂起，旧单层也可复现，最小尺寸边界仍未确定。遇到异常先关闭输入分辨率调整或提高推理尺寸。
- 自动 CPU／WARP、原生控件布局和合成 NGX 检查不能替代实际游戏画质、端到端延迟、Sunshine／外接显卡、所有 FG／HDR 组合与多显示器验收。
- 历史暂停的 VRR 分支不属于 0.6.9 范围。Beta2 为本地测试包，公开更新入口按正式发布流程维护。

Contributor: [liaanj](https://github.com/liaanj) 提供 PR #62 的仅显示间接适配器筛选修正。

---

# Magpie Experimental v0.6.9 Beta2

Local test release dated 2026-10-02. This is the **complete cumulative 0.6.9 change list relative to 0.6.8**, including all eleven feature branches and Beta1 fix1/fix2/fix3. Beta2 adds unified frame/refresh settings, the effect-group page repair, display-only adapter filtering and corrected Reflex marker boundaries.

## Interface, controls and configuration

### 1. Unified Frame Rate & Refresh

- Default and application profiles share one group for content rate, pacing, cursor publication and continuous idle redraw. Home shows the default-profile summary and an edit entry; cursor settings no longer duplicate refresh controls.
- Content modes are **Follow source / Automatic / Custom**. **Front Edge / Async / NVIDIA Reflex** pacing appears below the content mode and above its numeric target. Follow source disables fixed-target pacing while retaining the selection. Automatic derives the base target from display refresh and FG multiplier.
- Cursor modes are **Response first / Original content frames only / Prefer original frames with supplementation**. Supplemental rates can follow the display automatically or use a custom minimum. An unchanged cursor does not continuously present; only successfully published changes advance the deadline.
- The advanced section starts collapsed and contains idle redraw and its rate. Its effective rate respects the content ceiling. FG disables backend idle redraw while retaining the setting; cursor supplementation remains separate.
- New/restored defaults: **Custom content 60 FPS, Front Edge, original frames with supplementation, Custom cursor minimum 60 FPS, idle redraw enabled at 30 FPS**. Both the runtime panel and profile page use **15–360 FPS**, integer values in 1-FPS steps, for content, cursor and idle targets. Automatically calculated targets and legacy fractional values remain compatible; explicit edits use the new integer range.
- Six refresh dropdowns and three rate-and-FPS rows share 225-DIP control columns. Rate inputs use inline steppers, narrow layouts share wrapping rules, and conditional notes use secondary text styling.
- Edits save automatically and apply after restarting scaling. The runtime parameter panel uses the same model and supports save/restart. New profiles copy the current default or selected template and then save independently.
- Valid legacy global idle rates, pacing, independent ceilings and four cursor combinations migrate without being replaced by the new defaults. Effective rates do not overwrite saved values; hidden controls retain numbers, and restoring defaults affects only the current profile.
- Runtime saves validate profile, window, session and field conflicts to reject stale writes. Configuration recovery validates types, enums and ranges.
- FrameRate Filter now labels its inherited target “Follow content frame rate target,” retaining the effect ID, old values and custom-limit compatibility.

### 2. Effect-group page

- Restore **+ New effect group** at the lower right below the entire list, repairing the interaction between a negative margin and zero-size layout filtering.
- Show **Import, Export, Config folder, Reset** directly in the header instead of a more menu, with short labels in all 18 supported languages.
- The four actions use the existing neutral fill, corners and interaction feedback of effect-group and effect-row buttons, with a zero-width border.
- Config folder opens the active normal/portable path and reports the actual path on failure. Import, export, creation and reset retain their existing flows and reset confirmation.

### 3. Clear shortcuts

- Toolbar shortcut rows gain a clear button; all eight shortcut dialogs offer Save / Clear / Cancel.
- Clearing immediately unregisters the binding, displays “Not set” and persists across restarts. Rebinding keeps conflict checks. Explicitly empty and missing configuration fields remain distinct.

### 4. Toolbar dragging and appearance

- Drag directly from **blank or FPS areas**, while action buttons remain clickable. The final version removes the dedicated drag button.
- Dock at the top/bottom and move horizontally. Default/application profiles save windowed/fullscreen vertical docking separately; horizontal placement lasts for the session and starts centered next time.
- Snap within **12 physical pixels** of the horizontal center, showing a **2-physical-pixel cyan line** only while the drag is snapped; releasing, leaving or cancelling removes it.
- Uniform button frames, centered icons, scaled small-window corners and inward-opening bottom menus/tooltips. FPS centers across the whole toolbar, with the inherited button baseline offset corrected.

### 5. Parameter popups and Home options

- All effect parameter popups can extend beyond the main window, sizing against the monitor work area with readable columns and scrolling when needed.
- DLSSNR uses Detail Control / Advanced Adjustments / Pass 1 / enabled later Pass columns. Advanced starts hidden; collapsing retains values and processing, and diagnostics belong to Advanced.
- Remove leftover spacing from hidden columns/rows. DLSSNR names show concise hover descriptions in English, Simplified and Traditional Chinese; the runtime panel retains vertical groups.
- Duplicate detection moves to **Frame rate and refresh → Advanced**, retaining its global setting. Developer options remain on Home; FG detection uses effect parameters. Refresh descriptions are concise and the repeated save notice is removed.
- Remove an unavailable XAML style that prevented startup after shortcut clearing was introduced, and add detailed exception logging.

## Effects, processing and compatibility

### 6. Five xBR effects

Add **xBR 2x, 3x, 4x, NoBlend 3x and Hybrid 2x** for pixel art and low-resolution 2D content, retaining algorithm/license notices. LV3, MLV4 and Super-xBR variants are excluded.

### 7. Duplicate frames and effect caching

- Reuse outputs for duplicate input and propagate input/output revisions to avoid repeated downstream processing. Dynamic effects, same-frame edits, HDR, resizing and resource rebuilding retain their invalidation rules.
- Preserve accepted capture identity/timing and move HDR duplicate detection ahead of redundant conversion work. Reuse does not advance temporal history as new content.

### 8. DLSSNR Multi Pass

- The SDR resolution-adjustment path downsamples once, chains passes at inference size, then reconstructs and controls **final output minus initial input** once at the exit.
- Share guidance, surrounding resources and D3D12 scheduling, consolidating entry/exit synchronization while keeping each pass's NR parameters, Feature and model history independent.
- Residual-only edits reuse NR output; editing a pass recomputes its suffix. Neutral frequency settings skip extra decomposition.

### 9. Unified DLSSNR detail controls

- One residual algorithm replaces the old color-mode selector/branches, automatically retaining valid legacy numeric values.
- Basic controls are Overall, Chroma, Overall Lightness, Shadow/Structure and Highlight/Glow Strength. Advanced includes hue/shadow/highlight protection, overcorrection suppression, low/high-frequency strength and diagnostics.

| Parameter | Range | Default | Step and disabled behavior |
| --- | --- | --- | --- |
| Five basic strengths above | 0–2 | 1 | 0.05; Overall 0 applies no residual correction |
| Hue/shadow/highlight protection and overcorrection suppression | 0–1 | 0 | 0.05; 0 disables that protection/suppression |
| Low/high-frequency strengths | 0–2 | 1 | 0.05; both at 1 skip extra decomposition |
| Advanced Adjustments | Off/On | Off | Visibility only; values retained |
| Diagnostic View | Final image, raw/controlled total residual, lightness/chroma change, protection weight, gamut/compression scale | Final image | Selection |
| Input resolution adjustment | Off/On; 25–100% | Off; 100% | Percentage step 1; Off uses the original size |

- Remove the public extra chroma temporal-stabilization controls, fixing them at 0 and cleaning legacy fields. Anti-flicker and its lightness/color processing remain.
- Low-resolution chaining and unified residuals change earlier image behavior, particularly with nondefault legacy color controls; migration does not guarantee identical appearance.

### 10. Reflex markers and content/cursor scheduling

- Waiting capture polls do not open Render early. Texture copy, shared-resource opening or effect recomputation opens it immediately before work. Rejected candidates, interruptions and failures close it promptly.
- The frontend obtains swap-chain capacity before opening Render and copying base/reference/motion textures. No capacity returns to the message loop without copying; candidate/presentation IDs retain their correspondence.
- DLSS FG deferred-input deadline waits sit outside Render. NR motion history distinguishes valid capture-ID gaps from genuinely skipped accepted input.
- Ordinary Front Edge cursor/toolbar presentation does not advance the content clock. Between content deadlines, changed cursors can use an already presented background without running effects or submitting extra FG base input.
- Keep one pacing owner, Reflex fallback, XeLL ownership, FG FIFO and capacity/resource backpressure. No FPS or latency benefit is claimed from these code changes alone.

### 11. Display-only indirect adapter filtering

- Integrate PR #62: exclude successfully queried adapters with **IndirectDisplayDevice=1 and RenderSupported=0** from the UI, saved selection, ID rematching and automatic selection. This addresses wrong same-vendor/device-ID selection in Sunshine/virtual-display environments.
- Keep render-capable indirect adapters. Query failures are diagnosed and retain the candidate, normal device creation and WARP fallback. Query handles close on all applicable exit paths.
- Default/existing application profiles rematch the retained physical GPU using existing identity rules; original DXGI indices remain.

## Installation, verification and boundaries

- Fully extract **Magpie-Experimental-x64.zip** into a new folder, exit old Magpie completely and run the new `Magpie.exe`. Local deployment: `release/v0.6.9-beta2/Magpie-Experimental-x64/`; Beta1 remains available for recovery.
- Normal config: `%LOCALAPPDATA%\Magpie\config\v4e\config.json`; portable config: `config\v4e\config.json` inside the program folder. Back up existing settings before startup; recover with the matching old package/config snapshot.
- Build, automated regression, resources, package hashes and native startup results are recorded in the [Beta2 integration/deployment record](experimental/reviews/20261002-v0.6.9-beta2-integration.md), separately from feature implementation.
- Cursor minimum is a supplemental target subject to display/presentation capacity. Idle redraw can increase power and does not increase source-application FPS. DLSS FG shares the presentation channel; XeSS uses its separate overlay layer.
- Historical tests observed a device hang at tiny NR inference sizes such as 32×18, also on the earlier single-pass path; the minimum boundary remains unknown. Disable input scaling or increase inference size when troubleshooting.
- CPU/WARP, native-control layout and synthetic NGX checks do not replace actual-game quality, end-to-end latency, Sunshine/external-GPU, all FG/HDR combinations or multi-monitor acceptance.
- The historical paused VRR branch is outside 0.6.9. Beta2 is a local test package; the public update entry follows the release workflow.

Contributor: [liaanj](https://github.com/liaanj), PR #62 display-only indirect adapter filtering.
