# lock_frame 1.2.9 发布说明（2026-11-23）

> **本版包含 1.2.8 + 1.2.9 两批改动**（2026-11-23）：1.2.8 的性能配置全部并入；UI 合成本身
> 标记为**实验性并默认关闭**（手动打开时仍走 XeFG **mode 5**）、`fakerFG.ini` 拆成发布/测试两版、
> 输入源固定 `DLSSG`。版本号 **1.2.9**，协议 **v32**。1.2.8 是未公开的本地包，只作为本轮
> 性能记录的版本号保留。

## 0. 本轮新增（2026-11-23）

### 0.1 UI 合成标记为实验性，发布默认关闭

- **发布默认：`UiStream=0` / `UiSource=Off`**。UI 合成在边缘/半透明 UI 上仍会出现
  拖影或伪影，未达到发布质量，只在 A/B 时手动打开。
- 打开时（`UiStream=1` + `UiSource=Auto`）：无论 UI 来源（游戏 tag / render-card 派生
  synth），只要 **全分辨率 UI + 当帧 HUDless** 同时可用，就一起 tag `HUDLESS_COLOR` +
  `UI`，XeFG AUTO 选 `BACKBUFFER_HUDLESS_UITEXTURE`（mode 5）。
- mode 5 把 UI 合成回 HUDless（`U + (1−a)H' = P`），抵消 mode 4 里
  `(P−U)/(1−a)` 在 UI 边缘的放大；CP2077 4K/3x A/B 中派生 UI 的周边拖影消失。
- 派生 UI 的旧覆盖率门（`ui_quality` 50..6000 bp）已移除；`ui_quality` 仅作诊断。
- 半分辨率 UI（`UiFullRes=0`）仍自动退回 mode 4；`--no-ui-mode5` 可强制 mode 4。
- 日志：`ui: mode 5 (HUDless + UI), source=game|render-synth coverage=... bp`。

### 0.2 fakerFG.ini 拆成两版，输入源固定 DLSSG

- `fakerFG.ini`：**发布精简版**，只保留常用键（输入/码流/UI/诊断关闭）。
- `fakerFG.full.ini`：**测试全参数版**，含 `UiAlpha8` / `UiDilate` / `HudlessStream` /
  `HudlessSlots` / `UiSynthDump` / 诊断与内部键。
- UI「高级设置 → 游戏侧配置」新增 **配置版本（精简 / 完整）**：切换会先备份再覆盖
  游戏目录的 `fakerFG.ini`；首次部署按所选版本放入模板。producer 每会话只读一次，
  **改完要重启游戏**。
- `FgInput` 固定为 `DLSSG`：本版只实现 Streamline tag 车道。ini 里写
  `NVNGX` / `Upscaler` / `FSR` 会被代码强制回 `DLSSG`（`FgInput=NoFG` 仍是显式禁用）；
  UI 不再暴露输入源下拉，固定写 `FgInput=DLSSG`，游戏配置检查会提示非 DLSSG 的旧文件。
- 发布检查：`scripts/check_release.py` 现在要求两版 ini 都在包里，并校验测试版没有
  打开 `ProfGpu` / `FrCodecAblate` / `DepthFp16`。

### 0.3 已知限制

- mode 5 要求 HUDless/UI/backbuffer 同 `resourceSize`（XeFG 硬约束）。
- HDR 下若 XeFG 拒绝 RGBA8 UI（`-12 MISMATCH_INPUT_RESOURCES`），在游戏侧 ini 加
  `UiAlpha8=0`（UI 改走 R10，与 CP2077 的 fmt 24 一致）后重启游戏。
- UI 只在有 HUDless/UI 平面的标题上生效；没有平面的标题自动退化为不合成。

---

> 协议为 **v32**（`kSharedVersion`；`MgpuFgFrame` 布局不变，v31 起 stream 3 可承载渲染卡
> 全分辨率合成 UI、stream 4 承载全分辨率 HUDless），模型仍是 **keep2x**。
> 本版把 1.2.8 的性能配置真正接到 UI 上，顺带修掉一个会静默关掉 UI 合成的命令问题，
> 并把 UI 合成标记为实验性、默认关闭。

## 1. UI 现在会写这一轮的键（之前完全没写）

1.2.7/1.2.8 的 UI 写 `fakerFG.ini` 时只写 `FgInput / FgOutput / Passthrough /
RealSwapchainObject / StreamPercent* / MvCodec / DepthCodec / Log`。
也就是说 **从 UI 安装的人拿不到 `DepthCodecPercent`、`UiStream`、`UiSparseCodec` 这些默认值**，
游戏侧用的是模板里的旧值。

1.2.9 起「高级设置 → 游戏侧配置」新增两行，并且点「安装 / 应用配置」时会写进游戏目录：

| 设置 | 写到 fakerFG.ini | 默认 |
|---|---|---|
| **深度码流（无损）** 100 / 150 / 200 | `DepthCodecPercent`（同时固定 `MvCodecPercent=100`、`CodecStreamPercent=100`） | **150** |
| **UI 合成（HUDless，实验性）** 关 / 开启 | `UiStream` / `UiSource` / `UiFullRes=1` / `UiSparseCodec=1` | **关（发布默认；过渡版本曾为自动）** |

两项都按游戏保存在 `profiles\<游戏>.ini`（`depth_pct` / `ui_compose`），并且会从游戏目录的
`fakerFG.ini` 读回显示，所以下拉框显示的就是游戏会加载的值。

## 2. 修复：UI 启动命令从不打开 UI 合成

消费端的 UI 组合开关是**纯命令行**的（`g_ui_unblend` 只由 `--ui-unblend` / `--ui-source` 打开）。
UI 生成的启动命令一直是 `--streams=mv,depth`、不带 `--ui-source`，结果就是：

- 生产端照样发布 stream 3（占 8 个 UI 槽、白付 synth + encode 约 0.8 ms/帧）；
- 消费端**从不** unblend，插值帧的 HUD 照旧残影。

1.2.9 起，UI 合成开时命令是：

```text
--streams=mv,depth,ui --ui-source auto
```

关掉 UI 合成时不带 `,ui` 也不带 `--ui-source`，producer 因此不会白预留 UI 槽。

## 3. 各项设置的实测代价（CP2077 4K / 锁 40 / 3x）

| 设置 | 渲染卡 GPU | 链路 | 说明 |
|---|---|---|---|
| 深度码流 100% | 2.05 ms | 10.8 MB/帧 | 逐像素无损（最保守） |
| **深度码流 150%（默认）** | **0.79 ms** | **5.56 MB/帧** | depth 编码 −61%，MV 一字不改 |
| 深度码流 200% | 未测 | ~0.44× | 更省，遮挡边缘风险更高 |
| UI 合成 关（发布默认） | 0 | — | 最大帧率，HUD 在插值帧里残影 |
| UI 合成 开启（实验性） | +0.8 ms | — | HUD 不残影；无 HUDless 的标题自动退化 |

MV 始终 1:1 比特精确（`MvCodecPercent=100`），UI/深度都不影响它。

## 4. 兼容与回退

- 旧 `profiles\*.ini` 没有 `depth_pct` / `ui_compose` ⇒ 默认 150 / 关。
- UI 合成是实验性开关：需要时在 UI 里设为「开启」，或在 ini 里写
  `UiStream=1` / `UiSource=Auto` 并重启游戏；默认写的是 `UiStream=0` / `UiSource=Off`。
- 想回到"逐像素无损深度"：**深度码流**设为「100%」（或删掉 `DepthCodecPercent`）。

## 5. 已知限制（未变）

- 3x/4x 仍建议 120 Hz 面板 + 游戏锁 40/30 fps；`--unlock-mfg` 只对实测过的运行时版本有效。
- UI 合成是**实验性**：依赖游戏在 DLSS-G evaluate 里给 HUDless/UI 平面（CP2077 4K/DLAA
  可复现），边缘/半透明 UI 仍可能出现拖影或伪影，所以发布默认关闭；`--no-ui-mode5` 可回退 mode 4。
- 深度 150% 是「重采样后无损」，快速转视角时遮挡边缘的观感需要自己确认。
