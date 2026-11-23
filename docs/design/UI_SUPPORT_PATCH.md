# UI 支撑补丁：只传「被 UI 遮挡的画面」，XeFG mode 4-only（协议 v34）

> 状态（1.3.0+）：**默认开启，固定 mode 4。** `fakerFG.ini` 默认
> `UiStream=0 / UiSource=Off / UiPatch=1 / HudlessStream=1`；stream 3 / mode 5 封死。
> `UiPatch=0` 可回退旧路径。
>
> **v34 流语义（重要）**：
>
> | 流 | 内容 | 用途 |
> |---|---|---|
> | stream 3 `UiStream` | **关闭**：不再发布游戏 UI 平面 | 不参与合成，不 tag |
> | stream 4 `HudlessStream` | **支撑域 H patch**（R10 PQ + sparse 32×8 tile 掩码） | consumer 还原 H'，只 tag H' → mode 4 |
>
> 唯一路线是 `H' = present（支撑外）/ patch（支撑内）`，XeFG 始终走 mode 4。
> mode 5 的 UI 编码/解码成本在渲染卡上收益不高，已从默认链路移除。
>
> 基线（2026-11-24）：`UiPatch=1` + 消费端 **33³ 3D LUT**（1D 32 档 LUT 保留为兜底）。
> 历史 1D 实测（CP2077 4K DLAA / 3x / HDR10，`alpha=0` 干净像素，n=7683）：
>
> | 指标 | 结果 |
> |---|---|
> | hudless − backbuffer 亮度 | 均值 **+0.003**、中位 **+0.022** |
> | RGB MAE | **0.037 / 0.032 / 0.041**（约 8–10/255） |
> | 色度比（hudless / backbuffer） | **0.894×** |
> | LUT 置信度 | **0.966**（`LUT16 R=0.354 G=0.282 B=0.156`） |
>
> 1.3.0 最终构建把消费端颜色校正升级为 **33³ 3D LUT**：在 `alpha=0` 干净像素上直接取
> H（scene R10 code）与 P（WGC R10 code）配对，建立 code→code 的交叉通道映射；3D LUT
> 每帧在 GPU 上重建，无回读，空单元用邻域/恒等填充。1D 32 档 LUT 仅作为 3D 初始化
> 失败时的兜底。最终 CP2077 / PBZ / Wuchang 实机通过：UI 稳定无拖影；模型边缘剩余
> 伪影来自 XeFG 模型本身，不在 UI 合成链路。
>
> 已知残差：3D LUT 只能表达全局颜色映射；空间相关的曝光/tonemap 差异、UI 区域本身
> 没有 ground truth，仍是可见残差的来源。display-referred HUDless（如
> `ScalingOutputColor`）在 CP2077 仍不可用。
>
> 状态（1.3.0+）：**默认开启**；`UiPatch=0` 可完全回退。
> 性能审查与优化建议见 `docs/design/UI_PATCH_PERF.md`。
> 代码：`lock_frame/producer/shaders/synth_ui_full.hlsl`（`Patch` 分支）、
> `lock_frame/producer/producer.cpp`（`UiPatch` / `kHudlessFlagPatchPayload`）、
> `lock_frame/shaders/unblend_ui.hlsl`（patch + 3D LUT 采样）、
> `lock_frame/shaders/patch_lut3d.hlsl` / `patch_lut3d_fill.hlsl`（H→P 3D LUT）、
> `lock_frame/modules/085_ui_unblend.inc`、`140_frame.inc`。
>
> 结论先行：**HUDless（H）本身就是「被 UI 遮挡的画面」**，不需要任何 matting 或反卷积。
> 所以消费端不需要 `H' = (P−U)/(1−a)` 这个病态反演：把 UI 支撑域内的 H 稀疏地传过去，
> 消费端做一次 copy+overwrite 就能得到**逐像素精确**的 HUDless，然后只 tag H'，
> XeFG AUTO 自然选 `BACKBUFFER_HUDLESS`（mode 4）。

---

## 0. 为什么是这条路线

之前的两条 UI 合成路线各有硬伤：

| 路线 | 输入 | 问题 |
|---|---|---|
| mode 4 + unblend | 传 UI `(U,a)`，消费端 `(P−U)/(1−a)` | `a→1` 时放大 U 量化误差与 `P_t/P_c` 差异；HDR 只有 2-bit alpha |
| mode 5 + UI 平面 | 传派生 UI，H' 仍靠除法重建 | 半透明/HDR 边缘黑边；派生 UI 可能把场景冻进 overlay |
| 本方案 | **只传 UI 支撑域内的 H 切片** | 没有 alpha、没有除法；H' 是拷贝而不是反演 |

关键观察：

```text
支撑外：P 里没有 UI  ⇒  H == P（只差舍入）  ⇒  H' = P
支撑内：H 就是被 UI 遮挡的场景  ⇒  H' = H
```

因此 `H'` 的正确性只依赖一件事：**支撑掩码不能漏**。多覆盖完全无害——多传的像素
就是 H 本身，而 H 在任何位置都是合法的场景；漏覆盖才会让 UI 残留在 H' 里，插值帧
出现 1px 拖影。

---

## 1. 数据流

```text
游戏（渲染卡，DLSSG Evaluate 车道）
  P = DLSSG.Backbuffer（含 UI）
  H = HUDless（同帧、去掉 UI）
        │
        ▼
producer（同一次 synth dispatch）
  支撑 s(p) = 游戏 UI alpha > 0           （tag 67/68 可用时）
            | linear |P−H| / max(P) > ε  （否则；ε 默认 8/255）
  GroupMask[8x8] = OR s(p)
  patch[p] = PQ(H[p])                      ← 全帧写；没支撑的 group 由 codec 丢弃
        │  sparse codec（UI_SPARSE：四个 8x8 group 全空 ⇒ 32x8 tile 写 mode=3 头）
        ▼
stream 4（HudlessStream，kHudlessFlagPatchPayload，R10 PQ，full-res）
        │
        ▼
consumer（副卡）
  H' = present                      tile 头 mode=3（无支撑）
     = patch                        tile 有支撑（α=1；外侧 7×7 羽化）
  只 tag HUDless(H')                → XeFG mode 4
  SetUiCompositionState(ENABLED)
```

不变式：

1. patch 只出现在 stream 4，`kHudlessFlagPatchPayload`；stream 3 默认关闭。
2. 消费端按 **逐像素 support alpha** 使用 patch：`alpha=0` 的干净像素直接用 present，
   只有 `alpha>0` 的支撑像素才换成 patch；避免整 32×8 tile 替换导致 HUD 周围色调差。
3. **只 tag H'（mode 4）**；patch 永远不 tag 成 `RES_UI`，stream 3 / mode 5 封死。
4. 消费端必须有 sparse 头（`streams[4].codec != 0`）才能拿到占位；拿不到就本帧不 tag。
5. H' 与 backbuffer 同尺寸/同格式；当前只支持 **R10 PQ（HDR10/BT.2100）**，
   SDR 的 RGBA8 sRGB 编码路径尚未实现（`select_ui_source` 会直接拒绝）。
6. patch 与 `P_c` 同帧（复用现有 `pair_ok` / freshness）。
7. 默认 `UiPatch=1`（1.3.0+）；`UiPatch=0` 可回退。开启时强制
   `UiFullRes=1 / UiSparseCodec=1 / UiAlpha8=0 / UiCodec=1 / HudlessStream=1`。
   消费端由 patch flag 自动打开 composition 路径（`--fg-ui-none` 仍可显式关闭）。

---

## 2. 生产端

### 2.1 支撑掩码

- **游戏给 UI alpha 时**：支撑 = `alpha > 0.002` **或** 差分超阈值。alpha 覆盖普通
  半透明 UI，差分补上 alpha=0 的加法/发光 UI（否则这部分会留在 H' 里）。
- **没有 alpha 时**：在 `UiLinear=1` 的线性 nits 里算相对差
  `d = max_c|P_c−H_c| / max_c P_c`，`d > max(Threshold, 0.002)` 即支撑。
  这是「宁多勿少」的集合，不是物理 alpha；阈值 8/255 是经验值，低于帧残差
  （实测 CP2077 约 0.012 的原始码值差）会把整帧标成支撑。
- 每 8×8 group 用 `InterlockedOr` 汇总命中；codec 再把一个 32×8 tile 的四个 group
  取 OR，任一命中就编码整个 tile。

### 2.2 全帧填 H，不做「只写支撑」

`fr_encode` 的空 tile 判断是 **tile 级**（四个 group 全空才写 mode=3），而分区写只到
8×8 group。如果只给命中的 group 写 H，同一 tile 里没命中的 group 会留下上一帧的旧
数据，被一起编码进 patch。因此 patch 分支对**每一像素**都写 `PQ(H)`；没支撑的 tile
由 codec 丢弃，字节不会增加，但任何被传出去的 tile 都含有效场景。

### 2.3 传输格式与白点

- `UiPatch=1` 强制 `UiAlpha8=0`：patch 是 PQ 场景，不能落到 8-bit RGBA 的 UI 通道；
  传输格式必须是 R10（SDR 的 RGBA8 路径未实现，producer 会拒绝并保持 stream 4 off）；
- 源是 FP16 scRGB 时在 shader 里 `scRGB → BT.2020 PQ`；
- 源已是 R10 PQ 时原样写。

**白点与动态增益（2026-11-24 实测 + 实现）**：消费端的 present 是 display-referred
（WGC→PQ），而 CP2077 的 `DLSSG.Backbuffer` / HUDless 是 **scene-referred**，其 scRGB
1.0 约等于 **20 nits**，不是规范默认的 80。用 80 编码会让 patch 比周围 present 亮约
1.6 倍，视觉上就是“补偿区域发白”。

现在传输层固定用 80 编码，**消费端逐帧自动测增益**：

```text
alpha = producer 的逐像素 UI mask（1 = UI，0 = 干净场景）
在占用 tile 内、alpha=0 的像素上：
    r = pq_decode(present) / pq_decode(patch)      // nits
    hist[ch][64] += 1                              // log2(r) 直方图
    gain[ch] = 2^(模数 bin 的加权均值)             // clamp 0.02..4
patch' = pq_encode(pq_decode(patch) * gain)
```

- **默认 3D LUT（H code → P code）**：在 `alpha=0` 干净像素上，以 patch（H）的 R10
  三通道 code 为输入、present（WGC P）的 R10 三通道 code 为目标，建 **33³** 直方图；
  每格取 P 的平均 code。空单元从 3×3×3 邻域按距离加权填充；没有任何样本时用恒等
  （输入 code 原样输出），不会发明颜色。compose 用 8 个角点做四面体插值，
  sampler clamp），因此同时表达 tone 和跨通道 gamut 差异。
- 1D 32 档 LUT 仍保留：`patch_gain` 在 3D 初始化失败时作为兜底；两套都由同一批干净
  像素驱动，GPU 内完成，无 CPU 回读。
- CP2077 的 1D 收敛值约 `gain=0.25`（即 20 nits）；3D LUT 不再需要这个全局增益常数。
- 全屏 UI 没有干净像素时，3D 直方图全空 → 恒等；compose 会回退为 patch 原编码。
  `UiPatchWhiteNits` 只是传输提示，不再需要按机器调。
- 实现：`patch_lut3d.hlsl`（直方图）+ `patch_lut3d_fill.hlsl`（填 LUT）+
  `unblend_ui.hlsl`（采样），消费端同一命令列表内完成。

---

## 3. 消费端

### 3.1 unblend 变成 support-alpha copy + 3D LUT

`unblend_ui.hlsl` 的 patch 分支现在由 `support alpha` 驱动，而不是整 tile：

```hlsl
present = Present.Load(p);
u       = Ui.Load(p);              // patch(H)，全分辨率
blend   = saturate(u.a);
if (blend < 0.5) {                 // UI 外侧 7x7 覆盖率羽化
    float cov = 0;
    for (oy=-3..3) for (ox=-3..3) cov += Ui.Load(p+[ox,oy]).a;
    blend = smoothstep(0.02, 0.35, cov/49.0);
}
if (blend <= 0.001) { H' = present; return; }
if (Use3D) {                       // 33^3 H(scene code) -> P(WGC code)
    float3 pc = Lut.SampleLevel(Smp, saturate(u.rgb), 0).rgb;
    H' = lerp(present, pc, blend);
    return;
}
```

- 这里用 `Load` 而不是 `SampleLevel`：patch 是全分辨率，必须 1:1；
- **按逐像素 support alpha 使用 patch**：`alpha=0` 的干净像素直接用 present
  （它在支撑外/干净区域已经正确），只有 `alpha>0` 的 UI 支撑像素才用 patch；
  否则整个 32×8 tile 都会被 patch H 替换，颜色残差会在 HUD 周围显形；
- **默认 3D LUT**：`Use3D=1` 时 compose 用 8 个角点做四面体插值，把 patch 的 H code
  映射成 WGC P code；3D LUT 由同一帧干净像素构建，不读回 CPU；
- **兜底**：3D 初始化失败时 `Use3D=0`，退回 1D 32 档 LUT；
- **当前默认回到 scene-referred HUDLess patch**：`HudlessSource=Auto` 使用 HUDLess
  快照；`ScalingOutputColor` 在 CP2077 仍出现绿色块/未完成区域，只保留为显式实验。
- **support 边缘覆盖**：consumer 在 UI 外侧用 7×7 support 覆盖率向外羽化（UI 内部 alpha=1 保持 patch）；
  patch_calib/patch_lut3d 的干净样本排除 support 5×5 邻域，避免把 UI 亮边当成色调参考
  （否则移动时会出现细白轮廓）。
- **局部 tone field 已停用**：`patch_local`/`patch_local_fill` 仍保留代码但默认不运行；
  局部场会产生 UI 边缘轮廓，空间相关残差留待后续处理。
- 没有 `(1−a)`、没有 PQ 解码/编码、没有 `OpaqueAlpha` 黑分支；
- 空 tile 的 `Occ` 来自 `ui_occ.hlsl`（读取 codec 头的 mode 位），不新增协议字段。

### 3.2 tag 规则

`140_frame.inc`：

- `ui_patch = streams[4].ui_flags & kHudlessFlagPatchPayload`；
- patch 不是 UI 平面，永远不 tag 成 `RES_UI`；
- **只 tag `H'`（`RES_HUDLESS_COLOR`）→ AUTO 选 mode 4**；
- `mode5` 条件显式带 `!ui_patch`；即使有旧 `UiStream=1` 的 UI 平面，patch 模式也
  不会 tag UI / 进入 mode 5；
- 组合状态在 `040_cadence.inc` 由 stream 4 patch 打开。

---

## 4. 成本（4K、CP2077 实测约 8% 覆盖）

| 项 | 量级 | 说明 |
|---|---:|---|
| patch 原始 | ~2.2 MB | 8% × 4K × 4 B；tile over-cover 后 2–4 MB |
| patch 编码后 | **约 2.2 MB/帧** | sparse 32×8 tile；空 tile 只写 mode=3 头 |
| 对比全帧 H | 22.7–27.6 MB | v32 `HudlessStream` |
| 渲染卡 | 全帧 P/H 读 + 全帧 patch 写 + 支撑 tile 编码 | 仍在 synth dispatch 内，P1 可 tile-gated |
| 消费端 | 1 次 copy + 3D LUT + overwrite（≈0.1–0.2 ms）+ 解码 | 比 unblend 的 PQ 双 pow + 除法便宜 |
| stream 3 / mode 5 | **0** | 不编码、不解码、不 tag；只保留协议最小值 `UiSlots=2` |

支撑集中（字幕、准星、血条）时 tile 命中率远低于面积比；全屏半透明菜单会退化到接近
全帧 H，此时应自动退回 `UiPatch=0` 或 `HudlessStream=0`，不能硬传。

---

## 5. 失败模式与闸门

| 现象 | 原因 | 处理 |
|---|---|---|
| 插值帧 UI 边缘 1px 拖影 | 支撑漏检（阈值过高 / 细线） | 降 `Threshold`、`UiDilate=1`、或用游戏 alpha |
| H' 在 UI 下是黑/旧画面 | 引擎在不透明 UI 下跳过了场景渲染，H 无效 | `--dump` 检查 patch；该 title 退回 `UiPatch=0` |
| 字节暴涨 | 支撑≈全屏 | 支撑覆盖率门（例如 >35% 本帧不发布 patch） |
| 没有画面变化但 patch 很大 | P/H 帧残差被当成 UI | `UiLinear=1` + 提高 `Threshold` |
| 完全不出帧 | composition 未打开 | 检查 `SetUiCompositionState(ENABLED)`；patch 模式固定 mode 4，忽略 mode5 开关 |
| 旧 consumer 收到 patch | 协议不同步 | v33 直接拒绝混合二进制 |

---

## 6. 验收

1. **编译**：`./lock_frame/build.sh producer` + `./lock_frame/build.sh consumer`。
2. **日志**：
   - producer `UiPatch=1 - stream 4 carries the sparse HUDless patch...`
   - producer `UI source = HUDless patch under UI (mode 4...)`
   - consumer `ui-unblend: HUDless rebuilt from ... HUDless patch (source=patch)`
   - consumer `SetUiCompositionState(ENABLED)` + mode 4 不报 `-12`/`-14`
3. **图像**：`--tag-dump` 的 HUDless 图在 UI 内部应与游戏 H 逐像素一致（残差 < 1 码阶）；
   UI 支撑外应等于 backbuffer。
4. **A/B**：同一场景对比 `UiPatch=0`（关闭）与 `UiPatch=1`（mode 4）：
   - 半透明菜单 / 字幕底板 / 玻璃面板 / 1px 文字；
   - `fr: coded ui` 字节、链路 ms、GPU 占用；
   - 插值帧边缘是否还有除法环/黑边。
5. **回退**：`UiPatch=0` 即完全回到旧路径；协议 v33 与旧二进制不兼容，必须整包替换。

---

## 7. 与情况 1 的关系

本条是「情况 2：UI 不可信，但能稳定拿到被遮挡画面」。v34 定稿只保留一条流：

```text
stream 4 = patch（scene） （始终）
stream 3 = 关闭
```

- 只 tag `H'` → mode 4；
- `H' = P outside / patch inside`；
- patch 永远不 tag 成 `RES_UI`；不再叠加 UI 平面。
