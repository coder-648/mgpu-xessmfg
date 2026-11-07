# M4 颜色链路结果：WGC + HDR，零 GPU 成本

> **先读 `docs/findings/FINAL_CONFIG.md`**：那份是提炼后的最终配置（怎么装、怎么跑、开关全集、踩坑清单）。
> 本文是它的证据链——所有数字、实验和失败过程都在这里。

日期：2026-11-06　探针：`tests/wgc_probe/`（`wgc_probe.exe`）
日志：`temp/m2/wgc_run1_ok.txt`（SDR+回读）、`wgc_run2_nohash_ok.txt`（SDR 零回读）、`wgc_run3_hdr.txt`（HDR 零回读）

---

## 1. 三次实测对比

| 运行 | 帧池格式 | 每帧 GPU→CPU 回读 | 20 s 帧数 | 帧率 | 备注 |
|---|---|---|---|---|---|
| `run_wgc.bat` | B8G8R8A8 | 56 MB（哈希校验） | 787 | 39.3 fps | GPU +5% ← **回读造成的** |
| `run_wgc_nohash.bat` | B8G8R8A8 | 无 | 800 | **40.1 fps** | 性能恢复，节拍更稳 |
| `run_wgc_hdr.bat` | **R16G16B16A16_FLOAT** | 仅 1 行（40 KB） | — | **~40 fps** | HDR 保住，成本可忽略 |

**结论：WGC 颜色通路对游戏 GPU 是零成本。** 之前那 5% 全是探针自己的回读，不是 WGC。

## 2. HDR 验证结果

```
[wgc] pixel format : R16G16B16A16_FLOAT (scRGB, no HDR->SDR conversion)
[wgc] first frame: 5120x2880 fmt=10          ← R16G16B16A16_FLOAT
[wgc] HDR sample: max RGB = 5.172   pixels above 1.0 = 391/5120
[wgc] HDR verdict: real HDR range (scRGB above 1.0)
```

- `max RGB = 5.172` → **真实的 scRGB 高光范围**，不是被压到 0..1 的假 HDR
- 391/5120 像素超过 1.0 → 高光确实存在
- 帧率 ~40 fps，与游戏同步

## 3. 定稿的颜色链路（只有一次编码转换，且在 OS 层）

```
游戏渲染 HDR（内部 FP16）
   ↓
DWM 合成（内部 FP16 scRGB）          ← 跨卡这一步本来就存在（游戏在 NVIDIA、显示在 AMD）
   ↓
WGC 帧池 R16G16B16A16_FLOAT          ← 零转换，直接拿到 scRGB
   ↓
XeSS-FG（AMD）                       ← 颜色不经过共享内存，零传输成本
   ↓
bridge 交换链 FP16 + DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709 (scRGB)
   ↓
OS 做唯一一次 scRGB → HDR10 编码
```

**为什么这条链路特别适合本机**：
1. 游戏在 NVIDIA 渲染、显示接在 AMD 上，DWM 本来就要每帧把画面跨卡搬一次——WGC 复用这份流量；
2. 颜色**不进共享内存**，所以 FP16（5K 下 118 MB/帧）对我们**完全免费**；如果走后台缓冲拷贝，FP16 会把预算直接打爆 4 倍；
3. 全链路只有 OS 上屏前那一次 HDR 编码，没有重复转换。

## 4. 最终规格

| 项 | 规格 |
|---|---|
| 颜色来源 | WGC，游戏窗口（**需无边框/窗口化**，独占全屏不可捕获） |
| 颜色格式 | `R16G16B16A16_FLOAT`（scRGB） |
| 颜色尺寸 | 5120×2880（capture item 尺寸，与窗口客户区 DPI 无关） |
| 捕获设备 | AMD RX 6750 GRE（帧池直接建在副卡上，零拷贝交给 FG） |
| 捕获方式 | `CreateFreeThreaded` + 轮询 `TryGetNextFrame`（不用 WinRT 委托） |
| 光标 | 关闭捕获（`IGraphicsCaptureSession2`） |
| 帧率 | 跟随游戏，实测 39.3–40.1 fps |
| MV / 深度 | 仍走共享内存（2560×1440，28 MB/帧，约 1.5 ms） |

## 5. 遗留的配对问题（M4.2 必须处理）

WGC 拿到的是**已上屏那帧**，比 NGX 给我的 MV/深度晚一帧。若直接配对，运动矢量会指向错误位置 → 插帧拖影。

解法：在 bridge 侧把 MV/深度**延迟一帧**再与颜色配对（环形缓冲，我们完全控制）。
LS 没有这个问题是因为它自己算光流、不依赖游戏 MV。

## 6. 下一步（M4.2）

`bridge.exe`：
1. WGC 抓 FP16 颜色（已在探针里验证）
2. 从共享内存取 MV / 深度（延迟一帧配对）
3. `xefgSwapChainD3D12InitFromSwapChainDesc` 建代理交换链（FP16 + scRGB）
4. `xefgSwapChainD3D12TagFrameResource`：颜色 / MV / 深度
5. `xefgSwapChainTagFrameConstants`：jitter + **近似投影矩阵**（DLSS-SR 不提供矩阵，见 `docs/findings/milestones/M2_RESULTS.md` §4）
6. XeLL：`xellD3D12CreateContext` + 每帧 `xellSleep`
7. `xefgSwapChainSetPresentId` + `Present`

## 7. M4.2 实测结果：链路打通（2x 帧生成生效）

```
[bridge] HDR10 conversion pipeline ready (scRGB -> BT.2020 PQ)
[bridge] SetColorSpace1(HDR10): hr=0x00000000
[bridge] SetEnabled: SUCCESS (0)
[bridge] frame 841  slot 0  jitter=(...)  (34.5 fps)  presented=2 fg=1 SUCCESS
```

`presented=2` + `frameGenResult=SUCCESS` = 每个真实帧生成 1 个插值帧，35 fps 输入 → 约 70 fps 上屏。
`[xefg error]` 计数为 0。画面无闪烁、颜色正常、鼠标键盘可穿透、任务视图只剩一个窗口。

### 7.1 一路踩到的坑（每条都有日志证据）

| # | 现象 | 根因 | 解法 |
|---|---|---|---|
| 1 | `InitFromSwapChainDesc: ERR_LATENCY_REDUCTION_UNSUPPORTED (-15)` | `xefgSwapChainSetLatencyReduction` 调在初始化**之后** | 必须在 `InitFromSwapChain*` 之前调用 |
| 2 | `SetEnabled: -15` | 只 `xellD3D12CreateContext` 建了 context，没**启用** | `xellSetSleepMode(bLowLatencyMode=1)` |
| 3 | `InitFromSwapChainDesc: ERR_INVALID_ARGUMENT (-4)` | 进程非 DPI-aware，窗口客户区被 175% 虚拟化成 2925×1645 | `SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2)` |
| 4 | `CreateSwapChainForHwnd: E_ACCESSDENIED (0x80070005)` ×4 种格式 | 该机器不接受在**我们自己的窗口类 / 顶层弹窗**上建交换链 | 用普通窗口类建交换链（本机用 `STATIC` 类窗口兜底），建好后再改无边框 + topmost |
| 5 | 进程崩在 `GetSwapChainPtr` 前，日志断在 `InitFromSwapChain: SUCCESS` | `pApplicationSwapChain` 传的 `ComPtr` 是块内局部变量，出作用域即释放 → 悬垂指针 | 按文档用 `Detach()` **移交所有权** |
| 6 | 疯狂闪烁 | `FLIP_DISCARD` + BufferCount=2 下永远只写 `GetBuffer(0)`，另一块 buffer 是旧/黑帧 | 用 `GetCurrentBackBufferIndex()` + 每 buffer 独立状态跟踪 |
| 7 | 画面发灰 | #6 的连带：一帧正常一帧错的交替被 FG 当作历史帧 | 同 #6 |
| 8 | 节奏抖动 | XeLL 标记一个都没发 | 每帧发全套 `SIMULATION` / `RENDERSUBMIT` / `PRESENT` 标记 + `xellSleep`，**frame id 与 present id 相同** |
| 9 | `[xefg error] Backbuffer format is not valid. Found 40.` | 40 = `R16G16B16A16_FLOAT`。**XeSS-FG 不支持 FP16 / scRGB** | 交换链改 `R10G10B10A2_UNORM` + `DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020` |
| 10 | `Failed to allocate memory for a copy of DEPTH` + `Missing resource of type XEFG_RES_MOTION_VECTOR` | MV/深度是 `OpenExistingHeapFromFileMapping` 导入堆上的 placed 资源（`ALLOW_CROSS_ADAPTER`），FG 无法拷贝 | 每帧拷到本地 `DEFAULT` heap 的 committed 资源后再 tag |
| 11 | `local input copy FAILED hr=0x80070057` | 复制源 desc 时保留了 `D3D12_TEXTURE_LAYOUT_ROW_MAJOR`（跨适配器专用） | 本地副本改 `D3D12_TEXTURE_LAYOUT_UNKNOWN` |
| 12 | 白屏 + "程序无响应" | 主循环没有消息泵 | 每帧 `PeekMessage` / `DispatchMessage` |
| 13 | 无法操作游戏 | 覆盖窗口抢输入焦点 | `WS_EX_TRANSPARENT` + `WS_EX_NOACTIVATE` + `WS_EX_TOOLWINDOW`，`WM_NCHITTEST→HTTRANSPARENT` |
| 14 | 任务视图出现两个窗口 | 镜像窗口是独立顶层窗口 | `WS_EX_TOOLWINDOW` 从 Alt+Tab / 任务视图隐藏 |

### 7.2 最终链路

```
游戏（NVIDIA 渲染）
  ├─ NGX hook ──> 共享内存（MV 2560×1440 RG16F / 深度 R32，2 槽环形，延迟一帧配对）
  └─ 上屏窗口
        └─ WGC（AMD 设备，free-threaded）──> FP16 scRGB 5120×2880
              └─ 共享纹理（keyed mutex + shared fence）──> D3D12（AMD）
                    ├─ 本地副本：MV / 深度（committed，UNKNOWN layout）──> tag 给 FG
                    └─ PS 转换：scRGB → BT.2020 primaries → PQ → R10G10B10A2
                          └─ XeSS-FG 代理交换链（HDR10，2x）
                                └─ Present → 覆盖窗口（无边框、topmost、点击穿透）
```

### 7.3 已知限制

- **矩阵是近似的**：DLSS-SR 不暴露真实 view/projection（`docs/findings/milestones/M2_RESULTS.md` §4），目前用 60° FOV 硬编码投影 + 单位视图矩阵。快速转视角时边缘可能有轻微重影。
- **UI 没分开处理**：默认参与插值（`UI_MODE_AUTO`）。要做 HUD-less 合成需要游戏侧配合。
- **捕获包含叠加层**：RTSS 等 OSD 画在游戏画面里，会被一起抓进来再显示。
- **`sdrWhiteNits` 固定 80**（scRGB 定义：1.0 = 80 nits），亮度不合适就改这一行。
- **`--no-fg` 开关**：只建代理交换链不启用 FG，用于把颜色/显示问题和插帧问题分开排查。


## 8. 性能剖析：瓶颈是 XeSS-FG 在 5K 上的算力，不是管线

显示器 5120×2880 @ **120Hz**，HDR10 激活。`bridge.exe` 现在按阶段计时（`breakdown:` 行）并用
`Present` 后的 fence 实测真实 GPU 单帧耗时（`gpu=`）。

实测（6750 GRE 12GB，游戏自身约 37fps）：

| 场景 | `gpu=` | `present=` | 主循环 | 上屏（`show`） |
|---|---|---|---|---|
| 5K + FG | **19.6 ms** | 15–31 ms | 16–23 fps | 47–58 |
| 1440p + FG（`--half`） | **3–4 ms** | 0–4 ms | **36.8 fps** | **74–80** |
| 1440p 无 FG（`--no-fg`） | 6–7.5 ms | 0 ms | 36.6 fps | 38 |

### 8.1 结论

- **我们自己的 CPU 工作可以忽略**：`work≈0.5ms`，`capture≈0.0ms`（WGC 从不缺帧）。
- **5K 时 `gpu=19.6ms`**，其中我方的 5K 拷贝（118 MB）+ scRGB→HDR10 转换约 6–7 ms，
  **XeSS-FG 在 5120×2880 的插值约 12–13 ms**。
- **`present` 的 15–16 ms 不是 bug**：FG 每次 Present 提交两帧（真帧 + 插值帧），
  在 120 Hz 上正好是 2 个垂直同步周期。
- 降到 1440p 后 `gpu` 从 19.6 ms 降到 3–4 ms，主循环立即达到游戏自身的 36.8 fps，
  上屏 74–80。**这证明管线本身没有同步问题，瓶颈纯粹是 FG 的像素吞吐。**

```
5K   : 19.6ms GPU + 16.6ms 等垂直同步 = 36ms/帧 → 27fps 输入 → ~54fps 输出
1440p:  4ms GPU  +  0ms 等垂直同步    =  4ms/帧 → 36.8fps 输入(游戏上限) → 78fps 输出
```

### 8.2 与 Lossless Scaling 的差异

LS 的 ~25 ms 是它模型自身的延迟；它在**游戏进程内**替换 Present，因此不受"呈现→抓取"往返
的约束。我们的镜像架构天然多出：

- WGC 必须等一帧上屏后才能抓到（+1 帧，约 27 ms @37fps）
- MV/深度按设计延迟一帧配对（+1 帧）

这是外部进程方案的固有延迟下限，也是 `--no-fg` 时看不出来、开 FG 后才明显的部分。

### 8.3 可用的调参开关

| 开关 | 作用 |
|---|---|
| `--den N` | 输出（以及 FG 插值）分辨率 = 抓取尺寸 / N。N=1 为 5K，N=2 为 1440p |
| `--half` | 等价 `--den 2` |
| `--no-fg` | 只建代理交换链、不插帧（性能基准 + 颜色排查） |
| `--no-inputs` | 不导入跨适配器堆，用零运动矢量的本地假资源（隔离 NVIDIA TDR 用） |
| `--sdr` / `--no-primaries` | 颜色通路对照（SDR 输出 / 跳过 BT.709→BT.2020 矩阵） |

### 8.4 GPU 队列深度

`Present` 之后在命令队列上 `Signal`，下一帧开始前等待它完成（深度 1）。这既把输入延迟压到
一帧，也让 `gpu=` 成为可信的单帧 GPU 时间。

### 8.5 降分辨率输出的三个坑（M4.2c）

**坑 1：窗口不能跟着缩。** 交换链是 2560x1440、窗口也是 2560x1440 时，画面只剩屏幕左上角
1/4。窗口必须始终是**全屏尺寸**（`g_full_w/g_full_h`），让 DXGI 拉伸。

**坑 2：`InitFromSwapChainDesc` 不保留 `DXGI_SCALING_STRETCH`。** 由库内部创建原生交换链时
缩放行为被丢弃，1440p 后缓冲按 1:1 贴在左上角。修法：`--den N`(N>1) 时跳过该入口，自己用
`CreateSwapChainForHwnd` 建（我们的 desc 里 STRETCH 生效），再 `InitFromSwapChain` 移交所有权。

**坑 3：缩小 WGC 帧池是裁剪不是缩放。** 把 `CreateFreeThreaded` 的尺寸设成 2560x1440 时，
WGC 并不做降采样，而是只给我们左上角那 2560x1440 的内容，铺满屏幕后看起来像"严重拉伸"。
正确做法：**抓取保持全尺寸**，降采样放到我们自己的转换 pass 里（那里本来就有一次全屏采样，
5K 源 → 1440p 目标的双线性采样），再用 DXGI 拉到全屏。

最终可用的组合：

```
抓取 5120x2880  →  转换 pass 降采样到 2560x1440  →  FG 在 1440p 插值  →  DXGI 拉伸到全屏
```

实测 build 39 的这个组合：`gpu=3~4ms`、主循环 36.8fps（游戏上限）、`show=74~80`。

### 8.6 `--sharp`（放大锐化）

降分辨率输出会偏软。转换 pass 的 PS 里内置了反锐化掩蔽，开关为 `--sharp <0..2>`（默认 0 = 关闭）：

```hlsl
float3 blur = 0.25 * (四邻域采样);
lin = max(lin + (lin - blur) * sharpen, 0.0);
```

因为它复用已有的那次全屏采样，代价只有 4 次额外纹理取样，在 6750 上可忽略。建议值 0.5~0.8。

---

## 9. 自研插帧 `--fg warp`（原生分辨率，不放大）

### 9.1 为什么要自己写

XeSS-FG 在**交换链分辨率**上插值。5K 时 6750 GRE 要 12-13 ms，输入帧率被压到 ~27 fps；
降到 1440p 又快又好，但画面被放大，白白浪费了 3080 Ti 渲染出的 5K 细节。

Lossless Scaling 的做法是"低分辨率分析运动 + 原始分辨率扭曲"。我们比它多一个优势：
**游戏已经把逐像素运动矢量交给 NGX，我们直接就能读到**，所以连光流分析都不需要，
只剩一次原生分辨率扭曲。整条链路**没有任何放大**。

### 9.2 实现

```
每帧：WGC 5K -> g_hist[wi]（5K FP16 拷贝）
      warp pass：t=0.5 取 prev/cur 两帧双向重投影 -> g_interp（5K FP16）
      convert(g_interp) -> present   ← 中间帧先出，时间顺序正确
      convert(real)     -> present
```

着色器要点（`k_warp_hlsl`）：

```hlsl
float2 mv    = tMv.SampleLevel(smp, uv, 0) * mvScale;   // 游戏给的缩放
float2 pPrev = uv - mv * 0.5;
float2 pCur  = uv + mv * 0.5;
float3 a = prev_at(pPrev), b = cur_at(pCur);
// 用当前帧四邻域夹住重投影历史，抑制遮挡处的拖影
a = lerp(a, clamp(a, min(lo,b), max(hi,b)), clampK);
return float4(lerp(b, a, 0.5), 1.0);
```

MV 缩放来自 `NVSDK_NGX_Parameter_MV_Scale_X/Y`（游戏自己告诉 NGX 的值），由 add-on 写入
`FrameShared.mv_scale_x/y`；取不到时回退到 `1/render_size`（另一种常见约定）。
`SharedControl` 因此升到 version 3。

### 9.3 开关

| 开关 | 作用 |
|---|---|
| `--fg warp` | 用自研插帧替代 XeSS-FG（强制原生分辨率，忽略 `--half/--scale`） |
| `--mv-flip` | MV 取反（方向不对时用） |
| `--mv-mul <f>` | 强制 MV 缩放（覆盖游戏给的值），单位是 UV |
| `--warp-clamp <0..1>` | 邻域夹紧强度，默认 1；0 关闭 |

### 9.5 MV + 像素搜索融合（"两种都用上"）

引擎 MV 只在**不透明几何体**上有效；粒子、雨、烟、水面反射、SSR、贴花都没有 MV。
LSFG 之所以在这些地方更好，是因为它从像素本身估光流。两条路各占优势，所以正确做法
不是二选一，而是**按像素选择**：

```hlsl
// 1) 前后向一致性：在投影落点再采一次 MV，方向应该指回来
float2 mvBack = tMv.SampleLevel(smp, pPrev, 0) * mvScale;
float  conf   = saturate(1.0 - length(mvBack + mv) * confGain);

// 2) conf 低 = MV 不可信 -> 在上一帧里就地做 8 邻域块匹配
if (conf < confOn) {
    for (k in 8 方向) {
        cand = prev_at(pPrev + kOffsets[k] * texel * searchR);
        if (|cand - b| < bestErr) { best = cand; }
    }
    a = best;
    conf = saturate(1.0 - bestErr * confGain * 0.5);   // 匹配好则恢复可信度
}

// 3) 再用当前帧邻域夹紧 + 光度一致性，最后按 conf 混合
return lerp(b, a, 0.5 * conf);
```

几何区域走引擎 MV（精确、免费），无 MV 区域走像素搜索（正是 LSFG 做的事），
两边都不可信时退化成当前帧（保守但不出错）。

| 开关 | 作用 |
|---|---|
| `--warp-conf <f>` | 前后向一致性的灵敏度，默认 90；越大越严格 |
| `--warp-search <f>` | 局部搜索半径（MV 纹素），默认 2；0 关闭像素搜索 |

| | XeSS-FG 1440p | XeSS-FG 5K | **--fg warp 5K** |
|---|---|---|---|
| 输出分辨率 | 1440p（放大到 5K） | 原生 5K | **原生 5K** |
| GPU 单帧 | 2.4 ms | 19.6 ms | 待测（预计 5-9 ms） |
| 输入帧率 | 38 fps | 27 fps | 应为 38 fps |
| 遮挡处质量 | 模型推理，最好 | 同左 | 依赖夹紧，较差 |

---

## 10. XeLL / 缓冲 / 呈现节奏实测（结论修正）

之前的"5K 只有 16-23 fps"是**累计平均帧率**造成的假象（启动阶段拖低），改为按 40 帧窗口
统计瞬时帧率后的真实数据（5120x2880，XeSS-FG，buffers=2，xell mode 1）：

| 帧 | recent fps | gpu | present |
|---|---|---|---|
| 41 | 27.5 | 17.1ms | 16ms |
| 81 | 32.4 | 12.4ms | 15ms |
| 201 | 36.0 | 13.8ms | 16ms |
| 241 | **38.8** | 13.3ms | 16ms |
| 321 | 38.2 | 13.6ms | 0ms |

**5K XeSS-FG 实际能跑到 35-38 fps 输入（= 游戏上限），输出 70-76 fps。** 之前认为"5K 不可用"
是测量方式错误导致的误判。

### 10.1 三个被证伪/证实的假设

| 假设 | 实验 | 结果 |
|---|---|---|
| 交换链缓冲耗尽（2 个缓冲 + 每帧 2 次 Present） | `--buffers 3` | **证伪**：29.5 → 30.3 fps，无差别 |
| `xellSleep()` 配速压帧 | `--no-xell` | **证伪**：present 仍 15-19ms |
| 关掉低延迟模式能去掉 vsync 对齐 | `--xell-mode 0` | **不成立**：`SetEnabled` 直接返回 `ERR_LATENCY_REDUCTION_UNSUPPORTED (-15)`，bLowLatencyMode 必须是 1 |

### 10.2 那 15ms 的 present 是什么

它**不是**我们的 xellSleep 造成的（去掉后仍在），而是 XeSS-FG 内部把插值帧的呈现**对齐到
垂直同步**（120Hz 下约 2 个周期），属于库的固定行为，外部无法关闭。每帧账：

```
gpu 13-19ms（FG 模型，5120x2880）+ present 0-16ms（库内 vsync 对齐）≈ 26-35ms  →  30-38 fps
```

因为 30-38 fps 已经贴着游戏上限（37-38），这个 15ms 基本被游戏自身帧时间掩盖了，
只有在 GPU 端压力小时才表现出来。

**结论：XeLL 没有问题，它是必需的；缓冲数量无关；5K + XeSS-FG 是可用配置。**

### 10.3 修复：`frameRenderTime` 喂错导致 2x 不稳定

XeSS-FG 的 pacing 模型吃 `frameRenderTime`。我们之前传的是**自己主循环的间隔**
（约 30ms，里面含它自己 15ms 的 vsync 等待），而它要的是**游戏渲染一帧的时间**（约 25ms）。
喂大了它认为没有余力 -> 时不时跳过插值 -> 用户看到"5K 下无法稳定 2x"。

修法：在 WGC 出帧处单独测量游戏帧间隔，用它当 `frameRenderTime`。

```cpp
// 在 TryGetNextFrame 成功处
if (g_last_arrival) {
    const double dt = GetTickCount64() - g_last_arrival;
    if (dt > 0.0 && dt < 1000.0) { g_arrival_ms_ema = g_arrival_ms_ema * 0.9 + dt * 0.1; }
}
cd.frameRenderTime = static_cast<float>(g_arrival_ms_ema);
```

同时新增 `x2=` 指标（每个报告窗口内 `framesPresented` 的平均值），把"2x 是否稳定"变成可观测的数字。
（2026-11-06 之后这个字段改名为 `xN`，因为倍数不再固定为 2x，见 `docs/findings/MFG.md`。）

### 10.4 修复后的实测（两项都到 x2=2.00）

| 命令 | 输出 | `x2` | 输入 fps | `gpu` | `game` | 上屏 |
|---|---|---|---|---|---|---|
| `--den 1` | 5120x2880 | **2.00** | 37-40 | 12-18ms | 24-26ms | 73-81 |
| `--scale 0.75` | 3840x2160 | **2.00** | 35-40 | **5-14ms** | 24-26ms | 73-85 |

**5K 原生 + XeSS-FG 现在能稳定 2x，输出 80fps。** 游戏自身帧时间 24.5ms（约 40fps 上限）
成为唯一限制。`--scale 0.75` 的 GPU 余量约为前者的两倍，在重负载场景更抗掉帧。

### 10.5 present 阻塞锁定为 XeSS-FG 内部行为（无法消除）

| 实验 | present | load | 结论 |
|---|---|---|---|
| `--den 1 --latency 2` | 仍 10-31ms | 85-101% | 放宽帧延迟无效 |
| `--den 1 --tearing` | 仍 15-31ms（flags=0x800 生效） | 86-100% | 撕裂也不解除 |
| `--scale 0.8` | 0-16ms | 24-102%，中位 70% | 边际 |
| `--scale 0.75` | 0-16ms | 中位 ~60% | 舒适 |

**`present` 的 10-16ms 是 XeSS-FG 内部对齐垂直同步的行为**，与 DXGI 帧延迟上限、撕裂标志都无关。
它决定了每帧预算的划分：

```
输入 25ms（40fps 锁帧） - present 14ms = 只剩 11ms 给 FG + 我们的拷贝
5K    FG 13-14ms + 拷贝 3-4ms = 17ms  > 11ms   → 必然丢帧（load≈100%）
0.8   FG  8-10ms + 拷贝 3-4ms = 12ms  ≈ 11ms   → 边际
0.75  FG  5-8ms  + 拷贝 3-4ms =  9ms  < 11ms   → 稳定
```

### 10.6 最终配置

| 目标 | 配置 | 实测 |
|---|---|---|
| **推荐日常**：稳定 80fps + 近乎原生清晰度 | `--scale 0.75` | recent 38-41，x2=2.00，load ~60%，上屏 76-82 |
| 极限清晰度（牺牲插帧质量） | `--fg warp` | 原生 5K，recent 37-38，gpu 2-11ms |
| 极致帧率 | `--den 2` | recent 38，gpu 2.4ms |
| **不可行** | `--den 1`（XeSS-FG @5K） | present+FG 超过输入预算，必然丢帧 |

5K + XeSS-FG 在 6750 GRE 上受限于库内部的 present 对齐，没有任何外部开关能解决。
想在 5K 上拿到 80fps，只能用自研的 `--fg warp`——那条路径的 Present 完全由我们控制。

### 10.7 决定性修复：XeLL 的"标记"才是 XeFG 的节奏来源

参考 OptiScaler 的实现（`low_latency_tech/ll_xell.cpp`、`input_common.cpp`
的 `acquire_xefg_xell`）：

```cpp
void XeLL::xell_sleep(uint32_t frame_id) {
    if (!markers_only && (!forced_mode || is_enabled()))     // markers_only 时【不睡】
        o_xellSleep(xell_context, frame_id);
}
void XeLL::add_marker(uint32_t frame_id, marker_type_t t) {
    if (markers_only || !forced_mode || is_enabled())        // 但【标记永远发】
        o_xellAddMarkerData(xell_context, frame_id, t);
}
// 给 XeFG 的专用上下文：
xell->set_markers_only(true);
xell->set_forced_mode(true);
sleepMode.low_latency_enabled = true;
xell->set_sleep_mode(&sleepMode);
```

**XeFG 需要的是每帧的标记（SIMULATION / RENDERSUBMIT / PRESENT），不是睡眠。**
睡眠在非 Intel 卡上本来就是空转（"No sleep algorithm available."），所以它不该被当作
必需项；而标记一旦缺失，XeFG 只能退回最保守的呈现行为——表现为"帧率数字完美但看着卡"。

我们犯过的错误：把"跳过每帧 XeLL 调用"实现成了同时跳过标记，且设为默认。
修正后（标记永远发、只关睡眠）**系统性顿挫消失**。

对应到 OptiScaler 的术语：我们要的就是 `markers_only = true` + `forced_mode = true`。

### 10.8 最终配置（实测顺滑）

```
bridge.exe --den 2 --pool 4          ← run_mgpu.bat 已固化为默认
```

关键的两条经验：

1. **不要用 `--pace`**：XeSS-FG 自己按标记配速，且 `Present()` 内部要等约 15ms 垂直同步。
   我们再钉一次时间会让两者串行（`gpu` 从 2ms 涨到 17ms，`load` 到 89%，掉到 50Hz）。
2. **帧池必须"排空取最新"**：`TryGetNextFrame` 返回最旧的帧，配上 `--pool 4` 就等于把
   池子深度变成延迟（最多 +100ms），而且池子积压会让 WGC 丢帧（输入 37-39 而不是 40，
   输出自然到不了 80）。改成排空后：`gpu` 5.9→2.1ms，输入回到 37-40，输出稳定在 76-80。

实测（2560x1440 插值，40fps 输入）：

| 指标 | 值 |
|---|---|
| `gpu` / `total` | 2.1-2.7ms / 6.5-7.3ms |
| `load` | 27-37% |
| `x2` / `x2miss` | 2.00 / 0 |
| 上屏 | 76-80 fps |

剩余的延迟构成（每项约 25ms @40fps）：WGC 抓取 1 帧、MV/深度配对 1 帧、
XeFG 模型 ~25ms、GPU 队列 1 帧。其中"MV 配对延迟一帧"是我们可以选择去掉的。

### 10.9 延迟实测与两项优化

**WGC 的"+1 帧"其实只有 3–6ms。** 新增 `age=` 指标直接测量"拿到这一帧时它已多旧"：

```
age=3.2ms / 4.7ms / 5.9ms / 8.3ms        （离群值 94ms 对应一次卡顿）
```

因为 DWM 在 120Hz 的合成节拍上取用画面（最多 8.3ms），不是整整一个游戏帧（25ms）。
**所以"进程内挂钩 Present 自己拷后备缓冲"这条路不值得走**：只省约 4ms，却要把 59MB/帧
跨卡传输（PCIe 点对点正是导致 NVIDIA TDR 的路径），风险远大于收益。

**MV/深度配对延迟可以省。** 新增 `--mv-newest`：比较两个槽位的 fence 完成值，取更新的
那个，而不是刻意延迟一帧配对。省约 25ms，代价是运动矢量可能比颜色新半帧。

### 10.10 5K 原生也能跑起来（RTSS 30 + `--wait-ms 0`）

5K 实测：`present` 12.7-15.1ms + `gpu` 4-20ms，每帧约需 31ms。40fps（25ms 预算）装不下，
但把游戏锁到 30fps（33.3ms）之后：

```
run_mgpu_5k.bat          # --den 1 --pool 4 --mv-newest --wait-ms 0
```

| 指标 | 实测 |
|---|---|
| 输入 / 输出 | 28-30 fps / **56-61 fps** |
| `skip` | **0-2**（几乎不丢帧，之前是 11-17） |
| `wait` | **0.0ms**（`--wait-ms 0` 干掉了那个"因为落后才出现"的 MV 等待） |
| `HITCH` | 1 次 / 30 窗口 |
| `present` / `gpu` | 12.7-15.1 / 4.2-20.0ms |
| `load` | 58-100%（峰值仍贴顶，但 `skip` 才是真相指标） |

**关键判据从 `load` 换成 `skip`**：`load` 峰值贴顶只是瞬时，`skip` 才表示真的丢帧。




