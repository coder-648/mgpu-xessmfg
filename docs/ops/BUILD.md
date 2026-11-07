# 构建

三件产物、一套工具链。**构建必须在 Windows 侧完成**（MSVC + Windows SDK），
WSL 里的 `scripts/build_*.sh` 只是驱动：把源码拷到 `<STAGE_DIR>` 再调 `cmd.exe`。

```
build/game_capture.addon64   ReShade add-on（游戏进程）
build/bridge.exe             消费端 + XeFG + 覆盖窗口
build/mgpu_ui.exe            前端（可选，但推荐）
```

## 1. 前置

| 组件 | 说明 |
|---|---|
| Windows SDK 10.0.26100+ | `dxgi` / `d3d12` / `dcomp` / `windows.graphics.capture` |
| MSVC v143（VS 2022 Build Tools） | 装 `Microsoft.VisualStudio.Workload.VCTools` |
| WSL（可选） | 只是为了跑 `scripts/*.sh`；纯 Windows 也能直接调 `build.bat` |
| Python 3.10 + numpy | 只给 `scripts/analyse_dump.py`（分析 `--dump` 输出）用 |

第三方 SDK 不随仓库分发，按 `docs/ops/DEPENDENCIES.md` §1/§3 的钉住 commit 拉：

```bash
git clone --depth 1 --branch v6.8.0  --filter=blob:none --sparse https://github.com/crosire/reshade third_party/reshade
git clone --depth 1 --branch v3.0.2  --filter=blob:none --sparse https://github.com/intel/xess      third_party/xess
git clone --depth 1 --branch 8af6b4a --filter=blob:none --sparse https://github.com/TsudaKageyu/minhook third_party/minhook
```

`third_party/ffx/` 已在版本库里（头文件 + 两个 `.lib`），不用拉。

## 2. 构建

```bash
./scripts/build_bridge.sh                                  # -> build/bridge.exe
./scripts/build_addon.sh src/game_capture game_capture      # -> build/game_capture.addon64
./scripts/build_ui.sh                                       # -> build/mgpu_ui.exe
```

脚本行为一致：

1. 把源码拷到 `$MGPU_STAGE/<module>`（默认 `/mnt/c/temp/mgpu/<module>`）；
2. `cmd.exe /c build.bat <repo-win-path>` 编译，日志转成 UTF-8 打出来；
3. 产物拷回 `build/`。

`build_bridge.sh` 还会把 `third_party/xess/bin/` 里的两个运行时 DLL 一起拷到
staging 目录，这样 `bridge.exe` 能直接跑。

> `MGPU_STAGE` 必须是 **Windows 可见** 的路径：`cl.exe` 不接受 UNC 路径当工作目录，
> 所以不能让它在 `\\wsl$\...` 里编译。换盘符见 `docs/ops/CONFIG.md` §2。

## 3. 纯 Windows 构建

不想用 WSL 就进各模块目录直接跑 `build.bat`（参数是 repo 的 Windows 路径）：

```bat
cd src\bridge        && build.bat D:\src\mgpu
cd src\game_capture  && build_addon.bat D:\src\mgpu unity.cpp game_capture.addon64
cd src\mgpu_ui       && build.bat D:\src\mgpu
```

## 4. 打包（给别人用）

### 4.1 只要游戏侧（小）

```bash
./scripts/make_addon_package.sh          # dist/mgpu_reshade_pack/ + .zip
SKIP_TOOLS=1 ./scripts/make_addon_package.sh      # 只要 add-on
RESHADE_SRC=/path/ReShade64.dll ./scripts/make_addon_package.sh
```

详见 `docs/ops/DEPLOY.md`。打包会做三件容易忘的事：给 `.ps1` 补 UTF-8 BOM、
把 `.bat` 保持纯 ASCII、装完做 SHA256 校验。

### 4.2 完整发布包（工具 + 游戏侧 + 文档）

```bash
./scripts/make_release.sh                        # 缺的产物会先编，然后打包
REBUILD=1 ./scripts/make_release.sh              # 强制重编三件产物
MGPU_VERSION=0.10-demo ./scripts/make_release.sh # 自定义版本号（默认 0.10-<sha>）
SKIP_XESS_DLLS=1 ./scripts/make_release.sh       # 不含 Intel 的 DLL（公开分发用）
RESHADE_SRC=/path/ReShade64.dll ./scripts/make_release.sh
```

产出 `dist/mgpu-<版本>/` 与同名 `.zip`：

| 路径 | 内容 |
|---|---|
| `README.md` | = `docs/ops/USAGE.md`（面向最终用户） |
| `LICENSE` `NOTICE` | GPL-3.0 + 第三方署名 |
| `SHA256SUMS.txt` | 全部文件校验值（38 个） |
| `bin/` | `bridge.exe` `mgpu_ui.exe` 两个 XeSS DLL + 4 份配方脚本 + `0 - UI.bat` |
| `game-pack/` | `make_addon_package.sh` 的产物（ReShade 6.8 + add-on + 安装器） |
| `docs/` | 架构 / 协议 / 输入源 / 配置 / 使用 / 许可 / 结论 / 画质变量 |

> `libxess_fg.dll` / `libxell.dll` 是 Intel 的，按 Intel 许可使用：
> **公开分发请加 `SKIP_XESS_DLLS=1`**，让用户自己从 XeSS SDK 取（见 `docs/ops/LEGAL.md` §3）。

## 5. 测试程序（历史探针）

`tests/` 与 `src/m2_probe`、`src/m3_depth`、`src/bridge/osd_test.cpp` 是各里程碑的
验证程序，**不参与正式构建**，保留是为了"当时的结论是怎么测出来的"可追溯：

```bash
./scripts/build_m0.sh  m0c_payload      # 跨卡共享堆 + fence 的最小验证
./scripts/build_osd_test.sh             # 覆盖窗口 / OSD 单元
```

说明见 `tests/README.md`。
