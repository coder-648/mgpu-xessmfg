# 组件文档：fakerFG.dll（游戏进程内的 FG 管线所有者）

代码：`src/faker_fg/`（9.4k 行，14 个 .cpp + 4 个头文件）。最后更新：2026-11-15。

---

## 1. 角色

它是**唯一决定"谁生成帧"的组件**，并伪装成游戏的图形依赖：

| 部署名 | 被谁加载 | 干什么 |
|---|---|---|
| `dxgi.dll` | 游戏（DXGI 代理） | 接管 swap chain、拦截 Present |
| `nvngx_dlssg.dll` | 游戏 / DLSS-G 路径 | 回答 `NVSDK_NGX_*` 的 DLSS-G 接口 |
| `nvngx.dll` | 游戏 / DLSS-SR 路径 | 回答 DLSS-SR 的 `EvaluateFeature`（harvest 输入） |
| （同文件三份） | — | 三个名字必须是**同一份二进制** |

## 2. 五种模式（`MgpuFgMode`）

| 模式 | 行为 | 用途 |
|---|---|---|
| `OFF` | 不回答，真 runtime 全权 | 对照/回滚 |
| `OBSERVE` | 回答后转发给真 runtime | 采集语义、不接管 |
| `IDLE` | 回答但**不生成**，直接呈现真实帧 | 1X 基线、排障 |
| `LOCAL` | 本卡（NVIDIA）用 XeFG 生成 | 单卡对照 |
| `BRIDGE` | **第二张卡生成**，本进程只呈现 | 我们的生产配置 |

模式来自 `fakerFG.ini` 的 `Mode=`（`consume` → BRIDGE），也可被 UI 改写；
消费者对模式**没有任何话语权**（`docs/design/FAKER_FG.md` 的设计约束）。

## 3. 模块职责

| 文件 | 职责 |
|---|---|
| `faker_fg.cpp` | 模式选择、`g_record` 存储、`MgpuFgGetApi` 表、`publish_record()`、sink 派发 |
| `provider.cpp` | 回答 DLSS-G 的 Create/Evaluate/Release；决定"我们是否接管" |
| `proxy_dxgi.cpp` | DXGI Factory/SwapChain 代理；`CreateSwapChainForHwnd` 替换成我们的 |
| `fg_swapchain.h/.cpp`, `fg_swapchain_hooks.cpp` | 我们自己的 swap chain 包装（Present、tag、统计） |
| `backend_xefg.cpp` | 把 XeFG 当作后端：init/tag/present、LOCAL 与 BRIDGE 共用 |
| `passes.cpp` | GPU pass：**marker 绘制**、pre-marker 复制（scar）、色彩准备 |
| `mv_resample.cpp` | MV 重采样（游戏在渲染分辨率给 MV，输出分辨率用） |
| `proxy_ngx.cpp`, `proxy_nvngx.cpp` | NGX 入口（DLSS-SR / DLSS-G），含 arch 伪造 |
| `proxy_streamline.cpp` | `sl::Constants` / `sl::ResourceTag` 采集（真投影、jitter、reset） |
| `proxy_kernel.cpp` | kernel32 代理（LoadLibrary/GetProcAddress），用于发现与注入顺序 |
| `nvapi_arch.cpp` | 对 NGX 伪造架构信息（让它在本卡创建 feature） |
| `producer_link.cpp` | `LoadLibrary(xess_mfg_producer.dll)` + `MgpuFgSetSink` |

## 4. ABI（`mgpu_fg_api.h`，v2）

进程内 C 接口，函数指针表（因为模块以代理名部署，没有可链接的导入库）：

```c
typedef struct MgpuFgFrame {
    uint32_t size, version;
    uint64_t seq;            // 每次 Evaluate 递增（= 共享内存里的 frame_id）
    uint64_t present_id;
    uint64_t frame_qpc_ms;   // Evaluate 时刻（注意：不是帧间隔）
    void*    cmd_list;       // 打开中的命令列表（borrowed）
    void*    device;
    MgpuFgResource stream[MGPU_FG_STREAM_COUNT];  // MV/DEPTH/HUDLESS/UI/BACKBUFFER/...
    uint32_t render_w/h, output_w/h, declared_frames, mode, flags, generation;
    float    jitter_x/y, mv_scale_x/y;
    float    camera_near/far/fov_y, aspect;
    float    view_to_clip[16], clip_to_prev_clip[16], prev_clip_to_clip[16];
} MgpuFgFrame;
```

**两个绑定契约**：

1. `MgpuFgResource.validity` 声明资源可用到什么时候（`UNTIL_PRESENT` /
   `VALID_BUT_MAKE_COPY` / `VALID_UNTIL_PRESENT_FROM_DISPATCH`）——
   消费者必须按它决定"能不能延迟用"；
2. `MgpuFgFrameSink` 在 `publish_record()` 里**同步**调用，此时 `cmd_list` 仍打开 ——
   transport 必须在这个窗口内录完复制。

## 5. 一帧在 fakerFG 内部怎么走

```
DLSS-G Evaluate（或 SR Evaluate，取决于游戏用哪条）
  ├─ provider 判断：我们要不要接管？(Mode)
  ├─ harvest：从 NVSDK_NGX_Parameter / sl::Constants 读
  │    颜色/深度/MV 资源、jitter、mv_scale、相机、numFramesToGenerate、
  │    resetHistory、clip 矩阵、frame_key、present_id → MgpuFgFrame
  ├─ 拷贝进 g_record（sink 看到的是快照，不是活引用）
  └─ publish_record() → dispatch_sink() → producer 的 sink（同步）
Present（游戏）
  ├─ DXGI 代理拦截 → 我们的 swap chain
  ├─ pass_marker()：在 backbuffer 上画 4 个 16×16 block，编码 seq+crc
  ├─ present-ring 写入 (qpc, seq)
  └─ backend 呈现：IDLE 直接 Present；BRIDGE 交给 XeFG（tag 由消费者完成）
```

> 注意：**marker 是给消费者解码用的**（颜色侧配对）。`frame_qpc_ms` 是 Evaluate 时刻，
> 游戏一帧会 Evaluate 多次（CP2077 ≈3.3×），所以它不能当帧间隔用 —— 见 §7。

## 6. marker 与 scar

- marker：4 角各 16×16、按 `kMarkerCells` 网格编码 `seq12 + crc8`；用当前 backbuffer 格式
  （sRGB 或 HDR10）。消费者从 WGC 颜色里解码出 seq → 与共享内存的 `frame_id` 比对。
- scar（v16/v17）：在**画 marker 之前**把 16×16 内容复制到 `streams[kStreamScar]` 的同一发布槽；
  消费者在自己做完 HDR 转换后把它**贴回去**，于是 marker 不出现在成片里。
  「同一槽 = 同一帧」，不需要回读、不需要查找表。

## 7. 已知的语义陷阱

| 陷阱 | 后果 | 处理 |
|---|---|---|
| `frame_qpc_ms` 是 Evaluate 时刻，一帧多次 | 用它算帧间隔会偏小（~7.5ms vs 25ms） | 生产者改用 **publish 间隔** 的 EMA（`frame_time_ms`） |
| 一帧可能多次 publish（XeFG chain + title present） | 同一 `frame_qpc_ms` 会重复发布 | `publish_frame()` 用 `g_last_publish_frame_qpc` 去重 |
| `decalred_frames`（游戏自己的 DLSS-G 也在生成） | 我们再做 FG 会翻倍 | `ProducerInfo.declared_frames` 会暴露给消费者 |

## 8. 配置（`fakerFG.ini`，只列影响链路正确性的）

| 键 | 默认 | 说明 |
|---|---|---|
| `Mode` | `local` | `off/observe/idle/local/bridge`；生产用 `consume`(=bridge) |
| `Passthrough` | 0 | 1 = 完全不接管（排障第一开关） |
| `OwnSwapchain` | 1 | 0 = 用游戏的 swap chain（对照） |
| `Interpolated` | 1 | 请求的插值张数（本地后端用） |
| `Publish` | 1 | 0 = 不做整个发布路径（无 MV/depth 传输） |
| `DepthInverted` / `MvJittered` / `HighResMv` | 1/0/0 | 输入语义声明（会覆盖线上探测） |
| `MvResample` | 0 | 是否重采样 MV 到输出分辨率 |
| `TagHudless` / `TagBackbuffer` / `TagUi` | 0/1/0 | 哪些流参与 tag |
| `ArchSpoof` | 1 | 对 NGX 伪造架构（让它在本卡创建 feature） |
| `FakeDlssgFeature` | 1 | 伪造 DLSS-G 的 feature 句柄（不接管时） |
| `SkipFirstFrames` | profile | 开局跳过的帧数（避开首帧资源未就绪） |
| `ForceBorderless` | profile | 强制无边框（overlay 需要） |

## 9. 不变式（改代码必须保持）

1. 三份同名 DLL（`dxgi.dll`/`nvngx.dll`/`nvngx_dlssg.dll`）必须是**同一二进制**；
2. `publish_record()` 只能在 `cmd_list` 打开期间派发 sink；sink 内不得阻塞（它是游戏线程）；
3. 同一 `frame_qpc_ms` 不得重复发布（去重已在 `publish_frame()`）；
4. marker 必须在 Present 之前画进 backbuffer；scar 必须在画 marker **之前**复制；
5. `MgpuFgFrame.resource[].validity` 是硬契约，transport 不能假设资源活到下一帧。

| 资源 `ONLY_NOW` | 延迟使用会读到被复用的内容 | transport 必须在 sink 窗口内拷完 |
