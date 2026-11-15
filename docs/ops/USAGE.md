# mgpu 使用说明（当前链路，2026-11-14）

**双卡帧生成**：游戏用自己的卡渲染 + 超分（DLSS / FSR / XeSS），
另一块卡用 **XeSS-FG** 插帧，成品通过覆盖窗口上屏。

- 目标组合：RTX 3080 Ti 渲染 + RX 6750 GRE 插帧（本机实测）；
- A 卡 + N 卡混插可用，单卡也能用（插帧和渲染在同一块卡上）；
- 游戏本身**不需要**支持帧生成，只要支持 DLSS 2+ / FSR 2.2+ / XeSS 之一；
- 旧版（ReShade add-on + `bridge.exe`）说明已归档到
  [`archive/USAGE_bridge.md`](archive/USAGE_bridge.md)。

> 本机基准（仅参照，换硬件/游戏需重测）：4K、游戏锁 40 fps、2X → 80 fps；
> 跨卡 28.1 MB/帧（`StreamPercent=150`）；`marker: decoded≈帧数 / miss 个位数`。

---

## 1. 包内容

当前发布包（`scripts/make_release_fg.sh` 产出）结构：

```text
mgpu-<版本>/
├─ README.md                 ← 本文件（发布时由 scripts/release_README.md 生成）
├─ CHANGELOG.md
├─ SHA256SUMS.txt
├─ game/                     ★ 全部复制到游戏 exe 目录
│   ├─ dxgi.dll              = fakerFG.dll（代理 1）
│   ├─ nvngx_dlssg.dll       = fakerFG.dll（代理 2）
│   ├─ fakerFG.dll           = fakerFG.dll（同名副本，便于识别）
│   ├─ fakerFG.ini
│   ├─ xess_mfg_producer.dll
│   ├─ xess_mfg_consumer.exe
│   ├─ mgpu_ui.exe           （可选前端，与 consumer 同目录）
│   ├─ libxess_fg.dll        （Intel，公开包可不带）
│   ├─ libxell.dll           （Intel，公开包可不带）
│   └─ run_consumer.bat      启动脚本
└─ docs/
```

> `libxess_fg.dll` / `libxell.dll` 属于 Intel，按 Intel 许可分发。
> 公开包请用 `SKIP_XESS_DLLS=1` 打包，并让用户自行从 XeSS SDK 获取。

---

## 2. 前置条件

| 项 | 要求 |
|---|---|
| 系统 | Windows 10 1809+ / Windows 11（WGC + DirectComposition） |
| 游戏 | 64 位；支持 DLSS 2+ / FSR 2.2+ / XeSS 之一（能开超分即可） |
| 显卡 | 一块渲染、一块插帧；同一块也能跑。显示器接在**插帧卡**上 |
| 限帧器 | RTSS 或驱动自带 Max Frame Rate（把**游戏**锁到整数 vblank） |
| 运行库 | VC++ 14.40+（`msvcp140.dll` / `vcruntime140.dll` / `vcruntime140_1.dll`） |
| 显示模式 | 游戏**无边框/窗口化全屏**，分辨率与显示器**1:1** |

**为什么必须 1:1**：marker 的几何按画面尺寸的比例计算，缩放后消费端读不到四角码，
会报 `present size … != window client …`，配对退回时间戳，可能出现重影。

---

## 3. 安装（两步）

### 3.1 游戏目录

1. 完全退出游戏；
2. 把 `game/` 里的**所有文件**复制到**直接包含游戏 exe** 的目录
   （例如 `...\Cyberpunk 2077\bin\x64`、`...\PBZ\platform\steam`）；
3. 确认三个 fakerFG 副本是**同一份文件**：
   `dxgi.dll`、`nvngx_dlssg.dll`、`fakerFG.dll`（发布包已经保证，手工替换时别只换一个）；
4. 如果目录里原本有 `dxgi.dll`，先备份（开发期可用
   `scripts/deploy_faker_fg.sh --uninstall "<游戏目录>"` 还原）。

> **游戏加载的是 `dxgi.dll`**，不是 `fakerFG.dll`。只更新后者 = 游戏仍跑旧代码。

### 3.2 消费端

- `xess_mfg_consumer.exe`、`mgpu_ui.exe`、两个 Intel DLL **放同一目录**（任意位置）；
- 编辑 `run_consumer.bat`，把 `set "GAME_EXE=..."` 改成游戏的进程名
  （例如 `Cyberpunk2077.exe`）；
- 确认游戏已进入**实际场景**（主菜单通常不产帧）。

---

## 4. 启动

```bat
run_consumer.bat
```

等价命令（参数含义见 §6）：

```bat
xess_mfg_consumer.exe --game Cyberpunk2077.exe --den 1 --pool 4 --interp 1 --lock 0 ^
  --unlock-mfg --fg-rt-mode auto --fg-ui none --pub-fifo --report 40 ^
  --marker-probe --no-osd
```

启动后会创建一个无边框覆盖窗口，那就是最终画面（本机基准 2X，80 fps）。
`Ctrl+Alt+End` 可退出（consumer 注册的热键）。

开机自检应看到：

```text
[consumer] capture item 3840x2160
[consumer] present 3840x2160 == window client 3840x2160 (1:1, no stretch)
[consumer] marker code: seq12=… crc8=… ok
[consumer] cadence: input=25.0ms = 3.00 vblanks … xN=2.00
```

---

## 5. 图形界面（可选）

`mgpu_ui.exe` 与 consumer 同目录，双击即可：

- 枚举可见窗口，选中后用 `--hwnd` 让 consumer 抓该窗口；
- 调整模式/倍数/输出/帧池/色彩空间/叠加层/热键/GPU；
- 「启动命令」实时回显，所见即所传；
- 底栏解析 consumer stdout 显示 `show / xN / skip / gpu / game`。

注意：

- 「建议锁帧」只是提示，真正要锁的是**游戏本身**（驱动 Max Frame Rate / RTSS）；
- 消费端必须与 `mgpu_ui.exe` 同目录（它按自身路径找 `xess_mfg_consumer.exe`）；
- 详细映射见 [`UI.md`](UI.md)。

---

## 6. 常用开关

### 6.1 产品参数

| 参数 | 作用 |
|---|---|
| `--game <exe>` / `--hwnd 0x…` | 目标窗口（二选一；UI 用 hwnd） |
| `--interp N` | 固定倍率：`1`=2x、`2`=3x、`3`=4x…；3x+ 需 `--unlock-mfg` |
| `--interp-cadence` | 动态逐帧：按实际 vblank 数决定倍率；`--interp` 变成容量上限 |
| `--fg-out-hz N` | 输出帧率上限（0 = 面板刷新；最低 2x） |
| `--fg-rt-mode ema\|raw\|median\|quant\|auto` | 喂给 XeFG 的 `frameRenderTime` 估计 |
| `--pool N` / `--latency N` | WGC 帧池深度 / 最大帧延迟（默认 4 / 1） |
| `--den N` / `--scale F` | 生成分辨率 = 抓到的 1/N / 上屏缩放 F |
| `--sdr` | 输出 SDR（BT.709）；HDR 游戏不要加 |
| `--no-osd` | 关闭左上角 OSD |
| `--gpu N` | 插帧用哪块卡（不传=自动按输出显示器/避开生产者） |

### 6.2 质量/带宽 A/B

| 参数 | 作用 |
|---|---|
| `--upsample` | 在消费卡把 MV/深度重建到渲染尺寸 |
| `--stream-auto` | 按 WGC 延迟自动选择每流 `StreamPercent` |
| `--stream-asym` | MV 1:1 + 深度降档（隐含 `--upsample`） |
| `--no-depth` | 不传深度：跨卡减半；代价是遮挡边缘变差 |
| `--no-marker` | 关闭 marker 配对（退回时间戳/FIFO，**不推荐**） |
| `--no-prefetch-inputs` | 关闭输入预拷（回到 WGC 到达后再拷） |

### 6.3 调参与诊断

| 参数 | 作用 |
|---|---|
| `--unlock-mfg` | 内存补丁打开 3x–6x（仅供个人研究，见 `LEGAL.md`） |
| `--fg-ui none` | 不让 XeFG 合成 UI 纹理 |
| `--pub-fifo` / `--no-pub-fifo` | 默认按发布队列顺序消费；关掉后仍拷贝，但按 publication 值匹配 |
| `--report N` | 每 N 帧输出一次统计 |
| `--cadence-source tick\|mean` | 动态逐帧的 k 来源；默认 mean（均值+相位，smooth 8），tick 为 A/B（frame_qpc_ms → vblank） |
| `--dump N` | 抓成对帧（颜色/深度/MV）到磁盘 |
| `--marker-probe` / `--probe-colour` | marker/颜色探针 |
| `--list-modes` | 列出显示模式后退出 |
| `--no-capture-thread` | 回到 M1 单线程循环（A/B） |
| `--colour-queue N` | 颜色队列深度（默认 1；加大会增加延迟） |

> 另有大量 A/B/实验开关（`--fg-of`、`--mv-flip`、`--jbuf`、`--slack*`、`--lock` 等），
> 只在排障时用；产品路径不要依赖它们。

---


## 7. 怎么判断正常

| 指标 | 健康标准 |
|---|---|
| `marker: decoded` | 随帧数增长，`miss` 个位数 |
| `arr(vb):` | 集中在整数 vblank（40 fps @120 Hz → `3x=40`） |
| `HITCH` | 每分钟 0–2 次；持续增长说明输入节奏或拷贝队列有问题 |
| `SLOW frame` | 偶发可接受；`colour=100ms+` 需看 sink/GPU 是否落后 |
| `bus:` | 与 `StreamPercent` 相符（150 → 28.1 MB/帧） |
| `pf:` | hit 占绝大多数；`dropped` 高说明拷贝队列跟不上 |

---

## 8. 换游戏时的检查清单

1. **必须走 DLSS-SR / FG 的 Evaluate**：fakerFG 在那里拿 MV/深度并开始画 marker。
   完全没有 FG 的游戏不会有 `marker draw:` / `publish #N`，只能退回 `--fg-of`；
2. **无边框/窗口化全屏**：独占全屏 WGC 抓不到；
3. **分辨率 1:1**：渲染尺寸 = 显示器尺寸；
4. **锁 40（或 30/60）**：让输入落在整数 vblank；不要用 `--lock` 压游戏帧率；
5. **HUD 不挡四角 marker**：至少 1 个角可读，最好 4/4；
6. **SDR 游戏加 `--sdr`**；HDR 游戏不要加。

---

## 9. 日志与排障

| 文件 | 内容 |
|---|---|
| `mgpu_consumer.log` | 消费端：`breakdown` / `HITCH` / `SLOW frame` / `marker` / `pf` |
| `<游戏exe>_fakerFG.log` | 游戏侧：`marker draw` / `record:` / `publish` / `xefg: 2X present status` |
| `<游戏exe>_mfg_producer.log` | 跨卡 publication：`publish #N` / `shared ready` / `[mv]/[depth] copy` |

常见现象：

| 现象 | 先查 |
|---|---|
| 没有 `marker draw:` / `publish #N` | 游戏没开 FG/超分；或加载的是旧的 `dxgi.dll` |
| `present size != window client` | 游戏分辨率与显示器不一致，或窗口被缩放 |
| `shared version mismatch` | `producer`/`consumer` 不是同一次构建 |
| `marker decode: only n/4 corner blocks readable` | HUD 挡住四角，调整 HUD 或接受降级 |
| 画面重影/错位 | 先确认 marker 在增长；再检查 `--no-marker` 是否被误开 |
| 卡顿集中且 `colour` 很长 | GPU 落后/颜色 sink 阻塞；看 `SLOW frame` |
| `pf misses` 高、`cq` 尖峰 | 拷贝队列/预拷问题；先降 `StreamPercent` 或 `--no-depth` |

---

## 10. 已知限制

| 做不到 | 原因 |
|---|---|
| 游戏自带 FG + 本工具同时开 | 三层插值，画面必炸 |
| TSR / FSR1 / 原生 TAA | 拿不到 MV，插值质量不可接受 |
| 在消费端锁游戏帧率 | 无效；必须外部限帧器 |
| 5K 原生高倍率 | 只有 2x@30 ≈ 60 fps 跑得动 |
| 缩放窗口/非 1:1 | marker 与尺寸检查会失败 |
| 3x–6x 的“原生”质量 | 依赖 MFG 内存补丁；2x 是唯一原生档 |

---

## 11. 卸载

删除游戏目录里的：

```text
dxgi.dll
nvngx_dlssg.dll
fakerFG.dll
xess_mfg_producer.dll
fakerFG.ini（可选）
*.log（可选）
```

如果原来的 `dxgi.dll` 被替换过，用 `scripts/deploy_faker_fg.sh --uninstall "<游戏目录>"`
从 `.bak` 还原。消费端/UI/Intel DLL 是独立目录，直接删除即可。

---

## 12. 许可

GPL-3.0-or-later；第三方见 `NOTICE`。`libxess_fg.dll` / `libxell.dll` 属于 Intel，
公开分发前先读 `docs/ops/LEGAL.md`；`--unlock-mfg` 仅供个人研究。

