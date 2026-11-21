# 抖动运动矢量（MVJittered）：sm86 的做法与我们的可借鉴点（2026-11-21）

> 结论先行：**我们的链路已经端到端支持 jittered MV，缺的不是“支持”，而是两件小事**：
> ① NGX/DLSS-G 参数块里的 `DLSSG.MvecJittered` 没有读；② `jittered + >2x` 这个组合
> 没有实测、没有策略、失败时只有 `xNmiss` 计数，没有原因。
>
> sm86 的 `D = (jp − jc)·(1 − t)` 是在它自己重写的 model 15 合成核（D31）里做的；
> 我们用 Intel 的 `libxess_fg`，**这条公式搬不过来**。但它的前提事实和策略可以直接用。

---

## 0. 它为什么会有这一项（原文事实）

- 参考副本：`third_party/dlssg_for_sm86-main/`（0.4.2，2026-11-14）。
  `README.md:21`、`docs/INSTALL.md:137`、`docs/ARCHITECTURE.md:541`。
- 它从 DLSS-G 参数块里**显式读** `MvecJittered`（连同 `MvecScale`、`Jitter` 都是数值
  参数，`ARCHITECTURE.md:541`）。
- 它的 2X 用 model 18，3X/4X 用移植的 model 15；**这条 model 15 路径在 3X/4X
  没有 jittered 程序**（文档把来源写成“提供者拒绝这个配置”），所以它自己把 model 18
  的处理移植进了 D31。
- 它的解法：把 model 18 处理该标志的那一项移植到 model 15 的 D31——候选位置的两个
  扭曲方向都加上

  ```text
  D = (jp − jc) · (1 − t),   t = i / (N + 1)
  ```

  `jc` / `jp` 是本帧 / 上一帧的抖动（像素空间）。t=0（当前真实帧）时修正量是完整
  抖动量，t→1（上一真实帧）时归零。代价约每生成帧 +0.08 ms（1440p RGBA8）、
  +0.17 ms（4K）；不带抖动时输出与之前逐字节相同。
- 换句话说：因为 `MV_jittered = MV_true + (jp − jc)`（同一单位下），它是在合成阶段
  按生成帧自己的时间点把这项减回去。**这必须发生在模型内部**，因为它依赖每个生成帧
  的 t 和两个相邻真实帧的 jitter。

## 1. 我们现在在哪里（不是从零开始）

| 环节 | 现状 | 位置 |
|---|---|---|
| Streamline 车道 | 读 `sl::Constants.motionVectorsJittered`、`jitterOffset`、`mvecScale`，逐帧进 record | `lock_frame/faker_fg/proxy_streamline.cpp:611-614, 624, 897-908` |
| NGX / DLSS-G 车道 | 读 `JitterOffsetX/Y`、`MvecScaleX/Y`，但 jittered 标志来自 **DLSS-SR 的 create flags**（`cfg().mv_jittered`），**没有读 DLSS-G 的 `DLSSG.MvecJittered`** | `lock_frame/faker_fg/provider.cpp:153-158`；头文件已包含（`fg_internal.h:38`） |
| FSR 车道 | API 没有 jittered 声明，不发明 | `lock_frame/faker_fg/proxy_ffx.cpp:117-118` |
| 共享协议 | `kInputFlagMvJittered = 1u << 2`，v13 起就有 | `lock_frame/producer/mgpu_shared.h:138` |
| 消费端 | 自动跟随 producer 标志置 `XEFG_SWAPCHAIN_INIT_FLAG_JITTERED_MV`；有 `--fg-jittered-mv` / `--fg-no-jittered-mv` 覆盖；标志晚到时提示需要重启或强制 | `lock_frame/modules/020_state_overlay.inc:503-508`、`040_cadence.inc:1145`、`src/xess_mfg_consumer/modules/input_cache.cpp:235-246` |
| 帧常量 | 逐帧把**同一个槽**的 `jitter_x/y` 交给 XeFG；`--fg-jitter-zero/-flip` 只做 A/B | `src/xess_mfg_consumer/modules/frame.cpp:2065-2077` |
| XeFG 初始化 | `JITTERED_MV` 的官方语义是“motion vectors are not un-jittered”，jitter 是“jittered 时可选提供” | `third_party/xess/doc/xess_fg_developer_guide_english.md:362, 673, 680` |
| 多帧 | `want_interp` 由用户 / 动态节奏决定，上限来自 `GetProperties`；**没有 jittered 相关的钳位或专门日志**；失败只体现为 `xNmiss` 与 cadence 的 shortfall 学习 | `src/xess_mfg_consumer/modules/cadence.cpp:1369-1396`、`frame.cpp:2336-2365` |

**关键事实**：我们随包的是 Intel `libxess_fg 1.3.1.78` / `libxell 1.3.2.10`
（`lock_frame/docs/RELEASE_1.0.0.md:44`）；3x/4x 靠解锁补丁**复用 2x 模型**
（`docs/findings/MFG.md` §7），**不是 sm86 那条 model 15 真多帧路径**。所以它的
“3X/4X 拒绝 jittered”不能直接套到我们身上；我们的 `jittered && interp > 1` 是另一件
尚未验证的事（model 18 支持 jittered，但多帧下 t≠0.5 的修正是否正确还未知）。

## 2. 可以借鉴的三件事

### 2.1 读 `DLSSG.MvecJittered`（推荐先做，纯正确性）

- 在 `provider.cpp` 的 harvest 里加一个“参数存在吗”的读取（现有 `get_flag` 只判断
  值非零、不区分“没设置”）：

  ```cpp
  bool present = false;
  const bool jittered = get_flag_optional(p, NVSDK_NGX_DLSSG_Parameter_MvecJittered, present);
  if (present) { if (jittered) flags |= MGPU_FG_FRAME_MV_JITTERED; }
  else if (cfg().mv_jittered) { flags |= MGPU_FG_FRAME_MV_JITTERED; }
  ```

  规则是“**DLSS-G 参数存在就以它为准，否则回退到 SR create flags**”，不要简单 OR：
  两者可以合法地不一致（标题可以声明 FG 输入是 un-jittered，而 SR 的 create flags
  仍是 jittered）。
- `lock_frame/faker_fg/provider.cpp` 与 `src/faker_fg/provider.cpp` 是两份拷贝，必须
  同步改；协议不变（复用已有 bit）。
- 为什么值得做：DLSS-G 契约里这个字段是权威声明；依赖 SR 的 create flags 会漏掉
  “先建 DLSS-G、后建/不建 DLSS-SR”或“SR 用 FSR/XeSS”的题。

### 2.2 给 `jittered × 多帧` 一个策略（先测，后定）

sm86 的处理方式是“**做不到就明确拒绝，而不是给错画面**”（严格档直接 fatal）。
我们对应位置是 XeFG 提供者，能做的只有三档。注意我们的 3x/4x 实际是 **2x 模型复用**
（`docs/findings/MFG.md` §7），和 sm86 的 model 15 真多帧不是一条路径，所以它的结论
不能预设；下表以实测为准。

| 实测结果 | 建议行为 |
|---|---|
| 3X/4X + jittered 正常（`framesPresented == interp+1`、画面无异常） | 保留，写进文档与 `SUPPORTED-GAMES.md`，不需要特殊代码 |
| 提供者拒绝或只出不满（`xNmiss` 持续、`frameGenResult < 0` / `sr != SUCCESS`） | `jittered && want_interp > 1` 时**钳到 2X**，打一条一次性的 `reason`；或要求显式开关才允许 3X/4X |
| 结果不稳定（有时满有时不满） | 默认 2X；3X/4X 作为实验开关，验收前不发布 |

配套的最小改动不是钳位，而是**让失败可解释**：在 `GetLastPresentStatus` 之后，当
`jittered_mv && interp > 1 && (sr != SUCCESS || ps.frameGenResult < 0 ||
ps.framesPresented < interp+1)` 时打一条一次性日志，带上
`jittered_mv / interp / framesPresented / frameGenResult / sr`。
现在的 `xNmiss` 只告诉“少出了几张”，不告诉“为什么”（`frame.cpp:2343`）。

### 2.3 一行“声明”诊断（低成本，最像它的做法）

sm86 把可诊断性做成硬要求（`preset_ineffective` 一次、`xefg_kernel_fatal{kind}`、
抓取报告里的 `HUD 平面:` 一行）。我们对应的是在**状态变化时打一次**：

```text
mvec: jittered=1 jitter=(0.312,-0.187) scale=(0.000260,0.000463) src=dlssg count=3
```

`src` 取值 `sl` / `dlssg-param` / `saved-sr-flags` / `forced`，直接回答“这个标志是
从哪来的”。比逐帧打印便宜，也比事后翻 `initFlags=0x..` 好读。

## 3. 不值得照搬的部分 / 风险

- **不要移植 D31 的公式**。它成立的前提是“我们自己跑 model 15 的合成核”；sm86 为此
  建了完整的 D3D12+CUDA 运行器。我们的分层（`docs/design/FG_CLAIM.md`）明确不重写
  运行库。若真要自己做，成本是一个引擎，不是一项参数。
- **替代实验（仅记录，不作为第一步）**：在消费端做一次 MV 反抖动
  `mv' = mv − (jp − jc) · mvecScale`（MV 分辨率，1080p 约 2 MB 读+写），然后清掉
  `JITTERED_MV`。这能让 3X/4X 走普通程序，但会改变 XeFG 的历史对齐假设
  （是否还要把 `jitterOffset` 置 0、`--mv-flip` 的符号、UV/pixel 单位都要重验），
  风险不低于收益。
- **符号与单位**：SL 的 `jitterOffset` 是像素空间，NVSDK 是 `Jitter_Offset_X/Y`，
  `mvecScale` 又有 UV / 像素两种约定。任何公式都必须和 `--mv-flip`、`mvecScale`
  一起验证，不能直接抄 `(jp − jc)`。
- **初始化标志是“一次性的”**：XeFG 的 `JITTERED_MV` 在 init 时固定；标题若在建
  feature 之后才声明（我们的 `input_cache.cpp:235` 已经能观察到），只能重启或靠
  `--fg-jittered-mv`。sm86 可以逐 Evaluate 读，这是它自研引擎才有的自由度。

## 4. 验收清单

1. 先补 `DLSSG.MvecJittered` 读取，日志确认 `src=dlssg-param` 与游戏声明一致。
2. 找一款**明确声明 `MVJittered=1`** 的题；目前已能确认的都是 0
   （PRAGMATA `flags 0xB`、Khazan `flags 0x49`、PBZ 实测 0），2077 的 findings 只记了
   “已采集”，没有写取值。所以要么换题，要么用 `--fg-jittered-mv` 强制——但强制只能
   回答“库接受不接受这个组合”，不能回答“MV 对不对”，验收结论要分开写。
3. 2X / 3X / 4X 各跑一组：`initFlags`、`framesPresented`、`xNmiss`、`frameGenResult`、
   `sr`、以及静态 HUD / 快速平移的观感。
4. 通过判据：要么 3X/4X 稳定出满，要么默认回到 2X 且日志给出原因；二者都不允许
   “计数掉了但没人知道为什么”。
