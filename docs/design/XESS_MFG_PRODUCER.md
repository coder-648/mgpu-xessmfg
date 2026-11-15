# xess_mfg_producer.dll —— 生产端的最终形态（已实现并实测）

> 状态：**已实现，2026-11-14 在 CP2077（4K，RTX 3080 Ti 渲染 + RX 6750 GRE 插帧）整链路跑通**。
> `src/xess_mfg_producer/producer.cpp`，`./scripts/build_producer.sh` → `build/xess_mfg_producer.dll`。
> 同一天，旧 `src/game_capture/`（ReShade add-on）**已删除**。
> `StreamPercent` 默认已改为 150（4K ≈ 28.1 MB/帧）；COPY-only 列表会自动回退 100。
> 需求原文：仓库根目录 `问题和描述.md` 的"接下来的任务"第 4 条。

## 0. 一句话

FG 链路已经是我们的（fakerFG 拥有 swap chain 与 present），采集侧不再需要 ReShade：
`xess_mfg_producer.dll` 由 fakerFG 加载，职责只剩"把记录搬过 PCIe"。

```text
游戏
 └─ dxgi.dll (= fakerFG)
      ├─ provider / harvest / XeFG backend
      ├─ MgpuFgGetApi() v2 (subscribe)
      └─ LoadLibrary("<exe dir>\\xess_mfg_producer.dll")
              ├─ sink: 在 Evaluate 的 open list 上录 barrier + CopyTextureRegion
              └─ present: frames[slot] + Signal(fwd fence)     (fakerFG 的 Present 路径)
                        ↓ PCIe
                 bridge.exe（副卡，XeFG 插帧 + 覆盖窗口）
```

## 0.1 实测（2026-11-14 07:15，CP2077 / 4K / Mode=local / FakeDlssgFeature=0）

```text
# fakerFG 日志
record: upscaler lane (tagged, post-MvResample) mv=… depth=… render=3840x2160 out=3840x2160 flags=0x2251
producer: …\xess_mfg_producer.dll -> start=0 (api v1)
api: capture sink subscribed

# producer 日志
producer: subscribed to the fakerFG record (api v2, record 72 bytes)
shared ready: name=Local\mgpu_mailbox_v1 pid=… used=270.1 MB slots=4 mv 3840x2160 fmt 34 depth 3840x2160 fmt 39
producer: first record seq=1 mode=3 render=3840x2160 out=3840x2160 flags=0x2251
[mv] copy 3840x2160 fmt 34 (declared state 0x40, list type 0)
[depth] copy 3840x2160 fmt 39 (declared state 0x40, list type 0)
publish #1 slot=0 id=1 mask=0x3 render=3840x2160 unit=2
shared: credit window ON - consumer is ACKing

# bridge
frame 81  slot 2  jitter=(0.381, 0.345)  (28.0 fps avg) presented=2 fg=1 SUCCESS
bus: mv 3840x2160 31.6MB + depth 3840x2160 31.6MB = 63.3MB/frame (2.6 GB/s at 41.0 fps)
```

ReShade 与 add-on 均未加载（`InjectReshade=0`，目录里没有 add-on）。

## 0.1.1 两种运行形态（2026-11-14 实测）

| `fakerFG.ini` | 游戏自己的 DLSS-G | 游戏侧出帧 | 谁补帧 | 用途 |
|---|---|---|---|---|
| `Mode=local` | **被拒**（不转发真运行时） | XeFG 2X（31.9 ms/帧 ≈ 31 fps） | 渲染卡 | **自检平台**：整条 FG 由我们跑完 |
| `Mode=consume` | **被拒** | 1X 原生（21.6 ms/帧 ≈ 46 fps，无补帧开销） | **副卡 bridge** | 双卡出货 |

`local` 与 `consume` 都要求 `FakeDlssgFeature=0`（链路：标题关掉自己的 FG，把整帧渲进我们交
出去的链）。`consume` 下 fakerFG 只做三件事：拿全输入 → 发布记录 → 不做任何补帧，原生帧直接上屏。

> 修过的坑：`fg_create_impl()` 原先在拒绝 DLSS-G 契约时会**转发**给 `nvngx_dlssg_real.dll`，
> 于是 `local` 里游戏自己的 FG 仍在出帧（与 XeFG 并行）；`consume` 里则会白烧渲染卡。现在只有
> `off` / `observe` / `Passthrough=1` 允许转发。

## 0.1.2 测量开关：`Publish`

`fakerFG.ini`：

```ini
Publish=1     ; 默认：正常发布（记录 → sink → 传输）
Publish=0     ; 测量基线：publish_record() 直接返回
              ;   → 不拍快照、不加锁、不调 sink；producer 收不到任何帧
```

- `NoPublish=1` 是旧的负向写法，仍被接受；两者同时存在时 **`Publish` 优先**。
- 只影响发布路径，不改变 FG 链路（`Mode=local` 仍然自己出 2X）。
- 配置只在启动时读一次（规则 7），切换要重开游戏。
- 单独测"发布订阅"这个形式本身：560 字节记录、临界区加解锁、一次间接调用，
  MSVC `/O2` 实机微基准 **22 ns/次**（2,000,000 次平均）。其余开销来自
  `barrier + CopyTextureRegion` 的录制与 fence `Signal`，属于传输本身。

## 0.1.3 StreamPercent（降采样，2026-11-14 晚实现）

`fakerFG.ini`：`StreamPercent=150`（默认，≈2/3，4K→2560x1440）/ `100`（1:1）/ `200`（1/2）/
`400`（1/4）。producer 用一次 compute pass 把 **MV 和 depth 缩到同一个目标尺寸**，再拷进槽位；
两条流必须同尺寸（XeFG 会对 `MISMATCH_INPUT_RESOURCES` 拒绝）。

| 4K 渲染 | 流尺寸 | 每帧字节 | 拷贝代价（≈13–25 GB/s） |
|---|---|---|---|
| 100 | 3840x2160 | 63.3 MB | 2.5–4.8 ms |
| **150** | **2560x1440** | **28.1 MB** | **~1.2–2.2 ms** |
| 200 | 1920x1080 | 15.8 MB | ~0.7–1.2 ms |

实现要点（都是实测踩出来的）：

1. **深度不能用自己的格式做 SRV**：`D32_FLOAT` 当 SRV 是 `DXGI_ERROR_INVALID_CALL` 并直接
   device removal。SRV 用 `R32_FLOAT`（D32/R32 家族的文档格式），缩放目标也用 `R32_FLOAT`；
   槽位仍是 `R32_TYPELESS`，`R32_FLOAT → R32_TYPELESS` 的拷贝合法。
2. **UAV 不能是深度格式**：目标纹理若沿用 `D32_FLOAT` 建，Dispatch 时掉设备。
3. **根描述符不能绑纹理**：`SetComputeRootShaderResourceView/UnorderedAccessView` 只接受
   raw/structured buffer；纹理必须走描述符表，并且 `SetDescriptorHeaps` 先绑定我们的小堆。
4. **Dispatch 需要 DIRECT/COMPUTE 列表**：COPY 列表（UE5 的 tag 列表）首次遇到时自动回退到
   `StreamPercent=100`（重写 content 矩形 + generation），之后按 1:1 拷贝；日志会说明。
5. 缩放必须发生在**标题仍然打开的那条命令表**上（与拷贝同一时机），所以它天然落在
   fakerFG 的 Evaluate 里；任何 `Dispatch` 失败都跳过整帧，绝不发送"一条缩过、一条没缩"的输入。

实测（CP2077 4K，`Mode=consume`，bridge 运行）：`downscale: mv/depth 3840x2160 -> 2560x1440`、
`[depth] copy 2560x1440 fmt 41`、bridge `bus: 28.1MB/frame`、`presented=2 fg=1 SUCCESS`，
连续运行无崩溃。

## 0.2 实现与历史设计的差异（已知）

| 项 | 旧 add-on | 现在 |
|---|---|---|
| 跨卡流量 | `StreamHeight/StreamPercent` 可把 4K 压到 1440p（28 MB/帧） | **已搬进 producer**：`StreamPercent` 默认 150（4K → 2560×1440 ≈ 28.1 MB/帧）；COPY-only 列表自动回退 100 |
| colour / UI 流 | 可选（默认关，颜色走 WGC） | 不做（颜色走 WGC） |
| MV 归一化 / 翻转 | `MvNormalize` / `MvFlip` pass | 不做；声明原样发布（semantics 位给出单位/符号） |
| depth 转换 pass | 需要时才有 | 不做：D32/R32 家族直接拷进 R32_TYPELESS 槽 |
| 在线 probe / 统计 | 有 | 无（只保留每帧字节统计所需的 `stream_bytes`） |
| 槽位信用 / ACK | 有 | 相同（v10 语义） |


## 1. 为什么值得做

| 现状 | 代价 |
|---|---|
| 采集端是 ReShade add-on | 必须部署 ReShade + `reshade-shaders/`；ReShade 的 device/queue/list 包装层始终在场 |
| addon 自带 `fg_provider.cpp` / `fg_backend_xefg.cpp` / `fg_module_proxy.cpp` | 与 fakerFG **抢同一个 DLSS-G 契约和同一条 XeFG 链**（PRAGMATA 是同时在场的样本） |
| 状态来自 ReShade 的 barrier 事件 | 现在还多一条更可靠的路：记录的 `state` 是 fakerFG **自己声明的**（`CAPTURE_RULES` 规则 4 的升级版） |
| 发布点在 `reshade_begin_effects` | fakerFG 拥有 present，发布点可以放在自己的 present 路径里，时序更确定 |

一句话：ReShade 曾经解决的两个问题（**何时拿到合法的数据**、**何时安全地提交拷贝**）
现在分别由 *记录 + sink* 和 *我们自己的 swap chain / present* 解决了。

## 2. 模块边界

| 模块 | 形态 | 负责 | 不负责 |
|---|---|---|---|
| `fakerFG.dll` | 代理（dxgi / nvngx_dlssg） | FG 契约、harvest、XeFG 输出、present 所有权、加载 producer | 跨卡传输、共享内存布局 |
| **`xess_mfg_producer.dll`** | 普通 DLL，fakerFG 加载 | 订阅记录、barrier + 拷贝到共享槽、降采样（`StreamPercent`）、写 `mgpu_shared.h` v14 头、fence 发布 | 任何 FG 决策、任何对标题资源的解释 |
| `bridge.exe` | 独立进程 | 读槽 → XeFG 插帧 → 覆盖窗口上屏 | 抓 FG 输入（不再需要任何钩子） |

producer 不导出 ReShade 接口，也不钩任何函数：它只被 fakerFG 加载，只通过
`MgpuFgGetApi` 拿数据。

## 3. 从 `game_capture` 搬什么（一次性、可验证）

| 搬 | 来源 | 说明 |
|---|---|---|
| 共享块创建 | `ensure_shared()` / `reserve_bytes()` | 命名 file mapping → `OpenExistingHeapFromFileMapping` → placed resources + 共享 fence |
| 槽位布局 | `shared_layout.h`（`StreamSpec` / `stream_specs` / `layout_streams` / `maybe_update_content_size`） | 原样搬；v9 "槽位不移动、只改 content 矩形"的规则不变 |
| 槽位信用 | `claim_publish_slot()` / `consumed[]` ACK | 原样搬 |
| 拷贝录制 | `MgpuCaptureRecordFgInputs()` 的 `copy_stream` 部分 | 简化：**不再需要 list-type 分支**（见 §4），保留格式/尺寸检查 |
| 发布 | `on_reshade_present` 里的 `frames[slot]` + `Signal(fwd)` | 移到 fakerFG 的 present 路径，或 producer 自己的提交点 |
| 字节统计 | `note_stream_copy()` | 保留，日志格式不变 |

**不搬**：colour/UI 流（bridge 用 WGC）、`in_*` 四条车道、`fg_*` 三个 FG 文件、
`StreamlineCamera` / `Claim` / `FgCut` / `SuppressNgx` 全部开关。

## 4. 两个关键简化

1. **不再需要 ReShade 的 barrier 事件。** 记录的 `state` 是 fakerFG 声明的（它就是把资源
   交给 XeFG 的那条 pass），producer 直接用它做 transition；`ONLY_NOW` 的资源在 sink 里当刻
   复制，不存在"稍后再看"的问题。
2. **不再需要 list-type 分支。** 命令列表是我们自己的（或 fakerFG 转交的 DIRECT 列表），
   不是标题的 COPY 列表，所以 barrier + 拷贝永远合法；`CAPTURE_RULES` 规则 9 仍然遵守，
   但不再有"因为列表是 COPY 所以深度传不了"这一档。

## 5. 分阶段

| # | 交付 | 验收 |
|---|---|---|
| P1 | producer 骨架：DLL + `MgpuFgGetApi` 订阅 + 共享块/槽位 | 日志 `producer: shared ready …`，bridge 能打开 mapping（`api=fakerFG`），没有拷贝 |
| P2 | 拷贝：MV/depth 1:1 进槽 + fence 发布 | `bridge` 侧画面 2X；producer 日志的 MB/帧与 `StreamPercent` 一致 |
| P3 | fakerFG 侧加载 + present 侧发布 | 关掉 ReShade（目录里没有 `ReShade64.dll` / addon）仍有数据 |
| P4 | 删除 addon 的 FG 文件与 `in_fg.cpp` 拷贝路径 | `grep MgpuFgGetApi src/game_capture` 只剩 `in_faker.cpp`（或一并删除） |

## 6. 风险 / 未决

| # | 风险 | 处理 |
|---|---|---|
| 1 | 没有 ReShade 就没人替我们 `AddRef` / 延迟释放标题资源 | producer 自己 `AddRef` + 帧计数延迟释放（现成的 `hold_resource` 逻辑） |
| 2 | fakerFG 崩溃会带走游戏 | 与 OptiScaler 同风险；producer 里不做任何标题决策，崩溃面比 addon 小 |
| 3 | `StreamPercent` 降采样需要 compute/graphics pass | 只允许在我们自己的 DIRECT 列表上做；COPY 列表首次遇到时自动回退到 100 并记日志 |
| 4 | 不同标题的 present 路径不同（UE5 / RE / RED4） | 发布点选在 fakerFG 的 `xefg_before_present` / `xefg_present_result`，那是所有标题共享的收口 |
| 5 | 老部署（无 producer） | `LoadLibrary` 失败只记一行，fakerFG 其余功能不受影响 |
