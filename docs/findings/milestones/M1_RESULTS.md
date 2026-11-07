# M1 结果：跨卡传输层（可复用组件）

日期：2026-11-05　代码：`src/cross_adapter_mailbox.h`（header-only）　验证：`tests/m1_transport/`
原始输出：`temp/m1_run.txt`

---

## 1. 组件

`mgpu::CrossAdapterMailbox`

```cpp
std::vector<StreamSpec> streams = {
    { "color", 5120, 2880, DXGI_FORMAT_R10G10B10A2_UNORM },
    { "mv",    2560, 1440, DXGI_FORMAT_R16G16_FLOAT    },
    { "depth", 2560, 1440, DXGI_FORMAT_R16_UNORM       },
};
CrossAdapterMailbox m;
m.create(nvDevice, amdDevice, streams, /*slots=*/2, &err);

// 每帧（都不阻塞 CPU）：
m.publish(frame);     // 生产卡：等槽位释放 → 拷 source→共享槽 → 信号 forward fence
m.consume(frame);     // 副卡：  等 forward fence → 拷 共享槽→target → 信号 back fence
```

- `producer_source(i)`：游戏（或采集路径）写入的 VRAM 纹理
- `consumer_target(i)`：XeSS-FG 直接 tag 的 VRAM 纹理
- 槽位 = `frame % slots`，每个槽位**独立**的 fence + allocator + command list

## 2. 验证结果

| 阶段 | 内容 | 结果 |
|---|---|---|
| 建邮箱 | 3 流 × 2 槽，一张 154.69 MB 的文件映射 | ✅ 77.34 MB/帧 |
| phase 1 | 12 帧，每帧色彩首像素写入帧号，跨卡后读回比对 | ✅ **12/12 精确匹配**，槽位 0/1 正常轮转 |
| phase 2 | 120 帧按 40fps 定速、不阻塞 CPU | ✅ 39.9 fps，**0 失败**，CPU 提交 0.32ms 均 / 0.61ms 峰 |

phase 2 是在**赛博朋克仍在运行**（NVIDIA 3D 40%+copy 18%）的状态下跑的，属于真实负载条件。

**关键结论：CPU 侧提交成本 ~0.3ms/帧，可忽略**。瓶颈完全在 PCIe 带宽，与 M0.3 的测量一致。

## 3. 踩到的两个 API 坑（已写成注释）

1. **`D3D12_HEAP_TYPE_UPLOAD` / `READBACK` 堆不能放 `R10G10B10A2_UNORM` 纹理**（返回 `E_INVALIDARG`）。
   正确做法：用 **BUFFER + `D3D12_PLACED_SUBRESOURCE_FOOTPRINT`**，配 `CopyTextureRegion`。
   （`GetCopyableFootprints` 先按纹理描述算 pitch/offset，再建同样大小的缓冲区。）
2. `D3D12_PLACED_SUBRESOURCE_FOOTPRINT` 的偏移是 **`fp.Offset`**，不在 `fp.Footprint.Offset` 里。

## 4. 与 addon 的接入点

| addon 环节 | 接到哪里 |
|---|---|
| ReShade `present` 事件 | `publish(frame)` |
| MV 截取（M2） | 结果写进 `producer_source(1)` |
| 深度采集（M3） | 结果写进 `producer_source(2)`（tap shader 直接输出 R16_UNORM） |
| 副卡 FG（M4） | `consumer_target(0..2)` 直接传给 `xefgSwapChainD3D12TagFrameResource` |

## 5. 尚未覆盖（M1 的后续增量）

- **present 线程与 copy queue 的时序**：现在 publish/consume 由测试主线程调用；接进 addon 后要在 ReShade 的 present 回调里调用，需要确认不与游戏自己的队列冲突。
- **resize 处理**：分辨率变化时要重建邮箱（目前只支持创建时固定尺寸）。
- **丢帧策略**：当 consumer 落后超过 slots 时，是丢帧还是阻塞，需要在 M4 与 XeLL 一起定。
