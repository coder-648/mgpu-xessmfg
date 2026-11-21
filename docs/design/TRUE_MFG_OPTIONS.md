# 真多帧（true MFG）可行性：sm86 路线与我们能走的路（2026-11-21）

> 先给结论：
> 1. **当前 3x/4x 不是真多帧**，是解锁补丁让库的 N 帧循环复用 2x 模型
>    （`docs/findings/MFG.md` §7）。
> 2. sm86 的真多帧 = **自带 model 15 程序/权重 + 自己的 D3D12+CUDA 运行器**；
>    它的 C++ 是 MIT，但内核与权重是 Intel / MFG-Unlocker 条款下的二进制，**不是**
>    我们直接链接就能用的库。
> 3. **MFG-Unlocker V1.7.1 源码已经把这条路走通了**：模型不需要外部包，model 15 就在
>    `libxess_fg` 里，只是 ID 不是 16。它的做法是 `ModelPort` 把 model 15 重标记成 16、
>    `ShaderPort` 换掉 54 个 DXIL 程序、解锁时**不打** keep_2x 点，然后由 XeFG 自己跑
>    真多帧。我们手上的 `libxess_fg` / `libxell` 的 SHA-256 与它支持的 XeSS SDK 3.0.2
>    完全一致（见 §7）。剩下的是移植这三层 shim + 取得 54 个程序资产，不是写引擎。
> 4. 如果端口被拒（模型布局不符、程序资产缺失、驱动拒绝）：才需要退到“副卡换 Intel
>    （原生 MFG）”或“副卡 NVIDIA + 移植 sm86 引擎”。sm86 的 trunk 是 CUDA/IMMA，
>    AMD 跑不了；纯 DXIL 运行器他们做过又删了，所以不要在 AMD 上重写引擎。

---

## 1. 判定标准：什么才算 true MFG

满足以下三条才算：

1. **一次光流、N 次合成**：每个真实帧只算一次双向/单向光流，然后按 `t = i/(N+1)`
   对每张生成帧做一次合成；而不是对每张生成帧各跑一次完整的 2x 模型。
2. **算力次线性**：固定场景/分辨率，量 2x / 3x / 4x 的 `gpu`。真多帧把光流摊到 N 张，
   每多一张生成帧的增量应小于第一张；2x 模型复用则近似线性。
3. **相位正确的画质**：靠近上一真实帧的生成帧（t=1/4、1/3）重影应明显少于
   “2x 模型在非 0.5 相位”的输出（sm86 用跳帧真值做过这组对比）。

> 一个便宜的判据：4x 的中间帧（t=1/2）如果与 2x 模式的生成帧**逐位相同**，
> 基本可以判定是同一个 2x 模型；model 15 的 t=1/2 输出通常不会逐位相同。

## 2. 我们和 sm86 的差距到底在哪

| 组成 | sm86 | 我们 |
|---|---|---|
| 模型资产 | `xefg_kernels.dll`：model 18 的 61 个 DXBC + 96 个权重；model 15 的 54 个程序 + 权重；CUDA 镜像 | 只有 Intel `libxess_fg.dll`；它内部是否带 MFG 模型取决于 build（1.3.1.78 的解锁表里“18→16”不可用） |
| 推理执行 | 自己的 D3D12 + CUDA 运行器；少数派发走 DXIL；图序列是私有的 `.inc` | 调库，不碰模型 |
| 历史/重置 | 自管两组槽 + 明确的 reset 条件 | 交给库，只能看 `framesPresented` |
| 输入位置 | 游戏进程内（渲染卡）、NGX Evaluate 的参数 | 副卡进程，共享内存里的 MV/depth + WGC 颜色 |
| 许可 | C++ MIT；内核/权重是 Intel/MFG-Unlocker 二进制条款，**不在 MIT 内** | GPL-3.0 项目；不能直接吞二进制条款的资源 |

**关键点**：sm86 的“引擎”不是一个可以 `#include` 的库，而是**运行时 + 私有图 +
资源包**三件套。我们这份第三方副本只有资源包和后端 DLL，没有它的源码/图。

## 3. 四条路（按代价排序）

### 路径 A：复用 MFG-Unlocker 的 ModelPort/ShaderPort（推荐，源码已就位）

`third_party/MFG-Unlocker-main/` 是 V1.7.1 源码。它不是自己的推理引擎，而是对
`libxess_fg` 的**进程内移植层**：

1. `source/native/xess/port/model_port.h`：在模型目录（RVA `0x15be920`）里找到
   `i0`（id 15）、`i65`（id 18）、`i62`（id 17），断言没有 id 16，然后把
   **model 15 的 id 改成 16**，并修正 reconstruction / scheduling / graph 元数据。
   provider 的 MFG 路径因此会选中真正的多帧模型。
2. `source/native/xess/port/shader_port.h`：钩 provider 的程序解码点（RVA `0x1ad217`），
   用 54 个 portable DXIL 程序替换对应程序；AMD 上保留 Intel 的两个 U-Net 程序
   （实测更快且输出相同）。
3. `source/native/xess/host_unlock.cpp`：打 1/2/3/5/6 五个抬上限站点，**不打**
   `fg_keep_2x_model`；`kGeneratedCapacity = 5`（6x 上限）。
4. 这三层装好后，仍然是 XeFG 自己生成；我们只调 `SetNumInterpolatedFrames(N)`。

关键匹配（已核对）：

| 文件 | SHA-256 | MFG-Unlocker 要求 |
|---|---|---|
| `third_party/xess/bin/libxess_fg.dll` | `ec5e0c65…75ab27` | `kSdk302.fgSha256`，完全一致 |
| `third_party/xess/bin/libxell.dll` | `d2030dcd…3ded8c7` | `kSdk302.llSha256`，完全一致 |
| 版本 | 1.3.1.78 / 1.3.2.10 | 支持列表内 |

**缺的唯一东西是 54 个 DXIL 程序资产**：源码树里没有（`BUILD.md` 明确说
`programs/programs.tsv` 是单独准备的 build input）。但有两个可能的来源：

- `third_party/dlssg_for_sm86-main/dlssg_sm86/xefg/xefg_kernels.dll` 里有一个
  `"xess-fg 1.3.1.78 model 15, MFG-Unlocker 1.6.0 port"` 的清单，pinned manifest
  SHA 正是 `d74bd783…`（与 MFG-Unlocker 的 `MANIFEST_SHA256` 相同），共 230 个条目；
- MFG-Unlocker 的发布 DLL 把这 54 个程序作为 RCDATA `7000..7054` 内嵌，
  asset 表里有 `programs/<key>.dxil` 的名字与哈希，提取后就是 `programs/` 目录。

第一个来源的条目名是 `cs_...`，而 ShaderPort 用数字 key（例如
`3518243144275597942266`），所以需要一次 key↔bytes 的映射；第二个来源直接带 key。

最小验证（不需要先接进 lock_frame）：

1. 写一个 `xefg_port_probe.exe`：加载 `libxess_fg`，只装 `ModelPort` + 解锁（不装
   ShaderPort、不打 keep_2x），创建 XeFG context，`SetNumInterpolatedFrames(3)`。
2. 看 `GetProperties` / `GetLastPresentStatus` / `framesPresented`，并比较
   4x 的 t=1/2 帧与 2x 生成帧是否逐位相同（相同 = 还是 2x 模型）。
3. 若失败，再装 `ShaderPort`（需要 54 个资产），重复 2。
4. 成功后再接进 consumer：在创建 context 之前安装，失败自动回退到现有
   `--unlock-mfg` keep_2x 路径，并打 `model_port_rejected` 原因。

风险：模型布局/程序资产必须精确匹配这一份 DLL（我们已匹配）；许可是 MIT 代码 +
Intel 条款下的程序数据，GPL 发布前要单独审；所有改动只在本进程内存里。

### 路径 B：副卡用 Intel（零代码的真多帧）

`apply_unlock_mfg()` 在 Intel 适配器上直接跳过补丁，注释写明“MFG model exists”
（`lock_frame/modules/150_probe_mfg.inc:142-147`）。也就是说：

- 消费端是 Intel Arc 时，`libxess_fg` 原生就能 >1 插值，3x/4x 是真多帧；
- 我们已有的 `SetNumInterpolatedFrames` / cadence 代码不用改，只需要硬件矩阵支持。

代价：把“副卡必须是 Intel”写进支持范围；对现在的 RX 6750 GRE 测试机不适用。
如果产品能接受，这是唯一**今天就能成立**的真多帧配置。

### 路径 C：移植 sm86 的引擎到消费端（成本最高）

如果一定要在 AMD 副卡上真多帧：

1. 拿不到它的源码/图，只有二进制资源包；必须先解决源码与许可（C++ MIT 可谈，
   model 15 的 54 个程序/权重是 Intel 条款）。
2. 它的 trunk 是 **CUDA/IMMA**，AMD 不能执行；需要把卷积/主干翻译成 DXIL（或
   HIP/ROCm），并重新验证数值。sm86 自己有过“全图 DXIL”的运行器分支，但在
   2026-10-22 删掉了（`ARCHITECTURE.md:549-550`），说明这条路他们评估过且不划算。
3. 架构改造：把“游戏进程内、NGX 参数、渲染卡资源”换成“副卡进程、共享槽输入、
   自己的 Present 节奏”。这部分我们其实已有大半（本地 MV/depth/颜色、cadence、
   overlay 链），但历史、reset、输出 Present 要重写。
4. 若副卡是 NVIDIA，CUDA 可用，移植量会小一档；但依然要私有图 + 历史 + Present。

现实评估：这是“另开一个项目”的量级，不适合作为当前迭代目标。

### 路径 D：不用 model 15 的“伪真多帧”

在现有库上无法做到：swapchain API 不暴露“按任意 t 调一次 2x 模型”的入口；
递归 2x（先 2x(A,B) 得 M，再 2x(A,M)/2x(M,B)）需要模型级调用和正确历史，
公开 API 给不了。简单的 MV 重投影画质不如现有 2x 模型输出，不值得。

## 4. 如果走路径 C，需要补的组件（供估算，不是现在做）

- **输入层**：当前/上一真实帧颜色、深度、MV、jitter、矩阵、reset；我们已有共享槽和
  `g_depth_ring`，但 model 15 需要上一帧深度，必要时上一帧后缓冲——要新增一对
  历史纹理（输出尺寸）。
- **图执行**：一次光流 + N 次合成；每张生成帧的 `t`、jitter 修正
  （`D = (jp − jc)·(1 − t)`）、输出格式/尺寸校验。
- **Present**：N 帧/真实帧的 vblank 节奏；我们 cadence 已有计数逻辑，但现在是
  XeFG 代理持有 Present，换引擎就要自己持有（overlay 链已经是一个可用的起点）。
- **资源/状态**：历史失效条件（句柄、尺寸、格式、网络源、reset）、描述符环、
  中间资源池（sm86 是 42 个中间资源 + 16384 描述符环，按输出尺寸重建）。
- **后端**：AMD 必须全 DXIL；NVIDIA 可先 CUDA。模型资产是硬门槛。

## 5. 建议的推进顺序

1. **先写 `xefg_port_probe.exe`（最小验证）**：
   - 加载 `third_party/xess/bin/libxess_fg.dll`（SHA 已匹配）；
   - 只装 `ModelPort` + 解锁（1/2/3/5/6，不打 keep_2x），不装 ShaderPort；
   - 创建 context，`SetNumInterpolatedFrames(3)`，看 `GetProperties` /
     `GetLastPresentStatus` / `framesPresented`，并比较 4x 的 t=1/2 与 2x 生成帧。
2. 若失败：装 `ShaderPort`（需要 54 个程序资产），重复第 1 步。
3. 成功后接进 consumer：在创建 context 之前安装，新增 `--mfg-model=port`（默认关），
   失败自动回退到现有 keep_2x 路径并打 `model_port_rejected` 原因。
4. 如果 port 仍被拒：
   - 产品接受副卡 Intel → 路径 B，写进支持矩阵；
   - 否则维持 2x 模型复用，并在 UI 明示“3x/4x 单帧质量等同 2x”，推荐 2x@60。
5. 路径 C 只有在“拿到 sm86 源码 + 许可澄清 + 明确 NVIDIA 副卡”三个条件同时满足时
   才立项；否则不要开始。

## 6. 待确认的问题

1. **54 个 DXIL 程序资产从哪来**：优先从 MFG-Unlocker 发布 DLL 的 RCDATA
   `7000..7054` 提取（带 `programs/<key>.dxil` 名与哈希）；备选是从 sm86 的
   `xefg_kernels.dll` 提取（清单 SHA 已是 `d74bd783…`，但条目名是 `cs_...`，
   需要一次 key↔bytes 映射）。
2. 许可：C++ 是 MIT；54 个程序数据中 46 个是 Intel 派生、受 XeSS SDK 条款约束，
   以 GPL-3.0 项目分发前需要单独确认。
3. 支持矩阵是否接受“真多帧仅限 Intel 副卡”（路径 B）作为兜底？
4. 如果 port 失败，是否接受“3x/4x 不承诺单帧画质，只承诺帧数/节奏”？

---

## 7. MFG-Unlocker V1.7.1 源码逐层拆解（实现依据）

### 7.1 它其实没有“自己的推理引擎”

`Engine=XeSS` 在 sm86 里是自己跑模型；MFG-Unlocker 走的是更轻的路线：
**让 Intel 的 `libxess_fg` 继续生成，只在进程内把模型/程序换成真多帧版本**。
三个组件：

| 组件 | 位置 | 作用 |
|---|---|---|
| `ModelPort` | `source/native/xess/port/model_port.h` | 在模型目录（RVA `0x15be920`）找到 donor `i0`（id 15）、fallback `i65`（id 18）、`i62`（id 17），断言不存在 id 16，然后 **把 donor 的 id 从 15 改成 16**；同时按 fallback 的元数据修正 reconstruction 资源布局、scheduling 标量、graph 节点表。权重本身不动（“MFG weights remain in the donor's table zero”）。 |
| `ShaderPort` | `source/native/xess/port/shader_port.h` | 钩 provider 的程序解码点（RVA `0x1ad217`），把解码出的原生程序按 SHA/大小匹配后换成 54 个 portable DXIL 程序；另有 7 个自有 shader（motion-aware splat ×2、5 个 U-Net conv）。AMD 上保留 Intel 的 `aa016078` / `efbe3788` 两个 U-Net 程序（实测更快且输出相同）。 |
| `xess_host_unlock::Unlock` | `source/native/xess/host_unlock.cpp` | 打 5 个抬上限站点（1/2/3/5/6），**不含** `fg_keep_2x_model`；`kGeneratedCapacity = 5`（最多 6x）。 |

装好之后仍然是 XeFG 的 swapchain API 在生成；调用方只是
`SetNumInterpolatedFrames(N)`。这正是我们 consumer 已经在做的调用，所以接入点是
“在创建 context 之前装这三层”，而不是重写 Present。

### 7.2 已核对的事实

| 事实 | 证据 |
|---|---|
| 我们的 `libxess_fg.dll` 就是它支持的 SDK 3.0.2 构建 | SHA-256 `ec5e0c65…75ab27` = `xess_runtime.h` 的 `kSdk302.fgSha256` |
| 我们的 `libxell.dll` 也匹配 | SHA-256 `d2030dcd…3ded8c7` = `kSdk302.llSha256` |
| model 15 确实在 DLL 里 | `ModelPort` 断言 `i0` 的 id == 15、`i65` == 18、`i62` == 17，且没有 16 |
| 54 个程序的清单与我们手上的资源包一致 | sm86 `xefg_kernels.dll` 第二份清单的 `model` 是 `"xess-fg 1.3.1.78 model 15, MFG-Unlocker 1.6.0 port"`，`source_sha256` = `d74bd783…` = MFG-Unlocker 的 `MANIFEST_SHA256` |
| 资源包里有对应字节 | 该清单 230 个条目（含 `cs_...` shader、weight、cuda）；部分 sha 与 MFG-Unlocker 自有 shader 完全一致（如 `8c2efb9e…`、`40e15941…`） |

### 7.3 接入我们代码的改动面

1. **新增 shim**（可做成 `lock_frame/xefg_mfg_port.{h,cpp}`）：vendor `model_port.h`、
   `shader_port.h`、`host_unlock.cpp` 的最小依赖（`runtime_layout.generated.h`、
   `xess_layout`）。它们的 `logEvent`/`Self()` 换成我们自己的日志与 payload 加载器。
2. **程序资产**：把 54 个 `<key>.dxil` + `programs.tsv` 作为资源/数组嵌进我们的
   DLL/exe，或从磁盘目录加载（`ShaderPort` 已经接受 `dataRoot`）。
3. **consumer 启动顺序**：加载 `libxess_fg` → 可选 `ShaderPort` → `ModelPort` →
   `Unlock(..., keep2xModel=false)` → `D3D12CreateContext` →
   `maxInterpolatedFrames = N`。必须保证在第一个 context 之前完成。
4. **回退**：任何 shim 抛异常（`port_model_identity`、`shader_port_identity`、
   `reconstruction_program_pair_missing`）就撤销并走现有 keep_2x 路径，日志写明原因。
5. **UI**：`--mfg-model=port|keep2x`，发布默认 **keep2x**；`port` 是显式实验入口。

### 7.4 最小实验的判据

- 成功：`maxSupportedInterpolations = N`、`framesPresented = N+1`，且 4x 的
  t=1/2 帧与 2x 生成帧**不逐位相同**（说明换了模型）；`gpu` 随 N 次线性。
- 失败：`model_port_rejected` / `shader_port_identity` / provider 仍只出 2x；
  记录具体异常串与模型目录读取结果。
- 画质：与 sm86 的方法一致，拿跳帧真值比 t=1/4、1/3 的重影；只对 HUD/场景区块
  分别统计，避免把运行库算法问题算进来。

### 7.5 许可与风险

- `ModelPort`/`ShaderPort`/`Unlock` 的 C++ 是 MIT；54 个程序数据里 46 个是 Intel
  派生，受 XeSS SDK 条款约束，**不在 MIT 内**。GPL-3.0 项目要分发这部分必须先审。
- 版本强绑定：RVA 与字节断言只对 `libxess_fg 1.3.1.78`（ec5e0c65）成立；换版本会
  直接抛异常，不会半改。
- 只改本进程内存；但 `ModelPort`/`ShaderPort` 会修改已加载的商业 DLL 的代码/数据，
  与现有 `--unlock-mfg` 同级别，需保留 research-only 提示。
- AMD 不需要 CUDA：默认就是 DXIL 程序；CUDA 只是 RTX 上的可选加速。
- 这套 shim 的失败模式比“自己写引擎”可控：失败就回退 keep_2x，不会出黑屏。

## 8. 代码对齐状态（2026-11-21）

已按 MFG-Unlocker 的推荐路径（`PreparePort`）改好结构：

| 项 | 位置 | 状态 |
|---|---|---|
| 解锁站点支持 `keep2xModel`（5 个站点 vs 6 个） | `src/xefg_unlock.h` `build_table` / `Apply(..., bool keep2xModel)` | ✅ |
| ModelPort/ShaderPort shim | `lock_frame/xefg_mfg/`（`runtime_layout*`、`port/model_port.h`、`port/shader_port.h`、`runtime_payload.h`） | ✅ 已 vendor |
| 在 context 之前安装，失败回退 | `lock_frame/modules/150_probe_mfg.inc` `apply_unlock_mfg()` | ✅ |
| 命令行路由 | `--mfg-model=port|keep2x`（默认 keep2x；`auto` 仅作遗留） | ✅ |
| 构建拷贝 | `lock_frame/build.sh` 复制 `xefg_mfg/` | ✅ |
| 54 个 DXIL 程序 + 7 个 replacement shader | `scripts/extract_mfgunlocker_assets.py` → `build/xefg_programs`（gitignored） | ✅ 已生成 |

行为：

- **keep2x（默认）**：保持旧行为；3x/4x 复用 2x 模型，CP2077 上画质明显更好。
- `port`：显式实验入口，走 ShaderPort → ModelPort → 解锁 keep2x=false；
  缺资产/布局不符/vendor 不符时回退 keep2x，并打 `model_port_rejected`。
- `auto`（遗留值，不推荐）：先试 port，失败回退 keep2x。
- 没有 `xefg_programs` 时，日志是
  `model_port_rejected{reason:"assets_incomplete"}` → 2x 模型路径。

因此**代码已经对齐，运行时是否走真多帧取决于那 54 个程序资产是否就位**。
`third_party/MFGUnlocker-v1.7.1/MFGUnlocker.dll` 已经在树里，提取脚本也已就位：

```bash
python3 scripts/extract_mfgunlocker_assets.py
# decoded 690377 bytes -> 54 programs in build/xefg_programs
# decoded 271924 bytes -> 7 replacement shaders
# packed program group sha256 c1641e69...
```

输出在 `build/xefg_programs`（`.gitignore` 已忽略 `build/`，不会把 Intel 程序数据提交进仓库）。
**这些资产不要随发布包分发**，除非先完成 Intel XeSS SDK 条款审查；它是本机研究资产，
不属于本项目的 MIT/GPL 源码。
`build.sh` 生成的 exe 就在 `build/lock_frame.exe`，所以资产天然在 `<exe>\xefg_programs`。
用 `lock_frame/build.bat` 原地构建时，把 `build/xefg_programs` 复制到 `lock_frame/xefg_programs`
（或 exe 旁边）即可。

### 8.1 实机 probe（2026-11-21，RX 6750 GRE + libxess_fg 1.3.1.78）

`lock_frame/build.sh consumer` 用 MSVC 14.44 编译通过；用 Windows `cmd.exe` 运行：

```text
lock_frame.exe --fg-probe --unlock-mfg --mfg-model=port
```

输出（已实测）：

```text
xefg-mfg: shader_replacement_set "vendor_id":4098,
  "kept_intel":["shaders/unet_aa016078.dxil","shaders/unet_efbe3788.dxil"],
  "replaced":["shaders/splat_motion_aware.dxil","shaders/splat_motion_aware_inverted.dxil",
              "shaders/unet_3ed03daf.dxil","shaders/unet_748b0baf.dxil",
              "shaders/unet_cb391582.dxil"]
xefg-mfg: shader_port_applied "programs":54,"replacements":5
xefg-mfg: model_port_applied "donor_model":15,"target_model":16,"edits":167,
  "mfg_weights_preserved":true,"real_vendor_preserved":true
--unlock-mfg: ported MFG model route active (model 15 -> 16, 5 sites, no fg_keep_2x_model)
probe: maxSupportedInterpolations 3  ->  4x output
xefg-mfg: model_port_restored / shader_port_restored
```

`--mfg-model=keep2x` 也验证过：stock 值 1（2x）→ 解锁后 3（4x），旧路径未被破坏。

### 8.2 实机 3x/4x（2026-11-21，CP2077，RTX 3080 Ti 渲染 + RX 6750 GRE 消费）

捕获固定为 **layered overlay + `--wgc-minint 0`**；游戏锁 40/30 fps，4K 无边框。

| 档位 | 模型/解锁 | 输入 | 输出 | 负载 | 结果 |
|---|---|---|---|---|---|
| 3x | `maxSupportedInterpolations=2`、`MFG available up to 3x` | `game(vb): 3x=40`、clock 40.0 | `xN=3.00`、`show=120` | load 30%、gpu 1.3ms、total 8.1ms | skip 0，无 XeFG 错误 |
| 4x | `maxSupportedInterpolations=3`、`MFG available up to 4x` | `game(vb): 4x=40`、clock 30.0 | `xN=4.00`、`show=118–120` | load 77–87%、gpu 16.3ms、total 26ms / 33.3ms | skip 0–3，无 XeFG 错误 |

两份日志都有：

```text
xefg-mfg: shader_port_applied "programs":54,"replacements":5
xefg-mfg: model_port_applied ... "edits":167
--unlock-mfg: ported MFG model route active (model 15 -> 16, 5 sites, no fg_keep_2x_model)
```

并出现大量 `shader_native_lookup` / `shader_supplied`，说明 model 15→16 的模型/程序确实在跑。
`4x` 的 GPU 余量较小（total 26ms，预算 33.3ms）；重场景可能需要降一档，`3x` 余量最大。
画质结论（2026-11-21）：2x 干净；4x_keep2x 只有 UI 拖影和少量边缘伪影；
4x true model 15/16 的边缘伪影明显更严重，且 `--fg-use-matrix`、`--no-upsample`、
jitter/scene 调整都没有改善。因此 **3x/4x 默认回到 keep2x**，true model 只保留为
`run_3x_true_model.bat` / `run_4x_true_model.bat` 的可选实验入口。

> **2026-11-22 复核**：port 路线已实机确认在跑（`model_port_applied 15->16`、`shader_supplied=28`），
> fail-black 三条都不黑说明 4x 的合成是成功的；但 4x 画质明显更差，
> `--mfg-replace-all` 与 capacity 4/count 3 均无改善，jitter0 还会让 port 合成静默消失。
> **决定：所有倍率保持 keep2x**，port 只作为显式实验入口。
> 证据链见 `docs/design/OPTISCALER_XEFG_MFG_SCHEDULING.md` §9–§10。

**捕获结论**：2x/3x/4x 的 WGC 捕获都使用 `--overlay-layered --wgc-minint 0`；
`--wgc-minint -1`（Windows 默认 15–16ms）会卡高帧捕获，不再使用；`--capture-monitor` 在 overlay 场景下不可用（捕获 0 帧、画面冻结）。
### 8.3 审计记录：init 上限 vs 当前倍率，以及 port 路线的倍率策略（2026-11-21）

> 状态：**只记录，不改运行时**。当前发布主版本是 `lock_frame`；动态多帧
> （`--dyn-mfg` / `--interp-auto`）暂不开放，发布路径只走固定倍率，因此下面第 1 条
> 与现状等价，等启用动态多帧时再落地。`src/xess_mfg_consumer` 是协议 v22 旧包，
> 不随主版本同步。

**1. `maxInterpolatedFrames` 是上限，不是当前倍率。**

`xefg_swapchain_d3d12_init_params_t::maxInterpolatedFrames` 的定义是“不会超过这个数”
（`xefg_swapchain_d3d12.h:41-53`），而 SDK 的默认产出就是该上限
（`xefg_swapchain.h:462-470`）；`SetNumInterpolatedFrames` 只接受 `1..init 上限`，
超出返回 `ERROR_INVALID_ARGUMENT`（`xefg_swapchain.h:469-470`）。

`lock_frame` 现在把 `--interp`/起档值直接写进 init
（`lock_frame/modules/040_cadence.inc:1127-1129`，已建链分支 `:1261`），因此：

```
init ceiling = 3 (--interp 3)  ->  SetNumInterpolatedFrames(4/5) = ERROR_INVALID_ARGUMENT
                                 ->  --dyn-mfg / --interp-auto 只能停在启动值
```

固定倍率下这没有影响（init 上限 = 当前倍率）。启用动态多帧时的落地方式：

```
can_set_count = g_api.has_mfg();
init_ceiling  = can_set_count ? g_fg_interp_max : want_interp;
ip.maxInterpolatedFrames  = init_ceiling;
ip2.maxInterpolatedFrames = init_ceiling;

// init 成功后、SetEnabled(1) 之后：
g_fg_interp_actual = init_ceiling;      // SDK 默认产出 = 上限
fg_apply_interp("after SetEnabled");    // 真正下发当前 count
```

与上游一致：OptiScaler `intTarget = _maxInterpolationCount` → init，Dispatch 里
`SetNumInterpolatedFrames(targetCount)`（`XeFG_Dx12.cpp:335-355,729-777`）；
MFG-Unlocker `params.maxInterpolatedFrames = 5`（失败退
`USE_MAX_SUPPORTED_INTERPOLATED_FRAMES`），再 `SetNumInterpolatedFrames(frames-1)`
（`d3d12_host.cpp:674-698`）。

**2. port 路线请固定倍率。**

真多帧按 count 选模型：1 张插值 = 2x 模型（18），2 张及以上 = model 15→16。
`SetNumInterpolatedFrames` 文档明确可能切换模型并触发着色器编译/资源重分配
（`xefg_swapchain.h:472-473`）。所以 `--mfg-model=port` 不要配
`--dyn-mfg` / `--interp-cadence` / `--interp-auto`；A/B 时每次只测一个固定 `--interp`。

```bat
:: 固定倍率 + 真多帧（每次重启后只测一个倍率）
lock_frame.exe --mfg-model=port --interp 3 ...
lock_frame.exe --mfg-model=port --interp 4 ...
```

**3. 其他。**

- `--interp N` 同时是解锁补丁的容量（`xefg_unlock::Apply(want)`、`ll_generated_gate`）。
  要摸到 6x 必须 `--interp 5` 启动，否则库上限只有 3。
- 如果将来把“手动切倍率”做进 UI，照 OptiScaler 在用户改倍率后补一次
  `SetEnabled(0)` → `SetEnabled(1)`（`WAR_xefgRequestFGToggle`，`XeFG_Dx12.cpp:748-787`）；
  Dynamic MFG 自己算出的变化不需要 toggle。
- consumer 还没有传 `UITEXTURE_NOT_PREMUL_ALPHA`；UI 流默认关闭，打开前先确认生产端
  UI 数据是否预乘。
