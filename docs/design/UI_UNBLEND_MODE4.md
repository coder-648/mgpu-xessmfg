# B 方案定案：UI 流 + 消费端 unblend + XeFG mode 4（2026-11-20）

> **状态（2026-11-20）**：代码已落地但**未通过验收**。因此发布上：
> `UiStream` 默认 **0**（producer 不发布 UI 流）、消费端 unblend 默认 **关闭**，
> 且**不提供任何开关**（UI 无项、`fakerFG.ini` 无键、发布说明不提及）。
> 只有本机 A/B 才用内部参数 `--ui-unblend` + 手写 `fakerFG.ini` 的 `UiStream=1`。
> 这份文档记录的是设计意图与后续验收计划，不代表已发布能力。
>
> **v29 更新（2026-11-21）**：stream 3 不再只能是游戏 UI。`fakerFG.ini` 的
> `UiSource=Auto|Game|Hudless|Off` 允许在没有 UI tag 时发送降采样 HUDless，消费端用
> `synth_ui.hlsl` 从（present - hudless）合成近似 UI，再走同一条 unblend → mode 4。
> 生产端仍然只做一次降采样 pass，不传全分辨率 HUDless。开关与门控见
> `lock_frame/docs/UI_SOURCE_V29.md`。

> 结论：只增加 **一条降采样 UI 流**。UI 流是启用 XeFG UI composition 的唯一门控。
> 生产端不做 HUDless 传输、不做 HUDless 呈现拷贝；消费端用 OptiScaler 的思路
> （`UiUnblend`）把 UI 从含 UI 的画面里去掉，重建 HUDless，再交给 XeFG。
> XeFG 使用 `BACKBUFFER_HUDLESS`（mode 4）：插值 HUDless，从 backbuffer 提取 UI。

---

## 0. 为什么是 B

| 方案 | 额外流 | 生产端 | 消费端 | XeFG mode | 结论 |
|---|---|---|---|---|---|
| A：UI + mode 3 | UI | 需要 present HUDless（`OutputReal <- HUDLess` 或链拷贝） | UI 升采样 | 3 | 画质最好，但生产端多一次 HUDless 呈现拷贝 |
| **B：UI + unblend + mode 4** | **UI** | **只拷 UI** | UI 升采样 + unblend | **4** | **本方案：生产端成本最低，性能成本给副卡** |
| C：HUDless + mode 4 | HUDless | 拷 HUDless（大） | HUDless 升采样 | 4 | 整帧变软；只在没有 UI 时用 |
| D：UI + mode 2 | UI | 只拷 UI | UI 升采样 | 2 | UI 仍可能拖影；作为 unblend 失败兜底 |

关键理由：

- UI 是叠加层，降采样只影响 UI；HUDless 是整帧插值底图，降采样会让整帧变软。
- 只加一个流时，**UI 流的信息密度最高**：它既能用于 unblend 重建 HUDless，也能在
  unblend 失败时退回 mode 2。
- 生产端只做“UI 降采样 + 拷贝”；HUDless 的获取成本放在副卡（consumer）。

---

## 1. 数据流

```text
游戏（渲染卡）
  ├─ 正常 present 含 UI 的 backbuffer（B 方案不 present HUDless）
  ├─ fakerFG harvest：UI = UIColorAndAlpha（DLSSG lane）或 UI/UIAlpha
  ▼
producer（游戏进程）
  ├─ UI 降采样（可同时做 scRGB→R10 PQ / premultiply）
  ├─ UI 写入 stream 3，与 MV/depth 同一发布槽、同一 fence
  └─ 不发布 colour、不发布 HUDless
  ▼
共享槽（stream 1/2/3）
  ▼
consumer（副卡）
  ├─ WGC 抓游戏窗口：含 UI 的 backbuffer（R10 PQ 目标）
  ├─ 拷贝 UI 到本地（stream 3 的降采样尺寸）
  ├─ convert：WGC 颜色 → R10 PQ 到 chain buffer
  ├─ unblend：HUDless = f(present_with_ui, UI)
  ├─ tag HUDless（成功时）；失败时 tag UI
  ├─ XeFG UI composition ENABLED（门控 = UI 流存在）
  └─ XeFG mode 4（HUDless + backbuffer）或 mode 2（backbuffer + UI）
  ▼
Present
```

**不变式**

1. B 方案下 fakerFG **必须 present 含 UI 的 backbuffer**；不能启用 `PresentHudless`。
2. producer 的 `ColourStream=0` 保持；不传输 colour/HUDless。
3. UI 流存在 = 允许启用 XeFG UI composition；UI 流不存在 = composition 保持关闭。
4. HUDless 与 UI **不要同时 tag**，否则 `AUTO` 会走 mode 5（三者都有），不是本方案。
5. unblend 成功 → 只 tag HUDless（mode 4）；unblend 失败 → 只 tag UI（mode 2）。

---

## 2. 生产端（lock_frame/producer/producer.cpp）

### 2.1 新增 UI 流

- `layout_streams()`：`specs[3]` 从 `DXGI_FORMAT_UNKNOWN` 改为 UI 的 format/内容尺寸/容量；
  `needed` 加入 `kStreamUi`。
- 新增全局：
  - `g_ui_w/h`（UI 源内容尺寸）
  - `g_stream_w/h[3]`（降采样后的传输尺寸）
  - `g_percent[3]`（`StreamPercentUi`，默认 200 = 50%）
  - `g_ui_format`（传输格式；与 consumer 的 chain buffer 一致，建议 R10G10B10A2_UNORM）
- 容量按降采样后的尺寸放置，避免 32 槽 × 全分辨率 UI 的共享内存开销。

### 2.2 UI 降采样 pass

新增 `lock_frame/producer/shaders/downsample_ui.hlsl`：

- 输入：UI 源（RGBA/R10/FP16）；
- 输出：`g_stream_w[3] × g_stream_h[3]`，**premultiplied alpha**；
- 如果源是 scRGB FP16 而目标 chain buffer 是 R10 PQ，同一 pass 做 `scRGB→PQ`；
- 降采样必须在 premultiplied 空间做，避免透明边缘发黑；
- 如果源已经是 R10 PQ，只做降采样。

`producer.cpp`：

- `g_ds_tex[3]` / `g_ds_pso[3]` / `g_ds_state[3]` 复用 stream 1/2 的机制；
- `scale_dispatch()` 增加 stream 3 分支：
  - SRV 格式 = UI 源格式；
  - UAV 格式 = `g_ui_format`；
  - 不做 depth 的 wide-format 转换；
- `on_record()`：
  - `const MgpuFgResource& ui = f->stream[MGPU_FG_STREAM_UI_COLOR];`
  - `want_ui = (want & kRequestUi) && ui.resource != nullptr;`
  - 从 `ui` 的 desc 更新 `g_ui_w/h/format`；
  - `need_scaled_ui = want_ui && g_scaled[3]`；在游戏 list 上 dispatch，再 copy `g_ds_tex[3]`；
  - 如果 `g_scaled[3] == false`，直接 copy UI（全分辨率）；
  - `g_copied_mask |= kRequestUi`；
  - `active_mask` / `stream_bytes[3]` 加 UI。
- `copy_stream()`：
  - `kStreamName` 增加 `"ui"`；
  - `can_barrier` 的条件从 `stream == 0` 扩到 `stream == 0 || stream == 3`；
  - `use_dma` 要检查 UI 的 state；UI 源通常不是 `COPY_SOURCE`，不能走纯 DMA copy 队列。
- `update_content()`：UI 尺寸变化时更新 `streams[3]` 的 live rect，并 bump `generation`。

### 2.3 UI 源与格式

- DLSSG lane：`MGPU_FG_STREAM_UI_COLOR` 就是 `UIColorAndAlpha`，含 RGB+alpha；
- NVNGX lane：`UI_COLOR` + `UI_ALPHA` 可能分开；第一版只支持一个含 alpha 的资源，
  否则 producer 需要一次 combine pass；
- 如果 UI 格式没有可用 alpha（例如某些 R10 A2 不是问题，但 alpha 精度低），记录日志，
  仍可做 unblend，但质量受限；
- `MgpuFgResource.format` 可能是 TYPELESS；producer 要映射到可拷贝的 typed format，
  与 `copy_formats_ok()` 一致。

---

## 3. 消费端（lock_frame/modules）

### 3.1 请求与拷贝 UI

- `g_stream_mask` 默认加入 `kRequestUi`（当 B 方案开启时）。
- `copy_inputs_local()` 已有 stream 3 的拷贝路径：
  - `g_ui_local[slot]` 按 `streams[3].width/height` 拷贝；
  - 这是**降采样后的 UI**，不是全分辨率。
- `active_mask` 没有 `kRequestUi` 时，不要启用 composition，也不要 unblend。


### 3.2 unblend pass

新增 `lock_frame/shaders/unblend_ui.hlsl`：

```hlsl
// t0 = present（含 UI，R10G10B10A2_UNORM，已转 PQ）
// t1 = UI（降采样，和 present 同色彩空间/格式）
// u0 = HUDless（R10G10B10A2_UNORM）
// 常量：DstW, DstH, UiW, UiH, Premultiplied, OpaqueAlpha
//
// premultiplied:
//   hudless = (present - ui.rgb) / max(1 - ui.a, eps)
// straight:
//   hudless = (present - ui.rgb * ui.a) / max(1 - ui.a, eps)
//
// alpha >= OpaqueAlpha：场景不可恢复，先用 present（或上一帧 HUDless）占位，
// 避免黑边；具体策略在 shader 常量里切换。
```

- UI 在 shader 内用 bilinear（或 alpha 引导的 4-tap）升采样到 `DstW/DstH`；
- 先做 premultiply 再减，避免透明边缘误差；
- `max(1-a, eps)` 的 eps 取 1/255 或 1/1023；
- 输出 HUDless 的尺寸 = chain buffer 尺寸；
- 第一版在 **R10 PQ 空间** unblend；如果 HDR 半透明 UI 误差大，后续改成
  “WGC FP16 scRGB 空间 unblend → 再转 R10”。

### 3.3 插入位置

`lock_frame/modules/140_frame.inc` 的 convert draw 之后：

```text
warp/convert draw → chain buffer (R10 PQ with UI)
  transition chain: RENDER_TARGET → NON_PIXEL_SHADER_RESOURCE
  dispatch unblend: chain + g_ui_local → g_hudless_local[slot]
  transition chain: NON_PIXEL_SHADER_RESOURCE → RENDER_TARGET → PRESENT
```

- `g_hudless_local[slot]`：output 尺寸、R10 PQ、`ALLOW_UNORDERED_ACCESS`；
- 只在 `g_ui_unblend && ui_active && g_ui_local[slot]` 时运行；
- 失败/跳过时清空 `g_hudless_valid[slot]`，不要 tag 上一帧 HUDless。

### 3.4 tag 规则

在 `140_frame.inc` 的 tag 区：

```text
if (g_ui_unblend && hudless_valid) {
    tag XEFG_SWAPCHAIN_RES_HUDLESS_COLOR(g_hudless_local[slot]);
    // 不要 tag UI：mode 4 从 backbuffer 提取 UI
} else if (ui_active) {
    tag XEFG_SWAPCHAIN_RES_UI(g_ui_local[slot]);
    // mode 2 兜底
}
// 两者互斥，避免 AUTO 选到 mode 5
```

- HUDless tag 的 `resourceSize = { g_full_w, g_full_h }`；
- `incomingState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE`（或 COMMON，
  与 barrier 一致）；
- `tag_list` 用 `g_list.Get()`，不要用 nullptr 延迟到 Present，因为 HUDless 是本地纹理；
- backbuffer 必须是含 UI 的画面（B 方案不变）。

### 3.5 XeFG composition 门控

- 新增 `xefg_dyn` 的 `PFN_SetUiCompositionState` 并加载
  `xefgSwapChainSetUiCompositionState`。
- 初始化（`040_cadence.inc`）：
  - `have_ui_stream = streams[3].slot_size != 0 && streams[3].width >= 64`；
  - `ip.uiMode = have_ui_stream ? XEFG_SWAPCHAIN_UI_MODE_AUTO
                               : XEFG_SWAPCHAIN_UI_MODE_NONE`；
  - `SetEnabled` 之后，如果 `have_ui_stream`：
    `SetUiCompositionState(ctx, ENABLED)`；否则 `DISABLED`。
- 运行时：
  - UI 流存在时保持 ENABLED；
  - UI stream 连续 miss N 帧（N ≥ 3）才 DISABLED；
  - 不要逐帧切换：XeFG 改 composition state 会 skip 10 帧。
- `AUTO` 会根据实际 tag 选：
  - 只有 HUDless → mode 4；
  - 只有 UI → mode 2；
  - 两者都有 → mode 5（本方案避免）。

---

## 4. 成本

4K / R10 4B/px 估算：

| 项 | 字节/帧 | 位置 |
|---|---:|---|
| UI 50% 传输 | 8.3 MB | producer→consumer |
| UI 100% 传输 | 33.2 MB | 仅 A/B |
| unblend 读 present | 33.2 MB | consumer 本地 |
| unblend 写 HUDless | 33.2 MB | consumer 本地 |
| UI 升采样 | 读 8.3 / 写 33.2 | 可并入 unblend |
| XeFG mode 4 | 读 HUDless + backbuffer | 原有 FG 开销 |

- producer：只多 UI 降采样 + 拷贝；无 HUDless 呈现拷贝；
- consumer：多一个全分辨率 unblend dispatch（约 0.3–0.8 ms 量级，需实测）；
- 共享内存：UI 50% × 32 槽 ≈ 265 MB（R10）；全分辨率约 1.06 GB，不建议。


---

## 5. 回退矩阵

| 情况 | 行为 |
|---|---|
| 没有 UI stream / `active_mask` 无 UI | composition DISABLED；不 tag HUDless/UI；当前插值行为 |
| UI stream 存在，unblend 成功 | tag HUDless；mode 4 |
| UI stream 存在，unblend 失败 | 清 HUDless；tag UI；mode 2 |
| UI stream 连续 miss N 帧 | composition DISABLED；清 tag |
| HUDless 格式/尺寸不匹配 | 不 tag HUDless；退回 mode 2 |
| R10 alpha 导致 unblend 不稳定 | 关 unblend；mode 2；记录日志 |
| HDR 半透明 UI 误差大 | 后续改为 FP16 scRGB 空间 unblend；或 mode 2 |

---

## 6. 验证

### 6.1 功能

- producer 日志：`ui: WxH -> w x h fmt F`、`publish ... ui=1`；
- consumer 日志：`UI stream: copying`、`unblend: ...`、`tag hudless` / `tag ui`；
- `streams -> mv=1 depth=1 color=0 ui=1`；
- `SetUiCompositionState: ENABLED`；
- 画面无黑屏、无 UI 双份、无 marker 残留。

### 6.2 画质

- 静态 HUD 文字/1px 线：UI 50% vs 75% vs 100%；
- 快速平移：UI 是否拖影；
- 半透明 UI：是否闪烁/色边；
- 世界空间标记：mode 4 下是否稳定；
- HDR：半透明 UI、亮部 UI；
- unblend 伪影：UI 边缘光晕、暗边、过曝。

### 6.3 性能

- `total p95`、`gpu`、`show`、`HITCH`；
- unblend pass 单独打点；
- `stream_bytes[3]`、PCIe 带宽；
- 共享内存 `used=`。

### 6.4 失败场景

- 菜单/加载：UI/HUDless 断供；
- consumer 退出/重启；
- 分辨率/超分档变化；
- UI 资源 `ONLY_NOW`；
- UI 格式 alpha 精度不足。

---

## 7. 风险

| 风险 | 处理 |
|---|---|
| unblend 在 PQ 空间不精确 | 先测 SDR；HDR 改用 FP16 scRGB 空间 |
| R10 2-bit alpha | 半透明 UI 误差；mode 2 兜底 |
| UI 降采样后文字软 | 50% 起步；必要时 75% 或 RGB 50% + alpha 100% |
| XeFG AUTO mode 切换 | 只 tag HUDless 或只 tag UI，二者互斥；加 miss 滞回 |
| 10 帧 skip | 不要逐帧切 composition；门控按 UI stream 存在性 |
| UI 源只有 UI 没有 alpha | producer 拒绝/记录；mode 2 |
| 不透明 UI 无法反解 | shader 用 present/上一帧占位；边缘可能轻微 |

---

## 8. 落地顺序

1. producer：stream 3 布局 + UI 降采样 + copy + active_mask；
2. consumer：请求 UI、本地拷贝、`SetUiCompositionState` 门控；
3. consumer：`unblend_ui.hlsl` + HUDless 纹理 + tag HUDless（mode 4）；
4. 回退：unblend 失败时 tag UI（mode 2）；
5. 实测：4K/锁 40、2x@60、HDR/SDR、半透明/世界空间 UI；
6. 若 unblend 质量不足，再评估 A 方案（UI + mode 3）或 HUDless 流（C）。


---

## 9. 落地状态（2026-11-20，代码已编译）

已实现：

| 项 | 位置 | 状态 |
|---|---|---|
| producer stream 3 布局/放置 | `lock_frame/producer/producer.cpp` `layout_streams()` / `ensure_shared()` / `place_slots()` | ✅ |
| UI 降采样 + scRGB→PQ + premultiply | `lock_frame/producer/shaders/downsample_ui.hlsl`、`scale_dispatch(..., 3)` | ✅ |
| UI 与 MV/depth 同发布、`active_mask`/`stream_bytes` | `producer.cpp` `on_record()` / `publish_frame()` | ✅ |
| UI 请求位 | `lock_frame/modules/020_state_overlay.inc`、`160_main.inc` | ✅ |
| consumer UI 本地拷贝 + freshness | `060_frame_io.inc` `g_ui_local` / `g_ui_fresh` | ✅ |
| consumer unblend pass | `085_ui_unblend.inc`、`shaders/unblend_ui.hlsl` | ✅ |
| HUDless tag 与 UI tag 互斥 | `140_frame.inc` tag 区 | ✅ |
| XeFG composition 门控 | `040_cadence.inc` `SetUiCompositionState` | ✅ |
| `xefg_dyn` 加载 `SetUiCompositionState` | `src/xefg_dyn.h` | ✅ |
| 构建 | `./lock_frame/build.sh producer/consumer/faker` | ✅ 通过 |

尚未做（需要 Windows 实机）：

- 4K 锁 40 / 2x@60 的 UI 拖影、文字、半透明 UI、HDR A/B；
- unblend 的 `total p95` / `gpu` 实测；
- XeFG 是否接受当前 UI 尺寸（我们已升到输出尺寸再 unblend/tag，路径上不依赖它接受低分辨率 UI）；
- UI 源为 `UIColorAndAlpha` 之外的 lane（UI + UIAlpha 分开）时的 combine pass。

---

## 10. sm86 0.4.2 的处理方式（参考资料，2026-11-21 补）

随包副本：`third_party/dlssg_for_sm86-main/`（0.4.2，2026-11-14）。它的两条引擎对
UI / HUD-less 的处理不同，且都**不做 unblend**：它只消费游戏交出的平面，或在没有
平面时把 UI 烘进唯一输入。

### 10.1 它做了什么

| 引擎 | 键 | 行为 | 出处 |
|---|---|---|---|
| DLSS-G | `[DLSSG] Preset` | `A`（默认）= UI 烘进颜色历史一起插值；`B` = UI 重组，**只有游戏同时交出 `DLSSG.HUDLess` 和 `DLSSG.UI` 才生效**；缺一张就与 `A` 完全相同，每个特性句柄在第一次 Evaluate 记一条 Level 1 的 `preset_ineffective`（写明缺哪张），不逐帧刷；`C` = FG Lite，UI 重组同时关掉 | `docs/INSTALL.md` 164 / 592–598 / 631、`README.md` 327 |
| XeSS-FG | `[XeSSFG] UseHudless` | 0.4.2 起 `Auto` **与 `0` 相同**（2026-11-10，C139）：后缓冲做网络输入，整幅画面（含 UI）一起插值，游戏给不给 HUD-less 都一样，不再产生 `xefg_hudless_unused`；`1` = 严格档，每帧都用 HUD-less 做网络源；缺平面或 UI 平面与后缓冲尺寸不同即 `xefg_kernel_fatal{kind:"hudless"}`，进程以 `0xE0D55001` 结束 | `README.md` 24、`dlssg_sm86.ini` 75–82、`docs/INSTALL.md` 133–135 / 166 |
| 历史 | — | 2026-10-19 之前取值只有 `0/1`、出厂 `1`；10-19 到 11-10 之间 `Auto` 是**逐帧判定**；11-10 起 `Auto` 收敛成 `0` | `docs/INSTALL.md` 232 |

另外两条与其实现相关的约束：

- 运行库**比较指针**来判断“给没给 HUD-less”，所以同一个资源被两个参数共用时只存一份
  （`docs/CAPTURE.md` 98、`docs/ARCHITECTURE.md` 455）。平面按需抓取时同理（`CAPTURE.md` 105）。
- 平面交不交由游戏决定：本机 4K 抓取里**黑神话悟空两张都交，赛博朋克 2077 只交
  `DLSSG.HUDLess`、不交 `DLSSG.UI`**；`Preset=B` 在 2077 上是空操作
  （`docs/INSTALL.md` 596）。它的诊断工具会打印一行 `HUD 平面:` 直接回答这个问题。
- 画质归因：HUD / 浮空 UI 抖动走 `Preset` 这条路径；**场景细结构闪烁、遮挡边缘重影
  不受它影响**，`Preset=B` 也不是这类现象的解法（`docs/INSTALL.md` 598–606）。
- 成本：`B` 需要两张全分辨率 FP16 平面，1080p ≈ 33 MB、4K ≈ 133 MB（`INSTALL.md` 592）。

### 10.2 对我们（UI 流 + unblend + mode 4）的意义

**可参考的（策略层）**

1. **默认烘 UI、分离是显式 opt-in**：sm86 在 3 周内把“逐帧 Auto”收敛回“默认等于
   不做”，说明逐帧自适应的行为难以解释、维护成本高。我们的 `UiStream` 默认 0 是对
   的；恢复发布时也应保持“默认关 + 显式开启”，不要做逐帧自动开关。
2. **平面可用性必须按游戏探测，并作为开关前置条件**。我们的 UI 源是
   `sl::kBufferTypeUIColorAndAlpha`（`lock_frame/faker_fg/proxy_streamline.cpp:163`）
   或 NVNGX 的 `NVSDK_NGX_DLSSG_Parameter_UI` + `_UIAlpha`
   （`lock_frame/faker_fg/provider.cpp:118–119`）。当游戏只交 HUD-less、不交 UI 时，
   这条路没有输入：composition 应保持关闭，并明确记一条原因，而不是让 unblend 失败
   后静默退回 mode 2。
3. **一次性诊断而不是每帧刷**：`preset_ineffective` 按“每个特性句柄第一次 Evaluate”
   记一次，字段 `hudless/ui/missing[]` 直接说明缺哪张。我们的 `record_ui_unblend()`
   现在只在**首次成功**时打一条 `HUDless rebuilt`（`085_ui_unblend.inc:223–227`），
   失败分支（`140_frame.inc:1080–1085`）没有任何日志——验收时无法区分“没有 UI 平面”
   与“unblend 坏了”。建议照它的做法补一条带 `reason` 的状态变化日志（首次 + 状态翻转）。
4. **尺寸/格式是硬条件，不满足就不 tag**：sm86 严格档要求 HUD-less / UI 与后缓冲同
   尺寸，否则直接失败而不是猜。我们已经在 tag 前校验 `streams[3]` 尺寸与 `g_ui_fresh`，
   应把同样的规则显式写进日志（UI 尺寸、格式、alpha 位宽、是否 premultiplied）。
5. **成本论证支持我们的取舍**：它两张全分辨率平面 4K ≈ 133 MB；我们一条 50% 的 R10
   UI 流 4K ≈ 8.3 MB（本文 §4），代价是消费端一次 unblend dispatch。这个方向值得
   保留，但验收指标应只针对 **HUD 区块**，不要把场景细结构闪烁算成回归（sm86 明确
   那属于运行库算法）。
6. **不要逐帧切 composition**：它文档里同样强调状态切换的代价；我们已有“连续 miss
   N 帧才关”的滞回（本文 §3.5），与 0.4.2 的收敛方向一致。

**不能照搬的**

- sm86 不做反解：`UseHudless=1` 是“HUD-less 做网络源 + 自己合成 UI”，对应我们的
  **mode 3**（UI + HUDless 都 tag），不是 mode 4。unblend 的数学、alpha 空间与回退
  仍以 OptiScaler 的 `UiUnblend` 和我们自己的 `shaders/unblend_ui.hlsl` 为准。
- DLSS-G 的 UI 重组由 NVIDIA 运行库完成，XeFG 的 composition 由 Intel 运行时完成；
  两者的“谁合成 UI”不同，不能把 `Preset=B` 的行为当成 XeFG mode 4 的参照。

### 10.3 建议的下一步（按游戏分层验收）

| 游戏 | 已观测的平面 | 用途 |
|---|---|---|
| PRAGMATA | FG 开时 `hudless=y ui=y`（`docs/findings/FG_INPUT_V13_PRAGMATA.md:281`） | **主验收题**：UI 流 / mode 2 / mode 4 的 HUD 质量与 unblend 成功率 |
| Cyberpunk 2077 | 我们自己的抓取 `hudless=n ui=n`（`FG_INPUT_V13_2077.md:298`，FG 关）；sm86 的 4K 抓取也只交 HUDLess | **只做“无 UI 平面”回归**：确认 composition 关闭、画面与 1.2.1 一致、日志给出原因；不要用它验收 mode 4 |
| Wuchang / Khazan | Khazan 没有 `UI_COLOR_AND_ALPHA` tag（`FG_INPUT_V13_KHAZAN.md:215`） | 验证“缺平面”路径与回退文案 |

顺序建议：先 `UiStream=1` + 只 tag UI（mode 2，不依赖 unblend）确认 UI 不再拖影 →
再开 `--ui-unblend` 走 mode 4，A/B 看 HUD 区块与 unblend 光晕/暗边 → 最后把 2077
的“没有 UI 平面”回归锁进测试清单。每一步都以“producer `streams[3]` 有尺寸 +
consumer `tag ui/hudless` 各一条日志”为通过条件，而不是只看观感。

