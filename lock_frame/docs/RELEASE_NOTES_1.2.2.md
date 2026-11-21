# lock_frame 1.2.2 发布说明（本地版）

**1.2.2 是本地验证版**：默认行为仍是 2x/3x/4x + 2x 模型复用；
新增的 model 15/16 真多帧入口只作为显式实验，不改变默认画质路径。

## 捕获

- WGC 窗口捕获固定使用 **layered overlay + `--wgc-minint 0`**：
  `--overlay-layered --wgc-minint 0`。
- 2x 实测 `show` 中位 119、`skip 0`；3x/4x 也能稳定到 120。
- `--wgc-minint -1`（Windows 默认 15–16ms）会卡高帧捕获，不使用。
- `--capture-monitor` / DXGI Desktop Duplication 不作为默认：topmost overlay 下
  显示器级捕获会拍到 overlay 或让被遮挡的游戏停止更新（实测 monitor WGC 为 0 帧、
  画面冻结）。继续使用兼容性更好的 WGC 窗口捕获。

## MFG（model 15/16）实验

- 新增 `--mfg-model=keep2x|port`，**默认 keep2x**。
- `port` 走 MFG-Unlocker 的 ShaderPort + ModelPort（model 15 → 16）+ 5 站点解锁：
  54 个 portable DXIL 程序、7 个 replacement shader（AMD 保留 2 个 Intel U-Net）。
- CP2077 4K 实测：3x `xN=3.00`/`show=120`、4x `xN=4.00`/`show=118–120`，无 XeFG 错误；
  但 4x 真模型的屏幕边缘伪影明显比 keep2x 严重，`--fg-use-matrix`、`--no-upsample`、
  jitter/scene 调整均无改善。
- 因此 **3x/4x 默认仍是 keep2x**；真模型只在 `run_3x_true_model.bat` /
  `run_4x_true_model.bat`（本地测试脚本）或命令行显式 `--mfg-model=port` 时启用。

## 本地实验资产

本地包可带 `xefg_programs/`（54 程序 + 7 shader）。它是 Intel 派生的研究资产，
**不要随公开发布包分发**；没有它时 `port` 会回退 keep2x 并记录 `assets_incomplete`。

## 兼容性

- 共享协议仍为 **v28**；游戏侧 `fakerFG.dll` / `xess_mfg_producer.dll` 必须与本包同版本。
- UI 默认 `overlay_layered=true`；2x/3x/4x 的主脚本也已经使用 layered + minint0。
- 升级：关闭游戏与旧 UI → 解压覆盖 → UI 选游戏 → [重新应用配置] → 重启游戏。
