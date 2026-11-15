# Magpie Experimental v0.6.9 Beta3

2026-10-02 本地测试版，继承 Beta2 的功能与修正。本轮更新如下：

- 效果组标题右侧改为纯文字“其他选项”，单层菜单依次提供导入、导出、配置文件夹、分隔线与重置。入口沿用旧版 36 DIP 高度，继承普通按钮的 14 号字与默认内边距，提供更舒展的文字和留白；保留当前中性填充与零边框，重置仍先确认。
- 审查并更新全部 162 项内置效果器的中英文摘要、详细说明和推荐标签，以及分类、家族与自定义效果的通用说明。说明覆盖用途、素材、变体、放置位置与限制，并以当前源码和运行时行为为依据。
- 修正 DLSSNR 多 Pass、SDR 输入分辨率与总残差、时域超分和补帧的捕获输入限制、RTX Video 实时强度切换，以及部分 HDR 路径说明。模型规模和运行成本不再直接作为实验性或画质等级的依据。
- 保留效果 ID、现有配置和搜索兼容别名；18 种支持语言的菜单标签同步迁移。

本轮采用单节点、单编译进程、低优先级并限制一个逻辑核心的 Release x64 构建。源码专项检查与打包校验不替代实际页面点击、完整系统 DPI／键盘交互和游戏画质、性能验收。

## English

Local test build dated 2026-10-02, retaining Beta2 features and fixes.

- The effect-group header now has a text-only **Other options** button. Its single-level menu contains Import, Export, Configuration folder, a separator, and Reset. The button keeps the previous 36 DIP height and inherits the ordinary button's font size of 14 and default padding for more room around its label. The neutral fill and zero border remain; Reset still requires confirmation.
- Reviewed and updated the Chinese and English summaries, details, and recommendation labels for all 162 built-in effects, together with category, family, and generic custom-effect guidance. Descriptions explain purpose, suitable content, variants, placement, and limitations based on current source and runtime behavior.
- Corrected guidance for DLSSNR passes, SDR input resolution and total residual controls, captured-input limitations of temporal upscaling and frame generation, live RTX Video strength changes, and selected HDR paths. Model size and cost alone no longer determine experimental status or imply a quality ranking.
- Preserved effect IDs, existing configuration, and compatibility search aliases. Menu resource bindings were updated for all 18 supported languages.

This Release x64 package is built with one MSBuild node and one compiler process, at low priority on one logical processor. Source checks and package verification do not replace manual page interaction, full system-DPI and keyboard checks, or in-game quality and performance acceptance.
