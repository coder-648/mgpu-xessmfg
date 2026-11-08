# 画质与预算：变量作用表

2026-11-08 用 `--mv-shadow` / `--probe-colour` / `--dump` / `--fg-rt-mode` 四把
探针把画质从"玄学"变成了"可测量"。这张表是那一轮的产出，**改任何参数前先看它**。

## 1. 变量作用表（全部实测）

| 变量 | 对画质 | 对性能 | 结论 |
|---|---|---|---|
| **MV 开 / 关** | ✅ 有影响 | 游戏 MV：渲染卡 +7.4 ms | XeFG **确实消费 MV**（三组对照推翻了"死输入"） |
| **MV 方向** | ✅ | — | `--mv-flip` 更差 ⇒ 当前方向正确 |
| **MV 来源** | ✅✅ | 游戏 MV +7.4 ms / **光流仅 0.3 ms** | 3x 用 **AMD 光流**最干净；2x 用游戏 MV |
| **jitter** | ✅ | 无 | 必须传**游戏自己的**；`--fg-jitter-zero` 已弃用 |
| **倍率 2x / 3x / 4x** | ✅✅ | 越大越贵 | 3x 是 MFG 补丁（2x 模型跑两遍）⇒ 游戏 MV 会闪，光流不闪 |
| 深度（有无 / 延迟 1..3） | ❌ 无差别 | 2 ms | 别在这花时间 |
| 投影 / FOV（30 / 58.7 / 120） | ❌ **完全无差别** | 无 | 这个 FG 只吃彩色 + MV |
| **帧时间吸附** `--fg-rt-mode quant` | ✅ 平滑度（治周期性丢帧） | 无 | **默认开**：实测间隔吸附到整 vblank，2x@60 → 16.67 ms → 120.0 fps 整拍 |

## 2. 定稿配方

| 取向 | 配置 | 输出 | 画质 |
|---|---|---|---|
| **3x 满帧（推荐）** | `--interp 2 --fg-of` | 120 fps 整拍 | 光流 MV、无闪烁、`gpu` 9 ms |
| 2x 画质优先 | `--interp 1` + 游戏 MV（`SkipMv=0`）+ 游戏 jitter（锁 60 fps 则 120） | 80 / 120 fps | 边缘伪影最少 |
| 最低开销 | `--interp 2 --no-mv` | 120 fps | 快转时边缘更脏 |

对应的可执行脚本在 `scripts/recipes/`：

| 脚本 | 配方 |
|---|---|
| `run_3x_of.bat` | 3x 满帧 + 光流（推荐） |
| `run_2x_quality.bat` | 2x 画质优先 |
| `run_3x_nomv.bat` | 最低开销 |
| `run_3x_of_nodepth.bat` | 推荐配方的 A/B 版：`--no-depth`，用来量跨卡总线省了多少（`PERF_BASELINE.md` §5） |
| `run_dump.bat` | 抓 colour/depth/MV 做离线对比 |

## 3. 怎么自己复测

```bat
rem 三组 A/B：关 MV / 游戏 MV / 光流，其他参数保持一致
bridge.exe --game Game.exe --interp 2 --no-mv   --fg-ui none
bridge.exe --game Game.exe --interp 2           --fg-ui none
bridge.exe --game Game.exe --interp 2 --fg-of   --fg-ui none
```

判据：

- **画面**：看边缘（人的轮廓、栏杆、快速平移的景深边界）。开 MV 与关 MV 若完全
  一致，说明 MV 没进来（检查 `SkipMv=0` 与日志里的 `mv=` 字段）；
- **配对**：日志每 40 帧打印的 `gen=` 必须是 `N/N`（例如 `3/3`）。`gen 2/3` 说明
  生成帧没凑齐；
- **预算**：`gpu=` 是消费端每帧 GPU 时间，`slow=` / `skip=` 是迟到/丢帧计数。
  `slow` 常年非 0 说明 `gpu + present` 顶破了输入间隔预算（锁帧没锁准或插帧卡余量不足）。

## 4. `--dump` 的用法

```bat
run_dump.bat Game.exe 5 D:\mgpu_dump
```

每帧写三个文件（`f<NN>_colour.raw` / `_depth.raw` / `_mv.raw`）+ 一个 `_meta.txt`，
header 是 4 个 uint32（width / height / dxgiFormat / rowPitchBytes）。分析：

```bash
./scripts/analyse_dump.sh <DUMP_DIR>
```

> dump 会等 GPU，所以那一跑的 `gpu`/`show` 数字没有参考价值，只看数据。
> 想比"光流 MV vs 游戏 MV"：一次 `SkipMv=0`（tag 游戏 MV）、一次 `--fg-of`，
> 然后逐像素比 `_mv.raw`。
