# 许可与合规声明

本仓库整体以 **GNU GPL v3.0 或更新版本**发布，Copyright (C) 2026 bilibili夕阳虹舞
—— 全文见根目录 `LICENSE`，第三方组件的逐项署名见 `NOTICE`。

> 为什么是 GPL-3.0 而不是 MIT：`src/xefg_dynamic_count.h` 是 OptiScaler
> （GPL-3.0）`XeFG_DynamicCount.h` 的逐字移植，而且它被编译进了 `bridge.exe`。
> `third_party/optiscaler/` 下另有 6 个逐字保留的参考文件。
> 想改成宽松许可，必须先把这两处去掉（见本文末尾）。

## 1. 你分发二进制时必须做的事

| 场景 | 义务 |
|---|---|
| 分发 `game_capture.addon64` / `bridge.exe` / `mgpu_ui.exe` | GPL-3.0：附上或指向完整对应源码 |
| 分发 `docs/ops/DEPLOY.md` 生成的部署包（含 `ReShade64.dll`） | 再加上 ReShade 的 **BSD-3-Clause** 许可证文本 |
| 分发时把 `libxess_fg.dll` / `libxell.dll` 一起打包 | **先读 Intel XeSS SDK 的许可**（见 §3） |

本仓库**不**包含任何 Intel / NVIDIA 的 DLL：`.gitignore` 排除了
`third_party/xess/bin/`，部署包里的两个 DLL 由使用者自己从 SDK 取。

## 2. `--unlock-mfg` 是什么，以及它的风险

`--unlock-mfg` 不是"调用某个接口"，而是**在运行中的进程里修改商业 DLL 的机器码**：
`src/xefg_unlock.h` 把 `libxess_fg.dll` 与 `libxell.dll` 里的 6 处判断逐字节改掉，
把非 Intel 显卡上被限制成 1 的插值帧数放开到 2..5（即 3x..6x）。

- 补丁位置是**硬编码 RVA**，只在实测的那个版本上成立
  （`libxess_fg 1.3.1.78` / `libxell 1.3.2.10`）。库一升级就会
  `*_unexpected_bytes` 拒绝执行 —— 这是设计如此，不会瞎改。
- 机制出自 MIT 许可的 [Universal MFG Unlocker](https://github.com/dashdogy/MFG-Unlocker)；
  本仓库只沿用其偏移量与改法。
- **本仓库不随源码或二进制分发 `libxess_fg.dll`。**

> ⚠️ **声明**：这项功能仅供**个人研究**，用于观察 XeSS 帧生成在本机上的行为。
> 修改第三方商业库的机器码**可能违反该库的最终用户许可协议（EULA）**，
> 也可能被游戏的反作弊或数字签名校验视为篡改。作者不对由此产生的任何后果负责。
> 如果你不确定是否合规，**不要加 `--unlock-mfg`** —— 2x（`--interp 1`）不需要它。

## 3. XeSS SDK 的再分发限制

`third_party/xess/LICENSE.txt`（Intel 专有许可）与 GPL-3.0 **不兼容**。
因此：

- 本仓库只把它当**本地构建依赖**（`.gitignore` 排除 `third_party/xess/bin/`）；
- 要让项目在 GPL 下完整可用，请使用者自行下载 SDK；
- 任何"预打包"的发布物都应把这些 DLL 排除在外，或单独走 Intel 的条款。

## 4. 商标与游戏资产

AMD / Radeon / FidelityFX、Intel / XeSS、NVIDIA / DLSS / RTX、ReShade 等名称
属于其各自所有者；本项目与其无隶属关系。仓库内**不含**任何游戏资产或补丁，
`docs/` 里出现的游戏名只用于说明"在哪款游戏上实测过"。

## 5. 如果将来要改成宽松许可

按顺序做：

1. 删掉 `src/xefg_dynamic_count.h` 与 `--dyn-mfg` 的全部接线
   （`docs/findings/FINAL_CONFIG.md` §9.19 已实测"固定倍率"更好，功能上没有损失）；
2. 删掉 `third_party/optiscaler/**`，把 MV 约定改写为自己的结论 +
   引用上游 URL（现有摘录是逐字引用，不能直接沿用）；
3. 把 `LICENSE` 换成 MIT / Apache-2.0，同步修改 `NOTICE` 与
   `src/xefg_dynamic_count.h` 顶部注释。
