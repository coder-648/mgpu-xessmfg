# 现有 UI 计算方式与理论效果（2026-11-22）

> **2026-11-23 状态**：UI 合成整体标记为**实验性**，发布默认关闭
> （`UiStream=0` / `UiSource=Off`）。本文是打开后的误差分析。

> 对象：`UiSynthesis` 这条实验链路的两种源（GameUi / Hudless）+ 消费端 unblend。
> 代码：`lock_frame/producer/shaders/downsample_ui.hlsl`、`lock_frame/shaders/synth_ui.hlsl`、
> `lock_frame/shaders/unblend_ui.hlsl`、`lock_frame/modules/085_ui_unblend.inc`。
>
> 结论先行：
>
> 1. **低频部分是自洽的**：只要 `a' < 1`，重建出的 HUDless 低频恒等于传过来的
>    `H_low`，与 mask 是否等于真实 alpha **无关**；mask 只决定"哪里把低频换成黑"。
> 2. **主要误差不是 mask 的数学，而是格式**：HDR 路径的 R10A2 只有 **2-bit alpha**，
>    等价于把 `m ≥ 0.83` 全部当成不透明（写黑）；其次是 50% 下采样丢 1px 线。
> 3. **默认走 mode 5（HUDless + UI）**：我们提供的 UI 由 XeFG 合成回 HUDless
>    （`U + (1−a)H' = P`），边缘的除法误差被合成抵消；`--no-ui-mode5` 才回到 mode 4
>    （UI 由 XeFG 从全分辨率 backbuffer 提取）。不透明 UI 观感几乎无损，半透明 UI 的
>    误差会以"UI 本身略错"而不是"底图边缘环"的形式出现。

---

## 1. 两条路径与各自的输入

| 路径 | stream 3 的内容 | 消费端做的事 | 代码 |
|---|---|---|---|
| **GameUi**（游戏给了 UI） | 降采样的 UI（premultiplied，RGB+A） | unblend：`H = (P − U)/(1−a)` | `085` 的 unblend |
| **Hudless**（游戏只给 HUDless） | 降采样的 HUDless（场景） | 先 synth：`U = p − (1−m)h, a = m`，再 unblend | `record_ui_synth()` + 两个 PSO |

共同点：

- 生产端 2×2 box，premultiplied 空间，FP16 scRGB 源顺带 `scRGB→BT.2020 PQ`
  （`downsample_ui.hlsl:47-84`）；
- 消费端的 `P` 是**转换后的链缓冲**（R10 PQ），与 UI 流同一色彩空间；
- `OpaqueAlpha = 0.99`，`Threshold = 8/255`，`Premultiplied = 1`（`085:698-699`）；
- `Dilate` 已实现（producer 的 `synth_ui_full.hlsl` 3×3 max，`UiDilate=1` 打开，默认 0）；
  consumer 的 `synth_ui.hlsl` 里这个字段仍未使用。

---

## 2. 数学：把重建拆成低频 + 高频

真实合成（premultiplied）：`P = U + (1 − a)·H`，其中 `U = a·C`。

**synth（Hudless 路径）**：

```text
p = box2x2(P)                // 低分辨率 P
h = H_low                    // 传输过来的低分辨率 HUDless
d = p − h
m = smoothstep(t, 2t, max|d|)
U' = p − (1 − m)·h           // 写入时 saturate
a' = m
```

**unblend（两条路径共用）**：

```text
H' = (P − U') / max(1 − a', eps)     // a' < 0.99
H' = 0                               // a' ≥ 0.99
```

把 `U'` 代进去（`a' = m`，忽略双线性/量化）：

```text
H' = h + (P − p) / (1 − m)
```

这个式子说明两件事：

1. **低频项恒为 `h`**：只要 `m < 1`，`H'` 的低频就是传过来的 HUDless，与 `m` 是否等于
   真实 alpha 无关。**mask 不需要"估准 alpha"才能把低频场景恢复对**——它估错只影响
   高频项的放大倍数，以及是否落进 `m ≥ 0.99` 的黑分支。
2. **高频项来自 `P`**：`(P − p)/(1 − m)` 把 P 的高频细节保留下来（场景细节因此不会因为
   50% 的 H 而整体变软），但**UI 的边缘也在 P 的高频里**，所以 UI 边缘会残留，且被
   `1/(1−m)` 放大。这就是"光晕/暗边"的来源。

**GameUi 路径**没有 synth 的 mask 假设：`U` 是测出来的，`H' = (P − U_low)/(1 − a_low)`，
误差只来自 resampling/量化（下一节）。

---

## 3. 误差来源排序（按理论影响）

### 3.1 2-bit alpha（HDR 路径的最大问题）

`ui_transport_format()` 把 `R10G10B10A2` / `FP16` 源映射到 **R10A2**（`producer.cpp:625-639`），
alpha 只有 2 bit：`a_q ∈ {0, 1/3, 2/3, 1}`。代入上式：

```text
H' = h·(1 − m)/(1 − a_q) + (P − p)/(1 − a_q)
```

- `a_q > m` ⇒ 低频被**提亮**；`a_q < m` ⇒ 低频被**压暗**；`a_q = 1` ⇒ `H' = 0`（黑）；
- 舍入到最近值时，`m ≈ 0.83` 以上就会量化成 1（具体边界取决于驱动对 UNORM 的舍入）
  ⇒ **不透明分支的实际阈值是 ~0.83，不是 0.99**；
- 半透明 UI 只要对比度稍高（m 大）就会被当成不透明 ⇒ **黑边/黑块**；
- 这就是 `UI_UNBLEND_MODE4.md` §11.3 描述的"0 / 0.33 / 0.67 / 1 两端跳变"的数学来源。

**关键补充**：SDR 源是 `R8G8B8A8`，走 RGBA8 流（8-bit alpha），上面这条基本不成立；
**2-bit 问题是 HDR（R10/FP16）特有的**。

### 3.2 50% 下采样 + 双线性

- 1 px 的线/文字笔画在 2×2 box 里只占一半权重，`m` 可能低于阈值 ⇒ mask 漏检 ⇒
  这条线留在 `H'` 里（底图带 HUD）⇒ 插值帧上出现 1px 拖影；
- 反过来，细边缘的两个像素可能都被判成 UI ⇒ 边缘被写黑 1–2 px ⇒ 暗边；
- 低频项用 `h`（50% HUDless）⇒ 场景在 UI 附近偏软；高频项用 `P` 补回来，但补的是
  **含 UI 的高频**。

### 3.3 在 PQ 空间做减法和阈值

PQ 是非线性的：同样的码值差在暗部对应大的相对亮度差、在亮部对应小的相对亮度差。
`Threshold = 8/255` 是码值阈值，所以：

- 亮场景里的暗 UI ⇒ 差小 ⇒ **漏检**；
- 暗场景里的亮 UI ⇒ 差大 ⇒ **过检**（黑块）；
- `(P − U)` 在 PQ 空间不等于线性光下的减法 ⇒ HDR 半透明 UI 有系统性偏差。

### 3.4 `saturate(u)` 截断负值

`synth_ui.hlsl:70` 对 premultiplied UI 做 `saturate`。当 UI 让某通道**变暗**（U<0，例如
压暗的 HUD 底板）或 `P/H` 有轻微错位时，负值被截成 0 ⇒ `H'` 被抬亮 ⇒ 底板区域发灰。
GameUi 路径同样受 `unblend` 里的 `saturate` 影响。

### 3.5 其他

- `OpaqueAlpha = 0.99` 的硬分支：`m ≥ 0.99` 写黑。对不透明 UI 是对的（mode 4 从
  backbuffer 取 UI），对"被高估的半透明 UI"就是黑块；
- `Dilate` 默认关（`UiDilate=1` 打开，producer 全分辨率路径）：AA 边缘没有被 mask 覆盖时，边缘残留无法用膨胀补救；
- **NVNGX 的 `UIAlpha` 没有被消费**：`MGPU_FG_STREAM_UI_ALPHA` 在 API 里定义了
  （`mgpu_fg_api.h:121`），但 producer/consumer 都没用它（全仓只有这一处引用）——
  游戏分开发 UI 和 UIAlpha 时，alpha 直接丢了。

### 3.6 口径更新（2026-11-22）：无损链路 + 不下采样

前提改成：**stream 3 永远是全分辨率、无损**（UI 或 HUDless 都不再 2×2 降采样）。
这会**消掉 3.2 整条误差**（1px 线漏检、H_low 变软），但其余误差原样保留：

| 误差 | 半分辨率（旧） | 全分辨率（新） |
|---|---|---|
| 50% box + 双线性的细线/AA | 有 | **无** |
| `H_low` 导致的底图低频变软 | 有 | **无**（H 全分辨率） |
| 2-bit alpha（HDR） | 有 | **有**（格式没变） |
| PQ 空间减法/阈值 | 有 | 有 |
| mask 估计（Hudless 路径） | 有 | 有（但输入不再被降采样放大） |
| `saturate` 截断 | 有 | 有 |

理论上，**GameUi 路径在全分辨率 + 无损后只剩"量化 + PQ"两项**；Hudless synth 路径
剩"mask + 量化 + PQ"。也就是说：不下采样买到的是"不再系统性丢细线"，但**半透明/HDR
的黑边仍然存在**，因为根源是 R10A2 的 2-bit alpha，不是分辨率。

#### 成本（4K，相对今天 MV+depth 26.3–29.0 MB / 2.02–2.23 ms / 环 1.53 GB）

| 方案 | 字节/帧 | 链路名义 | 环（32 槽） | 渲染卡 | 副卡 |
|---|---:|---:|---:|---|---|
| UI 半分辨率纹理（今天） | 8.29 MB | 0.64 ms | 265 MB | 0.1–0.3（降采样） | 拷贝 ~0.24 + unblend 0.3–0.8 |
| **UI 全分辨率纹理** | 33.18 MB | 2.55 ms | 1.06 GB | 0 | 拷贝 ~1.9 + unblend 0.3–0.8 |
| **UI 全分辨率 + 码流** | 0.26–3.0 MB | 0.02–0.23 ms | 1.09 GB | **+1.02（编码）** | 解码 0.58–0.72 + unblend 0.3–0.8 |
| HUDless 全分辨率纹理 | 33.18 MB | 2.55 ms | 1.06 GB | 0 | 拷贝 ~1.9 |
| **HUDless 全分辨率 + 码流** | 22.7–27.6 MB | 1.75–2.12 ms | 1.10 GB | +1.02（+0.19 若 FP16→R10） | 解码 0.58–0.72（省 unblend） |

按 `LOSSLESS_TRANSPORT.md` §2 的时间不等式（`B − B' > BW·(T_enc − T_down)`，
`BW = 13 GB/s`、`T_enc = 1.02 ms` ⇒ 等效 13.3 MB）：

- **UI 全分辨率 + 码流成立**：省 30–33 MB ≫ 13.3 MB；
- **HUDless + 码流不成立**：只省 5.6–10.5 MB < 13.3 MB（除非副卡才是瓶颈）；
- **副卡侧的账反过来**：全分辨率纹理的跨卡拷贝约 1.9 ms，而码流解码只有 0.58–0.72 ms。
  所以在 4K/3x 这种副卡没有余量的配置上，**全分辨率 UI 应该走码流**（用渲染卡 1.02 ms
  换副卡 1.2 ms）；HUDless 是否走码流取决于渲染卡还有没有 1 ms。

结论：**"不下采样"之后，UI 的正确组合是"全分辨率 + 码流"**（字节小、副卡解码便宜）；
HUDless 则要按档位选（渲染卡紧就传原样，副卡紧就编码）。两者都不再需要"半分辨率"
这个折中，也就不会再有"细线被 50% 吃掉"的系统性问题。

---


## 4. 按 UI 类型的理论效果

> 下表按**旧口径（半分辨率）**列出；在 §3.6 的"全分辨率 + 无损"口径下，凡是写
> "下采样/50%"的误差项都消失，其余（2-bit、PQ、mask、saturate）不变。

| UI 类型 | GameUi 路径 | Hudless synth 路径 | 最终观感（mode 5 默认 / mode 4） |
|---|---|---|---|
| **不透明 HUD**（准星、血条、字幕底板） | 低频/高频都准；`a≈1` 走黑分支 | `m→1` 走黑分支，等价于"底图挖洞" | UI 由全分辨率 backbuffer 提供 ⇒ **几乎无损**；洞被 UI 盖住 |
| **半透明 UI**（玻璃面板、渐变） | 误差 = 2-bit alpha / 下采样；SDR 好得多 | 误差 = mask≠alpha + 2-bit；`m≥0.83` 变黑块 | 底图误差会**透出来**：黑边、色阶、闪烁 |
| **加法/发光 UI**（a≈0，P=U+H） | `a≈0` ⇒ `H≈P−U`，基本准确 | `m` 由 `|U|` 决定；`m<1` 时高频残留 ⇒ 轻微光晕 | 通常可接受；亮部可能过检 |
| **1 px 细线/小字** | 50% box 后 alpha 减半 ⇒ mask 可能漏 | 同左，且 mask 本身就是猜的 | 可能残留 1px 拖影（底图带线） |
| **HDR 暗部 UI** | PQ 阈值 + 2-bit | 同左 + mask 漏检 | 暗 UI 可能**不被去除**（拖影） |
| **HDR 亮部 UI** | 2-bit ⇒ 黑块 | 同左 | 边缘黑边/过曝洞 |
| **世界空间标记** | 作为 UI 参与重组 | 同左 | 会被"钉"在屏幕上而不是随场景移动（mode 4 的固有行为，需实测） |
| **UI 之后的后期**（景深/运动模糊） | `P−H` 含后期 ⇒ 被当 UI | 同左 | 该区域被写黑/挖洞 ⇒ 明显伪影；取决于引擎顺序 |

**一句话**：不透明 UI 已接近"构造上正确"；半透明/HDR/细线是主要短板，其中一半来自格式
（2-bit alpha），一半来自 50% 下采样和 PQ 空间。

---

## 5. SDR vs HDR：不是同一道题

|  | SDR（RGBA8 源） | HDR（R10 / FP16 源） |
|---|---|---|
| 传输格式 | RGBA8 | R10A2（FP16 先转 PQ） |
| alpha 位宽 | **8 bit** | **2 bit** |
| 有效不透明阈值 | ≈0.99 | **≈0.83** |
| 半透明误差 | 主要来自下采样 | 下采样 + 量化，明显更差 |
| 减法空间 | sRGB/R10 近似线性 | PQ 非线性，误差更大 |

所以"UI 合成验收不过"的现象在不同标题上不是同一个原因：SDR 标题更多是细线/阈值问题，
HDR 标题主要是 2-bit alpha + PQ。

---

## 6. 与"理想"的差距，以及改进的理论收益

理想输入 = 全分辨率、线性空间、alpha ≥ 8 bit 的真实 UI 平面。以此为零点：

> 在 §3.6 的"全分辨率 + 无损"口径下，表里的"下采样"项已经不存在；剩下的差距是
> **alpha 位宽 + 减法空间 + mask**。所以改进顺序也变了（见下）。

| 现状 | 相对理想多出来的误差 | 修掉它要做什么 | 理论收益 |
|---|---|---|---|
| GameUi + HDR | 2-bit alpha、PQ 减法（旧口径另加 50% 下采样） | 用 `kBufferTypeUIAlpha`(68) 或 RGBA8/独立 8-bit alpha；线性空间减法 | **最大**（消掉黑边/色阶） |
| GameUi + SDR | 阈值（旧口径另加 50% 下采样） | 8-bit alpha 已经是好的；膨胀 1–2 px | 中（AA 边缘） |
| Hudless synth（任何） | 上述 + mask 分类误差 | 用 68 的 alpha 走"直接重建"（`m=0→P, m=1→0, 0<m<1→H`），不再反解颜色 | 大（去掉 mask 猜 alpha） |
| 所有 | `saturate` 截断负值 | 保留有符号中间值，最后再 clamp | 小（底板发灰） |
| HDR + 2-bit | 模式选择 | 用 mode 5（UI + HUDless + backbuffer refine），厂商明确说对 2-bit 有帮助 | 中（2026-11-23 起已是默认） |

**"直接重建"比现在的 synth+unblend 更值得做**：它不需要从单帧 `(P,H)` 反解
`(RGB, alpha)`，只要一个 alpha；而 68 正好提供 alpha。公式：

```text
m = UIAlpha（真实 alpha，8 bit）
H' = P          (m ≈ 0)
H' = 0          (m ≈ 1)
H' = bilinear(H)  (0 < m < 1，可加 1–2 px 膨胀)
```

这在理论上把"半透明误差"从"反解不准 + 2-bit"降到"只剩 alpha 自身的量化"。
代价是两次 compute（mask 处理 + 重建），约 +0.5–1.0 ms（`UI_UNBLEND_MODE4.md` §11.3 的估算）。

---

## 7. 可验证的理论预期（验收指标）

| 场景 | 预期（当前实现） | 若修掉 2-bit / 下采样 |
|---|---|---|
| 静态准星/文字 | 不拖影；边缘可能有 1 px 暗/亮线 | 边缘干净 |
| 快速转视角 | 不透明 HUD 稳定；半透明边缘闪烁 | 半透明稳定 |
| 半透明菜单 | 4 级 alpha 色阶；`m≥0.83` 的黑块 | 连续 alpha |
| HDR 夜景暗 UI | 可能不被去除（阈值漏检） | 需要线性阈值 |
| 1 px 细线 | 可能残留拖影 | 75%/100% 或膨胀后消失 |
| 世界空间标记 | 可能"钉屏" | 同上（这是 mode 4 的语义，不是格式问题） |

这些判据比"看总 fps"更能定位问题：先用 `--ui-dump` 导出合成 UI/alpha（`085:374-380`），
看 mask 覆盖是否与 HUD 对齐，再判断是"格式"还是"阈值/下采样"。

---

## 8. 证据索引

| 内容 | 位置 |
|---|---|
| 生产端 2×2 / premultiplied / PQ | `lock_frame/producer/shaders/downsample_ui.hlsl:47-84` |
| synth 的 mask 与 `U'=p−(1−m)h` | `lock_frame/shaders/synth_ui.hlsl:49-70` |
| unblend 公式与黑分支 | `lock_frame/shaders/unblend_ui.hlsl:47-59` |
| 常量（0.99 / 8/255 / premul=1 / Dilate=0） | `lock_frame/modules/085_ui_unblend.inc:698-699`、`:509` |
| 传输格式映射（R10/FP16→R10A2，R8→RGBA8） | `lock_frame/producer/producer.cpp:625-639` |
| synth ring 用流格式（2-bit 也作用于本地 synth） | `lock_frame/modules/085_ui_unblend.inc:481-482` |
| NVNGX `UI_ALPHA` 未被消费 | `lock_frame/faker_fg/mgpu_fg_api.h:121`（全仓唯一引用） |
| 既有结论与限制 | `docs/design/UI_UNBLEND_MODE4.md:424-493`、`lock_frame/docs/UI_SOURCE_V29.md:68-80` |
| UIAlpha/ScalingOutputAlpha 的语义 | `third_party/streamline/include/sl_core_types.h:113,205-208` |

## 9. 结论

- 现有实现不是"随便猜一个 mask"：**低频由 H 精确提供、高频由 P 提供**，mask 只决定
  黑分支和放大倍数；不透明 UI 在理想格式下是构造正确的。
- 在"全分辨率 + 无损"口径下（§3.6），**50% 下采样那条误差已经消失**；真正限制观感的
  只剩 **HDR 的 2-bit alpha**（有效阈值 ~0.83）、PQ 空间的减法和阈值、以及 Hudless
  路径的 mask 估计。
- 最高性价比的顺序：**mode 5 已经是默认**（2026-11-23，UI + HUDless + backbuffer refine）
  → 下一步是用 68 的 8-bit alpha 做"直接重建"（消掉反解 + 2-bit）→ 最后才是线性空间减法。
  `--no-ui-mode5` 可随时退回 mode 4。
- 若这些都不做，那么在 HDR 标题上半透明 UI 的黑边/色阶是**理论上不可避免**的，
  不是调参能解决的。

