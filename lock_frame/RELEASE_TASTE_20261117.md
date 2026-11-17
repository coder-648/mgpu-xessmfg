# MGpu / lock_frame 尝鲜版发布说明

**版本日期：** 2026-11-17  
**适用场景：** Cyberpunk 2077 / PBZ 双显卡锁帧补帧链路  
**当前尝鲜包：** `mgpu_2077_state_bundle_20261117_163850.zip`

> 这是一个尝鲜版本，重点不是堆功能，而是把“配对失步后长期卡死”的问题收住，并加入可观测性和降级路径。

---

## 一、这一版的核心亮点

### 1. 修复共享内存控制块被 slot 覆盖

这是本版最重要的稳定性修复。

旧布局：

```text
SharedControl 实际大小：79272 字节
纹理数据起点：65536 字节
```

控制块尾部约 14 KB 一直落在 slot 数据区，导致：

- `colour_seq` 被 slot 数据覆盖；
- `consumer_session` 变成随机值；
- generation 变成几十亿；
- producer 反复误判“新 consumer 接入”；
- 出现一次错位后次次错位。

新布局：

```text
SharedControl：79272 字节
kDataStart：131072 字节
kSharedVersion：28
```

控制块和数据区彻底分开，配对状态不再被纹理传输破坏。

---

### 2. 连接/会话状态重置

consumer 启动或重连时会写入新的 `consumer_session` token。producer 检测到 token 变化后清空：

```text
colour_seq
consumed[]
frames[]
frame_published
producer_skipped
active_mask
stream_bytes[]
```

并递增 generation、重置 slot 起点。

同时增加了 stale ACK 保护：

- ACK 在 ring 范围内：正常 watermark；
- ACK 落后超过整个 ring：视为失效 ACK，不再无限跳过；
- ACK 远高于 producer 当前帧：consumer 可以按 marker 的真实 frame 纠正。

这解决了“旧 ACK 把新会话永久卡死”的问题。

---

### 3. 配对健康状态机

不再是简单的“连续失败 16 次就 reset”，而是按滑动窗口统计失败率：

```text
最近 64 次配对结果
至少 16 个样本
失败率 >= 30% -> DEGRADED
启动后 5 秒 warmup
reset 冷却 15 秒
DEGRADED 持续 2 秒才 hard reset
```

状态包括：

```text
OK
WATCH
RECOVER
DEGRADED
RESET
REJOIN
```

这样可以识别：

```text
成功 -> 失败 -> 成功 -> 成功 -> 失败 -> 失败
```

这类没有连续长串、但已经明显不健康的错位模式。

---

### 4. DEGRADED 光流降级

当配对进入 DEGRADED，且已有上一帧有效 slot 时：

- 当前 WGC 颜色继续输出；
- 用当前颜色跑光流，生成 MV；
- depth 优先使用最后一组有效 depth；
- 没有历史 depth 时使用 dummy depth；
- 不再把猜测的 publication 当作当前帧；
- 保持输出，尽量避免直接掉到长期 1X。

注意：光流不能可靠恢复真实 depth，depth 降级是“维持输出/避免黑屏”的方案，不是画质无损方案。

---

### 5. OSD 原生 MV 使用率

单行 OSD 新增：

```text
mvN 99%   OK
```

含义：

```text
mvN = 使用游戏原生 MV 的帧 / (原生 MV + 光流 + 无 MV) 的帧数
```

用途：

- 判断当前输出到底还在用游戏原生 MV，还是已经进入光流降级；
- 异常时能看到：

```text
mvN 下降   DEGRADED
```

配合日志中的 `session reset` / `stale colour ACK` 可以快速定位问题。

---

### 6. 低帧自适应补帧（可选 UI 开关）

“帧生成”卡片新增：

```text
低帧自适应（动态补帧）
```

默认关闭。

打开后追加：

```text
--interp-cadence --fg-out-hz 120
```

用途：

- 40x3/30x4 正常时保持固定倍率；
- 复杂场景输入低于目标帧率时，按实际 vblank 动态增加插帧数；
- 尽量维持 120fps 输出。

这是可选项，不改变普通固定倍率路径。

---

### 7. Wide-depth 100% 传输修复

CP2077/PBZ 的 depth 可能是：

```text
R32G8X24_TYPELESS (19)
```

它不能直接 1:1 拷贝到 R32 slot（39）。本版增加：

- wide-depth 自动识别；
- 即使 `StreamPercent=100` 也执行 R32 转换 pass；
- 使用可执行 Dispatch 的 DIRECT copy queue，而不是 DMA COPY queue。

日志会显示：

```text
downscale: wide depth (fmt 19) -> R32 conversion pass enabled
publish ... mask=0x3
```

---

## 二、当前推荐默认配置

包内 `fakerFG.ini`：

```ini
FgInput=DLSSG
FgOutput=Capture
StreamPercent=150
StreamPercentMv=150
StreamPercentDepth=150
ColourStream=0
CopyInline=0
```

推荐日常：

```text
FgInput=DLSSG
FgOutput=Capture
StreamPercent=150
CopyInline=0
```

`StreamPercent=100` 会触发全尺寸 depth 转换，带宽和 GPU 成本更高，只建议做 A/B。

---

## 三、尝鲜包内容

```text
fakerFG.dll
dxgi.dll
xess_mfg_producer.dll
fakerFG.ini
lock_frame\lock_frame.exe
lock_frame\lock_frame_ui.exe
README_部署说明.txt
```

安装目标：

```text
<Cyberpunk 2077>\bin\x64
```

不要覆盖：

```text
libxess_fg.dll
libxell.dll
lock_frame_ui.ini
```

只运行一个 consumer 进程。

---

## 四、当前验证结果

### 30x4 长跑

- 精确配对率约 99.2%；
- 输出约 99.77%；
- 稳态 `show=118~120`；
- 主要 no_slot 集中在一次游戏端约 5 秒的 15fps 停顿；
- 稳态 `game(vb)=4x=40`。

### 40x3

- 稳态输出 120/120；
- 配对正常时 `exact=40 no_slot=0`；
- 仍可能出现游戏自身 stutter 导致的短时错位。

这些数据说明：**配对算法在稳态可用；剩余主要风险是游戏/渲染卡自身停顿和结构性状态错位。**

---

## 五、已知限制

1. 光流降级不是无损：MV 质量低于游戏原生 MV，depth 不能靠光流真实恢复。
2. `--interp-cadence` 动态改变插帧数量，3x/4x 下可能与游戏 MV 出现 flicker/ghosting，默认关闭。
3. DEGRADED/reset 过程中可能出现短暂重复帧或 1X 帧。
4. 输入本身低于目标帧率时，任何补帧都无法创造真实运动信息。
5. 必须同时更新 fakerFG.dll、dxgi.dll、producer 和 lock_frame 两个 exe，避免 v28 协议不匹配。
6. 不要同时运行多个 consumer，否则会争用同一个共享控制块。

---

## 六、测试建议

1. 先跑正常 40x3，确认 OSD：

```text
mvN 99%   OK
```

2. 在复杂场景观察：

```text
mvN 下降
mvN 恢复到 99%
```

3. 人为制造一次游戏 stutter，观察是否：

- 进入 `DEGRADED` 后仍保持输出；
- 不出现长期 1X；
- 15 秒内不反复 reset；
- 恢复后回到 `OK`。

4. 需要低帧补偿时再打开 UI 的“低帧自适应”，不要默认开启。

---

## 七、发布结论

这是一版以**稳定性、可观测性、可控降级**为重点的尝鲜版：

- 修复了共享内存覆盖这个根因；
- 增加了连接/会话重置和 stale ACK 保护；
- 把 reset 从“失败率触发”改为“健康状态机 + 降级 + 最后硬重置”；
- 增加光流降级和 OSD 原生 MV 使用率；
- 增加了可选的低帧自适应补帧。

建议先用于 2077 场景尝鲜和日志观察，不建议直接作为无人值守长期生产配置。
