# M3 执行步骤：ReShade 深度采集

## 已部署（我这边）

| 项 | 内容 |
|---|---|
| `effects/mgpu_depth_tap.fx` | 自包含，无 `#include`；声明 `texture MGPU_SceneDepthTex : DEPTH;`，pass 写私有目标，不影响画面 |
| `src/m3_depth/m3_depth.cpp` | ReShade addon，观察全部深度/后台缓冲信息 |
| 部署 | `m3_depth.addon64` + `reshade-shaders\Shaders\mgpu_depth_tap.fx` 已就位；`m2_probe` 与旧项目 addon 已变 `.off`；`ReShadePreset.ini` 已启用 `MGPU_DepthTap@mgpu_depth_tap.fx` |
| 旧 .fx | 保留为 `mgpu_depth_tap.fx.bak` |

## 你要做的

1. 启动 2077（DLSS 性能档，HDR 不变）
2. 进存档，在世界里 **站 30 秒**
3. 退出游戏
4. 告诉我一声，我读日志

## 日志会回答什么

`m3_depth.log` 里每 120 帧一条，格式：

```
-- present #241
    screenshot size  : 5120x2880   back buffers=3
    backbuffer       : res=...  5120x2880  fmt=R10G10B10A2_UNORM(24)  ...
    depth            : res=...  2560x1440  fmt=R32_TYPELESS(39)  ...
    motion probe     : <none> (srv=0)
```

判读：

| 现象 | 含义 |
|---|---|
| `depth : ... 2560x1440 fmt=R32_TYPELESS(39)` | ✅ 与 M2 从 NGX 拿到的深度完全一致，M3.2 可以直接做 R16 转换 |
| `depth : <none>` | 语义没绑上：要么 .fx 没编译通过（看 ReShade.log），要么预设没生效 |
| `depth : variable not found` | ReShade 没加载到我们的 .fx（路径/预设问题） |
| `backbuffer : 5120x2880 fmt=R10G10B10A2_UNORM` | ✅ 颜色流的来源确认，正是 56.25MB 那个格式 |
| `motion probe` 有资源 | 🎁 额外收获：ReShade 也能提供 MV，可作为 NGX hook 的备选路径 |

## 下一步（M3.2）

拿到确认后，M3.2 会加一个 compute pass：把深度（R32_TYPELESS）
按 0.5 倍降采样并写成 **2560×1440 R16_UNORM**，写进 → `mailbox.producer_source(2)`。
届时需要给 `StreamSpec` 加 `D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS`（现在源码纹理没有 UAV 标志）。

## 回滚

删掉 `bin\x64\m3_depth.addon64` 与 `reshade-shaders\Shaders\mgpu_depth_tap.fx`，
把 `m2_probe.addon64.off` 改回即可。
