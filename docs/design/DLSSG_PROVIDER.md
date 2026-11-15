# 为什么 sm86 不需要 ReShade 也能拿到 FG 数据（2026-11-11 调查）

## 1. 它的结构

```text
E:\PRAGMATA\
  nvngx_dlssg.dll            7,607,336   ← 被替换的 DLSS-G 提供者（不是原版）
  nvngx_dlssg.dll.original   7,602,720   ← 原版备份
  dlssg_sm86.ini             Enabled=1  Engine=XeSS
  dlssg_sm86/runtime/nvngx_dlssg.dll     ← 它自己的运行时
  dlssg_sm86/{backend,kernels,sr,xefg,logs}
```

`strings runtime/nvngx_dlssg.dll` 里能看到 NGX 310.9 的 `dlssg2` 源码路径
（`.../NGX/snippets/rel_310_9/source/features/dlssg2/...`）和它自己的 `sm86` 标记。
**没有任何 dxgi / swapchain / Present 钩子。**

## 2. 它靠什么拿到数据

```text
它自己就是 DLSS-G 提供者（nvngx_dlssg.dll）。
引擎调用 NVSDK_NGX_D3D12_CreateFeature / EvaluateFeature 时，
参数块里就带着一切：DLSSG.MVecs / Depth / HUDLess / UI / CameraViewToClip /
ClipToPrevClip / JitterOffset / MvecScale / Backbuffer / OutputInterpolated …
以及一条【打开的命令表】和 output 资源。

生成帧也在这里完成（Engine=XeSS 就用 XeSS-FG 的模型；Engine=DLSS 用他们的 sm86 kernel）。
不生成 = 交回输入 → 游戏自然呈现 1x（这就是"打断"，而且不违反任何契约）。
```

## 3. 这解释了我们踩过的所有坑

| 我们踩的坑 | sm86 为什么不会遇到 |
|---|---|
| 记录到游戏那条【已关闭】的命令表 → GPU 崩溃 | 它在 `EvaluateFeature` 里拿到的命令表是**打开的** |
| 需要 swapchain → 只能去钩 DXGI/Present（且钩不到，因为 Streamline 抢先） | 它**不需要 swapchain**：颜色/深度/MV 都从参数块来 |
| 状态要靠猜（tag 声明 vs 观察到的 barrier） | 参数块里的资源就是这一帧要用的，直接用，无需状态推导 |
| "打断"只能靠 FgCut 短路 → 游戏判定 FG 失败 | 它作为提供者返回成功（不写插值帧），FG 契约始终成立 |
| 签名校验（我们为此退休了 dlssg 文件代理） | CP2077/PRAGMATA 这条路径上被替换的模块能工作（sm86 就是这么部署的） |

## 4. 我们接下来应该做的（新主线）

```text
把 "nvngx_dlssg.dll 提供者" 作为主实现，而不是继续在自有宿主里钩 DXGI：

S1  用 src/fg_stubs/retired/dlssg/dlssg_stub.cpp 重新做一个提供者
      · 导出全部 NVSDK_NGX_D3D12_* 与 DLSS-G 相关入口
      · CreateFeature / EvaluateFeature 里解析参数块（in_fg.cpp 已有完整解析代码可复用）
      · 每条流：MV / depth / HUDless / UI 直接拷到我们的共享槽位
        —— 记录在 EvaluateFeature 给的【打开的命令表】上，这是合法位置
      · 不写 OutputInterpolated（= 不生成）→ 游戏呈现 1x，FG 契约成立
S2  桥侧：从共享槽位取 MV+颜色（现在都有原生数据），在副卡补帧
S3  若要"由我们生成"：把插值帧写回 OutputInterpolated（需要和游戏交换链格式一致）
```

### 这条路线相对现在的好处

```text
· 不再需要 Present / 交换链 / DXGI 的一切（本轮所有崩溃来源都消失）
· 颜色不是后台缓冲而是 HUDless（参数块直接给），UI 也有独立参数
· 命令表是打开的、资源是本帧的、无需状态推导 → 三条 ReShade 纪律自然满足
· 我们已有的 in_fg.cpp 解析（NGX DLSS-G 参数）可直接搬过去当实现
```

## 5. 统一：既有后端既是"打断者"，也是"记录者"（2026-11-11 定案）

我们为每条 FG 管线本来就写过后端（`src/fg_stubs/`）：

```text
fsr/framegen_stub.cpp     amd_fidelityfx_framegeneration_dx12.dll —— 吞掉 ffxConfigure，
                          对 FRAMEGENERATION 的 ffxDispatch 回成功而不生成（CP2077 上验证过 2.00x/0 丢帧）
xess/xefg_stub.cpp        libxess_fg.dll —— 只有 provider 形状，从不转给真库
retired/dlssg/            nvngx_dlssg.dll —— 曾因签名校验退休；sm86 证明这条路在 CP2077 上可行
```

既然后端**本来就在 API 入口上**（它接收 dispatch / evaluate 的全部参数和命令表），
那它当然可以同时做记录 —— 而不需要再用 MinHook 去钩真实运行时：

```text
调用方（游戏） → 我们的后端（.dll）
   参数：MV / depth / HUDless / UI / 相机矩阵 / jitter / mv scale / backbuffer / output
   命令表：调用方传进来的那条【打开】的 list
   我们：① 拷贝四条流进共享槽位（就在这条 list 上）
         ② 回"成功"但不写 output（= 打断，契约成立）
         ③ 发布 fence，桥在副卡补帧
```

### 相对“钩真实运行时”的好处

| 项 | 钩真实运行时（本轮实现） | 后端即记录者 |
|---|---|---|
| 数据位置 | 需要钩 NGX/SL/FFX 导出 | 后端本来就在入口，直接拿参数 |
| 命令表 | 同样拿到（EvaluateFeature 的 list） | 同样拿到 |
| 打断 | 需要额外机制（FgCut 短路，游戏会判失败） | 天生成立：我们从不生成，但返回成功 |
| 每个管线一致性 | 四条车道四套钩子 | 四套后端同一个骨架 |
| 崩溃风险 | 钩子 + 状态推导 | 无钩子、无状态推导（资源就是本帧要用的） |

### 结论

```text
后端 = 打断者 + 记录者 + （可选）生成者。
本轮"在 EvaluateFeature 里记录"是同一件事的最小实现（用的是真实运行时）；
下一步把它搬进我们自己的后端，就同时得到了打断与记录，且不再需要任何导出钩子。
```
