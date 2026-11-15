# 传输 V2：全精度、端到端跨卡、严格帧同步（自有宿主）

前提：**不降精度**（禁止半分辨率 MV、禁止 8-bit 量化、depth 保持 32 位）。
目标：MV + depth + HUDless + UI + else 全部通过共享机制直达第二块 GPU，
帧同步是协议保证的（不是"通常对"），同时把现在 340 MB/帧的浪费挤掉。

## 1. 结论先行

```text
不降精度也能快，因为现在的开销 2/3 不是"数据本身"，而是：
  ① 消费者每帧把整段槽位再拷一份到本地（162 MB/帧，白拷）
  ② MV 走着色器 pass（66 MB 读 + 66 MB 写，而它是纯拷贝，拷贝引擎更快）
  ③ depth 先整幅拷进 typeless 中转再 pass（多 29.5 读 + 29.5 写）
  ④ 两个 pass / 六次状态转换（每次转换都可能是 cache flush 停顿）
去掉这四项，全精度下生产者本地流量从 178 → ~110 MB/帧，且全部在拷贝引擎/单 pass 上；
跨卡（PCIe）只传"数据本身"一次：MV 66 + depth 15 + HUDless 33 ≈ 114 MB/帧。
```

## 2. 传输结构：一个跨卡 heap，槽位里允许"多流打包"

现状已经正确（`D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER` 的 place 资源 + 每槽前后向跨卡 fence），
V2 只是在槽位内**按子资源打包**，让一次 barrier 覆盖多流：

```text
slot k (3840x2160 capacity, 4 深度流水)
  ├─ MV      R16G16_FLOAT    全分辨率（= 游戏 MV 原样，位级相同）
  ├─ depth   R32_FLOAT       全分辨率（SRV 读 D32S8X24 的值，只丢掉无用的 stencil 平面）
  ├─ hudless R10G10B10A2     全分辨率（仅在消费者要 Color 时才占位，字节数=0 否则）
  └─ ui      B8G8R8A8 / R8   见 §6，需要先确认 XeFG 的 UI 输入是否接受降采样
帧级元数据（FrameShared，几百字节）：jitter、mv_scale、render_w/h、view_to_clip、
  clip_to_prev_clip、near/far/vfov、camera pos/up/right/fwd、pinhole、frame_id/key、
  present_id、QPC、semantics、valid_w/h、fg_flags、mv_invalid_value
```

关键点：**元数据本来就是共享内存里的几百字节，成本为 0** —— "丰富数据"这一侧我们已经不缺，
缺的是把纹理流搬到"位级相同 + 零额外拷贝"。

## 3. 生产者路径（游戏 GPU）：两个引擎各干最擅长的活

```text
[拷贝引擎 / 主 list 上的 CopyTextureRegion]
  1. MV      : 源 → slot          （格式/尺寸相同，纯搬运，不经着色器，位级相同）
  2. HUDless : 源 → slot          （同上；Color=1 时才做）

[一个 compute dispatch（只做必须计算的转换）]
  3. depth   : SRV R32_FLOAT_X8X24_TYPELESS 读 → UAV R32_FLOAT 写全分辨率
               （8B/px → 4B/px，只丢 stencil 平面 = 不丢深度精度）
  4. ui      : 若需要颜色+alpha 则整幅拷；若只是掩码，则 1/4 分辨率 R8 一次生成

[barrier]  总共 2 次（进入 COPY_SOURCE/SRV，退出回原状态），而不是现在的 6 次
[发布]     填 FrameShared → 等 consumed[k] 允许后写入 → Signal(forward fence)
```

为什么把 MV 交给拷贝引擎：现在的 `record_downsample` 是为了**解决 swizzle**（PBZ 上直接拷出来是垃圾），
一旦确认某条车道是 row-major（ngx 车道出现 "swizzled MV 直接拷是垃圾" 的记录就要保留 pass），
就走拷贝引擎；判据是 `probe` 行的 MV 分布 + `--dump` 采样。

## 4. 消费者路径（第二 GPU）：一次拷进本地 ring，之后全读本地

```text
wait slot k forward fence（事件，已实现）
  → 拷贝队列：CopyTextureRegion slot k → local ring[k]（一次，异步，与上一帧的 FG 计算重叠）
  → Signal(back fence, published 值)  ← 消费者用 FrameShared.published 回 ACK
  → XeFG 只读 local ring[k]（本地显存，多 pass 采样不再打到 PCIe）
```

**这是"既直连又不被 PCIe 拖死"的关键**：共享资源负责*传输*，本地 ring 负责*多次采样*。
直接让 FG 跨卡采样会把 114 MB/帧放大成 300+ MB/帧（FG 内部多 pass 各读一遍），得不偿失。

## 5. 严格帧同步协议（现在的骨架已经够，补三条）

```text
已有：每槽 forward/back 跨卡 fence；FrameShared.published 作为可回传的发布号；
      consumed[k] 由消费者写、生产者读；生产者不覆盖未 ACK 的槽（满了就丢帧并计数 overrun）
补 ①：生产者写 FrameShared 用 seq 奇偶（写前 +1 归奇，写后 +1 归偶），消费者读到奇数就重读
补 ②：消费者丢弃 frame_id 非单调的槽（换了 slot 顺序也不会错配）
补 ③：HUDless / UI / MV / depth 必须在同一个 slot k、同一次 forward fence 之下 ——
      现在 HUDless 走的是另一条颜色通路，V2 把它并入 slot，配对就从"按时间猜"变成"按槽位定"
```

这样"哪一帧的 MV 配哪一帧的颜色"由槽号 + fence 值唯一确定，不再依赖 WGC 的到达时间。

## 6. UI 与 HUDless 的取舍（不降精度的边界）

```text
HUDless：必须全分辨率（它就是 FG 的输入颜色），走拷贝引擎直搬，位级相同 → 33 MB/帧（4K）
UI     ：XeFG 的 UI 输入是"颜色+alpha"。三条路，按代价排序：
          a) 游戏本身把 UI 画在独立 RT → 直接搬（最便宜，也最准）
          b) 只有在 backbuffer 里 → ui = backbuffer（与 hudless 的差值区域不减分辨率）
          c) 掩码路线（1/4 分辨率 R8，0.5 MB）只在"我们自己合成 UI"时才有意义
         先做 a；没有 a 时用 b；c 需要改消费者合成路径，放到最后
```

需要一次如实测量：XeFG 是否接受低于输出分辨率的 UI 纹理（决定 33 MB 还是 2 MB）。
在确认之前，预算按 33 MB 算。

## 7. 成本对照（4K MV + 1440p depth，78 fps）

| 项 | 现在 | V2 | 说明 |
|---|---|---|---|
| 生产者本地读 | 96 MB | 96 MB | 数据本身，无法省 |
| 生产者本地写 | 81 MB | 81 MB | 同上 |
| 生产者**额外**流量 | **+66+29.5 中转** | 0 | MV 不再过 pass；depth 不再 typeless 中转 |
| 生产者 barrier | 6 次 | 2 次 | 每帧省 0.1–0.3 ms 停顿 |
| 跨卡（PCIe） | 81 MB | 81–114 MB | depth 由 8B→4B，HUDless 按需 |
| 消费者本地 | 读 81 + 写 81 | 读 81 + 写 81 | 本地 ring（必要，见 §4） |
| 消费者 PCIe 放大 | 有（FG 多 pass 若直读） | 无 | 一次拷完再读本地 |

PCIe 预算：114 MB/帧 × 78 fps ≈ **8.9 GB/s**。PCIe 4.0 x16（~20 GB/s 实测）约 45% 占用，
PCIe 3.0 x16 约 70% —— 后者已经是"能吃但没余量"，所以 §6 的 UI 与 `Color` 按需开关很重要。

## 8. 分阶段落地（每步独立可验收）

```text
S1  MV 走拷贝引擎 + depth 直读 SRV 去掉 typeless 中转（都是位级相同）
    验收：probe 行不变；--dump 的 MV/depth 与现状逐像素一致；perf 的 barrier 计数下降
S2  槽位打包（MV/depth/hudless 同槽不同子资源）+ 2 次 barrier
    验收：单帧 forward fence 覆盖全部流；消费者一次 wait 拿全
S3  消费者局部 ring + copy 队列异步流水（N=4）
    验收：FG 帧时间不再随跨卡读抖动；overrun=0
S4  HUDless 并入槽 + UI 输入（先 a 路线）
    验收：开 FG 后 UI/字幕不与插值帧错位

S1 不含任何精度取舍，是纯工程收益 —— 建议从它开始。
```

## 9. 流开关板（v14）：每一条非必要数据都可动态开关

原则：**游戏画面（HUDless 颜色）是必需项，其余全部可开关**；开关只改"是否上线"，
不改槽位布局（容量一次 place、永不搬动），所以随时切换都不会破坏会话，也不会重启。

### 9.1 位定义

| 位 | 名称 | 内容 | 典型成本（4K/1440p, 78fps） |
|---|---|---|---|
| `kRequestMv` | mv | 运动矢量（全精度 RG16F） | 66.4 MB/帧 ≈ 5.2 GB/s |
| `kRequestDepth` | depth | 深度（全精度 R32） | 14.7 MB/帧 ≈ 1.1 GB/s |
| `kRequestColor` | color | HUDless 颜色（必需项，但**允许关掉以测基线**） | 33.2 MB/帧 ≈ 2.6 GB/s |
| `kRequestUi` | ui | UI 颜色+alpha（或掩码） | 33.2 MB/帧（掩码路线 0.5 MB） |
| `kRequestMeta` | meta | 相机/矩阵/jitter/mvscale/semantics（"else"） | 几百字节 ≈ 0 |

### 9.2 两侧的行为

```text
消费者（bridge/UI）：写 SharedControl.request_mask（随时，无需握手）
  --streams=all            全部
  --streams=none           只留游戏画面
  --streams=mv,depth       精确指定
  --no-mv / --no-depth / --fg-of 依旧生效，最终合并进同一个 mask（只有一个真相源）

生产者（add-on）：每帧读一次 mask
  request_mask != 0 → 用它；== 0 → 退回旧的 consumer_request（旧消费者兼容）
  每个流只在自己那一位打开时记录拷贝；关掉 = 零带宽、零 GPU 时间（不是只盖住）
  发布时回写 active_mask（实际传了哪些）+ stream_bytes[3]（实际花了多少字节）
```

### 9.3 为什么这个设计让"测成本"变容易

```text
① 全部关 → active_mask=0：这就是"纯转发宿主"的基线，可与打开逐项对比
② 一次只开一项 → 差值就是该项的真实成本（不是估算，是 active_mask/stream_bytes 实测）
③ 开关不重启游戏 → 同一次运行里就能 A/B，排除场景/画质设置漂移
④ 关掉不等于丢弃：消费者仍然按 frame_id 配对，缺流时走它自己的降级路径（XeFG 允许无 depth）
```

### 9.4 验收判据

```text
[bridge] streams -> mv=1 depth=1 color=0 ui=0 meta=1     ← 消费者侧
streams: mv=1 depth=1 color=0 ui=0 meta=1 (consumer switch board)  ← 生产者侧
两者一致；active_mask 与 request_mask 的差异只允许出现在"源不可用"时（并记日志）
```

## 10. S1 实测结论（2026-11-11）

### 10.1 depth：直读 SRV 成立，省掉 59 MB/帧

真机日志显示深度源是 **typed** 的 `D32_FLOAT_S8X24_UINT`（fmt 19），旧代码只对 typeless 走直读，
于是它落到"整幅拷进 typeless 中转"的慢路径：

```text
[depth] same-frame convert in place: 2560x1440 fmt 19, tracked state = 0xE0
  → CopyResource(8 B/px, 29.5 MB 读 + 29.5 MB 写) → 再跑转换 pass → 写 slot 14.7 MB
```

改法：

```text
条件从 "typeless" 扩到 "typeless 或 typed 深度格式"（D32_FLOAT_S8X24 / D32_FLOAT /
  D24_UNORM_S8 / D16_UNORM），因为 ds_run 早就按源格式选了同族的 SRV 视图
状态用 DEPTH_READ|PIXEL_SHADER_RESOURCE (0x50)，而不是 ALL_SHADER_RESOURCE (0xC0)
  —— 后者对 DSV 不是运行时期望的状态（和 2026-11-10 那次设备移除同类）
失败可回退：DepthDirect=0 恢复旧路径
```

效果：深度这一步 **读 29.5 + 写 29.5 + 写 14.7 → 读 29.5 + 写 14.7**，每帧少 59 MB。

### 10.2 MV：不能用拷贝引擎，原因已写进日志

```text
源   3840x2160 R16G16_FLOAT（上采样器输出的 hi-res MV）
槽位 2558x1438（消费者要 render 尺寸）
→ 拷贝不能缩放（CopyTextureRegion 只能裁剪），所以采样 pass 就是"保精度前提下的最便宜搬运"
日志：[mv] 3840x2160 -> 2558x1438: sample pass (a copy cannot rescale; full precision kept …)
```

真正能再省的是 S2 的选项（二选一）：

```text
(a) 槽位改放源分辨率的 MV，消费者自己重采样 —— 生产者零转换，代价是跨卡 66 MB 而不是 14.7 MB
(b) 保持现在：生产者在游戏侧重采样 —— 跨卡 14.7 MB，代价是游戏侧多一次 66 MB 读
```

按 §7 的 PCIe 预算，PCIe 3.0 平台选 (b)，PCIe 4.0 平台且游戏侧 GPU 更忙时选 (a)。
这一条留到 S2 用 `stream_bytes` 实测再定。

## 11. 主卡零处理（Dumb Producer）原则

原则：**游戏 GPU 只做"拷贝指令"，不做任何像素工作；所有重采样/格式转换/打包都在副卡完成。**
理由：主卡的性能就是游戏帧率本身，副卡（FG）在 2x 模式下本来就有整帧的空闲。

### 11.1 逐流拆解：现在谁在做处理

| 流 | 现状（主卡做处理） | 主卡代价 | 变成"零处理"后 | PCIe 代价变化 |
|---|---|---|---|---|
| MV | 采样 pass：4K→render 重采样 | 读 66 MB + 写 14.7 MB（SM 时间） | 拷贝引擎 1:1 直搬 4K | 14.7 → 66 MB（+51） |
| depth | 转换 pass：8B→4B | 读 29.5 + 写 14.7（SM 时间） | 拷贝引擎 1:1 直搬 8B | 14.7 → 29.5 MB（+14.8） |
| HUDless | 已经是 1:1 拷贝 | 仅拷贝引擎 | 不变 | 不变（33 MB @4K） |
| UI | 未实现 | — | 1:1 拷贝 | 见 §6 |
| else | memcpy 几百字节 | ≈0 | 不变 | ≈0 |

结论：MV 与 depth 的"处理"可以完全搬到副卡，主卡只剩拷贝指令（每帧约 2 µs 的 CPU 记录 + DMA）。

### 11.2 代价换到了哪里（必须诚实）

传输是**每"真实帧"一次**，不是每个生成帧一次（FG 的 3x/4x 只加副卡的工作量，不加总线）：

```text
每真实帧的跨卡字节（全 raw）：mv 66 + depth 29.5 + hudless 33 + ui 0.5 ≈ 129 MB
  2x @ 120 Hz → 60 Hz 真实帧 → 7.7 GB/s
  3x @ 120 Hz → 40 Hz        → 5.2 GB/s
  4x @ 120 Hz → 30 Hz        → 3.9 GB/s
（§12.3 里按 78 fps 输出算的那张表偏保守：它把生成帧当成了传输帧，已按本节修正）
```

这对 PCIe 4.0 x8（分叉 x8/x8，实测 ~13 GB/s）意味着 30–40% 占用；PCIe 3.0 x8（~6.5 GB/s）
在 3x/4x 下也只要 60–80%，配合选择性传输可行。**补帧倍数越高，总线越宽松** —— 副卡更忙、
主线更闲，正好互补。

所以这不是"免费"，而是**用 PCIe 带宽换主卡 GPU 时间**。在 PCIe 4.0 平台上这几乎是白赚的：
MV 的 66 MB 读在主卡上是一次全屏采样（约 0.15–0.3 ms），换到副卡只多占 4 GB/s 总线。
PCIe 3.0 平台则应只把 MV 搬到副卡（+4 GB/s），depth 留在主卡转换（省 14.8 MB）。

### 11.3 副卡需要补的那一步（Prepare 阶段）

副卡拿到的都是"原样数据"，喂给 XeFG 前要做一次预处理（一帧一次，可与 FG 计算重叠）：

```text
输入（跨卡槽位，1:1 拷贝得到的本地副本）
  mv    3840x2160 R16G16_FLOAT（或源家族里实际的那个）
  depth 2560x1440 R32G8X24_TYPELESS（8 B/px，含 stencil 平面）
输出（本地，XeFG 期望的形状）
  mv_prepared    render_w x render_h  RG16F   ← 一次双边滤波重采样（不上平均，避免跨边缘糊）
  depth_prepared render_w x render_h  R32F    ← 取 .x（深度平面），可选 2x 降采样
实现：一个 compute pass，2 个 SRV + 2 个 UAV，1 对 barrier
记账：这个 pass 的耗时计入副卡；它替代的是主卡上原来的两个 pass
```

### 11.3b 已实现（2026-11-11）

```text
副卡 bridge：新增 prepare pass（与 HUDless 转换共用同一 root signature）
  ps_prep_mv     双线性采样，RG16F → render 尺寸（MV 场内平滑，边缘交给 FG 的遮挡逻辑）
  ps_prep_depth  Load 最近点采样（深度不能跨轮廓插值）
  触发条件：request_mask 含 kRequestRawMv，且槽内尺寸 != 发布尺寸
  日志：[bridge] raw MV 3840x2160 -> 2558x1438 resampled on this GPU (prepare pass)

主卡 add-on：
  ensure 之前先解析开关板（raw 位决定流尺寸，晚解析就会建错尺寸）
  raw MV 走 record_copy_region 1:1（日志：[mv] raw 1:1 copy …）
  mask 允许 v15 位（kRequestAllV15）
```

用法：`bridge.exe … --streams=mv,rawmv`（不含 rawmv 时行为与之前完全一致）。

### 11.4 落地顺序（不动已验证的路）

```text
S1（已提交，待真机验证）：depth 直读 SRV，主卡省 59 MB/帧
S2a RawStreams=1：主卡零处理（MV/depth 走 1:1 拷贝）+ 副卡 Prepare pass
    开关粒度：RawMv=1/0、RawDepth=1/0，可分别验证"省主卡 / 费 PCIe"的平衡点
    PCIe 3.0 建议：RawMv=1、RawDepth=0；PCIe 4.0 建议：两者都 1
S2b 合并 barrier：无论 raw 与否，多流一次进 COPY_SOURCE、一次还原
```

### 11.5 验收判据

```text
主卡：perf 行里 begin（记录耗时）不升；GPU 侧不再有 mv/depth 的采样 pass（日志应打印
      "[mv] raw 1:1 copy" / "[depth] raw 1:1 copy"，而不是 "sample pass" / "direct convert"）
副卡：新增 prepare pass 的耗时 < 主卡释放出来的时间
总线：stream_bytes 实测 = 66/29.5/33 MB，与 §11.2 的理论值一致
画质：dump 的 MV/depth 与处理版对齐（重采样误差只允许在亚像素级）
```

## 12. 可行性判断：主卡全量获取、副卡决定传什么（2026-11-11）

提案：主卡保持对全部数据的获取，**传输哪几项由副卡回传的信号决定**；用户在 UI 上勾选 FG
要用哪些信息，PCIe 占用随勾选变化。

### 12.1 结论：可行，而且 v14 的开关板已经是它的骨架

现有实现正好就是"副卡决定"的通道：

```text
副卡 → 主卡：SharedControl.request_mask（任何时候写，无需握手，1 帧延迟）
主卡 → 副卡：active_mask + stream_bytes[3]（实际传了什么、花了多少字节）
槽位容量在启动时 place 一次，开关只影响"是否写"，不重新布局 → 可以随时切换、不重启
```

所以这不是新架构，而是把已有通道接到 UI + 让成本随勾选真实下降。

### 12.2 但必须把"获取"拆成三层，否则会白花主卡性能

| 层 | 内容 | 成本 | 是否常开 |
|---|---|---|---|
| ① 取得（acquire） | 钩住上采样器导出，拿到资源指针、尺寸、状态、语义、相机矩阵 | ≈0（只存指针/状态） | **常开**，这就是提案里的"全量获取" |
| ② 传输（transfer） | 把数据搬进跨卡槽位 | DMA + PCIe | **由副卡信号决定** |
| ③ 处理（process） | 重采样/格式转换 | GPU 时间 | 按 §11 放在副卡 |

关键点：**"获取"不需要拷贝**。MV/depth/HUDless 本来就在游戏显存里，取得只是记住指针与真实状态；
真正花钱的是 ② 和 ③。如果误把"获取"实现成"主卡先全部拷一份"，就等于为不发送的数据付 GPU 成本，
那才是不可行的做法。

### 12.3 勾选 → 带宽（4K MV / 1440p depth, 78 fps）

| UI 勾选 | 传输 | PCIe | PCIe 4.0 x16 占用 |
|---|---|---|---|
| 仅画面 | hudless | 2.6 GB/s | 13% |
| + MV | +66 MB/帧 | 7.8 GB/s | 39% |
| + depth | +29.5 MB/帧 | 10.1 GB/s | 50% |
| + UI | +33 MB/帧 | 12.7 GB/s | 63% |
| 全开（UI 用掩码） | UI 0.5 MB | 10.2 GB/s | 51% |

这就是"按选择消耗 PCIe"的定量形式：每一档都可以在 UI 上直接显示预计占用与实测占用。

### 12.4 落地的四个缺口（都不大）

```text
① UI/热键：现在只有 --streams 命令行；需要运行时开关 + OSD 显示 active/bytes
   （bridge 已有 RegisterHotKey 的先例，加 4 个组合键即可）
② 持久化：勾选写回 ini，下次启动沿用
③ 切换语义：关掉某流时副卡立刻切到自己的降级路径
   （--no-mv 走 ensure_dummy_mv、--no-depth 走 ensure_dummy_depth，这两条路径已存在）
④ 饱和保护：全开时若消费者等 fence 的时间变长，OSD 提示"总线饱和"并建议关掉某项
   （wait_slot 已有计时点，只需统计）
```

### 12.5 风险与边界

```text
✓ 切换不需要重启、不需要重建槽位（v9 布局设计的直接红利）
✓ 1 帧延迟只影响"谁先用上新选择"，不影响正确性（帧配对靠 frame_id + 槽位）
⚠ UI/游戏自己也要读这些资源时，开关会改变"主卡是否把资源切到 COPY_SOURCE"，
   所以每个流的状态恢复必须成对（现在的 native_barrier 已经成对）
⚠ PCIe 3.0 平台全开会到 ~77%，此时应以 rawmv/掩码等省带宽方案配合，而不是硬开
```

## 13. 按补帧倍数重算带宽（结论：x3/x4 下几乎不构成约束）

### 13.1 传输频率 = 真实帧率，不是输出刷新率

```text
显示器 120 Hz：
  2x → 真实帧 60 Hz
  3x → 真实帧 40 Hz
  4x → 真实帧 30 Hz
传输 / 拷贝 / fence 全部按真实帧发生；3x/4x 只让副卡多生成 1–2 帧，不收总线费用。
```

### 13.2 全 raw 传输的占用（4K MV + 1440p depth + 4K HUDless + UI 掩码 ≈ 129 MB/帧）

| 补帧 | 真实帧率 | 所需带宽 | PCIe 4.0 x8（~13 GB/s） | PCIe 3.0 x16（~13 GB/s） | PCIe 3.0 x8（~6.5 GB/s） |
|---|---|---|---|---|---|
| 2x | 60 Hz | 7.7 GB/s | 59% | 59% | 119% ✗ |
| 3x | 40 Hz | 5.2 GB/s | 40% | 40% | 79% |
| 4x | 30 Hz | 3.9 GB/s | 30% | 30% | 60% |

结论：**x3/x4 场景下 PCIe 4.0 x8 只用 30–40%**，即使分叉成 x8/x8 也没有问题。
只有 2x + PCIe 3.0 x8 这种极端组合才需要取舍（关 depth 或 UI 颜色）。

### 13.3 由此得到的默认策略

```text
真实帧率 ≤ 40 Hz（即 3x/4x）：默认全 raw（主卡零处理），无需权衡
真实帧率 50–60 Hz（2x）：raw 仍可行（PCIe4 x8 59%），但保留一个开关降级到
                      "主卡处理 + 传小数据"，给 PCIe 3.0 用户用
饱和度自检：用 stream_bytes × 真实帧率 与链路带宽估算值比较，OSD 显示百分比；
            超过 75% 自动建议降级（关 UI 颜色 / 关 depth / 关 rawmv）
```

## 14. 运行时开关 + OSD 计量（已实现）与 rawdepth 的实测否决

### 14.1 热键与 OSD（2026-11-11 实现）

```text
Ctrl+Alt+1   MV 开/关
Ctrl+Alt+2   depth 开/关
Ctrl+Alt+3   UI 开/关
Ctrl+Alt+4   HUDless 颜色开/关（需要主卡预留了颜色槽位，即 Color=1）
Ctrl+Alt+5   rawmv 开/关（主卡零处理 MV）
Ctrl+Alt+0   整组开/关

每次切换只写 SharedControl.request_mask（主卡下一帧生效），不重启、不重建槽位。
OSD 追加：  in 129MB 5.2G 40%     ← 实测字节 × 真实帧率 ÷ 声明带宽
参数：      --link=13        声明链路带宽（GB/s，默认 13 = PCIe 4.0 x8 实测）
```

OSD 的数字来自 `stream_bytes`（主卡报告实际写了多少），不是估算；真实帧率取输入到达间隔，
所以 3x/4x 下看到的就是 30–40 Hz 对应的占用。

### 14.2 rawdepth 被实测否决

`tests/host_smoke` 新增跨卡可放置性探针（`--cross-adapter`），在 RTX 3080 Ti 上：

```text
MV  RG16F   4K      31.7 MB   OK
MV  RGBA16F 4K      63.3 MB   OK
CLR RGBA8   4K      31.7 MB   OK
CLR R10G10B10A2 4K  31.7 MB   OK
DEP R32     1440p   14.1 MB   OK
DEP R32G8X24 1440p  28.1 MB   REJECTED (0x80070057)
DEP D32S8X24 1440p  28.1 MB   REJECTED (0x80070057)
```

结论：**8 字节深度格式无法放到跨卡堆上**（与 add-on 里 2026-11-10 的结论一致），
所以"深度也 raw 传输"在这个平台上不可行 —— 深度必须在主卡侧转成 R32。

好消息：S1 之后主卡的深度路径已经是最省的（读 29.5 MB + 写 14.7 MB，无中转），
而深度只有 14.7 MB/帧（40 Hz 下 0.6 GB/s），留在主卡并不构成瓶颈。
因此 `kRequestRawDepth` 位保留但不接实现，文档如实标注。

## 15. 原则修正：生产者不服从消费者（2026-11-11）

v14 让消费者用 `request_mask` 决定主卡传什么。这条在结构上是错的：

```text
· 主卡的每帧路径因此依赖另一个进程的状态（谁先启动、谁卡了一下，都会改变主卡的行为）
· 与"生产者只管发、消费者自己平衡"的分工相反
· 实测代价：bridge 未启动的头几十秒 MV 不传，启动后又切换，日志里出现
  "consumer asked for motion vectors - stream enabled" 这种中途状态迁移
```

现在的分工（`[capture] FollowConsumer=0`，默认）：

```text
生产者：渲染完就把所有能拿到的流写进槽位并发布，不看消费者，不等待，不降级
消费者：自己决定用哪些、丢哪些、几倍补帧、什么时候读；带宽是它要平衡的事
例外：raw 位仍然由消费者决定 —— 它不改变"传什么"，只改变"谁来重采样"，
      而只有消费者知道自己有没有 prepare pass
```

开关板保留，但语义变了：`active_mask` / `stream_bytes` 现在只用于**观测成本**，
不再用于**限制产线**。需要 A/B 时 `FollowConsumer=1` 回到旧行为。
