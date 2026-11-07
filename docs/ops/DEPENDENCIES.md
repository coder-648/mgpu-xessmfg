# 依赖组件（third_party）

记录**已拉入仓库**的依赖、版本、取用方式与更新命令。

> **版本控制（2026-11-07）**：本目录现在是 git 仓库根（`git init` 之后首次提交）。
> `third_party/` 下四个上游 SDK 的完整 checkout（xess / reshade / dlss / minhook）
> 是**各自独立的 git 仓库**，被 `.gitignore` 排除在外的同时，把**钉住的 commit** 记在下面 §1，
> 用 §3 的命令可以一键重建。真正进版本库的只有 `third_party/ffx/`（头文件 + 两个 .lib）
> 和 `third_party/optiscaler/`（GPL-3.0 的参考文件），共 ~5 MB。
>
> 许可：本仓库整体 **GPL-3.0-or-later**（根目录 `LICENSE` / `NOTICE`），逐项见 §4。

## 1. 已拉入

| 目录 | 来源 | 版本 / commit | 体积 | 取了哪些 | 进版本库 |
|---|---|---|---|---|---|
| `third_party/reshade/` | https://github.com/crosire/reshade | **v6.8.0**（`18deaa5`） | 7.7 MB | `include/`（8 个 `.hpp`）+ 根目录文件 | 否（外部仓库） |
| `third_party/xess/` | https://github.com/intel/xess | **v3.0.2**（`8fe81bd`，XeSS 3 SDK） | 162 MB | `inc/` `lib/` `bin/` `doc/` | 否（外部仓库；`bin/` 是运行时 DLL） |
| `third_party/dlss/` | https://github.com/NVIDIA/DLSS | **v310.9.16**（`9981c63`） | 1.1 MB | 只作参考（DLSS 输入约定） | 否（外部仓库） |
| `third_party/minhook/` | https://github.com/TsudaKageyu/minhook | `8af6b4a` | 436 KB | `include/` `src/`（add-on 的 Present 钩子） | 否（外部仓库） |
| `third_party/ffx/` | https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK | **SDK 1.1.4** | 4.9 MB | `include/FidelityFX/`（242 个头文件）+ `lib/ffx_opticalflow_x64.lib`、`lib/ffx_backend_dx12_x64.lib` | **是** |
| `third_party/ffx/api/` | 同上（tag `v1.1.4`，路径 `ffx-api/include/ffx_api/`） | **SDK 1.1.4** | 60 KB | `ffx_api.h` `ffx_api_types.h` `ffx_upscale.h` `ffx_framegeneration.h` `dx12/ffx_api_dx12.h` + `LICENSE.txt` + `SOURCE.txt` | **是**（add-on 的 `in_ffx.cpp` 用；2026-11-07 加） |
| `third_party/optiscaler/` | https://github.com/optiscaler/OptiScaler | `master`，2026-11-06 | 124 KB | `shaders/camera_motion/CM_*`、`inputs/FG/XeFG_Inputs_Dx12.*`、`LICENSE` | **是** |

### 2026-11-07 的瘦身（1.4 GB → 181 MB）

| 删掉的 | 原因 |
|---|---|
| `third_party/FFX/`（1.1 GB 完整 SDK checkout） | 只用到 `sdk/include` 和两个 `.lib`，其余是 samples/framework/bin/docs。头文件已移到 `third_party/ffx/include/`，`src/bridge/build.bat` 的 include 路径已同步。要重建 `.lib` 时按 `temp/ffx_sdk_build.sh` 顶部的说明重新 clone SDK 1.1.4。 |
| `third_party/OptiScaler-master/`（123 MB） | 只作参考：XeFG 的 MV 约定与相机运动生成。需要的三个文件摘到 `third_party/optiscaler/`（含 `README.md` 说明），结论写进 `docs/findings/OPTISCALER_NOTES.md`。 |

> `third_party/fidelityfx/Kits`（旧计划的 3.4 MB FFI 子集）本来就不存在——FFX 的 SDK 里也没有
> `ffxProviderExternal`，那条路当时已确认走不通。

### ReShade v6.8.0 —— 选它的理由
游戏目录里现装的就是 **6.8.0.2155**（`dxgi.dll`，5 592 064 B）。addon 的 API/ABI 必须与宿主运行时对齐，所以钉在 v6.8.0 而不是 main。

关键头文件（`third_party/reshade/include/`）：
- `reshade.hpp`（427 行）—— addon 注册入口 `ReShadeRegisterAddon` / `ReShadeRegisterEvent`
- `reshade_events.hpp`（1966 行）—— 事件表（`init_command_queue` / `present` / `draw` / `bind_pipeline` 等）
- `reshade_api.hpp`（846 行）—— `effect_runtime`、`command_queue` 抽象
- `reshade_api_resource.hpp`、`reshade_api_pipeline.hpp`、`reshade_api_device.hpp` —— 资源/管线/设备抽象（DX11/DX12/Vulkan 统一封装）
- `reshade_overlay.hpp` —— 面板 UI（先例项目用它做了"玄学双显卡"分类）

### XeSS 3.0.2 —— 关键点
`third_party/xess/bin/`（83 MB，**运行时 DLL，部署时要拷到游戏目录**）：
```
libxess.dll          XeSS-SR（超分）
libxess_fg.dll       XeSS-FG（帧生成）★ 副卡要跑的就是它
libxell.dll          XeLL（低延迟，FG 强制依赖）
libxess_dx11.dll     DX11 版超分
basic_sample_frame_generation.exe      ★ 可当参考实现/冒烟测试
basic_sample_shaders/hlsl/shader_xess_fg.hlsl
```
`third_party/xess/lib/`（68 KB，链接用）：`libxess_fg.lib`、`libxell.lib`、`libxess.lib`、`libxess_dx11.lib`

> `bridge.exe` 现在**不再链接 `libxess_fg.lib`**，改为运行时 `LoadLibrary` 加载（`src/xefg_dyn.h`），
> 这样 MFG 版本可以按需替换并用 `--fg-lib` 指定。`libxell.lib` 仍然是静态链接。

`third_party/xess/inc/`：
```
xess/xess_d3d12.h, xess_d3d11.h, xess_vk.h   超分
xell/xell.h, xell_d3d12.h                    低延迟：xellD3D12CreateContext / xellSleep / xellAddMarkerData
xess_fg/xefg_swapchain.h, xefg_swapchain_d3d12.h, xefg_swapchain_debug.h   ★ 帧生成
```

`third_party/xess/doc/`：
- `xess_fg_developer_guide_english.md`（1323 行）★ 已提炼到 `docs/findings/XESS_FG_NOTES.md`
- `xell_developer_guide_english.md`、`xess_sr_developer_guide_english.md`
- `xess_frame_generation_doxygen/`、`xess_super_resolution_doxygen/`（HTML API 参考）

## 2. 还缺（未安装）

| 组件 | 用途 | 安装方式（待执行） |
|---|---|---|
| MSVC v143 + MSBuild（VS 2022 Build Tools，VCTools 工作负载） | 编译 addon / 测试程序 | `winget install Microsoft.VisualStudio.2022.BuildTools --override "--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"`（需管理员，建议 `--installPath` 指到 D:） |
| VC++ 可再发行 14.40.33810+ | XeSS-FG 运行要求（`msvcp140.dll` `vcruntime140.dll` `vcruntime140_1.dll`） | 部署时随包附带或系统安装 |

已有可复用：Windows SDK **10.0.26100.0**、WSL 内 git/cmake/ninja、Python 3.10（`<PYTHON>`）。

## 3. 更新 / 重建命令

```bash
# 钉住的版本（当前工作树就是这些 commit）
git -C third_party/reshade checkout v6.8.0      # 18deaa5
git -C third_party/xess    checkout v3.0.2      # 8fe81bd
git -C third_party/dlss    checkout v310.9.16   # 9981c63
git -C third_party/minhook checkout 8af6b4a

# 换版本（例：ReShade；换完同步改本文件 §1 的版本号）
cd third_party/reshade && git fetch --depth 1 origin tag v6.8.0 && git checkout v6.8.0

# 重新从零拉（sparse，只取需要的目录）
git clone --depth 1 --branch v6.8.0 --filter=blob:none --sparse \
  https://github.com/crosire/reshade third_party/reshade
cd third_party/reshade && git sparse-checkout set include

git clone --depth 1 --branch v3.0.2 --filter=blob:none --sparse \
  https://github.com/intel/xess third_party/xess
cd third_party/xess && git sparse-checkout set inc lib bin doc

# FidelityFX：只需要头文件 + 两个预编译 lib（sdk/include 与 lib/ 已在版本库里）
# 重新生成 lib 见 temp/ffx_sdk_build.sh 顶部注释：
git clone --depth 1 --branch v1.1.4 \
  https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK third_party/FFX
```

> 注意：sparse-checkout 的 **cone 模式只接受目录**，把 `LICENSE.md` 这种文件路径写进去会报
> `fatal: 'LICENSE.md' is not a directory`。根目录文件在 cone 模式下本来就会自动包含。

## 4. 许可

- ReShade：BSD 3-Clause（`third_party/reshade/LICENSE.md`）
- XeSS SDK：Intel 专有许可（`third_party/xess/LICENSE.txt`）——**再分发 `bin/*.dll` 前必须读**，与 GPL 不兼容，本仓库只做本地构建用。
- FidelityFX SDK：MIT（`third_party/ffx/include/FidelityFX/` 各文件头）
- OptiScaler：**GPL-3.0**（`third_party/optiscaler/LICENSE`）。它不只是"参考文件"——
  `src/xefg_dynamic_count.h` 是它的逐字移植并参与编译（`--dyn-mfg`），
  **这就是本仓库整体定为 GPL-3.0 的原因**，见根目录 `NOTICE` 与 `docs/ops/LEGAL.md`。
- Universal MFG Unlocker：MIT（`src/xefg_unlock.h` 的 6 处字节补丁位置出自它）。
- 仓库本身**不把 `third_party/xess/bin`（83 MB DLL）提交进版本库**（`.gitignore` 已排除），用脚本按需拉取。
