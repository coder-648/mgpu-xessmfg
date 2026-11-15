# 输入契约：为什么逐帧配对对 drop 敏感，以及顶层方案（2026-11-14）

> **后续结论（2026-11-14 晚）**：§5.2 的"隐藏 marker"**已经实现**，并成为当前配对主键
> （四角 6×6 短雪花 + CRC-8，`marker: decoded / miss`）；颜色仍走 WGC，
> 没有采用 §3 的方案 A（producer colour）。实现细节与实测见
> `docs/findings/PERF_ROUND_HANDOVER.md`，状态见
> `docs/PROJECT_STATUS_2026-11-14.md`。

## 1. 现象

2x / fixed3x 都会出现同一模式：

```text
一段画面干净
-> 一次 HITCH / drop / qdrop
-> pub-queue seq 缺失或 reset-history 触发
-> 拖影出现
-> OUTPUT FRAMES MISSING 持续若干窗口
-> 过一段时间自行恢复
```

最新数据：

- 2x：`reset-history` 8 次，`pub-queue seq not queued` 8 次，`HITCH gap=63ms`，
  `OUTPUT FRAMES MISSING` 32 个窗口；
- fixed3x：`reset-history` 6 次，`pub-queue seq not queued` 8 次，
  `HITCH gap=57ms`，missing 6 个窗口；
- present-ring 已命中，producer 也已一帧一次发布。

`resetHistory = 1` 已经发给了 XeFG，但没有解决问题。

## 2. 根因判断

问题不是“某一帧 MV 找错了”这么简单，而是：

1. WGC 的 colour 时间线本身会抖动/丢帧；
2. producer 的 MV/depth 是另一条时间线（Evaluate/present）；
3. 我们试图用 **逐帧 seq/时间戳把两条独立时间线粘起来**；
4. 一旦某帧缺失，XeFG 看到的输入序列出现空洞，**它的 FG history / present cadence /
   interpolation count 被打断**；
5. `resetHistory` 只清 upscaler/history 的一部分，不会重置 XeFG 的 FG 状态机，
   所以 `OUTPUT FRAMES MISSING` 和拖影会持续一段时间。

结论：**逐帧配对对 drop 敏感是结构性的，继续微调 pair/queue 不会根治。**

另外，3x/4x 还有独立问题：当前 libxess_fg 没有真实 MFG 模型，3x/4x 是重复使用
2x 模型 + 同一套 MV；2x 干净、fixed3x 仍拖影已经证明这一点。

## 3. 顶层方案

### 方案 A（推荐）：producer 提供 colour，同一 record 配 MV/depth

fakerFG 在真正 Present 时，把最终 colour 复制到一块 **cross-adapter shared texture**，
与 MV/depth 一起发布：

```text
{ seq, present_qpc, shared colour texture, MV, depth }
```

consumer 直接在 AMD 上打开 shared texture，作为 colour sink；不再使用 WGC 做 FG 输入，
也不再需要 present ring 配对。

- 优点：colour/MV/depth **同帧且同一条 record**，彻底消除 WGC 抖动/丢帧/配对问题；
- 成本：producer 每帧一次全分辨率 GPU copy（4K 约 33MB）+ 跨卡读取带宽；
  需要评估 PCIe/共享纹理带宽和 render card 的 copy 开销；
- 这是唯一能从结构上保证“任何 drop 都不会错配”的方案。

### 方案 B：固定深度 publication ring + coast 策略

如果 colour 仍走 WGC：

- producer 保留 N 个 publication（8/16），consumer input 线程复制到 N 个本地 ring，
  **不在 presenter 消费前覆盖**；
- presenter 按固定 seq 偏移消费，而不是“最新的那一个”；
- 某帧 colour 缺失时 **不推进输入时间线**：重复上一帧（coast）或跳过生成；
- 连续缺帧超过阈值时，**重置整个 XeFG context**（不是只 `resetHistory`），
  然后重新建立 cadence。

这能减少 drop 后的持续污染，但仍然是两条时间线，复杂度高。

### 方案 C：colour 自带 MV

用 FidelityFX optical flow 在 consumer 的 colour 序列上算 MV，不再依赖 producer MV。
配对问题消失，但 4K OF 成本高、质量与 producer MV 不同；适合作为 fallback/对照。

### 方案 D：只支持 2x

当前 2x 干净、fixed3x 有拖影。若 MFG 模型不可得，**2x 是唯一稳定的画质模式**；
3x/4x 需要真实 MFG model 或自研按时间分数生成 MV。

## 4. 建议顺序

1. **产品/画质优先**：默认 2x；3x/4x 暂不作为可交付质量。
2. **若必须让 WGC 路径对 drop 免疫**：实现方案 A（producer colour shared），
   这是根因级修复；方案 B 作为带宽受限时的备选。
3. 继续 M3 的 input 线程/ACK 可以改善 CPU/credit window，但不能修复 drop 后的
   XeFG 状态污染。
4. 不再在 pair bias / queue 回退上继续投入。

## 5. 颜色指纹（colour fingerprint）

WGC 不提供 present id，DWM 又会做 HDR/缩放/合成，所以不能用全帧哈希做“唯一 ID”。
可行的是**低分辨率内容指纹**：

### 5.1 感知哈希（推荐，保持 WGC 时）

producer 在 publish 时额外发布：

```text
uint64_t colour_hash;   // 8x8 / 16x16 低频 luma perceptual hash
```

- producer：从最终 backbuffer（或 HUDLESS，取决于 UI 处理）取中心区域，GPU 降采样到
  8x8/16x16，luma 去均值后取 sign bits，得到 64/256 bit hash；
- consumer：对 WGC 颜色做同样的降采样/去均值，得到 hash；
- 在最近 N 条 publication 里选 Hamming 距离最小的一条；距离超过阈值则视为丢失，
  coast 或 reset，而不是按时间戳猜；
- 优点：丢帧/抖动不影响“当前颜色找自己的 MV”，因为匹配是内容驱动；
- 代价：一个小 GPU pass + 小 readback；需要验证 WGC 的 tonemap/UI 对 hash 的影响。

### 5.2 隐藏 marker（精确 seq）

producer/add-on 在最终颜色里写一个很小的时空码（例如角落 8x8 块的低位亮度码，或
未使用的 alpha 通道），consumer 从 WGC 读回并解码出 seq。

- 优点：真正唯一、可精确匹配，不依赖 hash 相似度；
- 缺点：修改游戏画面，可能可见；DWM/缩放/HDR 可能破坏码；alpha 是否保留需实测。

### 5.3 不推荐

- 全帧 SHA/xxHash：WGC 与 producer backbuffer 像素不一致，必然失败；
- 依赖 frame_qpc/present qpc 的“哈希”：仍然是时间线，不解决 drop。

### 5.4 推荐顺序

1. 带宽允许：producer colour shared（同 record，无需指纹）；
2. 保持 WGC：producer perceptual hash + consumer 最近邻匹配（本节 5.1）；
3. 精确但可接受画质修改：隐藏 marker（5.2）。

### 5.5 marker probe 实测（2026-11-14）

fakerFG 在最终 backbuffer 的 **centre** 和 **top-right y=128** 各写一个 8x8 品红块，
consumer 从 WGC 颜色读回：

```text
marker probe: colour 3840x2160 fmt=10
marker probe centre:          maxR=4683 maxG=B349 maxB=45B1 white=64
marker probe top-right-y128:  maxR=4683 maxG=B349 maxB=45B1 white=64
marker probe top-right-y0:    全 0
marker probe top-left:        HUD 本身的亮色
```

结论：

- **WGC 能拿到 Present 前写入的 marker**，中心/右上 y=128 都命中；
- marker 在 WGC scRGB 里是品红（R/B 高，G 为负的半精度位型），不是屏幕上看到的黄色；
  黄色来自 consumer 的 HDR/BT.2020 输出；
- y=0 不写就没有，不能说明裁剪；正式位置选 y>=128 更安全；
- 解码不能直接比较 raw half bits（负数的符号位会误判），要按 signed half 或亮度差分。

下一步：把 8x8 品红块换成 32x32 **低位/alpha 码 + CRC**，用 blend/pixel shader
修改现有像素而不是整块覆盖；先验证不可见性和 60 s 解码稳定性。
