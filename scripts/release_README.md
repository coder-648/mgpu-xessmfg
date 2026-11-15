# mgpu 双卡帧生成 · 本地版本

把 **`game/` 目录里的所有文件**复制到游戏的 exe 目录（例如
`...\Cyberpunk 2077\bin\x64`），然后按下面顺序跑。

## 一次性准备

1. 两块显卡都装好；**显示器接在插帧卡**（本机 RX 6750 GRE），另一张卡（RTX 3080 Ti）
   只负责渲染。
2. 游戏设成**无边框全屏**，分辨率与显示器**完全一致**（1:1）。缩放会让内嵌标记错位，
   消费端会报 `present size != window client`。
3. 在驱动里把**游戏帧率锁到 40**（120 Hz 面板的整数 vblank：40 fps → 25 ms = 3×8.33 ms）。
   **不要**用 `--lock` 去压游戏（那是丢帧，历史实测 151 次卡顿就是它）。
4. HDR 打开时输出走 HDR10（BT.2020 + PQ）；SDR 游戏需要对应参数（见下）。

## 启动

1. 进游戏到**实际场景**（不是主菜单，菜单里游戏不产帧）；
2. 编辑 `run_consumer.bat`，把 `set "GAME_EXE=..."` 改成游戏的进程名；
3. 双击 `run_consumer.bat` —— 出现一个无边框窗口，那就是最终画面（2X，80 fps）。

开机自检看这几行（`mgpu_consumer.log`）：

```
[consumer] capture item 3840x2160
[consumer] present 3840x2160 == window client 3840x2160 (1:1, no stretch)
[consumer] marker code: seq12=… crc8=… ok
[consumer] cadence: input=25.0ms = 3.00 vblanks … xN=2.00
```

## 日志（出问题先看这三个）

| 文件 | 内容 |
|---|---|
| `mgpu_consumer.log` | 消费端：`HITCH gap=` / `SLOW frame:` / `marker:` / `pf:` / `breakdown:` |
| `<游戏exe>_fakerFG.log` | 游戏侧：`marker draw:` / `publish #N` / `xefg: 2X present status` |
| `<游戏exe>_mfg_producer.log` | 跨卡 publication：`publish #N … mv=… depth=…` |

健康标准：`marker: decoded` 随帧增长、`miss` 个位数、`HITCH` 每分钟 0~2 次、
`arr(vb)` 集中在整数 vblank（40fps@120Hz → `3x=40`）。

## 图形界面（可选，`mgpu_ui.exe`）

`mgpu_ui.exe` 和消费端放同一目录，双击即可。它会：

- 列出可见窗口，选哪个就用 `--hwnd` 让消费端抓哪个（等于 `--game <进程名>`）；
- 让你挑倍数 / 帧池深度 / 色彩空间 / 叠加层，并在「启动」卡片里给出**命令行预览**；
- 启动后底部状态栏实时显示消费端的统计行（解析 stdout）。

注意：
- UI 里的「建议锁帧」只是**提示**（不发给消费端），真正要锁的是**游戏本身**
  （驱动 Max Frame Rate = 40）；
- 「不传深度」「关闭标记配对」就是 `--no-depth` / `--no-marker`；
- 消费端必须和 `mgpu_ui.exe` 在同一目录（它按自己的路径去找
  `xess_mfg_consumer.exe`）。

## 常用开关（加在 `run_consumer.bat` 的命令行里）

| 参数 | 作用 |
|---|---|
| `--no-depth` | 不传深度：跨卡 28.1 → 14.1 MB/帧。带宽/拷贝队列紧张时先试这个 |
| `--no-mv` | 不传 MV（改用零向量/本地光流），画质下降 |
| `--no-marker` | 关掉标记配对（退回时间戳/FIFO），**不推荐**，会偶发错配 |
| `--capture-monitor` | 抓显示器而不是窗口（有叠加层时用，但标记可能被盖住） |
| `--report 40` | 每 40 帧输出一次统计（日志更长，性能分析时用） |
| `--fg-rt-mode auto/input` | XeFG 的帧时间模型，画质/节奏二选一的 A/B 开关 |

调试开关（放在游戏目录，重启生效）：`marker_disable.on`（生产端不画标记）、
`marker_repair_off.on`（消费端不回填）、`dump_frame.on`（整帧回读 `frame_dump.raw`）。

## 已知限制

- 颜色必须走 WGC（不跨卡），跨卡的只有 MV/depth；
- **游戏侧那一帧的四角有 4 个 6×6 的小标记块**（配对用，属设计），消费端呈现的画面干净；
- 输出是 HDR10（BT.2020 + PQ）；
- 本包内含 Intel 的 `libxess_fg.dll` / `libxell.dll`（本地测试用，**不要公开分发**）。

细节（日志字段含义、基线数字、优化杠杆排序、五个坑）见
`docs/PERF_ROUND_HANDOVER.md`。

## 换到别的游戏时（重要）

1. **必须要游戏走 DLSS-G（或 FSR-FG）的 Evaluate 调用**：fakerFG 是在那里拿到 MV/depth 和
   帧序号的，标记也是那时开始画的。如果游戏完全不带帧生成：
   - 日志里不会有 `marker draw:`，也不会有 `publish #N`；
   - 配对会退回时间戳/FIFO，**可能偶发错配（重影）**；
   - 这时请用 `--fg-of`（消费端自己做光流）代替 MV，画质折中但能跑。
2. **游戏要无边框 / 窗口化全屏**：独占全屏时 WGC 抓不到（`CreateForWindow FAILED
   (exclusive fullscreen?)`）。
3. **分辨率必须 1:1**：游戏的渲染尺寸与显示器一致；不一致时日志会出现
   `present size … != window client …`，此时应调游戏分辨率或加 `--den` / `--scale`。
4. **帧率锁 40（或 30/60）**：这是"整数 vblank"的前提，`--lock` 不要用来压帧率。
5. **SDR 游戏**：加 `--sdr`（输出 8-bit gamma 而不是 HDR10 PQ）。HDR 游戏不要加。
6. 每个游戏的 HUD/标记遮挡不同：若某角长期读不出，日志会是 `only 3/4 corner blocks
   readable`，此时把游戏窗口的 HUD 布局挪一下（或接受该角失效，另三角仍能拼出码）。

## 干净的卸载

删掉 `dxgi.dll` / `nvngx_dlssg.dll` / `fakerFG.dll` / `xess_mfg_producer.dll` 即可；
`fakerFG.ini`、`*.log`、`libxess_fg.dll` 可留可删。（游戏目录里原本的 `dxgi.dll`
如果被替换过，仓库里 `deploy_faker_fg.sh --uninstall` 会从 `.bak` 还原。）
