# UI 支撑补丁的性能审查（2026-11-24，1.3.0 默认开启后）

> 对象：`UiPatch=1` 的 **mode 4-only** 路线（producer 只传 UI 支撑域 HUDless patch；consumer
> **33³ 3D LUT** + 四面体插值；stream 3 / mode 5 关闭）。目标：确认真实稳态成本，并列出
> 下一轮优化点。
>
> 结论先行：
>
> 1. **没有旧路径的重复工作**：mode 5、stream 3、tag-time synth 都不执行；共享的
>    patch 分支只解码 stream 4，consumer 只 tag H'（mode 4）。
> 2. **主要在渲染卡的开销是 patch synth 的全帧读写**（虽然只传约 8% 的 tile）；
>    它录在标题的 Evaluate list 上，会直接吃渲染卡余量。
> 3. **消费端不是瓶颈**：实测 `present avg` 约 8.6–9.3 ms；33³ LUT 只在支撑像素上做
>    8 角点四面体插值，1D 校准只在 3D 失败时运行。
> 4. **stream 3 / mode 5 已关闭**：不发布游戏 UI 平面，不请求 UI 槽，不做额外的 UI
>    编码/解码。`UiSlots=2` 仅保留协议最小值；producer 的 stream 3 内存也跳过放置。

---

## 1. 实测（CP2077 4K / 3x / HDR10，最终 patch-only）

| 指标 | 值 |
|---|---|
| `ui-synth` coverage | **8–11%** |
| `fr: coded ui` | **2.4–3.0 MB/frame** |
| consumer `work` | 0.3–0.5 ms |
| consumer `present` | 1.5–2.7 ms |
| consumer `gpu` | 0.5–2.9 ms |
| UI patch-only 渲染卡开销 | **约 2.5 pp** |
| stream 3 / mode 5 | **关闭** |

**PBZ 4K HDR / Wuchang** 也已实测通过：UI 稳定、插值帧无 HUD 拖影；两者都走同一
stream 4 sparse patch + 33³ 3D LUT / mode 4 路径。Patch 的跨卡字节不是问题，
剩余成本主要在渲染卡的全帧 synth。

---

## 2. 成本拆解

| 阶段 | 现行实现 | 量级 | 是否可避免 |
|---|---|---|---|
| producer `synth_ui_full` | patch 分支**全帧**读 P/H、逐像素 PQ 解码/差分、**全帧写 H** + GroupMask + stats | 估计 0.3–0.6 ms（4K，3080 Ti） | **是**：只填命中 tile 可省大部分 |
| producer codec encode | 每个 32×8 tile 一个 block；空 tile 读 GroupMask 后 mode=3 早退 | ~0.05–0.1 ms + 0.6–1.0 ms（非空内容）/ 实测 0.61 ms（full） | 难；已按稀疏 tile 跳过像素 |
| 跨卡 link | 0.8–1.6 MB（PBZ）/ 1.4–3 MB（CP2077） | <0.15 ms 名义 | 否（已是最小充分集） |
| consumer decode | 每 tile 解码/空 tile 写零；full-res 33 MB 写 | ~0.05–0.1 ms | 空 tile 写零可省（compose 不读空 tile） |
| consumer `patch_calib` | **不运行**：3D LUT 成功时跳过；仅 3D 失败时作为兜底 | 0（稳态） | 已移出热路径 |
| consumer `patch_lut3d` | 1/4 采样 → 33³ 直方图（uint4 原子）+ 5³ 填充；每帧一次 | 估计 0.08–0.2 ms（6750 GRE） | 可降频；空单元邻域填充是常数 35937 线程 |
| consumer `patch_gain` | 仅 3D 失败时运行 | 微秒级 | 否 |
| consumer compose | 支撑像素 8 角点四面体插值（Use3D）；兜底为 3×PQ decode + 3×LUT + 3×PQ encode | 3D ~0.1 ms；1D 0.1–0.3 ms | 3D 路径已去掉 PQ 往返 |
| consumer `ui_occ` | 32400 tile / 64 线程 | 微秒级 | 否 |
| **stream 3 / mode 5** | **关闭**：不请求 UI 槽、不编码/解码 UI 平面、不 tag UI | 省一次 UI 路径 | 已封死 |
| `g_patch_*` clear/readback | 4 KB clear + 388 B 回读（不等待） | 微秒级 | 否 |
| 内存 | stream 4 patch 8 槽 + H' 4 槽；stream 3 不放置 | ~0.2–0.4 GB | 比旧 UI 路径小 |

---

## 3. 旧实现是否还在干扰

| 旧路径 | 现状态 |
|---|---|
| XeFG mode 5 / UI 平面 tag | **封死**：patch 模式下 `!ui_patch` 条件禁止 mode5；stream 3 不发布 |
| stream 3（游戏 UI 平面） | **关闭**：`UiStream=0 / UiSource=Off`；消费端不请求 UI，producer 不放置 UI 槽 |
| stream 4 全帧 HUDless | **不用于 patch 模式**：patch 在 stream 4，consumer 合成 H'；`HudlessStream=1` 是 patch 开关 |
| `UiSource=Hudless` 消费端 `synth_ui` | **不执行**：patch 分支直接用 decoded patch |
| tag-time synth (`UiTagSynth`) | **不执行**：patch 不在 `MgpuProducerUiTag` 的 source 里 |
| `UiSynthDump` / tag dump | 基线 `UiSynthDump=0`；`--tag-dump` 仅手动 |
| 旧 unblend `(P−U)/(1−a)` | **不执行**：patch 分支直接 copy/3D LUT |
| `ui_occ` / codec headers | 仍执行，patch compose 的占位必需 |
| `SetUiCompositionState` | 会话内一次；启动 skip 10 帧，稳态无开销 |
| `UiPatch=0` 回退 | 同时设 `HudlessStream=0` 才回到旧路径；patch 默认只 tag H'（mode 4） |
| `UiPatch=1` 强制键 | 强制 `UiCodec=1 / UiAlpha8=0 / UiFullRes=1 / UiSparseCodec=1`，并把 stream 3 封死 |

**结论**：没有 mode5 与 patch 同时合成、没有两条 HUDless 流、没有两次 decode；
唯一的“旧代码”是共享的 unblend PSO/根签名和 `ui_occ`，这些都是 patch 需要的。

---

## 4. 已做的低风险优化

- **3D LUT 默认路径**：`Use3D=1` 时 compose 对支撑像素做 8 个角点的四面体插值
  （33³ LUT），输出已经是 WGC P code；不再执行 3×PQ decode + 1D LUT + 3×PQ encode。
- **跳过 1D 校准**：3D LUT 成功时不再每帧运行 `patch_calib` + `patch_gain`；1D 只在
  3D 初始化失败时兜底。
- **stream 3 / mode 5 关闭**：UI 平面不发布、不请求、不解码；这去掉了额外的 UI
  encode/decode、2 个 UI 共享槽和 mode 5 的 tag 路径。

---

## 5. 建议的下一步（按收益/风险）

| 优先级 | 优化 | 收益 | 风险 |
|---|---|---|---|
| P1 | **producer tile-gated synth**：先做 32×8 tile 命中掩码，再只对命中 tile 读 P/H、写 patch | 渲染卡 0.3–0.5 ms；把全帧 100% 的工作降到约 8% | 掩码漏检会 HUD 残留；需要 alpha 优先 + 保守 dilation |
| P2 | 空 tile 不写零（patch compose 不读） | consumer ~0.03–0.1 ms | 低；需要 decode 的 patch 模式开关 |
| P3 | **3D LUT 降频/变化触发**：稳态每 2–4 帧重建，场景/曝光变化立即重建 | consumer ~0.04–0.1 ms | 低；需要 CPU 端变化检测或 GPU 置信度 |
| P4 | **3D LUT 置信度/低样本格回退** | 只影响质量，不影响主路径 | 需要额外统计 buffer；不要再用局部 tone field |

**测量方法**：固定场景、锁 40 fps，`ProfGpu=1`；分别跑 `UiPatch=0/1`，比较
producer `prof: ui-synth`/`fr: coded ui` 和 consumer `gpu/work/total/present`；
再看 RTSS 渲染卡占用。CP2077 当前实测约 8% 覆盖，是 tile-gated 的主要收益来源。

---

## 6. 结论

- **mode 4-only 链路没有旧路径重复执行**：stream 3 / mode 5 已关闭；
- 消费端已去掉 1D 校准热路径，3D LUT 是 8 角点四面体插值；
- 渲染卡的 **全帧 patch synth** 是唯一明确的性能问题，建议按 P1 做 tile-gated 版本，
  再用 `UiPatch=0/1` 的 `prof:` A/B 决定是否保留默认开启；
- 33³ LUT 在支撑区域样本稀疏时仍可能有个别格使用邻域/恒等回退，这是质量/鲁棒性问题，
  与性能无关，P4 可补。
