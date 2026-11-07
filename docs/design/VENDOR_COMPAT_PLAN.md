# 厂商兼容计划：ReShade 侧（N 卡 / A 卡）与 bridge 侧（N 卡 / A 卡）

> 日期：2026-11-07　状态：**计划**（本次只出方案，未改代码）
> 前置结论全部来自本仓库实测文档：`FINAL_CONFIG.md` §1/§2.1.2/§9.6、`HANDOFF.md` §8.5/§8.6、
> `MFG.md`、`XESS_FG_NOTES.md`、`ENV_REPORT.md`。
> OptiScaler 参考实现：本机完整源码 `<DOWNLOADS>\OptiScaler-master.zip`
> （已解到 `temp/optiscaler_src/`，`temp/` 在 `.gitignore` 里，不进库）。

**术语约定**：本文把「(n/a) 卡兼容」读作 **N 卡（NVIDIA）/ A 卡（AMD）两种显卡**，
即"任何一张卡当生产端、任何一张卡当消费端都能跑"。若还包含 I 卡（Intel），
本计划的「XeSS 输入源」与「Intel 副卡不打 `--unlock-mfg`」两条已一并覆盖。

---

## 0. 结论先行

1. **全项目真正的厂商绑死点只有三处**，其余代码（D3D12 跨适配器堆、共享 fence、WGC 抓取、
   HDR10 上屏、XeFG 调用、OSD/UI）本来就是厂商无关的：

   | # | 位置 | 绑死内容 | 后果 |
   |---|---|---|---|
   | 1 | `src/game_capture/game_capture.cpp:301` `hook_module_if_ngx()` | 只认 `_nvngx.dll`，只挂 `NVSDK_NGX_D3D12_EvaluateFeature` | 游戏不开 DLSS（或跑在 A 卡上）⇒ 一个输入都拿不到，bridge 白等 |
   | 2 | `src/bridge/bridge.cpp:739` `find_amd_adapter()` | `if (d.VendorId == 0x1002)` | 副卡不是 A 卡 ⇒ `:5608` 直接 `no AMD adapter`，退出码 3 |
   | 3 | `src/bridge/bridge.cpp:1476` + `src/xefg_unlock.h` | `--unlock-mfg` 无条件打 6 处字节 | Intel 副卡上会把**原生** MFG（真模型、质量更好）拽成"每帧复用 2x 模型" |

2. **消费端（bridge）不需要按厂商分支**：XeFG 本身就是跨厂商库（`XESS_FG_NOTES.md` §1.5：
   先例项目已在 RTX 5050 上跑通；它只有"非 Intel 卡最多 1 个插值帧"这一条厂商钳制，
   而这条已被 `--unlock-mfg` 解开，代价是插值帧复用 2x 模型，见 `MFG.md` §1）。
   所以「bridge n/a 兼容」= 泛化副卡选择 + Intel 不 patch + 用现有硬件做一次 N 卡副卡实跑验证。
   **约 1 人日**（M6.1 已完成，见 §5：N 卡上 `CreateContext SUCCESS`、`--unlock-mfg` 1→3 均实测通过）。

3. **生产端（addon）才是真工作量**：要把"输入从哪来"抽象出来（NGX / FFX / XeSS / ReShade 兜底），
   协议升 v6 携带 `input_api + flags + 相机 near/far/fov`。**约 3–5 人日**（含两种副卡组合的实测）。

4. **明确不做 GPU 伪装 + 超分替换**。OptiScaler 的 `spoofing/Dxgi_Spoofing.cpp` 是为了
   "在 A 卡上骗游戏开 DLSS，再把 DLSS 换成 FSR/XeSS 执行"。我们的架构是"游戏照常渲染，
   我们只取 MV/深度"，一旦引入伪装就必须接管超分本身，和 `FINAL_CONFIG.md` §1 的两进程模型冲突。

5. **MV / 光流方向继续不动**。`FINAL_CONFIG.md` §2.1.2 结论 1：`libxess_fg 1.3.1` 不消费
   tag 进来的 MV。这次兼容改造的价值是 **深度 + jitter + render size + 相机参数**，不是 MV
   （MV 仍可 `SkipMv=1`）。

---

## 1. 现状审查（逐点，带行号）

### 1.1 addon（游戏进程）

| 事实 | 位置 | 说明 |
|---|---|---|
| 唯一的输入源是 NGX | `game_capture.cpp:216` `hook_evaluate()` | 从 `NVSDK_NGX_Parameter` 取 `MotionVectors/Depth/Jitter/Width/Height/MV_Scale` |
| 只挂 `_nvngx.dll` | `game_capture.cpp:307` | M2 实测结果（CP2077 的真身是驱动目录 shim） |
| 装载钩子只转发给 NGX | `game_capture.cpp:318-330` | 新增源只需在这两个函数里多加一次路由 |
| 帧循环入口 | `game_capture.cpp:1543` `on_begin_effects()` | 现有流程：`g_ngx_valid` → 选流尺寸 → barrier → `CopyResource` → `publish()` |
| 资源状态机 | `game_capture.cpp:205-211` | 只跟踪两个指针，初始状态写死 `shader_resource` / `depth_stencil` |
| 协议 | `src/mgpu_shared.h:35` `kSharedVersion = 5` | `FrameShared` 只有 `jitter_x/y、render_w/h、mv_scale_x/y、ready`；**没有相机参数、没有输入 API、没有 flags** |
| 配置 | `game_capture.cpp:1490` `load_config()` | `[capture] Color/RenderWidth/RenderHeight/StreamPercent/SkipMv/StreamAlign/DepthConvert/Log` |
| 构建 | `src/game_capture/build_addon.bat` + `scripts/build_addon.sh` | **只编译一个 `.cpp`**（sh 拷贝所有 `*.cpp`，但只把 `srcs[0]` 传给 bat）⇒ 多文件要先解决 |

### 1.2 bridge（消费进程）

| 事实 | 位置 | 说明 |
|---|---|---|
| 副卡硬编码 A 卡 | `bridge.cpp:739-755` | 找不到就 `:5608` 退出 |
| 设备创建日志写死 "on AMD" | `bridge.cpp:802` | 只是文案，但会误导排查 |
| FG 后端只有 XeFG | `bridge.cpp:1447` `init_fg()` + `:4385` `run_frame()` | `xefg_dyn::Api` 从 `libxess_fg.dll` 解析（`src/xefg_dyn.h:139` 默认 exe 目录） |
| MFG 补丁无条件 | `bridge.cpp:1476`、`xefg_unlock.h` | 与副卡厂商无关地打 6 处 |
| 换链/上屏参数 | `bridge.cpp:1548-1555` | `R10G10B10A2_UNORM` + `FLIP_DISCARD` + `DXGI_SCALING_STRETCH`（N 卡同样支持；`E_ACCESSDENIED` 的真因是"窗口所在显示器不在本适配器上"，`:1585-1636` 已打印） |
| 相机矩阵自己合成 | `bridge.cpp:515-543` + `--fg-proj-rz/--fg-fov/--fg-zn` | 因为 DLSS-SR 不给矩阵（`M2_RESULTS.md` 遗留风险）。**FSR/XeSS 能直接给相机参数**，协议 v6 后不必再猜 |
| UI 只读信息写死 | `src/mgpu_ui/mgpu_ui.cpp:312/314` | `"XeFG 1.3.1 (libxess_fg)"`、`"AMD Radeon RX 6750 GRE"` |

### 1.3 确认"哪些东西厂商无关"（本次不要动）

- 传输：`CreateFileMappingW` + `OpenExistingHeapFromFileMapping` + `ALLOW_CROSS_ADAPTER` + 共享 fence（M0.2/M1）。
- 颜色：WGC 抓 DWM 已经合成好的画面（`M4_WGC_RESULTS.md`）。
- 上屏：代理交换链由 XeFG 创建，HDR10 `SetColorSpace1`。
- 时序：XeLL 打标记 + 锁帧（`FINAL_CONFIG.md` §5.1/§5.4/§9.15）。

---

## 2. OptiScaler 怎么做的（可以直接照抄的四层）

```
加载期：hooks/LibraryLoad_Hooks.cpp + DllNames.h
        ├─ 按 DLL 名匹配（nvngx / fsr2 / fsr3 / libxess / libxess_fg …）
        └─ 命中就挂该 API 的输入钩子（HookFSR3Inputs / XeSSProxy::InitXeSS / …）

输入期：inputs/{NVNGX_DLSS_Dx12,FfxApi_Dx12,FSR3_Dx12,XeSS_Dx12}.cpp
        └─ 全部翻译成统一字典 NVSDK_NGX_Parameter（MV/Depth/Jitter/Width/MV_Scale/FSR.camera*）

泵血期：inputs/FG/Upscaler_Inputs_Dx12.cpp  UpscaleStart()/UpscaleEnd()
        └─ 把上面的资源 + 失效语义交给 FG：Dx12Resource{type, resource, state, validity}

消费期：framegen/IFGFeature.h（抽象）+ xefg/ ffx/ dlssg/ reprojection/（四个实现）
        └─ FG_Hooks.cpp 按 activeFgOutput 直接 new 对应实现；每个实现自己吃 FG_Constants
```

### 2.1 可直接引用的证据（路径相对 `OptiScaler-master/OptiScaler/`）

| 主题 | 文件:行 | 摘录要点 |
|---|---|---|
| DLL 名表 | `DllNames.h:133-140` | `DEFINE_NAME_VECTORS(fsr2,"ffx_fsr2_api_x64")` … `xess:"libxess"`、`xefg:"libxess_fg"`、`nvngx:"nvngx","_nvngx"` |
| FSR 钩子安装 | `inputs/FSR3_Dx12.cpp:844/934` | `HookFSR3Inputs(module)`、`HookFSR3Dx12Inputs(module)` |
| FSR 输入字段 | `inputs/FfxApi_Dx12.cpp:636-668` | `jitterOffset`、`motionVectorScale`、`depth.resource`、`motionVectors.resource`、`cameraNear/Far/FovAngleVertical` 全塞进 NGX 参数 |
| FSR 真实字段定义 | 本地 `third_party/ffx/include/FidelityFX/host/ffx_fsr3upscaler.h:196-215`（经典 SDK 的 `FfxFsr3UpscalerDispatchDescription`） | `jitterOffset / motionVectorScale / renderSize / upscaleSize / cameraNear / cameraFar / cameraFovAngleVertical / frameTimeDelta / viewSpaceToMetersFactor` —— **FSR 3.1 直接给了相机参数**（运行期 DLL 的 ffx-api 版叫 `ffxDispatchDescUpscale`，见 §6.1） |
| XeSS 输入字段 | `inputs/XeSS_Dx12.cpp:222-250` | `jitterOffsetX/Y`、`pDepthTexture`、`pVelocityTexture`、`pColorTexture`、`inputWidth/Height`、子矩形基址 |
| XeSS MV 单位换算 | `inputs/XeSS_Dx12.cpp:192-209` | `USE_NDC_VELOCITY` ⇒ `MV_Scale=(±W/2,∓H/2)`，`HIGH_RES_MV` 时用 `outputResolution`；否则 `1.0` |
| XeSS 初始化标志 | 同上 `:31-51` | `INVERTED_DEPTH/JITTERED_MV/HIGH_RES_MV/LDR_INPUT_COLOR/ENABLE_AUTOEXPOSURE` → NGX flags |
| FG 抽象 | `framegen/IFGFeature.h:28-48,156-166` | `FG_ResourceType{Depth,Velocity,HudlessColor,…}`、`FG_ResourceValidity{ValidNow,UntilPresent,…}`、`SetCameraValues/SetJitter/SetMVScale` |
| FG 选择 | `hooks/FG_Hooks.cpp:326-336,452-462` | `activeFgOutput == FSRFG/XeFG/DLSSG` ⇒ `new FSRFG_Dx12()/new XeFG_Dx12()/new DLSSG_Dx12()` |
| FG 相机兜底 | `inputs/FG/Upscaler_Inputs.cpp:10-49` | 游戏没给 `cameraNear/Far/Fov` 时用配置值；`DepthInverted()` 时交换 near/far；水平 FOV 按宽高比转垂直 |
| XeFG 矩阵合成 | `framegen/xefg/XeFG_Dx12.cpp:849-886` | 有相机位置就用 view 矩阵；否则 `XMMatrixPerspectiveFovLH(vfov, aspect, near, far)` —— 与 `approx_projection_rz()` 同思路，但参数来自真实相机 |
| XeFG + Reflex 并存 | `low_latency/input/input_common.cpp:1544-1605` | 已有低延迟输出（Reflex/AntiLag2）时，XeFG 用**自己的、只打标记**的 XeLL 上下文；我们 bridge 独占进程，暂时没这个问题，但换 FSRFG/DLSSG 后要重新审视 |
| 设备识别 | `misc/IdentifyGpu.cpp:26-96` | 枚举所有适配器 → `LUID/vendorId/deviceId/VRAM/名字`，按优先级排序，第一块当主卡 |

### 2.2 三条值得抄的设计决策

1. **输入归一化**：所有 API 的输入先翻译成一张"参数字典"，上层只认一种。我们的等价物是
   `FrameInputs`（§3.1）。注意**不要**抄成 `NVSDK_NGX_Parameter`：我们不在游戏里跑 FG，没必要背那个接口。
2. **失效语义显式化**：同一份资源可能"只在当前帧有效"，也可能"活到下次 present"。
   我们换成跨进程共享内存后天然只有一种语义（publish 后到槽位被复用前有效），
   但协议 v6 必须把"这一槽的 GPU 写真的做完了"显式化（已有 `FrameShared.ready`）。
3. **按 DLL 名路由，不按显卡厂商路由**：输入源由"游戏用了哪个超分"决定，
   而不是由"哪块卡"决定——同一块 A 卡上游戏也可能用 XeSS。

---
 
## 3. 目标架构

### 3.1 addon（生产端）：输入源抽象 —— 已实现

```
src/game_capture/game_capture.cpp   宿主：ReShade 事件 / 传输 / 配置 / 资源状态机
src/game_capture/inputs.h           FrameInputs + Source + 宿主服务声明 + 工厂
src/game_capture/in_ngx.cpp         ngx 源（DLSS，_nvngx.dll）
src/game_capture/unity.cpp          编译入口（build_addon.bat 只编一个 TU）
    in_ffx.cpp / in_xess.cpp / in_reshade.cpp   ← M6.4 / M6.5 再加
```

接口（三件事：谁的输入、这一帧有没有、资源初值/生命周期交给谁）：

```cpp
// inputs.h —— 与下面的实际实现一致
namespace mgpu::input {

struct FrameInputs
{
    ID3D12Resource* mv = nullptr;        // 可为空（SkipMv=1 或该 API 不给）
    ID3D12Resource* depth = nullptr;     // 必需
    uint32_t render_w = 0, render_h = 0; // 超分输入尺寸（= MV/depth 尺寸）
    uint32_t output_w = 0, output_h = 0; // 超分输出尺寸
    float jitter_x = 0.0f, jitter_y = 0.0f;
    float mv_scale_x = 1.0f, mv_scale_y = 1.0f;
    float near_plane = 0.0f, far_plane = 0.0f, vfov_rad = 0.0f;
    uint32_t flags = 0;                 // mgpu::kInputFlag*
    uint64_t serial = 0;
};

// 宿主服务：源只报告指针，状态机/延迟释放/日志归宿主
void  logf(const char* fmt, ...);
bool  ensure_minhook();
void  track(ID3D12Resource* mv, ID3D12Resource* depth,
            reshade::api::resource_usage mv_state, reshade::api::resource_usage depth_state);
void  hold(ID3D12Resource* res);

class Source
{
public:
    virtual const char* name() const = 0;
    virtual uint32_t api_id() const = 0;
    virtual void on_module(HMODULE module) = 0;   // 每次 LoadLibrary 成功都会被叫一次
    virtual void scan_existing() {}               // add-on 注入前就已加载的模块
    virtual bool poll(FrameInputs& out) = 0;      // 每帧一次；false = 本帧没有新输入
};

Source* make_ngx_source();     // M6.4: make_ffx_source()，M6.5: make_xess_source()
} // namespace mgpu::input
```

> 设计取舍：`state_hint()` 没有做成接口方法，而是让源在 `hook_evaluate` 式的回调里直接
> `input::track(mv, depth, 期望状态...)`——和原来 NGX 的写法一模一样（只在指针变化时重置状态），
> 少一层间接，也不会出现"提示和实际报告不一致"。

宿主侧改动（`game_capture.cpp`，均已实现）：

| 现状 | 改成 |
|---|---|
| `hook_load_library_{ex_,}w()` 里只调 `hook_module_if_ngx` | `notify_sources(m)`：把模块交给每个源，各自按路径判断 |
| `g_ngx_reported / g_ngx_valid / g_ngx_consumed` | `g_inputs`（`fetch_inputs()` 每帧 `poll` 到它），`g_input_api` |
| `on_begin_effects()` 开头判 `g_ngx_valid` | `fetch_inputs()`：第一个有数据的源胜出并**固定**；`[capture] Input=` 可强制 |
| `g_mv_ptr/g_depth_ptr/g_*_state` 写死初值 | 源通过 `input::track()` 给初值（NGX 仍是 shader_resource / depth_stencil） |
| `load_config()` | 新增 `[capture] Input=auto\|ngx\|ffx\|xess\|depth`（大小写不敏感） |
| 日志 `NGX provider:` / `NGX hooks active` | 多一行 `input source: ngx (api 1) - pinned for this process`；等不到时每 120 帧 `no upscaler input yet (waiting for ngx)` |


### 3.2 协议 v6（`src/mgpu_shared.h`）—— 已实现

保持"版本不匹配就拒绝通信"的既有规则，新增字段**只加在结构体尾部**（已有字段偏移不变）：

```cpp
inline constexpr uint32_t kSharedVersion = 6;

// 输入源 ID / 标志与 bridge 共用同一份头文件，避免两边各写一套常量
inline constexpr uint32_t kInputApiNone = 0, kInputApiNgx = 1, kInputApiFfx = 2,
                          kInputApiXess = 3, kInputApiDepth = 4;
inline constexpr uint32_t kInputFlagInvertedDepth = 1u << 0, kInputFlagMvNdc = 1u << 1,
                          kInputFlagMvJittered = 1u << 2, kInputFlagHiResMv = 1u << 3,
                          kInputFlagHasCamera = 1u << 4, kInputFlagHdr = 1u << 5;
inline uint64_t pack_luid(int32_t high, uint32_t low);      // 头文件保持不依赖 windows.h

struct ProducerInfo                    // 新增，写在 SharedControl 尾部
{
    uint32_t input_api;                // kInputApi*
    uint32_t input_flags;              // kInputFlag*
    uint32_t render_w, render_h;       // 超分输入尺寸（= MV/深度尺寸）
    uint32_t output_w, output_h;       // 超分输出尺寸
    uint64_t device_luid;              // 生产端 ID3D12Device::GetAdapterLuid()
};

struct FrameShared                     // 尾部追加（v6）
{
    // ... 现有 frame_id / jitter / render_w/h / mv_scale / ready ...
    float near_plane, far_plane, vfov_rad;   // 相机（NGX 没有，填 0）
    float frame_time_ms;                     // 游戏自己的 present 间隔
};
```

消费端（bridge）读法（均已实现）：

- `input_flags & kInputFlagInvertedDepth` ⇒ **自动**置 `XEFG_SWAPCHAIN_INIT_FLAG_INVERTED_DEPTH`
  （不必再手写 `--fg-depth-inv`；在 `init_fg()` 之前从共享块读，见 `open_shared()`）；
- `input_flags & kInputFlagHasCamera` ⇒ 用 `frames[slot].near/far/vfov` 合成投影
  （`approx_projection[_rz](m, aspect, fov_rad, zn[, zf])`），**取代 `--fg-fov` 手调常量**；
  日志第一条 `frame constants:` 会打 `camera=producer ...` 或 `camera=synthetic (--fg-fov ...)`；
- `device_luid` ⇒ 副卡选择自动"不是这块卡"（`peek_producer()` 在选卡前读一次）；
- `frames[].frame_time_ms` ⇒ 与我们的捕获节奏交叉校验一次（差 >35% 打一行提示；
  `frameRenderTime` 仍用我们自己的估算，`FINAL_CONFIG.md` §5.4）；
- `kInputFlagMvNdc` ⇒ 留给 M6.4（"MV 是死输入"的前提后来被推翻，换算逻辑随光流源一起落地）。

### 3.3 bridge（消费端）：副卡选择 + 厂商分支

```cpp
// bridge.cpp（替换 find_amd_adapter）
struct AdapterPick
{
    ComPtr<IDXGIAdapter1> adapter;
    DXGI_ADAPTER_DESC1    desc{};
    const wchar_t*        why = L"";
};

// --gpu <spec> 支持的写法：
//   "0" / "1"            适配器序号（EnumAdapters1 顺序）
//   "10de" / "1002" / "8086"  按厂商 ID
//   "nv" / "amd" / "intel"    同上，别名
//   "luid:1:2A3B"             精确 LUID
//   "6750" / "3080"           描述子串（不区分大小写）
static bool  adapter_matches(const DXGI_ADAPTER_DESC1& d, const std::wstring& spec);
static AdapterPick find_consumer_adapter(HWND wnd, bool have_producer, uint64_t producer_luid);

// 选择顺序（写进日志，出问题一眼看出选了谁、为什么）
// 1) --gpu 显式指定
// 2) 不是生产端那块卡（用协议 v6 的 device_luid；缺失时退回 --producer-luid）
// 3) 覆盖我们窗口所在显示器的适配器（CreateSwapChainForHwnd E_ACCESSDENIED 的唯一硬约束）
// 4) 非软件适配器里专用显存最大的那块
```

同时改三处文案/分支：

| 位置 | 现在 | 改成 |
|---|---|---|
| `bridge.cpp:802` | `devices ready (D3D11 + D3D12 on AMD)` | 打印 `%ls` 适配器名 + vendor/device/LUID/显存 |
| `bridge.cpp:5608` | `no AMD adapter` return 3 | `no usable consumer adapter`，并列出所有适配器 + 提示 `--gpu` |
| `bridge.cpp:1476` + `xefg_unlock.h` 调用点 | 无条件 patch | 读 `desc.VendorId`：**0x8086（Intel）不 patch**，日志说明"native MFG, model exists"；其他厂商照旧 patch |

`--fg-probe` 增强：不管有没有游戏，都打印
`adapters[] / chosen / vendor / XeFG version / maxSupportedInterpolations（patch 前后各一次）`，
这样"换一块副卡能不能跑"是一次命令就能回答的问题。

---

## 4. 兼容矩阵

### 4.1 生产端（游戏进程，ReShade add-on）

| 游戏跑在 | 游戏开启的超分 | 今天 | 计划后 | 输入源 |
|---|---|---|---|---|
| N 卡 | DLSS-SR | ✅ | ✅ 保持不变 | `ngx` |
| N 卡 | FSR 2.2/3.x | ❌ | ✅ | `ffx` |
| N 卡 | XeSS | ❌ | ✅ | `xess` |
| A 卡 | FSR 2.2/3.x | ❌ | ✅ | `ffx` |
| A 卡 | XeSS | ❌ | ✅ | `xess` |
| I 卡 | XeSS | ❌ | ✅ | `xess` |
| 任意 | 无超分（原生/FSR1/TAAU） | ❌ 完全没输入 | 🟡 只有深度（`depth` 源）+ bridge 侧光流补 MV（`MGPU_FG_OF`，前 6 帧恒零，见 `HANDOFF.md` §8.5） | `depth` |

> 说明：`depth` 源在 5K 下把深度降采样到 1440p 的代价已经在 `record_downsample()` 里做过（`StreamPercent`），
> 但它拿不到相机参数，只能靠 bridge 的 `--fg-fov/--fg-proj-rz` 猜。**它是兜底，不是主路径。**

### 4.2 消费端（bridge 进程）

| 副卡 | 计划前 | 现在（M6.1 后） | 备注 |
|---|---|---|---|
| A 卡（6750 GRE，现役） | ✅ | ✅ 不变 | 主战场 |
| N 卡（30/40/50 系） | ❌ `no AMD adapter` | ✅ 能力已实测（`--gpu 10de`：`CreateContext SUCCESS`，`--unlock-mfg` 1→3） | 整链实跑用 `run_mgpu_pbz_3x40_nvgpu.bat`（M6.6 验收） |
| I 卡（Arc） | ❌ | ✅ 且**不加** `--unlock-mfg` | 原生 MFG 有真模型，画质优于"每帧复用 2x 模型"（`MFG.md` §1 代价） |
| 只有一块卡（同卡双进程） | ❌ | 🟡 `--gpu` 指到生产端同一块卡 | 跨适配器堆仍可用（LUID 不同进程无影响），但 GPU 时间/显存会被两个进程抢；**不作为目标场景**，只保证不崩、日志明确 |

### 4.3 本机就能覆盖的四象限实测（不需要换硬件）

| # | 生产端 | 消费端 | 怎么摆 | 能验证什么 |
|---|---|---|---|---|
| 1 | 3080 Ti（N）+ DLSS | 6750 GRE（A） | 现状（游戏窗口在 A 卡显示器上、渲染在 N 卡，见 `ENV_REPORT.md` §4） | 基线 |
| 2 | 3080 Ti（N）+ **FSR** | 6750 GRE（A） | 同上，只把游戏里的超分从 DLSS 换成 FSR | `ffx` 源（副卡不动，只换输入源） |
| 3 | 3080 Ti（N）+ DLSS | **3080 Ti（N）** | 游戏窗口挪到 N 卡显示器，`--gpu 10de` 让 bridge 也落在 N 卡 | 消费端 N 卡路径（最便宜的 POC，不必等 A 卡当生产端） |
| 4 | 6750 GRE（A）+ FSR | 3080 Ti（N） | 游戏窗口留在 A 卡显示器，`--gpu 10de` 让 bridge 上 N 卡显示器 | N 卡副卡 + A 卡生产端（最完整的目标形态） |

> 两台显示器都在（主屏在 A 卡 5K、N 卡另接一台 4K，见 `ENV_REPORT.md` §4 与其顶部更正），所以上面四个组合
> 只靠"游戏窗口挪到哪块卡的显示器 + `--gpu` 选谁"就能覆盖，**不需要换硬件**。

`--gpu` 一旦落地，组合 3/4 就是纯命令行切换，可写进 `scripts/run_*.bat` 的 A/B 脚本。

---

## 5. 里程碑

| 里程碑 | 内容 | 主要改动 | 验收 | 风险 | 估时 |
|---|---|---|---|---|---|
| **M6.1** ✅ | bridge 泛化：副卡选择 + 能力自检 + 日志 | `bridge.cpp`（`find_consumer_adapter`、`--gpu`、`--producer-luid`、`--fg-probe` 增强、厂商分支） | 已在 3080 Ti 上实测：`--gpu 10de` 选中 N 卡、`CreateContext SUCCESS`、`--unlock-mfg` 1→3 | 低（不动协议） | 已完成（0.5 d） |
| **M6.2** ✅ | 协议 v6 | `src/mgpu_shared.h`、addon 写端、bridge 读端 | 合成生产者实测：v6 头被正确采纳（luid/api/flags/render/output）、v5 头被明确拒绝 | 中（两边必须同次重编，`HANDOFF.md` §2 的既有纪律） | 已完成（0.5 d） |
| **M6.3** ✅ | addon 输入源抽象 + `ngx` 源搬家 | `inputs.h`、`in_ngx.cpp`、`unity.cpp`、`game_capture.cpp`、`build_addon.{sh,bat}` | 编译 0 warning、导出 `AddonInit/AddonUninit`；行为等价（`g_inputs` 取代 `g_ngx_consumed`，选源后固定） | 中（重构，靠 A/B 日志对比；需要一次真机短跑复核） | 已完成（1 d） |
| **M6.4** ✅ | `ffx` 源（A 卡主线） | `in_ffx.cpp`（已写）、`third_party/ffx/api/`（ffx-api 头 v1.1.4）、`game_capture.cpp`（多资源状态表） | **已实测**：PBZ 切 FSR → `input source: ffx (api 2)`、`api=ffx flags=0x35`、bridge `camera=producer fov=46.7deg zn=10.00`、`xN=2.00`、无崩溃 | 高（踩到 2 个真 bug，见下） | 已完成（1.5 d） |
| **M6.5** ✅ | `xess` 源 | `in_xess.cpp`（`xessD3D12CreateContext/BuildPipelines/Init/Execute`，每模块一份 thunk） | **真机已通过**：`xess: first execute render=2562x1441 output=3840x2160 depth=0 mv=0x… reset=1 (HIGH_RES_MV: no depth, publishing MV only)` → `input source: xess (api 3)` → bridge `input geometry mv 2562x1441` + `SUCCESS`/`show=122`。注意 PBZ 用 `HIGH_RES_MV`，**只给速度不给深度** | 中 | 已完成 |
| **M6.6** 🟡 | 消费端 N 卡实跑 + 相机参数接入 | bridge 相机/flags 分支、`scripts/run_*.bat` | 组合 3 的能力已实测（`--gpu 10de`：`CreateContext SUCCESS`、`--unlock-mfg` 1→3）；**组合 4（A 卡渲染 + N 卡上屏）在本机无法验证**，见下方说明 | 中 | 待换机/新方案 |
| **M6.7**（可选） | FG 后端抽象（FSRFG / DLSSG） | `src/fg_backend/*`、FFX 重新编 `-DFFX_FI=ON` | — | 高（工程量另开一级） | 5 d+ |

**顺序理由**：M6.1 立刻解除"副卡必须 A 卡"的硬限制，且不动协议、不动 addon，是投入产出比最高的一步；
M6.2/M6.3 是后续一切的前提；M6.4 才是 A 卡生产端的主线（用户当前 A 卡是副卡，但 A 卡当主卡是常见形态）；
M6.6 用组合 3/4 收口验证；M6.7 与"n/a 兼容"目标无关，只是画质/性能备选，不排进本次范围。

### M6.1 已完成（2026-11-07）—— 实测记录

改动只动了 `src/bridge/bridge.cpp`（协议未变，addon 不用重编）：

| 改动 | 说明 |
|---|---|
| `find_amd_adapter()` → `find_consumer_adapter()` | 枚举全部适配器并逐条打日志；选择顺序 `--gpu` > 覆盖游戏窗口显示器的适配器 > 非生产端 > 显存最大 |
| `--gpu <spec>` | 序号 / 厂商别名 / 厂商 ID / `luid:HI:LO` / 描述子串 |
| `--producer-luid HI:LO` | 指定"游戏那块卡"，自动选另一块（协议 v6 后由 addon 自动填） |
| `apply_unlock_mfg()` | 从 `main` 的早期无条件调用改成"选完副卡之后调用"，**Intel 副卡直接跳过**；`--fg-probe` 下同时打印 stock 与 patched |
| `--fg-probe` | 先打印适配器表 + `adapter` 行（名字/厂商/device/LUID），再打印能力 |

实测（`<STAGE_DIR>\bridge\probe_*.txt`，本机 3080 Ti + 6750 GRE）：

| 命令 | 结果 |
|---|---|
| `--fg-probe` | 自动选 A 卡（largest VRAM）→ `CreateContext SUCCESS`，`maxSupportedInterpolations 1` |
| `--fg-probe --gpu 10de` | 选到 **3080 Ti**，`CreateContext SUCCESS`，`1 -> 2x`（**XeFG 在 N 卡上建上下文成功**） |
| `--fg-probe --gpu 10de --unlock-mfg --interp 3` | `stock 1` → 6 处补丁 OK → **`maxSupportedInterpolations 3 -> 4x`**，`VERDICT MFG available` |
| `--fg-probe --producer-luid 0:10392` | 避开创作者 → 选 3080 Ti（`not the producer`） |
| `--fg-probe --gpu intel` | 无 Intel 卡 → `WARNING ... matched no adapter` → 回退自动选择 |
| `--game explorer.exe`（10 s 后杀掉） | 游戏窗口在 A 卡显示器 → `drives the game window's monitor` → 选 A 卡，后续等待流程不变 |

**结论**：N 卡副卡的**能力**（建上下文 + MFG 解锁）已经在本机确认；还剩一次"带游戏的整链实跑"
（`run_mgpu_pbz_3x40_nvgpu.bat`，即组合 3）属于 M6.6 的验收项，不影响 M6.2 开工。

### M6.2 已完成（2026-11-07）—— 实测记录

| 改动 | 文件 | 说明 |
|---|---|---|
| 协议 v6 | `src/mgpu_shared.h` | `kSharedVersion=6`；`SharedControl` 尾部加 `producer`（`ProducerInfo`）；`FrameShared` 尾部加 `near/far/vfov/frame_time_ms`；输入源常量（`kInputApi*` / `kInputFlag*`）与 `pack_luid/unpack_luid` 放进共享头，两边只维护一份 |
| 写端 | `src/game_capture/game_capture.cpp` | `ensure_shared()` 写 `device_luid`（`ID3D12Device::GetAdapterLuid`）、`input_api=ngx`、`input_flags=0`、`render/output` 尺寸并打 `shared: producer luid=...`；`on_reshade_present()` 量 present 间隔写进 `frames[].frame_time_ms`，相机字段填 0（NGX 没有） |
| 读端 | `src/bridge/bridge.cpp` | `peek_producer()`（选卡前非阻塞读一次，自动"避开生产端"）；`open_shared()` 采纳 `producer`、按 `kInputFlagInvertedDepth` 自动置 init flag、同卡告警；`frame constants` 用真相机（有则用）、日志打 `camera=producer|synthetic` + `game_dt=`；**版本不符立即报错退出**（不再空等 120 s） |

**验证（合成生产者 `temp/fake_producer.cpp`，绕过游戏/GPU 只测协议头）**：

| 场景 | 结果 |
|---|---|
| 假 v6 头，`producer.luid=`A 卡 | `shared block present: producer pid=.. luid=00000000:00010392 api=ngx/dlss flags=0x0 render=2560x1440 output=5120x2880`；适配器表把该卡标成 `(producer)`；选卡=显示器那块 → `note: same GPU as the producer ...`；`open_shared` 再报一次同卡 WARNING；随后卡在 `placed resource FAILED`（假头没有真跨适配器堆，符合预期） |
| 假 v6 头，`producer.luid=`N 卡 | `(producer)` 标在 N 卡那行；选卡仍按显示器规则选 A 卡（硬约束优先），无同卡告警 |
| 假 **v5** 头 | `shared block version 5, this bridge speaks 6 - rebuild/redeploy BOTH sides (add-on + bridge) so they match` → 立即退出（码 5），**不会读歪、不会等 120 s** |

两边均 `cl` 编译 0 warning；`--fg-probe`（无共享块）行为不变。
**产物**：`build/bridge.exe`、`build/game_capture.addon64` 都已是 v6，**必须成对部署**。

### M6.4 已完成（2026-11-07）—— `ffx` 源 + 两个真 bug 的定位

**第一次真机（切 FSR）直接崩**，`ReShade.log` 只留两行 `Failed to close immediate command list with E_INVALIDARG`。
排查路径与结论（已修，均有复测）：

| # | 现象 | 根因 | 修法 |
|---|---|---|---|
| 1 | 切 FSR 后 ~13 s 游戏崩溃 | **单槽状态机**：我们只记"最后看到的那个资源 + 状态"。DLSS 每帧给同一个深度纹理，FSR 却**两个深度纹理交替**；指针一变，`input::track` 就把状态重置成猜的值，于是对"手里其实是别的状态"的资源发 transition ⇒ D3D12 `Close()` 返回 `E_INVALIDARG` ⇒ 崩 | `game_capture.cpp` 改成**固定 8 条的表**（`kTrackedResources`，每条 `handle/hint/state/seen`，无锁、只做线性比较）：`tracked_add/state/set_state`，`on_barrier` 按 handle 更新。**不是** map——当年用无界 map 把渲染线程饿死过 |
| 2 | 崩溃修好后仍 `flags=0x25`（没有相机） | FSR 的 create flags 是 `0x39`（含 `DEPTH_INVERTED|DEPTH_INFINITE`），这类反向 Z 游戏把两个距离**反着填**：`cameraNear=+inf/FLT_MAX`、`cameraFar=10`（真正的近平面）。我们的 `far>near` sanity 检查把它丢掉了 | `in_ffx.cpp` 在 `DEPTH_INVERTED` 且 `far<near` 时**先交换**再判断；bridge 里 `far` 非有限时强制用 reversed-Z 形式（否则有限形式会除 inf 得 NaN） |

验证（PBZ + FSR，2026-11-07 23:23）：

```
add-on: ffx: first frame: render=2880x1620 out=3840x2160 jitter=(-0.25, 0.17) mvscale=(2880, 1620)
                          camera=(near 3.4e38 far 10.0 vfov 1.2870)      ← 原始值确实是反的
        input source: ffx (api 2) - pinned for this process
        shared: producer luid=00000000:00012F2E api=ffx flags=0x35 render=2880x1620 output=3840x2160
bridge: shared block present: ... api=ffx/fsr flags=0x35 ...
        frame constants: jitter=(-0.44, 0.39) proj=reversed-Z approximation
                         camera=producer fov=46.7deg zn=10.00 zf=3.4e38 aspect=1.77778 (2880x1620)
        xN=2.00  xNmiss=0        （90 秒无崩溃、无 E_INVALIDARG）
```

`flags=0x35` = InvertedDepth | MvJittered | **HasCamera** | Hdr —— v6 的相机通路第一次吃到真数据；
`--fg-fov 58.7` 在 FSR 下已经不需要了（46.7° 是游戏自己报的）。

**性能另说**：这次是 4K 输出 + 游戏自身 ~20 fps（`game=50 ms`），`gpu 24–30 ms`、`skip 34–51`、
`load ~100%`。和 DLSS 那套一样，要干净数字得先按 `FINAL_CONFIG.md` §3/§9.15 锁帧对齐 vblank，
不是本次改动引入的回归（add-on 的 `record` 只有 0.02–0.05 ms）。

**顺带**：`[depth] typeless copy ... -> 0x0` 那条日志有误导（`-> %ux%u` 是"中间副本尺寸"，
走 in-place 时恒为 0），已改成 `same-frame convert in place: WxH fmt N, tracked state`。

---

### M6.4.1（同日，晚）—— 切换超分不再崩 + MV 请求通道 + UI 下拉

用户反馈：**在游戏内切换超分模式/档位会崩**；并且希望**把游戏的 MV 抓过来**，由 UI 选择方案。

| 改动 | 文件 | 说明 |
|---|---|---|
| 状态表按“年龄”淘汰 | `game_capture.cpp` | 每帧一次 tick；资源闲置 >2 帧就只信 hint（旧纹理可能已释放、地址可能被新分配复用）；表满时淘汰最久未用的。切换超分/档位会换掉深度/MV 纹理，旧实现会“没状态可用” |
| 不再猜状态 | `game_capture.cpp` | `record_depth_convert` 只在资源**确实在追踪表里**时才发 transition，否则跳过并记一次日志（`is not a watched resource - depth skipped`）。宁可丢一帧深度，也不发非法 barrier |
| 协议 v7 | `mgpu_shared.h` | `SharedControl.consumer_request`（消费端到生产端）+ `kRequestMv`；`kInputFlagMvActive` 表示 MV 流真的在产出 |
| MV 按请求产出 | `game_capture.cpp` | `SkipMv=0` 改成“跟随请求”（=1 仍是永不产出）；被请求时打 `[mv] the consumer asked for motion vectors - stream enabled` |
| bridge 侧 | `bridge.cpp` | 每帧写 `consumer_request`；`motionVectorScaleX/Y` 透传游戏自己的值（FSR 是像素单位 `(2880,1620)`），`--mv-flip` 取反 |
| UI | `mgpu_ui.cpp` | 「帧生成」卡片新增 **运动矢量** 下拉：关闭 / 游戏 MV / 游戏 MV(反向) → `--no-mv` / (无) / `--mv-flip`；ini 键 `mv_mode`（旧 `no_mv` 仍可读） |

**第二轮（模式切换仍崩）的真凶**——"切档位不崩、切模式崩"的原因：

| 切换 | 旧超分 | 旧逻辑 | 结果 |
|---|---|---|---|
| 档位 | 同一超分库继续被调用，只是尺寸变 | 每帧拿到**新**纹理 | 只触发尺寸告警 |
| **模式**（DLSS→FSR） | DLSS 不再被调用，纹理被**销毁** | `poll()` 只要"曾经有过"就永远 true（serial 冻结也不管）⇒ 反复用同一帧的旧纹理去拷/发 barrier | 非法 barrier / use-after-free → **崩** |

（第一轮只给 `record_depth_convert` 加了"不在表里就跳过"，另外三条拷贝路径拿到的仍是猜的状态，所以模式切换照样崩。）

| 第二轮改动 | 文件 | 说明 |
|---|---|---|
| `poll()` 只认新帧 | `in_ngx.cpp` / `in_ffx.cpp` | serial 没前进就返回 false（各自的 `g_last_serial` 闸门）。这是"不再碰死纹理"的关键 |
| 静默源自动释放 | `game_capture.cpp` | 当前源连续 90 帧没新帧 → `input source … went quiet … re-arming` → `g_source=nullptr`，注册表在**新** API 出数据时立刻接管 |
| `tracked_state` 三态 | `game_capture.cpp` | `0`=未知/过期（**不许碰**）、`1`=观测到的真实状态、`2`=本帧刚报告用 hint；超过 2 帧没被报告即算 0，四条拷贝路径统一按此处理 |
| 日志措辞 | `game_capture.cpp` | `(source: NGX render size)` → `(source: input render size)`（用 FSR 时不再误导） |

**验证**：
- 三个产物 0 warning；`SkipMv=0` + 不带 `--no-mv` 的 bridge →
  `[mv] the consumer asked for motion vectors - stream enabled` +
  `[mv] convert pass … tracked state = 0x8C0`，`xN=2.00` 不变；带 `--no-mv` 时 MV pass 完全不跑。
- 现场日志出现过 `input source ffx went quiet (no new frame for 90 frames) - re-arming` 后
  **自动重新接管**，游戏没崩。
- **2026-11-07 深夜：用户实测「来回切换超分模式多次，切换没有问题」**（此前必崩）。

> 仍存在的**非崩溃**限制：切模式/档位会改渲染分辨率，而共享块流尺寸**每进程只建一次**，
> 切完 bridge 会打 `render size is now AxB but the shared block was built for CxD` 并跳过拷贝
> —— 想让"切完不重启就能用"，要做**共享块重建 + bridge 重连**（下一步）。


---

### M6.5（同日，夜）—— `xess` 源 + UI 的 GPU 选择

| 改动 | 文件 | 说明 |
|---|---|---|
| xess 源 | `src/game_capture/in_xess.cpp`（新） | 钩 `xessD3D12CreateContext/BuildPipelines/Init/Execute`（每模块一份 thunk，模块 PIN 住）；字段：`pDepthTexture/pVelocityTexture/jitterOffsetX,Y/inputWidth,Height/resetHistory`；`xessD3D12Init` 的 `initFlags` 映射 `InvertedDepth/HiResMv/MvNdc/MvJittered/Hdr`；**无相机** ⇒ 不置 `kFlagHasCamera`；`USE_NDC_VELOCITY` 时 MV scale = `(w*0.5, -h*0.5)` |
| 接线 | `unity.cpp`、`inputs.h`、`game_capture.cpp`、`build_addon.bat` | `make_xess_source()`、`-I third_party/xess/inc`；`Input=auto` 时 ngx → ffx → xess 谁先给数据用谁 |
| UI 选 GPU | `src/mgpu_ui/mgpu_ui.cpp` | 「GPU / 显示器」卡片的只读行换成 **帧生成用的 GPU** 下拉：`自动` + DXGI 实时枚举的 `#i 名字`（→ `--gpu <i>`，ini 键 `gpu_sel`）。这就是"A 卡渲染 + N 卡上屏"的入口 |

> **实测结果（2026-11-08 03:33，PBZ 4K）**：
> ```
> xess: libxess.dll hooked (slot 0)
> xess: init flags=0x109 output=3840x2160 quality=102        ← HIGH_RES_MV|RESPONSIVE_PIXEL_MASK|AUTOEXPOSURE
> xess: first execute render=2562x1441 output=3840x2160 depth=0000000000000000 mv=0x… reset=1
>       (HIGH_RES_MV: no depth, publishing MV only)
> input source: xess (api 3) - pinned for this process
> [depth] copy failed - publishing with motion vectors only   （只一次，预期）
> bridge: input geometry mv 2562x1441, depth 2562x1441 / 45×SUCCESS / show=122 / gpu 13.3ms / 0 错误
> ```
> **注意**：PBZ 的 XeSS 用 `HIGH_RES_MV`，深度按上游头文件是**可选**的（它传 NULL），所以 XeSS 档
> 只有速度流可用 —— 而深度才是 XeFG 真正消费的那一路，因此 XeSS 对 XeFG 的价值天然低于
> DLSS/FSR 源。三个源同时存在时按"最近 250 ms 内真的被调用过"挑选（v10 起加了新鲜度判据）。

### 回归测试矩阵（2026-11-08 凌晨，PBZ 4K / 120Hz / 3x，全部真机）

| 用例 | 动作 | 结果 |
|---|---|---|
| T1 基线 | 不切任何东西 3 分钟 | 40fps 输入 / 消费端 `gpu` 8.5ms / 输出 119fps / 0 错误 / `skip` 0.25 每窗口 |
| T2 档位切换 | DLSS 内切 3 次（1920x1080 / 2228x1253 / 2562x1441） | gen **3/3 配对**，0 错误；暴露 `ov` 每次切后 +60~94（见 M6.4.4 第二个坑，已修） |
| T3 模式切换 | DLSS→FSR→XeSS→DLSS，含 TSR 档 | **XeSS 源打通**（见 M6.5）；TSR 优雅降级（95% 帧无输入、画面正常）；0 错误 |
| T4 小→大 | 超级性能(1279x720) 启动 → 质量(2562x1441) → 回小 | 面积 ×4 **无需重建、不越界**，gen 2/3 配对，0 错误，`show` 118–125 |
| T5 快速连切 | 20 秒内 5 次尺寸 + 2 次源切换 | gen **5/5 配对**，10 次 `HITCH`（游戏重编 pipeline），**`ov` 仅 +4**，0 错误 |
| T6 无消费者 | 只开游戏不开桥 | `credit window OFF`，`ov` 冻结（不再永久丢帧） |
| T7 MV A/B | 同 cmdline 只翻 `--no-mv` | 开 MV：消费端 **+9.7 ms** / 输出 **−6 fps** / `skip` ×7 / **画面无变化** ⇒ 保持关闭 |
| T8 UI 选 GPU（组合 4） | UI 下拉选 N 卡 + 强制 A 卡渲染 + 窗口放在 N 卡屏 | ❌ **本机无法验证**（驱动/环境限制，见下） |

#### `SlotPolicy=1`（等槽）实测：无效，保持默认 0（2026-11-08，PBZ）

配置：`[capture] SlotPolicy=1`、`SlotWaitUs=2000`，正常走动/战斗 30 秒。

```
[04:03:24] stats: seen=3601 … q=0/4 ov=116 waits=116  avg record=0.027 ms
[04:03:39] stats: seen=4201 … q=4/4 ov=233 waits=233  avg record=0.027 ms   ← 桥已停
```

- **`waits` 与 `ov` 完全相等** ⇒ 每一次等待最后仍然丢帧，2 ms 的超时一次都没救回来。
- 原因：消费端 ACK 按它自己的帧节拍回来（~25 ms 一次），而 `SlotWaitUs=2000` 比它小一个数量级；
  真要等到，就得等满一个输入间隔（25 ms），那会把游戏渲染线程卡住 25 ms，不可接受。
- 这两个计数**全部来自两次瞬态**：① 桥还没连上时的前 ~116 帧；② 桥刚停后的 ~117 帧。
  稳态（桥正常运行时）`waits=0` —— 正常负载下该策略是死代码。
- 顺带优化：ACK 宽限期 `kAckGraceFrames` 从 **120 帧 → 30 帧**（这两个瞬态的无谓发布从 ~116 降到 ~30）。

**结论**：`SlotPolicy` 默认 **0（丢帧）**；`1` 只作为极端场景的手工开关保留，不要日常开。

#### T8 为什么在本机测不了（2026-11-08，CP2077）

- 配置：在注册表 `HKCU/Software/Microsoft/DirectX/UserGpuPreferences` 里把 `Cyberpunk2077.exe` 钉到
  `SpecificAdapter=1002&73DF&67301EAE`（6750 GRE），游戏窗口放在 3080 Ti 驱动的显示器上，UI 里选 N 卡。
- 现象：游戏能起、upscaler 也能初始化（`xess: init flags=0x1E output=1920x1080 quality=103`），
  但 **0.3 秒内连续 4 次 `init_swapchain … resize=1`** 之后不再出帧，随后弹初始化错误框并退出；
  Windows 应用日志**没有任何崩溃记录**（不是崩溃，是主动退出）。
- 已排除：**不是光追/驱动不支持** —— `UserSettings.json` 里 `RayTracing=false`、`DLSS=DLAA`、
  `FrameGeneration=Off`；6750 GRE 是 RDNA2（DXR 1.1），驱动 `32.0.21045.16010`（2026-10）。
- 判断：更像"D3D 设备在 A 卡、窗口挂在 N 卡屏"这种**跨卡呈现**在 CP2077 上不成立（swapchain 无法
  建立稳定的 present 路径）。**按约定本机记为"驱动/环境限制，暂不可测"**，并已撤销该 per-app GPU 偏好
  （恢复自动选卡，CP2077 回到正常可启动状态）。
- 要真正验证组合 4，需要 bridge 支持 `--overlay-monitor N`：overlay 放到另一块屏、游戏窗口留在
  自己那块卡上正常呈现。该功能尚未实现。

**关键含义**：v9 的"槽位按容量一次性放置"让 T4（面积 ×4）成立；v10 的 ACK 信用窗口让 T5
（快速连切）不崩；seqlock 让每次尺寸变化的两条流尺寸严格成对；XeSS 的新鲜度判据让模式切换
时能选中真正活着的源。

---

### M6.4.4（次日凌晨）—— **消息队列语义**（协议 v10）：ACK 信用窗口 + seqlock + jbuf

v9 把"谁都不等谁"做到了，但留下了三个洞：**槽位复用靠运气**、**元数据非原子**、**队列水位不可见**。
v10 全部补上（实现按 P1/P2/P3，代价几乎为零）：

| 洞 | v9 的状况 | v10 的做法 |
|---|---|---|
| 槽位复用 | `fence_back[]` 建了、句柄也发了，**两头都没用**；生产端无条件 `g_slot_index = (i+1)%4` | 消费端每帧把"读完的发布序号"写进 `consumed[slot]`；生产端 `pick_free_slot()` 只挑已 ACK 的槽，找不到就按 `[capture] SlotPolicy` 丢帧/`SlotWaitUs` 等槽 |
| 队列水位 | 完全不可知 | add-on 统计行新增 `q=已发布未ACK/槽数 ov=丢弃次数 waits=等待次数`；`ov` 就是"被消费端卡住"的硬证据 |
| 元数据原子性 | `streams[i].width/height` 两个整数可能读到一半旧一半新，靠 clamp 兜 | `StreamShared.seq` seqlock（奇=正在写），消费端 `read_stream_geo()` 重试到一致 |
| 抖动缓冲 | `--slack N` 扣 **WGC 帧**，实测把 `gpu` 抬了 +5.7 ms | `--jbuf N`：彩色**立刻拷进自己的纹理**、WGC 帧马上释放，只延迟"处理" |

协议字段（`mgpu_shared.h`，`kSharedVersion = 10`）：
`SharedControl.consumed[4]` / `producer_overrun` / `producer_waits` / `slot_policy`、
`FrameShared.published`（该槽最近一次发布的 fence 值，消费端据此回 ACK —— 跨卡 fence 在消费端读不出来，HANDOFF 8.6）、
`StreamShared.seq`（seqlock）。

**实测（PBZ 4K，`--no-mv`，120 Hz，3x）**

```
无消费者   : shared: credit window OFF (falling back to round-robin) - consumer has been silent for 120 frames
             q=4/4 ov=115        ← 冻结在 115，不再增长
有消费者   : shared: credit window ON - consumer is ACKing
             q=3/4 ov=118        ← 55 秒只丢 3 次（0.05%）
bridge     : 64× SUCCESS, 0 error, gpu 10.6 ms, show 120
```

> **第二个坑（已修）**：ACK 粒度太细。第一版 bridge 每帧只 ACK **它取走的那个槽**，
> 于是队列长期停在 `q=3/4`（只剩 1 个空槽）；切换档位瞬间 bridge 有一次 `HITCH gap≈560ms`
> （游戏重编 DLSS pipeline），两边的槽计数错开一格就没地方吸收了 —— 实测**每次切换后
> 10 秒内丢 25~65% 的发布**（T2：`+94/240 = 39%`、`+62/120 = 52%`），之后自行对齐才归零。
> 改成"**除正在读的槽外全部 ACK**"（消费端每帧只读一个槽，其余槽它永远不碰，GPU 侧也没引用）：
> `q` 从 `3/4` 掉到 `0/4`，`ov` 完全冻结（实测 `ov=2627` 跨 6 个窗口 0 增长）。
> 保护仍在：真正在用的那个槽要等 `gpu_throttle()` 确认上一帧 GPU 完成后才 ACK，
> 且**即将读取的槽永远不 ACK**（避免与自己的拷贝命令抢）。

> **XeSS 真机首测（T3，2026-11-08 03:17）**：钩子与 init 都中了
> （`xess: libxess.dll hooked (slot 0)`；`xess: init flags=0x109 output=3840x2160 quality=102`，
> 即 `HIGH_RES_MV | RESPONSIVE_PIXEL_MASK | ENABLE_AUTOEXPOSURE`，无 INVERTED_DEPTH / 无 NDC 速度），
> 但**没被 pin 成输入源** —— 切到 XeSS 时旧源(ffx)要静默 90 帧才 re-arm，re-arm 那一刻
> NGX 回放了它的**最后一帧**，于是又 pin 回 ngx，之后整段等于"没有输入源"。
> 修法：`FrameInputs.tick_ms`（钩子被调用的时刻），pin 之前要求
> `now - tick_ms < 250 ms` —— 只有**真的还在被调用**的源才允许被 pin。
>
> 顺带修掉一个未爆的 bug：PBZ 的 XeSS 用 `HIGH_RES_MV`（速度纹理在**输出**分辨率、
> 深度在**渲染**分辨率），原来把 `render_w/h` 按输出分辨率上报会让两条流尺寸不一致
> （XeFG 必然拒绝 tag）；现在内容尺寸始终取 `inputWidth/Height`，速度流由 downsample
> pass 缩放（pass 是 SRV 采样，尺寸不需要匹配）。
>
> **关键修正（2026-11-08 03:30）**：PBZ 的 XeSS **本来就不传深度**。上游头文件写得很清楚：
> `pDepthTexture` 是 *Optional depth texture. Required if XESS_INIT_FLAG_HIGH_RES_MV has not
> been specified* —— PBZ 设了 `HIGH_RES_MV`，于是只给 `pVelocityTexture`、深度传 NULL。
> 我们原来那句 `if (!depth) return;` 把这些**合法帧全丢了**（日志实测：22 秒 600 次 execute，
> 全部被丢，于是整个 XeSS 档位"没有输入源"）。现在改成：只要 `mv` 或 `depth` 有一个就发帧，
> 深度缺失时**只发 MV**（宿主侧 `track()/hold()` 都是空安全，depth 流会打一次
> `[depth] copy failed - publishing with motion vectors only`）。
> **代价/限制**：XeSS(HIGH_RES_MV) 只提供速度、不提供深度，而深度才是 XeFG 真正吃的那一路，
> 所以 XeSS 档对 XeFG 的价值天然低于 DLSS/FSR 源。

> **TSR（你顺手跑的）**：82 秒 / 3120 帧里只 published **149** 帧 → TSR 不是
> NGX/FFX/XeSS API，三个源都抓不到；表现是**优雅降级**（画面正常、`xN=3.00` 不变，
> 只有 MV/深度停更）。已加提示：无输入源 15 秒后打印一次"TSR/FSR1/原生 TAA 抓不到输入"。

> **踩坑（已修，值得记住）**：第一版判断"消费者还在不在"用的是 **`any_ack == 0`**，
> 但 `consumed[]` 在 bridge 退出后**保留旧值**，于是桥一关就永远不满足 ⇒ 生产端永久丢帧
> （实测 **39% 的发布被丢**，`q` 卡在 3/4，游戏画面靠旧槽位"续命"）。
> 改成**看 ACK 是否还在推进**（`update_credit_state()`：连续 120 帧 ACK 和不变 = 消费者已走）。

**`--jbuf 1` 实测（同一 cmdline，只加 `--jbuf 1`）**

| 指标 | jbuf 0 | jbuf 1 | 结论 |
|---|---|---|---|
| `gpu` 中位 | 10.6 | 15.9 | +5.3（比 `--slack` 的 +5.7 略好，但仍不为零） |
| `show` 中位 | 120 | 117 | −3 |
| `skip` | 0.57/窗口 | 0.42/窗口 | −26% |
| **click→photons** | **48.4 ms** | **65.1 ms** | **+17 ms** |
| add-on `ov` | +3/55 s | **+139/55 s** | 消费端变慢 ⇒ 信用窗口如实报出丢弃 |

⇒ 结论和 §9.9 一致：**抖动缓冲在这里不划算**。区别是现在有 `ov` 这个硬指标能直接证明"是消费端在拖"，
不用再靠猜。默认 `--jbuf 0`。

---

### M6.4.3（同日，深夜）—— **解耦**（协议 v9）：add-on 只写缓冲，bridge 只读缓冲

要求（用户原话）：*addon 只负责抓取数据存放到指定位置，bridge 只负责捕获信息执行 fg*。
v8 虽然能做到"切档位不重启"，但两端还互相牵制，v9 把这些全部拆掉：

| v8 的耦合 | v9 的做法 |
|---|---|
| 尺寸变了 ⇒ add-on **重放槽位**（`CreatePlacedResource`），期间 `magic=0` 让 bridge 暂停 | 槽位按 `kStreamMaxW/H` **一次性放好，永不移动**；尺寸变化只改 `streams[i].width/height` + `generation += 1` |
| add-on 等 bridge 回 `consumer_ack`（≤200 ms，阻塞渲染线程） | **没有 ack 字段了**（`_reserved_ack`），没有任何等待 |
| add-on 为重建排空自己的队列（`wait_game_queue_idle`） | 连这个也去掉了：MV/深度 pass 的目标纹理也按容量分配，只改 viewport/scissor ⇒ 渲染线程**零停顿** |
| bridge 尺寸变了要丢视图 + 重新放（还可能要失败暂停） | 视图**不动**，只按需重建**自己的本地副本**；旧副本进 retire 队列，16 帧后才释放 |
| `consumer_request` 决定 add-on 产不产 MV（双向） | 保留成一维**提示**：add-on 读一次布尔，永不阻塞；bridge 也可以完全忽略 add-on 送来的东西 |

拷贝语义：槽位是 3840×2160 的固定缓冲，活的内容在左上角 `width × height` 矩形里。
add-on 用 `CopyTextureRegion` 只写这个矩形（`copy_into_slot()`），bridge 只用
`CopyTextureRegion` 读这个矩形到本地纹理（`copy_content()`）—— 双方都用 header 里的
两个整数，行距来自槽位容量（所以 bridge 必须按 `slot_w/slot_h` 放置，否则读歪）。

| 改动 | 文件 |
|---|---|
| `StreamShared` 增 `slot_w/slot_h`；`consumer_ack` → `_reserved_ack`；`kSharedVersion=9` | `src/mgpu_shared.h` |
| 槽位/通道按容量布局与放置；`rebuild_streams()` → `update_content_size()`（纯元数据） | `src/game_capture/shared_layout.h` |
| `copy_into_slot()` 区域拷贝；pass 目标纹理按容量分配 | `src/game_capture/game_capture.cpp` |
| `ensure_local_copy(dst, src, w, h)` + `copy_content()`；`release_local_inputs()` + retire 队列；`service_shared_layout()` 不再放视图 | `src/bridge/bridge.cpp` |

**实测（PBZ，4K，`TestSize=240` 每 240 帧切换一次内容尺寸）**：

```
addon : downsample: MV/depth pass targets are 3840x2160 (capacity), drawing 2562x1441
addon : shared ready: used=253.6 MB reserved=253.6 MB slots=4 gen=1
addon : shared: content size is now 2304x1296 (gen 2) - slots stay 3840x2160, nothing rebuilt
bridge: producer published gen 9: mv 2562x1441, depth 2562x1441 (slots stay 3840x2160)
bridge: input geometry: mv 2304x1296, depth 2304x1296
bridge: frame 3201 ... presented=3 fg=1 SUCCESS  show=118      (13 次尺寸切换, 0 失败)
```

> 踩坑（已修）：bridge 在尺寸变化时**立刻**释放本地副本 —— XeFG 还持有上一帧 tag 的
> 裸指针，于是消费者设备被移除（`local input copy FAILED hr=0x887A0005`，`xefg error:
> Resource pointer is null`）。v9 用 retire 队列（16 帧）解决，不需要 fence 握手。

---

### M6.4.2（同日，夜）—— 布局重建（协议 v8，**已被 M6.4.3 取代**）：切档位/模式不用重启

需求：切完超分模式/档位后，共享块的流尺寸还是第一个尺寸，bridge 会打
`render size is now AxB but the shared block was built for CxD` 并跳过拷贝 —— 只能重启游戏。

设计（关键：**不重建映射**，否则消费者手里的句柄会指向旧对象）：

| 端 | 做法 |
|---|---|
| 映射 | 一次建到**最大布局**（`kStreamMaxW/H = 3840×2160` 的 MV+depth × 4 槽 ≈ 254 MB **保留**，按页惰性提交，实际只用到当前的 ~115 MB） |
| add-on | `maybe_rebuild_layout()` 每帧比对目标尺寸（`stream_target_size()`，与首建同一套规则）；变了就 `rebuild_streams()`：`magic=0` → `generation++` → 旧槽位**先退休不销毁**（消费者可能还有一帧在飞，`release_old_resources()` 8 帧后释放）→ 同堆重新 `CreatePlacedResource` → `ds_prepare()` 按新尺寸重建本地 pass → `magic=magic` 最后写 |
| bridge | `service_shared_layout()`：`magic != magic` ⇒ 丢视图、本帧跳过；`generation` 变了 ⇒ `release_shared_views()` + `place_shared_views()`（同一个堆/fence）→ 日志 `shared layout rebound (gen N): MV AxB, depth AxB` |
| 协议 | `kSharedVersion = 8`，`SharedControl.generation`；`mapping_size` 现在记的是**保留大小**，日志会同时打 used/reserved |

验证：
- 冷启动：add-on `shared ready: … used=114.6 MB reserved=253.6 MB slots=4 gen=1`；
  bridge `MV/depth placed, fences duplicated` → `xN=2.00` 正常。
- **第一次真机切档位（FSR 质量→平衡）直接崩**（add-on 最后一行是 `rebuilt at 1920x1080, gen 2`，
  随后进程消失；这次 `ReShade.log` 没有 `E_INVALIDARG`，说明是 **GPU 侧**问题）。三个 bug：
  1. **没等 GPU 排空**：重建时直接 `Reset()` 旧 placed 资源、并且 `ds_prepare()` 重建 pass 目标，
     而上一帧记录在**游戏队列**上的拷贝/绘制可能还在执行 → GPU use-after-free。
  2. **没等 bridge 放掉它的视图**：两块卡可能同时碰同一段堆内存（资源别名）。
  3. **fence 计数被清零**（生产端 `g_fwd_value=0`、消费端 `g_fwd_last=0`）：fence 对象是同一个、
     消费者自己维护期望值，清零后它会去读"没人写过"的槽。
  修法：生产端 `generation++` → `magic=0` → **等 `consumer_ack`（≤200 ms）** →
  **等游戏队列排空**（自建 fence + `Signal`/`WaitForSingleObject`，≤200 ms）→ 销毁旧槽位并重放 →
  `magic` 最后写；消费端丢视图后写 `consumer_ack = generation`（打开时也写一次）；
  **两边都不再清零 fence 计数**（保持单调）。
- 自测钩子：`[capture] TestRebuild=N` 每 N 帧强制一次重建（真实尺寸 ↔ 90%），
  不用改游戏设置就能反复压这条路径；日志 `shared: TEST shrink|restore AxB -> CxD`。

**2026-11-08 00:39 用户实测通过**：一局里 **DLSS 质量 → DLSS 平衡 → FSR 质量 → FSR 平衡**
四次真实切换 + 强制重建，共 **16 代**（`rebuilt at … gen 2..16`，尺寸 2562×1441 / 2304×1296 /
2228×1253 / 1920×1080 / 1728×972 之间来回），全程无崩溃，日志以 `AddonUninit`（正常退出）结束，
`seen=2722 copied=1788 skipped=0`。**切超分模式/档位不再需要重启游戏**（`TestRebuild` 已从部署 ini 移除）。

> 上限：超过 3840×2160 的渲染尺寸（例如 5K 原生/DLAA@5K）不重建，保留旧布局由 pass 重采样，
> 只打一次 `is above the … reserve` 警告。要支持更大就调 `kStreamMaxW/H`（保留内存随之增加）。

---

### M6.4 实现细节（同日）—— ffx 源接线

| 改动 | 文件 | 说明 |
|---|---|---|
| ffx-api 头（vendored） | `third_party/ffx/api/`（新） | 从 FidelityFX-SDK **v1.1.4** 的 `ffx-api/include/ffx_api/` 取 5 个头（`ffx_api.h`/`ffx_api_types.h`/`ffx_upscale.h`/`ffx_framegeneration.h`/`dx12/ffx_api_dx12.h`）+ MIT `LICENSE.txt` + `SOURCE.txt`；仓库原来的 `third_party/ffx/include` 是**经典 SDK**，没有 ffx-api 结构体 |
| ffx 源 | `src/game_capture/in_ffx.cpp`（新） | 钩 5 个 ffx-api 导出（`ffxCreateContext/ffxDispatch/ffxQuery/ffxConfigure/ffxDestroyContext`）；读 `ffxDispatchDescUpscale` 的 `depth/motionVectors/jitterOffset/motionVectorScale/renderSize/upscaleSize/frameTimeDelta/cameraNear/Far/FovAngleVertical`；create 的 flags 决定 `kFlagInvertedDepth / kFlagHiResMv / kFlagMvJittered / kFlagHdr`，相机有效时置 `kFlagHasCamera`；看到 `ffxDispatchDescFrameGeneration` 说明游戏自带 FG，打一次告警 |
| 构建 | `src/game_capture/build_addon.bat` | 加 `/I"%REPO%\third_party\ffx\api\include"` |
| 接线 | `unity.cpp`、`game_capture.cpp`、`inputs.h` | `#include "in_ffx.cpp"`、`make_ffx_source()`、`-I`；`Input=auto` 时 ngx 与 ffx 谁先给数据用谁 |

**真机发现（PBZ，2026-11-07 23:02）**：

- PBZ 自带 FSR 插件：`ProjectPBZ/Plugins/FSR/Source/fidelityfx-sdk/Kits/FidelityFX/signedbin/{amd_fidelityfx_loader_dx12,amd_fidelityfx_upscaler_dx12,amd_fidelityfx_framegeneration_dx12}.dll`，
  另有 `Plugins/XeSS/Binaries` —— 三个超分它都带。
- 启动时**先加载 loader、再由 loader 加载 provider**，两个都被加载 ⇒ 只钩一个不可靠。
  已改成**每个模块一份 thunk**（`kThunks[N]`，`g_slots[N]` 保存各自的 original），
  最多 4 个模块，日志 `ffx: ... hooked (ffx-api, slot N)`；模块被 `GetModuleHandleExW(PIN)` 钉住，避免卸载后跳到野地址。
- 当前游戏仍是 DLSS ⇒ `input source: ngx (api 1)`；要测 ffx 必须**在游戏内把超分改成 FSR**（并关掉游戏内帧生成），
  然后**重启游戏**——输入源在本进程内固定，换源要新进程。

---

### M6.3 已完成（2026-11-07）—— 输入源抽象（ngx 搬家）

| 改动 | 文件 | 说明 |
|---|---|---|
| 输入源接口 | `src/game_capture/inputs.h`（新） | `FrameInputs`（mv/depth/render/output/jitter/mv_scale/near/far/vfov/flags/serial）、`Source`（name/api_id/on_module/scan_existing/poll）、宿主服务 `logf/ensure_minhook/track/hold`、工厂 `make_ngx_source()` |
| NGX 源搬家 | `src/game_capture/in_ngx.cpp`（新） | 原 `game_capture.cpp` 里的 NGX 钩子、`_nvngx.dll` 路径判定、参数读取、模块扫描全部搬进来，行为不变；`hook_evaluate` 里改成 `input::track(...)` + `input::hold(...)` |
| 宿主瘦身 | `src/game_capture/game_capture.cpp` | 删掉 NGX 专有代码；新增 `g_sources / g_source / g_inputs / g_input_api`、`fetch_inputs()`（选源后固定）、`notify_sources()`、`scan_loaded_modules()`；`g_ngx_consumed/g_ngx_reported` → `g_inputs`；`ensure_shared` 的 `ProducerInfo` 用 `g_source->name()` 与 `g_inputs.flags`；新增 `[capture] Input=`（auto/ngx/ffx/xess/depth） |
| Unity 编译 | `unity.cpp`（新）+ `scripts/build_addon.sh` | `build_addon.bat` 只能编一个 TU；`scripts/build_addon.sh` 现在**优先编译 `unity.cpp`** 并把 `*.h` 一起 stage；bat 增加 `/I"%~dp0."`（staging 目录，注意结尾不能是裸反斜杠，否则 cl 会把引号吃掉） |
| 文档/包 | `FINAL_CONFIG.md` §2.1.2、`packaging/reshade_pack/game_capture.ini` | 新增 `Input=auto` 说明 |

**验证**：

| 项 | 结果 |
|---|---|
| 编译 | `unity.cpp` 0 warning / 0 error（含 MinHook 的 4 个 .c） |
| 导出表 | `scripts/pe.py build/game_capture.addon64` → `exports: 2`（`AddonInit`/`AddonUninit`） |
| 行为等价 | `g_inputs` 逐字段对应原 `g_ngx_consumed`；`ensure_shared` 仍读"本帧刚取到"的值（原来是 `g_ngx_reported`，同样是即时值）；barrier 状态机（`g_mv_ptr/g_*_state`）逻辑未变，只是初值改为由源通过 `input::track()` 提供 |

> **真机短跑已完成（2026-11-07 22:47，PBZ 4K）**，M6.2 + M6.3 的现场证据：
>
> ```
> add-on: [22:47:12] input source: ngx (api 1) - pinned for this process
>         [22:47:13] shared: producer luid=00000000:00012F2E api=ngx flags=0x0 render=2562x1441 (source: NGX render size) output=3840x2160
>         [22:47:13] shared ready: ... size=114.6 MB slots=4      （2642 帧、copied 1882、record 0.03 ms）
> bridge: shared block present: producer pid=20848 luid=00000000:00012F2E api=ngx/dlss flags=0x0 render=2562x1441 output=3840x2160
>         consumer adapter: 'AMD Radeon RX 6750 GRE 12GB' - drives the game window's monitor
>         InitFromSwapChainDesc: SUCCESS / xN=2.00 / xNmiss=0 / skip=0（57 个统计窗口）
>         frame constants: ... camera=synthetic (--fg-fov 58.7) aspect=1.77793 (2562x1441) game_dt=22.36ms
>         note: producer frame time 20.37 ms vs our capture cadence 28.18 ms ...   ← v6 交叉校验
> ```
>
> `--interp 3 --unlock-mfg` 同场也验过：`all 6 sites patched` → `maxSupportedInterpolations = 3` → `xN=4.00`；
> 但**没有 RTSS 锁 30**，输入 ~30fps 撑 4x → `skip 13–19 / load ~100%`（与 `MFG.md` §3.1 的结论一致，不是回归）。
>
> 负向验证：`--gpu 10de`（消费端=生产端那块 N 卡）→ 两条新告警都打出来了
> （`does not drive the game window's monitor` + `producer and consumer are the SAME GPU`），
> **交换链能建、帧也能出**（`xN=2.00`），性能差（`game=104 ms / gpu=61 ms / slow=10`）
> 是**传输/显示路径**造成的——不是 bridge 的兼容性问题，是使用策略问题。
> 正确的摆法是：**游戏在 A 卡渲染（FSR），bridge 在 N 卡上屏到 N 卡驱动的显示器**
> （组合 4）；这需要 M6.4 的 `ffx` 输入源，见下面 M6.4。


---

## 6. 关键实现细节（照着写就不会跑偏）

### 6.1 `ffx` 源：钩什么、取什么

钩子目标是 **FFX 的统一入口五个导出**（`ffxCreateContext/ffxDestroyContext/ffxConfigure/ffxQuery/ffxDispatch`），
按 DLL 名路由（`DllNames.h` 的名字表）：

| DLL | 说明 |
|---|---|
| `amd_fidelityfx_upscaler_dx12.dll` | FSR 3.1 upscaler（优先） |
| `amd_fidelityfx_dx12.dll` | FSR 2.2 / 3.0 + 3.1 的合并入口 |
| `amd_fidelityfx_loader_dx12.dll` | loader 间接转发，命中后同样挂 |
| `ffx_fsr2_api_dx12_x64.dll` | 老式 FSR2 后端（导出 `ffxFsr2ContextDispatch`，字段不同，单独一条） |

**前置依赖（必须先补）**：钩运行期 DLL 要的是 **ffx-api** 结构体
（`ffxDispatchDescUpscale` / `ffxCreateContextDescUpscale` / `ffxQueryDescHeader`…），
这些头文件在 AMD `FidelityFX-SDK` 的 `sdk/include/FidelityFX/host/ffx_api*.h` 与 `dx12/ffx_api_dx12.h`。
我们仓库现有的 `third_party/ffx/include/FidelityFX/host/` 是**经典 SDK**（`ffx_fsr3upscaler.h`
里的 `FfxFsr3UpscalerDispatchDescription`），**没有 `ffx_api*`**（`grep -r ffx_api third_party/ffx` 为空）。
⇒ M6.4 的第一步是补这 4–5 个头文件（AMD 许可，与 `docs/ops/DEPENDENCIES.md` 的记法一致），
否则只能手搓结构体偏移，版本一变就崩。

字段映射（两套名字同时列出，避免写错）：

| FrameInputs | ffx-api（钩 DLL 用） | 经典 SDK（对照） |
|---|---|---|
| `depth` | `ffxDispatchDescUpscale.depth.resource` + `.depth.width/height` | `FfxFsr3UpscalerDispatchDescription.depth` |
| `mv` | `.motionVectors.resource` | `.motionVectors` |
| `jitter_x/y` | `.jitterOffset.x/y` | `.jitterOffset` |
| `mv_scale_x/y` | `.motionVectorScale.x/y` | `.motionVectorScale` |
| `render_w/h` | `.renderSize.width/height` | `.renderSize` |
| `output_w/h` | `.upscaleSize.width/height` | `.upscaleSize` |
| `frame_time_ms` | `.frameTimeDelta` | `.frameTimeDelta` |
| `near/far/vfov` | `.cameraNear / .cameraFar / .cameraFovAngleVertical`（radians） | 同名 |
| `flags` | create desc 的 `FFX_UPSCALER_FLAGS_DEPTH_INVERTED` ⇒ `kFlagInvertedDepth`；`..._DISPLAY_RESOLUTION_MOTION_VECTORS` ⇒ `kFlagHiResMv`；`..._MOTION_VECTORS_JITTER_CANCELLATION` ⇒ 不置 `kFlagMvJittered` | `FfxFsr3UpscalerInitializationFlagBits`：`ENABLE_DEPTH_INVERTED / ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS / ENABLE_MOTION_VECTORS_JITTER_CANCELLATION` |

三条实现纪律：

1. **只读不拦**：钩子里读 desc、记指针与参数，然后无条件调用原函数。**不要**像 OptiScaler 那样接管执行。
2. **区分 upscale 与 FG desc**：同一个 `ffxDispatch` 也可能收到 `ffxDispatchDescFrameGeneration`
   （游戏自带 FSR3 FG）。看到就记一条日志并**不接管**——否则我们会做出三层插帧。
3. **状态未知就别猜**：FFX 的 desc 不带资源的 `D3D12_RESOURCE_STATES`，所以 `state_hint()` 给
   `NON_PIXEL_SHADER_RESOURCE`（depth 用 `DEPTH_STENCIL_READ`），真正的状态由现有
   `addon_event::barrier` 追踪器（`game_capture.cpp:1181`）修正——它本来就以"观察到的最后一次 barrier"为准。

### 6.2 `xess` 源

钩 `libxess.dll` 的 `xessD3D12CreateContext / xessD3D12Init / xessD3D12Execute`（字段见 `third_party/xess/inc/xess/xess_d3d12.h`）：

| FrameInputs | 来源 |
|---|---|
| `depth` | `pExecParams->pDepthTexture` |
| `mv` | `pExecParams->pVelocityTexture` |
| `jitter_x/y` | `jitterOffsetX/Y` |
| `render_w/h` | `inputWidth/inputHeight` |
| `output_w/h` | `pInitParams->outputResolution.x/y`（Init 时缓存） |
| `mv_scale` | `USE_NDC_VELOCITY` ⇒ `(render_w*0.5, -render_h*0.5)`，否则 `(1,1)`；`HIGH_RES_MV` 时把 `render_w/h` 换成输出尺寸（同 OptiScaler `XeSS_Dx12.cpp:192-209`） |
| `flags` | `XESS_INIT_FLAG_INVERTED_DEPTH / JITTERED_MV / HIGH_RES_MV` |
| 相机 | **没有** ⇒ 不置 `kFlagHasCamera`，bridge 继续用合成投影 |

### 6.3 深度格式与"尺寸必须等于 MV"（`XESS_FG_NOTES.md` §1）

现有 addon 已经有 `DepthConvert`（R32→R16_UNORM 的 compute pass）与 `StreamPercent` 降采样，
**这两条对所有输入源通用**，不要按源分叉。

尺寸优先级**保持现状**（`game_capture.cpp:940-955`）：`[capture] RenderWidth/Height` > 源的 render size > 默认 2560×1440，
日志里 `mv_src` 会写成 `ini` / `NGX render size` / `default`，改成多源后把那句文案换成源名即可。
（ini 赢是有意的：它是"游戏报的尺寸不对"时的逃生口。）

### 6.4 相机参数缺失时的兜底（不要一步登天）

| 输入源 | 相机 | bridge 行为 |
|---|---|---|
| `ffx` | ✅ near/far/vfov | 用真实参数合成投影；`--fg-fov/--fg-proj-rz` 变成 override |
| `ngx` / `xess` | ❌ | 保持现状：按游戏手调 `--fg-fov`（PBZ 58.7 / CP2077 默认），`--fg-proj-rz` 控反向 Z |
| `depth` | ❌ | 同上 |

日志里必须一眼看出用的是哪条：`consts: camera=ffx near=0.10 far=100000 fov=58.70` /
`consts: camera=synthetic (--fg-fov 58.70, rz=1)`。

### 6.5 构建：多文件 addon

`scripts/build_addon.sh` 已经 `cp` 所有 `*.cpp`，问题只在 `build_addon.bat` 只接一个源文件名。两个方案：

- **A（推荐，零风险）**：新增 `src/game_capture/unity.cpp`：
  ```cpp
  // unity build：只为了让 build_addon.bat 的单文件编译继续成立
  #include "game_capture.cpp"
  #include "in_ngx.cpp"
  #include "in_ffx.cpp"
  #include "in_xess.cpp"
  #include "in_reshade.cpp"
  ```
  然后 `./scripts/build_addon.sh src/game_capture game_capture unity`（或把默认首源换成 `unity.cpp`）。
- **B**：改 `build_addon.bat` 接 `*.cpp` 列表 + 改 `scripts/build_addon.sh` 传参。
  更"正规"，但会动所有 addon 模块的构建路径（`tests/*` 也在用），回归面更大。

新增头文件（`inputs.h`）不需要链接改动；不需要新库；`ffx`/`xess` 源只用 `GetProcAddress` + 头文件里的结构体，
**不需要 link `third_party/ffx/lib/*.lib`**（那些是给 bridge 的光流用的）。

### 6.6 UI 只读信息改成运行时探测

`mgpu_ui.cpp:312/314` 现在写死。最小改法：bridge 新增 `--dump-caps <path>`，把
`{adapter, vendor, fg_api, max_interp, input_api}` 写成一行 `k=v` 文本；UI 启动 bridge 前先读它
（或读 `bridge.log` 的能力块）。这样"副卡换了"不需要改 UI 代码。

---

## 7. 风险与边界

| 风险 | 说明 | 缓解 |
|---|---|---|
| XeFG 在 N 卡副卡的未知项 | 先例在 RTX 5050 跑通，但那是**游戏内**；我们是独立进程 + 自建窗口。`D3D12CreateContext` 返回码、present 是否仍按 vblank、10 bit flip 是否被接受都要实测 | M6.1 先用 `--fg-probe` 拿能力再上整链；A 卡基线随时回退 |
| Intel 副卡"不 patch"的前提 | 假设原生 `maxSupportedInterpolations > 1` 且模型存在；若某代 Arc 仍报 1，则退回"patch + 接受 2x 模型" | 用 `GetProperties` + `D3D12GetInitParams` 回读，日志写明走了哪条 |
| 游戏用 FSR1 / 原生 TAAU | 既没有 MV 也没有 render-res 深度 | 只能 `depth` 源 + bridge 光流（`M2_RESULTS.md` 的深度尺寸问题需重新确认） |
| 游戏自己开了 FG（DLSSG / FSR3-FG） | 会变成三层插帧 | `ffx`/`ngx` 源检测到 FG desc 立刻告警，请求人工关游戏内 FG |
| MV 曾被当成"死输入" | 该结论已于 2026-11-08 被 `--mv-shadow` 对照推翻：XeFG **确实消费** MV | 按来源分工：3x/4x 用光流（`--fg-of`，0.3 ms）、2x 用游戏 MV（+7.4 ms）。`SkipMv` 仍由 bridge 的请求位决定 |
| 资源跨适配器不可用 | 游戏把 MV/深度放进保留资源/`DENY_*` 堆，无法跨适配器 | 现有降级：`--no-depth` / `--no-inputs`；源"给得到就给"，给不到报一次日志 |
| 协议 v6 两边不同步 | 旧 addon + 新 bridge | 既有规则：`magic/version` 不符直接拒绝（`mgpu_shared.h:24-35`），且 `HANDOFF.md` §2 要求同次构建部署 |
| 反作弊 | 在游戏进程 hook 超分库 | 与现状同级（已 hook NGX）；仍不建议联机游戏使用 |
| 显存 | A 卡副卡 4K/5K 余量本就接近零（`FINAL_CONFIG.md` §9.2） | 与本次改造无关；N 卡副卡显存更大反而更宽松 |

---

## 8. 验收清单（每个里程碑跑一遍）

```bat
rem 0) 纯能力自检（不需要游戏）
bridge.exe --fg-probe --gpu 1002      rem A 卡
bridge.exe --fg-probe --gpu 10de      rem N 卡
rem 期望：adapters[]（名字/vendor/device/VRAM/LUID/输出）
rem       chosen=... why=...
rem       XeFG version / maxSupportedInterpolations（before/after --unlock-mfg）

rem 1) 组合 1（基线）：游戏 N 卡 + DLSS，副卡 A
run_mgpu_pbz_3x40.bat
rem addon 期望：input source: ngx  render=2560x1440 out=5120x2880 camera=synthetic
rem bridge 期望：adapter 'AMD Radeon RX 6750 GRE' vendor=1002 why=default

rem 2) 组合 2：游戏 N 卡 + FSR，副卡 A
rem addon 期望：input source: ffx (ffxDispatch)  camera=ffx near=.. far=.. fov=..
rem bridge 期望：consts: camera=ffx ...

rem 3) 组合 3：生产端 N + 消费端 N（同机 POC，最便宜的 N 卡副卡验证）
bridge.exe --game PBZ.exe --gpu 10de --den 1 --pool 4 --interp 2 --unlock-mfg ^
           --fg-ui none --no-mv
rem 期望：选到 3080 Ti；xN=3.00；skip 与 A 卡基线同量级

rem 4) 组合 4：生产端 A + 消费端 N（目标形态）
rem 游戏窗口放到 A 卡显示器、开 FSR；bridge.exe --gpu 10de ...
```

判据沿用 `FINAL_CONFIG.md` §6.2：看 `show / xN / xNmiss / skip / gpu / total`，
以 `skip` 不恶化为前提；`skip` 恶化的第一嫌疑仍是 WGC/DWM（`MFG.md` §3.2 归因表）。

---

## 9. 参考与仓库同步建议

| 项 | 位置 |
|---|---|
| OptiScaler 完整源码（本机） | `<DOWNLOADS>\OptiScaler-master.zip` → 解到 `temp/optiscaler_src/`（gitignore） |
| 已入库的参考片段 | `third_party/optiscaler/{shaders/camera_motion, inputs/FG, LICENSE, README.md}` |
| 建议补进 `third_party/optiscaler/` 的 8 个文件（~250 KB） | `hooks/LibraryLoad_Hooks.cpp`、`hooks/FG_Hooks.cpp`（节选）、`inputs/FfxApi_Dx12.cpp`、`inputs/XeSS_Dx12.cpp`、`inputs/FG/Upscaler_Inputs_Dx12.cpp`、`framegen/IFGFeature.h`、`framegen/ffx/FSRFG_Dx12.cpp`（节选）、`misc/IdentifyGpu.h` |
| 新增第三方依赖（M6.4 前必须） | AMD `FidelityFX-SDK` 的 `sdk/include/FidelityFX/host/ffx_api.h`、`ffx_api_types.h`、`ffx_api_upscaling.h`、`ffx_api_framegeneration.h`、`dx12/ffx_api_dx12.h`（OptiScaler 从 `external/FidelityFX-SDK` 子模块取，我们这里该子模块是空的） |
| 我们的既有结论 | `docs/findings/OPTISCALER_NOTES.md`、`docs/findings/MFG.md` §1、`docs/findings/XESS_FG_NOTES.md` §3/§4 |

> `third_party/optiscaler/README.md` 已写明"完整 checkout 已删除"。若决定长期走多厂商，
> 建议按上表补 8 个文件，省得每次重新解 123 MB。补的时候沿用现有做法：
> 只保留**证明字段名/调用顺序**的最小片段，并在文件头注明来源 commit。

---

## 10. 一句话总结

**bridge 的"N/A 兼容"是拆掉 `VendorId == 0x1002` 并让 Intel 不打补丁（半天到一天）；
addon 的"N/A 兼容"是按 OptiScaler 的"输入归一化"思路把 NGX 单源换成
NGX / FSR / XeSS / ReShade 四源（三到五天），协议升 v6 把相机参数与输入标志带过去。**
两者都不需要改 XeFG、不需要碰光流、不需要 GPU 伪装。

