# DLSSG-only UI 合成：落地实现计划（2026-11-22）

> 范围：**只做 `FgInput=DLSSG`**（Streamline DLSS-G 输入车道）。不做 FSR/NVNGX，
> 不做 hudfix，不做"强制游戏提供 UI"的通用解。
>
> 目标链路：
>
> ```text
> 渲染卡（DLSSG Evaluate，标题 open list）
>   P = DLSSG.Backbuffer（含 UI）
>   H = HUDless（tag 时快照）
>   U = 游戏 UI（23/67/68）
>   if U == null && H != null:  U = synth_full(P,H)     // 全分辨率，render card
>   publish: MV + depth + meta + U（全分辨率）
>           不传 HUDless、不传 colour（WGC 唯一取色，stream 0 恒空）
>
> 副卡
>   P' = WGC 颜色（唯一取色路径；必须与 UI 流按 publication 配对命中）
>         └─ 先写进链缓冲（XeFG 的 backbuffer 是隐式的）
>   H' = unblend(P', U)                                  // full-res
>   tag: HUDless=H'（mode 4）
>        或 HUDless=H' + UI=U（mode 5，游戏真 UI 优先）
>   Present
> ```
>
> 默认 **关**：`UiStream=1` + `UiSource=Auto` + 新键 `UiFullRes=1` 才启用；
> 旧半分辨率路径保留作 A/B（`UiFullRes=0`）。

---

## 0. 先定死的设计决策

| 决策 | 选择 | 理由 |
|---|---|---|
| 采集时点 | **Evaluate 的 open list** | 运行库自己就在这读 Backbuffer/HUDLess/UI（sm86 M1），P/H 天然同帧 |
| UI 分辨率 | **全分辨率，不下采样** | XeFG 要求 HUDless/UI/backbuffer 的 `resourceSize` 相同；且细线不再丢 |
| UI 源优先级 | Game(23) > UIAlpha(68) > 67 > HUDless 差分 > Off | 真 UI 质量最高；差分是兜底 |
| 传输 | **只传 UI**；HUDless 不跨卡 | HUDless 22.7–27.6 MB，UI 0.26–3.0 MB（4K） |
| 编解码 | P1 先纹理，P2 再上 UI 码流 | 先证明画质/门控，再换传输形态 |
| XeFG 模式 | 游戏真 UI → **5**；派生 UI → **4**（默认） | 派生 UI 当 overlay 有"冻场景"风险（`140_frame.inc` 的教训） |
| 颜色来源 | **WGC 唯一**（删除 `ColourStream` 选项）；UI 必须从**配对槽**读取 | P/UI 同帧从"同 publication"变成"配对命中"；失败不 unblend |
| 默认 | **Off**，显式开关 | 沿用 sm86 "默认烘 UI" 的结论 |

---

## 0.1 两个外部约束（2026-11-22 确认）

### (1) lock_frame 砍掉 colour frame 选项，WGC 是唯一取色路径

**要删的**

- `lock_frame/fakerFG.ini` 的 `ColourStream` 键；
- `producer.cpp` 的 `g_want_colour_session` / `ColourStream=1` 分支（stream 0 恒不发）；
- 消费端 `g_fg_colour_shared` 分支（`060_frame_io.inc` / `140_frame.inc` 的 colour-slot 路径）；
- 协议里 `kStreamColour` 保留索引但 `slot_size=0`（v31 不做破坏性删除）。

**带来的硬约束**

- `P`（WGC 颜色）与 `U`（UI 流）**必须按出版槽配对**：
  - unblend 只能用 `g_ui_local[pair_slot]`，不能用"最新槽"；
  - `pair_slot_for_wgc` 未命中、`g_ui_fresh[pair_slot]==false`、或 UI 流不是同一 publication →
    **本帧不 unblend、不 tag**，composition 状态不动（避免 10 帧 skip）。
- WGC 颜色是 DWM 合成后的窗口内容，晚 8–12 ms；它包含 UI，但也可能包含桌面/覆盖层。
  全屏/无边框下就是 backbuffer，与现有链路一致。
- 成本迁移：渲染卡少一次 4K 颜色拷贝（约 6%）、链路少 66/33 MB；
  副卡承担抓取与延迟。UI 的 1–3 MB 在链路里的占比升到 ~5–10%，绝对量仍小。
- 新增指标：`pair:` 命中率、WGC `age`、`no_slot`。P1 要求配对命中率 ≥99%，
  否则先修配对，不进入 P2。

### (2) 验收档位：3x@40，而不是 4x@30

用户确认 **3x@40 是最优档位，当前余量还足**。计划以 **3x@40 为主验收档**，
但 P1 开头必须先做一次 **预算 pre-flight**：

```text
在目标机器/分辨率/标题上测：
  consumer gpu_p50 + 1 vblank  ≤  T_in (25 ms) ?
  input p95（含 copy/encode）   ≤  25 ms ?
```

原因：仓库里的 `TIMING_4K40.md` 在 **4K/3080 Ti** 上量到 `20.6 + 8.33 = 28.9 > 25.0`
（实测 106 fps、skip 5.3）。如果目标配置确实有更大余量，3x@40 就成立；
如果 pre-flight 也超 25 ms，那么**任何**每帧 pass（包括游戏自己给的 UI + unblend）
都会掉输出帧——此时先按 §5 的"异步 synth / 降档"处理，而不是改 UI 数学。

验收指标必须同时看：`presents` / `skip` / 输入 p95，不能只看"画面有没有拖影"。


---

## 1. 阶段计划（每阶段都有 kill criteria）

### P0 —— 普查与埋点（0.5 天，零行为变更）

**改什么**

- `lock_frame/faker_fg/proxy_streamline.cpp`
  - tag 普查：每标题第一次打一行 `sl: tags ui=23/67/68 hudless=... backbuffer=...`
    （type/尺寸/格式/lifecycle），并记录 `backbuffer` 的 `resource->state`；
  - 在 `hk_set_options` 里**只读**打印 `enableUserInterfaceRecomposition` / `uiBufferFormat`；
  - 接 `kBufferTypeUIAlpha`(68) / `kBufferTypeScalingOutputAlpha`(67) 到 `Stage`
    （先只记录，不发布）。

**判据**

- 三个标题（PRAGMATA / CP2077 / Khazan）各拿到一张 tag 表；
- 确认 CP2077 的 HUDless lifecycle、PRAGMATA 的 UI 是否 combined、Khazan 是否真的没有 UI；
- **不做任何传输/合成改动**。

**Kill**：如果目标标题连 HUDless 都没有 → 本方案对它们无效，只能等 hudfix。

**P0 落地记录（2026-11-22，已改代码）**

- `Stage` 增加 `ui_alpha` / `ui_alpha_type`；`backbuffer` 从裸指针改为 `StageEntry`
  （这样 `put()` 会把它的 `state`/`validity` 写进 record）；
- `record_tags()` 新增 67/68，并把 9 类 tag 的去重日志接到 `census_tag()`；
- `dummy_set_options()` 只读打印 `structVersion` / `enableUserInterfaceRecomposition` /
  `uiBufferFormat` / `hudLessBufferFormat`（按 `structVersion >= kStructVersion4` 保护）；
- `sl_stage_inputs()` 把 UI alpha 放进 `MGPU_FG_STREAM_UI_ALPHA`（producer 目前忽略），
  backbuffer 改走 `put()`（带 state/validity）；
- **未改**：传输、合成、XeFG tag、门控。默认行为与改动前一致，只多日志。
- **编译验证（2026-11-22）**：`./lock_frame/build.sh faker`、`producer`、`consumer` 全部通过
  （`fakerFG.dll` / `xess_mfg_producer.dll` / `lock_frame.exe` 已生成）。


### P1 —— 渲染卡全分辨率 synth + 纹理 UI 流（4–5 天）

**P1 开始前的两件事**

1. **预算 pre-flight**（§0.1(2)）：在目标机器/分辨率/标题上测 `gpu_p50 + 1 vblank`、
   输入 p95、`presents/skip`。若 3x@40 仍超 25 ms，先记录，并按 §5 的异步/降档预案执行；
2. **删 ColourStream**（§0.1(1)）：ini 键、producer 分支、consumer `g_fg_colour_shared`
   路径一起删；确认 WGC 是唯一颜色来源、且 `pair_slot_for_wgc` 命中率被记录。

**改什么**

| 文件 | 改动 |
|---|---|
| `lock_frame/fakerFG.ini` / `producer.cpp` / `060_frame_io.inc` / `140_frame.inc` | 删除 `ColourStream` 与 colour-slot 分支（stream 0 恒空） |
| `lock_frame/producer/shaders/synth_ui_full.hlsl`（新） | `m=smoothstep(t,2t,|P−H|)`、`U=P−(1−m)H`、`a=m`；全分辨率；`saturate` |
| `lock_frame/producer/shaders/ui_prepare.hlsl`（新，或给 `downsample_ui` 加 Mode 4） | 1:1 拷贝 / scRGB→PQ / premultiply；**不再 2×2 平均** |
| `lock_frame/producer/producer.cpp` | `select_ui_source()` 增加"无 UI、有 HUDless → synth"；在 `on_record()` 的标题 open list 上 dispatch；`ui_transport_format` 保持 R10/RGBA8；`ui_source` 新值 `kUiSourceSynth` |
| `lock_frame/include/mgpu_shared.h` | `kUiSourceSynth = 3`；`kSharedVersion` 30→31（布局不变，只加语义） |
| `lock_frame/modules/085_ui_unblend.inc` | `kUiSourceSynth` 走 unblend（不再 synth）；**UI 只读配对槽** |
| `lock_frame/modules/060_frame_io.inc` | 拷贝全分辨率 UI；`g_ui_fresh[pair_slot]` 作为硬条件 |
| `lock_frame/modules/140_frame.inc` | mode 4 tag；`resourceSize=输出`；质量门；配对失败不 tag |
| `lock_frame/faker_fg/mgpu_fg_api.h` | 记录 backbuffer 的 state/validity（v3 记录，见 §2） |

**验收（3x@40 为主）**

- 3x@40：`presents`/`skip` 不明显变差（相对同标题基线）；输入 p95 不恶化；
- 配对：`pair:` 命中率 ≥99%；未命中的帧不出现"UI 用了别的帧"；
- CP2077 / Khazan（只有 HUDless）→ 派生 UI → 副卡 unblend → mode 4；
- PRAGMATA（有真 UI）→ 直接用 UI → mode 4（先不用 5）；
- `--ui-dump` 的 alpha 掩码与 HUD 对齐；1px 线不再系统性丢失；
- 指标：HUD 不拖影、`total p95`、`gpu`、输入 p95、`no_slot`。

**Kill**：若 `abs(P−H)` 覆盖率在目标标题上不可信（>60% 或 <0.5% 长期），
或 UI 在 Evaluate 之后才画（P−H≈0），本方案对该标题无效；
若 3x@40 的 skip 明显增加且异步/降档也救不回，按实际情况降档验收。

**P1 落地记录（2026-11-22，已改代码并编译）**

- **WGC-only**：`fakerFG.ini` 的 `ColourStream` 删除；producer 恒不发布 stream 0；
  consumer 走原有 WGC 路径（`g_fg_colour_shared=false`）。
- **全分辨率 UI**：`UiFullRes=1` 时 `StreamPercentUi` 被忽略，`compute_ui_target` 强制 100%；
  `downsample_ui.hlsl` 新增 **Mode 4/5**（1:1 拷贝 / 1:1 + scRGB→PQ），避免 2×2 在 1:1 时模糊。
- **渲染卡 synth**：新增 `producer/shaders/synth_ui_full.hlsl` + 独立 root sig/PSO/heap；
  `kUiSourceSynth` 在 `select_ui_source()` 里选择，`on_record()` 在标题 open list 上
  dispatch 到 `g_ui_synth_tex`（全分辨率、已经是传输格式）。
- **协议**：`kUiSourceSynth=3`、`kSharedVersion=31`（布局不变）；producer 目录的
  `mgpu_shared.h` 旧副本已同步（构建用的是这份）。
- **consumer**：`kUiSourceSynth` 按 UI 处理（不做 synth），从**配对槽**读 UI；
  `ui_source_now()` 以 producer 的 `ui_source` 为准，显式 `--ui-source` 不再覆盖 Synth。
- **仍默认关**：`UiStream=0` / `UiSource=Off`；开启方式
  `UiStream=1 + UiSource=Auto + UiFullRes=1`（本地 A/B）。
- **限制**：synth 需要 `BACKBUFFER` tag 且 state 已声明；缺一帧就只让 stream 3 idle，
  不影响 MV/depth。P2 的 UI 码流与每流槽数尚未落地。
- **编译**：`faker` / `producer` / `consumer` 三者通过（2026-11-22）。


### P2 —— UI 码流 + 每流槽数（3–5 天）

**改什么**

- `lock_frame/shaders/fr_codec.hlsl`：新增 **UI 变体**（`NW=1`、u32 直读直写）。
  - R10A2 需要 `R32_TYPELESS` 视图：源若不可直接建 `R32_UINT` SRV，先
    `CopyResource` 到本地 typeless（≈0.09 ms）；解码到本地 typeless；
  - SDR RGBA8 同理（打包 u32）。
- `producer.cpp`：`stream_codec(3)`；`layout_streams` 支持 stream 3 codec；
  `fr_slot_bytes` 用内容尺寸；`TileLen` 复用。
- `consumer 060/085`：stream 3 为 buffer 时走 decode pass，解到本地 typeless UI 纹理。
- **每流槽数**：`StreamShared._pad_v10` → `slots`（UI=8，MV/depth=32），
  `ui_slot = pub_slot % slots`；环从 32×34.2 MB（1.09 GB）降到 8×34.2 MB（274 MB）。
- 离线：`tests/lossless_bench/fr_codec_test` 加 R10A2 分支；`ui_ratio_test` 回归；
  roundtrip **bit-exact**。

**验收**

- UI 4K 全分辨率：0.26–3.0 MB/帧、链路 0.02–0.23 ms、解码 0.58–0.72 ms；
- 环 ≤300 MB；`fr: coded ui ...` 日志与离线一致；
- 没有新的 device removal（参考 v30 的 raw UAV/COMMON 教训）。

**Kill**：若 R10A2 的 UAV/typeless 路径在目标驱动上不稳定 → 回 P1 纹理路径。

**P2 落地记录（2026-11-22，UI 码流部分，已编译）**

- `fr_codec.hlsl` 新增 UI 变体（`-D UIFMT_R10` / `-D UIFMT_RGBA8`）：pixel 侧是
  `Texture2D<float4>`，shader 内 pack 成 u32（R10A2 / RGBA8），tile body 不变；
  float32 pack/unpack 已对全部 1024 个 R10 值和全部 256 个 RGBA8 值验证**逐值无损**。
- producer：`UiCodec=auto|0|1`；`stream_codec(3)`；layout/place 支持 stream 3 buffer；
  `fr_encode_dispatch(..., 3, slot)`（PSO R10/RGBA8、独立 TileLen、`fr: coded ui`）。
- consumer：`060_frame_io.inc` 在 `streams[3].codec` 时解码到 `g_ui_local[slot]`
  （R10/RGBA8 UAV），解码后 state = COMMON，unblend 照旧；`UiCodec=0` 或格式不支持时回纹理槽。
- 离线：`tests/lossless_bench/ui_ratio_test.cpp` 现在同时验证 `decode(encode(x)) == x`
  （全部用例 bit-exact）。
- **未做**：每流槽数（UI 仍是 32 槽；4K 环约 1.09 GB）。这是 P2 的下一半。

**P2b 落地记录（2026-11-22，每流槽数，已编译）**

- `StreamShared` 的 `_pad_v10` 改为 `slots`（v31，布局不变）：MV/depth 仍 32，UI 默认 8
  （`UiSlots`，2..32）。`layout_streams`/`place_slots`/`reserve` 都按每流槽数计算；
- publication slot 仍是全局 0..31；UI 用 `pub_slot % slots`，producer 的
  `copy_stream`/`fr_encode_dispatch` 与 consumer 的 `place_shared_views`/`copy_inputs_local`
  都在同一处做映射；
- 4K 全分辨率 UI 环：**32 × 34.2 MB（1.09 GB）→ 8 × 34.2 MB（274 MB）**；
- 依赖：8 槽 ≥ consumer 的最大在飞深度（WGC 8–12 ms ≈ 1–2 帧），安全余量足够；
  若未来 lag 变大，把 `UiSlots` 提到 16 即可。



### P3 —— mode 5 与质量门（2–3 天）

**改什么**

- `140_frame.inc`：
  - 游戏真 UI → 同时 tag UI + HUDless（mode 5）；
  - 派生 UI → 默认 mode 4；新增 `UiDerivedMode5=1` 时走质量门后 tag 两者；
  - 质量门：`abs(P−H)` 覆盖率 0.5–60%、P/H 尺寸/格式一致、H 非黑；
  - 仍保持"不逐帧切 composition"（10 帧 skip 的教训）。
- `040_cadence.inc`：`uiMode=AUTO` 已有；确认 `SetUiCompositionState` 只在会话层切。

**验收**

- 同一标题 A/B：mode 4 vs mode 5 的半透明 UI/2-bit 闪烁；
- 派生 UI 在 mode 5 下不得出现"场景被冻进 overlay"（UI dump + 肉眼）；
- 记录两者 `gpu` 差（mode 5 的 refine 成本）。

**Kill**：mode 5 对派生 UI 不稳定 → 固定 mode 4（游戏 UI 仍可用 5）。

**P3 落地记录（2026-11-22，mode 5 + 结构门控，已编译）**

- 协议：`StreamShared` 的 `_pad` 改为 `ui_flags`（bit0 = 渲染卡 synth 派生，bit1 = premultiplied）；
  producer 每帧在 `publish_frame()` 写入 stream 3。
- consumer：`g_ui_mode5`（0 auto / 1 force / 2 off），CLI `--ui-mode5`、`--no-ui-mode5`、
  `--ui-mode5-auto`；
  - auto/force：两种 UI 来源都 → mode 5（2026-11-23 起；旧行为是“派生 UI → mode 4”）；
  - **所有 mode 5 都要求全分辨率**（XeFG 的 resourceSize 约束），否则自动退回 mode 4。
- `140_frame.inc`：HUDless 先 tag；mode 5 时再 tag `RES_UI`（`COMMON`，`draw_w/h`）；
  日志 `[lock] ui: mode 5 (HUDless + UI), source=game|render-synth coverage=... bp`。
- 门控只剩**结构层**（source 类型 + full-res + `g_ui_fresh` + unblend 成功）；
  旧的质量层（P3b 覆盖率）已于 2026-11-23 移除。
- **P3b（已落地）**：`synth_ui_full.hlsl` 增加 `RWStructuredBuffer<uint> Stats : u1`，
  对 `m>0.5` / `m>0.01` 的像素计数；producer 每帧 clear，最多 1 秒回读一次
  （模式同 TileLen），把 `count*10000/(W*H)` 作为 basis points 写进
  `StreamShared[3].ui_quality`；consumer 只在 `50..6000`（0.5%–60%）或未知
  （0xFFFFFFFF）时允许派生 UI 的 mode 5，越界时退回 mode 4 并打一条
  `[lock] ui: mode 5 rejected by coverage ...`。
  > **2026-11-23 更新**：覆盖率门已移除。CP2077 A/B 表明派生 UI 走 mode 5 更好，
  > 现在 `140_frame.inc` 对所有 UI 来源默认 tag HUDless+UI（mode 5），
  > `ui_quality` 只保留为诊断字段；`--no-ui-mode5` 可退回 mode 4。
- 1 Hz 采样意味着 mode 5 的开关最多滞后 1 秒；因为派生 mode 5 本来就是显式 opt-in，
  且 composition 状态有滞回，这个滞后不影响稳态。
- **编译**：producer / consumer / faker 通过（2026-11-22）。


### P4 —— UIAlpha 直接重建 / 线性空间（可选，2–4 天）

- 68 存在时：`a = UIAlpha`（8 bit），`U = P − (1−a)H`；不再用 mask 猜 alpha；
- 没有 68 时：把 alpha 单独走 R8 流（需要第二条 UI alpha 流，协议 +1）；
- 线性空间减法（scRGB 减、再 PQ）：HDR 色偏的最后一块；
- 这一步不做也不阻塞 P1–P3，只是 HDR 半透明的质量上限。

**P4 落地记录（2026-11-22，UIAlpha 直接重建，已编译）**

- `synth_ui_full.hlsl`：新增 `Alpha : t2` + 静态线性采样器 `s0` + `UseAlpha` 常量；
  `a = saturate(Alpha.SampleLevel(Smp, uv, 0).r)`，`U = P − (1−a)H`；
  支持 R8/R8G8/R16/R32 的 alpha 平面（**任意分辨率**，bilinear 对齐全分辨率 P/H）。
- producer：`synth_ui_dispatch` 从 `MGPU_FG_STREAM_UI_ALPHA`（faker 已把
  `kBufferTypeUIAlpha`(68) / `ScalingOutputAlpha`(67) 放进这条流）取 alpha；
  格式/状态不支持时退回差值 mask；root sig 的 SRV 表从 2 扩到 3、heap 5、
  常量 5；alpha 资源在 dispatch 前后做 SRV barrier。
- `ui_flags` bit2 = 本次 synth 使用了真实 alpha（诊断）；日志
  `ui-synth: using true UIAlpha WxH fmt F (P4)`。
- **未做**：线性空间减法（需要消费端 unblend 也在线性/scRGB 里做，属于跨端改动）；
  目前仍在 R10 PQ 空间减，HDR 半透明仍有系统性偏差。

**P4b 落地记录（2026-11-22，线性光减法，已编译）**

- `UiLinear=1`（默认，仅 HDR/R10 全分辨率路径）：
  - producer `synth_ui_full.hlsl`：`Linear=1` 时先 `pq_decode(P/H)` 到 nits（或 FP16
    scRGB → BT.2020 nits），做 `U_nits = P_nits − (1−a)H_nits`，再 `pq_encode`；
  - consumer `unblend_ui.hlsl`：`Linear=1` 时 `H_nits = (pq_decode(P) − pq_decode(U)) / (1−a)`，
    再 `pq_encode(H)`；SDR 仍按码值；
  - 对齐字段：`ui_flags` bit3 = 本会话 UI 流按线性减法构建；
  - 两边同时切，所以 `P − (1−a)H` 的往返在线性光下自洽（只剩 PQ 量化）。
- `UiLinear=0` 保留旧 PQ 码值减法做 A/B；切到 0 时 producer 不设 bit3，consumer 自动走旧路径。
- **编译**：producer / consumer / faker 通过（2026-11-22）。



---

## 2. ABI / 协议改动清单

| 位置 | 改动 | 兼容 |
|---|---|---|
| `kSharedVersion` | 30 → **31** | 混合版本拒绝启动（沿用现有检查） |
| `StreamShared._pad_v10` | 语义改为 `slots`（每流槽数，0=32） | 布局不变 |
| `StreamShared._pad_v30` | 语义改为 `flags`（bit0 ui_derived、bit1 ui_premul、bit2 alpha8） | 布局不变 |
| `kUiSource*` | 新增 `kUiSourceSynth = 3`（流 3 已是 UI） | 旧消费端当 Off（安全） |
| `MgpuFgFrame` | v3：`BACKBUFFER` 增加 `state/validity`，新增 `ui_flags` | `size/version` 握手；producer 检查 |
| `MGPU_FG_API_VERSION` | 2 → 3 | `MgpuFgGetApi(min,max)` 协商 |

**不新增流**：stream 3 仍是 UI；不新增 HUDless 槽。

---

## 3. 代码落点（按文件）

| 文件 | P | 改动 |
|---|---|---|
| `lock_frame/faker_fg/proxy_streamline.cpp` | P0/P1 | 67/68 收集、backbuffer state、tag 普查 |
| `lock_frame/faker_fg/mgpu_fg_api.h` | P1 | v3 record 字段 |
| `lock_frame/producer/producer.cpp` | P1/P2 | synth dispatch、全分辨率布局、codec、每流槽数 |
| `lock_frame/producer/shaders/synth_ui_full.hlsl` | P1 | 新 |
| `lock_frame/producer/shaders/ui_prepare.hlsl` | P1 | 新（1:1 转换） |
| `lock_frame/shaders/fr_codec.hlsl` | P2 | UI 变体 |
| `lock_frame/modules/060_frame_io.inc` | P1/P2 | 全分辨率拷贝/解码 |
| `lock_frame/modules/085_ui_unblend.inc` | P1/P3 | 去 synth 分支、全分辨率、UIAlpha |
| `lock_frame/modules/140_frame.inc` | P1/P3 | tag/mode/质量门 |
| `lock_frame/modules/040_cadence.inc` | P3 | composition 会话门控 |
| `lock_frame/include/mgpu_shared.h` | P1/P2 | v31 字段 |
| `tests/lossless_bench/*` | P2 | R10A2 roundtrip、UI ratio 回归 |

---

## 4. 验收矩阵

| 标题 | 提供什么 | 走哪条 | 判据 |
|---|---|---|---|
| PRAGMATA | HUDless + UI | 真 UI → mode 4/5 | 半透明菜单、HDR、转视角 |
| CP2077 | 只 HUDless（FG 开时） | 派生 UI → mode 4 | 准星/血条/字幕；1px 线；HDR 暗部 |
| Khazan | HUDless 每帧、无 UI | 派生 UI → mode 4 | HUD 区块不拖影；无黑屏 |
| 无平面标题 | 无 | composition off | 与基线一致；日志给出原因 |

指标：`total p95`、`gpu`、输入 p95、`copywait`、`no_slot`、`fr: coded ui`、
`SetUiCompositionState` 次数、`pair:` 命中率、WGC `age`、`--ui-dump` 覆盖。

**档位（2026-11-22 确认）**：**3x@40 是主验收档**（用户指定：最优档、当前余量足）。
每个标题先在 3x@40 上跑 **预算 pre-flight**（§0.1(2)），再进入画质验收；
4K/4x@30 与 1440p/3x 作为回归档（确认没有把其它档弄坏）。
如果 pre-flight 显示 3x@40 超 25 ms，则记录并先走 §5 的异步/降档预案，而不是改 UI 数学。

### 4.1 tag 前三角 dump（2026-11-22 新增）

消费者侧在“即将交给 XeFG”的那一刻，把三张颜色输入落盘，做视觉核对：

```bat
lock_frame.exe ... --tag-dump 5 --tag-dump-after 120 --tag-dump-dir tag_dump
```

每个导出帧产生：

| 文件 | 内容 | 状态来源 |
|---|---|---|
| `tag_XX_backbuffer.bmp` | 链缓冲 P（含 UI，即将 Present） | `RENDER_TARGET` |
| `tag_XX_hudless.bmp` | 重建的 H'（mode 4/5 要 tag 的那张） | `NON_PIXEL_SRV` |
| `tag_XX_ui.bmp` | 实际使用的 UI（解码/拷贝/渲染卡 synth） | `COMMON` |
| `tag_XX_meta.txt` | present_id / slot / source / mode5 / derived / linear / 尺寸 / ui_quality | — |

说明：

- dump 的拷贝录在**本帧同一条命令表**上（与 `--dump` 相同），所以看到的就是被 tag 的内容；
- BMP 是 8-bit 可视化（R10 PQ 直接量化），用于看 HUD 覆盖/边缘/漏检，不是 HDR 显示效果；
- `mode5=1` 时 `_ui.bmp` 就是同时 tag 的那张；`mode5=0` 时它只用于重建 H'，不会上屏；
- 没有 UI/HUDless 的帧会缺对应文件，`meta` 里 `*_bmp 0` 说明原因。

实现：`lock_frame/modules/118_tag_dump.inc`（复用 `110` 的 `dump_prepare` 读回与 `085`
的 `write_bmp24`）。dump 的 UI 取 `g_ui_used_current`（unblend 实际消费的那张），
所以 `UiFullRes=0 + UiSource=Hudless` 的旧路径会落盘 synth ring，而不是误 dump
`g_ui_local`。拷贝在 tag 之后、`Close()` 之前，按资源声明的状态做
`RENDER_TARGET / NON_PIXEL_SRV / COMMON → COPY_SOURCE → 原状态`。



---

## 4.3 CP2077 3x@40 实测部署（本次）

游戏：`E:\SteamLibrary\steamapps\common\Cyberpunk 2077\bin\x64`。脚本源在
`lock_frame/scripts/cp2077_ui/`，部署到游戏目录的 `ui_test\`。

游戏侧文件（`00_check.bat` 校验）：

| 文件 | 本次构建 |
|---|---|
| `fakerFG.dll` / `dxgi.dll` / `nvngx_dlssg.dll` | 同一份 1.2.7，协议 v31 |
| `xess_mfg_producer.dll` | P1–P4b（full-res/codec/8 slots/linear） |
| `lock_frame.exe` | 1.2.7，含 `--tag-dump` 与 P4b unblend |
| `fakerFG.ini` | `UiStream=1`, `UiSource=Auto`, `UiFullRes=1`, `UiSlots=8`, `UiCodec=auto`, `UiLinear=1` |

脚本矩阵：

| 脚本 | 作用 |
|---|---|
| `00_check.bat` | 文件/字节一致性/ini/进程 preflight |
| `01_ui_on.bat` | 写 UI 配置（线性，备份 ini）→ 必须重启游戏 |
| `01b_ui_on_linear0.bat` | 同上但 `UiLinear=0`（A/B） |
| `02_ui_off.bat` | `UiStream=0` 基线 → 必须重启游戏 |
| `03_run_3x_mode4.bat` | mode 4，240 帧后 dump 5 帧 |
| `04_run_3x_mode5.bat` | mode 5（强制），同样 dump |
| `05_run_3x_ui_off.bat` | 无 UI 基线 |
| `06_collect.bat` | 日志+BMP+`summary.txt` → `results\<时间戳>\` |

判据（详见 `ui_test/README_UI_TEST.txt`）：

- consumer：`ui-unblend: HUDless rebuilt from 3840x2160 present + 3840x2160 ...`；
  `cadence: input=25.0ms = 3.00 vblanks -> interp=2 (120.0 fps out)`；
  `presented=3`、`pair: exact` 占绝对多数、`out: presents=120 expected=120`；
- producer：`UI 3840x2160 ... -> 3840x2160`、`ui-synth: coverage`、
  `fr: coded ui`（码流字节明显小于 raw 33 MB/帧）；
- mode 5：`ui: mode 5 (HUDless + UI), source=... coverage=... bp`
  （2026-11-23 起为默认；`--no-ui-mode5` 才退回 mode 4）；
- BMP：`hudless` 无 HUD 残留，`ui` 只覆盖 HUD 区域，`meta.linear=1`。

---

### 4.3.1 首次实测崩溃修复（2026-11-23）

CP2077 第一次跑 `03_run_3x_mode4.bat` 时游戏在 `xess_mfg_producer.dll+0x6CE0`
以 `0xC0000005` 崩溃。映射到 `on_record` 的 DMA 可用性判断：

```cpp
const bool ui_ok = !want_ui || g_scaled[3] ||
    static_cast<D3D12_RESOURCE_STATES>(ui_aux->state) == ...;
```

`UiSource=Synth` 的第一帧 `ui_aux == nullptr`（UI 还没由 `synth_ui_dispatch`
生成），这里无条件解引用 `state`。修复为：

```cpp
const bool ui_ok = !want_ui || g_scaled[3] ||
    (ui_aux != nullptr && ui_aux->state == D3D12_RESOURCE_STATE_COPY_SOURCE);
```

同时 consumer 增加 `ui_enable_composition_when_ready()`：启动时 producer 还没
选 UI 源，`SetUiCompositionState` 会落到 DISABLED；UI 流出现后补一次 ENABLED。

---

## 4.4 CP2077 实测结果（2026-11-23）

| 项目 | 03 mode 4 | 04 mode 5 |
|---|---|---|
| UI 源 | `source=3`（render-synth） | `source=3` |
| UI 格式 | 3840x2160 R10 | 3840x2160 R10 |
| coverage | 7.0–8.3% | 7.0–8.1% |
| `ui_quality_bp` | 830 | 814 |
| mode5 | 0 | **1** |
| UI 码流 | ~1.6–2.1 MB/frame（15–20x） | 同 |
| pair | `exact=40 no_slot=0` | 同 |
| cadence | 3.00x / 120.0 fps | 同 |
| presents | `120/120` | `120/120` |
| xNmiss | 0 | 0 |
| present | 2–8 ms | 4–8 ms |
| 结果 | HUDless 正常、UI 只在 HUD | **UI 拖影基本消失** |

关键修正：

- CP2077 的 P（R10 backbuffer）与 H（FP16 HUDless）是**同一线性/scRGB 编码**；
  两边必须用同一传递函数，不能 P 走 PQ、H 走 scRGB；
- mask 外（`m < 0.01`）UI 强制为 0：之前 `P−H` 的低频残差会被编码成一层
  全屏暗噪声（UI BMP 77% 非零、均值 10/255），清零后 UI 码流从 ~25 MB/frame
  降到 ~2 MB/frame，mode 5 的 UI 只覆盖 HUD；
- producer 的 stats 清零用 8 字节 zero upload + `CopyBufferRegion`，避免
  `ClearUnorderedAccessViewUint` 在 NV 驱动上的 AV；
- synth root constants 必须覆盖 shader 的 `cb0[2]`（8 个 DWORD），否则驱动
  在第一次 Dispatch 时崩溃；
- `--tag-dump` 的 UI 取 `g_ui_used_current`，meta 宽高取 backbuffer desc。

输入 dump：`UiSynthDump=1` 时在 synth 同一帧写
`ui_synth_dump\frame_XX_{p,h,u}.bmp`（原始 P/H/U）；验证完默认关回 0。

---

### 4.5 定稿：只传 UI

clean HUDless 流验证后，画面差异只出现在 UI 覆盖区（opaque UI 会遮住），
且 HUDless 流成本 22–27 MB/frame（raw 33 MB）、环 +264 MB，因此定稿：

- **只传 UI**（`UiStream=1`，`HudlessStream=0`）；
- 消费端继续用 WGC present + UI 做 unblend，tag HUDless；
- `UiDilate=0`、`UiAlpha8=0`、`Threshold=8/255`、`OpaqueAlpha=0.99`，
  与 OptiScaler 推荐口径一致；
- clean HUDless stream 4 代码保留为可选实验（`HudlessStream=1` + `--hudless-stream`，
  见 07/08 脚本），默认关闭。

---

## 4.6 sparse synth / sparse UI codec 迭代（2026-11-23）

本轮完成并验证 1/2：8×8 group 早退 + 空 32×8 tile 的 `mode=3` 快速路径。稳定基线
coverage 约 6–8%，UI 码流约 1.36–1.55 MB/frame，成本从 105% 降到约 103%。

两次进一步实验默认关闭：

- `UiTagSynth=0`：tag-time synth 干扰标题渲染队列，且 tag 时的 P 不是稳定最终帧；
- `UiSnapshotPQ=0`：R10 compute 快照在 CP2077 的 HUDless 状态/描述符下读到黑图。

完整记录、配置、构建哈希、失败原因和复现步骤见
[`UI_ITERATION_2026-11-23_SPARSE.md`](UI_ITERATION_2026-11-23_SPARSE.md)。

---

## 5. 风险与回退

| 风险 | 处理 |
|---|---|
| UI 在 Evaluate 后画（P−H≈0） | 质量门识别 → 不派生；mode 2/off |
| 派生 UI 进 mode 5 冻场景 | 默认 mode 4；mode 5 需 `UiDerivedMode5=1` + 质量门 |
| R10A2 2-bit alpha | P4 的 68/线性空间；短期接受 HDR 半透明黑边 |
| 码流 UAV/typeless 不稳定 | 回 P1 纹理路径（全分辨率 33 MB） |
| 全分辨率 UI 环 1.1 GB | P2 每流槽数（UI 8 → 274 MB） |
| **WGC 配对失败**（没有 ColourStream 兜底） | 本帧不 unblend/不 tag；`pair:` 命中率 ≥99% 是 P1 门槛；未达标先修配对 |
| **3x@40 预算不足** | pre-flight 先测；超 25 ms 时把 synth 挪异步队列，或按实际情况降档 |
| 输入 p95 变差 | 同上：异步 synth / 降档 |

**回退**：`UiFullRes=0` 回到旧半分辨率路径；`UiStream=0` 完全关闭；
协议 v31 与旧二进制不混用。

---

## 6. 与现有文档的关系

- 传输成本：`UI_PLANE_TRANSPORT_COST.md`（全分辨率口径见 `UI_COMPUTE_QUALITY.md` §3.6）
- 数学与误差：`UI_COMPUTE_QUALITY.md`
- XeFG 参数：`XEFG_UI_INPUTS.md`（mode 4/5、resourceSize、2-bit）
- 采集/门控：`FORCE_UI_SUPPLY.md`（Evaluate lane、失败阶梯）
- hudfix 只作"连 HUDless 都没有"的兜底：`UI_HUDFIX_RENDER_CARD.md`

