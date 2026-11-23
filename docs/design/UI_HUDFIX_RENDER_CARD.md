# 渲染卡跑 hudfix、只传 UI 流：方案评估（2026-11-22）

> 状态：**评估文档，不改代码、不开口子**。结论先行：
>
> **方向是对的，而且是目前唯一能扩大 UI 合成覆盖面的路；但"完整复用 OptiScaler 的
> hudfix 管线 = 完美兼容"这个前提不成立。**
>
> 真正的收益不是"兼容性白拿"，而是**把 UI 流的来源从"游戏给不给平面"换成"我们在渲染卡
> 上自己造"**；真正的代价是 5k–7k 行紧耦合移植、一个新的进程内 hook 面、UI 发布点必须
> 挪到 present 侧（协议/配对改动），以及把成本从副卡挪回渲染卡。
>
> 建议按 M0（只读探针）→ M4 分阶段，**M0 失败就停**，默认保持 `UiStream=0`。

---

## 0. 提案与评估对照

| 提案里的判断 | 评估 | 依据 |
|---|---|---|
| 把 UI 流的计算放在渲染卡 | ✅ 可行，但"计算"不是现成的：hudfix 只产出 HUDless，UI 要我们自己做一次差分+降采样 | `hudfix/Hudfix_Dx12.cpp:874,904` 只 SetResource `HudlessColor` |
| 完整复用 OptiScaler 的 hudfix 管线 | ⚠️ 可移植（本仓已是 GPL-3.0、已移植过 RUI/HudCopy/HC），但体量 5k–7k 行 + 4 类 DXIL blob + 逐标题配置 | `resource_tracking/ResTrack_dx12.cpp` 2871 行、`hooks/D3D12_Hooks.cpp` 2665 行 |
| 以"hudfix 是否提供 UI"作为门控 | ⚠️ 不存在这个布尔量；要自己合成"捕获成功 && P 可用 && 尺寸/格式匹配 && 覆盖率合理"。且不能逐帧切 composition | `Hudfix_Dx12.cpp:547`；`UI_UNBLEND_MODE4.md:218` |
| 传输 UI 流到补帧卡做最终合成 | ✅ 消费端可以完全不动（B 方案链路已在），这是本方案最省事的部分 | `UI_UNBLEND_MODE4.md:205-222`、`085_ui_unblend.inc` |
| 可以完美复用 OptiScaler 的兼容性 | ❌ 继承的是"覆盖集 ∩ 我们的接管能力 ∩ 双卡时序"，不是并集；上游自己就有 10 处 `Hudfix incompatible` | `misc/GameConfigs.cpp:247,262,338,518,539,562,584,605,761,773` |
| 不用自己做 HUDless 捕获等高风险操作 | ❌ 恰恰相反：hudfix 本身就是**在游戏进程里 hook 每条 draw/dispatch/绑定**的启发式捕获，风险面比我们现在的换链接管更大 | `ResTrack_dx12.cpp:2471-2692` |

---

## 1. 概念先对齐（两处必须纠正）

### 1.1 hudfix 不产出 UI，它产出 **HUDless 快照**

`Hudfix_Dx12::CheckForHudless()` 做的是：

1. 判断某个资源像不像 hudless（同交换机尺寸、同格式组、不是带 `DEPTH_STENCIL`/`DENY_SRV`
   等 flag，见 `CheckResource()`，`Hudfix_Dx12.cpp:299`）；
2. `CopyResource` 到自己的 `_captureBuffer[fIndex]`（`:745`），最多 4 槽环形；
3. 把这块 buffer 作为 `FG_ResourceType::HudlessColor` 交给 FG（`:874` / `:904`）。

**"UI" 是下游派生的**：要么 FG 运行库从 `present − hudless` 里提，要么 OptiScaler 的
`HudCopy`/`UiUnblend` 在存在游戏 UI 平面时做 `(present − ui·a)/(1−a)`。

所以提案里的"hudfix 管线提供 UI"，落到我们的架构上必须是：

```text
渲染卡：H = hudfix 捕获的 HUDless；P = 带 UI 的最终 backbuffer
        UI_i = downsample(synth(P_i, H_i))      ← 这一 pass 是新写的
        publish stream 3 = UI_i                 ← 发布点要挪到 present 侧，见 §4
补帧卡：unblend / mode 4（现有链路，不动）
```

这一步的数学我们已经有（`shaders/unblend_ui.hlsl` 的逆运算、消费端
`synth_ui.hlsl`），但**位置要从"消费端算"改成"渲染卡算"**，这不是把现有 shader 挪个
地方那么简单（见 §4）。

### 1.2 没有"是否提供 UI"这个现成信号

hudfix 的对外状态只有"这一帧有没有捕获到 hudless"（`_captureCounter`/`_fgCounter`）。
它连"这块是不是真的 HUDless"都不保证——上游要靠 `FGResourceBlocking` 的资源重试
（70 帧）、`FGHudCutoff` 阈值、`FGHudfixDisable{RTV,SRV,UAV,OM,Dispatch,DI,DII}` 逐标题
关钩子来兜底。

因此"以 hudfix 是否提供 UI 为门控"要落成三层（§7），而不能写成一个 `if (haveUi)`。

---

## 2. 它解决的问题：我们的缺口是 **输入源**，不是合成路径

现有 B 方案（UI 流 + 消费端 unblend + XeFG mode 4）已经全链路落地：

| 环节 | 位置 | 状态 |
|---|---|---|
| stream 3 布局 / 降采样 / 拷贝 / active_mask | `lock_frame/producer/producer.cpp` | ✅ |
| 消费端拷贝 + freshness | `lock_frame/modules/060_frame_io.inc` | ✅ |
| unblend → HUDless → tag（mode 4） | `lock_frame/modules/085_ui_unblend.inc` | ✅ |
| `SetUiCompositionState` 门控 | `lock_frame/modules/040_cadence.inc` | ✅ |
| 默认发布 | `UiStream=0` / `UiSource=Off` | ✅ 关 |

缺的是"游戏愿不愿意给平面"。已记录的事实：

- CP2077：我们自己的抓取 `hudless=n ui=n`，sm86 的 4K 抓取也只交 `DLSSG.HUDLess`
  （`UI_UNBLEND_MODE4.md:367,414`）；
- Khazan：没有 `UI_COLOR_AND_ALPHA` tag（`:415`）；
- 结论文档写得很直白："在游戏只提供 present / HUDless / UI 三选一的现实输入下，跨卡 UI
  合成没有通用方案"（`lock_frame/docs/RELEASE_NOTES_1.2.5.md` §3）。

**hudfix 是唯一能在"三选零"时凭空造出第二条流的已知方法**——这正是提案的价值。
它也解释了为什么 `UI_SYNTHESIS_GATE.md:32,37` 会把"render-target heuristic
(Hudfix/ResTrack prototype)"单独列出来：那个精简原型已经被移除过一次，本次提案的差别是
**完整移植而不是简化**。


---

## 3. 可行性：插入点接得上，但要新挖三层地基

### 3.1 已有的插入点（这是好消息）

| hudfix 需要 | 我们的现状 |
|---|---|
| 帧边界 `UpscaleStart/UpscaleEnd` | `inputs/FG/Upscaler_Inputs_Dx12.cpp:128,256`；我们已在 `provider_evaluate` 拿到标题的 **open list**，并在同一位置跑过 `pass_hud_copy`（`FAKER_FG_PORT.md` §18.4/§22） |
| present 边界 `PresentStart/PresentEnd` | `hooks/FG_Hooks.cpp:1472,1578`；我们的 `fg_swapchain_hooks.cpp` 已有 Present/Present1/ResizeBuffers 槽位 |
| 描述符/SRV 解析 | ❌ 无（见 3.2） |
| tile 级 draw/dispatch 观察 | ❌ 无（见 3.2） |
| DXIL 着色器 | 大部分已有：`faker_fg/shaders/{hud_copy,hudless_compare,render_ui,render_ui_pm}.hlsl` + `passes.cpp` 的 PSO/根签名/内嵌 CSO 基建 |

也就是说，**"在 Evaluate 里往标题的 list 上录东西"这条经验我们已经有了**，这是整个移植里
最容易出问题的一段。

### 3.2 缺的是进程内的 D3D12 hook 面（风险最高的一段）

`ResTrack_Dx12` 为 hudfix 装了两族钩子：

| 族 | 钩子 |
|---|---|
| 命令列表（`ResTrack_dx12.cpp:2471-2549`） | `Reset` / `ClearState` / `OMSetRenderTargets` / `SetGraphicsRootDescriptorTable` / `SetComputeRootDescriptorTable` / `DrawInstanced` / `DrawIndexedInstanced` / `Dispatch` |
| 设备（`:2578-2676`） | `CreateRenderTargetView` / `CreateShaderResourceView` / `CreateUnorderedAccessView` / `CreateDescriptorHeap` / `CopyDescriptors` / `CopyDescriptorsSimple`，外加 heap `Release` 钩子 |

再叠 `D3D12_Hooks.cpp` 的根签名布局记录、描述符堆地址区间、持久绑定恢复
（`FGHudfixPersistentBindings`），才能把"GPU 描述符句柄 → 资源"反解出来。

我们当前只有：swapchain vtable、NGX/SL/FFX 代理、kernel32 加载器。**这一层是新增的、
每帧每条命令都会走的路径**，也是和 `CAPTURE_RULES.md` 里"只 hook 函数、不替换对象"最贴合
的做法（好），但它同时是"和游戏/中间件抢 vtable"的典型场景（不好）。

### 3.3 移植体量（本地 checkout 实测行数）

| 组件 | 行数 |
|---|---:|
| `hudfix/Hudfix_Dx12.cpp/.h` | 978 + 151 |
| `resource_tracking/ResTrack_dx12.cpp/.h` | 2871 + 750 |
| `hooks/D3D12_Hooks.cpp`（根签名/描述符堆/持久绑定子集，估 50–60%） | 2665 → ~1400–1600 |
| `shaders/format_transfer/FT_Dx12.cpp/.h` | 142 + 41 |
| `hud_copy` / `hudless_compare` / `render_ui` / `ui_unblend` | 已有 |
| 需补的 DXIL：format_transfer(DI/DS)、resource_flip、hudfix 相关 | 3–4 个 blob |
| **合计（净新增，不含配置数据）** | **≈ 5.5k–6.5k 行** |

还要替换依赖：`State::Instance()`（交换机 desc / currentFeature / currentFG /
gameQuirks）、`Config::FGHUD*`、`GameConfigs` 逐标题表、`DeferredRelease`、
Detours（我们已有 MinHook，可替换但要重写 attach 段）。

### 3.4 上游的隐含前提，我们并不都满足

- `IsResourceCheckActive()`（`Hudfix_Dx12.cpp:479-518`）要求
  `Config::IsFGEnabled() && FGHUDFix && currentFG->IsActive()`——在 OptiScaler 里
  "FG" 就在同一个进程。我们要换成"bridge 模式 + 本帧有 open list + 版本匹配"的等价判据；
- `Upscaler_Inputs_Dx12.cpp:275` 只在 `activeFgInput == Upscaler` 时把**上采样输出**
  当候选。我们的输入车道是 DLSSG / NVNGX（`FgInput=FSR` 未实现），得自己定义
  "哪个 Evaluation 是帧边界"；
- DX11 标题走上游的 `dx11_with_dx12_sc` 分支，我们只做 DX12，覆盖集不变。

---

## 4. 真正的硬骨头：**UI 的发布点在 present 侧**

这是整个评估里最容易被低估、也最可能让方案"看着能跑、画质不行"的一点。

### 4.1 时序事实

- hudfix 的 HUDless 捕获发生在**帧中间**（UI 画之前，`UpscaleStart` 之后的开窗里）；
- 带 UI 的 `P` 只有到 **present** 才完整；
- 所以 `UI = P − H` **只能在 present 侧算**。

### 4.2 与现有协议冲突

`lock_frame/faker_fg/mgpu_fg_api.h:20-35` 的契约是：

- 一条 record 由 Evaluate 发布，`cmd_list` 只在 Evaluate 调用期间是 **open** 的；
- 从 present 侧发布的 record **不带** `MGPU_FG_FRAME_CMD_OPEN`，在它上面 Dispatch 是
  **undefined behaviour**（命令被丢 / NV UMD 报错）；
- 当前 stream 3（UI）与 MV/depth 共用 record、共用 fence，在 Evaluate 发布
  （`producer.cpp` 的 `on_record()` / `scale_dispatch(..., 3)`），消费端按同一 `present_id`
  配对。

于是方案必须二选一：

| 做法 | 后果 |
|---|---|
| A. 在 present 侧加"第二发布点"（producer 自有 queue/list + fence） | 协议要动（UI 单独帧号/slot 偏移），配对逻辑要改；但同帧正确 |
| B. UI 沿用 Evaluate 的 record、晚一帧再发 | 实现简单，但 mask 与 HUDless **不同帧**，HUD 移动时正好重新引入边缘拖影——等于把 §11.3 的问题换了个地方 |

**评估结论：A 是唯一能接受的；B 不要做。** 好在 producer 已经有一套 present 侧的 4 槽
拷贝环（`FAKER_FG_PORT.md` §18.2 的 `blit` 环），可以作为第二发布点的起点，但它现在是
"present 前把 DLSSG.Backbuffer 拷进链缓冲"的兜底路径，语义和 fence 都要重新设计。

> **补充（2026-11-22，看过 producer 实现后修正）**：A 不一定要"第二发布点/协议 v31"。
> producer 的跨卡拷贝 list 是 **Evaluate 时录制、present 时才执行**的（`g_copy_active_list`
> 在 `on_record()` 打开、`publish_frame()` 关闭，且 `Wait(title frame fence)` 后才提交到
> `g_copy_q`；`producer.cpp:3033-3052`），slot 的 `fwd` 也在它之后才 signal。所以只要把
> UI pass 接在这条 list 后面、`Signal(g_fwd)` 之前，就仍然属于**同一个 publication / 同一个
> 帧号**——协议不用动。真正的新增约束只有一个：**P 必须在 present 路径上先拷进私有纹理**
> （不能让 `g_copy_q` 去读交换链缓冲），见 `FORCE_UI_SUPPLY.md` §5.1–5.2。

### 4.3 还有一处必须做对的取舍

hudfix 在渲染卡上留下的是**全分辨率 VRAM**（4 个 capture + 4 个 FT 缓冲）。4K R10 下
单块 33.2 MB，最多 **~266 MB** 额外 VRAM；可以收成 2 槽（我们是单槽流水线），但仍是
百 MB 量级。加上 `P` 的读带宽，这是"把成本从副卡挪回渲染卡"的直接代价。

---

## 5. 成本

4K / R10（4 B/px）估算，单位 MB/帧：

| 项 | 渲染卡 | 副卡 | 说明 |
|---|---:|---:|---|
| hudfix 捕获 `CopyResource`（H） | 33.2 读 + 33.2 写 | — | `Hudfix_Dx12.cpp:745` |
| UI 差分 + 降采样（P − H → 半分辨率 UI） | 33.2 读 + 33.2 读 + 8.3 写 | — | 新 pass；可直接输出 R10 半分辨率 |
| stream 3 跨卡拷贝 | 8.3 写（现有） | 8.3 读（现有） | 与今天一致 |
| unblend + HUDless 重建 | — | 33.2 + 33.2 | 与今天一致 |
| 额外 VRAM（2–4 槽 capture + FT） | +66–266 | — | 可压缩到 2 槽 |

量级判断：**渲染卡多 ~110 MB/帧片内带宽**（≈0.2–0.4 ms 的纯带宽，实测算上 ALU 估
0.3–0.8 ms），外加 hudfix 的 CPU 侧逐命令开销（未知，上游为此提供了 7 个
`FGHudfixDisable*` 开关，说明在部分标题上不可忽略）。参考值：我们 4K 一次降采样实测
0.30 ms（`LOSSLESS_TRANSPORT.md`），所以"多两个全屏 pass"不是零成本。

对照 B 方案的原始取舍（`UI_UNBLEND_MODE4.md:27,36`）："生产端只拷 UI、性能成本给副卡"。
**本提案把这条取舍反过来了**——用渲染卡换覆盖面。这是评估里最需要被明确接受的一条。

> HUDless / UI 逐字节的链路、环容量与编解码成本，见
> [`UI_PLANE_TRANSPORT_COST.md`](UI_PLANE_TRANSPORT_COST.md)：4K 下 UI（半分辨率）0.64 ms /
> 8.29 MB，HUDless R10 1.75–2.12 ms / 22.7–27.6 MB，HUDless FP16 3.5–4.3 ms / 45.5–55.7 MB。

---

## 6. 兼容性：能继承的是"覆盖集"，不是"完美"

### 6.1 上游自己就不是全兼容

`misc/GameConfigs.cpp` 里有 10 处 `Hudfix incompatible`，并且 profile 直接
`FGDisableHUDFix = true`：`ninjagaiden4`、`tlou-i`、`afop`、`metroexodus`、`outlaws`、
`acmirage`、`dyinglight`、`alanwake2/gotg/u4/tll`、`rottr`，注释里还列了
Veilguard / F1 2020/2021 / GTA V Enhanced / Shadow of the Tomb Raider / Stellar Blade。

要"继承兼容性"，就必须**把这份逐标题数据和 `FGHUDLimit`/`FGHudCutoff`/
`FGResourceBlocking` 一起搬**。只搬代码不搬数据，等于裸跑启发式，翻车率和上游完全不是一个
量级。

### 6.2 启发式的失效模式

- 候选是"任何与 swapchain 同尺寸、同格式组的 RT"——后处理链里常有这种目标（bloom 合成、
  TAA 历史、motion blur 源）。误判时 `P − H` 会变成**大面积"UI"**，unblend 把整帧抹黑；
- 上游的兜底全是启发式：`_hudlessList` 的 ignore/重试（70 帧）、`FGResourceBlocking`、
  `FGHudCutoff`、`_skipHudlessChecks`；
- 直接拒绝 bundle（`Hudfix_Dx12.cpp:559`）——用 bundle 录制的标题拿不到候选；
- 依赖游戏走描述符表/根 SRV 绑定；bindless / descriptor-indexing 的标题解析不到。

**所以"完美复用兼容性"应改写成**：*覆盖集 = OptiScaler hudfix 支持的标题 ∩ 我们能接管
Evaluate/交换链的标题 ∩ 双卡时序成立的标题*。对 CP2077 这类"只给 HUDless"的标题确实是
净增益；对 `Hudfix incompatible` 那批是零增益甚至负增益（我们会在它们身上误捕）。

### 6.3 我们已经踩过的同类坑

`FAKER_FG_PORT.md` §18.3/§18.5 记录过：评估点拷贝会闪烁、present 前拷贝才稳定、裸指针
在 `ResizeBuffers` 后悬空。hudfix 的 capture 是一个**我们自己持有的全分辨率资源**，同样
要处理 resize/Deactivate 时的释放与 `DeferredRelease`——不是新问题，但必须抄到位。

---

## 7. 门控设计（把"是否提供 UI"落成可实现的规则）

三层，职责分开：

| 层 | 判据 | 失效后果 | 处理 |
|---|---|---|---|
| 能力层（会话级） | 本会话连续 N 帧捕获成功 + 尺寸/格式匹配 | 标题不支持 | 永久保持 Off，记一次性原因 |
| 会话层（滞回） | 打开后连续 M 帧 miss 才关；关后连续 K 帧 success 才开 | `SetUiCompositionState` 抖动 → 每次切 skip 10 帧（`UI_UNBLEND_MODE4.md:218`） | 只在这层动 composition state |
| 帧层（逐帧） | 本帧 H 与 P 都新鲜、`generation` 未变、`abs(P−H)` 覆盖率在 [0.5%, 60%] 内 | 单帧误捕 | **不 tag**，退回"烘 UI"画质，但**不动 composition state** |

两点必须写死：

1. **不要逐帧切 composition state**（上游文档、我们自己的 §3.5 结论一致）；
2. **加质量门**：`|P−H|` 的覆盖率/均值差是最便宜的误捕检测，比"捕获成功就启用"稳得多。
   阈值可以先照 `synth_ui.hlsl` 的 `--ui-synth-threshold` 同一量纲。

---

## 8. 与其它路线的取舍

| 路线 | 额外流 | 渲染卡成本 | 覆盖面 | 结论 |
|---|---|---|---|---|
| B：游戏给 UI 平面（现状） | UI | 仅降采样 | 只覆盖给平面的标题 | **保留为最高优先级源**（游戏给的是真 UI+alpha） |
| **本提案：hudfix 本地造 UI** | UI | 捕获 + 差分 + 降采样 | OptiScaler hudfix 覆盖集 | **补充源，默认关，`UiSource=Hudfix` 显式开** |
| C：传 HUDless + mode 4 | HUDless 33 MB | 拷贝 | 同上 | 不推荐：整帧变软（`UI_UNBLEND_MODE4.md:28`） |
| sm86 `UseHudless=1` | — | — | 游戏给平面才生效 | 不做反解，等价 mode 3，不适用（`:403-407`） |

优先级建议：`UiSource = Game > Hudless > Hudfix`；`Auto` 只在前两者不可用且显式允许
hudfix 时才尝试。这与 sm86 的教训一致："默认烘 UI，分离是显式 opt-in"
（`UI_UNBLEND_MODE4.md:377-379`）。


---

## 9. 分阶段与止损点

| 阶段 | 内容 | 产出/判据 | 失败就停？ |
|---|---|---|---|
| **M0（1–2 天，只读）** | 在 `faker_fg` 里装最小钩子集（OMSetRenderTargets + Draw/Dispatch + CreateSRV/RTV/UAV），只**记录**候选 RT 清单与"本帧哪个被读"，不捕获、不传输 | CP2077 / PRAGMATA / PBZ 上能否稳定识别出同一块"同尺寸、UI 前"的 RT；日志行 + dump 指针 | ✅ **失败即停止**（说明 hook 面在目标标题上不成立） |
| M1 | 移植 `ResTrack` + `Hudfix_Dx12`（独立命名空间，开关默认 0），只捕获 + dump | `--dump-hudless` 能落盘一张真正无 UI 的图 | ✅ 捕获质量不达标即停 |
| M2 | present 侧 UI 差分（producer 自有 list/queue）+ stream 3 第二发布点（协议 v31） | 同帧 UI 流到达消费端，`--ui-dump` 与 HUD 对得上 | ✅ 若只能做晚一帧 → 停（§4.2 B） |
| M3 | 三层门控 + 质量门 + 一次性日志 | 不崩 composition、不黑屏、退化可解释 | — |
| M4 | 三游戏验收（PRAGMATA 主验收、CP2077 无平面回归、Khazan 缺平面） | HUD 区块不拖影；`total p95`/`gpu`/渲染卡帧时间 | 渲染卡 p95 恶化 > 3% → 回退 |

止损合计：**M0 一个探针就能否掉大半风险**，这也是本文档建议的第一步——在写 6k 行之前
先花一天验证"目标标题上到底有没有这块 RT"。

---

## 10. 风险表

| 风险 | 影响 | 处理 |
|---|---|---|
| 新 hook 面与游戏/中间件冲突 | 崩溃/黑屏，正是 `CAPTURE_RULES.md` 要防的 | 只 hook 函数、不替换对象；所有钩子可逐个关（照搬 `FGHudfixDisable*`）；M0 先探 |
| 误捕后处理 RT | `P−H` 大面积"UI"，unblend 抹黑整帧 | 质量门（覆盖率）+ `FGHudCutoff` + 资源 blocking/重试 |
| UI 发布点晚一帧 | HUD 边缘重新拖影 | 只做 §4.2 的 A 方案 |
| 渲染卡带宽/VRAM 回归 | 4K 下 +110 MB/帧、+66–266 MB VRAM | 2 槽 capture；UI 直接输出半分辨率；实测后再定槽数 |
| R10A2 2-bit alpha | 半透明 UI 边缘量化（现有问题原样继承） | 走 `UI_UNBLEND_MODE4.md` §11.3 的直接重建路线，或 FP16 流 |
| 上游升级漂移 | 移植代码与 master 分叉 | 记录所依据的 master 快照版本（当前 `third_party/OptiScaler-master`）；NOTICE 列文件 |
| 许可证 | 上游 GPL-3.0 | 本仓已是 GPL-3.0，且已有 OptiScaler 派生代码；补 NOTICE 条目即可（`NOTICE` §一.1） |
| 覆盖被高估 | 对 `Hudfix incompatible` 标题负增益 | 把上游 10 处 profile 一并移植，默认关 |

---

## 11. 证据索引

| 主题 | 位置 |
|---|---|
| hudfix 只产出 HUDless | `hudfix/Hudfix_Dx12.cpp:299,745,874,904` |
| hudfix 的候选判据/格式组 | `Hudfix_Dx12.cpp:20-110,299` |
| 4 槽 capture 环 | `Hudfix_Dx12.h:81,93-94`、`ResetCounters()` |
| 帧边界 | `inputs/FG/Upscaler_Inputs_Dx12.cpp:128,256,275`；`hooks/FG_Hooks.cpp:1472,1578` |
| 命令列表/设备钩子 | `resource_tracking/ResTrack_dx12.cpp:2471-2549,2578-2676` |
| 上游逐标题不兼容 | `misc/GameConfigs.cpp:247,262,338,518,539,562,584,605,761,773` |
| 上游可调开关 | `Config.h:650,671-692` |
| 我们的 B 方案链路 | `docs/design/UI_UNBLEND_MODE4.md:15-18,205-222`；`lock_frame/docs/UI_SOURCE_V29.md` |
| 门控现状与既有原型 | `lock_frame/docs/UI_SYNTHESIS_GATE.md:14-33,41-59` |
| 发布点契约（open/closed list） | `lock_frame/faker_fg/mgpu_fg_api.h:20-35` |
| 我们已有的 pass 基建 | `lock_frame/faker_fg/passes.cpp`（RUI/HudCopy/HC）、`docs/design/FAKER_FG_PORT.md` §18.4/§22 |
| 渲染卡侧已成本实测参考 | `docs/design/LOSSLESS_TRANSPORT.md`（4K 降采样 0.30 ms） |

---

## 12. 结论

1. **值得做，但它是"扩覆盖面"的投资，不是"白拿兼容性"的捷径。** 收益在于
   `UiSource` 多了一个不依赖游戏给平面的来源；成本在于 5–7k 行移植、新 hook 面、
   协议发布点改造、渲染卡带宽/VRAM 回归。
2. **"完整复用 hudfix" 的正确表述是**：移植它的捕获与资源追踪，UI 由我们在渲染卡上
   自己差分出来；门控由我们自己按三层规则合成。上游的兼容性来自"代码 + 逐标题配置 +
   持续调参"，只搬代码不搬配置会得到比上游差得多的结果。
3. **最大的技术风险不是 hook，而是时序**：UI 必须在 present 侧算、必须同帧到达副卡、
   且不能靠逐帧切 composition 门控。这三点任一不成立，画质会退回到现在的拖影。
4. **执行建议**：先做 M0 只读探针（1–2 天，不碰传输），在 CP2077 / PRAGMATA / PBZ 上
   验证"目标标题确实存在一块可用的 hudless RT"。M0 通过再谈移植；不通过就把这份文档当作
   "已评估、不落地"的结论保留。

## 13. 置信度与已知未知

- 高置信：hudfix 产出的是 HUDless（代码直读）；发布点必须在 present 侧（时序推导）；
  成本量级（按字节数 + 已有 0.30 ms 实测）。
- 中置信：移植体量（按行数估，未计调试）；上游 profile 的覆盖面（只统计了注释/默认值）。
- 低置信/未知：hudfix 的 CPU 侧开销在目标标题上的实际值；目标标题是否存在稳定候选 RT
  （M0 才能回答）；XeFG 对"半分辨率 UI 流 + 全分辨率 P"的容忍度（现在是升采样后
  unblend，路径不变，但 UI 质量来源变了）。
