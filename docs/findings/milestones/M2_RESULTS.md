# M2 结果：DLSS 参数截取成功

日期：2026-11-06　探针：`src/m2_probe/m2_probe.cpp` → `build/m2_probe.addon64`
成功日志：`temp/m2/m2_probe_run5_SUCCESS.log`（988 行，53 次 EvaluateFeature）
测试条件：赛博朋克 2077 / 5120×2880 / DLSS 性能档 / HDR / 游戏世界内站 60 秒

---

## 1. 结论先行

| 项目 | 实测值 | 与之前设计的关系 |
|---|---|---|
| 运动矢量 | **2560×1440 `DXGI_FORMAT_R16G16_FLOAT`(34)** | ✅ 与 M1 占位规格**完全一致**，不用改 |
| 深度 | **2560×1440 `DXGI_FORMAT_R32_TYPELESS`(39)**（即 D32_FLOAT） | 需在深度 tap 里降为 R16_UNORM（省 7MB） |
| DLSS 输入色 | 2560×1440 `R16G16B16A16_FLOAT`(10) | ⚠️ 是**渲染分辨率**，不是 5K |
| DLSS 输出 | **5120×2880 `R16G16B16A16_FLOAT`(10)** | ⚠️ **118 MB/帧，绝不能传这个** |
| jitter | 每帧变化（如 `-0.245, +0.430`） | ✅ 可读，直接用于 `TagFrameConstants` |
| 相机矩阵 | `CamViewToClip/ClipToCamView/ClipToPrevClip` **全为 0** | ⚠️ DLSS-SR 不给，M4 需备选方案 |
| 调用者 | `170_E658700.dll + 0x88AF0`（游戏自身模块） | CP2077 是**直接调 NGX**，不经 Streamline 的 sl.dlss |

**载荷规格正式锁定**（与 M0.3 实测的 77.3 MB 配置一致）：

| 流 | 尺寸 | 格式 | 字节 |
|---|---|---|---|
| 颜色（取 ReShade 的后台缓冲，不是 DLSS 输出） | 5120×2880 | R10G10B10A2 | 56.25 MB |
| 运动矢量（游戏原生，零转换） | 2560×1440 | R16G16_FLOAT | 14.06 MB |
| 深度（tap 输出 R16_UNORM） | 2560×1440 | R16_UNORM | 7.03 MB |
| **合计** | | | **77.34 MB/帧** |

> **关键陷阱**：DLSS 的 Output 纹理是 5120×2880 的 **FP16（8 字节/像素）= 118 MB**。
> 如果按"拿 DLSS 输出当颜色"的思路，单这一项就会把预算打爆。
> 正确做法是从 ReShade 的 present 事件拿后台缓冲（R10G10B10A2，4 字节/像素）。

## 2. 为什么前四轮全失败（踩坑记录）

| 轮次 | 做法 | 失败原因 |
|---|---|---|
| 1 | 轮询 `GetModuleHandle("nvngx_dlss.dll")` 后挂 hook | 挂上了，但游戏**每帧调用的不是这份** |
| 2 | 主动 `LoadLibrary("nvngx_dlss.dll")` 抢先挂 | 同上；还加载了一份游戏根本没用的副本 |
| 3 | hook 四个 `LoadLibrary*`，按路径匹配 | 生效了，但匹配到的仍是"探测用的副本"（加载后被卸载） |
| 4 | 加 `GetProcAddress` 记录 + 完整模块表 dump | 发现真相：常驻的 NGX 提供者是驱动目录的 `_nvngx.dll` |
| **5** | **改为 hook `_nvngx.dll`** | ✅ 一次成功 |

**根因**：CP2077 会把 `nvngx_dlss.dll` 的多个副本（exe 旁、`dlssg_sm86/sr/` 下）**加载-查版本-卸载**，
真正执行 DLSS 的是 `C:\WINDOWS\System32\DriverStore\...\nv_dispi*\`_nvngx.dll`**（NVIDIA 驱动垫片）。

## 3. 探针能力（可复用于后续模块）

- `AddonInit` 里 hook `LoadLibraryExW/W/A` + `GetProcAddress`，任何时刻出现的 NGX 提供者都能被抓到
- 按模块**完整路径**判断身份（`_nvngx.dll` vs `nvngx_dlss.dll`），不靠文件名猜
- `CreateFeature` / `EvaluateFeature` / `GetFeatureRequirements` 三个入口全挂
- `EvaluateFeature` 里读 `NVSDK_NGX_Parameter` 的 vtable：MV、深度、颜色、输出、jitter、候选矩阵键
- `_ReturnAddress()` 反查调用者模块（确认是游戏本体而非 Streamline）
- t+20s / t+45s 两次完整模块表 dump

## 4. 遗留风险：相机矩阵

XeSS-FG 的 `xefg_swapchain_frame_constant_data_t` **要求** view / projection 矩阵，但 DLSS-SR 的参数里没有。
`DLSSG.*` 那套键在 SR 的 parameter 对象里读不到（全为 0）。

M4 的候选方案（按优先级）：
1. **先试合成矩阵**：用 jitter + 一个近似投影（从 FOV/宽高比推）代入，看 XeSS-FG 是否接受、画面是否正常。
   XeSS-FG 主要用矩阵做相机运动一致性判断，未必需要逐像素精确。
2. 若不行，**hook `sl.dlss_g` / `nvngx_dlssg`** 拿 `DLSSG.CameraViewToClip`（需要游戏里开着 DLSS 帧生成，
   但可以把 Streamline 的 FG 输出丢掉，只借它的参数）。
3. 从深度 + MV 反推单应矩阵（工程量大，最后手段）。

## 5. 下一步

M3：ReShade 深度采集。已确认深度是 2560×1440 D32_FLOAT，tap shader 直接输出 R16_UNORM 2560×1440，
写进 `mailbox.producer_source(2)`。探针里 ReShade 已正常加载，`reshade::register_addon` 成功，可以复用。
