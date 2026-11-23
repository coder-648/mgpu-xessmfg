# lock_frame 1.2.8 发布说明（2026-11-23）

> 协议仍为 **v30**（`kSharedVersion` 不变），`MgpuFgFrame` 布局不变，模型仍是
> **keep2x（model 18，所有倍率）**。
> 本版是**性能收尾 + UI 合成转为默认**：两处默认值改变（见 §1、§2），其余是测量工具与
> 若干零风险修正。所有数字来自 CP2077 4K、锁 40 fps、3x 输出（消费端 6750 GRE，
> 渲染卡 3080 Ti），方法与原始日志见 `docs/design/PERF_OPT_2026-11-23.md`。

## 1. 新默认：depth 走 150%（MV 仍比特精确）

两条流的画质预算不同：MV 直接决定 warp 精度，**必须 1:1 精确**；depth 只用于
disocclusion clamp，分辨率不敏感（`FINAL_CONFIG` §2.1.2：480×270 都够用）。
新增 `DepthCodecPercent`（按流覆盖 `CodecStreamPercent`），本版默认 **150**：

| 项 | 1.2.7（全 100%） | 1.2.8（depth 150%） |
|---|---:|---:|
| depth 编码 | 2.05 ms | **0.79 ms（−61%）** |
| depth 码流 | 10.82 MB/帧 | **5.56 MB（−49%）** |
| MV 码流 / 精度 | 5.96 MB / bit-exact | **6.07 MB / bit-exact（未变）** |
| 链路 mv+depth | 16.8 MB/帧 | **11.6 MB/帧** |
| 消费端 load | 11% | 12%（多一次 joint MV/depth 重建） |

`presents 961/962`、`xN=3.00`、`HITCH=0`。想回到全分辨率 depth：把 `DepthCodecPercent`
删掉或设 100，**重启游戏**（槽尺寸在启动时定死）。

## 2. UI 合成转为默认开启（HUDless / mode 4）

1.2.7 里这条链路是关闭的实验（`UiStream=0`）。现在 CP2077 全分辨率合成已实测为稳定基线，
本版默认打开：`UiStream=1` / `UiSource=Auto` / `UiFullRes=1` / `UiCodec=auto` /
`UiSparseCodec=1`。

- **为什么值得开**：不开时 XeFG 会把 HUD 一起插值，插值帧出现 HUD 残影/拖影；开了以后
  HUDless 由「present − UI」重建，观感正常。
- **实测代价** ≈0.8 ms/帧（synth 0.20 + UI encode 0.61），占 25.6 ms 预算的 3%。
- **自动退化**：没有 HUDless/UI 平面的标题，生产端只保留一个空闲槽
  （日志 `reserving stream 3`），消费端不启用组合（`ui-unblend: inactive - ...`）。
- **要最大帧率**：`UiStream=0` / `UiSource=Off`（HUD 会在插值帧里残影）。

### 2.1 sparse unblend：空 tile 不采样、不跑 PQ

消费端 unblend 以前对每个像素都读 present + 双线性采样 UI，HDR 还要跑两次 PQ（各 6 个
`pow`）。而稀疏 UI 码流早就把空 32×8 tile 标成 `mode=3`、解码成 0，这些像素的结果恒等于
`present`。现在按 codec 的 tile 头派生一张占用表（`shaders/ui_occ.hlsl`，4K 下 130 KB/帧，
**不走链路、协议不变**），空 tile 直接写 `present`。

- 帧率上没有可测差异（消费端在 3x@40 下还有 ~88% 余量）；价值是少做无用功。
- 想 A/B：消费端加 `--no-sparse-unblend`（不用改 ini）。

## 3. 内存：UI 本地纹理按 UI 槽位建

`g_ui_local` 原来按 MV/depth 的 32 个 publication slot 建（4K 全分辨率 R10 ≈ **1.06 GB**），
而 UI 共享环只有 `UiSlots`（默认 8）。现在按 `slot % UiSlots` 索引 ⇒ **≈265 MB**。
老 producer（slots==32）时等价于旧行为。

## 4. 新增测量工具（默认全关，零开销）

| 键 / 开关 | 作用 |
|---|---|
| `ProfGpu=1` | 给每个我们自己的 dispatch 打 GPU 时间戳，每秒一行 `prof: t=… mv … depth … ui … synth …` |
| `FrCodecAblate=1|2|3` | 编码器消融（去扫描 / 去原子 / 去像素读取），只改 MV 流，**字节不可解码**，仅用于归因 |
| 消费端 `--run-sec N` | 跑满 N 秒自动退出，便于固定时长的 A/B |
| `CodecStreamPercent` / `MvCodecPercent` / `DepthCodecPercent` | 按流选择码流分辨率（100 = 无损 1:1） |
| `DepthFp16=1` | depth 先量化到 fp16 精度再无损编码（字节 −34%，有损 0.017%（均值相对距离）；实测**不省时间**，默认关） |

`ProfGpu` / `FrCodecAblate` / `DepthFp16` 都是**每秒重读一次 ini**，改完 1 秒生效、不用重启；
影响槽布局的键（`CodecStreamPercent`、`DepthCodecPercent`、`UiStream`、codec 开关）仍需重启。

## 5. 其他修正（不影响画质）

- **marker 探测不再每帧两次系统调用**：`OpenEventW/CloseHandle` 与
  `GetFileAttributesW("marker_disable.on")` 改为 1 s 缓存（命中后），未命中仍每帧重试，
  避免消费者刚启动时出现约 40 帧无 marker。
- **计时修正**：`capture=` 与 `g_frame_ms_ema` 原来用 `GetTickCount64`（15.6 ms 量化，
  读数只能是 0/16/31），改为 QPC；`--fg-rt-mode auto` 的 64 样本中位数从每帧一次改为
  每报告窗口一次。
- **Dispatch 合法性**：判别条件加上 list 类型（COPY-only 的标题车道不再可能把 Dispatch
  记到 COPY 队列上），encode / synth / scale / hudless 四处共用。

## 6. 已知限制

- 3x/4x 仍只建议在 120 Hz 面板、游戏锁 40/30 fps 时使用；`--unlock-mfg` 只对实测过的
  `libxess_fg/libxell` 版本有效。
- `DepthCodecPercent=150` 让 depth 变成「重采样后无损」：快速转视角时遮挡边缘的观感需要
  你自己确认；在意就设 100（MV 一直是精确的）。
- UI 合成依赖游戏在 DLSS-G evaluate 里提供 HUDless/UI 平面；不提供的标题会自动退化为
  不合成（画面上就是 HUD 残影，和 1.2.7 默认一致）。
