# OptiScaler 参考笔记（2026-11-07）

只记**对我们这个链路有直接价值**的几件事。原文抽在 `third_party/optiscaler/`
（完整 checkout 123 MB 已删除，见 `third_party/optiscaler/README.md`）。
上游：https://github.com/optiscaler/OptiScaler

## 1. XeFG 的 motion-vector 约定（一手证据）

`shaders/camera_motion/CM_Common.h`：

```hlsl
// Velocity in UV units as current - previous, alpha 0 where the game wrote no velocity.
// Output in pixels as previous - current, ...
float2 size = float2(Width, Height);          // = 速度纹理自己的尺寸
if (velocity.a != 0.0f) { Output[pixel] = -velocity.xy * size; }   // 游戏 MV
...
Output[pixel] = (previousUv - uv) * size;                          // 相机运动
```

⇒ XeFG 要 **像素单位（按 MV 纹理自身的分辨率）+ 符号 (previous − current)**。

我们当时的实现（`--fg-of` 的 `mvpx` 单位，`ux = -outW/fullW, uy = -outH/fullH`）就是照这个来的。
**2026-11-08 更新**：早先"tag 进去的 MV 是死输入"的结论已被 `--mv-shadow` 三组对照
推翻 —— XeFG **确实消费** MV（见 `docs/findings/CONCLUSIONS.md` §2）。所以：

- 这份约定**现在在生效**，`--fg-of` 就是按它输出像素单位 + (previous − current)；
- 换 `libxess_fg` 版本后复验的办法不变：`--fg-of-const 100000 0`，
  画面炸掉才说明 MV 真的被消费了。

## 2. 用「深度 + 重投影矩阵」逐像素生成相机运动矢量

`shaders/camera_motion/CM_Dx12.cpp` 的做法比块匹配光流精确得多：

```
Reprojection = inverse(curViewProj) * prevViewProj        // 用当前帧深度重建世界位置再投回上一帧
```

并且**只在游戏自己没写 MV 的地方填**（`velocity.a == 0` 判定），相当于"游戏 MV 优先、缺失处补相机运动"。
对 "The Witcher 3" 这类完全不写 XeFG 输入的游戏这是官方解法，对我们则是备选方案：
它需要每帧的 `curViewProj` + `prevViewProj`，而我们的投影矩阵是**自己合成**的
（`--fg-proj-rz --fg-fov 58.7 --fg-zn 10`），精度不如游戏自己的矩阵。

> 现状（2026-11-08 更新）：这条路仍然**没有启用**，但理由变了 —— MV 已被证明**会**被消费
> （`docs/findings/CONCLUSIONS.md` §2），只是"深度 + 自建投影矩阵"的精度不如 AMD 光流
> （`--fg-of`，0.3 ms），所以没理由再走这条更贵的路。

## 3. `D3D12TagFrameResource` 的调用习惯

`inputs/FG/XeFG_Inputs_Dx12.cpp` 里 MV / depth / UI 各自一次调用，每次带自己的
`resourceSize`（MV 与 depth 尺寸可以不同）——我们的 `run_frame_ex` 就是这么写的
（曾经的 bug 是两条流共用 `streams[1]` 的尺寸）。

## 4. `FrameTimeMs()` 的怪癖

`FrameTimeMs()` 里处理了"游戏把帧时间当**秒**传"的情况（值 < 1 就 ×1000）。
我们是从 WGC 时间戳自己算帧间隔（`frameRenderTime` 喂游戏自己的间隔，
`FINAL_CONFIG.md` §5.4），所以不需要这段补丁；但**换游戏接入时要检查量纲**。

## 5. 相关开关对照（OptiScaler → 我们）

| OptiScaler | 我们 | 说明 |
|---|---|---|
| `[XeFG] UIComposition=false` | `--fg-ui none` | 我们没有 UI 纹理，AUTO 会白等 |
| `[FrameGen] FTInput=2` | `--fg-rt 0` | 喂 `frameRenderTime = 0` |
| `[XeFG] DebugView` / corner tags | `--fg-debug-tag-interp` / `--fg-debug-only-interp` | 只上插值帧，伪影看得更清楚 |
