# lock_frame 1.3.2-local.camvm 发布说明

> **已回退，不建议使用。** 2026-11-25 实机对比后确认：100/100 + 不上采样带来的
> 额外 depth 码流/编码成本（约 +8 MB/帧、+1.3 ms）没有换来可见的人物边缘改善，
> 因此运行环境与仓库默认值都已回退到 **1.3.1-local.camvm**。本包仅作记录。

> **本地验证版，不作为公开发布。** 基于 1.3.1-local.camvm（协议 v34，`fakerFG` API v3）。
> `fakerFG.dll` 与 `xess_mfg_producer.dll` 必须一起替换；消费端/UI 用本包版本。

## 1. 默认值变更（本版重点）

| 键 | 旧默认 | 新默认 | 含义 |
|---|---|---|---|
| `CodecStreamPercent` | 100 | **100** | 码流分辨率基准 |
| `MvCodecPercent` | 100 | **100** | MV 1:1，比特精确 |
| `DepthCodecPercent` | 150 | **100** | depth 也 1:1（不再降采样） |
| `StreamPercent(Mv/Depth)` | 150 | **100** | 无码流回退路径也 1:1 |

- **消费端默认不再上采样 MV/depth**：100/100 时直接在游戏 **render/MV 网格**上 tag
  （与 OptiScaler 相同），不再重建到 3840×2160。日志：
  `--upsample: OFF (default) - 1:1 lossless streams are tagged at their own (render) size`，
  `xefg-in: mv=<render> depth=<render>`，且**没有** `rebuilt ... to 3840` 行。
- `--upsample` 保留为显式 A/B；如果旧 ini 仍把两条流设成不同尺寸，消费端会**自动**
  启用联合重建（只把 depth 升到 render 网格，MV 恒等），并打一行
  `MV/depth stream sizes differ ... enabling the joint rebuild`。
- 目的：避免“无损流又被消费端重采样”的特殊情况；人物边缘残留已定位为 XeFG 模型侧
  问题（见 §3），不再靠改 MV 来源/重建掩盖。

## 2. UI 同步

- 版本显示 `1.3.2-local.camvm`；
- 「MV/深度传输比例」默认 **100%**（1:1）；「深度码流」默认 **100%**；
- 文案/tooltip 改为“默认 1:1，不上采样”；
- UI 写 ini 时仍写 `CodecStreamPercent=100 / MvCodecPercent=100 / DepthCodecPercent=<选择>`，
  `StreamPercent*=<选择>`；默认即 100/100；
- 安装/还原/配置检查逻辑不变。

## 3. PBZ / UE5 结论更正

- 2026-11-10 的“PBZ MV 不可用、必须用 AMD 光流”**撤回**：PBZ 3x 在 `--fg-of` 下人物边缘
  伪影与游戏 MV 相同；
- 屏幕边缘伪影在 PBZ 也基本消失（viewMatrix 收益跨引擎成立）；
- 人物模型边缘残留**待定为 XeFG 自身（keep2x 模型/库）**，不再作为输入侧问题处理。
- 记录：`docs/INPUT_ALIGNMENT_CAMVM.md` §6、`docs/findings/UE5_MV_OPTISCALER.md` 顶部。

## 4. 包内容

```text
lock_frame.exe / lock_frame_ui.exe    消费端与 UI（1.3.2-local.camvm）
fakerFG.dll                           v3 代理（部署为 dxgi.dll / nvngx_dlssg.dll）
xess_mfg_producer.dll                 v3 producer
fakerFG.ini / fakerFG.full.ini        默认 100/100
libxess_fg.dll / libxell.dll          Intel 运行时（含许可）
docs/INPUT_ALIGNMENT_CAMVM.md         验证记录（含 PBZ 更正）
extras/cp2077_ab/                     CP2077 A/B 脚本 + 检查脚本
extras/pbz_ab/                        PBZ A/B 脚本（2x / port / 原生网格 / 符号 / 抖动）
RELEASE_NOTES.md / manifest.json / SHA256SUMS.txt
```

## 5. 安装 / 升级

1. 关闭游戏与 `lock_frame.exe` / `lock_frame_ui.exe`。
2. 备份游戏目录的 `fakerFG.dll`、`dxgi.dll`、`nvngx_dlssg.dll`、
   `xess_mfg_producer.dll`、`lock_frame.exe`、`lock_frame_ui.exe`。
3. **同一次**覆盖这 6 个文件（三份 fakerFG 必须是同一二进制）。
4. 把游戏目录的 `fakerFG.ini` 改成默认 100/100（或用 UI「重新应用配置」覆盖）：
   `CodecStreamPercent=100 / MvCodecPercent=100 / DepthCodecPercent=100`，
   `StreamPercent=100 / StreamPercentMv=100 / StreamPercentDepth=100`；
   然后**重启游戏**（producer 每会话只读一次 ini）。
5. 启动后在日志确认：`api ready (version 3, record 640 bytes)`、
   `producer ... api v3`、`--upsample: OFF (default)`、`xefg-in: mv=<render> depth=<render>`。

## 6. 回滚

恢复第 2 步的备份；旧版 1.3.1-local.camvm 的备份仍可按 `.bak_*` 找回。

## 7. 已知限制

- 3x/4x 的人物边缘残留是 keep2x 模型在 t≠0.5 的表现；本版不再通过输入侧“掩盖”。
  需要时可试 `extras/pbz_ab/pbz_18_port_3x.bat`（port model 15，3x）与 2x 对照。
- `initFlags` 仍不持久化；启动顺序异常时先看 `initFlags=0x1`。
- `frameRenderTime` 仍用消费端估计策略。
