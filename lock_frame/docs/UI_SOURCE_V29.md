# UI 流 v29：有 UI 用 UI，没有 UI 用 HUDless 自造

> **1.3.0 更新**：UI 合成**默认开启**（producer patch + consumer 还原干净 HUDless，
> XeFG mode 4）；`UiPatch=0` 可回退。下面保留 v29 的历史数据流说明。
>
> **2026-11-23 更新**：当时 UI 合成是实验性，发布默认关闭（`UiStream=0` / `UiSource=Off`）。
> 手动打开时，消费端把 HUDless 和 UI 一起 tag，XeFG 选 **mode 5**（`--no-ui-mode5` 才回退
> mode 4）。下面描述的数据流不变，只是最后的 tag 组合变了。

> 状态：代码已落地，默认关闭；需要显式打开 `UiStream=1` + `--ui-unblend`。
> 目的只有一个：**消除 XeFG 插值时的 UI 拖影**（插值在 HUDless 上做，UI 从全分辨率
> backbuffer 提取）。

## 1. 数据流

```text
游戏（渲染卡）
  ├─ UI tag 存在？ ──是──► 降采样 UI（一次 pass）──► stream 3（GameUi）
  ├─ 没有 UI，但 HUDless 存在且 != Backbuffer
  │                     └─► 降采样 HUDless（同一个 pass）──► stream 3（Hudless）
  └─ 两者都没有 ──► 不发布 stream 3（Off）

消费端
  ├─ WGC 抓全分辨率合成图（backbuffer，含 UI）
  ├─ stream 3 = GameUi:
  │     现有 unblend：H = (P - UI) / (1 - a) ──► tag HUDless ──► XeFG mode 4
  ├─ stream 3 = Hudless:
  │     1. 用全分辨率 P + 低分辨率 H 合成近似 UI：
  │          d = P_low - H
  │          m = smoothstep(t, 2t, max|d|)
  │          UI.rgb = P_low - (1-m) * H,  UI.a = m
  │     2. 升采样 UI，unblend 出 H_full
  │     3. tag H_full ──► XeFG mode 4
  └─ Off: 不开 UI composition（保持原行为，UI 会拖影）
```

生产端只做**一次**降采样 pass：有 UI 读 UI，没有 UI 读 HUDless。不会新增第二次
全屏 pass，也不传输全分辨率 HUDless。

## 2. 开关

### 游戏侧 `fakerFG.ini`

```ini
UiStream=1          ; 总开关；0 = 完全不发布 stream 3
UiSource=Auto       ; Auto | Game | Hudless | Off
StreamPercentUi=200 ; 200 = 每个维度 1/2（4K R10 约 8.3 MB/帧）
                    ; 150 = 2/3（4K 约 14.7 MB/帧，细 UI 更安全）
UiPremultiplied=1   ; 只对 GameUi 生效；Hudless 源自动按直通色处理
```

### 消费端

```bat
lock_frame.exe ... --ui-unblend
:: 或显式指定来源（同时打开 unblend）：
lock_frame.exe ... --ui-source auto
lock_frame.exe ... --ui-source game
lock_frame.exe ... --ui-source hudless
lock_frame.exe ... --ui-source off
:: 近似 UI 的差分阈值（0..255，默认 8；越小越敏感）
lock_frame.exe ... --ui-unblend --ui-synth-threshold 6
```

## 3. 门控规则

- `UiStream=0`：直接 Off，生产端不跑降采样 pass。
- `UiSource=Auto`：优先 `UI_COLOR` tag；否则使用 `HUDLESS`。
- `UiSource=Game`：只接受 UI tag；没有就 Off。
- `UiSource=Hudless`：只接受 HUDless；没有就 Off。
- HUDless 与 `BACKBUFFER` 是同一张资源时视为 Off（没有真正分离 UI）。
- 尺寸 < 64、格式为 0、非同一帧：视为不可用。
- 消费端 `--ui-source` 可覆盖 producer 的选择，便于 A/B。

## 4. 预期效果与限制

- **不透明 HUD**（准星、血条、字幕底板）：mask 基本正确，UI 不再被 MV 拖动。
- **半透明/加法 UI**：单帧 `(P, H)` 无法唯一还原 `(RGB, alpha)`，mask 只是差值强度，
  可能过实/过虚；最终 UI 仍由 XeFG 从全分辨率 backbuffer 提取，所以观感通常可接受，
  但半透明区域可能有边缘误差。
- **细 UI**：50% 降采样可能漏检 1–2 px 的线；优先改 `StreamPercentUi=150` 或提高阈值敏感度。
- **色彩空间**：P 与 H 必须同为 R10 PQ（HDR）或同一 SDR 空间；FP16 scRGB 的 HUDless
  会在 producer 的降采样 pass 里转 PQ。
- **R10A2 的 alpha 只有 2 bit**：合成的 `m` 会被量化到 0/1/3、2/3、1，
  半透明 UI 的 unblend 会更粗糙；需要更细的 alpha 时只能换 RGBA8 流（R10 PQ 的
  HDR 颜色会受影响），当前版本没有自动切换。
- **不保证**：没有 HUDless、或 HUDless 就是 backbuffer 的游戏无法造 UI；此时保持 Off。

## 5. 输出自造 UI 的视觉参考

可以在消费端把合成的低分辨率 UI 落盘，用于确认 mask/alpha 是否抓到了 HUD：

```bat
lock_frame.exe ... --ui-unblend ^
  --ui-dump 5 --ui-dump-after 120 --ui-dump-dir ui_dump_3x
```

- `--ui-dump N`：最多导出 N 帧（1–64）。
- `--ui-dump-after N`：跳过前 N 帧（默认 60，避开加载/菜单）。
- `--ui-dump-dir DIR`：输出目录，默认 `ui_dump`。

每帧生成：

```text
ui_00_rgb.bmp    // premultiplied synth UI RGB
ui_00_alpha.bmp  // 差分 mask（白 = 被判定为 UI）
ui_00_over.bmp   // 合成到棋盘背景，便于看半透明边缘
ui_00_meta.txt   // 尺寸/格式
```

BMP 是 R10G10B10A2 直接量化的 8-bit 视图，**不是最终上屏画面**；它只用于判断 UI 覆盖范围、
细线是否漏检、半透明区域是否被 mask 吃掉。最终 UI 仍由 XeFG 从全分辨率 backbuffer 提取。

## 6. 验证


1. `fakerFG.ini`：`UiStream=1`、`UiSource=Auto`；游戏内开 DLSS / FG（让 tag 有数据）。
2. `lock_frame.exe ... --ui-unblend`。
3. 日志应出现：
   - producer：`UI source = game UI tag` 或 `UI source = HUDless ...`
   - consumer：`ui path: unblend=1 source=...`
   - `ui-unblend: synth UI ...` / `HUDless rebuilt ... source=hudless`
   - `SetUiCompositionState(ENABLED)`（`040_cadence.inc`）。
4. A/B：`--ui-source off`（拖影）对比 `--ui-source auto`（无拖影）。
5. 回归：半透明菜单、加法发光 UI、快速转视角、HUD 动画。
