# OptiScaler —— 只保留作为参考的实现片段

上游：https://github.com/optiscaler/OptiScaler （我们最初拿的是 `master` 的 zip，
解压目录名 `OptiScaler-master`，2026-11-06 拉取；完整 checkout 123 MB，已删除）。

保留它的原因只有一个：**它是唯一把 XeFG 的输入约定写清楚的公开实现**。
`docs/HANDOFF.md` §8.6 第八轮、`docs/OPTISCALER_NOTES.md` 里的结论都出自这三个文件，
所以把原文留在这里，避免以后再下载 123 MB。

## 保留的文件

| 文件 | 为什么留 |
|---|---|
| `shaders/camera_motion/CM_Common.h` | **XeFG motion-vector 约定的一手证据**：`Output[pixel] = -velocity.xy * size`（游戏 MV）与 `(previousUv - uv) * size`（相机运动）⇒ 像素单位 + (previous − current)。见 §"MV 约定" |
| `shaders/camera_motion/CM_Dx12.{h,cpp}` | 用 `inverse(curViewProj) * prevViewProj` 逐像素生成相机运动矢量（游戏不写 MV 时的官方解法） |
| `inputs/FG/XeFG_Inputs_Dx12.cpp` | `D3D12TagFrameResource` 的实际调用顺序/尺寸约定，以及 `FrameTimeMs()` 对"游戏把帧时间当秒传"的处理 |
| `inputs/FG/XeFG_Inputs_Dx12.h`, `inputs/FG/Upscaler_Inputs.h` | 上面两个文件的类型/接口声明 |
| `LICENSE` | 原样保留：**GPL-3.0**。本仓库整体因此也定为 GPL-3.0（另见根目录 `NOTICE`） |

## MV 约定（原文摘录）

`shaders/camera_motion/CM_Common.h`：

```hlsl
// Velocity in UV units as current - previous, alpha 0 where the game wrote no velocity.
// Output in pixels as previous - current, ...
float2 size = float2(Width, Height);          // = 速度纹理自己的尺寸
if (velocity.a != 0.0f) { Output[pixel] = -velocity.xy * size; }   // 游戏 MV
...
Output[pixel] = (previousUv - uv) * size;                          // 相机运动
```

⇒ XeFG 要的是 **像素单位（按 MV 纹理自身分辨率）+ 符号 (previous − current)**。

> 注意：本仓库当前的 `libxess_fg 1.3.1` **完全不消费** tag 进来的 MV
> （`docs/FINAL_CONFIG.md` §2.1.2 结论 1），所以这份约定目前只是"换库版本后复验"的依据。
