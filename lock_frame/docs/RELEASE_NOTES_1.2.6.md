# lock_frame 1.2.6 发布说明（本地版）

**1.2.6 只做 PBZ（`F:\steam\...\PBZ.exe`）双卡 FG 的两个真实 bug 修复**，协议与
传输格式不变（仍是 v30），API 仍是 v2，`fakerFG.ini` 默认值不变——因此可以和
1.2.5 的消费端混跑，但**同包内的 `fakerFG.dll` 与 `xess_mfg_producer.dll` 必须成对升级**
（新增了一个只在两者之间使用的 record 标志位）。

## 1. 崩溃：在游戏已经关闭的命令列表上记录 Dispatch

现象（`PBZ_fakerFG.log`，两次会话同形，都发生在 boss 战结束→过场动画的瞬间）：

```text
sl: frame token 19387/19389/19390 already published - present-staged duplicate collapsed
crash: first-chance code=0xC0000005 at ... nvwgf2umx.dll+0x6DB77C
crash:   stack ... D3D12Core.dll+0x2A1EAD / +0x151494
crash:   stack 00007FF9813B4570 = xess_mfg_producer.dll+0x4570
```

`producer+0x4570` 用 `.pdata` 反查落在 `fr_encode_dispatch` 内，正好是
`SetComputeRootDescriptorTable(2, ...)` 的返回地址——调用链 `producer → D3D12Core → nvwgf2umx`。

根因：PBZ 从不调用 `slEvaluateFeature(DLSS_G)`（整场 `eval token=0`），所有记录来自
`slPCLSetMarker(ePresentStart)` 的 present-staged 兜底；那条路径交给生产端的 `cmd_list`
是标题**在 present 之前就已经 `Close()` 并提交**的列表。往已关闭的列表追加 Dispatch/barrier
是未定义行为：命令被静默丢弃（`fr: coded ... 0.00 MB/frame` 整场不变量），驱动偶尔直接 AV。

修法：

- `mgpu_fg_api.h` 新增 `MGPU_FG_FRAME_CMD_OPEN`（`1u << 16`），只有 Evaluate 车道置位；
  `MgpuFgFrame` 布局不变。
- 生产端：标题 list 还开着 → 照旧记在标题 list 上（行为不变）；否则记在**自己的 DIRECT
  队列列表**（`publish_frame()` 里 `Wait(标题帧围栏)` 之后提交，读 MV/depth 的时机与既有
  拷贝路径一致）。scale / codec / TileLen 回读三处统一。
- 兜底：present 车道又拿不到自己的 DIRECT 列表（`CopyInline=1` 或异步队列创建失败）时
  **拒绝记录**并打一条日志，而不是继续往关闭的列表里写。

## 2. MV 码流：MV 与 depth 共用描述符槽，MV 实际从未被写入

修完第 1 条后，深度开始有真实码率，但 MV 恒为 0：

```text
fr: coded mv    0.00 MB/frame (raw 14.32, 0.00x)     ← 612 次
fr: coded depth 7.59 MB/frame (raw 14.32, 1.89x)
```

`TileLen[]` 每 tile 至少 4–8 B，14661 个 tile 全零也要 ≈0.11 MB/帧，所以 `0.00` 只可能是
MV 的 dispatch 没写 `TileLen[0]`。根因：`g_fr_heap` 只有 3 个描述符，MV/depth 两次 encode
共用；两次都记在同一张 list 里，描述符由 CPU 在提交前写，GPU 执行时才读——depth 那次把
同一组槽覆盖成 depth 的表，于是两个 dispatch 都用 depth 的表跑：depth 槽被写两遍、
**MV 槽和 `TileLen[0]` 永远没人写**。

修法：描述符堆 6 个（`mv srv,uav,tl | depth srv,uav,tl`），`fr_encode_dispatch()` 按
`d32 ? 3 : 0` 取基址。消费端本来就是两个独立堆，无需改动。

## 3. 实测（PBZ，2562×1441，`MvCodec=1` / `DepthCodec=1`）

```text
fr: coded mv    avg 7.50 MB/帧 (min 2.68, max 10.91, 1.54–3.08x)   130/130 非零
fr: coded depth avg 7.15 MB/帧 (min 5.60, max 8.56,  1.67–2.56x)   130/130 非零
crash 0 · already published 0 · stale colour ACK 0 · reclaimed slot 0 · watermark skip 0
publish ... slot= 正常轮转（0,1,2,…,8,…,0），present 101 个窗口 avg 13.18 ms、长帧 0.75%
```

- MV 压缩比随画面变化（加载/静止 4.6–5.8 MB、战斗 8–11 MB）⇒ PBZ 提供的是**真实、有熵的
  MV**；早先“PBZ MV 不可用”的印象由这个 bug 造成，需要重测。
- 跨卡流量：MV+depth ≈ **14.65 MB/帧**（raw 两流 28.64 MB）⇒ 约 **1.96x**；按 39.8 发布/秒
  约 **0.58 GB/s**（1.2.5 的 150% 直拷路径是 1.14 GB/s 且 MV 还是错的）。

## 4. 已知遗留（本版未改）

- present 侧一帧内多次 publish（PBZ 过场窗口曾出现一个 600 次窗口里 97 次重复 token）：
  目前靠 `already published` 折叠兜住，属于节奏浪费。
- credit 窗口 / colour ACK 落后时 `publish slot=0` 会长期复用单槽（本次会话未出现，重连后
  自行恢复）；不影响正确性，但配对窗口会变窄。
- 过场（DLSS-SR 停帧、`srFresh=0`）时仍按 staged tag 继续发布，没有“过场不下发”的策略。

## 5. 升级注意

- 整包替换：`fakerFG.dll` + `xess_mfg_producer.dll`（必须成对），建议连
  `lock_frame.exe` / `lock_frame_ui.exe` / `fakerFG.ini` 一起换。
- 回退传输：`MvCodec=0` / `DepthCodec=0` 重启游戏即可，无需换回旧 exe。
- 构建验证：`./lock_frame/build.sh all` 通过（MSVC 14.44.35207；producer / fakerFG / UI 0
  warning，consumer 只剩既有的 C4996 `wcsicmp` 弃用警告）；本包由 `./lock_frame/release.sh`
  生成并通过 `check_release.py` 校验。
- 已知限制不变：`lock_frame` 仍是"游戏侧锁帧 + 固定倍率"，动态多帧不开放；只支持
  无边框/窗口化全屏，不要在带反作弊的在线游戏里使用。
