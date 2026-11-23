# DLSSG 车道的 HUDless / UI：它给什么，走无损链路要多少（2026-11-22）

> 口径：`FgInput=DLSSG`（我们复用的那条输入车道）、4K、链路 **13 GB/s**（PCIe 4.0 x8
> 实测，`LOSSLESS_TRANSPORT.md` §7）、真实帧 40 fps、共享环 **32 槽**（`kMaxSlots`）。
>
> 结论一句话：**UI 走现有半分辨率纹理槽几乎免费（名义 0.64 ms/帧、环 265 MB）；
> HUDless 才是贵的（R10 22–28 MB/帧、1.7–2.2 ms、环 1.1 GB；FP16 再翻倍）。**
> 所以"传 UI、不传 HUDless"的 B 方案在成本上是对的；无损码流对 UI 只在"必须全分辨率"
> 时才值得开，对 HUDless 只能省 20–46%，省不出数量级。

---

## 0. 先看结论表

| 方案 | 编码后/帧 | 链路名义 | @40fps | 环（32 槽） | 生产端 GPU | 消费端本地 GPU |
|---|---:|---:|---:|---:|---:|---:|
| **UI 50% 纹理（今天默认）** | **8.29 MB** | **0.64 ms** | 0.33 GB/s | 265 MB | ≈0.1–0.3 ms（4-tap） | 0.3–0.8 ms（unblend） |
| UI 50% + 码流 | 0.07–1.01 MB | 0.005–0.08 ms | ≈0.00–0.04 GB/s | 274 MB | +0.26 ms | +0.15–0.18（解码）+0.3–0.8 |
| UI 100% 纹理 | 33.18 MB | 2.55 ms | 1.33 GB/s | 1.06 GB | 0（无需降采样） | 0.3–0.8 ms（unblend） |
| UI 100% + 码流 | 0.26–3.00 MB | 0.02–0.23 ms | 0.01–0.12 GB/s | 1.09 GB | +1.02 ms | +0.58–0.72（解码）+0.3–0.8 |
| HUDless R10 100% 原样 | 33.18 MB | 2.55 ms | 1.33 GB/s | 1.06 GB | 0（游戏直接给） | **0**（直接 tag） |
| **HUDless R10 100% + 码流** | **22.7–27.6 MB** | **1.75–2.12 ms** | **0.91–1.11 GB/s** | 1.10 GB | +1.02 ms | 0.58–0.72（解码） |
| HUDless FP16 100% + 码流 | 45.5–55.7 MB | 3.50–4.28 ms | 1.82–2.23 GB/s | 2.19 GB | +≈2.0 ms | 1.2–1.4（解码） |
| *参考：今天 MV+depth（100%+码流）* | *26.3–29.0 MB* | *2.02–2.23 ms* | *1.06–1.21 GB/s* | *≈1.53 GB（实测 reserve 1526 MB）* | *2×1.02 ms（实测内核）* | *2×0.58–0.72 ms（实测内核）* |

> 注：`LOSSLESS_TRANSPORT.md` §15.5 给过"编码 0.13 ms / 解码 0.1–0.15 ms"的**目标/下界**
> （纯流式打包），而实际 v2.2 内核（直方图 + mask + patch）在 3080 Ti 上实测是
> **1.02 ms encode / 0.58–0.72 ms decode（4K、33 MB 一条流）**。本文一律用后者，
> 因为前者不是这版内核的数字。
>
> 列的含义：**链路**=跨卡读写（消费端"拷贝"纹理槽就是这次读，不另计本地时间）；
> **消费端本地 GPU**=解码/升采样/unblend 等落在副卡上的活。

读法：

1. **UI 的字节量根本不是瓶颈**——半分辨率 8.29 MB 只占今天 MV+depth 链路的 1/3，
   占 25 ms 帧预算的 2.5%；40 fps 下 0.33 GB/s，离 13 GB/s 很远。
2. **HUDless 是 10–20 倍**：同样 4K，HUDless 是"一整帧场景"，压缩率只有 1.2–1.5；
   UI 是"大面积透明 + 稀疏文字"，实测 11–128x。
3. **码流不缩环**：槽按最坏情况（`tiles × 264 words`）预留（`producer.cpp:365-369`），
   所以给 UI 上码流，共享内存一分不省（265 → 274 MB；1.06 → 1.09 GB）。

---

## 1. DLSSG 车道到底给什么

### 1.1 两个 tag

| 平面 | Streamline tag | 我们落到 | 格式/尺寸 | 备注 |
|---|---|---|---|---|
| HUDless | `sl::kBufferTypeHUDLessColor` | `MGPU_FG_STREAM_HUDLESS` | 与 backbuffer 同尺寸；R10 PQ 或 FP16 scRGB | 我们在 tag 时**拷贝成稳定快照**（`proxy_streamline.cpp:158-180,1097-1112`），否则 present 时可能已被复用成黑图 |
| UI | `sl::kBufferTypeUIColorAndAlpha` | `MGPU_FG_STREAM_UI_COLOR` | 单张 RGBA（premultiplied，alpha 承载覆盖度） | Streamline 车道是 **combined**；NVNGX 车道才分成 `_UI` + `_UIAlpha`（`provider.cpp:118-119`） |

### 1.2 可用性：取决于"游戏自己的 FG 在不在 evaluate"

这是关键前提，且有实测证据：

| 标题 | 游戏 FG 关 | 游戏 FG 开 | 证据 |
|---|---|---|---|
| PRAGMATA（RE Engine） | `hudless=n ui=n` | **`hudless=y ui=y`** | `FG_INPUT_V13_PRAGMATA.md:176-182,281-285` |
| Khazan | — | **`HUDLESS_COLOR` 每帧都有，`UI_COLOR_AND_ALPHA` 没有** | `FG_INPUT_V13_KHAZAN.md:215,273` |
| Cyberpunk 2077（我们抓的） | `hudless=n ui=n` | 需游戏 FG 真正 evaluate；sm86 的 4K 抓取里**只交 HUDLess** | `FG_INPUT_V13_2077.md:298,317`、`UI_UNBLEND_MODE4.md:367` |

所以"以 DLSSG 管线为标准"的准确含义是：

> **HUDless 比 UI 常见；UI 只在部分标题上出现，而且两者都要求游戏 FG 处于激活状态。**

这既解释了为什么 B 方案（只传 UI）覆盖面有限，也说明**不需要 hudfix 就已经有一条可用的
HUDless 源**（PRAGMATA / Khazan / 2077 的部分配置）——前提是愿意付 §0 里 HUDless 那一行的钱。

### 1.3 逐帧可用性

- HUDless 是**快照**（tag 时刻拷贝），present 时仍然有效；UI 是**活资源**，只能在本帧内用。
- 菜单/加载/场景切换/分辨率变化时可能连续缺帧；现有门控要求"连续 miss N 帧才关"
  （`UI_UNBLEND_MODE4.md:215-218`），这条对 HUDless 同样适用。

---

## 2. 成本口径（避免把名义值当实测）

- **链路**：13 GB/s（`--link` 默认，PCIe 4.0 x8 实测）。`bytes / 13 GB/s` 是**名义**时间；
  实测因为与游戏剩余工作重叠，边际只有名义的 ~30%（66.4 MB：名义 5.11 ms，实测 ≈1.5 ms，
  `LOSSLESS_TRANSPORT.md:53-55`）。本文同时给名义值，保守。
- **环容量**：`slot_size × 32`，`slot_size` 对纹理是分配尺寸，对码流是
  `tiles × 264 × 4`（最坏情况）。两者都不能按"平均压缩后大小"算。
- **编解码毫秒**：`fr_codec` 生产内核实测 **encode 1.02 ms / decode 0.58–0.72 ms**
  （4K、33 MB 一条流，RTX 3080 Ti；`lock_frame/shaders/fr_codec.hlsl:24-26`）。
  半分辨率按像素数线性折到 1/4（≈0.26 / 0.15–0.18 ms），是估算。
- **其他实测**：66 MB 纯拷贝 0.188 ms、4K→150% 9-tap 降采样 0.307 ms（`§3`）；
  我们的 `downsample_ui.hlsl` 是 2×2 四抽头，按 4K 估 0.1–0.3 ms。

---

## 3. UI 的压缩比（用生产内核实测）

方法：`tests/lossless_bench/ui_ratio_test.cpp` —— 合成 4K R10G10B10A2（按 u32/像素、
`NW=1`）喂给**与生产同一个** tile 内核（`tests/lossless_bench/fr_codec.h`，与
`lock_frame/shaders/fr_codec.hlsl` 逐字节一致），测 6 种 HUD 密度。

| 内容 | 1080p 原始 | 编码后 | 比率 | 4K 原始 | 4K 编码后 | 比率 |
|---|---:|---:|---:|---:|---:|---:|
| 空（无 HUD） | 8.29 MB | 0.065 MB | 128x | 33.18 MB | **0.259 MB** | 128x |
| 稀疏标记 1% | 8.29 | 0.129 | 62x | 33.18 | **0.534** | 62x |
| 文字 HUD 3% | 8.29 | 0.270 | 31x | 33.18 | **1.046** | 32x |
| 密集 HUD 10% | 8.29 | 0.700 | 12x | 33.18 | **2.819** | 12x |
| 半透明菜单 60% | 8.29 | 0.775 | 11x | 33.18 | **2.383** | 14x |
| 全屏面板 100% | 8.29 | 1.012 | 8.2x | 33.18 | **2.996** | 11x |

结论：

- **典型 HUD（1–10% 覆盖）：4K 下 0.5–2.8 MB/帧**，比 8.29 MB 的半分辨率原样还小一个量级；
- **最坏情况（整屏半透明面板）≈3 MB**，仍然只有 33 MB 的 1/11；
- 这是**合成数据**，真实 UI 还可能有视频、动态模糊、渐变，比率会更差，但不会差到 1–2x 的
  量级（那是"整帧彩色场景"的特征）。

---

## 4. HUDless 的压缩比（没有真帧，用 colour 的实测类比）

HUDless 是"去掉 UI 的整帧场景"，与 `colour` 流是同一类内容（已色调映射、有 TAA/dither/
颗粒噪声）。`LOSSLESS_TRANSPORT.md:502-517` 的 32 帧 4K 实测：

| 流 | 原始 | 现状（每 tile max 位宽） | 例外编码 | 用途 |
|---|---:|---:|---:|---|
| colour 4K FP16 | 66.4 MB | 55.7 MB（**1.20x**） | 45.5 MB（**1.46x**） | 实测 |
| **HUDless R10 4K（推算）** | 33.18 MB | **27.6 MB（1.20x）** | **22.7 MB（1.46x）** | 按同一比率折算 |
| **HUDless FP16 4K（推算）** | 66.37 MB | **55.3 MB** | **45.5 MB** | 同上 |

必须标注：**HUDless 这两行是折算，不是实测**。要变成实测只差一次抓取：现有 `--dump`
支持 RGBA16F / R32（`tests/lossless_bench/fr_codec_test.cpp:27-34`），R10A2 需要补一个
`bytes_per_px=4` 的分支（格式 24 的映射）。

另一个必须先定的变量是**源格式**：DLSSG 车道的平面常见 `RGBA16F`（CP2077 的 UI 日志是
`fmt 10`，UI 在传输侧才由 `ui_transport_format()` 降到 R10，`producer.cpp:625-639`）。
所以：

- **原样传 HUDless**：按 FP16 行算（45.5–55.7 MB/帧、3.5–4.3 ms、2.19 GB 环）；
- **先转 R10 再传**：多一次转换 pass（读 66.4 + 写 33.2 ≈ 0.19 ms，`LOSSLESS_TRANSPORT.md`
  §5.4 的实测同量级），之后按 R10 行算（22.7–27.6 MB、1.75–2.12 ms、1.10 GB 环）。
  这与 UI 走的是同一条"scRGB→PQ"的路（`downsample_ui.hlsl` 已经在做），只是 HUDless 不需要降采样。

**结论：HUDless 若要传，先转 R10 再编码**——用 0.19 ms 换掉一半字节和一半环，比任何压缩器都便宜。

---

## 5. 三个具体配置的账

### 5.1 今天的 UI 流（推荐保留）

```text
4K：UI 源 3840x2160 R10A2 → downsample_ui（2x2）→ 1920x1080 8.29 MB/帧 → 纹理槽
```

- 链路名义 **0.64 ms**，40 fps 下 **0.33 GB/s**（13 GB/s 的 2.5%）；
- 生产端 4-tap pass ≈0.1–0.3 ms；消费端拷贝 0.02 ms + unblend 0.3–0.8 ms；
- 环 265 MB。

**这条流没有压缩的必要**：省下的 8 MB 值 0.6 ms 名义链路，但编码要 +0.26 ms 渲染卡、
解码要 +0.15 ms 副卡，且环不变（265 → 274 MB）。按 `§2` 的时间不等式是**不划算**的。

### 5.2 全分辨率 UI（只在 50% 会丢 1–2 px 细线时）

```text
4K：UI 源 → 直接写 3840x2160 槽（33.18 MB/帧）
```

- 原样：链路 **2.55 ms**、1.33 GB/s、环 **1.06 GB**；
- 加码流：编码后 **0.26–3.0 MB**、链路 **0.02–0.23 ms**、**0.01–0.12 GB/s**，
  但环仍是 **1.09 GB**，多加 encode **+1.02 ms** / decode **+0.58–0.72 ms**。

**这是码流对 UI 唯一真正划算的场景**：用 ~1.7 ms 的 GPU 时间换掉 2.3 ms 的链路，
并且把 1.33 GB/s 降到 ~0.1 GB/s（PCIe 3.0 x8 上这很重要）。

工程注意：R10A2 资源**不能直接建 `R32_UINT` UAV**（UAV 格式必须与资源类型兼容）。
解码目标要先建 `R32_TYPELESS`，UAV 用 `R32_UINT`，SRV/RTV 用 `R10G10B10A2`——本地资源可以，
跨卡共享纹理不行（共享资源要求具体格式）。所以"UI 码流"解码到的是**本地 typeless 纹理**，
不是跨卡槽本身。

### 5.3 HUDless（PRAGMATA / Khazan / 部分 2077 配置）

- R10 + 码流：**22.7–27.6 MB/帧、1.75–2.12 ms 名义、0.91–1.11 GB/s、环 1.10 GB**，
  再加 decode 0.58–0.72 ms；
- FP16 + 码流：**45.5–55.7 MB/帧、3.5–4.3 ms、1.8–2.2 GB/s、环 2.19 GB**；
- 好处是消费端可以**直接 tag HUDless（mode 4）**，省掉 unblend 的 0.3–0.8 ms。

对比 §5.1：HUDless 的链路是 UI 的 **27–43 倍**（R10）/ **55–87 倍**（FP16）。
即使省掉 unblend，也不足以抵消——这正是 `UI_UNBLEND_MODE4.md:33-36` 选 B 的量化依据。

---

## 6. 消费端预算：为什么解码位置很重要

`LOSSLESS_TRANSPORT.md` §7 的结论是 4K/3x 消费端**没有余量**（20.6 + 8.33 > 25.0 ms）。
把它套到本文：

| 消费端动作 | 4K 半分辨率 UI（纹理） | 4K 全分辨率 UI 码流 | 4K HUDless 码流 |
|---|---:|---:|---:|
| 跨卡读取（已计入链路） | 0.64 ms | 0.02–0.23 ms | 1.75–2.12 ms |
| 本地解码 | — | 0.58–0.72 ms | 0.58–0.72（R10）/ 1.2–1.4（FP16） |
| unblend | 0.3–0.8 ms | 0.3–0.8 ms | **0**（直接 tag） |
| **本地合计** | **0.3–0.8 ms** | **0.88–1.52 ms** | **0.58–0.72 / 1.2–1.4 ms** |

在 3x 已经没有余量的前提下，**HUDless 的解码会直接挤掉 XeFG 的帧**；UI 半分辨率的
0.3–0.8 ms 已在噪声边缘；UI 全分辨率码流的 0.9–1.5 ms 需要实测确认能不能藏进 `fwd`
fence 的空窗（`LOSSLESS_TRANSPORT.md` §5.2 说"唯一能藏的位置已经在 gpu_throttle
的 20.6 ms 里"）。

---

## 7. 与 hudfix 方案的关系（上一份评估的补充）

`UI_HUDFIX_RENDER_CARD.md` 说"hudfix 只产出 HUDless，UI 要自己在渲染卡上差分"。
现在可以把那句话量化成钱：

- 如果直接把 hudfix 的 HUDless 传过来：就是本文 §5.3 的 **22.7–27.6 MB/帧**（R10），
  外加渲染卡的捕获/差分成本；
- 如果在渲染卡先做 `P − H → UI` 再传：链路变成 §5.1 的 **8.29 MB（半分辨率）** 或
  §5.2 的 **0.26–3.0 MB（全分辨率 + 码流）**，省下 2–4 ms 链路和 ~1 GB 环；
- 所以"**先在渲染卡把 UI 做出来再传**"这个方向，在传输账上是**压倒性正确**的
  （10–40 倍字节差），代价只是渲染卡多一次差分和 present 侧发布点（见上一份文档 §4）。

---

## 8. 建议

1. **保持 B 方案：传 UI，不传 HUDless。** 半分辨率纹理槽的账最干净（0.33 GB/s、265 MB、
   消费端 0.3–0.8 ms）。
2. **不要给半分辨率 UI 上码流**：环不省、两个 GPU 各加 0.15–0.26 ms、只省 0.6 ms 名义链路，
   按时间不等式不划算。
3. **只有"必须全分辨率 UI"时才开码流**：1–2.6 MB/帧、环 1.1 GB、解码 0.6–0.7 ms；
   需要 `R32_TYPELESS` 本地解码目标。
4. **HUDless 作为兜底源**（没有 UI 平面时）：**先 scRGB→R10（≈0.19 ms）再编码**，落在
   R10 行（22.7–27.6 MB、1.10 GB 环）；不要直接传 FP16（45.5–55.7 MB、2.19 GB 环）。
5. **补两格实测**：① 真机 UI dump 走 `fr_codec`（把 `--ui-dump` 加一个 raw R10 输出即可）；
   ② HUDless 的实测比率（在 `fr_codec_test` 的格式表里加 R10G10B10A2 分支）。

---

## 9. 复现与证据

| 内容 | 位置 |
|---|---|
| UI 压缩比（本文 §3） | `tests/lossless_bench/ui_ratio_test.cpp`（新增） |
| 码流内核 | `tests/lossless_bench/fr_codec.h`、`lock_frame/shaders/fr_codec.hlsl` |
| 实测颜色/压缩比 | `docs/design/LOSSLESS_TRANSPORT.md:502-517`（§12.1） |
| 链路模型与名义/实测折扣 | `LOSSLESS_TRANSPORT.md:29-57`（§1）、`:261-283`（§7） |
| 环容量与槽尺寸 | `lock_frame/producer/producer.cpp:365-369,477-503`、`:1326-1368` |
| DLSSG 平面 | `lock_frame/faker_fg/proxy_streamline.cpp:158-180,317,1097-1112`、`provider.cpp:118-119` |
| 逐游戏平面证据 | `docs/findings/FG_INPUT_V13_{PRAGMATA,KHAZAN,2077}.md` |
| B 方案的原始取舍 | `docs/design/UI_UNBLEND_MODE4.md:24-36,226-241` |
| hudfix 评估 | `docs/design/UI_HUDFIX_RENDER_CARD.md` |

## 10. 未知与风险

- §3 是**合成 UI**：真实 UI 若有全屏视频/动态模糊，比率会向 11x 靠拢（仍然远好于 HUDless）；
- §4 的 HUDless 两行是**折算**，不是实测；真帧可能在 1.2–1.5x 之外（R10 的 10 bit 打包
  可能比 FP16 略好或略差）；
- 13 GB/s 是 PCIe 4.0 **x8** 实测；x16 会更好、x4 会更差，结论的方向不变但绝对时间会变；
- 2-bit alpha 的量化问题与传输无关（是 UI 流的格式选择问题，`UI_UNBLEND_MODE4.md` §11.3）；
- 所有消费端毫秒数都假设解码能与 `fwd` fence 的空窗重叠；4K/3x 是否真的装得下要实测。
