# MV / 深度：降采样后的升采样质量（2026-11-13）

> 问题：`StreamPercent` 把 MV/depth 在**生产端**降采样、在**消费端**再还原，能不能把精度
> 尽可能捞回来？
>
> 结论：能，但**大头不在升采样器，而在降采样器**。当前两个降采样 shader 都是
> 双线性平均（`downsample_mv.hlsl` / `downsample_depth.hlsl` 的 `lerp(lerp(...))`）——
> 在运动/深度边界上它写出的是一个**从未存在过的矢量/深度**，任何升采样器都无法还原。
> 正确的顺序是：① 生产端改成“取真实样本”，② 消费端用另一个全分辨率信号做引导，
> ③（本仓库最划算）**干脆别降 MV**，只降深度。
>
> **定案（2026-11-13）**：方案 A 已无条件落地（两个 HLSL 已替换）；方案 B 落在
> `--upsample` / UI「升采样 MV/深度 (A/B)」后面（默认关，实测后决定去留）；方案 C 已落地为
> `--stream-asym`（MV 1:1 / depth 降档，隐含升采样，把 depth 重建到 MV 尺寸）。
> 去留判据见 `PLAN_2026-11-13.md`。

## 1. 现状：信息是在哪一步丢的

```hlsl
// downsample_mv.hlsl（现状）
Dst[id.xy] = lerp(lerp(v00, v10, f.x), lerp(v01, v11, f.x), f.y);
```

假设 2x2 块里 3 个像素是前景（向右 10 px），1 个是背景（0 px）。输出是一个 7.5 px 的矢量：
- 前景插值会“过冲”，背景会被拖出拖影；
- 升采样时无论用双线性、双三次还是 JBU，拿到的都是这个 7.5 px —— **信息已经没了**。

深度同理：边界上的平均值既不是前景也不是背景，XeFG 的 disocclusion clamp 会按错误的一侧拒绝。

所以“恢复精度”的上限由**降采样时保留了什么**决定：
- 保留一个真实样本 ⇒ 平滑区可近似无损，边界误差被限制在 1–2 像素带；
- 保留平均值 ⇒ 边界必然出现幽灵运动，升采样只能把它抹平，不能消除。

## 2. 三个方案（按性价比排序）

### 方案 A：生产端改为“取真实样本”（零额外带宽，收益最大）

MV：取 3×3 足迹里**离均值最近的那个真实样本**（medoid），而不是求平均。
边界上它会自然跟随占比大的一侧，不会合成幽灵矢量；平滑区 9 个样本相同 ⇒ 输出完全一致。

```hlsl
// downsample_mv.hlsl（建议替换）
const float2 scale = float2(SrcW / (float)DstW, SrcH / (float)DstH);
const float2 c = (float2(id.xy) + 0.5) * scale - 0.5;
const float2 o = scale * (1.0 / 3.0);          // 3x3 覆盖降采样足迹
float2 v[9]; float2 mean = 0;
[unroll] for (int y = -1; y <= 1; ++y)
  [unroll] for (int x = -1; x <= 1; ++x) {
      v[(y+1)*3+(x+1)] = Src[clamp_px(int2(round(c + float2(x, y) * o)))];
      mean += v[(y+1)*3+(x+1)];
  }
mean /= 9.0;
float2 best = v[0]; float bd = 1e30;
[unroll] for (int i = 0; i < 9; ++i) {
    const float dd = dot(v[i] - mean, v[i] - mean);
    if (dd < bd) { bd = dd; best = v[i]; }
}
Dst[id.xy] = best;
```

深度：取足迹的**中位数**（偶数会平均，所以用 3×3 的 9 个样本）。中位数保留真实样本、
不需要知道 Z/反向 Z 的约定；知道约定时用 `min`（标准 Z）/`max`（反向 Z）更锐，但会把细前景“加粗”。

```hlsl
// downsample_depth.hlsl（建议替换，Mode: 0=median, 1=min, 2=max）
// c / o / clamp_px 与现状相同（3x3 覆盖足迹）
float v[9];
[unroll] for (int y = -1; y <= 1; ++y)
  [unroll] for (int x = -1; x <= 1; ++x)
      v[(y+1)*3+(x+1)] = Src[clamp_px(int2(round(c + float2(x, y) * o)))];
float r;
if (Mode == 1 || Mode == 2) {
    r = v[0];
    [unroll] for (int i = 1; i < 9; ++i) r = (Mode == 1) ? min(r, v[i]) : max(r, v[i]);
} else {
    for (int i = 1; i < 9; ++i) {            // 插入排序，v[4] 即中位数
        const float key = v[i]; int j = i - 1;
        while (j >= 0 && v[j] > key) { v[j+1] = v[j]; --j; }
        v[j+1] = key;
    }
    r = v[4];
}
Dst[id.xy] = r;
```

两个 shader 都已用本机 Windows SDK 的 `fxc -T cs_5_0` 编译通过（每帧 ALU 增加约 20–30 条，
带宽与原来完全相同）。这一条不改变协议、不增加传输，**应该先做**。

### 方案 B：消费端联合升采样（一个 pass，两个输出）

用**深度引导 MV**（运动边界几乎总落在深度边界上），同时让深度输出**真实样本**而不是平均值：

```hlsl
// upsample_joint.hlsl（消费端，cs_5_0，已验证编译）
// t0 = 低分辨率 MV，t1 = 低分辨率 depth，u0 = 全分辨率 MV，u1 = 全分辨率 depth
const float2 uv   = (float2(id.xy) + 0.5) * scale - 0.5;
const int2   base = (int2)floor(uv);
const float2 f    = uv - (float2)base;
const float  d0   = Depth[clamp_px((int2)round(uv))];
const float  iz0  = 1.0 / max(abs(d0), 1e-6);      // 1/z：标准/反向 Z 都只看差值

float2 mv = 0; float wsum = 0; float best_w = -1, best_d = d0;
[unroll] for (int y = 0; y <= 1; ++y)
  [unroll] for (int x = 0; x <= 1; ++x) {
      const int2  p  = clamp_px(base + int2(x, y));
      const float iz = 1.0 / max(abs(Depth[p]), 1e-6);
      const float ws = (x ? f.x : 1 - f.x) * (y ? f.y : 1 - f.y);
      const float wd = 1.0 / (1.0 + DepthK * abs(iz - iz0));
      const float w  = ws * wd + 1e-6;
      mv += Mv[p] * w; wsum += w;
      if (w > best_w) { best_w = w; best_d = Depth[p]; }
  }
OutMv[id.xy]    = mv / max(wsum, 1e-6);
OutDepth[id.xy] = best_d;                          // 真实样本，不平均
```

要点：
- **从最近样本出发**（bilinear 权重只作空间项），边界两侧不会被各取一半；
- 深度权重用 `1/z` 差值，不依赖 Z 约定；`DepthK` 按场景调（建议 50–200，深度归一化到 [0,1]）；
- 深度**不做加权平均**，直接输出权重最大的那个真实样本 —— 保住轮廓；
- 两个输出同尺寸，满足 XeFG 的“MV 与 depth 必须同尺寸”。

### 方案 C：非对称流（本仓库最划算，推荐先做）

`FINAL_CONFIG.md` §2.1.2 实测：**深度分辨率不敏感**（2562×1441 与 3840×2160 无差别，
480×270 才更差），而 **MV 是被 XeFG 真正读取的那个**（喂真 MV vs 零 MV 差 7.4 ms）。
那就不该给两者同一个档位：

```text
MV    = 100（全分辨率，33.2 MB）
depth = 150/200（28.1 / 15.8 MB）
合计 41.3 MB（比 150/150 的 28.1 多 13 MB，但比 100/100 的 63.3 少 22 MB）
```

消费端只需要把 depth 升到 MV 的尺寸（XeFG 要求同尺寸），**MV 完全不经过升采样**。
引导信号可以用全分辨率 MV（同一帧、已在本地、且可以先于 WGC 完成）：

```text
t0 = 全分辨率 MV，t1 = 低分辨率 depth，u0 = 全分辨率 depth
权重：w = 空间权重 × (1 / (1 + K·|1/z_i − 1/z_ref|)) × (|mv| > 阈值 ? 1 : 0)
```

- `|mv|` 大 ⇒ 有运动，MV 边缘可信，用它引导；
- `|mv| ≈ 0` ⇒ 静止或纯旋转，MV 没有边缘信息 → 退回双线性（深度本身已经足够平滑）。

这条最符合本仓库的实测结论：**把精度预算花在 MV 上，深度用最便宜的方式补齐尺寸。**

## 3. 精度上限：能捞回多少，捞不回什么

| 区域 | 方案 A + B 之后 | 说明 |
|---|---|---|
| 平滑区（矢量/深度近似常量） | **近似无损** | 真实样本 + 权重≈1，输出等于原值 |
| 运动/深度边界 | 误差被压到 **1–2 像素带** | 低分辨率块内只能保留一侧；另一侧靠邻块/引导补 |
| 块内亚像素变化 | **不可恢复** | 降采样是投影；除非传残差或全分辨率 |
| 被遮挡背景的矢量 | **不可恢复** | 低分辨率块里根本没有；只能靠时域重投影（增加延迟，不做） |
| 深度绝对值 | 不需要 | XeFG 只用它做 disocclusion 判断，轮廓比数值重要 |

所以“尽可能恢复原有精度”的现实目标是：**平滑区无损、边界不串色、误差带宽 ≤ 2 px**，
而不是逐像素还原。要做到逐像素无损，只能不降采样（方案 C 的 MV 部分）。

## 4. 成本与执行位置

| 项 | 4K / 150 档 | 说明 |
|---|---|---|
| A：生产端换滤波器 | **0 额外带宽**，ALU +20–30 条/像素 | 仍在游戏那条 list 上，和现在同一个 dispatch |
| B：消费端联合升采样 | 读 28+28 MB，写 63+63 MB（本地）≈ **0.3–0.5 ms** | 计算队列；两份输入都来自生产端，**可以在等 WGC 时跑** |
| C：非对称 + 深度升采样 | 读 33+8 MB（跨卡），写 63 MB（本地）≈ **0.2–0.3 ms** | 只升深度；MV 不经过重采样 |

放置建议：
- 消费端的 pass 放在 **COMPUTE 队列**，由 `fwd` fence 触发，和 `--prefetch-inputs` 同一时机
  —— 它藏在 WGC 的 8–12 ms 里，不进 XeFG 的关键路径；
- 但要注意它和 XeFG 抢同一张卡的算力：4K/40 的余量只有 0.3 ms（`TIMING_4K40.md` §3.2），
  所以**先上 A + C（只处理 depth，K 小），B 只在 1440p 输出或有余量时开**；
- 引导信号用深度/MV（本地）而不是 WGC 颜色：颜色要等 WGC，会把 pass 推到关键路径上。

## 5. 怎么验证（不要靠眼睛）

仓库已有 `--dump`（导出 MV/depth/colour 原始数据）和 `scripts/analyse_dump.py`：

1. **端到端误差（EPE）**：临时让生产端在 dump 的那一帧同时写一份全分辨率 MV，
   离线比较“真值 vs 降采样→升采样”的矢量误差，报 p50/p95/p99（像素）；
2. **边界带误差**：用全分辨率深度梯度做 mask，只统计边界 ±2 px 的 EPE —— 这是观感最相关的一列；
3. **拖影/串色**：同一段快速平移，A/B 看移动物体边缘的插值帧；
4. **预算**：`gpu`、`skip`、`slow`、`load` 的 A→B→A（本机噪声 2–10 ms，必须三轮）；
5. 接受标准：EPE p95 下降 ≥30%，且 `gpu` 不增加超过 0.3 ms（否则拿画质换帧率，不值）。

## 6. 风险

| 风险 | 说明 | 缓解 |
|---|---|---|
| medoid 逐帧跳变 | 边界块每帧可能选中不同侧的样本 → 插值帧闪烁 | 边界块退化为“上一帧选择”的时域保持；或对 MV 误差设阈值后混合 |
| 深度 min/max 加粗前景 | 细物体（栏杆、头发）被膨胀 | 默认用中位数；min/max 仅作为可选档 |
| JBU 的引导本身是低分辨率 | 深度引导只能分辨到流尺寸的边界 | 方案 C 用全分辨率 MV 引导；必要时用 WGC 颜色（付关键路径代价） |
| XeFG 同尺寸约束 | MV 与 depth 尺寸不同会被 `ERR_INVALID_ARGUMENT` 拒绝 | 方案 C 必须把 depth 升到 MV 尺寸后一起 tag |
| 时域方法 | 重投影补遮挡区能再提一档，但 +1 帧延迟、鬼影风险 | 不做；与“低延迟”目标冲突 |

## 7. 推荐顺序

1. **A（生产端 medoid/median）**：零带宽、零协议，先做，A/B 看边界 EPE；
2. **C（MV 100 + depth 150/200 + 深度升采样）**：最符合“深度不敏感、MV 敏感”的实测，
   每帧只多 13 MB 跨卡，MV 完全无损；
3. **B（联合 JBU）**：只在 A+C 仍能看到边界串色、且 `gpu` 有余量时开；
4. 以上都做完，再考虑把自动档位（`STREAM_AUTOSCALE.md`）的档位**按流分别给**
   （MV 与 depth 各一个 percent），那时控制律里 `B(p)` 要拆成 `B_mv + B_depth`。

