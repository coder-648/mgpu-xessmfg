# PBZ：sm86 DLSSG 后端复用评估与 DLSSG 输入车道测试

## 结论

`third_party/dlssg_for_sm86-main/dlssg_sm86/backend/sm86_backend.dll` **不是可独立调用的
DLSSG 后端**：

- 它由 sm86 的 `version.dll` 代理加载；
- 它按 `DlssgMod_GetInfo` / `DlssgMod_Install` 的私有 ABI，在 `nvngx_dlssg.dll` 内部
  安装 CU-module 路由和内核补丁；
- 它没有逐帧输入回调。`DlssgMod_ConfigureCapture` 是写盘抓取，不是实时数据面；
  `SM86Bridge_*` 是它自己的测试/统计接口；
- 它和 fakerFG 都会修改同一段 NGX/交换链钩子，旧实验已证明两套内联钩子会互相破坏
  （黑屏或不出帧）。

因此**不建议把 sm86 后端直接塞进 fakerFG 的帧生成管线**。sm86 值得复用的是它看到的
**输入契约**：`NVSDK_NGX_DLSSG` 参数块里的
`MVecs / Depth / HUDLess / UI / Backbuffer / OutputReal / camera / jitter / mvScale`。

fakerFG 自己作为 DLSS-G provider 时，`provider_evaluate()` 的 `harvest()` 读到的就是
同一份参数。下面的测试就是让 fakerFG 走这条 DLSSG 输入车道，与当前 PBZ 的
`FgInput=Upscaler` 做 A/B。

## 测试配置

`lock_frame/pbz_dlssg_lane.ini` 是基于当前 PBZ `fakerFG.ini` 生成的测试配置，关键差异：

```ini
Passthrough=0             ; 不转发真实 runtime；由 FgInput/FgOutput 决定
OwnSwapchain=1
RealSwapchainObject=1
FakeDlssgFeature=1        ; 让游戏进入 DLSS-G 契约，吐出完整参数块
WriteOutputReal=1         ; 用 DLSSG.Backbuffer 填 OutputReal，保证画面
FgInput=DLSSG             ; 输入取 DLSSG 参数块，而不是 DLSS-SR 的 Upscaler 参数块
FgOutput=Capture          ; 本次只测输入/传输，不在游戏内跑 XeFG
TagAtEvaluate=0
FakeSlDlssg=1
DepthInverted=1
MvResample=1
```

`Passthrough=0` + `FgOutput=Capture` 的目的：不生成插值帧，但保持 DLSS-G 契约成立；如果 PBZ 在这个配置下仍
只画 UI，先看日志里的 `provider: OutputReal <- DLSSG.Backbuffer` 是否出现。

## PBZ 步骤

1. 备份当前配置：
   ```
   copy F:\steam\steamapps\common\PBZ\platform\steam\fakerFG.ini
        F:\steam\steamapps\common\PBZ\platform\steam\fakerFG.ini.before_dlssg_lane
   ```
2. 用仓库里的 `lock_frame/pbz_dlssg_lane.ini` 覆盖游戏目录的 `fakerFG.ini`。
3. 启动 PBZ，进入游戏画面；打开 DLSS Super Resolution，分辨率与桌面一致。
4. 游戏内打开 **DLSS Frame Generation**（这次需要它进入 DLSS-G 契约）。
5. 运行 `lock_frame\run_ui.bat`，选 30x4 或 40x3，启动。
6. 观察：
   - `PBZ_fakerFG.log` 是否出现：
     ```
     provider: eval #1 ... mv=... depth=... hudless=... ui=...
     provider: OutputReal <- DLSSG.Backbuffer (3840x2160), contract satisfied
     ```
   - `PBZ_mfg_producer.log` 的 `publish #...` 中 `mv=... depth=...` 尺寸/格式；
   - consumer `pair: exact=40 no_slot=0`，以及画面是否有黑屏/重影。
7. 测完恢复：
   ```
   copy ...\fakerFG.ini.before_dlssg_lane ...\fakerFG.ini
   ```

## 预期与判定

| 日志 | 含义 |
|---|---|
| `provider: eval ... mv/... depth/... hudless/... ui/...` | 已拿到完整 DLSSG 输入（sm86 同款数据面） |
| `provider: OutputReal <- DLSSG.Backbuffer` | DLSS-G 契约的真实帧已填，画面应正常 |
| 没有 `provider: eval` | 游戏没有创建 DLSS-G feature，检查游戏内 FG 是否真的打开 |
| `pair` 正常但画面黑 | `FgOutput=Capture` 的呈现路径不成立，改用 `FgOutput=XeFG` 再试 |
| `pair` 的比例/格式优于当前 Upscaler 车道 | 可以把 DLSSG 车道作为 PBZ 的新默认输入 |

## 备注

如果之后确认要“让 sm86 本体跑 DLSSG、fakerFG 只做被动采集”，需要另外做一个
**passive capture 模式**：fakerFG 不接管 DLSS-G provider，只钩 Streamline
`slSetTag/slEvaluateFeature`，并且必须移除 fakerFG 的 `nvngx_dlssg.dll` 别名，
让 sm86 的 `version.dll` 加载它自己的 runtime。那是第二阶段，不是本次这个配置。
