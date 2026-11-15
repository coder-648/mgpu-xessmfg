# fakerFG —— 生产端重构定案（2026-11-12）

> 状态：**P1–P5 已实现并编过**（`src/faker_fg/`，`./scripts/build_faker_fg.sh` 产出
> `build/fakerFG.dll`，88 KB / 80 个导出）。真机验证步骤见 §11；capture 侧的消费
> （`in_faker.cpp`）仍是 P3 的剩余部分，不影响 §11 的三档验证。
> 本文取代 `FG_BACKEND.md` 的"角色分配"部分（它把 provider 与 backend 塞进 ReShade add-on，
> 现在看是错的位置），也取代 `src/fg_host/` 那个半成品模块。
> 需求原文见仓库根目录 `问题和描述.md`。

## 0. 一句话

游戏里的 **FG 管线由我们自己的 `fakerFG.dll` 拥有**：它回答 DLSS-G、把这一帧的全部输入整理成一条
记录、用 XeFG 上屏；**`game_capture.addon64` 退回纯传输**，只从 `fakerFG` 拿数据、压小、发到副卡，
不再打断、不再猜、不再介入 FG 的执行。

```text
              ┌──────────────────────── fakerFG.dll（最早加载）───────────────────────┐
  game ──sl/ngx──►│ provider → harvest → FrameRecord ──► backend(XeFG, 拥有 present)   │
              └───────────────┬───────────────────────────────────────────────────┘
                              │ MgpuFgGetApi()（进程内，零拷贝的指针表）
                              ▼
              game_capture.addon64（ReShade）──► 共享槽/fence ──► bridge.exe（副卡）
                       只做：拷贝、降采样、发布            插帧、覆盖窗口上屏
```

## 1. 为什么这个形状是唯一能成立的

三条实测，每条都指向"必须在游戏进程里、而且必须最早加载"：

| 事实 | 出处 |
|---|---|
| Streamline 在 `slInit` 时问 `NvAPI_GPU_GetArchInfo`，那时候还没有 `sID3D12Device`，ReShade add-on 也还没加载 | `docs/design/PRODUCER_V14.md` §1 |
| `sl.dlss_g` 插件在**加载时**用 `GetProcAddress` 把 `NVSDK_NGX_D3D12_*` 缓存下来并一直持有；add-on 在交换链创建期才装，改不动那些指针 | `src/game_capture/fg_module_proxy.cpp` 文件头（CP2077 实测） |
| 作为观察者去写 `DLSSG.OutputInterpolated` 只能拿到 ReShade 的 `general` 伪状态，两次都掉设备；`FgMode=consume` 在不生成的情况下必然黑屏，因为这张纹理是游戏的 presenter 要看的 | `CHANGELOG.md`（consume 崩溃 / 黑屏两条） |

也就是说：**只有"我们是 provider"时，"不生成但要画面正确"才合法**——资源是游戏按 FG 契约交给我们的，
状态由我们声明，拷贝是我们记的。这正是 `FG_BACKEND.md` §1 的结论，本文只是把它落在一个独立的
DLL 上，而不是让 ReShade add-on 去干这件事（时机不允许）。

## 2. 职责划分（新的三方边界）

| 模块 | 进程 / 形态 | 负责 | 明确不负责 |
|---|---|---|---|
| **fakerFG.dll** | 游戏进程，最早加载（代理 DLL） | FG 契约（Available/Requirements/SetOptions/GetState）；DLSS-G 输入；XeFG 输出；present 所有权；`FgMode`（off/observe/idle/local/bridge）；决定这一帧生不生成 | 跨卡传输、共享槽、协议头、降采样 |
| **game_capture.addon64** | 游戏进程，ReShade add-on | 从 fakerFG 取 `MgpuFgFrame`；MV/深度降采样（`StreamPercent`）；写共享槽 + fence；发布 `mgpu_shared.h` v14 头 | 打断、claim、`FgCut`、`SlFgOff`、`FgSuppressNgx`、填 `OutputInterpolated`、任何对 FG 参数的改写 |
| **bridge.exe** | 独立进程 / 第二块卡 | 读槽位 → XeFG 插帧 → 覆盖窗口上屏；节拍、OSD、诊断 | 抓 FG 输入（不再需要任何钩子） |
| **sm86** | 游戏进程，`version.dll` | **不再需要**（见 §3.3）。保留为回退 | — |

一句话记法：**fakerFG 决定"有没有 FG、谁生成"，capture 决定"数据怎么走"**。

## 3. 注入链（fakerFG 是那个"第一个进去的东西"）

### 3.1 文件部署（与 OptiScaler 同构）

```text
<GAME_DIR>\
  dxgi.dll            ← fakerFG.dll 改名（代理 1：DXGI）
  ReShade64.dll       ← ReShade 改名（ReShade 6.7+ 接受带 "d3d12" 的名字也行）
  nvngx_dlssg.dll     ← fakerFG.dll 的另一份拷贝（代理 2：NGX 提供者）
  nvngx_dlssg_real.dll← 游戏原来的 DLSS-G 运行时（回退/observe 模式用）
  fakerFG.ini         ← 我们自己的配置（与 game_capture.ini 分开）
  reshade-shaders\    ← MGPU_DepthTap.fx 等空转效果
  game_capture.addon64← 采集端（仍是 ReShade add-on）
```

`dxgi.dll` 是**进程级最早**能拿到执行权的位置之一（D3D12 游戏几乎都静态导入 dxgi），
`nvngx_dlssg.dll` 是**FG 契约的入口**。两个文件同名不同副本，逻辑同一个 DLL：以哪个名字被加载
就干哪份活（`GetModuleFileNameW(self)` 判断），这比 OptiScaler 的 `OptiScaler.dll + nvngx.dll`
少一跳，也避免"主 DLL 被游戏当成依赖加载"的歧义。

### 3.2 启动顺序

```text
进程启动
  → dxgi.dll(fakerFG) DllMain：起一个自己的线程（不在 loader 锁里做重活）
      · 立刻装 GetProcAddress / LoadLibraryW 的早期拦截（此时 slInit 还没跑）
      · 架构问答（NvAPI_GPU_GetArchInfo）在这里被回答成 sm_80+
      · 加载真正的 system32\dxgi.dll，转发五个导出
      · 按 ini 加载 ReShade64.dll（ReShade 自己会去加载 add-on）
  → 游戏 slInit → sl.dlss_g 加载 → 解析 nvngx_dlssg.dll
      · 如果 nvngx_dlssg.dll 是我们的副本：指针天然就是我们的（最稳）
      · 否则：早期拦截把 GetProcAddress 的答案换成我们的 thunk（备选）
  → 菜单出现 Frame Generation，且真的能开
  → 游戏 CreateFeature/EvaluateFeature → fakerFG 的 provider
```

**关键纪律**：`CAPTURE_RULES.md` 第 1/3 条对 fakerFG 同样适用——不把我们的包装对象交给游戏
（DXGI 五个导出原样转发，工厂/设备/队列不加壳），不在别人的初始化窗口里 `MH_EnableHook`。
早期拦截只改 `GetProcAddress` 这一个导出，且在我们自己的线程上、引擎开始创建设备之前完成。

### 3.3 sm86 为什么可以退场

sm86 干两件事：改写架构答案、让 30 系能开 DLSS-G。fakerFG 都要干，而且干了以后更彻底：
`Available=1` 是我们回的，`Requirements` 是我们回的，`CreateFeature` 是我们接的，
生成由 XeFG 完成（不依赖 NVIDIA 的多帧能力）。两个都改 `nvngx_dlssg.dll` 会打架，
所以**部署时二选一**，默认 fakerFG，`fakerFG.ini` 留一个 `Passthrough=1` 当 A/B 回退。

> 未验证：某些标题会把 `nvngx_dlssg.dll` 的文件签名/版本一起查（CP2077、PRAGMATA 在 sm86 下都能跑，
> 说明这条路可行；但"fakerFG 的副本"必须带上一个像样的版本资源，见 §9 风险 3）。

## 4. fakerFG 内部结构（文件级）

```text
src/faker_fg/
  unity.cpp              单 TU 编译入口（与 game_capture 同一种做法）
  faker_fg.cpp           DllMain / 配置 / 日志 / 记录 / MgpuFgGetApi / MT：起加载线程
  nvapi_arch.cpp         nvapi_QueryInterface 钩子：只给 Streamline 回 sm_80+（ArchSpoof）
  proxy_dxgi.cpp         20 个 DXGI 导出（5 个有逻辑，其余转发）+ 工厂 vtable 第 16 项
  proxy_ngx.cpp          nvngx_dlssg.dll 的 50 个导出 + 双实例桥（MgpuFgNgxBridge）
  provider.cpp           handle 空间、harvest（参数块 → MgpuFgFrame）、Evaluate 编排
  backend_xefg.cpp       XeFG 上下文、InitFromSwapChainDesc、tag、present id、日志
  mgpu_fg_api.h          与 capture 的唯一接口
  fakerFG.def            导出名表（NGX + DXGI 两组；名字由它决定，函数名不参与）
  build_addon.bat        （不需要：构建走 scripts/build_faker_fg.sh）
```

设计上原计划的 `api.cpp`（记录）并进了 `faker_fg.cpp`，`backend_idle.cpp` 并进了
`backend_xefg.cpp`——"不生成"只是 `SetEnabled(0)`，同一个上下文，不值得多一个文件。

可以直接搬的现成代码（都在树里，且是我们自己的）：

| 搬到 | 从 | 改什么 |
|---|---|---|
| `provider.cpp` | `src/game_capture/fg_provider.cpp` | 去掉对 `g_inputs` / `MgpuCaptureFillInterpolated` / `resolve_ini_path` 的依赖，改成 fakerFG 自己的配置与记录 |
| `backend_xefg.cpp` | `src/game_capture/fg_backend_xefg.cpp` | 交换链与队列不再来自 ReShade 事件，而是 provider 收到 `CreateFeature` 后的设备 + 游戏的 `IDXGIFactory`；补 `Present` 与 `ResizeBuffers` |
| `proxy_ngx.cpp` | `src/fg_host/fg_host.cpp` | 保留"转发真运行时"的骨架，把 Create/Evaluate 换成 provider |
| `harvest.cpp` | `in_fg.cpp` 的参数解析 + `in_ngx.cpp` 的 NGX 参数读取 | 只保留"读"的部分；拷贝不再在这里做（那是 capture 的活） |

对照参考（GPL-3.0，允许搬运，见 `FG_BACKEND.md` §7）：

| 我们的文件 | OptiScaler 对应 |
|---|---|
| `proxy_dxgi.cpp` | `proxies/Dxgi_Proxy.h`（导出列表与转发方式）、`Util.cpp` `ReShadePath()` |
| `proxy_ngx.cpp` | `framegen/nvngx/Nvngx_DllProxy.cpp` |
| `provider.cpp` | `framegen/nvngx/Nvngx_FG.cpp`（handle 空间、`PopulateParameters` 回调） |
| `harvest.cpp` | `inputs/FG/Streamline_Inputs.cpp`、`XeFG_Inputs_Dx12.cpp` |
| `backend_xefg.cpp` | `framegen/xefg/XeFG_Dx12.cpp`（`CreateSwapchainInternal`、`Present`） |

一个必须记住的兼容点：**交给 XeFG 的对象要先从 ReShade 的壳里拿出来**。
ReShade 6.7+ 用 `IID_UnwrappedObject`（OptiScaler `Util.cpp:829-870`）提供原对象，
否则 XeFG 拿到的是 ReShade 的代理指针，`TagFrameResource` 会失败或行为诡异。

## 5. 两端接口（`src/faker_fg/mgpu_fg_api.h`）

进程内、纯 C、函数指针表。不用共享内存传结构：记录里全是本进程的指针，跨进程没有意义；
真正的跨卡传输仍是 capture 的共享槽（协议 v14）。

```c
const MgpuFgApi* (*MgpuFgGetApi)(min_version, max_version);   // fakerFG 导出
struct MgpuFgApi {
    uint32_t (*mode)(void);                 // off/observe/idle/local/bridge，实时
    int      (*frame_available)(void);      // 有比上次 acquire 更新的帧吗
    int      (*acquire)(MgpuFgFrame* out);  // 拷一条记录（只在 Evaluate 线程里调）
    void     (*release)(void);              // 消费完，避免下一次 Evaluate 重发旧资源
    void     (*log)(const char*);
    void*    (*shared_block)(uint32_t*);    // 预留，今天为 0
    // v2：同步 sink。记录里的 cmd_list 只在 Evaluate 期间是打开的，
    // 要录 GPU 拷贝的消费者必须当场做完 —— 轮询永远拿不到合法时机。
    int      (*subscribe)(MgpuFgFrameSink sink, void* user);
    void     (*unsubscribe)(void);
};
struct MgpuFgFrame {            // 一条记录 = 一帧的全部 FG 输入
    seq, present_id, frame_qpc_ms, cmd_list, device
    stream[8]                   // MV/Depth/HUDLess/UI/UIAlpha/BB/OutputReal/OutputInterp
                                //   每个带 { resource, state(我们声明), validity, w, h, format }
    render_w/h, output_w/h, declared_frames, mode, flags, generation
    jitter, mv_scale, camera(near/far/fov/aspect), view_to_clip, clip_to_prev/prev_to_clip
};
```

规则（写进头文件的注释，也是 review 判据）：

1. `acquire` 只在 Evaluate 线程里调用；capture 的压力（降采样、发布）必须当场做完，
   因为它就在同一帧里，而不是"等下一个 present"。
2. `state` 是**我们声明的**，capture 照抄，不再推导（规则 4 的对象从"游戏的声明"变成"我们的声明"，
   更强：我们就是那条 pass 的作者）。
3. 记录按 `size`/`version` 扩展；capture 用 `size >= sizeof(MgpuFgApi)` 做前向兼容判断。
4. 找不到 fakerFG（老部署）→ capture 走自己的车道（`in_fg/in_ngx/...`），行为与今天一致。

`mgpu_fg_attach()` 用导出名而不是文件名找人（因为模块名是 `dxgi.dll`/`nvngx_dlssg.dll` 之类），
候选名就是 OptiScaler 支持的那几个代理名。

## 6. capture 侧收缩（删什么、留什么）

**删（不是留开关，是删掉）**

| 项 | 位置 | 理由 |
|---|---|---|
| `FgMode` 的 consume/idle/local/bridge 语义 | `fg_provider.cpp` | 模式搬到 fakerFG；capture 不再有意见 |
| `Claim` / `FgCut` / `SlFgOff` / `FgSuppressNgx` | `in_fg.cpp`、`in_ngx.cpp`、`game_capture.cpp` | 打断是 fakerFG 的职责（并且现在它合法） |
| `MgpuCaptureFillInterpolated` | `game_capture.cpp` | 填别人的输出纹理 = 崩溃 + 黑屏，新架构不需要 |
| `fg_provider.*` / `fg_backend_xefg.cpp` / `fg_module_proxy.cpp` | `src/game_capture/` | 整体搬进 `src/faker_fg/` |
| `in_fg.cpp` 的拷贝与状态推导路径 | `src/game_capture/` | 由"读 fakerFG 的记录"替代，行数从 3245 降到预计 <300 |

**留（原样）**

- ReShade 事件、`shared_layout.h`、`mgpu_shared.h`（v14）、槽位放置、fence、`StreamPercent` 降采样、
  `streams:` 字节统计、`CAPTURE_RULES.md` 的 9 条、`in_ngx/in_ffx/in_xess`（老部署回退用）。

> **2026-11-14 更新**：这一节的 `in_faker.cpp` 已随 `src/game_capture/` 一起删除。
> 记录消费者现在是独立模块 `xess_mfg_producer.dll`（`src/xess_mfg_producer/`，
> fakerFG 加载，不再需要 ReShade），逻辑相同：sink 里用记录声明的状态录 barrier + 拷贝，
> present 时发布元数据并 signal fence。见 `docs/design/XESS_MFG_PRODUCER.md`。

**新增**

- `src/game_capture/in_faker.cpp`：`mgpu_fg_attach()` → 订阅 `MgpuFgApi::subscribe`（API v2）。
  记录在 fakerFG 的 `publish_record()` 里同步送达，此时 `frame->cmd_list` 仍打开，
  所以 sink 直接调用现成的 `MgpuCaptureRecordFgInputs()` 录 barrier + 拷贝；
  `poll()` 只把元数据（尺寸 / jitter / MV.Scale / 相机 / 剪辑矩阵 / frame key）交给 host
  发布。**已在 `unity.cpp` 注册，且排在 `g_sources` 第一位**；fakerFG 不存在时
  `poll()` 恒为 false，旧挂钩车道照常工作。

### 6.1 v2 为什么必须存在

轮询拿不到合法时机：`reshade_begin_effects` 时标题早已把 Evaluate 的 list 关掉，
而记录里的资源声明是 `ONLY_NOW`。轮询 + 之后再拷贝 = 在关闭的 list 上录命令，
或者在已被复用的资源上读。所以记录必须在**产生它的那一刻**交给消费者 ——
`sink` 在 `publish_record()` 里、锁外、Evaluate 线程上同步调用。

## 7. 一帧的时序（bridge 模式）

```text
游戏线程
  CreateFeature(DLSS_G) ──► fakerFG：分配自己的 handle（magic），记住 device
                            XeFG：D3D12CreateContext + InitFromSwapChain（在链空闲时）
  EvaluateFeature(list, params)
     │  fakerFG.provider
     ├─ harvest：params → MgpuFgFrame（MV/depth/hudless/UI/矩阵/jitter/render&output 尺寸）
     ├─ capture（同一线程，回调/主动 acquire）
     │     · 降采样 MV/depth（StreamPercent）
     │     · 记到 list 上：barrier(声明状态 → COPY_SOURCE) → CopyTextureRegion → barrier(回去)
     │     · 写槽位 + 发布 fence（协议 v14）
     ├─ backend：TagFrameResource(list, present_id, incomingState=我们声明的状态)
     │           XeFG 按 incomingState 自己 barrier，不需要第二份状态推导
     └─ 返回 Success（一次 Evaluate = 一个"帧对"）
  Present
     └─ fakerFG：SetPresentId 之后 XeFG 呈现（idle：只呈现实帧；local/bridge：呈现它生成的帧）
bridge.exe（另一块卡）
  等 fence → 读槽位 → XeFG 插帧 → 覆盖窗口上屏
```

**为什么这样不会黑屏**：游戏 presenter 看到的永远是 fakerFG 让它看到的纹理（`OutputReal` 或
XeFG 呈现的实帧），而不是"没人写过的 `OutputInterpolated`"。这是旧架构唯一做不到的事，
也是这次重构的全部理由。

## 8. 阶段计划（每一步都是可编译、可实测的树）

| # | 交付 | 验收（CP2077 4K DLAA 为基准） | 依赖 |
|---|---|---|---|
| **P0** ✅ | 本文 + `mgpu_fg_api.h` + `faker_fg.cpp` + `scripts/build_faker_fg.sh` | `cl` 编出 `build/fakerFG.dll`，导出表只有 `MgpuFgGetApi`；`frame_available` 恒为 0，capture 行为不变 | — |
| **P1** ✅ | `proxy_dxgi.cpp`：五导出转发 + 工厂 vtable 第 16 项改指（ReShade 式指针写），`InitFromSwapChainDesc` + `GetSwapChainPtr` 接管交换链 | `build/fakerFG.dll` 80 个导出（50 个 NGX + 20 个 DXGI + 桥/API）；未接管时原调用照常执行 | P0 |
| **P2** ✅ | `proxy_ngx.cpp`：`nvngx_dlssg.dll` 的 50 个导出（4 个有逻辑，其余通用 thunk 转发），`CreateFeature` 自己的 handle、`GetFeatureRequirements` 回答 supported | 导出表核对：`NVSDK_NGX_D3D12_EvaluateFeature` / `GetCapabilityParameters` 均在；`nvngx_dlssg_real.dll` 缺失时 provider 仍可独立工作 | P0 |
| **P3** ✅ | `provider.cpp` 的 harvest + 记录 + `publish_record`；`in_faker.cpp`（capture 侧消费，API v2 sink）**已接**；链路模式（`FakeDlssgFeature=0`）下由 `proxy_nvngx.cpp` 的 `publish_upscaler_record()` 发布 | `provider: eval #1 ... render=… mv=… depth=… hudless=…`（provider 模式）/ `record: upscaler lane (tagged, post-MvResample) …`（链路模式），随后 capture 日志 `faker: subscribed to the fakerFG record` + `input source: faker (api 9) … pinned` | P2 |
| **P4** ✅ | `backend_xefg.cpp`：XeFG 上下文、`SetEnabled(0)`、tag MV/depth、`SetPresentId`、`TagFrameConstants` | 日志 `xefg: the swap chain is ours now` + `generation OFF (idle/bridge)`；画面正确、无黑屏 | P1 |
| **P5** ✅ | 同一文件的 `Mode=local`（`SetEnabled(1)` + `SetNumInterpolatedFrames`） | `generation ON (local)`，游戏窗口自己出 2 倍帧 | P4 |
| **P6** 🟡 | `Mode=bridge`（游戏侧只呈现实帧，副卡出帧）——fakerFG 侧已就绪，capture 传输待 P3 收尾 | 渲染卡不掉帧、覆盖窗口 2x–4x | P3 |
| **P7** | 清理 capture 的 FG 介入代码与键、删 `src/fg_host/` | `FgMode/Claim/FgCut/SlFgOff/FgSuppressNgx` 不再被读取 | P6 |

P2 和 P3 是"风险归零点"：即使后面 XeFG 输出全线失败，**只做 P2+P3 也已经比今天强**
（不再有钩子嗅探、不再猜状态、输入是显式记录），所以这两步先行。

P4 是"所有权证明"：如果它能出正确画面，剩下的事都是增量。

## 9. 风险与未决问题

| # | 风险 / 未决 | 影响 | 处理 |
|---|---|---|---|
| 1 | NGX ABI 保真度：`PopulateParameters` 回调、`MultiFrameCount/Max`、`OutputReal` 语义 | 高：答错就黑屏或游戏崩 | 照 `Nvngx_FG.cpp` 搬；先只支持 2x（`MultiFrameCountMax=1`），多帧后加 |
| 2 | ReShade 与 XeFG 抢 present / 对象被 ReShade 包了一层 | 高：present 失败、tag 失败 | 用 `IID_UnwrappedObject` 取原对象；`fakerFG.ini` 留 `NoReshade=1` 做隔离实验 |
| 3 | `nvngx_dlssg.dll` 被查签名/版本 | 中：游戏拒载 | 版本资源写成 `310.9.16` 一类；不行就退回"早期 GetProcAddress 拦截"路线（不换文件） |
| 4 | XeFG 在 `CreateFeature`（帧中）时 `InitFromSwapChain` 未验证 | 中 | 推迟到第一帧 `Evaluate` 或第一次 `Present`；失败就 `Passthrough=1` 交回真运行时（**绝不黑屏**） |
| 5 | 40/30 系没有 DLSS-G 也要"看起来能开" | 低（我们自己回 Available） | 架构问答在 dxgi 代理里统一回 `sm_80+` |
| 6 | 反作弊 / 完整性校验 | 中 | 文档明说：仅供个人研究；不联网、不注入别人的进程 |
| 7 | 谁负责 `frame_qpc_ms` / `present_id` 的配对 | 低 | fakerFG 出 `present_id`，capture 透传；`frame_key` 语义不变 |
| 8 | 旧部署（无 fakerFG） | 低 | capture 回退到自己车道，`mgpu_fg_attach()` 返回 null 即老行为 |

## 10. 文档与仓库收口

| 文档 | 处理 |
|---|---|
| `FG_BACKEND.md` | 保留为"为什么不能当观察者"的论证；§9/§10 的端口计划**作废**，改为指向本文 |
| `DLSSG_PROVIDER.md` | 保留（sm86 调查与"后端即记录者"的思路，本文采用） |
| `PRODUCER_V14.md` | §7 的待办里第 0 项（DLSS-G 参数块作为源）由本文 P2 直接解决 |
| `CAPTURE_RULES.md` | 适用面扩到 fakerFG；规则 4 的对象从"游戏声明"升级为"我们声明" |
| `PRODUCER_SPLIT.md`（`src/fg_host/fg_host.cpp` 引用的） | 从未存在 → 本文就是它；P7 时把该引用改成 `FAKER_FG.md` |
| `docs/README.md` | 索引加一行（见下） |
| `src/fg_host/` | 临时保留（`build_fg_host.sh` 仍在用）；P7 删除，逻辑并入 `src/faker_fg/proxy_ngx.cpp` |

## 11. 在 CP2077 上验证（本机 5 分钟）

> 构建产物：`build/fakerFG.dll`（88 KB，80 个导出）。编译在 WSL 里通过
> `./scripts/build_faker_fg.sh` 完成并核对过导出表；**进程内的行为需要你在 Windows 上跑一次**。

### 11.1 部署

```bash
# 目标目录 = cyberpunk2077.exe 所在的 bin/x64
./scripts/deploy_faker_fg.sh --dry-run "/mnt/e/.../Cyberpunk 2077/bin/x64"   # 先看一遍
./scripts/deploy_faker_fg.sh          "/mnt/e/.../Cyberpunk 2077/bin/x64"
```

装完目录里应当是：

```text
dxgi.dll                = fakerFG
nvngx_dlssg.dll         = fakerFG（同一份文件）
nvngx_dlssg_real.dll    = 游戏原来的 DLSS-G
ReShade64.dll           = 原来的 ReShade（从 dxgi.dll 改名而来）
game_capture.addon64 / game_capture.ini / reshade-shaders/
fakerFG.ini             Mode=local（先跑 local，再看 idle/bridge）
```

**必须移走 `version.dll` 与 `dlssg_sm86/`**：它们同样占 `nvngx_dlssg.dll`，两个
provider 不能共存；架构问答现在由 fakerFG 自己回答（`ArchSpoof=1`）。

### 11.2 期望日志（`cyberpunk2077_fakerFG.log`）

```text
DllMain: attached as dxgi.dll, pid=…            ← 我们确实被当成 dxgi.dll 加载
loader: thread up
arch: answering 0x90 (was 0x86) so Streamline offers DLSS-G
reshade: loaded …\ReShade64.dll -> 0x…
dxgi: factory vtable captured (…), CreateSwapChainForHwnd @ …
xefg: libxess_fg …  / version 1.3.1.78 / maxSupportedInterpolations=1
xefg: D3D12InitFromSwapChainDesc -> OK (0)
xefg: D3D12GetSwapChainPtr -> OK (0), chain=0x…
xefg: the swap chain is ours now (3840x2160) - generation ON (local)
dxgi: handed the game XeFG's swap chain 0x… (3840x2160)
ngx: CreateFeature feature=11 3840x2160 mode=local owns_output=1
provider: handle slot 0, mode=local interpolated=1 - CreateFeature is ours
ngx proxy: this is the DLSS-G instance - the contract is handled by the dxgi instance
provider: eval #1 id=1 mode=local render=2560x1440 out=3840x2160 flags=0x1841 mv=… depth=…
provider: eval #600 …
xefg: last present: framesPresented=2 fgEnabled=1 result=OK (0)
```

（第二条 `ngx proxy: this is the DLSS-G instance` 只有在游戏真的把 `nvngx_dlssg.dll`
当独立模块加载时出现——出现是好事，说明两份实例的桥接生效了。）

### 11.3 判读表

| 症状 | 说明 | 下一步 |
|---|---|---|
| 没有 `cyberpunk2077_fakerFG.log` | 我们没被加载 | 游戏是不是从 `bin/x64` 启动；`dxgi.dll` 是不是我们（`strings dxgi.dll | grep MgpuFgGetApi`） |
| `loaded as dxgi.dll` 之后没有 `arch:` | nvapi 当时还不在进程里 | 看是否有 `nvapi not loaded yet`；把 `ArchSpoof=0` 再跑一次对照（此时菜单里不会有 FG） |
| `reshade: not found` | ReShade 没改名 | 把 ReShade 的 dxgi.dll 改名成 `ReShade64.dll` |
| `dxgi: swap chain left alone (effect=…)` | 不是 flip model | CP2077 应是 `FLIP_DISCARD`；若不是，检查全屏独占/无边框设置 |
| `XeFG could not take the swap chain` | libxess_fg 缺失或初始化被拒 | 把 `libxess_fg.dll`（+`libxell.dll`）放到游戏目录；日志上一行会写具体错误码 |
| `CreateFeature … owns_output=0` + 画面正常但没插帧 | 我们没接管输出，交回了真运行时 | 看前面的 `xefg:` 行定位原因（这是安全回退，不会黑屏） |
| 画面全黑 | **不应该发生** | 立刻 `Passthrough=1` 重开；把日志给我，按 §9 风险 4 处理 |
| `provider: eval #1` 有但 `mv=0x0` | FG 菜单开了但游戏没真正跑 FG | 确认游戏内「帧生成」是开着的；`declared_frames` 是否 ≥1 |

### 11.4 三档模式的验证顺序

1. `Mode=local`（默认）：游戏里开帧生成 → 游戏窗口自己应出 ~2 倍帧，`fgEnabled=1`，
   `framesPresented=2`。这一步验证「我们是 provider + XeFG 拥有 present」。
2. `Mode=idle`：同样的日志但 `generation OFF (idle)`，画面是原生帧率、**且不是黑屏**。
   这一步验证旧的 `FgMode=consume` 做不到的事。
3. `Mode=bridge`：游戏侧只呈现实帧；接上 capture 的传输（P3 的 `in_faker.cpp`）后，
   副卡出帧并由覆盖窗口上屏。

### 11.5 回退

```bash
./scripts/deploy_faker_fg.sh --uninstall "<game dir>"
```

或单键：`fakerFG.ini` 里 `Passthrough=1`（不回答、不接管，等于没装）。
