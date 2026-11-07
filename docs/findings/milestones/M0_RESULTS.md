# M0 结果报告：能力验证与卡间传输实测

日期：2026-11-05　程序：`tests/m0_probe/`（M0.1）、`tests/m0b_transport/`（M0.2）
构建：`./scripts/build_m0.sh <module>`（Windows 侧 cl.exe 14.44.35207 / Windows SDK 10.0.26100）
原始输出：`temp/m0_probe_run_stdout.txt`、`temp/m0b_run_stdout.txt`

---

## M0.1 XeSS-FG / XeLL 能力验证

| 检查项 | RTX 3080 Ti (0x10DE) | RX 6750 GRE (0x1002) |
|---|---|---|
| D3D12 设备 | OK | OK |
| 最高 Shader Model | 6.7 | 6.7 |
| `xellD3D12CreateContext` | **SUCCESS** | **SUCCESS** |
| `xefgSwapChainD3D12CreateContext` | **SUCCESS** | **SUCCESS** |
| `xefgSwapChainSetLatencyReduction` | SUCCESS | SUCCESS |
| `maxSupportedInterpolations` | **1** | **1** |
| `CrossAdapterRowMajorTextureSupported` | **FALSE** | **FALSE** |
| libxess_fg / libxell | 1.3.1 / 1.3.2 | 同 |

**结论**
1. **总闸门通过**：XeSS-FG 上下文在两块卡上都能创建，AMD 副卡跑 FG 在软件层面成立。
2. `maxSupportedInterpolations = 1` 实测确认：**当前 SDK 上限就是 2x**。3x（5K40→120）确实需要社区解锁版 `xefgSwapChainSetNumInterpolatedFrames`。
3. `CrossAdapterRowMajorTextureSupported=FALSE` 一度看起来是致命伤，**M0.2 证明它不影响我们**。

## M0.2 卡间传输实测（payload 5120×2880 RGBA8 = 56.2 MB/帧）

| 变体 | 路径 | 结果 |
|---|---|---|
| V1 | 共享纹理，生产卡 VRAM，无 sim-access | CreateSharedHandle OK / **跨卡 OpenSharedHandle 失败** (E_INVALIDARG) |
| V2 | 同上 + `ALLOW_SIMULTANEOUS_ACCESS` | 同上失败 |
| V3 | 共享纹理，custom heap L0（系统内存） | `CreateCommittedResource` 失败（CPU 可见堆不允许 `D3D12_HEAP_FLAG_SHARED`） |
| V4 | V3 + sim-access | 同上失败 |
| V5 | CPU 中转（VRAM→readback→memcpy→upload→VRAM） | 可用，**20.9 ms/帧，5.4 GB/s** |
| **V6** | **file mapping 建跨适配器堆 + 线性 BUFFER** | **ZERO-COPY 生效，12.24 ms/帧（双向 112.5 MB），9.19 GB/s** |
| **V7** | **file mapping 跨适配器堆 + ROW_MAJOR 纹理** | **两块卡都能放置成功 → 共享同一份像素** |

### 关键结论

1. **传输方案定为 file-mapping 跨适配器堆**（`CreateFileMappingW` + `ID3D12Device3::OpenExistingHeapFromFileMapping` 在两块卡上各建一个堆视图，指向同一段系统内存）。
   - 不需要 `D3D12_HEAP_FLAG_SHARED` 的普通共享纹理（跨厂商不可用）。
   - 不需要 CPU memcpy（V5 的 20.9ms 被 V6 的 12.24ms 取代）。
   - 资源必须带 `D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER`（验证层明确要求）。
2. **V7 比 V6 更进一步**：连纹理都能直接放在共享堆里，理想情况下根本不用"拷贝"，两边各自 `texture→本地` 即可。M1 应先按 V7 做，失败再退 V6 的线性缓冲 + footprint 拷贝。
3. 实测吞吐 ≈ **9.2 GB/s**（符合 Gen4 ×8 的物理预期，也印证了立项时的判断（双卡 PCIe 4.0×8））。

### 延迟预算换算

| 场景 | 每帧过总线 | 传输耗时 @9.2GB/s | 80 fps 预算 12.5ms |
|---|---|---|---|
| 5K 颜色 56.2MB 单向 | 56.2 MB | ≈ 6.1 ms | 占一半 |
| 5K 颜色 + MV/深度（渲染分辨率 1440p，各 ~14MB） | ≈ 84 MB | ≈ 9.1 ms | 只剩 3.4ms |
| 若 MV/深度也按 5K | ≈ 168 MB | ≈ 18.3 ms | **超预算** |

**因此架构必须满足两条硬约束**
1. **运动矢量与深度按渲染分辨率传输**（不是输出分辨率）——这也正好符合 XeSS-FG 指南"MVs 建议低分辨率、depth 尺寸必须等于 MV"。
2. **传输必须流水线化**：单帧 6–9ms 不能串行阻塞渲染。要双缓冲/三缓冲 + 共享 fence 跨卡同步（`ID3D12Fence` 用 `D3D12_FENCE_FLAG_SHARED` 建共享句柄），让"拷第 N 帧"与"渲染第 N+1 帧"重叠。

## 下一步（M1）

| 步骤 | 内容 |
|---|---|
| M1.1 | 封装 `CrossAdapterMailbox`：file-mapping 堆 + 双卡放置资源 + 共享 fence 双向同步 |
| M1.2 | 纹理直放版本（V7 路线）：生产卡写 5K 颜色纹理，消费卡直接读，测往返延迟 |
| M1.3 | 跨卡 fence 同步正确性验证（避免撕裂 / 读到半帧） |
| M1.4 | 加入 3 路载荷（颜色 5K + MV/depth 1440p），测量稳态 40fps 下的帧间开销 |

## 环境更正

- 双卡实际 **PCIe 4.0 ×8**（实测 9.2 GB/s 与 x8 吻合；PnP 报的 AMD ×16 不可信）。
- 主屏是 **AMD 5K**（`\\.\DISPLAY1`，逻辑分辨率 2926×1646 是 175% DPI 缩放的结果），副屏 NVIDIA 4K（`\\.\DISPLAY8`）——之前报告里"主屏在 NVIDIA"的推断是错的。
- **MSVC 存在**：`C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools`（MSVC 14.44.35207）。之前报告写"无编译器"是探测路径遗漏。
- 先例项目 `玄学双显卡1.7版` **不再作为参考**（用户明确其存在错误），仅其 `mgpu_depth_tap.fx` 里关于 ReShade 深度语义的经验值可作背景知识。
