# `mgpu_ui.exe` —— 启动器（当前链路，2026-11-14）

`mgpu_ui.exe` 是 `xess_mfg_consumer.exe` 的图形前端：枚举游戏窗口、拼参数、
启动/停止消费端、解析 stdout 显示状态。它**不参与渲染**。

旧版（启动 `bridge.exe`）的说明已归档到 [`archive/UI_bridge.md`](archive/UI_bridge.md)。

---

## 1. 部署

- `mgpu_ui.exe` 必须与 `xess_mfg_consumer.exe`、`libxess_fg.dll`、`libxell.dll` **同目录**；
- 设置保存在同目录的 `mgpu_ui.ini`；
- 启动游戏并进入实际场景后，再在 UI 里点「启动」或按热键。

---

## 2. 界面卡片与参数映射

界面由 5 张卡片组成：`帧生成` / `捕获` / `启动`（左列），`GPU / 显示器` / `叠加层`（右列）。
每一行都映射到一个 consumer 参数，底部「启动命令」实时回显。

### 2.1 帧生成

| 行 | 取值 | consumer 参数 |
|---|---|---|
| 类型 | 只读 `XeFG 1.3.1 (libxess_fg)` | — |
| 模式 | 固定 / 动态逐帧 / 关闭 (1:1 直通) | 固定：`--interp N-1`；动态：`--interp 5 --interp-cadence`；关闭：`--no-fg` |
| 倍数 | 2x–6x（默认 3x，仅固定模式可用） | `--interp N-1` |
| 输出帧率 | 0 = 面板刷新；5–1000 Hz（仅动态模式） | `--fg-out-hz N`（0 不发） |
| 输出 | 1:1 / 5K→4K (0.75x) / 半分辨率 | `--den 1` / `--scale 0.75` / `--den 2` |
| 升采样 MV/深度 (A/B) | 默认关 | `--upsample` |
| 自动 StreamPercent (A/B) | 默认关 | `--stream-auto` |
| MV 全分辨率 / 深度降档 (A/B) | 默认关（开时自动加 `--upsample`） | `--stream-asym` |

> 动态逐帧模式把 `--interp` 当容量上限（UI 固定发 5 = 6x），实际倍率由每帧
> 经过的 vblank 数决定。

### 2.2 捕获

| 行 | 取值 | consumer 参数 |
|---|---|---|
| 捕获 API | 只读 `WGC` | — |
| 色彩空间 | HDR10（默认）/ SDR（BT.709） | SDR 时 `--sdr` |
| 帧池深度 | 2–6，默认 4 | `--pool N` |
| 最大帧延迟数 | 1–3，默认 1 | `--latency N` |
| 输入预拷 (A/B) | 默认开 | 关闭时 `--no-prefetch-inputs` |
| 不传深度 | 默认关 | `--no-depth` |
| 关闭标记配对 | 默认关（**不推荐**） | `--no-marker` |

### 2.3 GPU / 显示器

| 行 | 说明 | consumer 参数 |
|---|---|---|
| 帧生成用的 GPU | `自动` 或枚举出的适配器 | `--gpu <index>`（自动不发） |
| 输出显示器（只读） | 当前桌面模式 | — |

### 2.4 叠加层

| 行 | 默认 | consumer 参数 |
|---|---|---|
| 分层窗口 (A/B) | 开 | `--overlay-layered` |
| 输入转发 (A/B) | 关 | `--relay-input` |
| 单行 OSD | 开 | 关闭时 `--no-osd` |

两个 A/B 开关互斥：开一个会自动关掉另一个。

### 2.5 启动 / 热键

| 行 | 说明 |
|---|---|
| 热键作用于 | 前景窗口（默认）或上次启动的窗口 |
| 开关热键 | 默认 `Ctrl+PageUp`；支持预设与自定义（Ctrl+点击捕捉） |
| 建议锁帧 | **只是提示**，不会发给 consumer；真正要锁的是游戏本身（驱动 Max Frame Rate / RTSS） |
| 启动命令 | 只读回显完整命令行 |

---

## 3. 固定参数

除了界面行拼出的参数，UI 总会附加：

```text
--unlock-mfg --fg-ui none --fg-rt-mode auto --pub-fifo --report 40
```

| 参数 | 作用 |
|---|---|
| `--unlock-mfg` | 解锁 3x–6x（个人研究；读 `LEGAL.md`） |
| `--fg-ui none` | 不让 XeFG 合成 UI 纹理 |
| `--fg-rt-mode auto` | 锁帧时吸附整 vblank，帧率漂移时用真实间隔 |
| `--pub-fifo` | 按发布顺序消费 |
| `--report 40` | 每 40 帧输出统计（日志可读性） |

窗口目标用 `--hwnd 0x…`（选中的窗口）或 `--hwnd {当前前台窗口}`（前景模式）。

---

## 4. 启动/停止与状态

- 标题栏「启动 / 停止」或全局热键：未运行则启动，运行中则停止；
- 停止顺序：先 `WM_CLOSE`，2.5 s 后仍未退出再终止；
- 底栏解析 consumer stdout：`show / xN / skip / gpu / game`，右侧显示最后一条
  `breakdown:` / `WARNING` / `ERR`；
- 启动失败会提示：找不到 consumer、游戏窗口无效、已在运行等。

---

## 5. 配置档

- 每个配置档独立保存上面的全部设置与目标窗口；
- `+` / `−` 新建/删除（至少保留一个）；
- 改动即时写 `mgpu_ui.ini`；
- 旧键（`jitter_zero` / `tearing` / `sdr` / `no_mv` 等）只用于迁移，界面不再暴露。

---

## 6. 当前已知问题

1. 代码/日志里仍有 `bridge` 命名（变量、`bridge_*.txt`），与真实进程
   `xess_mfg_consumer.exe` 不一致；
2. `帧生成用的 GPU` 只读行仍硬编码显示 `AMD Radeon RX 6750 GRE`；
3. 没有直接输入 `StreamPercent` 的控件（只有 auto/asym）；
4. 不显示 `fakerFG.ini` 的 `Mode`（consume/local）；
5. 启动参数里的产品默认值硬编码在代码中；
6. 没有“一键收集日志/ini/GPU 信息”的诊断包。

> 这些已在 `docs/PROJECT_STATUS_2026-11-14.md` §2.5 列为后续改进项。
