# 双卡xessfg插件

## 要求
1. 本体是reshade addon插件
2. 运动矢量通过截取dlss获取
3. 深度信息通过reshade获取
4. 画面+运动矢量+深度信息 通过pcie通道完成卡间传输
5. 主卡渲染, 副卡执行xessfg任务

## 当前硬件环境
1. 主卡 rtx 3080ti 12g
2. 副卡 amd 6750gre 12g
3. 主板 两条直连cpu的pcie4.0x8通道
4. 两张显卡都连接显示器
5. 考虑到链路特性, 只选择6750gre连接的信号源输出画面

## 下一步
1. 先检测当前设备环境是否符合要求
2. 然后一小步一小步来, 从卡间传输数据开始, 分成多个模块实现, 最终合并成插件

# 补充
1. 观察到当前的agent cline在执行命令时会出现问题, 终端执行了命令并显示了结果, 但cline处于等待状态. 不如专门在本地创建一个temp空间, 将输出重定向到temp中的文件里, 通过读取文件来获取终端输出

> **已定位并解决（2026-11-06）**：根因是**命令里带真实换行**——bash 进入 PS2 续行状态回显 `>`，
> 使 VS Code 的 `__vsc_command_complete` 结束标记与 Cline 解析错位。环境本身正常（集成已加载）。
> 硬性规则：**run_commands 每条命令必须单行**；多步逻辑写成 `.sh` 再单行调用
> `./scripts/rcmd.sh <name> -f scripts/xxx.sh`。详见 `docs/findings/TOOLING_NOTES.md`。

---

# 第 1 步完成：设备环境检测（2026-11-05）

完整结论见 **`docs/findings/ENV_REPORT.md`**，原始探测数据在 `temp/`。

## 判定摘要

| 要求 | 判定 |
|---|---|
| 1. ReShade addon | ✅ 宿主 ReShade **6.8.0.2155** 已在游戏目录 |
| 2. 截取 DLSS 拿运动矢量 | ✅ 可行（先例用 NGX hook；游戏需开 DLSS） |
| 3. ReShade 拿深度 | ✅ 可行（先例 `mgpu_depth_tap.fx` 已趟坑） |
| 4. 卡间 PCIe 传输 | ⚠️ 技术可行，**带宽需实测**（见下） |
| 5. 主卡渲染 / 副卡跑 XeSS-FG | ⚠️ 原理可行 —— Intel XeSS 3 明确支持 "non-Intel GPUs with SM 6.4" |

## 已修正的认知（重要）

1. **只能 2x**：XeSS-FG 在非 Intel GPU 上「最多生成 1 帧」。6750 GRE 是 AMD → PLAN 的插帧倍数上限是 2x，不存在 3x/4x。
2. **XeSS-FG 是代理交换链，不是纯计算函数**：`xefgSwapChainD3D12InitFromSwapChainDesc` 会为 bridge 窗口自己创建交换链并接管 present。上屏方案因此简化。
3. **深度尺寸必须等于运动矢量尺寸**（指南硬性要求）→ 用低分辨率 MV（游戏 DLSS 渲染分辨率）+ 同尺寸深度，省一次上采样。
4. **必须带 XeLL**（`libxell.dll`，每帧 `xellSleep` + `xellAddMarkerData`），FG 不能单独跑。
5. 默认 UI 模式是"UI 参与插值"，**第一版可以不传 HUD-less / UI 纹理**，省 2 份 4K 流量。

## 硬件实测

- 主卡 RTX 3080 Ti：FL 12_2，Gen4 **×8**，驱动 32.0.16.1742，接 DISPLAY1（当前主屏 3840×2160@120）
- 副卡 RX 6750 GRE（RDNA2）：FL 12_2 / SM6.4 / DP4a，上报 Gen4 **×16**（与"两条×8"不符，待 GPU-Z 复核），接 DISPLAY8（3840×2160@120）
- i9-11900KF / MSI MEG Z490 ACE / 64GB / Win11 Insider Dev 29695 / WDDM 3.2 / HAGS 已开
- 显示器：两块 LG TV SSCR2（4K120）
- ⚠️ 当前主屏在 NVIDIA 上，PLAN 要求只出 AMD → 需切主屏，或让 bridge 窗口落在 AMD 屏

## 载荷预算（4K，低分辨率 MV）

颜色 33.2 MB + 深度 14.7 MB + MV 14.7 MB ≈ **62.6 MB/帧** → 60fps 需 ≈3.8 GB/s，120fps 需 ≈7.5 GB/s。
跨卡拷贝走 WDDM 中转（非裸 P2P），若副卡实际挂在芯片组后面（DMI≈7GB/s）则 4K120 会卡死。**必须实测**。

## 依赖已就位

- `third_party/reshade/` = ReShade **v6.8.0**（`include/`）
- `third_party/xess/` = Intel **XeSS 3.0.2**（`inc/` `lib/` `bin/` `doc/`，含 `libxess_fg.dll` / `libxell.dll` / FG 示例程序与 HLSL）
- 清单与更新命令：`docs/ops/DEPENDENCIES.md`；FG 约束提炼：`docs/findings/XESS_FG_NOTES.md`

## 唯一硬阻塞

**本机没有 C++ 编译器**（VS2022/2019 是空壳，`vswhere` 无实例；clang-cl 缺 libc++）。编译相关操作暂停，待安装 MSVC 后再进行。

# 下一步：模块化推进

## M0 已完成（2026-11-05）→ `docs/findings/milestones/M0_RESULTS.md`

| 子模块 | 结果 |
|---|---|
| M0.1 能力验证 | ✅ **两块卡** `xellD3D12CreateContext` / `xefgSwapChainD3D12CreateContext` / `SetLatencyReduction` 全部 SUCCESS |
| M0.1 插帧上限 | 实测 `maxSupportedInterpolations = 1` → **当前只能 2x**，3x 需社区解锁 |
| M0.2 传输 | ✅ **file-mapping 跨适配器堆**跑通：buffer 路径 9.19 GB/s（12.24ms / 双向 112.5MB）；纹理可直放共享堆 |
| M0.2 淘汰方案 | ❌ 跨厂商共享 VRAM 纹理（OpenSharedHandle 失败）、❌ CPU 中转（20.9ms，太慢） |

**定下的传输方案**：`CreateFileMappingW` + `ID3D12Device3::OpenExistingHeapFromFileMapping` 在两块卡上各建堆视图 → 同一段系统内存；资源带 `D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER`；跨卡用 `D3D12_FENCE_FLAG_SHARED` 的共享 fence 同步。

**两条硬约束**（由实测推得）：
1. 运动矢量/深度**按渲染分辨率**传输（不是 5K），否则 168MB/帧 → 18.3ms，直接爆预算。
2. 传输必须**流水线化**（双/三缓冲 + 跨卡 fence），单帧 6–9ms 不能串行阻塞渲染。

## M0.3 真实载荷测量（2026-11-05）→ `docs/findings/milestones/M0.3_STEPS.md`（步骤）+ `docs/findings/milestones/M0.3_RESULTS.md`（结果）

测量器 `tests/m0c_payload/`：文件映射共享堆 + 跨卡共享 fence + ring 环 + `--paced` 定速模式。

**用户实机三组测量**（赛博朋克 2077 / 5K / DLSS质量 / 40Hz / GPU 90%）：

| 预设 | 载荷 | 空载 | 游戏内 | 上限 |
|---|---|---|---|---|
| 5K色 + MV/深 3413×1920×32bit | 106.2 MB | 9.54 ms | 20.24 ms | 49 fps |
| 5K色 + MV/深 2560×1440×32bit | 84.4 MB | 7.79 ms | 16.32 ms | 61 fps |
| 仅5K色 | 56.2 MB | 5.17 ms | 11.28 ms | 89 fps |

**真实负载把成本抬高约 2 倍；ring 3 在负载下反而更差 → ring 定为 2。**

定速（40fps）优化对比（游戏运行中）：

| 配置 | 载荷 | 定速交接 | 预算占用 |
|---|---|---|---|
| 原始 5K色+MV/深 3413×1920×32b | 106.2 MB | 25.41 ms | 101.6% ❌ |
| 5K色+MV/深 3413×1920×16b | 93.7 MB | 24.19 ms | 96.8% ⚠️ |
| **5K色+MV/深 2560×1440×16b** | **77.3 MB** | **19.31 ms** | **77.2% ✅** |

**M1 传输规格（据此定稿，2026-11-05 按"实际用 5K 性能档"修正）**：
颜色 5120×2880 R10G10B10A2(56.25MB) + MV 2560×1440 R16G16F(14.06MB) + 深度 2560×1440 R16_UNORM(7.03MB) = **77.3 MB/帧**，ring 2。

> **关键简化**：实际游玩一律用 **DLSS 性能档**（3080Ti 跑不动质量档，黑神话/影之刃0 等），
> 性能档渲染分辨率正好 **2560×1440**，所以 **MV/深度原生就是目标尺寸，不需要降采样 pass**。
> M1 因此不依赖 M2/M3，可用假数据先把全链路跑通。
> 深度 16 位转换由 M3 的深度 tap 顺带完成（它本来就要渲染到自建目标）。
> 颜色必须留 5K（DLSS 已升上去，降 4K 等于丢画质）。

各档位对照：质量 93.7MB(96.8% 预算，太紧) / 平衡 84.6MB(~83%) / **性能 77.3MB(77.2% ✅)** / 超高性能 65.6MB(~66%)。

**待补**：步骤D（游戏+LSFG x3 同跑）因 LS 失焦停止而未拿到有效数据 → `run_lsfg.bat [延迟秒]`，可选，优先级低于 M1。

## 待做模块

## M2 已完成（2026-11-06）→ `docs/findings/milestones/M2_RESULTS.md`

**实测参数（赛博朋克 2077 / 5K / DLSS 性能 / HDR）**

| 项目 | 实测 |
|---|---|
| 运动矢量 | 2560×1440 **R16G16_FLOAT** ← 与 M1 占位规格完全一致 |
| 深度 | 2560×1440 **R32_TYPELESS**（D32_FLOAT） |
| DLSS 输入色 | 2560×1440 R16G16B16A16_FLOAT |
| DLSS 输出 | 5120×2880 R16G16B16A16_FLOAT = **118 MB/帧（绝不能传）** |
| jitter | 每帧可读 |
| 相机矩阵 | **DLSS-SR 不提供**（`DLSSG.*` 键读不到，全为 0） |
| 调用者 | `170_E658700.dll`（游戏本体，直连 NGX 不经 Streamline） |

**真凶**：CP2077 把 `nvngx_dlss.dll` 的多个副本加载→查版本→卸载，
真正执行 DLSS 的是驱动目录里的 **`_nvngx.dll`**。前四轮都挂在"探针副本"上，所以零调用。
最终修法：hook `LoadLibraryExW/W/A`，按**完整路径**识别，只挂 `_nvngx.dll`。

**载荷规格正式锁定 = M0.3 的 77.3 MB 配置**：颜色 5K R10G10B10A2(56.25) + MV 2560×1440 RG16F(14.06) + 深度 2560×1440 R16_UNORM(7.03)。
颜色必须取 ReShade 的后台缓冲，**不能拿 DLSS 输出**（那是 FP16 的 118 MB）。

**遗留风险**：XeSS-FG 的 `TagFrameConstants` 要求 view/projection 矩阵，而 DLSS-SR 不给。
M4 优先试"jitter + 近似投影"，不行再借 `sl.dlss_g` 的参数。

## M3.1 已完成（2026-11-06）→ `docs/findings/milestones/M3_RESULTS.md`

ReShade 深度采集**技术上通了**，但它给的深度不是 DLSS 用的那一份：

| 来源 | 尺寸 | 格式 |
|---|---|---|
| ReShade `DEPTH` 语义 | 5120×2880 | `R32G8X24_TYPELESS`（带模板） |
| NGX（DLSS 真正用的） | **2560×1440** | `R32_TYPELESS`（纯 D32） |

XeSS-FG 要求深度与 MV 同尺寸（2560×1440），所以**深度必须和 MV 一样从 NGX 钩子取**；
ReShade 只负责颜色（5K `R10G10B10A2`，已实测确认）+ present 时机。原计划的 tap shader 因此不再需要。

**M3.2 方案**（API 已验证存在）：hook `addon_event::barrier` 跟踪资源状态 →
在 `addon_event::reshade_begin_effects` 拿到的命令列表上 `barrier → CopyResource → barrier 还原`，
把 MV/深度/颜色搬进邮箱后 `publish()`。深度流先用 `R32_FLOAT`（84.4 MB = M0.3 配置 B）打通链路，
R16 下采样留到 M3.3（省到 77.3 MB）。

## 待做模块

| 模块 | 内容 | 产出 | 状态 |
|---|---|---|---|
| **M1 传输层** | 文件映射共享堆 + 双卡放置纹理 + 每槽独立共享 fence + 环形流水线 | `src/cross_adapter_mailbox.h` + `tests/m1_transport/` | ✅ v0 通过（`docs/findings/milestones/M1_RESULTS.md`） |
| **M2 MV 截取** | hook NGX `EvaluateFeature`，拿真实 MV/深度/jitter | `src/m2_probe/` | ✅ 通过（`docs/findings/milestones/M2_RESULTS.md`） |
| **M3 采集** | M3.1 ReShade 深度探测 ✅；M3.2 MV+深度采集搬进本进程纹理 ✅ | `src/game_capture/` | ✅ **通过**（`docs/findings/milestones/M3.2_RESULTS.md`） |
| **M4 副卡 FG 上屏** | M4.1 跨进程传输 ✅；M4.2a WGC+HDR 颜色 ✅；M4.2b bridge.exe + XeSS-FG + 窗口 | `src/mgpu_shared.h`、`tests/bridge_probe/`、`tests/wgc_probe/`、`src/fg_context.*` | 🟡 颜色链路已定稿（`docs/findings/milestones/M4_WGC_RESULTS.md`），下一步写 bridge.exe |
| **M5 合成 addon** | 接 ReShade 事件 + 面板 + `mgpu.ini` | `mgpu_bridge.addon64` | |

**M1 已验证**（`temp/m1_run.txt`）：12 帧跨卡内容逐帧精确匹配（证明槽位轮转与 fence 正确）、120 帧定速 40fps 零失败、CPU 提交 0.32ms/帧。测试是在赛博朋克运行中做的，属真实负载。

**M1 遗留**：publish/consume 接进 ReShade present 回调后的队列时序、resize 重建、consumer 落后时的丢帧策略。

## 工具链（已可用）

```bash
./scripts/build_m0.sh m0_probe        # 能力探针
./scripts/build_m0.sh m0b_transport   # 传输实验
```
Windows 侧 cl.exe 从 WSL 调用；产物落在 `build/`，中间文件在 `<STAGE_DIR>\<module>`。

