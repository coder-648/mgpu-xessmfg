# lock_frame 尝鲜版 PRE-RELEASE

**版本日期：** 2026-11-17  
**共享协议：** v28  
**定位：** 双显卡 WGC 颜色 + 跨卡 MV/depth，锁帧 30x4 / 40x3 输出 120fps  
**测试场景：** Cyberpunk 2077 / PBZ

---

## 一、这一版是什么

lock_frame 把游戏画面和运动数据拆成两条通道：

```text
颜色：WGC 捕获游戏窗口
MV/depth：producer 跨卡发布，consumer 按 marker 精确配对
输出：XeSS-FG 插帧，30x4 或 40x3 输出 120fps
```

这一版重点不是增加花哨功能，而是让长时间运行更稳、出错时更可诊断、恢复过程不长时间卡死。

---

## 二、核心亮点

### 1. 双显卡锁帧补帧

- 渲染卡负责游戏和 producer。
- 消费卡负责 WGC 颜色、XeSS-FG 和上屏。
- 支持：
  - **30x4 = 120fps**
  - **40x3 = 120fps**

### 2. marker 精确配对

- WGC 画面四角携带短 marker。
- 用 `seq12 + ms12` 与 producer 的 `frame_id` 精确匹配。
- 匹配失败不会猜测 slot，避免错帧插帧。

### 3. v28 共享内存安全修复

修复了控制块尾部被纹理 slot 覆盖的问题。此前会导致：

- `colour_seq` 被覆盖；
- session token 变随机值；
- producer 反复重置；
- 一次错位后长期错位。

现在控制块和数据区完全分离：

```text
SharedControl：79272 字节
数据起点：131072 字节
```

### 4. 连接与会话重置

- consumer 启动/重连时生成新的 `consumer_session`。
- producer 检测到新会话后清空 ACK、frame 记录和 watermark。
- 支持 stale ACK 恢复，不会因为旧 ACK 永久跳过所有发布。

### 5. 配对健康状态机

使用最近 64 次配对结果的失败率，而不是连续失败次数：

```text
OK / WATCH / RECOVER / DEGRADED / RESET / REJOIN
```

特点：

- 单次 miss 不 reset；
- 启动后 5 秒 warmup；
- reset 冷却 15 秒；
- DEGRADED 持续 2 秒才硬 reset。

### 6. DEGRADED 光流降级

配对状态不健康时：

- 当前颜色继续输出；
- 用光流生成 MV；
- 使用最后有效 depth；没有历史时使用 dummy depth；
- 尽量保持输出，避免长期 1X。

这是“维持画面/恢复队列”的降级，不是无损画质方案。

### 7. OSD 原生 MV 使用率

单行 OSD 新增：

```text
mvN 99%   OK
```

`mvN` 表示实际使用游戏原生 MV 的帧占全部输入帧的比例。  
进入降级时会看到：

```text
mvN 下降   DEGRADED
```

### 8. 可选低帧自适应

UI 的“帧生成”卡片新增：

```text
低帧自适应（动态补帧）
```

默认关闭。打开后：

```text
--interp-cadence --fg-out-hz 120
```

正常仍走固定倍率；复杂场景低于目标帧率时，按实际 vblank 动态增加插帧数。

### 9. 其他改进

- Wide-depth（R32G8X24）自动转换到 R32，`StreamPercent=100` 也不会缺 depth。
- 32 槽 publication ring。
- UI 支持配置文件、窗口选择、GPU 选择、帧池和最大帧延迟设置。
- 支持 HDR10 / SDR 输出。
- 支持 40x3 / 30x4 模式切换。
- OSD 可显示帧率、插帧倍数、输入帧率、GPU 时间、配对状态和原生 MV 使用率。

---

## 三、默认配置

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

说明：

| 配置 | 作用 |
|---|---|
| `FgInput=DLSSG` | 从 Streamline DLSS-G 输入链取 MV/depth |
| `FgOutput=Capture` | 颜色由 WGC 捕获，不走 producer 颜色流 |
| `StreamPercent=150` | MV/depth 每个维度 2/3，降低带宽 |
| `ColourStream=0` | 颜色不跨卡 |
| `CopyInline=0` | MV/depth 使用独立 copy queue，避免长时间占用渲染卡 |

日常推荐保持这套配置。

---

## 四、安装与启动

### 1. 安装

将尝鲜包内容覆盖到：

```text
<游戏目录>\bin\x64
```

例如：

```text
E:\SteamLibrary\steamapps\common\Cyberpunk 2077\bin\x64
```

需要更新的文件：

```text
fakerFG.dll
dxgi.dll
xess_mfg_producer.dll
lock_frame\lock_frame.exe
lock_frame\lock_frame_ui.exe
fakerFG.ini
```

保留游戏目录已有的：

```text
libxess_fg.dll
libxell.dll
lock_frame_ui.ini
```

### 2. 游戏设置

- 无边框/窗口化全屏。
- 开启游戏 DLSS Frame Generation（作为输入声明）。
- 游戏锁帧：
  - 30x4 场景锁 **30fps**；
  - 40x3 场景锁 **40fps**。
- 分辨率建议与桌面/输出一致。

### 3. 启动

1. 先启动游戏，进入游戏画面。
2. 运行：

```text
lock_frame\lock_frame_ui.exe
```

3. 选择游戏窗口。
4. 选择：
   - 锁帧 30x4；
   - 或锁帧 40x3。
5. 点击启动。

只运行一个 consumer 进程。

---

## 五、OSD 说明

正常示例：

```text
120 fps   x3   in 40.0 fps   gpu 2.5 ms   game 25.0 ms   mvN 99%   OK
```

重点看：

```text
mvN 99%   OK        使用游戏原生 MV，配对健康
mvN 下降  DEGRADED  已启用光流/历史输入降级
WATCH / RECOVER     配对质量开始下降，正在软恢复
RESET / REJOIN      会话正在重置或恢复
```

---

## 六、尝鲜限制

- 光流不能可靠恢复真实 depth，降级画质会下降。
- 低帧自适应会改变插帧数量，3x/4x 下可能与游戏 MV 产生 flicker/ghosting，默认关闭。
- DEGRADED/reset 期间可能出现短暂重复帧或 1X。
- 游戏自身低于锁帧上限时，补帧不能创造真实运动信息。
- 建议先用于 2077/PBZ 尝鲜和日志观察，不建议直接作为无人值守生产配置。
- 必须同时更新 fakerFG、producer、consumer、UI，避免 v28 协议不匹配。

---

## 七、近期长跑参考

- 30x4：精确配对约 99.2%，输出约 99.77%，稳态 `show=118~120`。
- 40x3：稳态输出 120/120，正常时 `exact=40 no_slot=0`。
- 主要残余卡顿来自游戏/渲染卡自身短时停顿，而不是稳态配对队列。
