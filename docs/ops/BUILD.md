# 构建（当前链路，2026-11-14）

当前产物四件（+ 一个 legacy bridge）：

```text
build/fakerFG.dll            游戏侧 FG 管线（部署为 dxgi.dll + nvngx_dlssg.dll）
build/xess_mfg_producer.dll  跨卡传输（fakerFG 加载）
build/xess_mfg_consumer.exe  当前消费端（WGC + XeFG + 覆盖窗口）
build/mgpu_ui.exe            前端启动器
build/bridge.exe             旧消费端（legacy，仅 A/B）
```

构建必须在 **Windows 侧**完成（MSVC + Windows SDK）。WSL 里的
`scripts/build_*.sh` 只是驱动：把源码拷到 `<STAGE_DIR>` 再调 `cmd.exe`。

旧构建说明（add-on / bridge）已归档到 [`archive/BUILD_legacy.md`](archive/BUILD_legacy.md)。

---

## 1. 前置

| 组件 | 说明 |
|---|---|
| Windows SDK 10.0.26100+ | `dxgi` / `d3d12` / `dcomp` / WGC / `fxc.exe` |
| MSVC v143（VS 2022 Build Tools） | `Microsoft.VisualStudio.Workload.VCTools` |
| WSL（可选） | 只用于跑 `scripts/*.sh` |
| Python 3.10+ | `embed_cso.py`（着色器内嵌）、`extract_hlsl.py`（运行时着色器校验）、`analyse_dump.py` |
| 第三方头/库 | 见 §2 |

脚本里钉的 `fxc` 路径是：

```text
C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\fxc.exe
```

SDK 版本不同的话，改 `scripts/build_*.sh` 里的 `FXC_WIN`。

---

## 2. 第三方依赖

`third_party/` **整体不入库**（`.gitignore` 的 `/third_party/`）。本地必须按
`docs/ops/DEPENDENCIES.md` 的版本放到以下路径：

| 路径 | 内容 |
|---|---|
| `third_party/xess/inc` + `third_party/xess/lib` | XeSS/XeFG/XeLL 头与 `libxell.lib`（**SDK 需自行拉取**） |
| `third_party/ffx/` | FidelityFX SDK 头 + `ffx_opticalflow_x64.lib` / `ffx_backend_dx12_x64.lib` |
| `third_party/minhook/` | fakerFG 的 MinHook（源码随构建编译） |
| `third_party/streamline/` `third_party/dlss/` | Streamline / DLSS 头（只作声明参考） |
| `third_party/optiscaler/` `third_party/Magpie-experimental/` 等 | 只作参考；不参与 lock_frame 构建 |

唯一随仓库跟踪的第三方源码是 `lock_frame/third_party/nlohmann/json.hpp`
（MIT，单头文件）。

拉取示例：

```bash
git clone --depth 1 --branch v3.0.2 --filter=blob:none --sparse https://github.com/intel/xess third_party/xess
git clone --depth 1 --branch 8af6b4a --filter=blob:none --sparse https://github.com/TsudaKageyu/minhook third_party/minhook
```

> `third_party/xess/bin/` 里的 `libxess_fg.dll` / `libxell.dll` 是 Intel 的，
> 不能随仓库/公开包分发。

---

## 3. 构建命令

```bash
./scripts/build_faker_fg.sh      # -> build/fakerFG.dll
./scripts/build_producer.sh      # -> build/xess_mfg_producer.dll
./scripts/build_consumer.sh      # -> build/xess_mfg_consumer.exe
./scripts/build_ui.sh            # -> build/mgpu_ui.exe
./scripts/build_bridge.sh        # -> build/bridge.exe（legacy）
```

脚本行为一致：

1. 跑结构校验（`check_bridge_modules.py`，bridge/consumer）；
2. 把源码 staging 到 `$MGPU_STAGE/<module>`（默认 `/mnt/c/temp/mgpu`）；
3. 用 SDK 的 `fxc` 编译并内嵌着色器（fakerFG / producer / consumer）；
4. `cmd.exe /c build.bat <repo-win-path>` 编译；
5. 产物拷回 `build/`。

`build_faker_fg.sh` 额外把 `third_party/xess/bin/` 的两个运行时 DLL 拷到 staging，
`build_consumer.sh` 也会把 XeSS DLL 拷到消费端目录，方便直接跑。

> `MGPU_STAGE` 必须是 **Windows 可见** 路径：`cl.exe` 不接受 UNC 工作目录。

---

## 4. 纯 Windows 构建

```bat
cd src\faker_fg          && build.bat D:\src\mgpu
cd src\xess_mfg_producer && build.bat D:\src\mgpu
cd src\xess_mfg_consumer && build.bat D:\src\mgpu   rem modules\ 必须在旁边
cd src\mgpu_ui           && build.bat D:\src\mgpu
cd src\bridge            && build.bat D:\src\mgpu   rem legacy
```

> fakerFG/producer 的 `build.bat` 在 `scripts/build_*.sh` 里生成/调用；
> 纯 Windows 构建时请确认它与 unity.cpp、modules 相对位置正确。

---

## 5. 打包

```bash
./scripts/make_release_fg.sh                 # 缺产物时先编，然后打包
REBUILD=1 ./scripts/make_release_fg.sh       # 强制重编四件
MGPU_VERSION=0.16-local ./scripts/make_release_fg.sh
SKIP_XESS_DLLS=1 ./scripts/make_release_fg.sh   # 不带 Intel DLL（公开包）
```

产出 `dist/mgpu-<ver>/` 与同名 `.zip`：

| 路径 | 内容 |
|---|---|
| `game/` | fakerFG 三副本 + producer + consumer + UI + ini + `run_consumer.bat` |
| `README.md` | = `scripts/release_README.md` |
| `docs/` | `PERF_ROUND_HANDOVER.md`（发布时只带这一份性能说明） |
| `SHA256SUMS.txt` | 全部文件校验值 |

> 当前 `make_release_fg.sh` 默认版本是 `0.11-<sha>`，与 CHANGELOG 的版本号不一致；
> 这是已知问题，见 `docs/PROJECT_STATUS_2026-11-14.md` §2.6。

旧发布脚本（`make_release.sh` / `make_addon_package.sh` / `make_local_release.sh`）
属于 bridge/ReShade 时代，不要用于当前链路。

---

## 5.1 lock_frame 1.0.0 构建与发布

```bash
./lock_frame/build.sh all        # consumer / producer / fakerFG / ui
./lock_frame/release.sh          # 用现有产物打包
BUILD=1 ./lock_frame/release.sh  # 先全量构建再打包
VERSION=1.0.0 ./lock_frame/release.sh
WITH_XESS=1 ./lock_frame/release.sh   # 可选：内置 Intel 运行时（附许可）
WITH_THIRD_PARTY=1 ./lock_frame/release.sh  # 可选：源码包放入本地 third_party
```

可覆盖：`MGPU_STAGE`、`MGPU_FXC`、`MGPU_VSDIR`。

产物：

| 路径 | 内容 |
|---|---|
| `dist/lock_frame-<ver>.zip` | 运行包（默认不含 Intel DLL） |
| `dist/lock_frame-<ver>-src.zip` | GPL-3.0 对应源码（含 ffx/streamline/minhook/dlss） |
| `.../manifest.json` | 版本、协议、git、逐文件 SHA-256 |
| `.../SHA256SUMS.txt` | 校验清单 |

打包后自动运行 `lock_frame/scripts/check_release.py`，检查必需文件、禁止
`.pdb/.map/.log`、`fakerFG.ini` 默认键与全部校验值。

---

## 6. 测试与检查

```bash
python3 scripts/check_bridge_modules.py src/bridge
python3 scripts/check_bridge_modules.py src/xess_mfg_consumer
python3 scripts/check_fakerfg_regression.py <日志路径>
```

- `tests/`、`src/m2_probe`、`src/m3_depth` 是历史探针，不参与正式构建；
- 当前没有 CI；建议的测试分层见 `docs/PROJECT_STATUS_2026-11-14.md` §2.7。

---

## 7. 常见构建问题

| 现象 | 原因 |
|---|---|
| `fxc.exe not found` | Windows SDK 版本/路径不对，改 `FXC_WIN` |
| `modules\ is missing` | staging 时没把 `modules/` 放到 `unity.cpp` 旁边 |
| `cl.exe` 不能打开 UNC 路径 | `MGPU_STAGE` 指到了 `\\wsl$\…` |
| `libxell.lib` / FFX lib 找不到 | 缺 `third_party/xess` 或 `third_party/ffx` |
| 消费端起不来但编译成功 | 同目录缺 `libxess_fg.dll` / `libxell.dll` |
| 运行时版本不匹配 | 四件产物不是同一次构建（协议 v17 硬拒绝） |
