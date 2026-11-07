# M2 执行步骤：截取 DLSS 的运动矢量

探针已部署进 2077，**不需要你做任何文件操作**，只要跑一次游戏。

## 已完成（我这边）

| 项 | 状态 |
|---|---|
| `src/m2_probe/m2_probe.cpp` | 已写好并编译成 `build/m2_probe.addon64`（28 KB） |
| 依赖 | `third_party/dlss`（NVIDIA DLSS SDK 头文件）、`third_party/minhook`（MIT） |
| 部署 | `dxgi.dll`（ReShade 6.8.0.2155）已恢复；`m2_probe.addon64` 已放进 `bin\x64` |
| 旧项目清理 | `mgpu_bridge.addon64` 与 `nvngx.dll_mgpu_bridge.addon64` 已重命名为 `.off`（可随时改回） |

## 你要做的（约 3 分钟）

1. **启动赛博朋克 2077**（Steam 正常启动即可）。
2. 进游戏后确认 **DLSS 超分辨率 = 性能档**，**DLSS 帧生成关闭**。
3. 进入存档，**站到有动态画面的地方**（车流/行人），**等 30 秒左右**。
   探针只记录前 20 次调用，之后每 120 次记一条，所以 30 秒足够。
4. 退出游戏（正常退出，让 addon 走 `AddonUninit`）。
5. **告诉我一声**，日志我自己读：
   - `<GAME_DIR>\bin\x64\m2_probe.log`
   - 顺便我会看 `ReShade.log` 确认 addon 是否被加载。

## 探针会回答的问题

| 问题 | 日志里的位置 |
|---|---|
| 这游戏到底是 `nvngx_dlss.dll` 直连，还是走 Streamline | `== modules at add-on start ==` |
| `nvngx_dlss.dll` 暴露了哪些 D3D12 入口 | 导出表 dump |
| hook 是否装上 | `hooked NVSDK_NGX_D3D12_EvaluateFeature @ ...` |
| **MV 的真实格式与尺寸** | `MotionVectors : 0x... 2560x1440 fmt=...` |
| 深度的真实格式与尺寸 | `Depth : ...` |
| jitter 是否可读 | `jitter : ...` |
| **相机矩阵是否可拿到** | `matrices : CamViewToClip=... ` |
| DLSS 的输入/输出分辨率 | `CreateFeature` 那几行 |

## 三种可能结果与对策

| 日志特征 | 含义 | 下一步 |
|---|---|---|
| `hooked ...` + 大量 `EvaluateFeature #n` | 成功，直接拿到 MV | 按 dump 出的格式定 M1 的 MV stream 格式，进 M3（深度）|
| 只有 modules，没有 `nvngx_dlss.dll found` | 这游戏只走 Streamline，NGX 在别处加载 | 改 hook `sl.dlss.dll`/`sl.interposer.dll` 的 NGX 包装层 |
| hook 装上但 `eval calls` 一直是 0 | 游戏在别的模块里调用（或已被 Streamline 代理） | 先看导出表与模块列表再定 |
| `matrices` 全为 0 | 相机矩阵只给帧生成侧 | M4 需要从 DLSSG 侧拿，或用 MV+jitter 反推 |

## 回滚方式

在 `bin\x64` 里把 `*.addon64.off` 改回 `*.addon64` 即可；或直接删掉 `m2_probe.addon64`。
