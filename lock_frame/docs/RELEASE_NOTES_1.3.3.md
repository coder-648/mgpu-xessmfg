# lock_frame 1.3.3 — 发布说明（2026-11-25）

> 上一版：**1.3.0**（2026-11-24）。共享协议仍 **v34**，`fakerFG` 记录 API 仍 **v4**；
> `fakerFG.dll` / `xess_mfg_producer.dll` / `lock_frame.exe` / `lock_frame_ui.exe`
> 必须同版本一起更新（旧 producer 会拒绝 attach，不会错读布局）。

本版把 1.3.1 / 1.3.2 的输入契约修复与回退结论收口，并完成四件事：
**4K 显存降 43%**、**环满时只复用安全槽**、**consumer 退出后 producer 停止空转**、
**UI mode 4 的小 UI 抖动**。协议布局与 ini 键保持兼容。

## 1. 下载

| 文件 | 内容 |
|---|---|
| `lock_frame-1.3.3.zip` | 运行包（自带 Intel 运行时，解压即用） |
| `lock_frame-1.3.3-src.zip` | 对应源码（GPL-3.0） |

包内 `manifest.json` 记录版本 / 协议 / 构建提交，`SHA256SUMS.txt` 是逐文件校验值；
`docs/` 下是排障与本说明，`extras/` 是本轮 A/B 与审计脚本。

## 2. 重点变化

### 2.1 4K 显存映射 −43%

`TransportSlots` 默认 **16**（可设 4..32）。4K codec 每槽约 33 MiB，CP2077 4K 实测映射
**1773.6 MB → 1017.6 MB**。`frames[] / consumed[]` 仍是 32 项，consumer 按 `st.slots`
工作，无需改逻辑。出现稳态 `pair: no_slot` 时把 `fakerFG.ini` 的 `TransportSlots`
调到 24/32 并重启游戏（余量换显存）。

### 2.2 环满时只复用安全槽

producer 以前无条件覆盖最旧的槽，实测每个新 run 启动都会 `reclaimed oldest slot 1/2/3`，
可能盖掉 consumer 正要配对的槽。现在只覆盖“消费者不可能还需要”的槽
（`frame_id == 0`，或 `frame_id + 2 <= colour_seq`），其余丢帧等 ACK 后自然恢复。
日志由 `reclaimed oldest slot` 变为 `reclaimed SAFE slot` / `no safe slot to reuse`。

### 2.3 consumer 退出后不再空转

consumer 以 `TerminateProcess` 退出时 `request_mask` 会残留，producer 以前一直每帧编码/拷贝
两路流。现在用 `consumer_session` 高 32 位的 pid 每秒探测一次：消失 3 s 后暂停拷贝，
之后每 5 s 放一次保活发布以便被 ACK 唤醒。不能用 `kConsumerPresentEvent` ——
`lock_frame_ui.exe` 会持有一整个生命周期。consumer 两处令牌构造改为
`(pid << 32) | 低 32 位`，保证高半区永远是 pid。

### 2.4 UI mode 4：小 UI 抖动

- **hard edge 设为默认**（`--ui-patch-box-feather` 可回退）：7×7 盒平均的权重是 1/49 量化的，
  小 UI 移动时混合强度逐帧变化，是“抖动”的来源；5×5 max 膨胀的权重只会是 1 或 0。
- **LUT 直方图时间平滑**（新 `patch_lut3d_decay.hlsl`，`--ui-lut-decay` 默认 92，约 12 帧记忆；
  `0` = 旧行为每帧清零）：33³ LUT 不再逐帧跳变。
- `--ui-lut-stride`（默认 4）：有时间平滑后可以放宽采样步长，降低插帧卡的 LUT 构建成本。
- `--tag-dump` 新增 `tag_XX_alpha.bmp`（支撑掩码）与 `tag_XX_diff.bmp`（`|P − H'| × 8`），
  用于肉眼检查 HUDless 重建。

### 2.5 跨卡同步与状态修正（consumer）

- **A**：帧内 MV/depth 的拷贝/解码前，把 producer 的 `fwd[slot]`（值 = `frames[slot].published`）
  排进同一条队列（`order_publication_read()`）。此前只有 prefetch 路径做这件事，而 prefetch
  在锁帧版被强制关闭，主路径依赖“WGC 恰好晚一帧”的巧合。队列级 Wait，不阻塞 CPU。
- **C**：重建结果用 `g_up_pub` + `g_up_slot` 与 tag 条件一起校验，槽号复用时不会 tag 到上一轮结果。
- **D**：`record_upsample()` 的 `StateBefore` 由 `COPY_DEST` 改为 `COMMON`（codec 解码与
  纹理 copy 的真实收尾状态）。
- **E**：codec 会话禁用 prefetch（否则会往已关闭的 list 录 Dispatch）。
- **F**：`--lock` + `--jbuf` 丢帧路径补 `jbuf_give_back()`（本版**未实机验证**，见 §7）。

## 3. 组件明细

### consumer（`lock_frame.exe`）

- 上述 A / C / D / E / F；
- UI mode 4：hard edge 默认、LUT 92% 时间平滑、`--ui-lut-stride`、`--tag-dump` 增强。

### producer（`xess_mfg_producer.dll`）

- 上述 2.1 / 2.2 / 2.3；
- `UiStats`（默认 **60**）：synth 的覆盖率/诊断 `InterlockedAdd` 从每帧改成每 N 帧
  （`0`=从不，`1`=旧行为）。4K、6% 覆盖时原来约 50 万 + 13 万次 atomic/帧，读数本来就只按 1 Hz 记录；
- `UiSynthThreshold`（随包 ini **2**，代码兜底 8）：P/H 差分支撑阈值，之前硬编码 8。
  实测 8 会漏掉小 UI 的软边（移动时抖），2 明显更好；干净像素残差约 0.012 码值，
  所以 2 会把少量噪声也收进支撑（视觉无害，只多一点码流）。

### UI（`lock_frame_ui.exe`）

- 随版本号重建（1.3.3）；命令行为不变（hard edge / LUT 平滑 / 阈值都由默认值生效）。
- 默认部署的 `fakerFG.ini` 已带 `TransportSlots=16` / `UiSynthThreshold=2` / `UiStats=60`。

## 4. 默认值（相对 1.3.0）

| 键 | 1.3.0 | 1.3.3 |
|---|---|---|
| `TransportSlots` | 32（硬编码） | **16** |
| `UiSynthThreshold` | 8（硬编码） | **2**（随包 ini；代码兜底 8） |
| `UiStats` | 每帧 | **每 60 帧** |
| UI mode 4 边界 | 7×7 盒羽化 | **5×5 hard edge** |
| LUT 直方图 | 每帧清零 | **每帧衰减 92%** |

1.3.2 曾把 `CodecStreamPercent` / `DepthCodecPercent` 全改 100 并关闭消费端重建，实机对比后
**收益不足、成本偏高**（depth 回到 2562×1441：码流 +~8 MB/帧、编码约 +1.3 ms，人物边缘没有
可见改善），已回退。本版仍是 **MV 100%（比特精确）/ depth 150%（消费端联合重建）**。

## 5. 实机验证

环境：CP2077 4K DLAA、锁 40 fps、3x keep2x；渲染 RTX 3080 Ti，帧生成 RX 6750 GRE。

- `pair: exact=40 no_slot=0` 全程（首个窗口为启动 2 帧）、`xN=3.00`、`out 120/120`、
  `wait=0.0ms`、`slow/timeouts=0`；
- `--d3d-debug`：1045 条校验消息全部良性（`CheckFeatureSupport` / upload buffer `GENERIC_READ`），
  **barrier / state / invalid-call 0 条**；
- 映射 **1017.6 MB**（`ring=16`），`reclaimed oldest slot` 0 次；
- `consumer process is gone/alive` 正常切换；退出后只剩 3 s 宽限 + 5 s 一次的保活编码；
- UI：阈值 2 + hard edge + LUT 平滑后，小 UI 移动抖动明显减小，H' 与呈现画面无可肉眼分辨
  的色差；差分支撑里残留的少量“正常画面小区块”是干净像素残差越过阈值，视觉无害。

本轮实机回归以 CP2077 为主；PBZ / Wuchang 沿用 1.3.0 的验证结论，未在本轮重跑。

## 6. 安装 / 升级 / 回滚

- 把包内 `lock_frame_ui.exe` 放在任意可写目录；UI 会把 `fakerFG.dll` / `xess_mfg_producer.dll`
  等装进游戏目录，并重写 `fakerFG.ini`（原文件有备份）。
- **从 1.3.0 升级**：`TransportSlots` / `UiSynthThreshold` / `UiStats` 三个新默认值随包里的
  `fakerFG.ini` 模板生效。请点一次「安装 / 应用配置」（或「配置版本 → 精简」）覆盖游戏目录的
  `fakerFG.ini`，再重启游戏；只替换 exe/dll 而不重写 ini 时，producer 的代码兜底仍是
  `TransportSlots=16` / `UiStats=60`，但 `UiSynthThreshold` 会保持旧值 8（小 UI 仍会抖）。
- `xess_mfg_producer.dll` 需**重启游戏**；`lock_frame.exe` 可随 UI 重启。
- 回滚：UI →「还原 / 移除组件」；或恢复 `dxgi.dll.bak` / `nvngx_dlssg_real.dll`。

## 7. 已知限制 / 未验证

- **F**（`--lock` + `--jbuf`）只做了代码修复，未实机验证；
- **Q**（consumer 放在工具目录、不被注入 fakerFG 的“出厂布局”）尚未实机对比；
- K（producer copy ring，启动 `copywait max=16 ms`）、L（LUT 重建间隔）、8×8 group 最少命中数、
  编码器跳空 tile 仍未做；
- 4x 仍不推荐（边缘模糊带比 3x 宽）；port 模型仍仅实验；
- UI 合成需要游戏在 DLSS-G evaluate 里提供 HUDless；HDR 要求 R10/HDR10，SDR 标题自动退化。

## 8. 文件清单

```text
lock_frame.exe / lock_frame_ui.exe   消费端与 UI（版本 1.3.3）
fakerFG.dll                          v4 代理（部署时复制为 dxgi.dll / nvngx_dlssg.dll）
xess_mfg_producer.dll                v4 producer
fakerFG.ini / fakerFG.full.ini       游戏侧配置模板（含 TransportSlots / UiStats / 阈值）
libxess_fg.dll / libxell.dll         Intel 运行时（含许可）
docs/                                发布说明、排障、UI patch 开销
profiles/                            单游戏配置目录（首次运行生成）
extras/                              本轮 A/B 与审计脚本
manifest.json / SHA256SUMS.txt       版本与完整性校验
```
