# lock_frame 1.3.3-local.ui — 发布说明（2026-11-25）

基线：**1.3.1-local.camvm**（已验证二进制）+ 2026-11-25 的三轮审查/修复。
共享协议仍 **v34**，fakerFG API 仍 **v4**；producer / fakerFG / consumer / UI 需要一起更新。

## 1. 消费者（lock_frame.exe）

- **A 跨卡读同步**：帧内 MV/depth 的拷贝/解码前，把 producer 的 `fwd[slot]`（值 =
  `frames[slot].published`）排进同一条队列（`order_publication_read()`）。此前只有 prefetch
  路径做这件事，而 prefetch 在锁帧版被强制关闭，主路径依赖"WGC 恰好晚一帧"的巧合。
- **C 重建结果按 publication 校验**：`g_up_pub` + `g_up_slot` 一起判断，避免同槽复用时
  tag 到上一轮的重建结果。
- **D `record_upsample()` 的 barrier 状态**：`COPY_DEST` → `COMMON`（codec 解码与纹理 copy
  的真实收尾状态）。`--d3d-debug` 实测 barrier/state/invalid-call 消息为 0。
- **E prefetch 的 codec 守卫**：codec 会话直接禁用 prefetch（否则会往已关闭的 list 录 Dispatch）。
- **F `--lock` + `--jbuf` 泄漏**：丢帧路径补 `jbuf_give_back()`。
- **UI mode 4 合成**：
  - 默认 **hard edge**（5×5 max 膨胀）：7×7 盒平均的权重是 1/49 量化的，小 UI 移动时混合强度
    逐帧变化是"抖动"的来源；`--ui-patch-box-feather` 可回退旧路径；
  - **LUT 直方图时间平滑**（`--ui-lut-decay N`，默认 92，约 12 帧记忆）：33³ LUT 不再逐帧
    重建噪声；
  - `--ui-lut-stride N`（默认 4）：有时间平滑后可以放宽采样步长，降低插帧卡上的 LUT 构建成本。
- **`--tag-dump` 增强**：新增 `tag_XX_alpha.bmp`（支撑掩码）与 `tag_XX_diff.bmp`
  （`|P − H'| × 8`），用于肉眼检查 HUDless 重建。

## 2. producer（xess_mfg_producer.dll）

- **B `claim_slot()`**：环满且无 ACKed 槽时，只覆盖"消费者不可能还需要"的槽
  （`frame_id == 0`，或 `frame_id + 2 <= colour_seq`）；其余丢帧。日志由
  `reclaimed oldest slot` 改为 `reclaimed SAFE slot` / `no safe slot to reuse`。
- **J `TransportSlots=16`**：只发布/放置前 16 个槽。4K codec 实测映射
  **1773.6 MB → 1017.6 MB**；出现稳态 `pair: no_slot` 时改 24/32 并重启游戏。
- **R consumer 判活**：用 `consumer_session` 高 32 位的 pid 探测进程；退出 3 s 后暂停拷贝，
  之后每 5 s 一次保活发布。此前 consumer 关掉后渲染卡仍每帧编码两路流，直到游戏重启。
- **UI patch（mode 4）**
  - `UiStats`（默认 60）：synth 的覆盖率/诊断 `InterlockedAdd` 从每帧改成每 N 帧
    （4K、6% 覆盖时约 50 万+13 万次 atomic/帧，读数本来就只按 1 Hz 记录）；
  - `UiSynthThreshold`（默认 **2**）：P/H 差分支撑阈值，之前是硬编码 8。实测 8 会漏掉
    小 UI 的软边（移动时抖），2 明显更好；干净像素残差约 0.012 码值，所以 2 会把少量
    噪声也收进支撑（视觉无害，只多一点码流）。

## 3. UI（lock_frame_ui.exe）

- 随版本号重建（1.3.3-local.ui）；命令行为不变（hard edge / LUT 平滑 / 阈值都由默认值生效）。
- 默认部署的 `fakerFG.ini` 已带 `TransportSlots=16` / `UiSynthThreshold=2` / `UiStats=60`。

## 4. 默认值变化（相对 1.3.1）

| 键 | 1.3.1 | 1.3.3 |
|---|---|---|
| `TransportSlots` | 32（硬编码） | **16** |
| `UiSynthThreshold` | 8（硬编码） | **2** |
| `UiStats` | 每帧 | **每 60 帧** |
| UI mode 4 边界 | 7×7 盒羽化 | **5×5 hard edge** |
| LUT 直方图 | 每帧清零 | **每帧衰减 92%** |

## 5. 实机验证（CP2077 4K / 40 fps / 3x keep2x）

- `pair: exact=40 no_slot=0` 全程（首窗口为启动 2 帧）、`xN=3.00`、`out 120/120`、
  `wait=0.0ms`、`slow/timeouts=0`；
- `--d3d-debug`：1045 条校验消息全部良性（`CheckFeatureSupport` / upload buffer
  `GENERIC_READ`），barrier/state/invalid-call 0 条；
- 映射 1017.6 MB（`ring=16`），`reclaimed oldest slot` 0 次；
- UI：阈值 2 + hard edge 后小 UI 移动抖动明显减小，H' 与呈现画面无可肉眼分辨的色差；
  差分支撑里残留的少量"正常画面小区块"是干净像素残差越过阈值，视觉无害（见 §2 的说明）。

## 6. 已知限制 / 未验证

- **F**（`--lock`+`--jbuf`）只做了代码修复，未实机验证；
- **Q**（consumer 放在工具目录、不被注入 fakerFG 的"出厂布局"）尚未实机对比；
- K（producer copy ring，启动 `copywait max=16 ms`）、L（LUT 重建间隔）、
  8×8 group 最少命中数、编码器跳空 tile 仍未做；
- 4x 仍不推荐（边缘模糊带比 3x 宽）；port 模型仍仅实验。

## 7. 部署 / 回滚

- 把包内 `lock_frame_ui.exe` 放在任意可写目录；UI 会把 `fakerFG.dll` / `xess_mfg_producer.dll`
  等装进游戏目录，并重写 `fakerFG.ini`（原文件有备份）。
- 本次实测部署：`xess_mfg_producer.dll` 需**重启游戏**；`lock_frame.exe` 可随 UI 重启。
- 回滚：UI → 「还原 / 移除组件」；或恢复 `dxgi.dll.bak` / `nvngx_dlssg_real.dll`。
