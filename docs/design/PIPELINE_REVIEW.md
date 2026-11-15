# 整链路梳理与性能优化点（2026-11-11，CP2077 实测后）

## 1. 现在的链路（已验证：游戏内 FG 正常、OSD 正常、0 崩溃）

```text
[主卡 RTX 3080 Ti]  游戏渲染（DLSS SR 开、游戏内 FG 开）
      │  slSetTag / slSetConstants（带命令表与资源）
      ▼
  mgpu_capture.dll（自有宿主，无 ReShade）
      · 包装 device/queue/list（barrier 观测）
      · tag 时刻：用 tag 声明的状态做 barrier → 1:1 CopyTextureRegion 进跨卡槽位
          MV    3840x2160 RG16F   读 31.6 + 写 31.6 MB
          depth 3840x2160 R32     读 31.6 + 写 31.6 MB
      · 发布：fence Signal + FrameShared（jitter/mvScale/camera/frame_id）
      ▼  PCIe（PCIe 4.0 x8 实测约 13 GB/s 可用）
[副卡 RX 6750 GRE]  bridge.exe
      · 等槽位 → 槽位 63.3 MB 拷进本地 ring
      · 颜色：WGC（窗口捕获 → D3D11 共享纹理 → D3D12 copy）
      · XeFG 打 tag：MV / depth / colour，生成 2x
      · present + OSD
```

实测（27.9 fps 输入 → 2x 输出）：

```text
bus        63.3 MB/帧 = 1.8 GB/s（PCIe 4.0 x8 的 ~14%）
bridge     work 0.3ms  gpu 4.0ms  present 4.9ms  total 5.2ms  load 25%
producer   拷贝 ≈126 MB 本地流量/帧 ≈ 3.5 GB/s（3080 Ti 的 ~0.5%）
```

## 2. 性能优化点（按"能省多少 / 代价"排序）

| # | 位置 | 优化 | 收益 | 代价/风险 |
|---|---|---|---|---|
| P1 | 主卡 | **拷贝搬到 copy 队列**（我们已有 `OffloadQueue` 基础设施） | 主卡图形队列不再承担拷贝；4K 下约 0.1–0.3 ms/帧 | 需要跨队列同步（游戏队列 → copy 队列 wait），实现中等 |
| P2 | 主卡 | `StreamPercent=150`（流尺寸减半） | 126 → ~32 MB/帧 | **精度下降**（与"全精度"目标冲突，仅作带宽实验） |
| P3 | 主卡 | 关 depth（`SkipDepth=1`） | 省 31.6 MB 写 + 31.6 MB 读 | XeFG 失去去遮挡判断，画质下降 |
| P4 | 主卡 | barrier 由"每流 2 次"减为 1 次（拷完不还原，下一个 tag 前再还原） | 少一半转换 | 若游戏在中间用该资源会出错，风险高 |
| P5 | 副卡 | 槽位 → 本地 ring 的 63.3 MB 拷贝改为**直接采样跨卡资源** | 副卡省 63 读 + 63 写 | XeFG 多次采样会放大 PCIe 读，可能更慢（测量需 A/B） |
| P6 | 副卡 | WGC 颜色换成"共享链路颜色" | 去掉 D3D11 捕获与 keyed mutex 跳 | 需要颜色流（见 §3） |
| P7 | 副卡 | `present 4.9ms` | 这是 vblank 节拍等待，不是浪费 | 无 |

**结论**：主卡侧的拷贝流量只有 0.5% 带宽，实测占用没变也印证了这一点 —— 真正的优化空间在 **P1（换队列）**，其余都是画质/带宽取舍。

## 3. color 用哪个方案更好（三条路对比）

| 方案 | 数据来源 | 主卡成本 | 副卡成本 | UI 正确性 | 现状 |
|---|---|---|---|---|---|
| **A. WGC**（现在用） | 窗口捕获 | 0 | D3D11 捕获 + 1 帧延迟 + keyed mutex | UI 已烘焙进颜色 → 插值帧里 UI 会糊 | ✅ 已跑通 |
| **B. tag HUDless + UI**（OptiScaler 模型） | `slSetTag` type 2 / 23 | +66 MB/帧（两条 4K R10G10B10A2 流） | 一次跨卡读 | **XeFG 可正确合成 UI**（插值帧 UI 不糊） | 槽位已实现，tag 路径只差验证 |
| **C. Present 图（我们自己的交换链包装）** | 对象级 vtable 补丁后的 Present | 0 | 一次跨卡 | UI 已烘焙（同 A） | 曾崩溃；对象级补丁在 CP2077 上拦不到（sl.dlss_g 抢先） |

### 建议

```text
画质优先 → B：HUDless（pre-UI）+ UI 分开传，XeFG 自己合成 UI。
            代价是 +66 MB/帧（30 fps 下 +2 GB/s），PCIe 4.0 x8 仍只到 ~30%。
成本优先 → A：WGC，主卡零成本，但插值帧的 UI 会有拖影。

混合     → A + B 的"有就用"：tag 有 HUDless/UI 就切 B，没有就回落 A（我们已有 fallback 结构）。
```

## 4. 下一步建议顺序

```text
1) 先验证 B 是否真的可用（Color=1 Ui=1，只跑采集不接桥，看 [color]/[ui] copy 行）
2) 若 B 成立 → 桥加一条"颜色从共享链路来"，A/B 同场景对比画质与总线占用
3) P1（copy 队列）作为主卡侧的收尾优化，排在功能之后
```
