# 输入源：DLSS / FSR / XeSS

add-on 里"这一帧的 MV/深度/相机从哪来"被抽成 `mgpu::input::Source`
（`src/game_capture/inputs.h`）。选哪个源**由游戏实际加载了哪个超分库决定**，
不由显卡厂商决定 —— 同一块 A 卡可以跑这三个中的任何一个。

配置项：`game_capture.ini` 的 `Input=auto|ngx|ffx|xess|depth`（默认 `auto`，
先给出数据者胜）。

## 1. 能力对比

| 源 | 钩什么 | MV | 深度 | jitter | 相机（near/far/vfov） | 状态 |
|---|---|---|---|---|---|---|
| `ngx`（DLSS-SR） | `NVSDK_NGX_EvaluateFeature` | ✅ | ✅ | ✅ | ❌ **NGX 不导出相机** | ✅ 可用 |
| `ffx`（FSR 2.2 / 3.x） | 5 个 ffx-api 入口 | ✅ | ✅ | ✅ | ✅ **能拿到真相机** | ✅ 可用 |
| `xess`（XeSS） | `xessD3D12Execute` | ✅ | ⚠️ 看游戏 | ✅ | ❌ 不导出 | ✅ 可用 |
| `depth` | 只走 ReShade 深度 | ❌ | ✅ | ❌ | ❌ | 兜底 |
| TSR / FSR1 / 原生 TAA | — | ❌ | ✅(ReShade) | ❌ | ❌ | **不支持**，优雅降级 |

三个源都是**观察者**：每个调用都原样转发，不改参数、不替换实现。这是它能和游戏
自己的超分（甚至游戏自己的帧生成）共存的原因。

## 2. 各源的细节

### `ngx` — DLSS-SR

- 只钩**真正执行 DLSS 的那个模块**。判据（2026-11-09 起）：
  1. 取**文件名**，必须含 `nvngx`（覆盖 `_nvngx.dll`、`nvngx.dll`、`nvngx_dlss.dll`）；
  2. 排除 `dlssg` / `dlssd` / `dlssnr`（帧生成 / 光追重建 / NR 不是本项目的输入源）；
  3. **真的导出 `NVSDK_NGX_D3D12_EvaluateFeature`** 才挂。
- 为什么不是"只认 `_nvngx.dll`"：CP2077 会加载好几个 `nvngx_dlss.dll` 副本只是为了读版本号，
  实际执行的是驱动商店的 shim `_nvngx.dll`（`docs/findings/milestones/M2_RESULTS.md`，load-bearing）；
  但 **RE Engine / Streamline** 的题目（PRAGMATA）把 `nvngx_dlss.dll` 放在游戏目录并
  **直接调用它的 NGX 入口**，旧的"只认 shim"规则让它们**一个钩子都装不上**
  （`docs/findings/RE_ENGINE_NOTES.md`）。第 3 条判据同时满足这两种情况：
  CP2077 那些只读版本号的副本不导出入口，仍然被挡掉。
- 拿不到相机 ⇒ bridge 用合成投影（`--fg-fov`）。实测合成投影对画质没有影响
  （见 `docs/findings/CONCLUSIONS.md` §1），所以这不是缺陷。

### `ffx` — FSR 2.2 / 3.x

- 钩 `amd_fidelityfx_upscaler_dx12.dll`（FSR 3.1）或 `amd_fidelityfx_dx12.dll`
  （2.2 / 3.0）的 5 个 ffx-api 入口。
- `ffxDispatchDescUpscale` 里带的相机是**真的**：
  `cameraNear/Far/FovAngleVertical` ⇒ 置 `kInputFlagHasCamera`，bridge 用它构建
  真实投影，`--fg-fov` 会被忽略。
- 头文件是 `third_party/ffx/api/include/ffx_api/*`（FidelityFX-SDK v1.1.4，MIT）；
  注意 `third_party/ffx/include/FidelityFX/` 里那套 per-effect 头是**另一套 API**，
  这里不用。

### `xess` — XeSS

- 钩 `xessD3D12Execute`，从 `xessD3D12Init` 的 `initFlags` 读语义：
  `HIGH_RES_MV`、`INVERTED_DEPTH`、`USE_NDC_VELOCITY`、`JITTERED_MV`、`LDR_INPUT_COLOR`。
- `HIGH_RES_MV` 意味着 MV 在**输出分辨率**上（`kInputFlagHiResMv`），这时 MV 的
  尺度/单位与 UV 不同，桥侧要按 flag 换算。
- **实测（PBZ）：该游戏只给 MV、不给深度。** 所以 bridge 必须在"没有深度"时也能
  继续跑（深度实测对画质无影响，见 `CONCLUSIONS.md` §1）。
- 不导出相机。

### `depth` — 兜底

没有超分（或者超分不认识）时，只从 ReShade 拿深度。没有 MV ⇒ bridge 只能
`--no-mv` 或自己算光流（`--fg-of`）。

### 不支持的：TSR / FSR1 / 原生 TAA

不挂钩子，**优雅降级**：画面正常、没有 MV/深度，30 秒内日志会明确写出
"no supported upscaler found"，不会静默失败。

## 3. 加一个新源要做什么

1. 新建 `src/game_capture/in_xxx.cpp`，实现 `mgpu::input::Source` 接口
   （`inputs.h`），namespace 用自己的名字（`unity.cpp` 把每个源编进同一个 TU，
   匿名 namespace 会合并、状态会串）；
2. 填 `FrameInputs`：`mv`/`depth` 指针、`jitter_*`、`mv_scale_*`、`render_*`、
   `output_*`，有相机就填 `near/far/vfov` 并置 `kInputFlagHasCamera`；
3. 在 `inputs.h` 的 `kInputApi*` 里加 id，在宿主的选择逻辑里登记；
4. 在 `unity.cpp` 里 `#include` 新文件；
5. 更新本文的能力表与 `mgpu_shared.h` 的 `kInputFlag*` 注释。

**不要**在源里做任何"修正"（翻转、缩放、单位换算）：源只报告事实，
换算交给消费端按 flag 处理 —— 这是 `VENDOR_COMPAT_PLAN.md` §M6.3 定下的分工。
