# 变更记录

只记**用户能感知**的变化与对应实测依据。每条结论的原始数据在
`docs/findings/PERF_BASELINE.md` / `FINAL_CONFIG.md`。

## v0.12.2 —— 文档：UE5 备注（2026-11-09）

- README 新增「游戏引擎备注」：**UE5 不建议用运动矢量**（加 `--no-mv`）+ **必须开「分层窗口」**
  （`--overlay-layered`，UI 默认已开），并把原因写清楚（UE5 的 MV 在抖动后的画面上算、单位/方向
  不统一；UE5 自认为"未激活"时会丢菜单点击）。同样的备注也进了发布包里的 `README.md`
  （= `docs/ops/USAGE.md` §6）。

---

## v0.12.1 —— add-on 安装器改为强制覆盖（2026-11-09）

**修的是"装好了但跑不起来"这类问题**：旧 add-on / 旧 `game_capture.ini` 残留会让协议对不上、
或让新参数（`StreamHeight=1440`）看着设了其实没生效。

- 安装器**把所有文件强制覆盖**成本包版本（不再"存在就跳过"）；覆盖前改名备份
  （`x.mgpu_bak` → `.mgpu_bak2` … 最多 9 份，绝不覆盖更早那份原件）；
- 每个文件写完立刻 SHA256 校验，不一致直接失败并说明原因（被占用 / 杀软拦截 / 没权限）；
- 装完打印结果表（新建 / 覆盖 / 已是最新 + 备份名）并扫描残留（只提示不删）；
- 新增 `-KeepReShadeIni`：保留用户自己在 ReShade 面板里调过的 `ReShade.ini`，只补两个键；
- 重复运行安全（内容一致 → "已是最新"，不产生新备份）。

顺带修掉一个真 bug：安装新建文件（例如 shader）时 `Split-Path -Leaf` 拿到空备份名会抛
"Path 为空字符串"，导致安装在第 4 步中断。

---

## v0.12.0 —— 动态逐帧可用、MV 不再 drop（2026-11-09）

> 本版包含 0.11.3 – 0.11.12 的全部改动（0.11.2 之后的工作）。

**新功能**

- **动态逐帧模式**（UI 的"模式"分成 固定 / 动态逐帧 两块，互斥）：
  每帧按实际经过的 vblank 数决定倍率（`--interp-cadence`），输出恒定 1 帧/vblank，
  输入 35–45 fps 也能把 120 Hz 填满。配套：
  - `--cadence-smooth N`（默认 8）：先对"到达间隔"做 N 帧 EMA 再算倍率。
    逐帧原始间隔会让倍率跟着抖 1:1（切换 29 次/秒），实测 EMA 后切换 12 次/秒、
    空刷新 3.9% → **1.3%**。
  - `--fg-out-hz N`：输出帧率上限（`0` = 面板刷新）。把"120 Hz 面板只要 60 fps"
    变成 2x 而不是 4x；最低 2x，够不到时会打 warning 说明实际跑到多少。
- **`[capture] StreamHeight`（add-on，默认 1440）**：跨卡流（MV+深度）的高度上限，
  宽度按渲染宽高比算。4K 渲染由 63.3 → **28.2 MB/帧**；`0` = 跟随渲染尺寸（旧行为）。
- UI 的"输出帧率"改成手输数字框；"倍数"在动态模式下作为容量上限。

**修复**

- **开 MV 就 drop**：根源不是插帧算法，而是渲染卡每帧为 MV+深度扛的 63 MB 跨卡流量
  和两个全屏转换 pass（渲染卡被游戏吃满，转换抢不到执行窗口）。`StreamHeight=1440`
  之后**实测不再 drop** ⇒ 动态逐帧也能正常用游戏 MV。
- 到达间隔统计曾用 `GetTickCount64`（1 ms 粒度，0 ms 样本被丢），中位数报 31 ms
  而不是 25 ms，倍率被吸附到 4 ⇒ 输出只有 96.8 fps。改用 WGC 的 `SystemRelativeTime`
  后 3x@40 = **120.0 fps / 1.00 vblank**。
- `show=` 曾按 `loop_fps × framesPresented` 计算（120 Hz 面板上读出 148/164），
  改成 `presents / 窗口时长` ⇒ ≤ 122。
- bridge 的 `Present`/fence 等待不再每次新建事件（`wait_fence_value`），后缓冲缓存、
  只在 generation 变化时重读流几何；日志 flush 限流 200 ms（`--log-flush`）。

**诊断**

- `breakdown` 里新增 `colour=` / `cpu=`；新增 `bus:`（每帧跨卡字节与 GB/s）、
  `arr(vb):`（到达间隔的 vblank 分布）、`out:` / `out2:`（present 与 vblank 计数）、
  `game(vb):`、`bursts:`、`rt spikes:`、`rt auto:`。
- `bus:` 超过 48 MB/帧或 `inputs+copy` 占循环 ≥25% 时，额外打 `headroom:` 建议；
  流尺寸 > 1440p 时提示 add-on 的 `StreamHeight=1440` 能省多少。
- `--overlay-monitor N` / `--overlay-hidden`：覆盖窗口放到指定显示器、
  或只跑交换链不上屏（用来隔离"同屏覆盖窗口抢 independent flip"的问题）。

**已知取舍（都实测过，别再试）**

| 尝试 | 结论 |
|---|---|
| `--cadence-hold N`（保持倍率 N 帧） | 三组都更差：空刷新 6.6–10.4%、`slow` 10–13 帧/窗口。用丢帧换稳定，不值 |
| `--fg-cad-rt 1`（frameRenderTime = 批次×vblank） | 理论上更自洽，实测更差：`Present` 按预算阻塞，而输入间隔在下一帧到达时才量 ⇒ xN 从 3.00 漂到 3.38 |
| 动态逐帧 + `--fg-of`（光流） | 同卡多一个全屏 pass，drop 明显增加；30 秒段看不出来，长玩露馅 |
| `--no-depth` | disocclusion 会渗色（`HANDOFF` §8-28） |
| 固定 3x @44–45 fps 输入 | 45×3 = 135 > 120，必然超额，`slow` 23–32/窗口 |

**UI**

- 模式分成"固定 / 动态逐帧"两块（互斥，模式外的行灰显且点不动）；
  移除 `--dyn-mfg` 实验档。
- 动态逐帧下"运动矢量"行会显示权衡：`游戏 MV（动态逐帧可能闪烁）` /
  `AMD 光流（动态逐帧更易 drop）`。
- 修 Win+Tab 后覆盖窗口丢失：创建即带 `WS_EX_TOOLWINDOW`、DWMWA peek 排除、
  `ITaskbarList::DeleteTab`；`fg_is_shell_ui()` 把任务视图/Alt+Tab/Win+D 排除在焦点丢失之外。
  （不要用 `GWLP_HWNDPARENT` 设 owner —— 会让游戏鼠标输入失效。）

## v0.10.x 及更早

见 `git log`、`docs/findings/HANDOFF.md`（里程碑历史）与
`docs/findings/ARCHIVED_EXPERIMENTS.md`（已移除的实验与结论）。
