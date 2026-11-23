# 能不能"强制游戏按指定链路提供 UI"（2026-11-22）

> 结论先行：
>
> **"强制链路"能做到；"强制引擎产出它本来不画的 UI 平面"做不到。**
>
> 我们已经在做能做的部分：代理 Streamline/DLSS-G、回答 `slDLSSGGetState`（可用/2x）、
> 记录 `slDLSSGSetOptions`、在 `slSetTagForFrame` 处收下它 tag 的一切。剩下能加的只有
> **"把 UI alpha 的收件箱补全"**（tag 23/67/68 + research buffers）和**"确认引擎有没有开关"**，
> 前者零风险、后者需要逐引擎验证。
>
> 真正能达到"按我们的链路拿到 UI"的办法，不是逼引擎打 UI tag，而是：
> **只要它交 HUDless（覆盖率远高于 UI），UI 由我们在渲染卡 present 侧算出来。**
> 这条路对 CP2077（只交 HUDless）和 Khazan（HUDless 每帧都有）直接成立。

---

## 0. 把"强制"拆成三层

| 层 | 能不能强制 | 我们有什么手段 | 证据 |
|---|---|---|---|
| **L1 链路/车道**：让游戏走 Streamline DLSS-G，而不是别的路径 | ✅ 能（且已是默认） | 代理 `sl.interposer` / 剥离 `sl.dlss_g`、回答能力查询、改写 NGX 参数 | `proxy_streamline.cpp:485-532,693-700`、`provider.cpp` |
| **L2 开关**：让引擎的 DLSS-G 分支认为"UI 重组开着" | ⚠️ 只能改**运行时**看到的副本；改不了引擎自己的布尔量 | `DLSSGOptions::enableUserInterfaceRecomposition`（v4+ 字段）、驱动 preset | `sl_dlss_g.h:114-116`、sm86 `ARCHITECTURE.md:271-291` |
| **L3 产出**：让渲染器画出一张它本来不画的 UI 平面 | ❌ 不能 | 只能改引擎/重放 draw，属于引擎手术 | Khazan/CP2077 的反例（§3） |

一句话：**L1 是合同，L2 是运行时内部状态，L3 是引擎代码。** 我们只能碰前两层。

---

## 1. 我们已经在强制的部分（现状，别重复造）

`FgInput=DLSSG` 的配置是（`faker_fg.cpp:603-611`）：

```text
fake_dlssg_feature = 0      // 让游戏真的创建 DLSS-G 特征
fake_sl_player     = 1      // slDLSSGGetState/SetOptions 由我们回答
strip_dlssg        = 1      // 把 sl.dlss_g 插件从 SL 里摘掉
tag_at_evaluate    = 0
```

也就是说：

1. **游戏以为 DLSS-G 可用且已开**：`dummy_get_state` 返回 `status=eOk`、
   `numFramesToGenerateMax=1`、vsync 可用（`proxy_streamline.cpp:493-511`）；
2. **游戏每帧照常调用它的 DLSS-G 集成**（`slSetTagForFrame` / `slEvaluateFeature`），
   这就是我们能收到 MV/depth/HUDless/UI 的原因；
3. **真正出帧的不是它**：`sl.dlss_g` 被 strip，`SetOptions` 也被 `dummy_set_options`
   吞掉，所以真插件不会自己补帧；补帧在副卡的 XeFG。

这就是"已把 L1 做到极限"的含义：**链路、能力、帧边界都在我们手里**。
再往下就只剩引擎的 L3。

---

## 2. 为什么 L2 的"官方开关"在我们这里无效（容易踩的坑）

Streamline 的 `DLSSGOptions` 里有一个看似能"强制 UI 重组"的字段
（`sl_dlss_g.h:114-116`）：

```cpp
// kStructVersion4
//! Optional - if true, DLSSG will allocate a codepath that supports interpolating
//!            'HUDless' and 'UI Color & Alpha' separately than the Color Backbuffer.
//!            this property can be overridden by Nvidia App.
Boolean enableUserInterfaceRecomposition = Boolean::eFalse;
```

它**不是**给引擎的输入，而是给 **DLSS-G 运行库**的输入：让运行库分配一条
"分开插值 HUDless / UI" 的 codepath。sm86 的逆向也证实了这一点——驱动/NVIDIA App 的
"预设 A/B"在 310.9.1 内部就只是这一个布尔量，而且：

- 它在 **CreateFeature 内部一次性解析**（`dlssg_for_sm86-main/docs/ARCHITECTURE.md:271-291`）；
- **没有游戏交的两张平面时，`B` 与 `A` 完全一样**，sm86 只会记一条
  `preset_ineffective`（`docs/INSTALL.md:164,592`）。

而在我们的架构里，**这个运行库根本不参与出帧**（`strip_dlssg=1`），所以：

> 改 `enableUserInterfaceRecomposition` 只是改了一份没人读的副本；
> 它既不会让引擎多打一个 tag，也不会改变 XeFG 的行为。

同理，"强制 `mode=eOn`"也没有意义：`dummy_set_options` 只记录，不转发给真插件；
引擎是否走 UI tagging 分支，取决于**它自己的**选项状态，不是我们返回的 `eOk`。

**结论：L2 的唯一合法用法是"读日志"**（记录游戏传了什么值、`uiBufferFormat` 是多少），
用来判断"这个引擎到底知不知道 UI 重组这回事"。改它不会有收益。

---

## 3. L3 为什么不可能：三个反例

| 标题 | 引擎 | 观察 | 说明 |
|---|---|---|---|
| Khazan | UE 4.27 | FG 跑起来后 **`HUDLESS_COLOR` 每帧都有，`UI_COLOR_AND_ALPHA` 完全没有**（`FG_INPUT_V13_KHAZAN.md:215,273`） | 引擎的 DLSS-G 集成里没有 UI tagging 这段代码 |
| CP2077 | RED4 | 我们抓的 `hudless=n ui=n`；sm86 的 4K 抓取里**只交 HUDLess**（`UI_UNBLEND_MODE4.md:367`） | 引擎只实现了 HUDless 一侧 |
| PRAGMATA | RE Engine | FG 开时 **`hudless=y ui=y`**（`FG_INPUT_V13_PRAGMATA.md:281-285`） | 正例：引擎两边都实现了 |

如果"强制"能创造数据，Khazan 和 CP2077 就不会是反例——它们都已经跑在
Streamline DLSS-G 上、都收到了我们伪造的"可用/已开"，引擎仍然不 tag UI。
**UI 平面的供给是引擎代码，不是运行时开关。**

---

## 4. 合同内还能做的四件事（按风险排序）

### 4.1 把 UI alpha 的收件箱补全（零风险，先做）

Streamline 有 **三种** 与 UI 有关的 tag，我们目前只认第一种
（`proxy_streamline.cpp:295-322` 的 `switch`）：

| tag | 值 | 内容 | 我们的现状 |
|---|---:|---|---|
| `kBufferTypeUIColorAndAlpha` | 23 | RGB + alpha（premultiplied） | ✅ 收（`g_stage.ui`） |
| `kBufferTypeScalingOutputAlpha` | 67 | "覆盖 ScalingOutputColor 的 alpha" | ❌ 漏 |
| `kBufferTypeUIAlpha` | 68 | **只有 alpha**（1 通道，0..1） | ❌ 漏 |

后两种不是"多收一点"那么简单：**1 通道 alpha 没有 R10A2 的 2-bit 量化问题**，
正好能修掉 `UI_UNBLEND_MODE4.md` §11.3 里"半透明 UI 边缘跳变"的老毛病。

实现上只需要：

1. `Stage` 增加 `StageEntry ui_alpha`（`proxy_streamline.cpp:62-70`）；
2. `switch` 里加两个 case；
3. `put()` 的优先级：`23`（有颜色）> `68`（只有 alpha，消费端走"直接重建"）> `67`；
4. `f.stream[MGPU_FG_STREAM_UI_ALPHA]` 已经存在（`mgpu_fg_api.h:121`），传输层不用改。

**收益**：对"只交 UIAlpha"的标题，从"没有 UI"变成"有高质量 mask"；成本 0。

### 4.2 做一次 tag 普查（只读，半天）

在 `slSetTagForFrame` 里把每帧见到的 `type/尺寸/格式/state/lifecycle` 去重后打一行日志
（每标题一次）。它回答三个问题：

- 这个标题到底 tag 了哪些平面（23/67/68/59-66 的 research buffers 也在内）；
- `UIPremultiplied` / `2-bit alpha` 的猜测对不对；
- 有没有"UI 前的颜色"tag（如 `kBufferTypeColorBeforeDepthOfField`），可作为
  `UI = P − 该 tag` 的另一个来源。

零风险、不改行为，但能避免我们"以为它没给、其实给了没接"。

### 4.3 把 `enableUserInterfaceRecomposition` 只读地记下来（半天）

在 `hk_set_options` 里加一行日志（**不改值**）：

```cpp
// structVersion 必须 >= kStructVersion4 才能读这个字段（sl_struct.h:100-104）
if (options.structVersion >= sl::kStructVersion4) {
    logf("sl: SetOptions mode=%u frames=%u uir=%d uiFmt=%u\n",
         ..., static_cast<int>(options.enableUserInterfaceRecomposition),
         options.uiBufferFormat);
}
```

判读：

- 引擎**根本没设**（一直是 `eFalse`/`eInvalid`）⇒ 它不知道这个功能，别指望开关；
- 引擎设了 `eTrue` 却仍不 tag UI ⇒ 这个布尔量不是 tagging 的闸门，死了这条心；
- 引擎设了 `eTrue` 且 tag 了 UI ⇒ 正例，可以进一步研究它的触发条件。

### 4.4 车道/能力偏好（低风险，长期）

两条车道都能出 UI，但形式不同：

| 车道 | UI 的来路 | 备注 |
|---|---|---|
| DLSSG（Streamline） | tag 23/67/68 | tag 种类多、还能看到 research buffers |
| NVNGX | 参数块 `NVSDK_NGX_DLSSG_Parameter_UI` + `_UIAlpha` | 我们已读（`provider.cpp:118-119`），是分开的两张 |

所以 `FgInput=Auto` 的偏好可以写成"**能用 Streamline tag 就不用参数块**"（运行时探测，
不查表）。这不是强制，但能把"游戏已经给了、我们没接"的概率降到最低。

---

## 5. 真正能"按我们的链路拿到 UI"的办法：要 HUDless，不要 UI

把目标从"逼引擎交 UI"换成"逼引擎交 HUDless"，问题就从 L3 掉回 L1/L2：

```text
游戏（渲染卡，只按 FG 合同交 HUDless）
  H = `kBufferTypeHUDLessColor` 的 HUDLESS（或 SCALING_OUTPUT 兜底），我们在 tag 时已拷成稳定快照
  P = 本帧 present 的链缓冲（含 UI）
        ↓ present 路径：P 先拷进私有 ring（不能从异步队列直接读交换链缓冲）
        ↓ producer 的 present 阶段：同一 publication 里算 UI
  UI_i = downsample(synth(P_i, H_i))      // 半分辨率 R10，8.29 MB
        ↓ 现有 stream 3 + 消费端 unblend + mode 4
```

为什么这是"我们的指定链路"：

1. **不需要引擎有 UI tagging 代码**——只要它有 HUDless 一侧（Khazan/CP2077 都有）；
2. **不是启发式**——P 和 H 都是游戏自己给的，差分是确定性的（不像 hudfix 要猜哪块 RT）；
3. **复用已有成本最低的那条流**：8.29 MB/帧、0.64 ms 名义链路
   （`UI_PLANE_TRANSPORT_COST.md` §5.1）；
4. 对 PRAGMATA 这种两边都给的标题，可以优先用游戏的真 UI，差分只作 fallback。

### 5.0 更正（2026-11-22）：正常 FG 链路的采集点是 **Evaluate**，不是 Present

sm86 对 DLSS-G 运行库的逆向给出了直接证据：每次 Evaluate 的 M1 模块从参数块读取
`DLSSG.Backbuffer` / `Depth` / `MVecs` / `HUDLess` / `UI` / `Output*`
（`dlssg_for_sm86-main/docs/ARCHITECTURE.md:541`），M4 把 38 次调度录在**调用方的 list**上，
UI 模式按"R10 彩色 + 8 位 UI + HUD-less = 4"选择（`:544`）。换句话说：

- **合成的 backbuffer 与 UI 前的 HUDless 在 Evaluate 就同时可用**（否则运行库无法做 UI 重组）；
- 运行库自己就是在这个点、用这对图做 UI 估计/重组——它的内核列表里有一个
  `Kernel_DownsampleRGBAndEstimateUIFused`（`:106`），名字直接说明"下采样 RGB 并估计 UI"。

我们自己的对应点不是 present，而是 `on_record()`（`provider_evaluate`）：producer 已经
在这里、在标题的 open list 上 dispatch UI 降采样（`producer.cpp:2654-2670,2737` 的
`scale_dispatch(..., 3)`）。所以正常链路应该：

```text
Evaluate 的 open list 上：
  H = HUDLESS（tag 时已快照，pre-UI）
  P = DLSSG.Backbuffer（tag 的合成帧，含 UI）
  dispatch UI = downsample(synth(P, H)) → stream 3
```

这样**不新增 present pass、不新增异步队列、不新增 P 拷贝**，而且 P/H 天然同帧。

实现前要补的三个坑：

1. **backbuffer 的 state 没进 record**：`proxy_streamline.cpp:1120-1125` 只填了
   resource/尺寸/格式，没有 `state`/`validity`。synth 前要么把 tag 的 `resource->state`
   也带过来（和 MV/depth 一样），要么先 `CopyResource` 到私有纹理再读——后者多 0.09 ms
   但不需要猜状态；
2. **Evaluate 的 list 可能是 COMPUTE / COPY**：COMPUTE 可以 dispatch，COPY 不行。
   producer 已有 `dispatch_lane_ok` 与"list type 不能 Dispatch 就关掉 UI"的处理
   （`producer.cpp:2706-2730`），synth 要接进同一套判断；
3. **UI 是否真的在 Evaluate 前画完**：用 `abs(P−H)` 的覆盖率做质量门（≈0 就是没画），
   识别到之后走 present 兜底或直接不 tag。

**"在 present 时算 UI"只在两种情况下才需要**：

1. 游戏的 Evaluate 在 UI **之前**（不符合 DLSS-G 合同，`P−H ≈ 0`，运行库自己也无法重组）；
2. 我们不是 FG 的 evaluator（只观察别人的 present）。

第 1 种可以用质量门（覆盖率 ≈0）当场识别；识别到之后走 §5.2 的 present 兜底，或直接
mode 2 / 关闭。5.1–5.2 描述的是这条兜底路径，正常标题不需要它。

### 5.1 落点比上一份文档乐观：不需要协议 v31（兜底路径也不改协议）


`UI_HUDFIX_RENDER_CARD.md` §4 当时认为"UI 只能在 present 侧算 ⇒ 要加第二发布点/改协议"。
看过 producer 的实现后可以修正：

- producer 的跨卡拷贝 list 是 **Evaluate 时录制、present 时才执行**的
  （`g_copy_active_list` 在 `on_record()` 打开、`publish_frame()` 关闭；
  `producer.cpp:3033-3052`），而且它是 `Wait(title frame fence)` 之后才提交到
  `g_copy_q`。也就是说**这条 list 在 GPU 上看到的是"帧已经提交完"的内容**；
- slot 的 `fwd` 本来就是在这次提交之后才 signal（同一段代码），所以 UI pass 只要接在
  这条 list 后面、在 `Signal(g_fwd)` 之前，就仍然属于**同一个 publication / 同一个帧号**；
- 因此**不需要新协议、不需要第二发布点**。有两种接法，优先第一种：
  1. **fakerFG 在 Present 前把 P 拷进私有 ring**（用游戏队列、present 侧的 4 槽 copy ring），
     然后照常调用 `producer.present(queue)`。producer 的 `g_frame_fence` 是在这次
     present 之后才 signal 的，所以**既有那条 copy list 已经能看到 P_copy**——UI pass 直接
     录在这条 list 上（Evaluate 时录、present 时执行），连新 list 都不用开。双方只需要约定
     "本帧用 ring 的哪个槽"（seq % ring 即可）。
  2. 若不想让 fakerFG 参与，则在 `MgpuProducerPresent()` 里新开一条 list，接在既有 copy
     list 之后、`Signal(g_fwd)` 之前；代价是多一次提交和一条 fence 链。


### 5.2 兜底路径（present lane）的工程约束：P 必须先在 present 路径上快照

> 正常标题走 §5.0 的 Evaluate lane，不需要本节；只有在"游戏先 Evaluate、后画 UI"
> 或"我们不是 evaluator"时才用 present lane。

不能拿交换链缓冲去异步队列上读：

- `g_copy_q` 是第二个 DIRECT 队列，它的 list 与真正的 `Present()` 并发执行；
  在上面把链缓冲 `PRESENT → NON_PIXEL_SHADER_RESOURCE` 会破坏 Present 的期望状态；
- 正确做法：在 fakerFG 的 present 路径上，用**已有的 present 侧 4 槽 copy ring**
  （`FAKER_FG_PORT.md` §18.2 的 blit 环）把 P 拷到一块私有纹理，再把这块私有纹理交给
  producer。这一步是 33.2 MB 的片内拷贝（≈0.09 ms），不碰交换链状态。

同时要保证 **P 与 H 同帧**：

- H 的 snapshot 是 4 槽环（`proxy_streamline.cpp:163`），必须把"这条 record 用的
  snapshot 序号"一起带过去；否则帧在飞的时候（1–2 帧）会被下一帧覆盖；
- H/P 的色彩空间要对齐：H 可能是 FP16 scRGB，P 是 R10 PQ；synth 前先做
  `scRGB→PQ`（`downsample_ui.hlsl` 已有同样的转换），否则 `P − H` 会整体偏色；
- 若标题把"场景"画在 `DLSSG.Backbuffer`、只把 UI 画进链（Wuchang 的情况），
  必须先完成 `BlitDlssgBackbuffer`，P 才是"场景 + UI"。

### 5.3 失败阶梯（"不保证 100%"具体怎么退）

| 顺序 | 条件 | 动作 |
|---|---|---|
| 1 | 游戏 tag 了 UI（23/67/68） | 直接用游戏的真 UI（质量最好） |
| 2 | 没有 UI，但有 HUDless（tag 或 SCALING_OUTPUT） | present 侧 `P − H` 造 UI（本方案） |
| 3 | H 存在但不可信（黑图/全 1/与 P 不相关） | 质量门拒收，本帧不 tag UI（composition 状态不动） |
| 4 | 没有 HUDless | hudfix 兜底（上一份文档），或保持 composition 关闭 |

"不可信"用最便宜的统计量判：`abs(P−H)` 的覆盖率、均值、与 P 的相关系数，落在
[0.5%, 60%] 之外就拒收。这比 hudfix 的资源启发式简单得多，因为输入是游戏自己给的。

代价与前置条件：**UI 必须在 present 侧算**（`mgpu_fg_api.h:20-35` 的 open-list 契约
不允许在 Evaluate 之后再往标题的 list 上录命令）——但 producer 本来就是"present 时
才提交跨卡 list"，所以这是接一段，不是改架构。

### 5.4 附加成本裁决：贵的不在链路，在调度余量

先澄清一件事：**本方案不传 HUDless**。HUDless 只是渲染卡本地的 `P − H` 输入，
跨卡传的仍然只有 UI 一条流。所以"UI + HUDless 两条都传"不是这个方案的成本，
下面把它列出来只是为了说明"那样做才叫贵"。

相对今天的 MV+depth（100% + 码流：26.3–29.0 MB、2.02–2.23 ms 名义、环 ≈1.53 GB）：

| 方案 | 附加链路 | 附加环 | 渲染卡附加 | 副卡附加 | 4K/3x@40 | 4K/4x@30 | 1440p/3x@40 |
|---|---|---:|---:|---:|---|---|---|
| **+UI（半分辨率，Evaluate lane）** | **+8.29 MB / +0.64 ms** | **+265 MB** | **游戏队列 +0.2–0.5 ms**（synth+downsample，录在标题的 open list 上；同帧、无异步） | +0.3–0.8 ms（unblend） | ⚠️ 本来就超 3.9 ms | ✅ 余 1.6 ms | ✅ 余 2.4 ms |
| +UI（present 兜底 lane） | 同上 | 同上 | 游戏队列 +0.09（P 拷贝）；synth 0.2–0.5 在异步队列（不动命令表，但推迟 `fwd`、仍抢 SM/带宽） | 同上 | 同上 | 同上 | 同上 |
| +HUDless（R10 + 码流） | +22.7–27.6 MB / +1.75–2.12 ms | +1.10 GB | **游戏队列 +1.0–1.2 ms**（转换 0.19 + 编码 1.02，都在 Evaluate 的 list 上） | +0.58–0.72（解码）−0.3~0.8（省 unblend）≈ 净 −0.2~+0.4 | ❌ | ⚠️ 勉强 | ✅（字节约减半） |
| +UI +HUDless | +31–36 MB / +2.4–2.8 ms | +1.4 GB | 两笔相加 | 两笔相加 | ❌ | ❌ 且逻辑冗余 | ❌ 且逻辑冗余 |

预算口径来自 `TIMING_4K40.md` §4.2（4K/40：`gpu 20.6 + 1 vblank 8.33 = 28.9 > 25.0`，
实测 106 fps / skip 5.3；4K/4x@30 是 `23.4 + 8.33 = 31.7 ≤ 33.3`）。

裁决：

1. **链路不是问题**：UI 只加 0.64 ms 名义、0.33 GB/s，占 25 ms 预算的 2.5%，离 13 GB/s 很远；
2. **正常链路的 UI 成本落在 Evaluate 的 list 上**：synth+downsample 0.2–0.5 ms，与 MV/depth
   的降采样/编码同一位置、同一 publication，天然同帧，没有异步队列；
   只有兜底 lane 才需要"P 拷贝 + 异步 synth"（推迟 `fwd`，但仍抢 SM/带宽）。
   而渲染卡今天已经是 `21.6 + 跨卡拷贝 1.1–2.2 + 2×1.02 编码 ≈ 24.7–25.8 ms`，压在 25.0 上
   ——**HUDless 的 +1.0–1.2 ms 会把它推成常态超预算；UI 的 +0.2–0.5 ms 是"可能超"，要用
   p95 输入间隔来判**；
3. **HUDless 传输本身也贵**：3 倍字节、7 倍环；
4. **两条都传是最差的选项**，而且数学上冗余——有 HUDless 就能算 UI，有 UI 就不需要 HUDless；
5. **真正卡住的是 4K/3x@40 本身没有余量**（消费端已超 3.9 ms）。这不是 UI 方案的锅：
   在这个档位上加任何每帧 pass（包括游戏自己给的 UI + unblend）都会掉输出帧。
   **UI 方案应该先在 4K/4x@30 或 1440p/3x 上验收**，这两个档的余量才装得下 0.3–0.8 ms。

关于"固定支付一次真实 HUDless 的拷贝"：这笔钱**今天已经在付**——
`remember()` 对 `onlyValidNow` 的 HUDless 无条件做 4 槽快照拷贝
（`proxy_streamline.cpp:166-249`），即使 UI 流是关的；成本是 33.2 MB 拷贝（≈0.09 ms，
在游戏队列上）+ 4×33.2 MB 显存。差分方案只是把这份已经付掉的拷贝**用起来**；Evaluate
lane 下不再需要 P 拷贝，所以**新增固定成本为 0**（只剩 synth 0.2–0.5 ms）。反过来说，
如果某个标题连 UI 都不做，应该把这次快照改成按需（只在我们真的要 HUDless 时才拷），那是净赚。

异步风险只属于 present 兜底 lane：UI pass 接在 `g_copy_q` 的 list 上会推迟 `fwd`。判据是
现有的 `copywait=ema/max`（`producer.cpp:2149,3072`）和"fwd 迟到导致的输入丢帧"，不是
看总 fps。若抖动不可接受，退路是把 P 拷贝 + synth 都放到**游戏队列 present 之前**
（此时才真正占用 0.3–0.6 ms 的渲染卡帧预算，但不会让消费者等一条异步队列）。
Evaluate lane 没有这个问题，代价是那 0.2–0.5 ms 直接进游戏的 25 ms 帧。



---

## 6. 逐引擎的"开关"：记数据，不写特判

| 引擎 | 已知行为 | 可能的开关 | 处理 |
|---|---|---|---|
| RE Engine（PRAGMATA） | FG 开 → HUDless + UI 都有 | 无（默认就给） | 直接用 |
| UE 4/5（Khazan、PBZ） | 有 HUDless，UI 看插件版本/配置 | 插件侧 UI/Slate 路径、引擎 CVar（版本相关，未验证） | 记进逐标题数据表，不写代码分支 |
| RED4（CP2077） | 只给 HUDless | 无已知开关 | 走 §5 的差分 |
| 其他 | 未知 | 未知 | 先跑 §4.2 普查 |

原则沿用 `FG_CLAIM.md` §0.1：**"这款游戏能过"不是结论，"这种形状的输入能过"才是。**
所以这些数据只用来回答"这条车道有没有某平面"，不用来给每个游戏写 if。

---

## 7. 不建议做的四件事

| 做法 | 为什么不行 |
|---|---|
| 重定向/重放 UI draw 到我们的目标 | 改游戏行为，`CAPTURE_RULES.md` 明确反对；比 hudfix（只读拷贝）更危险 |
| Patch 引擎内存里的 UI 选项布尔量 | 版本/构建脆弱，且我们不认识每个引擎的布局 |
| 在 `slSetTagForFrame` 里伪造 UI tag 指向 backbuffer | 整帧会被当成 UI，mode 4 直接灾难 |
| 把 `mode` 强制转发成 `eOn` 给真 `sl.dlss_g` | 真插件会开始自己补帧，和副卡 XeFG 双重出帧（我们 strip 它就是为了这个） |

---

## 8. 实验计划（E0–E3）

| 阶段 | 内容 | 成本 | 判据 / 止损 |
|---|---|---|---|
| **E0** | §4.2 普查 + §4.3 只读日志，三个标题各跑一次 | 0.5 天 | 拿到"每标题有哪些 tag / 引擎有没有设 uir"的表 |
| **E1** | §4.1 收 67/68，`--ui-dump` 区分颜色/alpha | 1 天 | 至少一个标题从"无 UI"变成"有 alpha mask" |
| **E2** | §5 的 present 侧差分（复用现有 `synth_ui`/`unblend_ui` 数学 + 第二发布点） | 2–3 天 | CP2077/Khazan 上 HUD 区块不拖影；UI 流 8.29 MB/帧 |
| **E3** | 只有在 E2 也拿不到 HUDless 的标题上，才考虑 hudfix | 见 `UI_HUDFIX_RENDER_CARD.md` | 连 HUDless 都没有时才启动 |

E0 的结论会直接决定后面：如果普查发现大多数标题其实 tag 了 67/68，那就根本不需要强制，
也不需要 hudfix；如果发现 HUDless 覆盖足够，E2 就是主路线。

---

## 9. 结论与决策树

```text
引擎 tag 了 UI（23 / 68 / 67）？
  ├─ 是 → 直接收（先做 §4.1/4.2），质量最好，成本 0
  └─ 否
      ├─ 有 HUDless（Khazan/CP2077/PRAGMATA）？
      │    └─ 是 → 渲染卡 present 侧算 UI = P − H，传半分辨率 UI（推荐主路线）
      └─ 连 HUDless 都没有？
           └─ hudfix 自己造（上一份评估；只作兜底）
```

回答最初的问题：

- **没有**"强制游戏按指定链路提供 UI"的通用办法——UI 的供给在引擎代码里，
  我们能强制的是链路、帧边界、以及"把它已经 tag 的东西全部收下"。
- **有一个更划算的等价办法**：强制/利用 **HUDless**（覆盖率高得多），
  UI 由我们在 present 侧按自己的链路算出来。这才是"指定链路"的正解。
- 官方那个 `enableUserInterfaceRecomposition` 开关在我们的架构里**没有下游消费者**
  （`strip_dlssg=1`），改它不会创造 UI；它只能作为诊断信息读出来。

---

## 10. 证据索引

| 主题 | 位置 |
|---|---|
| 我们已有的 claim（GetState/SetOptions/能力） | `lock_frame/faker_fg/proxy_streamline.cpp:485-532,693-700`；`docs/design/FG_CLAIM.md` §1 |
| DLSSG 车道配置（strip/ fake） | `lock_frame/faker_fg/faker_fg.cpp:603-611,625-640` |
| UI tag 的三种形式 | `third_party/streamline/include/sl_core_types.h:113,205-208` |
| `enableUserInterfaceRecomposition` 语义 | `third_party/streamline/include/sl_dlss_g.h:114-116` |
| 它是运行库内部布尔量、需两张平面才生效 | `third_party/dlssg_for_sm86-main/docs/ARCHITECTURE.md:271-291`、`docs/INSTALL.md:164,592` |
| 逐标题平面证据 | `FG_INPUT_V13_{PRAGMATA,KHAZAN,2077}.md` |
| UI/HUDless 的传输成本 | `docs/design/UI_PLANE_TRANSPORT_COST.md` |
| UI 差分/present 侧发布点 | `docs/design/UI_HUDFIX_RENDER_CARD.md` §4 |
| 不建议捕获规则 | `docs/design/CAPTURE_RULES.md` |

## 11. 未知与风险

- UE5 的 UI/Slate tagging 是否受某个 CVar 控制：本仓没有插件源码，未验证；只能靠 E0 普查
  加逐标题数据，不要写进代码。
- `kBufferTypeScalingOutputAlpha`(67) 在不同引擎里的语义可能不是 UI alpha（名字是
  "覆盖 ScalingOutputColor 的 alpha"），E1 要连 dump 一起看，避免把别的 alpha 当 UI。
- 67/68 的尺寸/格式可能和 backbuffer 不同（有的引擎给 1/2 或 1/4 分辨率），
  传输层要按 `extent`/desc 走，不能假设 4K。
- 若某标题的 UI alpha 是 uint 归一化而不是 0..1 float，unblend 前要确认量纲（按
  `FG_INPUT_V13` 的 probe 方式，不猜）。

