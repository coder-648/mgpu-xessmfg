# XeFG UI 合成实际需要的完整参数（2026-11-22）

> **1.3.0/v34 状态**：UI 合成**默认开启**。stream 3 只承载游戏 UI 平面；
> stream 4（`HudlessStream`）承载支撑域 H patch。CP2077 默认用
> `ScalingOutputColor`（显示侧、UI 前）作为 patch 颜色，mask 仍由 scene-referred
> HUDLess 计算；consumer 只跑一次全局 WGC 色彩映射，不跑局部 tone field。

> 依据：`third_party/xess/inc/xess_fg/xefg_swapchain.h`、
> `xefg_swapchain_d3d12.h`、`third_party/xess/doc/xess_fg_developer_guide_english.md`
> §"UI Composition Modes"（约 540–640 行），以及我们自己的
> `lock_frame/modules/040_cadence.inc` / `140_frame.inc`。
>
> 结论先行：XeFG 的 UI 合成只认**三个资源**（BACKBUFFER / HUDLESS / UI）+
> **一个初始化模式** + **一个启用状态**。它不是"给个 alpha 就能合成"：
> UI 纹理必须是 **RGB + alpha**、**premultiplied**（或显式关掉 premul flag）、
> **与 backbuffer 同格式同色彩空间**，并且满足
> `Final = UI + (1 − a)·HUDless`。

---

## 1. 三个资源与公式

| 资源 | 枚举 | 内容要求 |
|---|---|---|
| 合成后画面 | `XEFG_SWAPCHAIN_RES_BACKBUFFER` | 最终帧（含 UI），与 backbuffer 同格式/空间；**通常不用 tag**（见下） |
| 无 UI 场景 | `XEFG_SWAPCHAIN_RES_HUDLESS_COLOR` | **所有后处理都已应用**、去掉 UI 的整帧；同格式/空间 |
| UI 单独纹理 | `XEFG_SWAPCHAIN_RES_UI` | 只含 UI/HUD，RGB + alpha；必须满足下式 |

> **角色 vs API 输入（最容易被误解的一点）**：
>
> | 资源 | 在合成里的角色 | 需要 tag 吗 |
> |---|---|---|
> | backbuffer | 显示帧 + mode 5 的 refine 源 + mode 4 的 UI 提取源 | **不需要**（proxy swapchain 隐式提供）；只有 letterboxing 时要 tag，而且**只取 size/offset，`pResource` 被忽略** |
> | HUDless | 插值底图 | 需要（`RES_HUDLESS_COLOR`） |
> | UI | 合成层（RGB + alpha） | 需要（`RES_UI`） |
>
> 所以"XeFG 用三个资源合成"是对的，"三个都要作为参数传进去"不对：**只有两张要 tag**。
> 消费端拿到的颜色（WGC 或 colour 流）必须先**写进链缓冲**，XeFG 看到的就是那块
> backbuffer；不能把一张 WGC 纹理通过 `RES_BACKBUFFER` 塞给它（那个 tag 不接收资源）。
>
> 另一个硬约束：**HUDless / UI / backbuffer 的 `resourceSize` 必须相同**（guide
> `:794-802`）。所以 UI 要么全分辨率，要么先升采样到输出尺寸再 tag——这也是"不下采样"
> 之后天然满足的一条。

```py
FinalColor.RGB = UIonly.RGB + (1 - UIonly.Alpha) * HUDlessColor.RGB
```

- 默认 **premultiplied**；如果 UI 的 RGB 没有乘 alpha，必须在初始化时设
  `XEFG_SWAPCHAIN_INIT_FLAG_UITEXTURE_NOT_PREMUL_ALPHA`（`xefg_swapchain.h:95`）。
- 我们的 `downsample_ui.hlsl` 输出是 premultiplied，所以这个 flag **不设**（默认 0），
  与我们的 `Premultiplied=1` 一致。
- **alpha-only 平面不能直接当 `RES_UI`**：Streamline 的 `kBufferTypeUIAlpha`(68) 只有
  一个通道，缺少 RGB；要用它就必须自己重建 `UI.rgb = P − (1−a)·H`（premultiplied），
  或者走 mode 4（不需要 UI 纹理）。

---

## 2. 初始化参数

| 参数 | 取值 | 作用/约束 | 我们的现状 |
|---|---|---|---|
| `uiMode` | `AUTO(0)` / `NONE(1)` / `2` / `3` / `4` / `5` | 只有在 composition state = ENABLED 时才生效 | `040_cadence.inc:1168`：有 UI 流 → AUTO，否则 NONE |
| `initFlags` bit5 | `UITEXTURE_NOT_PREMUL_ALPHA` | UI RGB 未预乘时置位 | 未设（我们预乘） |
| HDR 格式 | `R10G10B10A2_UNORM` + HDR10/BT.2100 | **backbuffer / HUDless / UI 三者必须同格式同空间**；不支持 FP16/scRGB | 消费端链缓冲是 R10 PQ，UI 流也是 R10 PQ |
| `maxInterpolatedFrames` | 1..N | 与 UI 无关，但改它会重建资源 | 已有 |
| `xefgSwapChainSetUiCompositionState` | `ENABLED` / `DISABLED` | **默认 DISABLED**；`NONE + ENABLED` 等价于关闭；切换有代价（我们按 10 帧 skip 处理） | `040_cadence.inc:1393`，按 `ui_source != Off` 决定 |
| `XEFG_SWAPCHAIN_UI_COMPOSITION_STATE_*` | 见上 | "UI mode set during initialization has no effect" 直到 ENABLED | —— |

---

## 3. 每帧参数（`xefg_swapchain_d3d12_resource_data_t`）

| 字段 | 含义 | 注意 |
|---|---|---|
| `type` | `RES_UI` / `RES_HUDLESS_COLOR` / `RES_BACKBUFFER`（另有 depth/MV） | 每帧按模式 tag |
| `validity` | `RV_UNTIL_NEXT_PRESENT` / `RV_ONLY_NOW` | `ONLY_NOW` 时库会在**你给的 cmdList** 上录一次拷贝；该 list 必须提交，资源在提交前不能改 |
| `resourceBase` | 资源内的起始偏移 (x,y) | 支持子矩形 |
| `resourceSize` | 有效尺寸 (w,h) | 我们的 UI/HUDless 都按输出尺寸填 |
| `pResource` | `ID3D12Resource*` | 必须是同格式/同空间的纹理 |
| `incomingState` | 传入时的 D3D12 状态 | 我们：HUDless = `NON_PIXEL_SHADER_RESOURCE`，UI = `COMMON` |

调用：`xefgSwapChainD3D12TagFrameResource(ctx, cmdList, presentId, &data)`
（`xefg_swapchain_d3d12.h:256`），以及 `xefgSwapChainSetPresentId`。

---

## 4. 模式矩阵（决定"要传什么"）

| ID | 模式 | HUDless | UI 纹理 | 插值底图 | 备注 |
|---|---|---|---|---|---|
| 0 | `AUTO` | 可选 | 可选 | 按已 tag 的资源自动选 | 我们初始化用这个 |
| 1 | `NONE` | 不用 | 不用 | backbuffer | 等价于关 composition |
| 2 | `BACKBUFFER_UITEXTURE` | 不用 | **必需** | backbuffer（UI 烘在里面）+ UI refine | 兜底；仍可能拖影 |
| 3 | `HUDLESS_UITEXTURE` | **必需** | **必需** | HUDless + UI 混合 | UI 必须严格满足公式 |
| 4 | `BACKBUFFER_HUDLESS` | **必需** | 不用 | HUDless + 从 backbuffer 提取 UI | 我们 unblend 成功后的路径；半透明更差 |
| 5 | `BACKBUFFER_HUDLESS_UITEXTURE` | **必需** | **必需** | HUDless + UI 混合 + backbuffer refine | **UE / UI 纹理不完美 / 2-bit alpha 闪烁时的推荐模式** |

`AUTO` 的判定就是"看 tag 了哪些资源"：

- 只 tag UI → mode 2；
- HUDless + UI → mode 5（优先）或 3；
- 只 tag HUDless → mode 4；
- 都不 tag → 1。

我们现在的 tag 规则（`140_frame.inc:1136-1205`，2026-11-23 起）：HUDless 与 UI 同时可用就
**一起 tag**（AUTO 选 mode 5），游戏 UI 和 render-card 派生 UI 一视同仁；没有 HUDless 时才退回
只 tag UI（mode 2，仅 GameUi），派生 UI 此时不 tag。`--no-ui-mode5` 可强制回 mode 4。
`ui_full_res` 仍是硬条件（XeFG 要求三者 `resourceSize` 相同）；覆盖率不再作为门控——
空的派生 UI 在 mode 5 里合成回 HUDless，无害。

---

## 5. HDR 的硬约束（与 2-bit alpha）

开发指南（`xess_fg_developer_guide_english.md:622-635`）明确写：

- HDR 下 backbuffer / HUDless / UI **必须同为 `R10G10B10A2_UNORM`，且是 HDR10/BT.2100**；
- **不支持 FP16 HDR / scRGB**；
- **2-bit alpha 可能不足以表示半透明 UI**；切到
  `MODE_BACKBUFFER_HUDLESS_UITEXTURE`（mode 5）可能避免闪烁。

这从厂商侧证实了我们 §3 的分析：**HDR 的半透明问题是格式/模式问题，不是我们的
shader 写得不对**；如果只用 mode 4，2-bit alpha 的黑边是理论上的上限。

---

## 6. 我们现在的实现 vs 完整参数

| 项 | 我们的代码 | 是否满足 |
|---|---|---|
| `uiMode` | `AUTO` / `NONE`（`040_cadence.inc:1168`） | ✅ |
| `SetUiCompositionState` | 按 `ui_source != Off`（`:1393`） | ✅ |
| `RES_HUDLESS_COLOR` | `140_frame.inc:1113-1120`，`resourceSize = 输出尺寸`，state = `NON_PIXEL_SRV` | ✅ |
| `RES_UI` | `:1133`，`resourceSize = streams[3]` 的尺寸、state = `COMMON` | ⚠️ 半分辨率时尺寸 ≠ 输出（我们升采样后再 tag） |
| 两者同时 tag | 是（mode 5） | ✅ 2026-11-23 起默认 |
| `UITEXTURE_NOT_PREMUL_ALPHA` | 未设 | ✅（我们的 UI 是预乘） |
| HDR 同格式/同空间 | UI/HUDless 都是 R10 PQ | ✅ |
| `validity` | 未显式区分（默认 `UNTIL_NEXT_PRESENT`） | ⚠️ 需要确认；`ONLY_NOW` 必须给 cmdList |

如果改成"全分辨率、不下采样"，`resourceSize` 与输出一致，上表唯一的 ⚠️ 也消失；
2026-11-23 之后我们默认就是全分辨率 UI + 同时 tag HUDless/UI（mode 5），
不再依赖"两条流只在单卡有意义"的旧结论。

### 6.1 派生 UI 也走 mode 5（2026-11-23 实测更新）

- **游戏给的真 UI**：mode 5（或 3）是官方推荐，尤其 UE 和 2-bit alpha 场景；
- **我们自己差分的 UI**：原先把 mode 5 当成"会把场景颜色冻进 overlay"的风险项，因此
  `140_frame.inc` 里强制派生 UI 走 mode 4。2026-11-23 的 CP2077 A/B 表明 **派生 UI 走
  mode 5 反而更好**：mode 5 把我们自己的 UI 合成回 HUDless（`U + (1−a)H' = P`），
  边缘的 `(P−U)/(1−a)` 除法误差被合成抵消，不再出现 mode 4 的边缘环；XeFG 的
  backbuffer refine 还能进一步修正。
- 因此现在的规则是：**无论 UI 来源，只要 UI 全分辨率且当帧有 HUDless，就 tag 两者走
  mode 5**；`--no-ui-mode5` 保留为回退到 mode 4。覆盖率门控（旧 50..6000 bp）已移除——
  空 UI 在 mode 5 里合成回 HUDless。
- 仍要注意：mask 估错（例如把整块场景判成 UI）时，mode 5 会把它当 overlay 冻住；
  这类标题仍应靠 `UiDilate` / 阈值 / 或 `--no-ui-mode5` 处理。

---

## 7. 验收与错误码

- `XEFG_SWAPCHAIN_RESULT_ERROR_MISMATCH_INPUT_RESOURCES` (-12)：输入不满足所选模式的
  资源要求（最常见：mode 3/5 缺一张、格式不一致、尺寸不一致）；
- `XEFG_SWAPCHAIN_RESULT_ERROR_INCORRECT_INPUT_RESOURCES` (-14)：输入不足以插值；
- HDR 格式不符（FP16/scRGB）不会"降级"，直接不满足；
- `ONLY_NOW` 但 cmdList 为空：库无法录拷贝，行为未定义；
- `SetUiCompositionState` 每切一次会有 skip（我们按 10 帧处理），不要逐帧切。

---

## 9. 补充：支撑补丁（v34，mode 4/5 的共同 HUDless 来源）

`docs/design/UI_SUPPORT_PATCH.md`：渲染卡把 **HUDless 在 UI 支撑域上的切片** 走
stream 4（`HudlessStream` + `kHudlessFlagPatchPayload`），消费端
`H' = present outside / patch inside`：

- 没有游戏 UI：只 tag H' → AUTO 选 mode 4；
- 有游戏 UI：stream 3 tag UI，同时 tag H' → AUTO 选 mode 5。

patch 本身**永远不 tag 成 RES_UI**；`UiPatch=1` 发布默认，仅 R10/HDR。

## 10. 证据索引

| 内容 | 位置 |
|---|---|
| 资源类型（HUDLESS/DEPTH/MV/UI/BACKBUFFER） | `third_party/xess/inc/xess_fg/xefg_swapchain.h:100-110` |
| `RV_UNTIL_NEXT_PRESENT` / `RV_ONLY_NOW` | `xefg_swapchain.h:70-77` |
| `UITEXTURE_NOT_PREMUL_ALPHA` | `xefg_swapchain.h:81-96`（bit 5） |
| UI 模式 0–5 | `xefg_swapchain.h:275-294` |
| composition state（默认 DISABLED） | `xefg_swapchain.h:490-512` |
| `uiMode` 初始化字段 | `xefg_swapchain_d3d12.h:76-79` |
| 资源数据（type/validity/base/size/resource/state） | `xefg_swapchain_d3d12.h:89-103` |
| `TagFrameResource` | `xefg_swapchain_d3d12.h:256` |
| UI 合成公式 | `third_party/xess/doc/xess_fg_developer_guide_english.md:566-580` |
| 模式说明与必需资源表 | 同上 `:584-621` |
| HDR 约束 + 2-bit alpha 提示 | 同上 `:622-635` |
| validity 的拷贝语义与示例 | 同上 `:700-735` |
| 我们的 `uiMode` / `SetUiCompositionState` | `lock_frame/modules/040_cadence.inc:1168-1169,1393` |
| 我们的 UI / HUDless tag 规则 | `lock_frame/modules/140_frame.inc:1113-1145` |
| `SetUiCompositionState` 动态加载 | `src/xefg_dyn.h:54,95,168` |

