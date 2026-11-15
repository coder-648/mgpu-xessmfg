# src/bridge

`bridge.exe` 的源码。**`bridge.cpp` 只是公共头 + 模块索引**，实现位于
`modules/*.inc`，按数字前缀顺序在同一个翻译单元里展开。

- 结构与修改规则：`docs/design/BRIDGE_ARCH.md`
- 构建：`./scripts/build_bridge.sh`（先跑 `scripts/check_bridge_modules.py`）
- 着色器：`shaders/upsample_joint.hlsl`，由构建脚本用 fxc 编译并嵌入
- OSD 复用 `osd_panel.h`；`osd_test.cpp` 是独立的布局测试程序

新增模块时：

1. 在 `modules/` 建 `NNN_name.inc`，文件头写职责；
2. 在 `bridge.cpp` 的索引里按依赖顺序加入；
3. 跑 `python3 scripts/check_bridge_modules.py src/bridge` 和构建。
