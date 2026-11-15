# 测试环境：Cyberpunk 2077（唯一测试床，2026-11-12 起为 ReShade 栈）

```text
游戏：E:\SteamLibrary\steamapps\common\Cyberpunk 2077\bin\x64\Cyberpunk2077.exe
显卡：RTX 3080 Ti（sm86）+ RX 6750 GRE（副卡）
```

## 1. 现在这套栈里有什么

| 文件 | 作用 |
|---|---|
| `dxgi.dll`（ReShade 6.8.0） | 注入器 + 设备/队列/命令表包装，add-on 的数据视图来自它 |
| `game_capture.addon64` | **生产端**（本项目）。只采 MV/depth（+相机等元数据），颜色交给消费端 WGC |
| `game_capture.ini` | 生产端配置，39 个键都有说明；启动时读一次 |
| `version.dll` + `dlssg_sm86\` + `dlssg_sm86.ini` | 第三方解锁器：让 30 系能开 DLSS-G，同时它替 Streamline 回答架构问题 |

一个文件一个代理：**ReShade = `dxgi.dll`，sm86 = `version.dll`**。sm86 自带的
`alternatives\dxgi.dll` 不能用（会顶掉 ReShade，add-on 直接不加载）。

历史（自有 `version.dll` 宿主）已退役，代码在 `src/legacy/`，见
`docs/design/PRODUCER_V14.md`。

## 2. 采集配置（`game_capture.ini`）

```ini
[capture]
Log=1
Color=0               ; 颜色走消费端 WGC；=1 是每帧多 56 MB 跨卡写，实测约 -2 fps
Input=auto
SlInput=1             ; Streamline 的 constants/tags 作为输入源（MV/depth/camera/jitter）
StreamlineCamera=1
FgSniff=1             ; 只读诊断：把游戏自己的 FG 声明打进日志
FgNgxHooks=auto       ; 钩 DLSS-G 导出，但跳过自称解锁器的模块（dlssg_sm86）
FgCut=0               ; 打断开关（见 §5），默认不动游戏的 FG
FgProvide=0           ; 解锁交给 sm86，本 add-on 不冒充
SkipMv=0
SkipDepth=0
DepthDirect=1         ; 深度直读 SRV
StreamPercent=200     ; MV/depth 走 1/2（4K 下每条 7.91 MB）
MvNormalize=0
LoaderHook=1
```

**解锁 → 打断 → 获取** 这条链在现在这套栈里的对应关系：

```text
解锁：version.dll（dlssg_sm86）→ 游戏菜单里出现 Frame Generation，且架构问题由它回答
打断：FgCut（bit1 FSR-FG / bit2 Streamline DLSS-G / bit4 NGX DLSS-G），默认 0 不动游戏
获取：SlInput + slSetTag → MV/depth 在 tag 给出的命令表上原地拷贝
```

## 3. 操作顺序

```text
1) 启动游戏，画面设置：DLSS 超分 开（本项目的数字都在 DLAA 下）、**帧生成 开**
   （FgMode=consume 要求游戏 FG 开着：声明才完整，生成由我们打断）
2) 进入世界（站在有运动的场景，最好开车）
3) 运行 C:\temp\mgpu\bin\run_cp2077.bat
   → bridge.exe --game Cyberpunk2077.exe --den 1 --pool 4 --interp 1
                --streams=mv,depth --link=13 --fg-ui none
```

三种场景的配置对应（`game_capture.ini`）：

| 场景 | 游戏菜单里的 FG | FgMode | 谁在生成 | 预期 fps（4K DLAA） |
|---|---|---|---|---|
| 双卡管线（默认） | 开 | `consume` | 副卡 bridge | 渲染卡回到原生水平，副卡出 2x |
| 纯数据调查 | 开 | `observe` | 游戏自己 | 48（生成开销在渲染卡上） |
| 游戏 FG 关掉 | 关 | `off` | 副卡 bridge | 与 consume 接近，但少 FG 语义 |

`consume` 的关键日志（证明"声明还在、生成没了"）：

```text
fg: generation INTERRUPTED at slEvaluateFeature(DLSS_G) - the declaration stays complete,
    the second GPU produces the frames (FgMode=consume)
fg: sl.eval recorded streams mask=0x3        ← 打断前已经记录完拷贝
```


## 4. 日志判据

`bin\x64\game_capture.log`（`Log=1` 才有）

```text
=== game_capture … build <编译时间> ===                 ← 确认跑的是哪个二进制
AddonInit (addon=… reshade=0x…)                          ← reshade 非空 = 真注入了
config: Color=0 … StreamPercent=200 SkipMv=0 SkipDepth=0 FgSniff=1 Input=auto
fg sniffer: DLSS-G …\dlssg_sm86\runtime\nvngx_dlssg.dll - NOT hooked (auto: …)
                                                         ← 解锁器模块必须"不钩"
input source: sl (api 5) - pinned for this process
semantics: mv=uv sign=prev-cur camera=matrix depth=reversed valid=3840x2160
shared: producer … render=1920x1080 (source: StreamPercent …) output=3840x2160
streams: 300 frames, copies … | one copy: mv=7.91 depth=7.91 MB | total ~15.8 MB/frame
pending: N calls, P parked, C copied, skip(slot=? state=? fmt=?)
```

判读顺序：**先看 `NOT hooked` 与 `reshade=` 非空**（注入与解锁器共存），再看
`input source`（tag 通道有没有活），最后看 `one copy:` 的实测字节（1/2 档应恒为
7.91 / 7.91 MB）。`skip(slot=…)` 持续增长而 `copied` 不掉，说明是槽位被消费端占着，
不是丢数据。

`dlssg_sm86\logs\dlssg_sm86.log`

```text
{"event":"fg_gate_create_feature", … "status":"0xbad00xxx","succeeded":false}
```

出现这一类行说明解锁器拒绝了建 feature —— 如果 `caller.module` 是 `game_capture.addon64`，
就是我们的 DLSS-G 钩子和解锁器打架（`FgNgxHooks` 应为 `auto`）。

`C:\temp\mgpu\bin\bridge_cp2077.log`

```text
[bridge] MV 1920x1080, depth 1920x1080 (stream size)
[bridge] streams -> mv=1 depth=1 …                        （消费者侧确认）
[bridge] producer api=sl / projection: from the game's own view-to-clip
[streams] / OSD: in 15MB … 40%                            （实测带宽与占链路百分比）
```


## 5. 已知不确定点（第一次跑要确认的）

```text
① 解锁是否成功：游戏里能否勾上 Frame Generation
   - 若不能：把 version.dll.sm86 放回（改名覆盖 version.dll），
     此时由 dlssg_sm86 负责解锁，但我们的 Host 加载会失效；
     需要的话把 Host 加载做进 sm86 的代理，或改用它的 LoadLibrary 重定向。
② 打断是否彻底：FgCut=6 后游戏应 Present 1x（帧率等于真实帧率），
   但 hudless/MV/depth 仍持续更新（capture 日志里帧号在涨）
③ 数据是否全：MV 尺寸（CP2077 DLSS-G 是输出分辨率还是渲染分辨率）、
   depth 格式、camera 矩阵是否来自 sl::Constants
```
