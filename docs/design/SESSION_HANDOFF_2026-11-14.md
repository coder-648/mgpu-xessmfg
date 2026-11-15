# 会话交接（2026-11-14）：XeFG 链路标准 + capture 重构起点

> 下一轮对话的入口文档。链路的权威说明见 `docs/design/XEFG_CHAIN_STANDARD.md`，
> 本轮全部推演与实测见 `docs/design/FAKER_FG_PORT.md` §18–§27。

---

## 1. 本轮成果（全部有实测日志）

**Wuchang 的 XeFG 链路跑通**：画面正常、2X 平滑、无重影、无回拷、无双层。

成立的四条件（缺一不可）：

| 条件 | 缺了会怎样 |
|---|---|
| `RealSwapchainObject=1`（交还 libxess 真链 + vtable 钩子） | 门面对象让标题渲染到别处，链里只有 UI → 黑屏 |
| `FakeDlssgFeature=0`（不伪造 DLSS-G 特征） | 标题进入 FG 契约（只画 UI、整帧写 `OutputReal`）→ 黑屏 + 菜单 x2 假计数 |
| `MvResample=1`（真的把 MV 重采样到 depth 尺寸） | 全分辨率 MV + 缩放时 `Activate()` 拒绝；MV/depth 资源尺寸不等时库报 `-12` |
| `ForceBorderless=1` | libxess 判全屏 → `Interpolation is not supported in fullscreen mode`，`SetEnabled` 仍返回 SUCCESS |
| `TagAtEvaluate=1` | tag 走 present 路径 ⇒ `MvResample` 永不执行 ⇒ 尺寸不齐（PBZ 第一轮就是这个） |

### 通用设置（`packaging/reshade_pack/fakerFG.ini` 已按此配置）

```ini
Mode=local / Interpolated=1
OwnSwapchain=1
RealSwapchainObject=1
FakeDlssgFeature=0
WriteOutputReal=0
MvResample=1
TagAtEvaluate=1
ForceBorderless=1
FgInput=Upscaler / FgOutput=XeFG
```

### 实测矩阵

| 游戏 | 车道 | 门控通过原因 | MV 处理 | 结果 |
|---|---|---|---|---|
| Wuchang | NGX DLSS-G 契约 | `lowResMV=1` | 3840x2160 → 2560x1440 | 2X 平滑 ✓ |
| PBZ | Streamline (`sl.dlss_g`) | `lowResMV=1` | 2562x1441 → 2564x1444 | 2X ✓ |
| Khazan | Streamline + DLSS | `nativeAA=1`（render==display） | 不需要（已同尺寸） | 2X ✓，零错误 |
| CP2077 | Streamline + DLSS | `lowResMV=1` + nativeAA | 不需要 | 回归 PASS（8/8） |
| PRAGMATA | RE Engine | 未测（已部署，待跑） | — | — |

---

## 2. 死路清单（别重走）

| 做法 | 为什么死 |
|---|---|
| 把 `DLSSG.Backbuffer` 回拷进链缓冲 | 真链一上就不再需要；它曾造成双层与双 Present 错配（已删除代码） |
| 在 DLSS-G Evaluate 里做那个回拷 | 标题之后还写链，逐帧覆盖 → 闪烁 |
| `MvFromTarget`（把 MV 声明成 render 尺寸 + 缩放向量值） | 库按声明尺寸采样 ⇒ 速度场错位 ⇒ 拖影（已删除） |
| 拒绝 DLSS-G 特征（`FakeDlssgFeature=0`）后期待标题自己合成 | 标题只是不进 FG 契约，画面路径不受影响；但**输入也一并没了**，只适合"链路模式"（标题自己渲染整帧） |
| sm86（`dlssg_sm86`, `Engine=XeSS`） | 用户明确排除：自研 FG 的目的是双卡 XeSS-FG，需要完全掌管输入/状态 |
| MinHook 传 `nullptr` 当 trampoline | detour 递归 → `0xC00000FD`，rip 在 `dxgi.dll+0xB4EA`（已修） |

---

## 3. 部署状态与回退

| 游戏 | 路径 | 部署内容 |
|---|---|---|
| Wuchang | `D:\SteamLibrary\...\Wuchang Fallen Feathers\Project_Plague\Binaries\Win64` | `dxgi.dll` + `fakerFG/streamline/nvngx_dlssg.dll` + `fakerFG.ini` |
| CP2077 | `E:\SteamLibrary\...\Cyberpunk 2077\bin\x64` | 同上三处（`dxgi.dll`、`nvngx_dlssg.dll`、`fakerFG/streamline/nvngx_dlssg.dll`） |
| PBZ | `F:\steam\...\PBZ\platform\steam` | `dxgi.dll` + `nvngx_dlssg.dll` + `fakerFG.ini` + capture |
| Khazan | `F:\The First Berserker Khazan\BBQ\Binaries\Win64` | 同上；历史残留移到 `_fakerfg_test_backup/` |
| PRAGMATA | `E:\PRAGMATA` | 同上；ReShade 改为 `ReShade64.dll`，sm86 + dlssg-to-fsr3 移到 `_fakerfg_test_backup/` |

回退：`./scripts/deploy_faker_fg.sh --uninstall "<game dir>"`，或按上表手工删 `dxgi.dll`/`nvngx_dlssg.dll` 并把 `nvngx_dlssg_real.dll` 改回原名。

---

## 4. 代码地图（`src/faker_fg/`，已验证的这一套）

| 文件 | 行数 | 职责 |
|---|---|---|
| `backend_xefg.cpp` | 1475 | 建链、tag、present 派发、门控（`xefg_frame_feedback`）、`MvResample` 调用点（`xefg_tag_at_evaluate`） |
| `provider.cpp` | 464 | DLSS-G 契约：Create/Evaluate/Release、`FakeDlssgFeature`、`WriteOutputReal`、输入采集（`MgpuFgFrame`） |
| `fg_swapchain_hooks.cpp` | 373 | 真链 vtable 钩子（Present 8 / SFState 10 / GFState 11 / RB 13 / RT 14 / GFDesc 19 / P1 22 / Waitable 33 / RB1 39）+ borderless + keep_flags |
| `passes.cpp` | 506 | OptiScaler 三个合成 pass（`FGDrawUIOverFG`/`FGHudCutoff`/`FGHudlessCompare`，默认关） |
| `mv_resample.cpp` + `shaders/mv_resample.hlsl` | 236 | MV → depth 尺寸；构建期 fxc 编译 + `scripts/embed_cso.py` 内嵌 |
| `proxy_dxgi.cpp` | — | 工厂钩子、接管判定（`OwnSwapchain`）、`xefg_start()` |
| `proxy_streamline.cpp` | — | Streamline 代理 + `slSetTag` 输入车道 |
| `proxy_nvngx.cpp` | — | NGX GetProcAddress 层 + 上采样器车道采集 |
| `mgpu_fg_api.h` | — | 发布给 capture 的 API（v1） |

---

## 5. capture 侧：旧实现已删除（2026-11-14 晚）

`src/game_capture/`（ReShade add-on，约 1.2 万行：FG provider、XeFG 后端、四条输入车道、
模块代理、传输）**整体删除**。替代物是 `src/xess_mfg_producer/`（约 700 行）：

| 项 | 旧 add-on | 现在 |
|---|---|---|
| 加载方式 | ReShade 6.8 + `game_capture.addon64` + 启用一个 effect | fakerFG 的 loader 线程 `LoadLibrary` |
| 数据来源 | 钩 NGX/FFX/XeSS/Streamline，自己猜 lane | `MgpuFgApi::subscribe` 的显式记录 |
| 拷贝时机 | `reshade_begin_effects`（list 已关闭）| sink 内部，标题的命令表仍打开 |
| 发布时机 | `addon_event::reshade_present` | fakerFG 的 Present detour（`xefg_before_present`）|
| FG 介入 | Claim/FgCut/SuppressNgx/占位输出 | 无（transport only）|
| 状态来源 | ReShade barrier 事件 | 记录里的声明状态 |

接口：`src/mgpu_producer_api.h`（`MgpuProducerGetApi` / `start(fg_api)` / `present(queue)`）。
CP2077 实测（4K，4K 渲染）：`shared ready 270.1 MB`、`[mv]/[depth] copy 3840x2160`、
`publish #1`、bridge `fg=1 SUCCESS`、`bus 63.3 MB/frame`。详见
`docs/design/XESS_MFG_PRODUCER.md` §0.1。

**模式语义（2026-11-14 追加）**：`Mode=local` = 自检平台（fakerFG 在本卡跑完整 FG，游戏自己的
DLSS-G 被拒）；`Mode=consume` = 双卡（拿全输入 → 拒绝补帧 → 原生帧上屏，副卡 bridge 出帧）。
关键修复：`fg_create_impl()` 原来在拒绝 DLSS-G 契约时转发给 `nvngx_dlssg_real.dll`，导致
`local` 里游戏 FG 与 XeFG 并行、`consume` 里白烧渲染卡；现在只有 `off`/`observe`/`Passthrough=1`
允许转发。实测：local 31.9 ms/帧(fgEnabled=1)，consume 21.6 ms/帧(fgEnabled=0)+bridge `fg=1 SUCCESS`。

**仍然要补的**（按价值排序）：

1. `StreamPercent` / `StreamHeight` 降采样（现在固定 1:1，4K ≈ 63 MB/帧）；
2. 1440p 渲染 / DLSS 档位切换时的尺寸跟随（`update_content` 只在容量内生效）；
3. 首帧 `present` 若发生在第一条记录之前（菜单），producer 不发布——预期行为，但日志应只说一次。

## 6. 未解决 / 待观察

* PBZ 的 `present` 统计里 `long` 帧偏多（10–20 每 120 帧，max 236 ms），未定位
  （候选：`mvr` dispatch、首帧 PSO/纹理创建、游戏流式加载）；
* PRAGMATA 未实测（RE Engine 的加载点可能与我们钩的 `dxgi.dll` 不同）；
* `docs/design/FAKER_FG_PORT.md` §18–§27 里的历史实验记录保持原样（作为"为什么不能那样做"的证据）。
