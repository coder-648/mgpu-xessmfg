# Release notes

每个版本一份，可直接粘到 GitHub Releases。版本历史看 `CHANGELOG.md`。

---

## v0.13.0 —— UE5「模型运动伪影」结案：主因是上采样倍率

> 游戏用自己的卡渲染 + 超分（DLSS / FSR / XeSS），另一块卡专职跑 **XeSS-FG** 插帧，
> 再通过覆盖窗口上屏。A 卡 + N 卡混插可用；单卡也能跑。

这版把 UE5（影之刃零 / 黑神话 / 明末 …）上一直存在的**模型边缘伪影**查清了：
**主因不是运动矢量，而是上采样倍率** —— 插帧插的是上采样之后的画面，DLSS 性能档
（720p→4K，3 倍）连"不插帧的游戏本身"都有一点伪影；换成 **DLAA（1:1）+ `--fg-of`** 就干净。

### ✨ 新配方

- **UE5 / 画质优先**：游戏内 **DLAA**（或最高画质档）+ `--interp 1 --fg-of`
  （4K DLAA 下 26→52 fps，实测无模型边缘伪影）
- 必须用性能/平衡档时：伪影随放大倍率走，与开不开插帧关系没那么大

### 🔧 修掉的真 bug

- **`--fg-of` 之前基本没生效**：光流场是 screen-UV，`motionVectorScale` 却透传游戏的
  `MV.Scale`（PBZ 报 `1.0`）⇒ 库把 `0.009` 当成 0.009 像素。现在用 MV 纹理尺寸
  （日志 `mvscale=(1278.0000, 720.0000)`）。**影响所有 `--fg-of` 配方，含 3x/4x 推荐档。**
- **add-on 开始读游戏声明的 DLSS flags** 并转发 `MVJittered` / `MVLowRes` / **`DepthInverted`**；
  bridge 在创建 swap chain 前再读一次，迟到会打 WARNING。PBZ 声明
  `0x4B = IsHDR｜MVLowRes｜DepthInverted｜AutoExposure`（`MVJittered=0`）。

### 🔍 新工具

- `--fg-mv-scale raw|px|<float>`、`--fg-no-jittered-mv`；`--fg-jitter-zero` 重新生效（A/B 用）
- `scripts/analyse_dump.py` 的 **units fit**：拟合 `motion_px = mv_raw × k`，直接判 MV 单位与帧配对
- `scripts/recipes/run_ue5_mv_ab.bat`：`auto/jit/nojit/depthinv/jitdepth/mvscale/nomv/of/ofsign/offov/jz/nodepth/only/dump`

### ⚠️ 注意

- UE5 交给 NGX 的运动矢量可能是**坏数据**（PBZ 实测：0.5% NaN/inf、17% 饱和到 fp16 上限、
  与真实位移相关性 ≈0）⇒ 那条流别用，用 `--fg-of` 或 `--no-mv`
- 详细证据链：`docs/findings/UE5_MV_OPTISCALER.md` §7–§8

---

## v0.12.2 —— 双卡帧生成：动态逐帧可用、MV 不再 drop

> 游戏用自己的卡渲染 + 超分（DLSS / FSR / XeSS），另一块卡专职跑 **XeSS-FG** 插帧，
> 再通过覆盖窗口上屏。A 卡 + N 卡混插可用；单卡也能跑。

这版把"输出不够丝滑"这条线彻底查清了，落成三件事：**动态逐帧**（输入帧率抖动也能填满高刷）、
**跨卡流量减半**（开 MV 不再掉帧）、**安装器强制覆盖**（不再被旧文件坑）。

### ✨ 新功能

**动态逐帧模式**（UI 模式分成「固定」/「动态逐帧」两块，互斥）

- 每帧按实际经过的 vblank 数决定倍率（`--interp-cadence`），输出恒定 **1 帧/vblank**，输入 35–45 fps 也能把 120 Hz 填满
- `--cadence-smooth N`（默认 8）：先对到达间隔做 EMA 再算倍率 —— 逐帧原始间隔会让倍率跟着抖 1:1
- `--fg-out-hz N`：输出上限（0 = 面板刷新）。把"120 Hz 面板只要 60 fps"变成 2x 而不是 4x
- UI「输出帧率」改成手输数字框；「倍数」在动态模式下作为容量上限

**`[capture] StreamHeight=1440`（add-on 侧，默认）**

- 跨卡流（MV + 深度）的高度上限，宽度按渲染宽高比自动算；4K 渲染由 **63.3 → 28.2 MB/帧**
- `0` = 跟随渲染尺寸（旧行为）；`1080` 再降一档；`2160` = 全分辨率

### 🐛 修复

- **开 MV 就 drop**：根源不是插帧算法，而是渲染卡每帧为 MV+深度扛的 63 MB 跨卡流量 + 两个全屏转换 pass（渲染卡被游戏吃满，转换抢不到执行窗口）。`StreamHeight=1440` 后**实测不再 drop** ⇒ 动态逐帧也能正常用游戏 MV
- **到达间隔量错**：原来用 `GetTickCount64`（1 ms 粒度，0 ms 样本被丢），中位数报 31 ms 而非 25 ms，倍率被吸附到 4 ⇒ 输出只有 96.8 fps。改用 WGC 的 `SystemRelativeTime` 后 3x@40 = **120.0 fps / 1.00 vblank**
- `show=` 曾按 `loop_fps × framesPresented` 算（120 Hz 面板读出 148），改成 `presents / 窗口时长`
- `Present`/fence 等待不再每次新建事件；后缓冲缓存、流几何只在 generation 变化时重读；日志 flush 限流 200 ms
- Win+Tab / Alt+Tab / Win+D 后覆盖窗口丢失
- **安装器**：新建文件时 `Split-Path -Leaf` 拿到空备份名会中断安装（已修）

**安装器改为强制覆盖**（v0.12.1）：所有文件一律覆盖成本包版本，覆盖前轮转备份（`.mgpu_bak` → `.mgpu_bak2` … 最多 9 份），逐文件 SHA256 校验，装完打结果表 + 扫残留；新增 `-KeepReShadeIni` 保留你自己的 ReShade 设置

### 📊 实测（CP2077 4K / 120 Hz，3080 Ti 渲染 + 6750 GRE 插帧）

| 配置 | 输出 | 空刷新 | 备注 |
|---|---|---|---|
| 动态逐帧（输入 35–45 fps 抖） | 120 fps | **1.3%** | 倍率切换 12 次/秒 |
| 固定 3x @40 fps | 120 fps | **0.1–0.3%** | 最大空洞 2 个 vblank |
| 固定 2x @60 fps | 120 fps | 2.5% | 画质优先 |
| 固定 4x @30 fps | 120 fps | 2.6% | 用动态逐帧可到 0.6% |

### 🔍 诊断增强

`breakdown` 新增 `colour=` / `cpu=`；新增 `bus:`（每帧跨卡字节与 GB/s）、`arr(vb):`（到达间隔的 vblank 分布）、`out:`/`out2:`（present 与 vblank 计数）、`game(vb):`、`bursts:`、`rt spikes:`、`rt auto:`、`headroom:`（跨卡预算超阈值时给降载建议）

### ⚠️ 升级必读

- **add-on 与 bridge 必须同版本**（协议 v10，版本不匹配会直接拒绝通信）—— 用包里的 `game-pack/install.bat` 重装一次游戏侧
- `game_capture.ini` 新增 `StreamHeight`（默认 1440）。旧 ini 会让 4K 渲染仍走 63 MB/帧 ⇒ 请让安装器覆盖它
- 旧的 `--dyn-mfg` 实验档已从 UI 移除（命令行仍在）

### 🚫 已实测排除（别再试）

| 尝试 | 结论 |
|---|---|
| `--cadence-hold N`（保持倍率 N 帧） | 用丢帧换稳定：空刷新 6.6–10.4%、`slow` 10–13 帧/窗口 |
| `--fg-cad-rt 1`（frameRenderTime = 批次 × vblank） | 理论上更自洽，实测更差：`Present` 按预算阻塞，输入间隔在下一帧到达时才量 ⇒ xN 从 3.00 漂到 3.38 |
| 动态逐帧 + `--fg-of`（光流） | 同卡多一个全屏 pass，drop 明显增加（30 秒段落看不出，长玩露馅） |
| `--no-depth` | disocclusion 会渗色 |
| 固定 3x 配 44–45 fps 输入 | 45×3 = 135 > 120，必然超额 |

### 📦 下载

| 文件 | 说明 | 大小 |
|---|---|---|
| **`mgpu-0.12.2-public.zip`** | **公开版**：不含 Intel 的 `libxess_fg.dll` / `libxell.dll`，请自行从 XeSS SDK 获取（包内 `README-DLL.txt`） | 2.8 MB |
| `mgpu-0.12.2.zip` | 本地完整版（含运行时 DLL，仅自用，不用于分发） | 15.6 MB |

```
SHA256  mgpu-0.12.2-public.zip  4bf2096116f04bff6e7fdc2d4b6fb2b1369a1aebc358d5e770ccab72ba9f5ee9
SHA256  mgpu-0.12.2.zip         1b856a65aed0f62e055c62e948b131238faf76961f6682f45c2cb8140ebde045
MD5     mgpu-0.12.2-public.zip  ab5ddc7df68dfa0a818c6e3b4ef1ee13
MD5     mgpu-0.12.2.zip         5f7e7066d818291390ef2069224cad71
```

### 已知限制

游戏自带帧生成必须关；不支持 TSR / FSR1 / 原生 TAA（拿不到 MV）；必须用外部限帧器锁帧；
5K 原生只有 2x@30 ≈ 60 fps 跑得动；A 卡渲染 + N 卡输出的完整组合本机未跑通；端到端延迟 ~40–70 ms。

**许可**：GPL-3.0-or-later。`--unlock-mfg` 在内存里修改 `libxess_fg` 以打开 3x–6x，
**可能违反 Intel EULA，仅供个人研究**（`docs/ops/LEGAL.md`）。

---
