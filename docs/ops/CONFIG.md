# 配置速查：占位符、环境变量、ini 键

这份文档解释 `docs/` 与脚本里出现的占位符和可调项。**仓库里没有任何本机路径**
—— 看到 `<GAME_DIR>` 这类尖括号就是要你填自己的位置。

## 1. 路径占位符

| 占位符 | 含义 | 例 |
|---|---|---|
| `<GAME_DIR>` | **直接包含游戏 exe** 的那一层目录 | `D:\Games\XXX\bin\x64` |
| `<TOOLS_DIR>` | `bridge.exe` + `mgpu_ui.exe` + 两个 XeSS DLL 放一起的目录 | `D:\mgpu\bin` |
| `<STAGE_DIR>` | WSL 侧构建的中转目录，**必须是 Windows 可见的路径**（`cl.exe` 不能用 UNC 路径当工作目录） | `C:\temp\mgpu`（默认） |
| `<DUMP_DIR>` | `bridge.exe --dump` 写 `.raw` 的目录 | `<TOOLS_DIR>\dump` |
| `<PYTHON>` | 带 numpy 的 python（`scripts/analyse_dump.sh` 用） | `C:\Python\Python310\python.exe` |
| `<RTSS_DIR>` | RivaTuner Statistics Server 安装目录（限帧器） | 见 §4 |
| `<BUILDTOOLS_DIR>` / `<MINGW64_DIR>` | MSVC Build Tools / 可选的 mingw 工具链 | 见 `docs/ops/BUILD.md` |

`<GAME_DIR>` 一定要指向 exe 所在层，不是上一级：`...\Cyberpunk 2077\bin\x64`、
`...\PBZ\platform\steam` 都是"exe 所在层"的例子。

## 2. 环境变量

| 变量 | 作用 | 默认 |
|---|---|---|
| `MGPU_STAGE` | 覆盖 `<STAGE_DIR>`（构建脚本与打包脚本共用） | `/mnt/c/temp/mgpu` |
| `RESHADE_SRC` | 打包时指定 ReShade 6.8 x64 DLL 的路径 | 依次找 `$MGPU_STAGE/ReShade64.dll` |
| `SKIP_TOOLS` | `=1` 表示打包只要 add-on，不带 `bridge.exe` / `mgpu_ui.exe` | `0` |
| `REBUILD` | `=1` 强制重新编译 add-on 再打包 | `0` |
| `MGPU_CAPTURE_LOG` | `=1` 打开 add-on 日志（等价于 ini 里 `Log=1`） | 关 |

```bash
MGPU_STAGE=/mnt/d/mgpu_stage ./scripts/build_bridge.sh
RESHADE_SRC=/mnt/d/ReShade64.dll ./scripts/make_addon_package.sh
```

## 3. `game_capture.ini`（放在 add-on 旁边，游戏目录里）

| 键 | 取值 | 说明 |
|---|---|---|
| `Color` | `0` / `1` | `0`（默认）= 不做颜色跨卡拷贝，颜色由 bridge 用 WGC 从屏幕抓；`1` = 从后台缓冲拷贝 |
| `StreamPercent` | `100`…`200` | `>100` 表示 MV/深度先缩小再跨卡（`150` = 2160p → 1440p） |
| `StreamAlign` | `0` / `1` | 行对齐处理，默认 `1`，别乱改 |
| `SkipMv` | `0` / `1` | `1` = 永不产出 MV；`0` = 听 bridge 的请求位（协议 v7） |
| `DepthConvert` | `0` / `1` | **默认 `1`**：把深度转成 R32_FLOAT 再共享。缺这个键时旧版本会静默失效 |
| `Input` | `auto` / `ngx` / `ffx` / `xess` / `depth` | 输入源；`auto` 先到先用。见 `docs/design/INPUT_SOURCES.md` |
| `Log` | `0` / `1` | 默认关，排查时开 |
| `SlotPolicy` | `0` / `1` | `0` = 没空槽就丢这一帧（实时默认）；`1` = 最多等 `SlotWaitUs` 微秒。**实测 `1` 没有收益**，当遗留开关看 |
| `SlotWaitUs` | 微秒 | 仅 `SlotPolicy=1` 时生效，默认 `2000` |
| `RenderWidth` / `RenderHeight` | ≥256 才生效 | 强制渲染分辨率（只在游戏上报错时用） |
| `MvNormalize` / `MvFlip` | `0` / `1` | MV 单位/符号修正，默认全 `0` |

## 4. bridge 侧

参数全集见 `docs/findings/FINAL_CONFIG.md` §4，日常只用三份配方脚本
（`scripts/recipes/`，见 `README.md`）。最容易被误调的三个：

| 参数 | 说明 |
|---|---|
| `--input-ms X` | 把"输入间隔"当成 X ms（**仅影响 `load`/`slow` 统计**；`--fg-rt-mode quant` 下不再参与 pacing）。界面已不开放这一项 |
| `--no-mv` / `--fg-of` | 前者完全不产 MV，后者改用 AMD 光流算 MV。**两者互斥** |

### 4.1 上屏放哪块屏（2026-11-08 新增）

| 参数 | 说明 |
|---|---|
| `--overlay-monitor N` | 把覆盖窗口（也就是 XeFG 的上屏）放到**第 N 块显示器**（`EnumDisplayMonitors` 顺序，0 起）。默认跟随游戏窗口所在屏。它的镜像参数是 `--capture-monitor`（抓哪块屏）—— **抓屏和上屏是两个独立的决定**，可以抓屏 1 上屏 2 |
| `--overlay-hidden` | 只做测量：交换链照建、Present 照跑，但窗口不显示。用来分辨"抖动是我们这层覆盖窗口造成的"还是"WGC/合成器本来就抖"（实测见 `docs/findings/PERF_BASELINE.md` §3.12） |

两条硬性前提：

1. **显示器必须接在"跑 XeFG 的那张卡"上**。`--overlay-monitor` 指向的那块屏如果不由消费卡驱动，
   `CreateSwapChainForHwnd` 会 `E_ACCESSDENIED`，所以 bridge 会**先校验再回落**到游戏窗口所在屏，
   并在日志里写明原因。
2. **两块屏接同一张卡才有意义**。如果第二块屏接在渲染卡上，自动选卡会为了"驱动上屏屏"把
   XeFG 换到渲染卡（日志会出现 `note: same GPU as the producer`），此时插帧和游戏抢同一张 GPU，
   `gpu` 会从个位数涨到 60+ ms（实测）。正确形态是**两块屏都接帧生成卡**。

### 4.2 覆盖窗口在 Alt+Tab / Win+Tab 里只算一个"游戏"

覆盖窗口是**另一个进程里的另一个窗口**，一不小心就会在 Alt+Tab / Win+Tab 里变成第二个条目
（而且那个缩略图是**成品画面**，游戏的缩略图是**原始画面**，看起来像"两张图"）。
bridge 在窗口显示之后做四件事，日志里各占一行：

| 手段 | 日志 | 作用 |
|---|---|---|
| **创建时** `WS_EX_TOOLWINDOW` | 无 | 从源头就不登记为应用窗口（不进任务栏 / Alt+Tab / Task View） |
| `DWMWA_EXCLUDED_FROM_PEEK` / `DWMWA_DISALLOW_PEEK` | `switcher: peek excl hr=...` | 不进 Aero Peek / 缩略图预览 |
| `ITaskbarList::DeleteTab` | `switcher: ... DeleteTab hr=0x00000000` | 删掉可能已登记的条目 |

> ⚠️ **不要**用 `SetWindowLongPtr(GWLP_HWNDPARENT, 游戏窗口)` 把覆盖窗口变成游戏的"被拥有窗口"。
> 它确实能让 Task View 分组，但**实测会让游戏的鼠标输入完全失效（键盘仍正常）**：
> `click probe` 显示点击仍然落在游戏上（不是 hit-test 吃掉的），是 owner 关系改变了输入/激活语义。
> 覆盖窗口必须保持普通顶层窗口。

另外 **Task View / Alt+Tab / Win+D 不再算"焦点丢失"**：`explorer.exe` 的
`XamlExplorerHostIslandWindow` / `Windows.UI.Core.CoreWindow` / `TaskSwitcherWnd` /
`ForegroundStaging` / `Progman` / `WorkerW` / `Shell_TrayWnd` 这些窗口被显式排除，
所以按 Win+Tab 待 8 秒 bridge 也不会退出（实测 `alive after 8 s in Task View: True`）。
真正的其他应用（浏览器、另一个游戏）仍然会触发 `--exit-on-focus-loss`。

输入是否被吃掉，看这行（整场会话持续采样，前 12 次每 3 s、之后每 30 s）：

```
[bridge] click probe: cursor 1920,1079 under=...pid=22084 root=...game=...  (ok)
[bridge] click probe: ... <== OUR OVERLAY eats this click      ← 我们吃了点击
[bridge] click probe: ... <== the OSD window eats it           ← OSD 吃了点击
[bridge] click probe: ... <-- NOT the game                     ← 别的程序在前面（如 VS Code）
```



| `--fg-jitter-zero` | **已弃用**：只打一行提示，不再生效。jitter 必须用游戏自己的 |

限帧器（`<RTSS_DIR>`）：**必须用外部限帧器把游戏锁到目标输入帧率**，
消费者侧锁帧（`--lock` / `--lock-auto`）做不到这件事，原因见
`docs/findings/FINAL_CONFIG.md` §9.16。
