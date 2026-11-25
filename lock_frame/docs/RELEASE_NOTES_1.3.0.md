# lock_frame 1.3.0 发布说明

> 后续 **1.3.1** 已通过 `viewMatrix` 输入对齐解决**屏幕边缘伪影**；人物模型边缘残留
> 归因 XeFG 模型本身。当前发布见 [`RELEASE_NOTES_1.3.3.md`](RELEASE_NOTES_1.3.3.md)。

## 状态：DLSS FG 双卡补帧链路完成

1.3.0 的 UI 合成链路已经定稿并通过 **CP2077 / PBZ / Wuchang** 实机验证：

```text
游戏 DLSS FG 输入（渲染卡）
  -> P/H 差分，仅取被 UI 覆盖的 support
  -> stream 4 sparse HUDless patch
  -> consumer 重建 H'
  -> 只 tag H'，XeFG BACKBUFFER_HUDLESS（mode 4）
  -> 输出卡上屏
```

- **UI 稳定，插值帧无 HUD 拖影**；
- 准星移动时的色块/抖动已消失；
- 模型边缘的轻微伪影是 XeFG 模型本身的限制，不再属于 UI 合成链路；
- stream 3 / mode 5 已封死，不再支付 UI 平面编码/解码成本；
- CP2077 实测 UI patch-only 开销约 **2.5pp**（旧 mode 5 路径约 3pp）。

## 默认行为变化

**UI 合成默认开启，但固定为 mode 4；stream 3 / mode 5 已封死。**

- 默认链路：
  - `UiStream`（stream 3）：**关闭**，不发布游戏 UI 平面；
  - `HudlessStream`（stream 4）承载 producer 计算出的**支撑域 HUDless patch**
    （P/H 差分 support，含 sparse 32×8 tile 掩码）；
  - consumer 统一还原 `H' = present（支撑外）/ patch（支撑内）`，并用非 UI 像素对做
    **33³ 3D LUT（H scene code → WGC P code，8 角点四面体插值）**；1D 32 档 LUT 只
    在 3D 初始化失败时兜底；
  - **始终只 tag `H'`** → XeFG `BACKBUFFER_HUDLESS`（mode 4）。不 tag UI 平面。
- `fakerFG.ini` 默认：
  ```ini
  UiStream=0
  UiSource=Off
  UiSlots=2
  UiPatch=1
  UiPatchWhiteNits=80
  UiFullRes=1
  UiCodec=1
  UiSparseCodec=1
  UiAlpha8=0
  HudlessStream=1
  ```
- 回退：
  - `UiPatch=0` + `HudlessStream=0` 关闭 UI 合成；
  - 旧 stream 3 / mode 5 路径不再作为默认，脚本里的 mode5 也不会被 patch 模式采用；
  - SDR 标题会自动退化（patch 只支持 R10/HDR10）。
- 游戏不在 DLSS-G evaluate 里提供 HUDless 时自动退化，不会报错。
- 协议 **v34**：stream 3/4 的语义变了，必须整包替换 producer/consumer；
  旧 v33 二进制会拒绝混合运行。

## 实测与验证

| 游戏 | 状态 |
|---|---|
| Cyberpunk 2077 4K HDR / 3x | **通过**：UI 稳定无拖影，准星色块/抖动消失；patch-only 约 2.5pp |
| PBZ 4K HDR | **通过**：UI 稳定无拖影 |
| Wuchang: Fallen Feathers | **通过**：UI 稳定无拖影 |

CP2077 最终 patch 覆盖率约 8–11%，`fr: coded ui` 约 2.4–3.0 MB/帧。
PBZ / Wuchang 的小 UI 场景走同一 sparse patch + 3D LUT 路径。

> 1.3.0 最终构建在消费端使用 **33³ 3D LUT**：`alpha=0` 干净像素直接给出 H 的 R10 code
> 与 WGC P 的 R10 code 配对，在 code 空间建 H→P 的交叉通道映射；空单元用邻域填充，
> 没有任何干净样本时退回恒等（不发明颜色）。这样能表达 1D LUT 无法表达的 gamut /
> tonemap 串色项，且不需要回读。

## 其它

- `patch_gain` 仍保留每通道 32 档 LUT 作为兜底；默认路径改为 **33³ 3D LUT**（H scene
  code → WGC P code），修复 1D LUT 的发灰问题并补齐交叉通道项；
- 消费端按 **逐像素 support alpha** 使用 patch：`alpha=0` 的干净像素直接用 present，
  只有支撑像素才应用 patch/LUT，避免整 32×8 tile 都被 patch 替换后 HUD 周围色调差。
- **display-referred patch 暂不作为默认**：`ScalingOutputColor` 在 CP2077 Present 时仍
  出现绿色块/未完成区域，不能作为 patch 颜色。`HudlessSource=Auto` 回到 scene-referred
  HUDLess 快照；`Scaling` 仅保留为显式实验。
- **局部 tone field 已停用**：它会产生 UI 边缘轮廓；当前使用全局 3D LUT 处理
  scene→WGC 的交叉通道/tone 差异，compose 用 8 个角点做四面体插值。
- 新增 **support 边缘 7×7 coverage 羽化**和干净样本的 support 5×5 排除，修复移动时
  UI 边缘的细白轮廓；`patch_local`/`patch_local_fill` 用同一帧干净像素上的
  `present / (patch * globalLUT)` log2 比例，按 32×8 tile 填充并双线性应用。
- 修复诊断回读 buffer 越界（256B → 512B），避免 AMD 设备移除；
- `UiPatchWhiteNits` 仅作为传输编码提示（默认 80），消费端自动校正；
- 已知残差：3D LUT 依赖干净像素覆盖；样本稀疏的 code 单元用邻域/恒等填充，UI 区域
  本身没有 ground truth。最终 CP2077/PBZ/Wuchang 实机均无可见 HUD 拖影；仅剩 XeFG
  模型边缘伪影，与 UI 合成无关。
- 性能审查见 `docs/UI_PATCH_PERF.md`：CP2077 实测 patch-only 约 2.5pp；消费端 compose
  为 8 角点四面体插值；3D 直方图构建每 4 像素采样一次。渲染卡的 patch synth 目前仍是
  全帧读写，tile-gated 是下一步主要优化。
