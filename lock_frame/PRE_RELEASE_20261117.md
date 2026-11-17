# lock_frame 尝鲜版 PRE-RELEASE

**2026-11-17 · Shared v28 · 适用 Cyberpunk 2077 / PBZ**

## 整体架构

```text
渲染卡：游戏渲染 + producer 发布 MV/depth
              │ 跨卡共享内存（32 槽 ring）
              ▼
消费卡：WGC 捕获颜色 + marker 配对 + XeSS-FG 插帧 + 上屏
```

- 颜色走 WGC，不占渲染卡颜色拷贝带宽。
- MV/depth 走独立共享内存和 copy queue。
- 消费端按 marker 精确配对，匹配失败不会猜测 slot。
- 支持 **40x3（40fps 输入 → 120fps 输出）** 和 **30x4（30fps → 120fps）**。

## fakerFG 设计

fakerFG 是游戏侧的记录层，不负责生成画面：

- 通过 DXGI/Streamline 代理加载，拦截 DLSS-G 的 SetOptions、PCL marker 和 NVNGX 调用。
- 阻断游戏原生 DLSS FrameGeneration，保证游戏每次 Present 都是真实的渲染帧。
- 从 Streamline 标签 / DLSSG 参数块获取 MV、depth、相机和 jitter。
- 在 Present 前把短 marker 画进画面四角，作为颜色与 MV/depth 的唯一配对键。
- 将一帧记录交给 producer，由 producer 复制到跨卡共享 ring；fakerFG 自身不做补帧、不接管上屏。

默认配置就是：

```ini
FgInput=DLSSG
FgOutput=Capture
```

即 **DLSS-G 输入 + WGC 颜色捕获**。

## 帧节奏设计

- 游戏侧锁定输入：40x3 锁 40fps，30x4 锁 30fps。
- 消费端固定倍率插帧：40x3 → 3x，30x4 → 4x，目标都是 120fps。
- marker 把 WGC 颜色和对应 MV/depth 对齐，32 槽 ring 吸收 WGC 的异步延迟。
- `frameRenderTime` 使用输入间隔的中位数并吸附整 vblank，减少 XeFG 的节奏误判。
- 输入稳定时保持固定倍率；复杂场景低于目标时，可打开可选的“低帧自适应”动态补帧。
- 40x3 的真实帧更多、延迟更低；30x4 插帧比例更高、负载和延迟更大。
- 配对异常时进入健康状态机和光流降级，避免一次错位演变成长期卡顿。

## 核心算法与作用

- **Marker 配对**：颜色四角携带 seq12/ms12，与 producer frame_id 精确匹配，防止颜色和运动数据错帧。
- **32 槽 publication ring**：吸收 MV 先到、颜色后到的异步延迟；ACK/watermark 防止覆盖仍在使用的槽。
- **v28 共享布局**：控制块与纹理数据区彻底分离，修复控制字段被 slot 覆盖导致的长期错位。
- **Session/ACK 恢复**：consumer 重连生成新会话，producer 清空旧 ACK/帧记录；stale ACK 不再造成永久跳过。
- **健康状态机**：按 64 帧窗口的失败率区分 `OK / WATCH / RECOVER / DEGRADED / RESET / REJOIN`；只有持续结构性错位才 hard reset。
- **DEGRADED 降级**：配对异常时用当前颜色光流生成 MV，复用最后有效 depth；保持输出，避免长期 1X。
- **低帧自适应（可选）**：输入低于锁帧上限时按 vblank 动态增加插帧数，目标仍是 120fps。
- **Wide-depth 转换**：PBZ 的 R32G8X24 depth 自动转换为 R32，避免 `StreamPercent=100` 时 depth 缺失。

## 效果与数据指标

- 40x3 稳态：`exact=40 no_slot=0`，`presents=120/120`，`show=118~120`。
- 30x4 长跑：精确配对约 **99.2%**，输出约 **99.77%**，稳态 `show=118~120`。
- 残余卡顿主要来自游戏/渲染卡自身短时掉帧，而不是稳态配对队列。
- OSD 新增：

```text
mvN 99%   OK
```

`mvN` = 使用游戏原生 MV 的帧比例；下降表示已进入光流/历史输入降级。

## 使用说明

1. 将包内文件覆盖到游戏 `bin\x64`：
   ```text
   fakerFG.dll
   dxgi.dll
   xess_mfg_producer.dll
   lock_frame\lock_frame.exe
   lock_frame\lock_frame_ui.exe
   fakerFG.ini
   ```
2. 保留游戏已有的 `libxess_fg.dll`、`libxell.dll` 和 `lock_frame_ui.ini`。
3. 游戏设为无边框/窗口化全屏，开启 DLSS Frame Generation。
4. 游戏锁帧：30x4 锁 30fps，40x3 锁 40fps。
5. 启动游戏后运行 `lock_frame_ui.exe`，选择窗口和模式，点击启动。

默认配置：

```ini
FgInput=DLSSG
FgOutput=Capture
StreamPercent=150
StreamPercentMv=150
StreamPercentDepth=150
ColourStream=0
CopyInline=0
```

## 注意事项

- 只运行一个 consumer，避免多个进程写同一共享状态。
- 必须同时更新 fakerFG、producer、consumer 和 UI，避免 v28 协议不匹配。
- 低帧自适应默认关闭；它可能在 3x/4x 下产生轻微 flicker/ghosting。
- 光流降级的画质低于游戏原生 MV，depth 也不是无损恢复。
- reset/degraded 期间可能出现短暂重复帧或 1X。
- 游戏自身低于锁帧上限时，补帧无法创造真实运动信息。
- 当前为尝鲜版，建议先用于 2077/PBZ 测试和日志观察。
