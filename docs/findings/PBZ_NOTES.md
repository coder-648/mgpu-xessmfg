# 影之刃零（PBZ）专项记录

> 日期：2026-11-10　状态：**记录中**（实测环境，不是计划）
> 相关：`docs/findings/INPUT_SEMANTICS.md`（语义归一化/探针）、`docs/findings/FG_SNIFF.md`（只读抓 FG）

## 1. 环境

| 项 | 值 |
|---|---|
| 渲染进程 | `F:\steam\steamapps\common\PBZ\platform\steam\PBZ.exe`（UE 5.7.4） |
| 引擎 | UE **5.7.4**，NVIDIA Streamline 插件在 `Engine\Plugins\Runtime\Nvidia\StreamlineCore\` |
| ReShade | 6.8.0.2155，`dxgi.dll` 代理（**它自己也钩了 Streamline**：`dxgi.dll` 里有 `HookStreamline`） |
| 解锁器 | `dlssg_sm86`，代理 `version.dll`（`Engine=XeSS`、`MaxGeneratedFrames=3`） |
| 显卡 | RTX 3080 Ti / 驱动 617.42 / 4K HDR10 |
| 本仓库 | add-on `game_capture.addon64` + `bridge.exe`（协议 v11） |

## 2. **FSR 会让游戏崩溃（游戏自带 DLL 的 double-free）** —— 最重要的发现

`%LOCALAPPDATA%\CrashDumps\PBZ.exe.25748.dmp`（2026-11-10 02:50，切档位时）用 `cdb` 分析：

```
FAILURE_BUCKET_ID: HEAP_CORRUPTION_ACTIONABLE_BlockNotBusy_DOUBLE_FREE_c0000374_dxgi.dll!Unknown
EXCEPTION_CODE_STR: c0000374

ntdll!RtlFreeHeapFast
dxgi+0x197ccc / +0x2be20 / +0x16a49
D3D12Core!CDestructionNotifier::Destroy
D3D12Core!CDescriptorHeap::LUCPrepareForDestruction
D3D12!CUseCountedObject<...>::Release
D3D12Core!CLayeredObject<CBackingCommandAllocator>::Release      ← command allocator
dxgi+0x8553
amd_fidelityfx_upscaler_dx12+0x279a
amd_fidelityfx_upscaler_dx12+0xd3fd
amd_fidelityfx_upscaler_dx12+0x7910
amd_fidelityfx_upscaler_dx12+0x55
amd_fidelityfx_loader_dx12+0x2d
PBZ+0x173bfad
```

**结论**：游戏自带的 FSR 运行时
（`ProjectPBZ\Plugins\FSR\Source\fidelityfx-sdk\Kits\FidelityFX\signedbin\amd_fidelityfx_upscaler_dx12.dll`）
在自己的**销毁路径**里把一个 D3D12 command allocator **释放了两次**。
栈里**没有** `game_capture` / `bridge` / `ReShade` —— 与我们无关。

**触发时机**（三次崩溃全部吻合）：

| 时间 | 场景 | 当时 upscaler |
|---|---|---|
| 02:50 | 切档位（swapchain 重建） | **FSR** |
| 02:59 | 加载界面 | FSR（日志最后一行 `input source: ffx pinned`） |
| 03:03 | 加载界面 | FSR |

**应对**：

1. **PBZ 里不要用 FSR**（DLSS / XeSS 都正常，一旦换成 DLSS 后能顺利进游戏并游玩）；
2. 测试配方固定用 DLSS；
3. 若以后要抓这类**静默退出**，先开 LocalDumps（需管理员）：
   `reg add "HKLM\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\PBZ.exe" /v DumpType /t REG_DWORD /d 2 /f`

## 3. 输入语义（实测）

| 项 | 值 |
|---|---|
| 渲染/输出 | 依赖档位：**2562×1441**（66.7%）、**2880×1620**（DLSS Quality）、输出 3840×2160 |
| MV 纹理（NGX） | `2562×1441 R16G16_FLOAT`；**FSR 时是 2564×1444**；**XeSS 时是 3840×2160**（HIGH_RES_MV） |
| 深度纹理 | 比渲染尺寸大 2×3 像素（`2564×1444`），SL 的 tag extent 只声明 `2562×1441` ⇒ 必须按 extent 裁 |
| `MV.Scale` 声明 | **会随会话/档位变**：有时 `1.0`（⇒ 单位不可知）、有时 `≈2880`（⇒ `uv`，实测同意） |
| **MV 单位（实测）** | **pixels**（多尺度胜率搜索的第一名，与 `probe: mv` 的测量独立吻合） |
| **MV 符号（实测）** | **`prev − cur`**（`+mv` 对齐更好）⇒ **不需要 `--mv-flip`** |
| **MV 质量（实测）** | 旧指标 `quality=0.20`（`ratio=0.800`）；同一份数据按新的配对胜率重算 ≈ 68.6% / 0.37 —— **待 03:19 版重新测一次确认** |
| 深度方向 | DLSS/FSR 声明 reversed-Z；**XeSS 段根本没有深度纹理**（所以没有深度的声明可言） |
| `cameraFOV` | 75.01° 是**水平** FOV；矩阵反推的垂直 FOV = **46.7°** |
| `cameraFar` | 75000（有限），而矩阵是无限远 reversed-Z |
| jitter | 已**烤进** `cameraViewToClip`（`projShear` 与 `jit` 数值级对应，y 符号相反） |
| `motionVectorsInvalidValue` | `FLT_MAX` ⇒ fp16 饱和成 65504（以前被误判成"垃圾数据"） |

**符号是怎么量出来的（第一份完整数据，2026-11-10 03:14）**：

```
[03:14:35] probe: sign scale=pixels used=22206 static=3150
                     plus=431.7979 minus=539.9221 ratio=0.800
                     -> prev-cur (+mv aligns)  quality=0.20
```

`ratio=0.800` 用当时的阈值（0.85）会判成 `ambiguous` —— 这就是 A-v3 把"残差求和"换成
"逐点配对胜率"的直接动机：**20% 的方向性差异被均值抹平成了 1.9%**，而投票制不受这个影响。
（同一份数据按胜率重算约 `wins≈68.6%, quality≈0.37`，方向结论不变；这是**推算**，
以 03:19 版重测的日志为准。）

**实践结论**：对 PBZ，bridge 的默认（不翻转、`motionVectorScale` 透传、深度方向来自 flag）
**已经是对的**；`--mv-flip` / `--fg-mv-scale` 在这个标题上确认不需要。自动化（B-v2）的价值在于
别的标题（例如明末需要 `--mv-flip`），而不是这里。


## 5. 已知的 add-on 侧问题（记录，避免重复踩）

1. **模块表饱和**：`in_fg.cpp` 的模块登记表原来只有 4 个槽，PBZ 里有 4+ 个匹配模块 ⇒
   饱和后每 120 帧**重复 `MH_CreateHook`**（ReShade 也钩 Streamline）⇒ 已修为 8 槽 + "已忽略"表。
2. **`slGetFeatureFunction` 的第一个参数是 `sl::Feature` 不是 ViewportHandle** ⇒ 早期版本按后者解引用直接崩（已修，见 `FG_SNIFF.md` §10）。
3. 共享段固定 253.6 MB（v9 的容量设计）。在没有 bridge 时也照建；内存紧张时可以缩。
4. **构建脚本默认编译错模块**：`./scripts/build_addon.sh` 不带参数时默认 `src/m2_probe`，
   要打游戏用的 add-on 必须写 `./scripts/build_addon.sh src/game_capture`
   （第一次踩到是 03:19，日志第 1 行就是 `[build] source dir : …/src/m2_probe`）。
5. **进游戏到出结果之间要有足够长的"真画面"时间**。03:21 那次会话只有 ~105 秒，
   探针落在加载/菜单上：`probe: mv mean|v|=(82.4,97.3) large=24.8%`（正常是 `(3.4,0.11)`），
   深度几乎全 0（`mean=0.0000`），符号测试只拿到 446 票且两侧残差差 0.3% —— 全是垃圾输入。
   探针每 120 帧（约 2 s）一轮，**进世界后至少 20 s 真实运动**才有稳定样本。
6. **ReShade 会重载 add-on（PBZ 启动时重建 swapchain ⇒ 一次启动装载 5 次）**。
   实测证据：`[04:26:05…04:26:10] fg sniffer: ON` 打了 5 次。后果：
   - **跨实例状态不能用 `static`**。相机矩阵的交接最初就是这么坏的：旧实例的 XeFG 钩子往
     **旧实例的**全局里写，新实例的 present 路径读**新实例的**全局 ⇒ 永远 0
     （`raw_tick=0 seq=0` 而写者确实执行了）。改成命名共享内存 `Local\mgpu_fg_camera_v1` 后立刻通。
   - 钩子是旧实例装的，`MH_CreateHook` 的"已装过"守卫每实例一份，重载后新实例会再钩一次
     （MinHook 返回 `ALREADY_CREATED`，我们按"已装"处理）。
   - 排查时看到同一行日志出现多次，先想重载，别怀疑逻辑。


## 5. 参考：三次符号测试的对比（记录判据演化）

| 时间 | 判据 | 结果 | 为什么不能用 |
|---|---|---|---|
| 02:55 / 02:57 | 像素偏移（最近邻） | `plus == minus` | UV 源被当成像素 ⇒ 偏移被抹成 0 |
| 03:08 / 03:11 | 双线性 | `ratio=0.983/0.984` | 尺度仍错，位移过小 |
| 03:14 | 三尺度 + 残差求和 | `scale=pixels ratio=0.800` → `prev-cur` | 真差 20% 被均值抹成 1.9%，差点判成 ambiguous |
| 03:16 | 三尺度 + 求和（换个场景） | `scale=ndc used=437` → `cur-prev` | 跟着小样本跑（437 票） |
| 03:21 | 三尺度 + 胜率（无样本量门槛） | `scale=uv used=446` → `cur-prev` | 446 票的平局压过 22k 票，且两侧残差只差 0.3% |
| 03:26 | 加 z 分数 + 残差门 | `ndc: 65.7% 但 gap=0 → unknown`（门生效）；`uv: 77.3% gap=3.7% → prev-cur` | `big=46,222/63,504`：73% 的 texel 超 16 纹素上限，正确尺度被饿死 |
| 03:29（待跑） | 256 patch + 64 上限 + 深度梯度门 | — | — |

结论：**判据的每一步都必须同时约束"票数"和"残差实际差多少"**，否则小样本或数值噪声都能左右结论。
03:26 还额外证明了这条链是能"自己刹车"的 —— 残差门当场把 65.7%/z=40.3 的那一行判成 unknown。

