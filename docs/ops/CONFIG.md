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
| `--input-ms X` | 把"输入间隔"当成 X ms。**这是"容忍迟到"的旋钮**，不是设置刷新率；3x 建议 `33.3` |
| `--no-mv` / `--fg-of` | 前者完全不产 MV，后者改用 AMD 光流算 MV。**两者互斥** |
| `--fg-jitter-zero` | **已弃用**：只打一行提示，不再生效。jitter 必须用游戏自己的 |

限帧器（`<RTSS_DIR>`）：**必须用外部限帧器把游戏锁到目标输入帧率**，
消费者侧锁帧（`--lock` / `--lock-auto`）做不到这件事，原因见
`docs/findings/FINAL_CONFIG.md` §9.16。
