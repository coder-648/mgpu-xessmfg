# lock_frame 1.0.0

**双卡帧生成正式版 · 固定倍率 2x / 3x / 4x · Shared Protocol v28 · GPL-3.0**

游戏照常在自己的 GPU 上渲染 + 超分；`lock_frame` 在另一块 GPU 上用 XeSS-FG
插帧，再叠加到游戏窗口上屏。颜色走 WGC，跨卡只传 MV + depth。

配套源码提交：`265fa88`（release 代码提交 `937454d`）

## 下载

| 文件 | 内容 | SHA-256 |
|---|---|---|
| `lock_frame-1.0.0.zip` | 运行包（不含 Intel DLL） | `872b7562b23344be36012155a9a5f4ad0da073ff03654c20258f0b337860c8f4` |
| `lock_frame-1.0.0-src.zip` | GPL-3.0 对应源码 | `eeee0c66f6558320991865c7ba50b49498b2d4866dddef1ff692152de3546d0f` |

> 运行包默认不包含 `libxess_fg.dll` / `libxell.dll`（Intel 许可）。请从
> Intel XeSS SDK 获取原版文件，放到 `lock_frame_ui.exe` 同目录。
>
> 源码包默认不包含根目录 `third_party/`；按 `docs/ops/DEPENDENCIES.md`
> 拉取 ffx / streamline / minhook / dlss 与 Intel XeSS SDK。

## 本次更新

- **固定倍率 2x / 3x / 4x**：120Hz 面板分别锁 60 / 40 / 30 fps，输出恒定 120。
- **UI 支持目标显示器刷新率**：自动计算建议输入锁帧；不能整除时明确提示。
- **fakerFG 输入源可选**：DLSSG（默认，已实测）/ NVNGX / Upscaler / FSR；
  UI 写入并备份游戏目录 `fakerFG.ini`，同时强制 `FgOutput=Capture`、
  `Passthrough=0`。
- **环境自检**：版本/协议、GPU 数量、`libxess_fg`/`libxell`、游戏配置四项。
- **发布工程化**：manifest、SHA-256、GPL 源码包、`deploy_game.bat`、
  `collect_diagnostics.bat`、日志轮转。
- **2x 不再需要解锁补丁**；3x/4x 继续使用 `--unlock-mfg`。

## 核心数据

| 项目 | 实测/默认 |
|---|---|
| 4x（旧 30x4）长跑 | 精确配对约 99.2%，输出约 99.77%，`show=118~120` |
| 3x（旧 40x3）稳态 | `exact=40 no_slot=0`，`presents=120/120` |
| 延迟（120Hz 填满） | 2x@60 ≈ 40–48ms；3x@40 ≈ 55–65ms；4x@30 ≈ 73–82ms |
| 跨卡流量（4K，150%） | MV+depth ≈ 28MB/帧（不再传 4K 颜色） |
| 共享 ring | 32 槽；v28 控制块与数据区分离 |
| 3x/4x 依赖 | `--unlock-mfg`；实测 `libxess_fg 1.3.1.78` / `libxell 1.3.2.10` |

## 安装

1. 解压到一个可写目录（不要只解压到游戏目录）。
2. 运行：
   ```bat
   deploy_game.bat "F:\...\游戏 exe 目录"
   ```
   脚本会备份/改名并写入 `fakerFG.dll`、`dxgi.dll`、`nvngx_dlssg.dll`、
   `xess_mfg_producer.dll`；`fakerFG.ini` 只在缺失时放入模板。
3. 把 `libxess_fg.dll` / `libxell.dll` 放到 `lock_frame_ui.exe` 同目录。
4. 启动游戏，进入画面，开启 DLSS Frame Generation。
5. 运行 `lock_frame_ui.exe`：选择游戏窗口、固定倍率、输入源 `DLSSG`，启动。
6. 游戏侧锁帧：120Hz 面板下 2x 锁 60、3x 锁 40、4x 锁 30。

其他刷新率：输入 = 刷新率 / 倍率；不能整除时 UI 会提示。

## 已验证 / 支持

- **FgInput=DLSSG**：Cyberpunk 2077、PBZ、RED 引擎、UE5、UE4。
- `NVNGX` / `Upscaler` / `FSR` 为实验性输入车道，需要按游戏验证。
- 推荐双 GPU；单卡可跑但不建议。
- 只支持无边框/窗口化全屏。

## 已知限制

- 3x/4x 的 `--unlock-mfg` 会修改 Intel DLL 的机器码，只对实测版本成立，
  可能违反 Intel 许可/触发反作弊；失败会回退并打日志。2x 不需要它。
- 非整除刷新率（如 165Hz）不承诺完美节奏。
- 低帧自适应默认关闭；开启后可能有轻微 flicker/ghosting。
- 本包未签名；请用 `SHA256SUMS.txt` 或本页 SHA-256 校验。
- 不要在带反作弊的在线游戏中使用。

## 校验

```text
lock_frame.exe --version
lock_frame 1.0.0 (shared protocol v28)
```

包内 `manifest.json` 记录版本、协议、git 与逐文件 SHA-256；
`SHA256SUMS.txt` 可直接校验全部文件。

## 许可与致谢

本项目以 **GPL-3.0** 发布；`LICENSE` 与 `NOTICE` 随包提供，对应源码见
`lock_frame-1.0.0-src.zip`。

第三方项目：OptiScaler（GPL-3.0）、AMD FidelityFX SDK（MIT/AMD 许可）、
Universal MFG Unlocker（MIT）、nlohmann/json（MIT）、MinHook（BSD-2-Clause）、
ReShade（BSD-3-Clause）、Intel XeSS SDK（Intel 许可，默认不分发）、
NVIDIA DLSS SDK / Streamline（NVIDIA 许可）、Magpie（GPL-3.0）、
dlssg_for_sm86（GPL-3.0，NVIDIA 组件不转授权）、Microsoft Windows SDK /
DirectX 12。完整说明见 `NOTICE` 与 `docs/ops/LEGAL.md`。
