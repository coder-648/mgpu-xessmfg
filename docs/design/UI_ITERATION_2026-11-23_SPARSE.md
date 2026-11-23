# UI 合成迭代记录：sparse synth + sparse UI codec（2026-11-23）

> 状态：**1/2 已验证，作为当前稳定基线**。
>
> 范围：CP2077、`FgInput=DLSSG`、4K DLAA、锁 40 fps、3x 输出、只传 UI
> （`HudlessStream=0`）。不涉及 FSR/NVNGX/hudfix。

## 1. 本轮目标

在已经跑通的 DLSSG-only UI 合成链路上降低渲染卡成本：

1. **sparse synth**：8×8 group 先做 4×4 点采样；空 group 不再读 4K P/H；
2. **sparse UI codec**：空 32×8 tile 只写 `mode=3` 头和 `TileLen=4`，consumer
   decode 时把该 tile 清零；
3. 顺带验证：tag-time synth、HUDless R10 snapshot 两条进一步优化路径是否值得。

## 2. 当前稳定结果

### 2.1 画质 / 链路

`03`（mode 4）：

```text
ui-synth: coverage 6.40% ~ 6.78% (m>0.5)
fr: coded ui       1.36 ~ 1.55 MB/frame  (~20.4x ~ 23.2x)
pair: exact=40 no_slot=0
presents=120 expected=120
xNmiss=0
```

`04`（mode 5）：

```text
tag_m5_20261123_053220
mode5 1
source 3
derived 1
ui_fmt 24
ui_quality_bp 655
pair: exact=40 no_slot=0
presents=120 expected=120
xNmiss=0
cadence: avg 3.00x, 107–111 fps out
```

`tag_XX_ui.bmp` 只保留 HUD；`backbuffer/hudless/ui` 三张图都正常写出。

### 2.2 成本

RTSS / `12_gpu_sample`，4K DLAA、锁 40 fps：

| 场景 | 占用 |
|---|---:|
| native | 94% |
| 本系统，无 consumer | 97.5% |
| 本系统 + consumer，UI off | 100% |
| 本系统 + consumer，UI on（本轮前） | 105% |
| 本系统 + consumer，UI on（1/2 后） | **103%** |

1/2 净收益约 **2 pp**。其中 sparse UI codec 把 UI 码流从
`1.82–1.86 MB/frame` 降到 `1.36–1.55 MB/frame`（约 18–20%）。

空 group 不写 4K 零的改动没有测到额外收益（仍在 103% 附近），说明 UI 纹理写
不是当前瓶颈；代码保留，但不要把它当作性能优化项。

## 3. 当前配置

```ini
UiStream=1
UiSource=Auto
UiFullRes=1
UiCodec=auto
UiSparseCodec=1
UiTagSynth=0
UiSnapshotPQ=0
HudlessStream=0
UiAlpha8=0
UiDilate=0
D3dDebug=0
StreamPercentUi=100
```

关键开关：

- `UiSparseCodec=1`：启用空 tile 的 `mode=3` 快速路径；
- `UiTagSynth=0`：tag-time synth **实验关闭**；
- `UiSnapshotPQ=0`：HUDless 快照保持 FP16 精确 copy，**不做 R10 转换**。

## 4. 已验证的修复点

本轮为了 sparse 路径修过以下问题，均在当前代码中：

1. `GroupMask` 不能再用 root UAV：改成 UI `u0` + mask `u1` + stats `u2`
   的 UAV descriptor table；
2. synth root constants 从 8 个 DWORD 扩到 9 个（增加 `Sparse`）；
3. synth 空 group 只有在 `Sparse=1` 时跳过写零，tag dump 帧强制写零；
4. `fr_encode_dispatch` 的 t4 描述符在 `UiSparseCodec=0` 时按实际 TileLen
   大小创建，避免越界 SRV；
5. sparse codec 空 tile：`mode=3` + `TileLen=4`；decoder 对 `mode=3` 清零；
6. 资源命名/校验日志：`ui-synth`、`ui-mask`、`ui-stats`。

## 5. 失败实验（默认关闭，不要回退启用）

### 5.1 tag-time synth（`UiTagSynth=1`）

目标：在 `slSetTag(HUDless)` 时直接用当前 P/H 做 synth，省掉整帧 HUDless 快照。

实际结果：

- 在标题的 tag command list 上插入 dispatch/barrier，会干扰 CP2077 的渲染队列，
  GPU 占用来回波动、帧率下降；
- tag 时的 chain backbuffer 不一定是最终合成帧；
- 改用 `ScalingOutputColor` 作为 P 后，UI 仍出现大量场景边缘/火焰，coverage
  从 ~7% 涨到 60–98%。

结论：**不可行，`UiTagSynth=0` 固定为默认**。代码保留仅作实验。

### 5.2 HUDless R10 快照（`UiSnapshotPQ=1`）

目标：把 HUDless snapshot 从 FP16（66 MB 读 + 66 MB 写）改成 R10 PQ
（66 MB 读 + 33 MB 写），synth 再读 33 MB。

实际结果：

```text
sl: HUDLESS snapshot ... -> R10 PQ (compute, no full copy)
ui-synth: coverage 99.82%
fr: coded ui 24.73 MB/frame
tag_00_hudless.bmp mean ≈ 0.12
```

compute 快照读到的 H 基本为黑。CP2077 的 HUDless 实际状态/描述符与普通 SRV
不一致，旧的 `CopyResource` 路径可以工作，新 compute SRV 路径拿不到有效像素。

结论：**暂不可行，`UiSnapshotPQ=0` 固定为默认**。`hudless_to_pq.hlsl` 和
`pass_hudless_to_pq` 保留为实验代码；后续若继续，必须先单独解决 H 的状态/
descriptor，并用 tag dump 验证 H 不是黑图。

## 6. 构建与部署

本轮稳定运行时：

```text
fakerFG.dll / dxgi.dll / nvngx_dlssg.dll  b423947d...2d21
xess_mfg_producer.dll                     f7c4427f...4394b
```

注意：`dxgi.dll`、`fakerFG.dll`、`nvngx_dlssg.dll` 必须是同一份构建；测试前
运行 `00_check.bat`。部署后必须重启游戏，producer/faker 每个会话只读一次 ini。

## 7. 复现 / 验证

1. `00_check.bat`；
2. 启动游戏，读档，DLSS-G ON，锁 40 fps；
3. `03_run_3x_mode4.bat`（mode 4）或 `04_run_3x_mode5.bat`（mode 5）；
4. 看 `ui-synth: coverage`、`fr: coded ui`、`pair: exact`、`presents`；
5. 看 `tag_m*/tag_00_ui.bmp` 是否只有 HUD；
6. 成本用 `12_gpu_sample` 与 UI-off / native 对比。

## 8. 后续建议

- 当前 103% 作为 1/2 稳定基线；不要为了 <103% 回退启用 tag-time 或 R10。
- 若要继续降成本，优先做**不碰标题 command list、不改 H 语义**的项；
- 在没有单独解决 HUDless 状态前，不建议投入 item 4（synth+encode 融合）；
  它最多省 UI 纹理读写，而本轮已经证明纹理写不是瓶颈。
- **2026-11-23 追加**：消费端 unblend 已改成 sparse —— 占用表由 codec 的 tile 头派生
  （`shaders/ui_occ.hlsl`），空 32×8 tile 直接写 `present`，不再采样 UI / 跑 PQ。
  协议不变。记录与验证方法见 `docs/design/PERF_OPT_2026-11-23.md` §1.6。
