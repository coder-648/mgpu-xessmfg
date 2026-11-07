# 环境检测报告（PLAN 下一步 1）

> ## ⚠️ 依据 PLAN2 的更正（2026-11-05，请先读这段）
>
> 1. **主屏在 AMD 上**，不是 NVIDIA。`\\.\DISPLAY1` 边界 2926×1646 是 **5K 屏 + 175% DPI 缩放**的结果（5120/1.75≈2926），不是 4K。NVIDIA 的 4K 是 `\\.\DISPLAY8`。
> 2. **双卡实际跑 PCIe 4.0 ×8**。PnP 上报的 AMD "×16" 不可信；M0.2 实测 9.19 GB/s 吞吐与 Gen4 ×8 吻合。
> 3. **MSVC 实际存在**：`C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools`，MSVC 14.44.35207。本文档 §5 里"无编译器"是探测路径遗漏（只查了 `Program Files\...\2022`，没查 `Program Files (x86)\...\2022\BuildTools`）。
> 4. 先例项目 `玄学双显卡1.7版` **不再作为参考**（用户明确其存在错误）。§7/§8 中相关引用仅作背景。
> 5. 插帧目标：**当前 5K@40 → 80 fps（2x）**；3x（→120）等社区解锁版 `xessmfg`。
>
> M0 实测结果见 `docs/findings/milestones/M0_RESULTS.md`。

探测时间：2026-11-05　探测方式：WSL2 内调用 `powershell.exe` / `nvidia-smi`，输出重定向到 `temp/`（遵循 PLAN 补充 1）。
原始数据：`temp/env_os.txt`、`temp/env_gpu.txt`、`temp/detect_out.txt`、`temp/detect2_out.txt`、`temp/detect3_out.txt`、`temp/detect4_out.txt`、`temp/env_toolchain.txt`、`temp/net_check.txt`、`temp/prior_docs.txt`

---

## 1. 主机与系统

| 项目 | 实测值 | 判定 |
|---|---|---|
| 宿主系统 | Windows 11 专业工作站版 **Insider Preview Dev 29695.1001** | ⚠️ 预览版，驱动/图形栈行为可能与正式版不同 |
| 内核/WDDM | WDDM **3.2**，DDI 12 | ✅ 满足 DX12 跨适配器要求 |
| 开发环境 | WSL2 + Ubuntu 24.04（本仓库所在） | ✅ 可作为代码仓与构建编排 |
| CPU | Intel i9-11900KF（Rocket Lake，16+4 条 PCIe 4.0） | ✅ |
| 主板 | MSI MEG Z490 ACE (MS-7C71) | ⚠️ 双卡拆分方式需复核（见 §3） |
| 内存 | 63.9 GB | ✅ |
| HAGS（硬件加速 GPU 计划） | `HwSchMode = 2`（已开启） | ✅ 对跨卡与 FG 有利 |

## 2. 显卡

| | 主卡（渲染） | 副卡（XeSS-FG） |
|---|---|---|
| 型号 | NVIDIA GeForce RTX 3080 Ti | AMD Radeon RX 6750 GRE 12GB |
| 架构 | GA102 / Ampere | Navi 22 / RDNA2 |
| PCI ID | `VEN_10DE&DEV_2208`，总线 4:00.0 | `VEN_1002&DEV_73DF`，总线 3:00.0 |
| 显存 | 12288 MiB | 12 GB |
| 驱动 | 32.0.16.1742（2026-09-30，Win 侧 nvidia-smi 无输出，见 §6） | 32.0.21045.16010（2026-10-16） |
| Feature Level | **12_2, 12_1, 12_0 …** | **12_2, 12_1, 12_0 …** |
| Shader Model | SM 6.7 | SM 6.4+（RDNA2 支持 DP4a） |
| 当前链路 | Gen4 ×8（Max ×16） | Gen4 ×16（Max ×16）⚠️ 与"两条 x8"不符，待复核 |

**XeSS-FG 可行性（关键）**：Intel 官方 XeSS SDK（仓库 `intel/xess`，现已是 **XeSS 3 SDK**）README 原文——
> "XeSS Frame Generation (XeSS-FG) — ... available on discrete and integrated Intel® Arc™ GPUs, **as well as non-Intel GPUs with SM 6.4 support**."

RX 6750 GRE 为 FL 12_2 / SM 6.4 / DP4a 平台 → **硬件侧满足 XeSS-FG 运行条件**，PLAN 的核心思路成立。

> **已修正（2026-11-05 二次校验）**：本机先例包里的 `libxess_fg.dll`（FileVersion 1.3.1.78）与 **Intel XeSS SDK v3.0.2** 官方 `bin/libxess_fg.dll` **MD5 完全相同**（`e5da7e91e3af8a8340b3fef922912e6e`），`libxell.dll` 亦然。
> 即"1.3.x"只是 FG 内部组件版本号，**不是旧运行时**。且先例项目已在非 Intel 的 RTX 5050 上跑通过。
> 详见 `docs/findings/XESS_FG_NOTES.md` §1.5。仍需在 6750 GRE 上实测 `xefgSwapChainD3D12CreateContext`。

⚠️ **重要目标修正**：指南明确 "On non-Intel GPUs the maximum number of generated frames is one" → 副卡是 AMD，**只能 2x**，不存在 3x/4x。

## 3. PCIe 链路待复核

PnP 上报：NVIDIA `CurrentLinkWidth=8`（Max 16），AMD `CurrentLinkWidth=16`（Max 16），两者 `CurrentLinkSpeed=4`。
PLAN 声明"两条直连 CPU 的 PCIe 4.0 ×8"，但 AMD 上报 ×16 → 存在三种可能：
1. PnP 的 `CurrentLinkWidth` 在 AMD 驱动上不准确（常见）；
2. 副卡实际插在芯片组槽（Z490 芯片组槽通常 ×4 Gen3，经 DMI ×8 Gen3 上行）；
3. 主板做了 ×8/×8 拆分但上报错误。

**影响**：跨卡拷贝走的是 WDDM 系统内存中转（不是裸 P2P），若副卡在芯片组后面，带宽会被 DMI（≈7 GB/s 双向共享）限制。
4K 单帧载荷估算（RGBA8 色彩 33.2 MB + R32F 深度 33.2 MB + MV RG16F 16.6 MB ≈ 83 MB/帧）→ 需要按 5–8 GB/s 预算，60 fps 需 ~5 GB/s，120 fps 需 ~10 GB/s → **如果被 DMI 卡住，4K120 不可行，4K60 勉强**。
复核手段：GPU-Z / HWiNFO64 看 "PCIe Link Width"，并确认 BIOS 中 Above 4G Decoding、Resizable BAR 状态。

## 4. 显示输出

| 屏幕 | 绑定显卡 | 分辨率 |
|---|---|---|
| `\\.\DISPLAY1`（**当前主屏**） | NVIDIA 3080 Ti | 3840×2160@120（逻辑 2926×1646，DPI 缩放） |
| `\\.\DISPLAY8` | AMD 6750 GRE | 3840×2160@120（X 偏移 5120） |

显示器：两块 **LG TV SSCR2**（4K120，HDMI 2.1）。
注意：AMD 驱动 `Current Mode` 报 5120×2880 → 疑似开了 VSR，属驱动侧虚拟分辨率，桌面实际仍是 4K。
PLAN 要求"只用 6750 GRE 的信号源输出" → 需把主屏切到 DISPLAY8，或让 bridge 窗口落在 AMD 屏（先例项目即用后者：独立 bridge 窗口 + 输入转发）。

## 5. 软件与工具链

| 组件 | 状态 |
|---|---|
| ReShade 运行时 | ✅ 游戏目录内已有 **6.8.0.2155**（`dxgi.dll`，5 592 064 B）；⚠️ 无独立安装目录、`reshade-shaders` 仅散落在先例包 |
| MSVC（cl.exe / MSBuild C++） | ❌ **缺失**（`VS2022`/`VS2019` 是空壳，只有 Installer；`vswhere` 无实例） |
| Windows SDK | ✅ 10.0.26100.0（另有 14393/15063/16299/17134 旧版） |
| LLVM clang-cl | ⚠️ 有（`C:\Program Files\LLVM\bin`），但**无 libc++ 头**，配 SDK 也编不了 STL |
| MinGW g++ | ⚠️ 有（`<MINGW64_DIR>`），ABI 与 ReShade 不兼容，不建议 |
| CMake / Ninja / git | ✅ 仅在 WSL 内（3.28.3 / 有 / 2.43.0）；Windows 侧无 |
| Python | ✅ `<PYTHON>` |
| winget | ✅ 可用（可用于静默安装 Build Tools） |
| 网络 | ✅ GitHub 可达（WSL 实测 HTTP 200，成功下载 2.4 MB 源码包）；也可拉 Intel/ReShade 仓库 |
| 磁盘 | C: 剩 35.5 GB（紧）；D: 190 GB、J: 929 GB（充裕） |

## 6. 已知异常

1. **Windows 侧 `nvidia-smi` 无任何输出**（exit=0 但 stdout 空）——仅 WSL 侧 `/usr/lib/wsl/lib/nvidia-smi` 正常。后续 NVIDIA 侧指标（PCIe、FG 占用）需从 WSL 或改用 NVAPI/PnP 获取。
2. `dxdiag /t` 首次在 `-WindowStyle Hidden` 下产出空文件，改为直接调用并 `Start-Sleep` 才成功 → 记录在 `temp/detect4.ps1`。
3. PowerShell 5.1 以 ANSI 读取无 BOM 的 `.ps1`，脚本内**中文路径字面量会导致解析失败** → 探测脚本一律用 ASCII 通配符定位中文目录。

## 7. 重要发现：本机已有同族先例产物

`<DOWNLOADS>\玄学双显卡1.7版\`（409 MB）是"跨卡 ReShade addon"的完整部署包：

- `mgpu_bridge.addon64`（1 022 464 B）——ReShade addon 主程序
- `mgpu.ini`（60+ 配置项：MVec/Depth/FrameGen/FrameGenMultiplier/XeLLFpsCap/UseOpticalFlow/MidFrameQueue/RenderFlush…）
- `reshade-shaders/Shaders/mgpu_depth_tap.fx`——**深度采集 shader（带极详细设计注释）**
- 运行时：`libxess_fg.dll 1.3.1.78`、`libxess.dll 2.0.2.68`、`libxell.dll 1.3.2.10`、`nvngx_dlss.dll 310.x`、`nvngx_dlssnr.dll 165 MB`、Streamline 2.11.1
- `新对话上下文.txt`：记录了源码结构（`src/gpu1_context.cpp` 28000+ 行等）、编译命令与踩坑
- 输入转发脚本（AHK）：bridge 独立窗口 + F10/F11/F12 焦点管理

⚠️ 但其中引用的源码 `<OLD_TOOLS_DIR>\\` 与编译器 `<BUILDTOOLS_DIR>\\` **在本机已不存在**（E: 只剩游戏），即：**有二进制、无源码、无编译器**。

## 8. 结论

| PLAN 要求 | 判定 |
|---|---|
| 1. 本体是 ReShade addon | ✅ 可行，ReShade 6.8 现成，addon API 稳定 |
| 2. 运动矢量通过截取 DLSS 获取 | ✅ 可行（先例用 NGX hook）；游戏需启用 DLSS 才有 MV |
| 3. 深度通过 ReShade 获取 | ✅ 可行（先例 `mgpu_depth_tap.fx` 已验证该路径与坑） |
| 4. 画面+MV+深度 经 PCIe 卡间传输 | ⚠️ 可行但需实测带宽：跨卡走 WDDM 中转，副卡链路位置决定上限 |
| 5. 主卡渲染 / 副卡跑 XeSS-FG | ⚠️ 原理可行（XeSS 3 支持非 Intel GPU，SM 6.4）；官方运行时 DLL 已进仓库，MD5 与先例包一致；**须先实测 libxess_fg 在 6750 GRE 上能否 `CreateContext`**，且**上限只有 2x** |

**硬阻塞（必须先解决）**：本机没有 C++ 编译器。任何验证（跨卡拷贝 demo、XeSS-FG 初始化测试）都要先有 MSVC。

## 9. 下一步（建议顺序）

1. 安装 VS 2022 Build Tools（VCTools 工作负载）→ 具备 `cl.exe` / `MSBuild.exe`。
2. 拉取 ReShade SDK（对齐 6.8.0）+ Intel XeSS 3 SDK（含新版 `libxess_fg`）。
3. **模块 0：能力验证程序**（最小 DX12 单文件，不接 ReShade）
   - 在 AMD 设备上枚举 adapter、创建 device、加载 `libxess_fg.dll` 并初始化 context → 证明副卡能跑 FG；
   - 在 NVIDIA+AMD 双设备上创建 shared texture 并互拷 → 证明卡间传输可用并**实测带宽**。
4. 模块 1：卡间传输层（ring buffer / fence 同步 / 双队列）
5. 模块 2：DLSS 运动矢量截取（`nvngx_dlss.dll` hook）
6. 模块 3：ReShade 深度采集（复用 depth tap 思路）
7. 模块 4：副卡 XeSS-FG 管线 + 独立 bridge 窗口上屏
8. 模块 5：合成为 ReShade addon，`mgpu.ini` 配置化
