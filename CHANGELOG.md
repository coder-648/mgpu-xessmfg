# 变更记录

只记**用户能感知**的变化与对应实测依据。每条结论的原始数据在
`docs/findings/PERF_BASELINE.md` / `FINAL_CONFIG.md`。

### chore(release): 精简发布包（1.2.0 独立软件形态）(2026-11-19)

- 移除与 GUI 重复/无关的文件：`deploy_game.bat`、`run_2x/3x/4x/30x4/40x3.bat`、
  `run_ui.bat`、`README.txt`、`CHANGELOG.md`、`docs/DEPLOY.md`、`docs/UNINSTALL.md`。
- 包内现在只剩运行所需 + 必要说明：两个 exe、`fakerFG.dll`、`xess_mfg_producer.dll`、
  `fakerFG.ini`、Intel 运行时（`libxess_fg.dll` / `libxell.dll`）、
  `collect_diagnostics.bat`、`README.md`、`RELEASE_NOTES.md`、`LICENSE`、`NOTICE`、
  Intel 许可、`docs/TROUBLESHOOTING.md`、`docs/SUPPORTED-GAMES.md`、
  `manifest.json`、`SHA256SUMS.txt`，外加空 `profiles/` 目录。
- `check_release.py` 的必需文件同步调整（不再要求 bat 安装脚本 / README.txt）。
- 卸载说明并入包内 README；还原仍由 UI 的「还原 / 移除组件」完成。

实测：打包校验 17 文件通过；从下载目录启动 `sha256: checked=16 bad=0`。


- **存储拆分**：
  - `<exe>\lock_frame_ui.ini` 只保留索引：`[games] count=N` + 每项 `[gameN] path=profiles\xxx.ini, name=...`；
  - 每个游戏的完整配置（身份 + 全部参数）写在 `<exe>\profiles\<游戏名>.ini` 的 `[game]` 节。
- **旧档案自动迁移**：启动时发现 `[gameN]` 里还是内联字段（`dir/root/exe/cfg/...`），
  先读进内存、按游戏名生成 `profiles\*.ini`，再重写索引并**删掉旧内联键**（只留 path/name）。
- **命名/改名**：文件名用游戏名做安全化（非法字符→`_`，最多 48 字符，重名自动 `_2/_3`）；
  在界面里重命名游戏时，如果该文件是按旧名字自动命名的，会 `MoveFile` 同步改名并更新索引。
- 启动时落一次盘，保证 profiles 目录和文件存在。

实测（Win11 125%）：用旧格式 ini（2 个内联游戏）启动 → 生成
`profiles\LegacyMix.ini` / `profiles\LegacyAuto.ini`，索引只剩 `path/name`；
界面把 LegacyMix 改名 Renamed → 文件变为 `profiles\Renamed.ini`、内容 `name=Renamed`，
索引 `path=profiles\Renamed.ini`；点 3x 后该文件 `fg_mode=1` 单独更新。

### fix(ui): 添加游戏取错路径（名字/路径都退了一级）+ 字体与布局再放大 (2026-11-19)

- **根因**：`pick_game_exe()` 返回的是"exe 所在目录"，而「添加 exe…」和安装回退把它当作
  文件路径又取了一次父目录 → 结果显示 exe 的**文件夹名**、路径退到**父目录**。
- 新增 `pick_game_file()` 返回完整 exe 路径；「添加 exe…」和安装回退改用它：
  名字 = exe 文件名（去扩展名，如 `Cyberpunk2077`），`dir` = exe 所在目录，
  `exe` = 完整路径；主界面路径行与列表都显示**完整 exe 路径**。
- **字体整体放大**：标题 24→26、卡片 17→18、正文 15→16、小字 14→15（逻辑像素），
  125% 下正文约 20 px；侧栏行高 42→46。
- **随字号重排**：当前游戏的路径行 72–94、配置摘要 96–118、胶囊 122–148；
  运行状态 KPI 标签 42–62 / 数值 62–94、建议 100–124、日志 130–152，
  避免大字号压行。

实测（Win11 125%）：用 `cp2077\bin\x64\Cyberpunk2077.exe` 添加 →
`name=Cyberpunk2077`、`dir=...\cp2077\bin\x64`、`exe=...\Cyberpunk2077.exe`，
卡片路径行显示完整 exe 路径；对比旧版（名字=x64、路径=bin）已修正。

### feat(ui): 游戏配置名跟随 exe 名 + 名字可点击重命名 (2026-11-19)

- **默认命名**：通过「添加 exe…」新加的游戏，配置名 = 所选 exe 的文件名（去扩展名），
  例如 `client.exe` → `client`；不再用目录名。
- 旧条目如果是"自动起的目录名"（与所在目录同名，忽略大小写），在解析出 exe 后
  （后台初始化 / 主程序… / 安装时）会同步改成 exe 名；用户手动改过的名字不会被覆盖。
- **行内重命名**：点「当前游戏」卡片上的游戏名即进入编辑（蓝框 + 光标），
  Enter 保存、Esc 取消、点别处自动保存；刚进入时首个字符直接覆盖原名字，退格也可清空。
  限制 1–64 个字符，保存后立即写入 `[gameN] name=`。
- 重命名与其它子页互斥：切换游戏 / 打开游戏库会先结束编辑（不保存）。

实测（Win11 125%）：把 MixGame 的主程序选成 `client.exe` → 名字变 `client`；
点击名字输入 `Rename{Enter}` → ini `name=Rename`，等 20 s 进程稳定、界面刷新为 Rename。

### refactor(ui): 下线全局扫描添加 + 第二轮整体视觉/布局优化 (2026-11-19)

**扫描添加**
- 选游戏面板不再提供「扫描本机」：当前扫描规则（Steam/Epic/GOG/注册表关键词）筛选不可靠，
  默认不做批量导入，避免一次塞进几十个错误条目。
- 保留 `scan_*/.scan_games_merge` 实现（注释说明）以便后续做可选入口；UI 只保留
  「添加 exe…」：选一个游戏的主程序 → 加入列表 → 直接选中；不安装、不扫描。
- 选游戏面板高度随条目数自适应（不再固定占满整屏）。

**视觉/布局**
- 窗口逻辑尺寸 1200×720 → **1240×720**（150% DPI 下 1860×1080 仍在 1080p 内），
  内容区更宽。
- 卡片：圆角 10→12，卡片头加 1px 分隔线；右侧新增**状态文字**（已安装/未安装、
  就绪/等待窗口/等待 producer）+ 状态点。
- 「当前游戏」标题行：游戏名后面加**安装状态胶囊**（绿框 已安装 / 灰框 未安装），
  去掉重复的"游戏侧安装"胶囊。
- 「启动补帧」：倍率按钮改为**铺满左侧**（3 等分），启动按钮固定 200 px；禁用状态
  文字提亮到 150 灰，仍能看清。
- 「运行状态」：一行文字改成 **5 个 KPI**（输出 fps / 插帧 / 消费卡 ms / 游戏 ms / 丢帧），
  小标签 + 大数字，便于扫读。
- 侧栏：标题改为「游戏 + 数量」，帮助入口下移与版本号成组，减少大片留白。

实测（Win11 125%，client 1550×900）：卡片无重叠；安装状态胶囊绿框可见；
倍率按钮等宽铺满；KPI 数字清晰；侧栏 2 个游戏时面板紧凑。

### style(ui): 布局精修——高级设置并入安装/还原同一行，子页互斥 (2026-11-19)

- 「高级设置」从内容区顶部的独立按钮改成**与「安装 / 还原」同一行、右对齐**：
  顺序为 `安装 / 应用配置` → `还原 / 移除组件` →（右对齐）`高级设置` → `主程序…`，
  统一 36 px 高、10 px 间距；高级设置仍保留左侧强调条，打开时由抽屉覆盖，不可重复点。
- **互斥逻辑**：抽屉打开时该行的安装/还原/主程序按钮被抽屉盖住（点击不生效）；
  打开游戏库 / 日志 / 帮助会先自动收起抽屉（`close_advanced_silent`），保证同一时间
  只有一个子页在前台。
- 卡片整体上移并收紧：当前游戏 94–308、启动补帧 322–528、运行状态 542–700，
  去掉顶部独立按钮占用的 18 px，竖向节奏更均匀。
- 文案同步：帮助/状态提示里的"点右上角[高级]"改为"点[高级设置]"，"左上角选择游戏"
  改为"[当前游戏]卡片里的[选择游戏]"。

实测（Win11 125%）：同一行四个按钮对齐、无重叠；抽屉打开时点安装位置无弹窗（被抽屉
消费），按 F1 打开帮助后抽屉自动关闭（ini `advanced=0`）。

### style(ui): 整体布局重排——高级入口挪到内容区右上角、侧栏收窄、卡片重排 (2026-11-19)

- **「高级设置」入口重做**：不再塞在标题栏的小格子里，改成主内容区右上角的
  次级按钮（灰色底 + 左侧强调条，打开时整块变强调色），124×34 逻辑像素，
  和卡片右边缘对齐；UIA 的「高级」也指向新的位置。
- **整体尺寸/栅格**：侧栏 250→232、外边距 24→20、标题栏 40→42；内容区变宽
  24 px，游戏名/路径/胶囊/按钮都在同一 18 px 内边距上对齐。
- **三块卡片重排**：`当前游戏` 112–326、`启动补帧` 340–540、`运行状态` 554–706；
  卡片间距统一 14，按钮行高 36、主按钮 240 px、次按钮 230 px、右对齐「主程序…」。
- **层级收敛**：一屏只有一个主色按钮组（更换游戏 / 安装·重新应用 / 启动）；
  高级设置保持次级样式，避免和主操作抢注意力。
- **抽屉**：标题与"正在编辑：<游戏>"分行（不再压字），内容区从标题下 52 px 开始。

实测（Win11 125%，client 1500×900）：三块卡片在 1200×720 逻辑坐标内无重叠、
无横向滚动；「高级设置」在内容区右上角可点、打开后整块高亮；抽屉无压字。

### refactor(ui): 导航去重——单一"选择游戏"、游戏库只做选择/添加、高级入口移到右上角 (2026-11-19)

- **"选择游戏"只保留一个入口**：放在「当前游戏」卡片标题右侧，强调色主按钮；
  已选游戏时文案变「更换游戏」。侧栏底部、卡片底部、游戏库页脚里的重复入口全部删除。
- **游戏库改成纯选择器**：标题「选择游戏（n）」，只保留 `扫描本机` / `添加 exe…` / `取消`；
  点击任意一行立即选中并关闭。安装/还原/删除/主程序…这些按钮从库里移除
  （安装/还原在「当前游戏」卡片里，主程序改到卡片右下角「主程序…」）。
  「添加 exe…」只入列表、不再顺带安装。
- **高级设置入口移到右上角**：标题栏新增「高级」按钮（打开时高亮），点开/收起右侧子页；
  删掉侧栏的「高级设置」入口，抽屉里重复的「启动」按钮也移除（只留「完成」/Esc）。
- **UIA 同步**：可访问性列表更新为 14 个按钮（新增「更换主程序」，`gamelib` 改名为
  「选择游戏」），顺序与界面一致；`高级` 的 UIA 位置改为标题栏按钮。
- 提示文案统一改成"点右上角[选择游戏] / 点右上角[高级]"。

实测（Win11 125%）：侧栏只剩游戏列表；「更换游戏」→ 选择器点 AutoGame 一行即返回并
生效（`[ui] game_dir` 更新）；标题栏「高级」打开后显示"正在编辑：AutoGame"、只有「完成」；
UI Automation 枚举到 14 个按钮且 `gamelib` 仍可 Invoke 打开选择器。

### fix(ui): 运行检测覆盖托盘/隐藏窗口；目录归属按父/子目录匹配 (2026-11-19)

- **进程路径检测**：`game_dir_running` 不再只看可见窗口，还会枚举进程
  （`CreateToolhelp32Snapshot` + `QueryFullProcessImageNameW`），只要 exe 在该游戏
  目录或其子目录下就算"正在运行"。最小化到托盘、窗口被隐藏的游戏也会拦住安装/还原。
- **修目录归属**：`find_game/find_game_mut` 改成父/子目录都算同一个游戏
  （按 `\` 边界做前缀匹配）。此前 `[ui] game_dir` 指向 `...\Binaries\Win64`、条目只有
  `root=...\Game` 时会匹配不到，点"安装"会退化成弹出选 exe 的文件框。

实测（Win11 125%）：把 `cmd.exe` 复制成 `autogame\bin\Win64\game_stub.exe`，
以 `-WindowStyle Hidden` 启动（无可见窗口）→ 点「安装 / 应用配置」立即弹
「游戏正在运行：请先完全退出游戏再安装」，日志出现 `install: blocked (game running)`；
结束该进程后再点安装，正常进入安装确认框。

### feat(ui): 游戏列表滚动 / 运行中拦截 / 编辑归属 / ini 漂移提示 (2026-11-19)

- **侧栏游戏列表可滚动**：超过可见行数时右侧出现细滚动条，滚轮直接翻页（3 行/格），
  从游戏库选中或用键盘切换时自动滚到可见区域。
- **运行中禁止安装/还原**：按游戏目录找到正在运行的窗口时，安装、重新应用配置、还原
  都会先弹"请先关闭游戏"（替换 dxgi.dll 会被占用），避免半写坏状态。
- **高级抽屉显示归属**：标题下新增"正在编辑：<游戏名>"；没选游戏时红字提示
  "未选择游戏（改的是通用配置）"，避免误改。
- **fakerFG.ini 漂移检测**：每 2 s 读一次该游戏 ini 的 `FgInput/StreamPercent/Log`，
  与保存的配置不一致时，"当前游戏"摘要变琥珀色并追加
  "ini 被改过，点[重新应用配置]"，点一下即写回并清除提示。

实测（Win11 125%）：16 个游戏的侧栏滚轮翻到 Dummy06–16 且滚动条同步；
把 mixgame 的 `FgInput` 手动改成 `NVNGX` → 3 s 内摘要变琥珀并提示，点
「重新应用配置」后 ini 回到 `FgInput=DLSSG / StreamPercent=150 / Log=1`，提示消失。

### refactor(ui): 客户端改为"游戏为中心"，取消三步卡片 (2026-11-19)

**问题**：配置存在与游戏无关的 profile 里，游戏库条目只记 3 个字段；切游戏会丢配置、
简易页改倍率不落盘、安装/还原和"选择游戏"分散在两张卡，用户要先懂 exe 才能开始。

**改法**
- **一个游戏 = 一份完整配置**：`GameEntry` 持有完整 `State`，`S()` 按当前游戏路由，
  `save_state/load_state` 与 profile 共用同一套键；`[gameN] cfg=1` 标记新档案，
  旧档案从 `mult/fg_input/lock` 自动迁移。任何改动（倍率 / 输入源 / MV / 高级项）
  立刻 `save_all()`。
- **第一步改成选游戏**：侧栏列表 = 游戏（不再是 profile），点一行即切换配置并从该
  游戏的 `fakerFG.ini` 读回实际值；「＋ 选择 / 扫描游戏…」打开游戏库。
- **简易页整合为三块**：`当前游戏`（名称/路径/配置摘要/4 个状态胶囊 + 选择/安装/还原）、
  `启动补帧`（该游戏窗口、2x/3x/4x、启动按钮 + 唯一"下一步"提示）、`运行状态`。
  取消 ①②③ 编号和"游戏配置"概念。
- **安装/还原互斥**：未安装=只能安装；已安装=「重新应用配置」+「还原 / 移除组件」；
  还原后回到未安装。安装时自动把该游戏配置写入 `fakerFG.ini`
  （`apply_config_after_deploy`）。
- **启动门槛**：`安装 + 该游戏窗口 + producer + Intel 运行时` 才点亮启动按钮；
  否则灰色并在按钮上方写出唯一缺失项（没选游戏 / 没安装 / 游戏没跑 / producer 未加载）。
  窗口识别改为"只认当前游戏目录"，不会再拿 VS Code 窗口当游戏窗口。
- 帮助/首次运行文案同步改为"选游戏 → 安装/应用 → 进游戏 → 启动"。

**实测（Win11 125%，测试目录 C:\temp\mgpu）**
- MixGame 选 4x → `[game0] mult=4`；点侧栏 AutoGame → 摘要回到它自己的 2x，
  `[game1] mult=2` 未被污染；切回 MixGame 仍 4x。
- AutoGame 点「安装 / 应用配置」→ 确认框显示目录与 `2x / DLSSG` → 部署后
  `fakerFG.ini` = `FgInput=DLSSG / FgOutput=Capture / Passthrough=0 /
  StreamPercent=150 / Log=1`，胶囊变绿、按钮变「重新应用配置」。
- 点「还原 / 移除组件」→ 目录里 `dxgi.dll/fakerFG.dll/nvngx_dlssg.dll/
  xess_mfg_producer.dll` 移除（`.bak` 恢复），胶囊变黄、安装按钮回到"安装 / 应用配置"。

### feat(ui): 主程序候选面板 + 游戏条目后台初始化 (2026-11-19)

- **主程序…**：游戏库底部新增按钮，按评分列出该目录最多 8 个候选 exe
  （文件名 / 体积 / 分数 / 完整路径），点一行即写入 `[gameN] exe=/dir=` 并保存；
  面板高度随候选数自适应，点面板外或 Esc 关闭。低置信度游戏不再只能靠文件选择框。
- **安装时先选主程序**：点「安装 / 更新」但目录还没解析出 exe 时，先弹候选面板，
  选完自动继续安装（确认框里能看到目录与 exe 名），不再直接掉进 `GetOpenFileName`。
- **后台初始化**：扫描或启动后，未解析的条目每 4 s 解析一个（轮询，不阻塞 UI），
  成功即落盘并刷新行内文字；扫描完成从新条目开始优先解析。
- 实测（Win11 125%）：`C:\temp\mgpu\mixgame`（client.exe 5 MB / tools.exe 4 MB，
  同名目录、评分 15 不达阈值）→ 选中后点「主程序…」→ 面板列出两行（5.0 / 4.0 MB,
  score 15）→ 点 client.exe → ini 写入 `exe=...\client.exe`；`autogame` 无需点选，
  启动约 4 s 后日志出现 `bg-init: resolved entry`，行内变为
  `主程序：Auto-Win64-Shipping.exe`。

### feat(ui): 可视化客户端改为"简易模式 + 高级设置"两层，安装/还原/诊断进界面 (2026-11-19)

- **默认进入简易模式**：三张步骤卡（① 装进游戏 ② 进入游戏 ③ 倍率+启动）+ 一张
  "运行状态"卡。每步都有颜色点、一句话说明和一个按钮，不需要懂 WGC / 配对 / 命令行。
- **一键安装 / 一键还原**：`GetOpenFileNameW` 选游戏 exe，UI 自动完成
  `deploy_game.bat` 的全部文件操作（备份 `dxgi.dll`、写三个别名、复制 producer、
  缺失才放 `fakerFG.ini` 模板、`version.dll` 冲突提示），结果逐条列在弹窗里；
  "还原"会把游戏原来的 `dxgi.dll` / `nvngx_dlssg.dll` 换回去，`fakerFG.ini` 保留。
- **一键诊断 / 打开日志目录**：直接调用发布包里的 `collect_diagnostics.bat`，
  不需要离开 UI。
- **就绪状态模型**：游戏侧安装、Intel 运行时、`fakerFG.ini`、producer 模块
  （`TH32CS_SNAPMODULE` 探测）四项做成彩色胶囊；窗口列表自动优先选中
  "已经加载 producer"的游戏窗口。
- **启动前检查**：单卡、缺 `libxell`（3x/4x）、`fakerFG.ini` 不是 Capture、
  面板不整除、没有 producer 时弹窗列出原因，可"仍然启动"；热键启动不弹窗。
- **人话状态**：解析 `[lock] pair: exact=… (total=…)`，把命中率翻成
  插帧中 / 部分插帧 / 几乎没插帧，并给下一步建议（原来只有一行被截断的计数）。
- 高级模式保留原有 7 张卡片，布局与行为不变；侧栏「高级设置 / 返回简易模式」
  切换并记忆（`[ui] advanced`）。分析与后续路线见 `lock_frame/docs/UI_CLIENT_PLAN.md`。

### feat(ui): DPI 缩放 + 键盘操作（P1）(2026-11-19)

- **真正的 DPI 缩放**：界面仍按 1200×620 逻辑坐标布局，绘制时用
  `MM_ANISOTROPIC` 把逻辑坐标映射到客户区，字体按目标尺寸重新栅格化（不是拉大
  位图）。窗口创建/`WM_DPICHANGED` 都按 DPI 重算，125% / 150% 下卡片和字号一起变大。
- **界面缩放（高级设置 → GPU / 显示器）**：自动 / 100% / 110% / 125% / 150% /
  175% / 200%，写入 `[ui] scale_pct`；手动模式下不跟显示器 DPI 变，适合视力/电视场景。
- **键盘**：简易模式 Tab / Shift+Tab 循环焦点（2px 焦点环），Enter/Space 触发，
  Esc 取消；打开的下拉列表支持 ↑/↓ 选择 + Enter 确认。
- 鼠标坐标统一经 `to_logical()` 换算，避免缩放后"看得见点不到"；裁剪改用
  `IntersectClipRect`（`SelectClipRgn` 吃设备坐标，缩放后会裁错）。
- 实测（3840×2160 @125%，Win11）：自动缩放窗口 1500×775、文字清晰；ini 强制
  150% 时窗口 1800×930、布局正确；Tab 焦点环与高级模式卡片均正常。

### feat(ui): 启动命令折叠 + 内嵌日志查看器（P1）(2026-11-19)

- **启动命令默认折叠**：高级模式「启动 → 启动命令」只占一行，右侧「展开 ▾ / 收起 ▴」，
  点击切换并把状态写进 `[ui] show_cmd`；折叠时卡片高度与普通行一致。
- **内嵌日志查看器**：简易模式状态卡新增「查看日志」。面板显示最近 **400 行**
  （consumer stdout + UI 的 `[ui] …` 消息），等宽字体；`WARNING` / `ERR` / `失败`
  高亮；支持滚轮、PageUp/PageDown、Home/End、Esc；右上「刷新 / 关闭」。
  打开时取快照，运行中的新行不会让列表跳动；完整日志仍写 `lock_frame_ui_HHMMSS.txt`。
- 实测：面板空态与真实 `[ui]` 行渲染正常；折叠行点击后 `show_cmd=1` 并展开成
  三行完整命令；重启 UI 后两种状态都保持。

### feat(ui): 游戏库 + 首次运行欢迎页（P1 收尾）(2026-11-19)

- **游戏库**：按游戏 exe 所在目录记住名称 / 倍率 / 输入源 / 锁帧（`[games]` +
  `[gameN]`，最多 16 个）。安装成功、成功启动、改 fakerFG 输入源时写入；
  启动 UI、选中游戏窗口、从「已用游戏 ▾」下拉切换时恢复。卡片 1 显示
  「已安装 · 游戏名：目录」，游戏标题变化/重开窗口不再丢配置。
- **首次运行欢迎页**：`[ui] welcomed=0` 时显示一次，三条要点（双卡 / 无边框 +
  开 DLSS FG / 三步走）+「知道了，开始」；Enter/Space/Esc 也能关，之后不再打扰。
- 实测（fakegame / fakegame2 两个目录）：启动恢复到 3x；下拉列出两个游戏并
  切换后变成 4x 且 `game_dir` 落盘；欢迎页点「知道了」后 `welcomed=1`，重启不再出现。

### feat(ui): 本机游戏扫描 + 单页抽屉 + 悬浮说明 (2026-11-19)

- **本机游戏扫描**：`扫描本机` 读 Steam（`libraryfolders.vdf` + `appmanifest_*.acf`）、
  Epic（`LauncherInstalled.dat`）、GOG 与三条 Uninstall 注册表，收"游戏名 + 安装位置"；
  过滤运行库/Steamworks/Proton/反作弊等噪音，路径小写去尾斜杠去重，上限 200 条。
- **游戏库面板**（替代原来的「已用游戏 ▾」）：名称 / 来源 / 状态 / 路径一列展示，
  选中后可直接「使用此游戏」「安装 / 更新」「还原」「删除」（删除只删记录）。
  索引：已解析的 exe 目录优先，安装根目录次之；只有根目录时向下 3 层找主程序。
- **高级设置改为右侧抽屉**：不再整页切换，三步流程始终在下面；抽屉单列卡片、
  滚轮翻页、Esc 或 [完成] 收起，`[ui] advanced` 记忆。
- **去重**：移除标题栏启动/停止（与第③步重复）、侧栏"手册"（指向不存在文件）、
  状态卡第三个日志按钮；"打开目录"移进日志面板表头。
- **悬浮说明**：停在控件 450 ms 后弹出说明框（自动换行、避让窗口边界），覆盖三步
  流程全部按钮、抽屉全部行、日志/游戏库主要按钮。
- 实测（3840×2160 @125%）：扫描出真实 Steam 库（Persona 5 Royal / Warframe /
  The Witcher 3 等）；抽屉遮罩下三步仍在、Esc 收起；2x 与"固定倍率"悬停均出说明。

### style(ui): 布局/尺寸统一 + 交互打磨 (2026-11-19)

- 窗口 1200×620 → **1200×720**，重排三步卡与状态卡：卡片头统一 40 px、间距 16、
  外边距 24，状态卡按"徽章行 / 数据 / 建议 / 就绪胶囊 / 日志"分层。
- 引入设计 token：`RAD_CARD=10`、`RAD_CTL=7`、按钮 30/36/64 三档、字阶 22/16/15/13。
- 步骤头改为 24 px 圆形数字徽章（完成变绿）；标题栏加品牌点、最小化/关闭悬停高亮；
  侧栏配置项 40 px 行 + hover、底部固定"高级设置"和版本号；侧栏新增"当前游戏"
  摘要（名称 + 目录），把中间留白变成上下文。
- 交互：可点击控件显示**手型光标**；游戏库底部按钮在无选中时显示**禁用态**；
  抽屉选项行 hover 整行高亮；游戏库/抽屉支持 PageUp/PageDown/Home/End；
  日志面板行高 18→20、按钮 26→30；下拉/输入框宽度 210→240（长文案不再截断）。
- 一致性修复：抽屉里的"游戏配置"和"游戏配置检查"改用**游戏库选中的目录**，
  不再指向当前前台窗口（VS Code 等）。
- 实测（125%）：窗口 1500×900；三卡/状态卡间距均匀；抽屉与游戏库布局正常；
  悬停 2x 与"固定倍率"均弹说明；手型光标在各按钮上生效。

### feat(ui): 自动发现游戏窗口 + 内置帮助面板 (2026-11-19)

- **自动发现游戏窗口**：每 4 s 枚举一次顶层窗口，找到已加载
  `xess_mfg_producer.dll` 的进程就自动选中、读取它的 `fakerFG.ini` 与游戏库配置，
  并在状态卡提示"已自动选中游戏（直接点启动即可）"。只在未运行且用户没手动选过
  窗口时生效；同一个窗口只提示一次。省掉新手的"刷新列表"这一步。
- **内置帮助面板**（F1 / 侧栏「帮助 / 快捷键」）：三步流程、当前热键与全部快捷键、
  三个高频问题（看不到窗口 / 部分插帧 / 单卡）、版本与协议；Esc/F1/[关闭] 关闭。
  不再依赖 exe 旁边是否存在 `docs/UI.md`。
- 侧栏底部顺序：帮助 → 高级设置 → 版本号。
- 实测（125%）：F1 打开/关闭正常；帮助文案与当前热键一致；自动发现逻辑在无
  producer 时静默，不影响手动选择。

### feat(ui): 安装包完整性校验（启动一次，不采样）(2026-11-19)

- 启动时读取 exe 同目录的 `SHA256SUMS.txt`，用 BCrypt 对**本机存在**的条目逐个算
  SHA-256 并比对；不做周期采样，运行期零开销。
- 结果显示在抽屉「环境自检 → 安装包完整性」：`OK（N 个文件）` /
  `M/N 个文件校验失败` / `未提供 SHA256SUMS.txt（开发构建）`；
  Intel 运行时等未随包分发的文件自动跳过。
- 同时写入 `lock_frame_ui.log`（`sha256: checked=… bad=…`）便于诊断。
- 实测：正常包显示 `OK（2 个文件）`；篡改一个文件后显示 `1/2 个文件校验失败`。
- 顺带微调：标题栏品牌色块改为 14×14 圆角方块；卡片 1 的两行文字间距 +2 px；
  状态卡底行移到卡片内 8 px，避免贴边。

### feat(ui): 抽屉键盘补全 + 复制命令/日志 (2026-11-19)

- **抽屉键盘**：`Tab / Shift+Tab` 或 `↑ / ↓` 在可操作行之间移动（蓝色焦点环、自动
  滚到可见），`Enter / Space` 激活：开关切换、数字 +1、打开下拉、录制热键、展开命令；
  下拉打开后 `↑ / ↓ + Enter` 选择、`Esc` 关闭。抽屉底部改为带背景的提示条。
- **复制**：抽屉「启动命令」展开后「复制」把完整命令行写入剪贴板；日志面板新增
  「复制全部」，把最近 400 行写入剪贴板（本地操作，不联网）。
- 实测（125%）：Tab 聚焦"固定倍率"→ Enter 弹出 2x/3x/4x → Enter 选中；
  日志「复制全部」后 `Get-Clipboard` 能读到日志内容。

### feat(ui): UIA 可访问性（讲述人 / NVDA）(2026-11-19)

- `WM_GETOBJECT` 返回自定义 UIA provider：窗口 + 简易流程 13 个按钮 + 运行状态文本，
  含 Name / ControlType / AutomationId / BoundingRectangle（按 DPI 映射到屏幕坐标）。
- 按钮实现 **Invoke 模式**（`PostMessage(WM_APP+2, id)`，在 UI 线程执行与鼠标相同的动作）；
  `SetFocus` / Tab 时发 `UIA_AutomationFocusChangedEventId` 与 MSAA 焦点事件；
  `ElementProviderFromPoint` / `GetFocus` 支持命中与焦点查询。
- 模态面板（抽屉/日志/游戏库/欢迎页）期间子元素标记为 offscreen + disabled。
- 实测：用 PowerShell `UIAutomationClient` 枚举到窗口 + 13 Button + 1 Text；
  对 `AutomationId=logview` 调 `InvokePattern.Invoke()` 成功打开日志面板。
- 说明：provider 为进程级单例（不 delete），避免 UIA 异步回调悬空；朗读顺序/措辞
  仍需在装有讲述人或 NVDA 的机器上复核。

### feat(ui): 多语言（中文 / English）(2026-11-19)

- 集中翻译表 + `txt()` 绘制时查表 + `T(zh,en)` 处理带参数的动态文案；查不到回退中文。
- 抽屉新增「界面语言 / Language」，写 `[ui] lang`，切换后立即重绘。
- 英文覆盖：标题栏、侧栏、三步卡、运行状态（徽章/建议）、帮助面板、游戏库、日志面板、
  抽屉卡片名/行标签/主要选项、版本与协议、就绪胶囊、快捷键提示。
- 文案长度适配：就绪胶囊按翻译后文本重新测宽；按钮文案缩短（Restore folder）。
- 实测（125%）：`lang=1` 启动，三步卡/状态/帮助/抽屉均为英文；中文模式不受影响。
- 暂未翻译：部署结果弹窗细节、部分 toast、悬浮说明、日志正文（原始输出）。

### feat(ui): 游戏侧 fakerFG.ini 必要参数接入 (2026-11-19)

- 抽屉「游戏侧配置」新增两项（加上原有输入源，共三项必要参数）：
  - **MV/深度传输比例**：写 `StreamPercent` + `StreamPercentMv` + `StreamPercentDepth`
    （100 / 150 / 200 / 400 四档，对应 1:1、每维 2/3、1/2、1/4）；
  - **游戏侧日志 (Log)**：`Log=1/0`。
- 写入时同时固定 `FgOutput=Capture`、`Passthrough=0`、`RealSwapchainObject=1`。
- **逐游戏独立**：路径优先游戏库选中目录，其次当前游戏窗口的 exe 目录；每次写入前
  备份 `fakerFG.ini.bak_<tick>`，每个游戏目录只保留最新 8 份。
- **读回**：启动 / 选窗口 / 切换游戏库 / 自动发现游戏时读取这三项，界面显示真实值。
- 写入后提示"重启游戏生效"（producer 每会话只读一次 ini）。
- 实测：把比例改成 200%、关闭日志后，fakegame 的 `fakerFG.ini` 变为
  `StreamPercent/Mv/Depth=200`、`Log=0`、`RealSwapchainObject=1`，生成 2 个备份；
  重启 UI 后控件显示 200% 与关闭。

### feat(ui): 主程序自动解析，用户只需安装/还原 (2026-11-19)

- 新增 `resolve_game_exe`：读 PE 导入表（d3d11/d3d12/dxgi）+ 名称相似度 +
  `-Win64-Shipping` + 体积 + 目录深度打分；黑名单跳过启动器/卸载/崩溃/反作弊/
  redist；<1 MB 跳过。最高分 ≥40 才采纳。
- 游戏库条目录入 `exe=`；选中条目或点安装时自动解析并写回，行内显示
  「主程序：Game-Win64-Shipping.exe」；用户不需要知道包体结构。
- 已部署游戏：`find_deployed_dir` 在根目录 3 层内找 `fakerFG.dll`，直接认部署目录。
- 「手动 exe」改为 **「自选 exe…」安装**：选 exe → 生成/更新条目 → 询问安装，
  一步完成配置 + 安装（高级玩家入口）。
- 安装成功后再次解析 exe 并落盘，后续「使用 / 还原 / 删除」都基于该目录。
- 实测：`biggame` 含 `Game.exe`、`Launcher.exe`（4 MB）、
  `bin/Win64/Game-Win64-Shipping.exe`（5 MB）→ 选中后解析为 Shipping（Launcher 被
  黑名单排除）；点「安装 / 更新」后 fakerFG.dll / dxgi.dll / producer / fakerFG.ini
  全部落在 `bin/Win64`，条目录入 `dir` 与 `exe`。

### feat: lock_frame v30 —— `frameRenderTime` 改为连续限斜率 feed（Wuchang 实测通过）(2026-11-19)

- **`--fg-rt-mode auto` 不再是 quant/raw 二选一**，而是一个连续量
  （`rt_auto_feed_ms`）：`target = max(m·vblank, 中位数)`，2% 死区，
  **每批最多 +0.5 ms、恢复时立即回落**。
  - 40 fps → 25.0 ms（1 张/vblank，速度准确）；
  - 进入 33 fps 场景 → ~0.35 s 内 25→31 ms 平滑爬升，**没有模式跳变**；
  - 回到 40 fps → 立即回 25.0。
  实测（Wuchang，120 Hz、3x、输入 32.8–40.1 fps、39 个统计窗口）：
  `rt ≥ 25.0` 与 `rt ≤ 输入+1 ms` **全部成立（0 次超额）**，`dropped=0`，
  运行中 0 次配对失败；慢场景输出 97–107 fps。主观：**"整体稍慢但是顺"**（已确认）。
- **抽帧门加 2% 死区**：`q > 0.98`（输入高出目标不到 2%）时不丢、额度重置为 1。
  工作点正好在 40 fps 上下时，一次丢弃换来一个 2× 内容间隙，不值得。
- **修掉 `presentation is not vblank-paced` 误报**：现在只有"显示帧率也明显低于批次应产出"
  才报（`total` 变小是 XeFG 异步 pacing 的正常现象）。
- **UI 移除"低帧自适应"开关**（旧算法）：它会传 `--interp-cadence`，按设计让时隙抽帧门
  让位并每批换倍率（3x/4x 闪烁/重影）。现在界面只暴露固定倍率 2x/3x/4x，v30 的连续 feed
  就是默认路径。`--interp-cadence` 仍保留为命令行 A/B，UI 不再发送。
  同时删掉 `lock_frame_ui.ini` 的 `cadence` 键（旧文件里的该键会被忽略）。
- 已知未做：`--lock` 仍用 `GetTickCount64` + 整数 ms（60 档 58.8 fps）；XeLL 的 `sleep`
  仍在 `SIMULATION_START` 之后；`--lock` 的非整除 Bresenham。
  `--slack` / `--slack-auto` 实测更差（hold 造成两批背靠背），不要用于这条链路。

### feat: lock_frame v29 —— 默认路径改用游戏时钟 + 时隙抽帧门 (2026-11-18)

- **测速换源（默认生效）**：`frameRenderTime` 与 cadence 判决改用 producer 发布的
  **游戏自己的 present 时钟** `FrameShared.frame_qpc_ms`（1 ms 分辨率），不再用 WGC 的
  `SystemRelativeTime`。后者被量化到整 vblank：39 fps 和 42 fps 都读成 25 ms，
  于是 39 fps 时 XeFG 按 25 ms 摊 3 张、剩下的 3.6 ms 什么都不显示——
  这就是"每批尾部的小停顿"。现在 39/35 fps 会按真实间隔摊开（1,1,2）。
- **默认 `--fg-rt-mode` 从 `quant` 改为 `auto`**：供得上（输入 ≥ 刷新/(interp+1)）时仍贴
  `m·vblank`（落面板网格）；供不上时喂真实间隔（摊开而不是定格）。
- **时隙抽帧门（默认开启）**：`--mode 2x/3x/4x` 下按
  `target = 刷新/(interp+1)` 用漏桶保留输入，超供（41–43 fps）不再交给 DWM 抽帧。
  丢帧发生在**颜色拷贝之前**，并把该槽立即 ACK；漏桶让丢帧均匀
  （42 fps + 3x：每 21 帧丢 1 张，实测保留 40.0 fps）。`--no-slot-gate` 回到旧行为。
- **观测**：OSD 的 `in fps` / `game ms` 改用游戏时钟；报告每个窗口新增
  `game clock: in … target … kept/dropped …` 与 `in(vb)`（0.5 vblank 分桶）两行；
  `drop` 计数包含门丢弃。
- **不改变**：颜色仍走 WGC、marker 精确配对、`--upsample`（MV/depth 150%）、
  prefetch 强制关闭、固定倍率（不做动态倍率）。
- **已知未做**：`--lock` 仍用 `GetTickCount64` + 整数 ms（60 档 58.8 fps）；
  XeLL 的 `sleep` 仍在 `SIMULATION_START` 之后；`--lock` 的非整除 Bresenham。
- 依据与验收步骤：`docs/design/LOCK_FRAME_IMPROVEMENTS.md` §11/§12
  （离线仿真实测：保留率精确等于 target，丢帧最大连续 1–2 张）。

### feat: lock_frame 1.0.0 正式版基线（固定倍率 + 输入源 + 开源发布）(2026-11-17)

- **固定倍率 2x / 3x / 4x**：UI 从 30x4/40x3 两个写死模式改为三档；consumer
  `--mode 2x|3x|4x`，旧 `30x4/40x3` 保留别名。建议输入锁帧按目标显示器刷新率
  计算（120Hz：60/40/30），不能整除时 UI 提示。
- **fakerFG 输入源可选**：UI 写入/备份游戏目录 `fakerFG.ini` 的 `FgInput`
  （DLSSG / NVNGX / Upscaler / FSR），并强制 `FgOutput=Capture`、`Passthrough=0`。
- **UI 环境自检**：版本/协议、GPU 数量、`libxess_fg`/`libxell`、游戏配置四项
  直接显示；新增“游戏窗口”下拉，按目标显示器取刷新率。
- **版本与发布工程**：`version.h`（1.0.0）、`--version`、日志轮转（保留 8 份）；
  新增 `lock_frame/release.sh`、`deploy_game.bat`、manifest/SHA256 与 GPL 源码包，
  已生成并校验通过 `dist/lock_frame-1.0.0*.zip`。
- **开源**：项目继续以 GPL-3.0 发布；发布包随附 `LICENSE`、`NOTICE` 与对应源码。
- **保留** `--unlock-mfg`：2x 不发送；3x/4x 继续发送，保证非 Intel 卡开箱即用。
  该开关会修改 Intel DLL，风险已写入发布说明。

### feat: 颜色入槽（协议 v22）—— 彻底删除 WGC (2026-11-15)

- **一条发布 = 一帧**：producer 在同一个槽、同一个 fence 下发布 **颜色（stream 0）+
  MV + depth + 元数据**。颜色按游戏 backbuffer 的源格式原样复制（4K FP16 66.4 MB /
  HDR10 R10 33.2 MB），producer 不做转换。方案与成本见
  `docs/design/COLOUR_TRANSPORT_V22.md`。
- **WGC 整条路径删除**：capture item / session / frame pool / FrameArrived /
  颜色队列 / capture 线程 / `--pool` `--colour-queue` `--wgc-minint` `--legacy-poll`
  `--slack*` `--jbuf`。consumer 不再抓屏。
- **配对机制删除**：marker 四角绘制与解码、present-ring、`pair_slot_for_wgc`、
  `pair:` 指标。槽号就是身份；`--no-marker/--pair-bias-ms/--no-pair-key` 仅作为
  旧命令行兼容被忽略。
- **consumer 主循环改为 publication-driven**：取**最旧未处理**的发布 → `wait_slot`
  （颜色/MV/depth 都到齐）→ 一次拷贝 → convert → XeFG → Present；ACK 在下一帧
  `gpu_throttle()` 之后写（含义 = 本地拷贝已退休）。
- **动态逐帧**：输入时钟只剩 `FrameShared.frame_time_ms`（每呈现帧一次的 EMA）；
  `arr/pin/pq` 三套估计里的 `arr` 不再参与决策。7% 的非整除失真仍是算术下限，
  但不再被 WGC 的到达抖动放大。
- **内存**：`kMaxSlots` 8 → 4（颜色不再滞后），共享映射 4×(66+33+33) ≈ 531 MB。
- **暂停/加载**：没有新发布超过 250 ms 时隐藏覆盖窗口（否则会把最后一帧冻结在
  菜单上），发布恢复后自动显示。

### feat: mgpu_ui 对接到 xess_mfg_consumer.exe（图形界面版启动器）(2026-11-14)

原来的 `mgpu_ui.exe` 是 bridge/ReShade 时代的前端，只认 `bridge.exe`。现在对接当前链路：

- **目标进程**：`bridge.exe` → **`xess_mfg_consumer.exe`**（同目录查找、启动、退出检测、
  残留进程清理、日志重定向 `mgpu_ui_*.txt`、状态栏文案全部改名）。
- **窗口选择即配对**：UI 枚举可见窗口，选中的窗口以 `--hwnd <hwnd>` 传给消费端
  （消费端本来就支持：它从中解析出进程→WGC 抓那个窗口），所以不用手写 `--game`。
- **命令行补齐当前流水线**：`--unlock-mfg --fg-ui none --fg-rt-mode auto --pub-fifo --report 40`，
  其余仍由界面选项生成（倍数/帧池/色彩空间/输出模式/叠加层…）。
- **新增两个开关**（采集卡片）：`不传深度 (--no-depth)`、`关闭标记配对 (--no-marker)`；
  两者都进 ini（`no_marker` 是新增键），鼠标悬停/切换时有中文说明。
- 「建议锁帧」默认从 24 fps 改成 **40 fps**（120 Hz 的三整 vblank），并保留"只是提示"的语义
  —— 真正要锁的是游戏本身，`--lock` 不用来压帧率。
- 打包：`scripts/make_release_fg.sh` 会把 `build/mgpu_ui.exe` 一起放进发布包的 `game/`
  （UI 按自身路径找消费端，因此必须同目录）。


- **定位**：新增 `SLOW frame` 日志（每帧 >55ms 打印 decode/layout/colour/inputs/present
  各阶段耗时）后，慢帧全部落在一处：
  `SLOW frame: total=149.3ms decode=0.1 layout=0.0 colour=147.9 inputs=0.2 present=1.2`
  —— 其余阶段都 <9ms，**只有 `colour` 是 120~220ms**。
- **原因**：颜色 sink 只有一张共享纹理。D3D11 的 66MB `CopyResource` 与 D3D12 转换
  pass 读的是同一张；XeFG 突发提交时，驱动把这次拷贝串到上一帧 draw 之后，
  `CopyResource` 阻塞 120~220ms → 整帧冻结 → WGC 丢掉中间 5 帧（标记序号一次跳 5）。
- **修复**：sink **双缓冲**（`g_shared11_pair[2]` / `g_shared12_pair[2]` /
  每张一个 keyed mutex，共享名 `mgpu_colour_sink0/1`），每处理一帧翻转 `g_sink`；
  SRV 堆放 2 个描述符，转换 pass 用 `base + g_sink*stride` 绑定，**永远采样刚写的那张**，
  另一张留给上一帧的 draw 继续读。
- 配套：GPU 落后时的"跳过拷贝复用上一帧"缓解保留，阈值 = 落后 **>1 帧**（双缓冲只给
  一帧余量，落后 2 帧时正撞车）。
- 实测（52 秒 / 2069 帧）：`HITCH` 6 → **2**，`SLOW frame` 6 → **1**，
  `marker: decoded=2069 miss=3`；剩 1 次仍是 `colour=168ms`（GPU 落后 2 帧），阈值改回
  >1 后由"跳帧复用"兜住。

### feat: 标记成为配对主键 + 消掉最后 8 次 reset + 清理 16×16 scar 遗迹 (2026-11-14)

- **reset-history 降级**：`g_force_reset_history` 现在只在**标记解不出来**时设置
  （状态机的不连续分支 + pub-fifo 缓存 miss 分支）。标记能识别本帧时，MV/depth 就是
  这一帧的，扔掉 XeFG 历史只会白丢一次插值——实测一轮 8 次 reset 正好对应 10 次
  >150ms 顿挫中的 8 次。
- **pub-fifo 用标记的 frame_id 查队列**：`g_marker_seq` 在状态机之后被设为
  `frames[marker_slot].frame_id`，于是 `pair_slot_for_wgc()` 按 frame_id 命中 slot 并让
  `g_pair_seq` 等于该 id，队列查询不再因为 present-ring 的另一套计数器而落空
  （落空就会走 "falling back to head" + 强制 reset）。
- **清理 16×16 scar 遗迹（全部为死代码）**：
  - 消费端：`marker_repair()`（D3D11 pre-PQ 修补）、`marker_tile_stats` /
    `marker_pixel_value`、`g_marker_fill_*`、转换前对 sink 的 copy 修补块、`scar_path`
    及其 fence wait、`--scar-exact` / `--marker-repair` 开关；
  - fakerFG：`scar_dbg_*`（`scar_tile.raw` 回读）、`marker_norm_init()` /
    `pass_norm_tile()` / `marker_norm.hlsl`（构建脚本也删了该着色器）、
    `producer_record_scar()`；`xefg_set_marker_color()` 保留为 no-op（upscaler 车道仍在调）；
  - producer：`MgpuProducerRecordScar()` 实现（现在返回 5 = 不支持，api 表里为 nullptr）、
    `g_scar_valid/x/y/format`、`fr.marker_valid/…/marker_tile` 的写入（字段保留为 reserved）；
  - `streams[kStreamScar]` 的布局/放置保留（reserved，避免动共享协议），但已无人读写。
- 结果：scar 这条线只剩"画 4 个 6×6 → 解码 → 完成配对 → 在转换 pass 里回填"。

#### 配对主键（同轮，先于上面的清理）

- **配对改为标记主键**：解码出 `payload24` 后，直接在 publication 槽里找同源
  `frame_id`（`frames[s].frame_id & 0xFFF == (seq12+1) & 0xFFF`，`+1` 是 staging 晚一帧
  的固定延迟），命中的槽就是本帧的 MV/depth；时间戳 / `--pub-fifo` / present-ring 只作为
  快速猜测，猜错时以标记为准并打 `marker pair: heuristic slot X -> Y`。
  **不再用 present-ring 的 seq**（它被游戏和消费端两个进程以不同计数器共写，120 帧只
  匹配 7 次，会把状态机拖进 miss/coast）。
- **实测（`--lock 0` + 游戏锁 40fps，约 4900 帧）**：
  - `marker: decoded=4907 restored=0 dup=0 fallback=0 miss=0`；
  - `frame_id = seq12 + 1`，即"晚一帧"精确为一帧；
  - `marker pair: heuristic slot 0 -> 3` 出现 **23 次**（标记真的在纠错，而不是只做确认），
    且这 23 次**没有一次**伴随 HITCH；
  - 观感：**完全没有错位/重影**。
- 剩余顿挫：`HITCH` 中位数 68.7ms、仅 10 次 >150ms（全程），与
  `reset-history: forced after an MV/publication discontinuity`（8 次）几乎一一对应——
  下一步可用标记把这 8 次从"reset 历史"降级为"复用上一帧"。

### fix: 标记解码的 Map 调用用错 API + `--lock` 造成的偶发严重掉帧 (2026-11-14)

- **解码一直失败的真因**：`ID3D11DeviceContext::Map` 的 `MapType` 和 `MapFlags` 是**两个
  参数**，原来写成 `Map(res, 0, (D3D11_MAP)(D3D11_MAP_READ | D3D11_MAP_FLAG_DO_NOT_WAIT), 0, &m)`，
  把 flag 或进了 MapType（0x10001）→ 每次 `E_INVALIDARG (0x80070057)`，`fail=2879/ok=0`，
  所以 `readable=0/4`、`decoded=0` 而配对全靠 present-ring 兜底。改成
  `Map(res, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m)`。
- **偶发严重掉帧（GPU 却有空闲）**：`--lock 40` 在游戏实际 ~35fps 时唯一作用就是"丢帧"，
  把输入节奏打乱（`input=28.1ms = 3.37 vblanks`, `NOT-EXACT`），XeFG 于是周期性退回 1X
  （`xN=1`）→ 200~440ms 的顿。去掉 pacing（`--lock 0`）并把游戏锁 40fps 后：
  - `HITCH` 151 → **2**，`drop` → 0，`not in slots` 92 → 1；
  - `cadence: input=25.0ms = 3.00 vblanks, avg 2.00x, 80.0 fps out`；
  - `arr(vb): 3x=40/40`（输入稳定在 3 个 vblank）。
  剩下 2 次顿挫都紧跟在 `reset-history: forced after an MV/publication discontinuity`
  之后，属于单帧 publication 抖动；标记解码修好后可精确命中 slot，预期进一步消失。


原来标记带的是 `seq32 | CRC32`（64 bit，四块 8×8），现在换成**短雪花**：

- **ID**：`payload24 = sequence12 | low12(ms)`，由 `snowflake_id()`（41+10+12 标准布局）
  派生；完整 64 位时间戳/机器号对标记没用（消费端有 present-ring 时间），12 bit 序列
  + 12 bit 毫秒已经保证逐帧不同。校验用 **CRC-8**（poly 0x07），不再是 CRC32。
- **码**：`code32 = payload24 | crc8<<24`，四个角各带 1 字节（XOR 0x5A）。
- **几何**：每块 **6×6**（3×3 格，格 2×2 px），四角合计 **144 px**（原 256 px，-44%）。
  第 9 格是"参考格"：数据位全 1 时画黑，否则画白 —— **块内永远同时有两级**，
  min/max 阈值解码不会遇到纯色块（穷举 256 个字节值 + 20000 帧模拟，0 失败）。
- **配对**：消费端解出 payload24 后，用 `ring_seq & 0xFFF == sequence12` 把 24 bit
  还原成**完整帧 id**，其余状态机（重复帧、publication 匹配）完全不变。
  解不出时仍走 present-ring 兜底。
- **双缓冲 staging**：`MAP_FLAG_DO_NOT_WAIT` 最初配单张 staging 时**永远读不到**
  （copy 刚在同一 immediate context 上提交，`WAS_STILL_DRAWING` 几乎 100%，
  实测整轮 `decoded=0`）。改成两张 staging 交替：本帧写 A，读上一帧写好的 B，
  标记因此晚一帧，配对同时接受 `ring_seq` 与 `ring_seq-1` 的序列匹配。
- 开销不变：仍是 4 次小 copy + 1 次非阻塞 `Map`，回填仍在转换 draw 里。

#### 历史：四角标记的收尾（几何对齐 / 转换内回填 / 部署漏名）(2026-11-14)

**结论：scar 这条线跑通了，游戏侧那一帧带标记属于设计，消费端呈现的画面干净。**

- **几何最终形态**：一块 16×16 拆成四块 **8×8**，按画面分辨率贴四顶角
  （`margin = w/32, h/32`，4-px 对齐；4K 为 (120,64)/(3712,64)/(120,2088)/(3712,2088)）。
  64 位码（seq32 | CRC32）分四片，每块 16 bit，每片 XOR `0x5A3C`，避免
  `seq>>16` 那种全 0 片变成纯黑方块（解码器靠 min/max 阈值，读不出纯色块）。
- **生产端一次 draw 画完四角**：原来是 4 次带 scissor 的 draw，实测**只有第一块落到画面上**
  （角 0 对比度 4.6，其余三块恰为 0.0）。改成一次全屏 draw、PS 里判断落在哪个角、其余
  `discard`，四块一起正常（`readable=4/4`）。
- **回填搬进转换 draw**：原来用 `bbres → 8×8 temp → bbres` 的 copy 回填（每帧 16 次 copy
  + 16 次 barrier），现在由 `ps_main` 在采样时把四角像素的 UV 向内偏移一个块宽——零额外
  pass、零 copy、零 barrier，1:1 直通下即像素级偏移。**两个插值输入因此都是干净的**。
- **解码不再阻塞**：四个角块合并到一张 8×32 staging（一次 `Map`），并改
  `MAP_FLAG_DO_NOT_WAIT`；解不出码时配对走 present-ring 兜底。阻塞式 `Map` 是每帧一次
  GPU→CPU 同步，GPU 越忙代价越大（观感："不转视角 80，一剧烈转视角 70"）。
- **回填门控**：只在 present-ring 有新 commit（= fakerFG 当帧确实画了标记）后 250 ms 内执行。
  之前无条件执行，在不开 FG（生产端不画标记）时会把四角真实画面替换成邻居副本，
  看起来就是"四个 16×16 的疤，开关 FG 都在"。
- **踩过的三个坑（都已修，写在这里防止重复）**：
  1. **游戏加载的是 `dxgi.dll`，不是 `fakerFG.dll`**。只更新 `fakerFG.dll` 时游戏仍跑旧版，
     于是标记一直画在旧的"象限中心 16×16"位置（4K 下 (960,536)…，正好是居中 1920×1080
     矩形的四角），消费端却按新几何去读四顶角 → 永远解不出、永远不擦。部署必须
     `dxgi.dll` + `nvngx_dlssg.dll` + `fakerFG.dll` 三名字一起（用
     `scripts/deploy_faker_fg.sh`）。
  2. **present ring 的映射是 `FILE_MAP_READ`**：用 `InterlockedCompareExchange64(&ring->write,…)`
     读计数会因读-改-写在只读页上触发 `0xC0000005`（消费端启动即崩）。改用对齐 volatile 读。
  3. **转换 shader 是运行时编译的字符串**，`cl` 查不出语法错误（曾因 cbuffer 改名 `pad0`
     导致 `ps_prep_depth` 编译失败、消费端无法启动）。现在构建期用 `scripts/extract_hlsl.py`
     + `fxc` 校验四个入口。
- 诊断开关（默认全关，A/B 用）：`marker_disable.on`（生产端不画）、`marker_repair_off.on`
  （消费端不回填）、`dump_frame.on`（整帧回读 `frame_dump.raw` + `marker_corners.raw`，
  `scripts/raw_to_png.py` 可转图）。
- 消费端崩溃地址可用 `build/xess_mfg_consumer.map`（构建已加 `/MAP`）符号化。

#### 历史：四角 16×16 + 传图块回填（已废弃）

### feat: scar 拆成四角 4×8×8 分块 + 邻居采样回填（最终形态）(2026-11-14)

前几轮把 16×16 的 scar 放在四个象限中心、每块各自携带完整 64 位码，回填又依赖
"producer 传回的原始图块"（实测那张图块与 WGC 实际抓到的内容不一致，回填出的是
一块死色）。现在按最小耦合的方式定版：

- **几何**：一块 16×16 = 四块 **8×8**，位置由**画面分辨率**算出，贴四角
  （距边 32 px）：左上 `(32,32)`、右上 `(w-40,32)`、左下 `(32,h-40)`、
  右下 `(w-40,h-40)`。总面积仍是 256 px，但每次要补的面积只有 8×8。
  `mgpu_shared.h`: `kMarkerSide=8`、`kMarkerCells=4`（2×2 px 见方）、
  `kMarkerBits=16`、`kMarkerMargin=32`、`marker_corner()`。
- **码的分片**：64 位码（seq32 | CRC32）平均切成 4 份，第 i 块携带
  bit[16i..16i+15]，`marker.hlsl` 新增 `dim` 常量（4 = 4×4 格 = 16 bit；
  8 = 老的 16×16 64 bit），`pass_marker()` 多一个 `dim` 参数。
  → 任意一块被 HUD 挡住就无法配对（这是拆分的代价，换来更小的补丁面积）。
- **消费卡解码**：对四角各取 1 次 8×8 staging，4×4 格取 2×2 均值、min/max
  阈值判 bit，四块拼回 64 位再校验 CRC（保留单 bit 修复）。任一块读不到就
  `marker decode: only n/4 corner blocks readable` 并放弃本帧配对。
- **回填（用户要求的一对一采样）**：转换 draw 之后，在本帧 list 上把每个 8×8
  块用**同一帧、同一 buffer 的内侧邻居**（同高、向内一个块宽）覆盖
  `bbres→8×8 temp→bbres`；源和目标都在上屏的 backbuffer 里，所以颜色/时间
  一致，不涉及跨卡图块。
- **两帧都要干净**：回填在**每帧** Present 之前完成，所以 XeFG 缓存的上一帧和
  本帧都是已回填的画面，插帧输入不会带 scar。
- 诊断开关：`probe_marker.on` 把左上角的 8×8 块**复制到画面中心**——中心出现
  黑白块说明"拷贝写进上屏 buffer 是可见的"（这一条决定回填用 copy 还是 draw）；
  没有则说明 copy 被丢掉，回填必须改用 draw。

#### 历史：四角 16×16 + 传图块回填（已废弃）

### feat: scar 走 MV/depth 同一条 publication（协议 v17，GPU 直拷）(2026-11-14)

WGC 必须保留（整帧颜色过卡不可接受），但**16x16 的 scar 补丁没必要走 CPU**。
协议 v17 把它挂到和 MV/depth 完全相同的拷贝/发布路径上：

- 新增 `streams[4] = kStreamScar`（16x16 R10，cross-adapter placed，4 槽）；
- **fakerFG**：present 时把 chain buffer 的 16x16 原图 normalize 到一张常驻 R10
  texture，然后 `producer_record_scar()` 在**同一帧的 list** 上直接 `CopyTextureRegion`
  进该 publication 的 scar 槽。删除 CPU readback / `Map` / worker 线程 / pending 表 /
  20 ms 等待；Present 只多一次 16x16 的 GPU copy；
- **顺序**：`xefg_before_present()` 改为 `before_present()` → `producer_present()`，
  保证 scar 拷贝在 publication fence 之前入队；
- **producer**：布局/放置 scar 槽，`publish_frame()` 只发布 `marker_valid/x/y/format`
  （像素在同槽里），不再需要任何 tile 表；
- **consumer**：回贴改为一次 16x16 `CopyTextureRegion`（从 placed scar 槽直接读），
  在本帧 list 前对该槽的 `g_fwd` 加一个 GPU wait；不再有 CPU tile / ring 查询 /
  伸缩纠错；新增 2x2 cell 解码、CRC32、单 bit 修复；
- **配对状态机**（WGC 路径）：
  - 解出 seq -> 精确配对；
  - 同一个 seq -> WGC 重复帧（DWM/2x present）：复用上一 slot，不推进 XeFG；
  - 解码失败但 present-ring 候选 `== last+1` -> 仍按精确处理；
  - 其它 -> coast（重复上一帧）+ `resetHistory`，**不再按时间戳乱配**；
  - 每 120 帧打印 `marker: decoded/dup/fallback/miss`；
- `marker_repair()`（转 PQ 前的邻居填充）改为默认安全网：只要当帧解出了 marker
  就运行，覆盖 `--sdr` / 缩放输出等无法直接 blit 的情况。

开销：渲染卡 +1 KB/帧 GPU copy；消费卡 +1 KB 读取；跨 PCIe +1 KB/帧；
共享内存 +~256 KB。MV+depth 的预算不变，WGC 仍是颜色来源。

预期日志：`marker probe: 16x16 R10 scar tile ready` →
`marker probe: scar tile rides the MV/depth publication (16x16)` →
`marker code: seq=... crc=... ok` → `scar restore: seq=... slot=... at x,y (16x16)` →
`marker: decoded=... dup=... fallback=... miss=0`。

#### 首轮真机（CP2077）暴露并修复的两个 bug

1. **marker shader 用的是绝对 `SV_Position`**：marker pass 用全屏 viewport + 16×16 scissor，
   `SV_Position` 是渲染目标绝对坐标（960..975, 536..551），`pos>>1` 后 bit index 变成
   2144+，HLSL 移位按 5 bit 取模 → 图案变成高频重复，DWM 后是一块**均色**，CRC 永远
   失败（消费端 `decoded=0`）。旧 8×8 版本用 `pos & 7` 恰好因为原点都是 8 的倍数才没暴露。
   现在把 `origin_x/origin_y` 放进 b1.zw，shader 里先减原点。
2. **回贴没有用 `scar_path` 闸门**：`marker_restore_after_convert()` 只看
   `frames[slot].marker_valid`，解码失败（`seq=0`）时也照贴，而且因为 `scar_path` 为
   false **没有插 `g_fwd[slot]` 的 GPU 等待** → 读到生产者尚未写完的 scar 槽，把垃圾
   贴进画面（真机上的 16×16 色块）。现在 `seq == 0` 一律不贴，改由转 PQ 前的邻居填充
   兜底；而邻居填充也不再依赖解码成功——只要生产者开始发布 scar（`g_marker_seen`），
   每个新颜色帧都会跑。

#### 第二轮真机（CP2077）：菜单/暂停把链路打崩

第二轮日志里 marker 已经正常（`decoded=111`，`seq=1058..1168 ok`，`scar restore` 生效）。
12:03:03 游戏**停止 Evaluate**（进菜单：`xefg: upscaler Evaluates 0`），随后：

- 消费端把"生产者没有新发布"误判成"marker 丢了"：**每帧** `reset-history: forced ...`，
  XeFG 直接进 `ERR_OTHER`、`presented=1`、`OUTPUT FRAMES MISSING`；
- 转 PQ 前的 `marker_repair`（`g_marker_seen` 一旦为真就每帧跑）继续在 (960,536)
  用邻居猜一块 16×16 盖上去 —— 菜单里根本没有 marker，这就是画面上的**色块**。

修法：

- 每帧判断 `g_pub_fresh`（`frames[].published` 是否前进）。**没有新发布时**：
  不配对、不回贴、不跑邻居填充、不强灌 `resetHistory`，只按当前颜色原样呈现；
- `g_pub_stall_frames >= 2` 时 `mv_available = false` → 自动 `SetEnabled(0)` 切 1X，
  发布恢复后自动回到 FG（以前是每帧 reset，把库打进 `ERR_OTHER`）；
- 解码成功但 `!pub_fresh`（缓冲里残留的旧码）时不前向配对；
- 统计行扩展为 `marker: decoded=.. restored=.. dup=.. fallback=.. miss=.. pub_stall=..`，
  `restored` 应该与 `decoded` 同步增长。

#### 第三轮真机：闪烁的 16×16 块 = 在"本来没有 marker 的帧"上做邻居填充

日志统计 `decoded` 每 120 个处理帧只涨 ~30（约 25%）：fakerFG 只在有 Evaluate/发布
时画 marker，而标题的 present 频率高于发布频率，还有 FG 关闭/暂停的时段，所以**大多
数捕获帧本来就没有 marker**。

当时的 `marker_repair` 只按"有新的 publication"放行，于是在这些无 marker 的帧上凭空
把 (960,536) 涂成邻居补丁；下一帧有 marker 时又被精确回贴覆盖 —— 一帧补丁一帧正常，
就是用户看到的**闪烁色块**。

修法：邻居填充只在"这一帧解码出了 marker"时运行（只有那时才需要擦除）；没有 marker 的
帧一律不动。另外把"连续 2 帧无发布切 1X"改成 8 帧，避免卡顿时 FG 1X↔2X 反复切换
（实测 217 次 → 1 次）。

#### 第四轮真机：开 FG 才闪 = 游戏侧重复 present 的错配补丁

现象：**不开 FG 干净，开 FG 就出现闪烁的 16×16 块**。

机制：游戏的 FG 打开后，同一个渲染帧会 present 两次（第二帧是空/旧缓冲）。第一次
present 记录 scar 并画 marker；第二次 present 命中 `publish_frame` 的去重，
`record_scar` 当时返回 0（"成功"）却**并没有重新取图**，marker 还是画进了第二个缓冲。
消费端把第一帧采到的 16×16 原图贴到第二帧上 → 一帧对、一帧错 → 开 FG 才看得见的闪烁。

修法：

- producer：重复 present 时 `record_scar` 返回 **1**（不再假装成功）；
- fakerFG：rc==1 → 这次 present **不画 marker**（日志 `duplicate present (same record)`）；
- consumer：用 present-ring 的 seq 识别重复帧 —— 同一记录两次 present 落在同一个 seq，
  所以 `ring_seq <= last` 或解出的 seq 倒退时，一律 `g_reuse_frame`（复用上一帧输出），
  不再 `resetHistory`；只有真正无法配对的帧才 reset。
- 修正 `marker_out.raw` 的读回 Map 范围（之前 `Map FAILED`）。

#### 第五轮真机：菜单里黑屏+冻结 —— ring 判重复没有上界

现象：从游戏启动就开着 consumer，进主菜单后黑屏（只有 UI 有色），随后画面完全冻结，
`marker_out.raw` 显示"实心 16×16 亮块 + 周围全黑"。

日志：

```
marker: decoded=189（不再增长）  dup 每 120 帧固定 +120   ← 每帧都判成重复帧
pub_stall=962                                            ← 生产者早已停止发布
frame … jitter=(-0.493, 0.468) 每帧完全相同                ← 在重播同一帧
```

生产者停止发布后，present-ring 里永远是最后那个 seq，`ring_seq <= last` 恒成立，于是
消费端**无限 `g_reuse_frame`**：画面不再更新（冻结），并在每个复用帧里把同一个旧 scar
补丁再贴一遍（那个实心块）。菜单/暂停里 FG 打开只是让冻结更明显。

修法：

- 只有生产者仍在发布（`g_pub_stall_frames < 4`）时才允许"ring 判重复 → 复用"；
- 加连续复用上限 `g_reuse_streak < 6`（解码判重复同样受限），超过就强制呈现新颜色；
- 重复但不复用的情况（`ring_seq <= last` 且被限流）→ 按普通帧呈现，不猜配对、不 reset。

#### 第六轮真机：raw 取证定位到 norm pass 参数读成 0

`marker_in.raw`（WGC 输入）是标准 2×2 黑白码；`marker_out.raw`（回贴后的呈现块）中心却是
**一块均匀偏暗的 16×16**，周围是正常场景。说明 marker 被擦掉了，但贴进去的 scar 图是错的。

根因：`pass_norm_tile` 的参数（src_x/src_y/format/mode）走的是**描述符堆里的 CBV**，而这个
堆同时被 UI/HUD pass 复用；实际读到的常量是 0 → `src_x=src_y=0`、`mode=0`（线性，而不是
R10 的 pass-through），于是它采样了帧左上角、又按线性重新 PQ 编码，得到一块偏暗的平坦图，
贴到 (960,536) 就成了"均匀色块"。

修法：

- norm 参数改为 **b1 根常量（8×32 bit）**，并在 `SetGraphicsRootSignature` 之后设置
  （设置根签名会清空根参数）；
- 删除该 pass 对共享堆 CBV 的写入；
- 根签名 `b1` 常量个数 4 → 8（marker pass 仍只用前 4 个）。

#### 第七轮：开 FG 后游戏崩溃 —— 是我加的调试 dump 造成的

```
[12:44:56.864] marker probe: scar debug dumps armed (scar_tile.raw / scar_src.raw)
[12:44:56.869] crash: unhandled code=0x80000003 … stack … D3D12Core.dll
```

调试 dump 里有一段"从 chain buffer 拷 16×16 到 R10 读回缓冲"。chain 是 R16F（scRGB）时这就是
**格式不匹配的 CopyTextureRegion**，D3D12 直接 fail-fast。已删除该拷贝，只保留 R10→R10 的
tile dump，并且在初始化时创建读回资源（不在 Present 里分配）。

#### 第八轮：`scar_tile.raw` 证明生产端原图不可用 → 改为捕获图本地邻居填充

`scar_tile.raw`（生产端发布的 16×16，R10）：

```
R min 176 max 274 mean 210.7
first (212,169,123)   centre (215,171,134)
```

整块是一块**很暗、很平坦的暖色图**，而同一位置在呈现画面里是亮的。说明生产端从 chain buffer
采到的内容与 WGC 看到的画面在这一帧并不是同一份（跨缓冲区/时序差异）。因此"精确回贴"无论
贴得多准都会是一块异色 —— 这就是一直存在的色块。

修法：

- **默认关闭**精确回贴（保留 `--scar-exact` 供 A/B）；
- 改用**捕获图自身的 16×16 邻居填充**（转 PQ 之前）：窗口 64×48，候选"左/上/下"各 16×16，
  按"平坦度 + 与 marker 上方条带颜色距离"打分，一次 `CopySubresourceRegion` 贴入。
  补丁与画面**同帧同色**，不可能出现异色块；
- marker 只负责 MV/depth 精确配对（`decoded≈restored`，`miss=0`），不再参与颜色修补。

#### 第九轮：D3D11 写共享 sink 对 D3D12 不可见 → 把补丁搬到 D3D12 侧

截图与 `marker_out.raw` 都显示：呈现画面里是**marker 本身**（黑白格）。而 `marker repair` 日志
在跑（`(960,536) <- above`）。说明 D3D11 侧对 keyed-mutex 共享纹理的 `CopySubresourceRegion`
**没有被 D3D12 视图观察到**（而整帧的 `CopyResource` 恰好被看到）。之前"精确回贴"是 D3D12 侧
拷贝，所以能改变画面，这也侧面印证了这一点。

修法：补丁改在 **D3D12 侧**执行 —— 用 CPU 选好的邻居坐标，在 convert 之前做
`Sink -> 16×16 temp -> Sink` 两次 `CopyTextureRegion`（同帧、同编码、无跨进程）。补丁坐标
在"有 marker 的帧"保持有效（复用帧要再贴一次），无 marker 的帧清除。

#### 第十轮：放弃 scar 传输，marker 拆四角 + 转换后本地邻居填充

按讨论定案：**生产端不再传 scar 原图**（那条路采到的内容与 WGC 看到的帧不一定一致，
`scar_tile.raw` 已证明），配对只靠 marker；修补改成**在转换后的 backbuffer 上用相邻像素填充**。

- `mgpu_shared.h`：`marker_corner(i,w,h)` 定义四个象限位置（1/4、3/4），四个 16×16 块都携带
  同一份 64 bit 码（seq32 + crc32）→ 任何一个被 HUD 挡住都还能配对；
- **fakerFG**：present 时对四个位置各画一次 mark（同一个 `pass_marker`，scissor 到各自块），
  不再调用 `producer_record_scar`，不再做 norm/tile/读回；
- **consumer**：`marker_decode_code()` 依次尝试四个角，任一 CRC 通过即配对；
  `marker_restore_after_convert()` 改为**转换之后**对四个块做
  `backbuffer → 16×16 temp → backbuffer`（源取同行的内侧邻居），同帧同色；
- 删除 D3D11 侧的 sink 补丁（D3D12 视图观察不到它，正是之前色块一直在的原因）。

### fix: scar 补全链路（取图 → 传输 → 回贴）(2026-11-14)

补帧后的 16x16 破块来自整条 sidecar 链路上的若干独立缺陷，逐个修掉：

- **fakerFG / 取图**：Present 时 chain buffer 的真实状态是 `PRESENT`，旧代码把
  `src_state`/`marker_state` 一律写成 `RENDER_TARGET`，并且 marker draw 之前没有
  barrier（`marker_target == dst` 直接跳过）。现在按 `PRESENT → RENDER_TARGET → PRESENT`
  显式过渡，真实 Present 也拿回合法的 `PRESENT` 状态；
- **fakerFG / 取图**：readback 的 `Map` 只声明 `RowPitch * 8` 却读 16 行（同步路径甚至是
  `{0,0}`），越界读让 tile 下半部分是垃圾；现在范围覆盖全部 16 行；
- **fakerFG / 取图**：去掉 Present 线程里的 20 ms 阻塞读回，改成每帧只记录拷贝 + 挂 fence，
  由（此前从未启动的）readback worker 线程 Map/发布；4 个 slot 轮换，全部在飞时**不画
  marker**（宁可少一帧精确配对，也不留下无法补的破块）；
- **transport**：`marker_worker_main` 真正 `CreateThread`；`PresentRingEntry.marker_tile`
  从 512 B 修正为 16x16（1024 B 实际使用），ring 版本 v4（旧映射被判不兼容并回退时间戳
  配对）；producer 的 pending tile 表不再被不匹配的旧 seq 永久占满；
- **consumer / 回贴**：tile 坐标按 capture→backbuffer 缩放映射（`--scale/--den` 下旧代码
  贴错位置）；按 `g_colour_mode` 处理 PQ/709/SDR 编码（`--sdr` 下不再把 PQ 码直接贴进
  gamma 帧）；解码不再只认 R16F（R10/R8/BGRA 也能解）；精确 seq 的 tile 未到时回退到
  同一矩形的最近一帧 tile，保证**永远不留 full-contrast 破块**；
- **consumer / 诊断**：`marker_probe_read` 与旧的 `--marker-repair` 邻居填充改为真实的
  marker 原点（原来探/修的是右上角，既看不到 marker 又会改坏无关区域），邻居填充覆盖完整
  16x16；`marker_probe.on` 存在时 consumer 自动开启解码/回贴，不再依赖 `--marker-probe`。

预期日志：`marker probe: async COPY structures + readback worker ready` →
`marker probe: tile published (seq=... fmt=28 16x16)` →
`marker code: seq=... ok` → `marker restore: seq=... tile ... available` →
`marker tile: seq=... at x,y (16x16)`。

### feat: M3 序列匹配 publication queue（2026-11-14）

`--pub-fifo` 之前虽然打印了 `present-ring: seq=... -> slot ...`，但 `run_frame()` 里的
FIFO 仍然 `pub_q_pop()` 盲取队头并覆盖 `slot`，所以 ring 的精确配对并没有真正用于
MV/depth 选择。

- `PubQueueEntry` 增加 `frame_id`，并且在读取 `published` 时**同时快照 frame_id**；
  之前是在拷贝完成后才读 `frames[s].frame_id`，producer 可能已经覆盖了同一 slot，
  导致 `pub_q_pop_seq(seq)` 找不到并回退到错误队头（2x 的轻微拖影就是这次暴露的）；
- 新增 `pub_q_pop_seq(seq, ...)`：按 present-ring 的 `seq` 找到对应 publication，
  而不是取最旧队头；
- `pair_slot_for_wgc()` 命中 ring 时记录 `g_pair_seq`，`run_frame()` 优先按它 pop；
  seq 不在队列时打印 `pub-queue: seq=... not queued` 并回退队头；
- `pub_q_pop_seq` 未命中时**不再回退到无关队头**：优先用 ring slot 的本地 cache，
  没有匹配 cache 就对下一帧强制 `resetHistory`；M2 colour 队列丢弃/锁帧丢弃也会设置
  这个标记，避免一帧丢失污染 XeFG history 几十帧；
- producer 侧仍保持一帧一次发布（`publish_record` + `publish_frame` 去重）。

这使“颜色帧 seq → publication → 本地 MV/depth copy”成为同一条链。构建并部署：
`xess_mfg_consumer.exe` `640e9982...`。


### feat: present-time ring 配对 + producer 单帧发布（2026-11-14）

不再只靠 `frame_qpc_ms` 猜槽位：fakerFG 在**真正 Present 之后**写入一个小 ring：

```text
{ present_qpc_100ns, eval_qpc_100ns, record.seq, frame_key }
```

consumer 用 WGC `SystemRelativeTime`（同一 QPC 基准）在 ring 中找最近的 present，
得到 `record.seq`，再按 producer 的 `FrameShared.frames[s].frame_id == seq` 精确选
MV/depth 槽；ring 不存在时才回退到原来的时间戳/偏置逻辑。

- 新文件 `src/mgpu_present_ring.h`（64 项、seqlock 式写索引）；
- fakerFG：`present_ring_start/begin/commit`，`hk_present` 在 `o_present` 返回后提交；
- consumer：`open_present_ring()`，配对日志新增 `present-ring: seq=... -> slot ...`；
  只读映射下不能用 `InterlockedCompareExchange64`（会向只读页写入并崩溃），已改成
  volatile 读；
- producer：`publish_frame()` 对同一个 `frame_qpc_ms` 只发布一次，避免同一个
  Evaluate/Present record 被复制 2–3 次；
- 仍保留 `--pair-bias-ms` 作为 ring 缺失时的回退诊断。

三件已重建并部署：

- fakerFG.dll `60fa098c...`
- xess_mfg_producer.dll `fba8d322...`
- xess_mfg_consumer.exe `37828a43...`


### fix: producer 时间戳改用 QPC（2026-11-14）

`faker_fg/provider.cpp` 的 `frame_qpc_ms` 一直写成 `GetTickCount64()`（~15.6 ms 粒度，
且不是 QPC），而 consumer 的 WGC `SystemRelativeTime` 是 QPC 100 ns。两边的“同一时钟”
假设不成立，`pair:` 的 `age` 呈 0.3/13.3/22.3/50.3 ms 的量化跳变，±25 ms 偏置自然
无法稳定配对。

- `harvest()`/Streamline/NVNGX 三个 `frame_qpc_ms` 写入点统一改用
  `now_qpc_ms()`（`QueryPerformanceCounter` 毫秒）；之前只修了 `provider.cpp`，
  `proxy_streamline.cpp`/`proxy_nvngx.cpp` 仍用 `GetTickCount64()`，CP2077 走的正是
  这两条路径之一；
- consumer 的 `g_frame_ms_ema`（喂给 XeFG 的 `frameRenderTime`）和 producer matrix
  新鲜度判断也从 `GetTickCount64` 改为 QPC；
- 重新构建 `build/fakerFG.dll` 并部署到 `dxgi.dll`/`nvngx_dlssg.dll`
  （旧文件备份为 `*.before_qpc_2026-11-14`）；
- 预期：`pair:` 的 `age` 变成连续、窄分布；如果拖影明显改善，说明之前的“颜色/MV
  错位”主要是时钟量化，而不是 slot 选择本身；
- 仍需验证 `present_id/frame_key=0`（producer 没有真正的序列 key）；若 QPC 修正后
  仍有固定偏移，再考虑 present-time ring。



consumer 的 WGC drain 从 presenter 线程拆到独立 capture 线程：capture 线程只做
`TryGetNextFrame`、取纹理/`SystemRelativeTime`/age、放入有界队列；presenter 只从队列
取帧并 `run_frame`，因此 `Present()` 阻塞不再挡住 WGC 的下一次 `TryGetNextFrame`。

- 零拷贝交接：队列保存 `IDirect3D11CaptureFrame + ID3D11Texture2D`，presenter 用完
  （`run_frame` 内的 colour copy 完成后）把 frame 放回 retire 队列；`Close()` 只在
  capture 线程调用，presenter 不跨线程调 WinRT 帧方法；队列默认深度 1
  （`--colour-queue N` 可调到 4），溢出丢最旧帧并计数；
- `g_ctx11` 仍只由 presenter 使用，capture 线程不碰 D3D11 立即上下文；
- 到达统计（`arr`/`age`/`g_wgc_ts_ms`）在 capture 线程取时间戳，presenter 侧统一
  `apply_arrival_values()` 更新，避免双线程写统计；
- 新增 `--no-capture-thread` 回到 M1 单线程循环，用于同会话 A/B；
- `proc:` 增加 `colour`/`cap`/`qdrop` 字段；`breakdown capture=` 现在是 colour 队列
  驻留时间；
- 测试脚本：`mgpu_test_V2_m2.bat`（默认 M2），`mgpu_test_V2_m1.bat` 已加
  `--no-capture-thread`；
- 配对诊断：新增 `--pair-bias-ms`（WGC 时间戳偏移，±250 ms）和周期性 `pair:` 日志
  （`key/present/id/age`）。当前 `pair_slot_for_wgc()` **只按 `frame_qpc_ms` 选槽，
  没有真正使用 `frame_key`**；日志里启动时 `frame_key=0`，需要这次 A/B 确认后续是否
  有非零 key；
- 同会话实测（深度 2）：`proc` 6.45 → 5.08 ms/frame（-1.37，单核 25.6% → 20.5%），
  `total` 24.50 → 24.80，`skip` 0.46 → 0；但 `capture`（队列驻留）中位 **29.9 ms**，
  端到端 colour 延迟比 M1 多约 20 ms，`OUTPUT FRAMES MISSING` 2/41 → 6/43，
  3x 89.3% → 86.6%。因此默认队列深度改为 **1**，待复测；
- 预期收益：`present` 不再阻塞 WGC，`OUTPUT FRAMES MISSING`/`arr` 抖动应改善；
  待硬件实测。



### feat: 独立 `xess_mfg_consumer` 源码树 + M1 事件驱动循环（2026-11-14）

`xess_mfg_consumer.exe` 不再通过 `/DMGPU_CONSUMER_V2` 复用 `src/bridge` 的编译单元，
改为独立源码树 `src/xess_mfg_consumer/`；`bridge` 回到原有的轮询实现，两边只共享 ABI 头
（`src/mgpu_shared.h`、`src/xefg_*.h`）和第三方库。

- 构建：`./scripts/build_consumer.sh` → `build/xess_mfg_consumer.exe`；
  `./scripts/build_bridge.sh` 只构建 `bridge.exe`，不再接受 `consumer` 参数；
- `unity.cpp` 是唯一编译单元，`consumer_internal.h` 放公共前置，16 个实现单元是
  `modules/*.cpp`（fakerFG 式 unity build，没有任何 `.inc`）；`check_bridge_modules.py`
  自动识别 `unity.cpp` / `bridge.cpp`；
- M1 主循环：WGC `FrameArrived` 只 `SetEvent(g_frame_event)`；producer 每个 slot 的共享
  `fwd` fence 用 `SetEventOnCompletion(published + 1, g_pub_event)` 驱动（`g_pub_armed[]`
  去重）；主线程 `MsgWaitForMultipleObjectsEx` 等这两个事件，不再 `Sleep(1)`；
- 失败回退：`add_FrameArrived` 或 `SetEventOnCompletion` 失败时，对应源退回 1 ms 超时，
  日志明确写出哪个事件不可用；
- `proc:` 行新增 `wakes/s`，用于确认空转唤醒从 ~1000/s 降到输入帧率量级；
- 测试：游戏目录 `mgpu_test_V2_m1.bat`（`xess_mfg_consumer.exe` + A5 参数）；
  同一游戏会话 A5 vs M1（35/36 个窗口）实测：`total` 24.60 → 24.55 ms（持平），
  `age` 9.50 → 7.80，`pf` miss 1.3% → 0.36%，`wakes/s` 中位 76（不再是 ~1000），
  `arm` 0.38 ms/s（跨适配器 fencing 很便宜）；`proc` 6.64 → 7.03 ms/frame，3x 比例
  两次运行方向相反、暂不能归因；详见 `docs/findings/M1_EVENT_LOOP.md`；
- 设计：`docs/design/BRIDGE_V2.md`、`docs/design/XESS_MFG_CONSUMER.md`（目录与
  M0–M4 里程碑已按独立树更新）。

### fix: COPY 提交改为 8 组 allocator 轮转（2026-11-14）

A5（`--pub-fifo --den 2`）的 FIFO 已到 `pf` 35–40/40、miss 0–5、overflow 0，且
A2/A4/A5 都没有 `copy FAILED`；但 A4 的 `cq` 中位数 **17.5 ms**、`gpu` 5.7–20.3 抖动。
根因是 `input_copy_begin()` 为了复用单个 command allocator，在 **CPU 侧**等上一批
28 MB 拷贝完成（最多 50 ms）；producer 的 fwd/DMA 变慢时，这个等待直接传导到主线程。

- COPY 队列改为 **8 组 allocator/list 轮转**，每批记录自己的 fence 值；
  只有当 8 批全部在飞时才在 CPU 侧等待；
- `input_copy_submit(false)` 也会 `Close()`，避免 allocator 在 list 未关闭时被 Reset；
- 队列仍然由 COPY 队列按提交顺序执行，main 只对 FIFO 队头那条 batch 的 fence 发 GPU wait；
- 观察点：`cq` 应从 17.5 ms 降到 0–3 ms；`gpu`/`present` 的分配会随 vblank pacing 互换，
  判断是否正常看 **`total` / `load` / `skip` / 空 vblank**，不要只看 `gpu`。

### refactor: bridge.cpp 拆分为 16 个模块（2026-11-14）

`src/bridge/bridge.cpp` 从 10,450 行降到 **114 行**（公共 include + 模块索引），
实现按依赖顺序拆到 `src/bridge/modules/*.inc`：

- 16 个模块，最大的是 `140_frame.inc`（`run_frame_ex`，1,428 行）和
  `160_main.inc`（`main`，1,316 行）；其余按职责分为平台/状态/设备/WGC/FG/
  cadence/着色器/帧 IO/OSD/warp+upsample/stream-auto/convert/配对+dump/光流/输入缓存；
- **保持单翻译单元**：现有 200+ 个文件级 `static` 状态不需要改成 extern 表，
  模块只能引用上面已定义的符号，所以拆分前后语义一致（exe 尺寸 537,088 字节不变）；
- 新增 `scripts/check_bridge_modules.py`：构建前校验索引与磁盘文件一致、无重复/漏引、
  编号有序；`build_bridge.sh` 和 `build.bat` 都会先检查 `modules/`；
- 文档：`docs/design/BRIDGE_ARCH.md`（模块表、规则、下一步）、`src/bridge/README.md`；
- 删除审计出的未用全局 `g_out_hz_max_warned`；
- 下一步见 BRIDGE_ARCH §5：`run_frame_ex` 的 `FrameContext` 拆分、`main` 参数解析独立、
  叶子模块提升为独立 TU、实验路径（`--fg warp` / `--half` / `--mv-shadow`）隔离。

### feat: publication FIFO（--pub-fifo，2026-11-14 晨）

A/A2/A3 三组复测（4K/40/3x）都出现 drop。拆开看是两件事：

1. **A 默认路径本身就超预算**：`gpu 22.05 + present 4.10 + work ≈ 27.5 ms`，输入 `game 25.35 ms`，
   `load 102%`、空 vblank 18.5。上一版 A 的 24.8 ms 是更轻的场景；这次不是代码回归，是场景/吞吐。
   先把 `total` 压回 25 ms 以内（`--den 2` / `--scale 0.75`）再谈配对。
2. **A2 的 100% 缓存仍只有 1–7/40 命中**：槽级 key 修复后依旧，说明大量槽的 MV/depth 拷贝**本身失败**，
   不是 key 比较错。已加一次性诊断 `pf: slot N copy FAILED (...)`（mv/depth 指针、尺寸、格式、本地资源）。

按“WGC 比拷贝慢”这个事实（`age 10.6 ms > copy 3.6 ms`）新增 **`--pub-fifo`**：

- 预拷成功的 publication 按 **publication 值升序**入队（不是槽顺序；生产者是轮转发布）；
- 主线程每个 WGC 帧 **pop 队头**，不再做时间戳选槽、不再比较 `local_pub == published`；
- 队列深度上限 `kMaxSlots-1`（防止生产端复用仍被队列引用的槽）；溢出丢最旧并计数；
- 报告行：`pf: N served from the publication cache, M missed (empty queue), D dropped by overflow; queue now Q`；
- `--pub-fifo` 隐含 `--prefetch-publications`，默认仍关。

注意：FIFO 不解决“消费侧跑不到 40 fps”的 drop；它只保证“拿到的 MV/depth 是已拷贝的那一版”。

### fix: 100% 预拷与 miss 光流改为 opt-in（A 组实机回归，2026-11-14 晨）

A 组（4K/40 锁/3x）复测 `bridge 929e99f5`（copy-on-publication + miss→光流）出现严重 drop：
`gpu 21.2→23.55 ms`、`load 96%→106.5%`、`skip 1→6`、空 vblank 4→17、`pf` 命中只有 3–9/40。

| 根因 | 证据 | 处置 |
|---|---|---|
| 4K 下光流每帧太贵 | `--fg-of: 3840x2160`；`gpu` +2.3 ms，`of` 中位 0.3 ms 只是 CPU 记录时间，GPU 约 2–3 ms | miss→光流改为 `--of-miss-fallback`（默认关），miss 默认回到帧内补拷 |
| 100% 缓存命中判定整批失效 | `prefetch_publications()` 用**整批** `g_copy_count` 判成功；任一槽 `ensure_local_copy`/宽 depth 失败，整批 `local_pub` 都不写 | 改为**槽级**：`copy_slot_streams()` 只有 MV+depth 都录进列表才返回 true，逐槽写 `local_pub/fence` |
| 默认回退 | 上一版 A 实测 `gpu 21.2 / load 96% / skip 1 / 空 4` | `--prefetch-publications` 默认关（单槽轮转 + miss 补拷）；`--prefetch-publications` / `--of-miss-fallback` 保留为 A/B 开关 |

### fix: 首次实机暴露的三个问题（2026-11-14 夜）

| 问题 | 实测现象 | 修复 |
|---|---|---|
| 自动档位振荡 | C 组在 100% ↔ 400%（960×536）之间跳；`age_p10` 只有 0.5–4.6 ms（中位数 12 ms） | 年龄改用**中位数**；选**第一个满足**的档（最低 percent）；全不满足回退 400；生产端改用 `frame_time_ms` 增量头寸 |
| 非对称流未生效 | D 组 `bus 63.3 MB`（两条都 100%）；生产端把“请求 0”当成“保持上一次”，沿用了 C 的 100 | 生产端保存 `g_ini_percent[]`，请求 0 → 回退 ini（depth=150） |
| 升采样 pass 未运行 | B/D 日志只有 `--upsample: ON`，没有 `reconstruction targets`；诊断 `deploc=0` | depth 实际在 `g_depth_ring[]`（tag 也用它），pass 改用同一个资源；加 `ring=` 诊断 |
| D 被关成 1X | `xN=1.00`、`presents=40`、`ERR_INVALID_ARGUMENT` | 同上；tag 尺寸不一致的根因就是 pass 没跑 |
| 渲染卡被拷贝拖慢 | 150 档 `game=25.1 ms`、100 档 `27.2 ms`；拷贝串在游戏帧里（90%→99% 占用） | 生产端**两条**异步队列：源在 COPY_SOURCE 时用 **DMA/COPY 队列**（不占 3D 引擎），否则用第二条 DIRECT 队列（可发 barrier）；present Signal → Wait → Copy → Signal(fwd) |
| 自动档位误判 | `frame_time_ms=7.5 ms`（record 间隔；CP2077 每帧 Evaluate ~3.3 次） | 改为 **publish 间隔**（每帧一次，≈25 ms） |
| 自动档位再次误判 | present detour 每帧调两次 publish → `frame=13 ms` | 只在 `frame_qpc_ms` 变化时计间隔（同一源帧只计一次，≈25 ms） |
| 输入拷贝在 WGC 之后 | y+z 没有被 x 覆盖 | `--prefetch-inputs` + `--input-copy-queue` **默认开**：Present 后在 copy 队列预拷下一槽（GPU 侧等 fwd），WGC 到达只查表；`--no-prefetch-inputs` 回旧顺序 |
| 自动档位压到 400% | 拷贝移到 DMA 后 frame_time 不再含拷贝，旧模型算出 headroom 0.2–0.9 ms | 控制器改为按副卡 `gpu+present` 反馈：>T_in−1 降档、<T_in−1.5 才升档；首次采用 producer 的 active 档 |
| 预拷命中率 ~90% | 单槽轮转猜槽，WGC 抖动时选到相邻槽 | **copy-on-publication**（默认开）：capture loop 轮询 `frames[s].published`，每个新 publication 都拷进对应槽的本地缓存；命中判定 `local_pub[s] == published`；`--no-prefetch-publications` 回单槽预拷 |
| 预拷 miss 补拷 | 3–8/40 miss，miss 时 `cq 4–9 ms` + 3.6 ms 拷贝 | **miss 不再补拷**：FFX 光流每帧跟踪（维持内部历史），miss 帧 tag flow MV + 最后一帧 depth；`--no-of-fallback` 回补拷 |
| 升采样 depth 源 | `deploc=0`（depth 在 ring 里） | pass 改用 `g_depth_ring[g_ring_write]` |

生产端同时开始发布 `FrameShared.frame_time_ms`（两次 record 的间隔 EMA），
这是自动档位计算生产端余量所需的量。

### feat: MV/深度降采样取真实样本 + `--upsample` A/B 开关 (2026-11-13)
### feat: MV/深度降采样取真实样本 + `--upsample` A/B 开关 (2026-11-13)

**用户能感知**：`StreamPercent` 降采样后，运动边缘不再出现“幽灵运动”（平均出来的矢量），
深度轮廓不再被平均糊掉。副卡可选地把 MV/深度重建回渲染尺寸（默认关，A/B）。

- **生产端（无条件）**：`downsample_mv.hlsl` 从双线性平均改为 **medoid**（3×3 足迹里离均值最近的
  真实样本）；`downsample_depth.hlsl` 改为 **3×3 中位数**。带宽不变，只多 20–30 条 ALU。
  理由：平均会在运动/深度边界写出一个从未存在过的值，消费端无论怎么升采样都救不回来
  （`docs/design/UPSAMPLE_QUALITY.md` §1）。
- **消费端（开关）**：`bridge.exe --upsample`（`--upsample-k` 调深度权重）与 UI
  「升采样 MV/深度 (A/B)」。一个联合 compute pass：depth 引导 MV、depth 输出真实样本，
  两个输出同尺寸（XeFG 强制 MV/depth 同尺寸）。目标尺寸 = 游戏渲染尺寸。
  开启时暂关 `--input-copy-queue` / `--prefetch-inputs`（pass 录在本帧的 DIRECT 表上）。
- **定案**：方案 C（MV 全分辨率 / depth 半分辨率）因同尺寸约束推迟；自动档位推迟。
  A/B 计划与“开关保留/移除”的判据在 `docs/design/PLAN_2026-11-13.md`。
- **协议 v15 / 自动档位 / 非对称流（同日补齐）**：
  - `SharedControl` 追加 `requested_percent_mv/depth` + `active_percent_mv/depth`；
    生产端每帧应用请求并回显，内容矩形变化时 bump `generation`（槽位容量按**渲染尺寸**预留）。
  - `bridge --stream-auto`：每个统计窗口按 `age` p10 + 拷贝模型选档（阶梯
    100/125/150/175/200/250/300/400，2/4 窗确认，2 s 驻留），UI 有开关。
  - `bridge --stream-asym`：MV 1:1、depth 降档，隐含 `--upsample`；升采样 shader 已支持
    MV/depth **不同源尺寸**。UI 有开关，打开时会自动勾上升采样。

构建验证（本机）：`build/xess_mfg_producer.dll`、`build/bridge.exe`、`build/mgpu_ui.exe`
全部重建成功；三个 shader 都用 Windows SDK `fxc -T cs_5_0` 编译通过。
**真机行为未测**：pass 的 barrier、画质与 `gpu`/`skip` 变化需要按定案的 A/B 跑。

### feat: fakerFG 完整实现（P1–P5）—— 自建 FG 提供者 + XeFG 输出 + 注入 ReShade (2026-11-12)

按新架构把生产端拆开：`fakerFG.dll` 拥有整条 FG 管线，`game_capture` 以后只负责传数据。

```text
fakerFG.dll  -> dxgi.dll          20 个 DXGI 导出（5 个有逻辑）+ 工厂 vtable 第 16 项
             -> nvngx_dlssg.dll   50 个 DLSS-G 导出（4 个有逻辑）+ 双实例桥
             -> nvapi_QueryInterface 钩子：只给 Streamline 回 sm_80+（30 系才看得到 FG 菜单）
             -> XeFG：InitFromSwapChainDesc + GetSwapChainPtr，present 归我们
```

要点：

- **交换链接管**：`CreateSwapChainForHwnd` 的 vtable 项改成指针写（ReShade 式），XeFG 建链、
  游戏拿到 XeFG 的链；失败/非 flip 模型/无 D3D12 队列 → 原调用照常执行，游戏用自己的链。
- **provider**：`CreateFeature` 给游戏我们自己的 handle（magic 校验），`EvaluateFeature`
  先 harvest（参数块 → `MgpuFgFrame`）再 tag MV/depth + `SetPresentId`，**不写**
  `OutputInterpolated`（这正是旧方案崩设备/黑屏的那一步）。
- **绝不黑屏的两道保险**：`provider_owns_output()` 要求 XeFG 上下文**且**交换链确实被接管；
  任一不满足就交回 `nvngx_dlssg_real.dll`（日志里写原因）。
- **一个 DLL 两份实例**：`dxgi.dll` 和 `nvngx_dlssg.dll` 是两次加载 ⇒ 两套全局状态，
  所以 DLSS-G 实例通过导出 `MgpuFgNgxBridge` 把契约转给 dxgi 实例（否则 provider 拿到
  的是一份没有 XeFG 的空壳，会静默退回真运行时）。
- **架构问答**：`ArchSpoof=1` 时只对 Streamline 侧把 `NvAPI_GPU_GetArchInfo` 报成 AD10x
  （全量伪装会破坏同一适配器上的 DLSS-SR/D，这是 OptiScaler 的既有结论）。
- **颜色默认用 XeFG 自己的后台缓冲**（已含 UI）；`TagHudless`/`TagUi` 是画质选项，默认关。

产物与验证（本机可复现）：

```text
./scripts/build_faker_fg.sh                 -> build/fakerFG.dll  88,064 字节
objdump -p 导出表                           -> 80 个：50 NGX + 20 DXGI + MgpuFgGetApi
                                               + MgpuFgNgxBridge + 3 个 DLSSG_* 别名
构建警告                                    -> 0
```

新增文件：`src/faker_fg/{unity,faker_fg,nvapi_arch,proxy_dxgi,proxy_ngx,provider,backend_xefg}.cpp`、
`src/faker_fg/fakerFG.def`、`scripts/deploy_faker_fg.sh`、`packaging/reshade_pack/fakerFG.ini`；
`docs/design/FAKER_FG.md` 增 §11（CP2077 部署 / 期望日志 / 判读表 / 三档验证顺序 / 回退）。

**真机验证未做**（本环境无法启动游戏）：P1–P5 的验收条件是编过 + 导出表 + 逻辑自检，
画面与插帧率需要按 §11 在 Windows 上跑一次。capture 侧的 `in_faker.cpp`（把记录发给副卡）
是 P3 的剩余部分。

### design: 生产端重构定案 —— fakerFG 拥有 FG，capture 退回纯传输 (2026-11-12)

需求（`问题和描述.md`）把生产者端拆成两半，本轮只落设计 + P0 骨架：

```text
fakerFG.dll         FG 契约（Available/Requirements/Create/Evaluate）+ DLSS-G 输入
                    + XeFG 输出（idle/local/bridge）+ present 所有权
game_capture        只从 fakerFG 取 MgpuFgFrame → 降采样 → 共享槽 + fence → 发布
bridge              不变
```

为什么角色换位（三条都在树里有出处）：Streamline 在 `slInit` 问架构（add-on 还没加载）；
`sl.dlss_g` 在加载时缓存 `GetProcAddress` 的结果（add-on 的 MinHook/内联代理都太晚，CP2077 实测）；
观察者写 `OutputInterpolated` 只能拿到 ReShade 的 `general` 伪状态，掉设备或黑屏。
只有"我们是 provider"时，"不生成但画面正确"才合法。

新增：

- `docs/design/FAKER_FG.md`：职责边界、注入链（`dxgi.dll` + `nvngx_dlssg.dll` 两份副本 + ReShade64）、
  fakerFG 内部文件结构、进程内接口、capture 的删除/保留清单、P0–P7 阶段与验收、风险表。
  `FG_BACKEND.md` 加"§9–§10 作废"的指引，`docs/README.md` 索引补一行。
- `src/faker_fg/mgpu_fg_api.h`：两端唯一接口（纯 C、`size`/`version` 前向兼容、按**导出名**找人）。
- `src/faker_fg/faker_fg.cpp` + `scripts/build_faker_fg.sh`：P0 骨架，导出 `MgpuFgGetApi`，
  `frame_available` 恒为 0（capture 因此照旧走自己的车道，行为不变）。验证：
  `./scripts/build_faker_fg.sh` 通过，`build/fakerFG.dll` 13,312 字节、导出表只有 `MgpuFgGetApi`。

尚未变化：`game_capture` 一行未改（P2/P3 才接记录），`src/fg_host/` 保留到 P7。

### experiment: 打断 FG 时填充运行时输出 —— 成功一次，但状态不可靠，默认关闭 (2026-11-12)

目标：`FgMode=consume` 打断游戏生成的同时，屏幕仍显示原生帧。

做法：短路 DLSS-G evaluate 之前，把 `DLSSG.Backbuffer` 拷进 `DLSSG.OutputInterpolated`
（CP2077 呈现的就是这张纹理），三条门槛：表类型可记 barrier、格式可拷、两个资源的状态是观察到的。

结果（`game_capture.consume_nofill.log` + 本次崩溃）：

```text
✓ 逻辑打通了一次：
  interrupt: wrote the native frame into DLSSG.OutputInterpolated (3840x2160, state 0x80000000->copy->0x800)
✗ 但状态是错的：
  0x80000000 = ReShade 的 general 伪状态，没有 D3D12 等价物 → StateBefore 非法 → 设备移除
  0x800      = COPY_SOURCE，是我们上一帧自己恢复出来的值，游戏并不在这个状态
→ 游戏启动即崩溃（REDEngine report queue）
```

顺带修掉的两个真 bug（都不是这个实验特有的）：

- **COMPUTE 表被误判为不能记 barrier**：只有 COPY/BUNDLE 不行。CP2077 的 DLSS-G evaluate
  给的就是 COMPUTE 表，第一版因此"填不了"。
- **共享块预留用固定 2560x1440 计算**：DLAA 4K 时布局需要 270 MB 而只预留 120 MB →
  `layout needs … - disabling`。现在预留与布局用同一组 `(w, h)` 与格式。

结论与后续：

- `MgpuCaptureFillInterpolated` 与 `MgpuCaptureTrackResource` 保留，但由
  **`[capture] FgFill=0`（默认关）** 控制，避免再次掉设备；
- 要真正做成，必须先拿到两个交换链纹理的**可信状态**。ReShade 对 CP2077 的这两张纹理只报
  `general`，说明它们在游戏侧走的是 enhanced barrier，我们的传统状态表和 ReShade 的映射都
  覆盖不到——下一步应改为"用 enhanced barrier 组 + layout/sync/access 描述"来转换，
  或者改用 ReShade 报告的资源状态（需要扩展 shim，当前 API 没有状态查询）。
- 现实可行的那条路：`FgMode=consume` 只在 **bridge 在场**时使用——那时屏幕由 bridge 呈现，
  游戏自己的黑帧被覆盖，不需要填充。

2077 已回到 `FgMode=off`（安全配置）。

### feature: 生产端逐流能力判定 + 消费端按实际情况降级 (2026-11-12)

按"能不能传就传，不能传就明说；消费端自己降级"的原则，把能力判定做成机械规则，不看游戏是谁：

```text
生产端（每场一次，写进布局）
  shared_safe_format(fmt)   深度模板（含 typeless 形式）→ 不能上跨适配器堆 → 该流不布局
  布局里 slot_size == 0      = 这条流本场不存在（消费端据此得知，不靠猜）
  运行时逐帧               copy_formats_ok + tag 声明的 state + 表类型 → 能拷就拷，否则跳过
  日志                     capabilities … mv=transferred / depth=NOT transferred (原因)

消费端（bridge）
  槽位不存在 → ensure_dummy_depth + 一行说明，插值靠游戏 MV（深度只影响去遮挡钳制）
  颜色/UI 本来就走 WGC，不依赖生产端
```

这替代了之前的"按游戏打补丁"：CP2077 的深度是 `R32_TYPELESS`（能放）→ 传；
明末的深度是 `D32_FLOAT_S8X24`（堆直接拒绝）→ 不传，日志写明原因，bridge 用 dummy。

新增日志（生产端，一次性）：

```text
capabilities (per frame, from the list type + declared formats):
  mv    transferred (fmt 34, 2558x1439)
  depth NOT transferred (fmt 19, 2558x1439) - depth-stencil cannot cross a cross-adapter heap
```

新增日志（消费端，一次）：`[bridge] the producer does not transfer depth this session …`。

### fix: UE5 不能在任何游戏命令表上跑深度转换 —— 明末改为 MV-only (2026-11-12)

同一个崩溃出现两次，两次都紧跟在深度转换之后：

```text
LowLevelFatalError  Interfaces.CopyCommandList->Close() failed  E_INVALIDARG
[08:32:29.915] tag list type=0 (DIRECT) … graphics=1
[08:32:29.923] [depth] same-frame convert in place: 2560x1440 fmt 19
```

tag 表自身是 DIRECT、转换在它上面完全合法，但引擎随后关闭它自己的 copy 表时失败。
而**不跑转换、直接把深度跳过**的那一轮（08:23）连续跑了 7 分钟无异常。
结论：UE5 这条管线不能容忍第三方在它的帧里插入深度转换，不管记在哪张表上。

因此明末的深度处理固定为"跳过 + 明确说明"，不再有任何 pass 尝试：

- `copy_formats_ok` 失败时只记一次日志，说明"深度模板资源需要转换、而转换会让 UE5 崩溃，
  该标题的深度跳过，由消费端光流兜底"；
- 懒加载的 `ds_prepare` 也从 tag 通道移除（那一块显存本来也是为了 pass 才存在）；
- 跨适配器堆本身也拒绝深度模板格式（`tests/shared_probe` 实测 `D32_FLOAT_S8X24`
  → `E_INVALIDARG`），所以就算转换成功，源格式也进不了总线。

新增 `tests/shared_probe`：桌面 2 秒验证这台机器的真实能力，不用进游戏：

```text
189 MB heap from a file mapping: ok (flags 0x421)
placed RG16F / RGBA16F / R32_TYPELESS: ok
placed D32_FLOAT_S8X24: E_INVALIDARG
253 MB 及以上: DXGI_ERROR_DEVICE_REMOVED   ← 之前黑屏的来源
committed ALLOW_CROSS_ADAPTER 路线: E_INVALIDARG（CrossAdapterRowMajorTextureSupported=0）
```

### fix: 共享块按真实格式预留；1:1 后不再预建 pass 目标 (2026-11-12)

明末 1:1 版本的第一次运行：**黑屏卡死、有声音、mv/depth 全 0**。日志只有一行关键：

```text
shared: layout needs 380.1 MB but only 253.6 MB is reserved - disabling
```

原因：`reserve_bytes()` 用固定 `R32_TYPELESS`（4 B/px）估算每个槽位，而槽位格式现在跟着源走
（明末 MV 是 RGBA16F = 8 B/px）。实际需要 380 MB，只预留了 253 MB → 整个共享块被禁用
→ 一帧都没拷。

三处修正：

1. `reserve_bytes()` 改成用**同一份 `stream_specs()`** 逐流计算（颜色/UI 关闭时跳过），
   预留与布局从此不可能不一致——这正是注释里警告过的那类 bug；
2. 删除"提前创建 downsample / depth-convert 目标"那段：1:1 传输不再需要任何 pass，
   而它会占两块 3840x2160 表面（约 127 MB），在 4K UE5 上是实打实的显存压力；
3. 顺带修掉一条会说"-> stream R32 via convert pass"的陈旧日志。

证据：`game_capture.shared_disabled.log`。

### change: 删除 RenderWidth / RenderHeight —— 渲染尺寸只认游戏的声明 (2026-11-12)

`RenderWidth` / `RenderHeight` 是当年为了"某个库报错尺寸"留的手填旋钮。现在它是纯风险：
填错和管线坏掉在日志里长得一样。删掉后渲染尺寸只有两个来源，且都在声明侧：

```text
① tag / 参数块里声明的 render 尺寸（dlss Quality@4K = 1440p，DLAA = 2160p）
② 声明没给 extent 时，从资源描述本身取（Pragmata 那种 extent=(0,0) 的情况）
```

连带清理：`g_render_w_ini` / `g_render_h_ini` 两个变量、`ensure_shared()` 与
`stream_target_size()` 里的覆盖分支、以及那条指向已不存在键的警告文案。

配置面：19 → 17 个键（生效 13 + 注释 4）。生效键现在是：
`Log FgMode Input SlInput PreferFg StreamlineCamera FgSniff FgNgxHooks SkipMv SkipDepth
FollowConsumer LoaderHook NoCopies`。

### change: 删除 StreamPercent —— 传输永远 1:1，性能交给游戏自己的分辨率/档位 (2026-11-12)

理由：缩放需要一次图形 pass，而 tag 交给我们的命令表在 UE5 上可能是 COPY 类型，
那条路上既不能记 barrier 也不能记 draw。硬做只有两种结果——崩（关表失败）或偷偷裁剪。
所以这个旋钮从根上去掉：

- `StreamPercent` 键与 `g_stream_percent` 变量删除；
- 槽位一律**跟随声明的源格式与源尺寸**（MV 可能是 RGBA16F，depth 可能是 D32_FLOAT_S8X24），
  传输就是一次 `CopyTextureRegion`；
- tag 通道里整块 "1/N transport" 分支删除（`want_ds`），保留"槽位比源小就跳过并说明"的守卫
  （那是切档位后布局还没跟上的那一两帧）；
- 通用通道也改成"尺寸与格式都相符就纯拷贝"，转换 pass 只作为格式不符时的兜底；
- 成本从此只由游戏自己的渲染分辨率/DLSS 档位决定：4K DLAA ≈ 63 MB/帧（约 5 fps），
  1440p 渲染 ≈ 28 MB/帧（约 2 fps）。

配置面：ini 键从 20 个降到 19 个（生效 15 + 注释 4），双向校验零缺口。

### feature: 方案 1 落地 —— 不窄化时槽位格式跟随源，传输退化为纯拷贝 (2026-11-12)

UE5（明末）的 tag 表是 COPY 类型，既不能记 barrier 也不能记 draw/dispatch，因此
"缩放到 1/2" 和 "深度格式转换" 在那条表上物理上做不到。方案 1 把这两件事交给消费端：

- **生产端**：`StreamPercent>100` 时槽位仍是规范格式（RG16F / R32_TYPELESS），由转换 pass 写；
  `StreamPercent=100` 时槽位**跟随声明的源格式**（RGBA16F / D32_FLOAT_S8X24），
  传输退化成一次 `CopyTextureRegion` —— COPY 表唯一能接受的操作；
- **消费端**：`StreamShared.format` 一直带着格式，bridge 的 prepare pass 按它选采样视图
  （`D32_FLOAT_S8X24` / `R32G8X24_TYPELESS` → `R32_FLOAT_X8X24_TYPELESS`），并把分辨率缩放到输出尺寸；
- 新增一行日志，让每次运行都能看出走的是哪条：
  `shared: slots mv=…x… fmt=… depth=…x… fmt=… (narrowed=yes(conversion pass)|no(plain copy))`

代价与收益：明末走纯拷贝时 MV+depth ≈ 14 MB/帧（2558x1439），按 13 GB/s 模型约 1 fps；
换来的是 COPY 表游戏完全可用、且不需要我们创建任何队列或 staging。

已知风险：跨适配器堆是否接受 `D32_FLOAT_S8X24` 未验证（早期注释说 R32_TYPELESS 是这里唯一
能分配的深度格式）。若分配失败，日志会出现 `placed resource FAILED (stream 2 …)`，
下一步就是对深度单独走"自有 DIRECT 表"（方案 2 的最小切片）。

### fix: UE5 的 tag 命令表是 COPY 类型 —— 记 barrier/图形 pass 会让游戏自己关表失败 (2026-11-12)

明末（UE5.1）崩溃：

```text
LowLevelFatalError
D3D12Util.cpp [Line: 903] Interfaces.CopyCommandList->Close() failed
D3D12CommandList.cpp:284  with error E_INVALIDARG
```

原因：这个游戏的 `slSetTag` 交出来的命令表是 **D3D12_COMMAND_LIST_TYPE_COPY**。
COPY 表上不能记 `ResourceBarrier`，也不能记 draw/dispatch —— 我们新加的深度转换 pass
正好插在 tag 表上，于是 UE5 自己 `Close()` 时炸在自己的 RHI 里。

三处修正（`game_capture.cpp`，tag 通道）：

1. 读 `list->GetType()`，得到 `can_barrier`（非 COPY）与 `can_graphics`（DIRECT）；
   首次打印 `tag list type=N (...): barriers=? graphics=?`，以后日志里一眼可知；
2. 深度转换 pass 与降采样 pass 都加 `can_graphics` 条件；barrier 加 `can_barrier` 条件；
3. 槽位比源小又跑不了缩放时**拒绝并提示**，不再按 `min(src,slot)` 拷一个左上角
   —— 上一轮那 3.50 MB/帧的 MV 很可能就是这个裁剪。

配置结论：UE5 这类"tag 表是 COPY"的游戏用 `StreamPercent=100`（全尺寸传输），
因为缩放必须在 DIRECT 表上做。明末的 ini 已改成 100。

证据：`game_capture.crash_copylist.log`（崩溃前最后一行就是我们记进 copy 表的转换 pass）、
`game_capture.observe.log`（FG 开时 DEPTH/MV/HUDLESS 每帧全部到位）。

### fix/design: FgMode 默认改为 off；consume 在 DLSS-G 接管 present 的游戏上必然黑屏 (2026-11-12)

2077 + dlssg_sm86 实测三步：

| 步骤 | 结果 |
|---|---|
| FG 开时的声明 | 完整（415 组 tag、`MOTION_VECTORS`/`SCALING_INPUT`、DLSS-G 参数块**带真实 MV 指针**：46 次 evaluate 里 20 次非空） |
| `FgMode=consume` 打断 | 生效（`short-circuit = 1`），拷贝照旧 300/300、15.8 MB/帧 |
| 画面 | **全黑** |

原因：游戏的 present 就是 DLSS-G 运行时。它把 `outInterp`
（`NVSDK_NGX_DLSSG_Parameter_OutputInterpolated`，3840x2160）交给运行时写，再呈现返回值；
我们跳过 evaluate 就等于呈现一张没人写过的纹理。

因此：

- `FgMode` 默认值 `consume → off`。`off` 下 2077 的 tag 通道依然完整（两次实测 MV/depth
  字节数完全相同），管线用这个；
- `consume` 保留但注明适用边界：只有"FG 运行时不管 present"的游戏（FFX-FG / XeSS-FG 类）
  可能接受，逐个试，黑屏就退回；
- 顺带修掉一处漏改：`in_ngx.cpp` 的 `FgSuppressNgx` 之前直接读 ini，没跟随 FgMode ——
  2077 的 DLSS-G evaluate 走的是这条路（`slEvaluateFeature(DLSS_G)` 从未被调用），
  所以第一次 consume 测试实际没打断；现在两条路都由 FgMode 推导。

证据保留：`game_capture.observe.log`（dlssg_mv 非空）、`game_capture.consume_black.log`
（打断生效 + 黑屏）。

### cleanup: ini 从 44 键砍到 20 键，删掉旧场景的残留代码 (2026-11-12)

一份键表只有"每个键都真的被读、且真的能改行为"才有用。清理后：代码读 20 个键，
ini 生效 16 + 注释形式 13（那四个显式覆盖写一次即可），**双向零缺口**。

删掉的键与理由（ini 末尾也留了这份清单）：

| 类别 | 键 | 为什么删 |
|---|---|---|
| 颜色 / UI | `Color` `ColorFormat` `Ui` `UiFormat` | 决策上归消费端 WGC；代码里恒为 0，留键等于骗人 |
| 语义改写 | `MvNormalize` `MvFlip` `MvFromUav` | 违反规则 8；MV 单位随协议发出，不需要我们改数据 |
| 深度 | `DepthConvert` `DepthDirect` | 两个开关决定一件事：能直拷就直拷，不能才转换 |
| 契约 | `FGVelocityValidNow` `FGDepthValidNow` `FGHudlessValidNow` | onlyValidNow 就是契约本身，不是可选项 |
| 尺寸/开发 | `StreamAlign` `TestSize` `SlotPolicy` `SlotWaitUs` | 裁剪会让 MV 与画面错位；等槽位等于等延迟 |
| 测量 | `NoTransitions` `SubmitPathCopy` `PublishOff` `OffloadQueue` | 不合法 / 只有一种正确行为 / 收益为 0 / 违反规则 1 |
| 解锁器 | `FgProvide` | 交给 dlssg_sm86；同进程两套解锁必然打架 |

对应的代码一并删除（不是留着不读）：

- `in_fg.cpp`：NvAPI 架构伪装、`prov_alloc`/`prov_index` 空壳 feature、capability /
  requirements / slGetFeatureRequirements 的改写分支 —— 这些钩子现在**只读**：
  把 API 的回答打进日志，一个字节都不改；
- `in_ngx.cpp`：同一套 stub 机制（`fg_provide` / `prov_*` / 三条改写），同样改为只读；
- `game_capture.cpp`：颜色/UI 流配置、`StreamAlign` 裁剪、`SlotPolicy` 等待、
  `OffloadQueue`（在游戏 device 上建我们自己的队列）、`TestSize`、`SubmitPathCopy`、
  `PublishOff`、MV 归一化/翻转/从 UAV 读取的键、深度两个开关的键；
- `uav_read_allowed()` 恒为 true：状态由 tag 声明，格式从资源读，不再是特例。

体积变化：`game_capture.addon64` 189,440 → 183,296 字节。

保留的只有：`Log` `FgMode` `Input` `SlInput` `PreferFg` `StreamlineCamera` `FgSniff`
`FgNgxHooks` `StreamPercent` `RenderWidth/Height` `SkipMv/SkipDepth` `FollowConsumer`
`LoaderHook` `NoCopies` ＋ 四个显式覆盖（`Claim` `FgCut` `FgSuppressNgx` `SlFgOff`）。

### feature: FgMode —— 游戏开 FG、我们抓输入、打断生成、副卡出帧 (2026-11-12)

真实使用场景固化为一个键：

```text
游戏开 FG → 抓完整 FG 声明 → 打断游戏的生成 → 输入过桥 → 副卡执行 FG
```

| FgMode | 游戏 FG | 谁生成 | 说明 |
|---|---|---|---|
| `consume`（默认） | 保持开启 | 副卡 bridge | 声明最完整（帧配对/reset/dilated/HUDless），生成被逐通道打断 |
| `observe` | 保持开启 | 游戏自己 | 纯数据调查，4K DLAA 实测 48 fps（生成开销在渲染卡上） |
| `off` | 关闭 | 副卡 bridge | 只走超分通道 |

打断按通道实现，**只停生成、不停声明**：

- Streamline/DLSS-G：`slEvaluateFeature(DLSS_G)` 返回 eOk 不调用真实现（拷贝已在此之前记录）
- NGX DLSS-G：`nvngx_dlssg` 的 Evaluate 短路
- FSR-FG：`FrameGenerationPrepare` dispatch 丢弃
- 使能：`Claim` 把游戏的 `mode=eOff` 改写成 `eOn`

`Claim` / `FgCut` / `FgSuppressNgx` / `SlFgOff` 仍然生效，**ini 里写出来的一律优先**于 FgMode
的推导值 —— 某个标题在 consume 下黑屏时，退路是改模式而不是重新编译。

配套：`game_capture.ini` 增加 `FgMode` 与 `PreferFg`（FG 通道优先询问），三个低层键改为
注释形式；`run_cp2077.bat` / `TESTBED_CP2077.md` 的操作说明改为"游戏里的 FG 要开着"。

### refactor: 生产端回到 ReShade add-on，自有宿主退役 (2026-11-12)

决定：不再维护自己的 `version.dll` 宿主链路（不稳定、且和 dlssg_sm86 抢同一个文件名），
生产端重新只以 ReShade add-on 的形态存在。详见 `docs/design/PRODUCER_V14.md`。

代码层面（`src/game_capture/`）：

- 8 处 `#ifdef MGPU_OWN_HOST` 全部移除，两种构建的差异不再存在；
- **tag 通道升级为生产端核心**：`MgpuCaptureRecordFgInputs()` 不再藏在宿主分支里，
  in_fg.cpp 的 slSetTag 钩子直接调用它，在 tag 给出的命令表上原地记录拷贝；
- 删除宿主专用入口 9 个（`MgpuHostDrainModules` / `MgpuHostRecordPresentCopy` /
  `MgpuCaptureRecordPending` / `MgpuCaptureHasPending` / `MgpuHostEnsureShared` …）；
- 删除 `CopyAtFrameEnd` 停放路径、`NoTick` / `NoShared` / `TransferEveryN` 三个宿主握手键
  （`NoTick` / `NoShared` 从未写进模板，属于测量开关）；
- `tracked_state()` 恢复"声明即可用"：作为 add-on 时 ReShade 会通过 `addon_event::barrier`
  报告游戏的每一次状态转换，所以"声明过但没观察到"只意味着游戏从不转换该资源
  （2077 的 MV 全程留在 UAV）；
- 目录调整：`src/host/` → `src/legacy/own_host/`，`src/version_proxy/` → `src/legacy/version_proxy/`，
  对应构建/部署脚本 → `scripts/legacy/`。

行为决策（与实验结论对齐）：

| 项 | 结论 |
|---|---|
| 数据通道 | SL 流（2077 类）可直接拿到完整 FG 输入；NGX 流（Wuchang 类）需要显卡支持，由 dlssg_sm86 解锁 |
| 传输内容 | 只传 MV + depth（+相机等元数据），颜色走消费端 WGC |
| 尺寸 | `StreamPercent` 档位：100 全/200 半/400 四分之一，默认 200 |
| FG 打断 | 三档保留：游戏菜单 → `FgCut` → `SlFgOff`/`FgSuppressNgx`；`FgProvide` 默认 0（解锁交给 sm86） |
| 注入 | ReShade = `dxgi.dll`，sm86 = `version.dll`，一文件一代理，互不替代 |

配套：

- `packaging/reshade_pack/game_capture.ini` 按 add-on 实际读取的 39 个键重写，
  每条都有中文说明与默认值理由（旧模板里的 `StreamHeight` 已不存在）；
- 新增 `scripts/deploy_producer.sh`：把 add-on 装进游戏目录，检查 ReShade 是否在位、
  sm86 是否完整、`Engine=XeSS` 是否会抢插帧，并提供 `--dry-run`；
- 验证：`./scripts/build_addon.sh src/game_capture` 通过（`game_capture.addon64`）。

### perf: 1/2-size streams recover 4 of the 5 fps (2026-11-12, evening)

The cost is linear in bytes over the bus after all - the earlier "bytes are free" reading came from
runs where the narrowed size never reached the copy. Three bugs, in order of discovery:

1. `kRequestRawMv` skipped the StreamPercent block entirely (raw only means "do not normalise",
   not "send 4K"), so the layout metadata stayed 4K.
2. With the metadata narrowed, `g_slot[]` still held the old 4K resources, so the tag-path copy
   was `min(src, dst)` = 4K. Fixed by clamping the copy box to the declared stream size
   (`g_stream_w/h`) - the consumer reads exactly that region, so it stays correct.
3. `CopyTextureRegion` cannot scale: a narrowed slot received a CROP. The tag path now runs the
   existing downsample pass (`ds_run`) into a stream-sized target and copies that.

Result, CP2077 4K DLAA, colour not transferred (the bridge gets colour from WGC for free):

| transport | measured MB/frame | fps |
|---|---|---|
| nothing (`NoCopies=1`) | 0 | 35 |
| MV+depth, full 4K | 34-41 | 30 |
| MV+depth, 1/2 (1920x1080) | **8.8-9.7** | **34** |
| + colour (4K) | +31.6 | 28 |

`one copy: mv=7.91 depth=7.91 MB` - the sizes are measured from the copy boxes now, not from the ini
(`streams:` line), which is what made the three bugs above visible at all.

Model, when the whole run is added up: **cost = bytes / ~13 GB/s, one copy per stream per frame**, no
fixed term. So 31.6 MB per stream is worth about 2 fps, and the only lever is bytes.

### perf: none of the copy properties matter - elimination table closed (2026-11-12)

Every variable a copy has was measured on its own, always CP2077 at 4K, `NoCopies=1` = 35 fps as the
reference:

| variable | run | fps |
|---|---|---|
| bytes | full streams vs 1/9 (`StreamPercent=300`) | 30 / 30 |
| streams per frame | MV only vs MV+depth | 30 / 30 |
| frames that copy | every frame vs every 2nd (`TransferEveryN=2`) | 30 / 30 |
| queue | game graphics queue vs dedicated COPY queue | 30 / 30 |
| position | inline in the game list vs end of frame | 30 / 30 |
| sync point | `PublishWait=1` vs `0` (no queue-side wait at all) | 30 / 30 |
| publish / consumer | `PublishOff=1` | 30 |
| nothing copied | `NoCopies=1` | **35** |

A side observation that killed the "per copy" model: the pending path only actually copied ~6 times
per second (300 calls in 48 s) while the frame rate was 30, so the loss was never proportional to the
number of copies in the first place.

So the tax is charged for *having cross-adapter writes in the frame at all*, independent of their size,
number, frequency, queue or synchronisation. The one remaining contradiction is `PublishOff=1` (30)
versus `NoCopies=1` (35): both leave the consumer without data, yet only the second is fast. The test
that resolves it needs no code - run with the copies on and the bridge process not running:
* 35 fps -> the cost is the consumer's work on the second GPU competing for the same PCIe link,
  and the lever is on the consumer side (smaller reads, fewer presents);
* 30 fps -> the cost is inside GPU1's driver path for cross-adapter writes, and it is not addressable
  from our side at all.

Until that run exists, the shipping configuration stays `CopyAtFrameEnd=0` (inline, the longest-tested
path) and the 15% is accepted.

### perf: downsample-on-producer / upsample-on-consumer cannot help - bytes are irrelevant (2026-11-12)

The last idea for the 15% was to have the producer narrow the streams and let the consumer scale them
back up (`[capture] StreamPercent`, which already exists: `stream_target_from` narrows the slot, the
bridge samples MV by normalised UV, so the upsample is free).

Measured with the copies on the dedicated COPY queue (so the earlier inline-path run cannot be blamed):

| run | bytes/frame | fps |
|---|---|---|
| `StreamPercent=100` (4K streams) | ~63 MB | 30 |
| `StreamPercent=300` (every dimension /3, pixels /9) | ~7 MB | **30** |

`pending: 600 calls, 1062 parked, 2196 copied` - the copies really happened. One ninth of the bytes
changes nothing, so the cost is a flat per-frame charge for touching cross-adapter memory at all. Every
remaining lever that only reduces work per copy is therefore dead: resolution, stream count, format,
queue, position in the frame.

Caveat found while measuring: with the NGX/tag copy path the transfer is a `CopyTextureRegion`, which
cannot scale - a narrowed slot therefore receives a CROP, not a downsample. `StreamPercent` must stay
100 for the tag path; the historical downsample pass only feeds the older render-target path.

Only two options remain: transfer on fewer frames (halve the flat cost, cost is MV accuracy) or accept
the 15%.

### perf: the 15% is the cross-adapter write itself - not CPU, not the queue (2026-11-12, final)

Completed the attribution. Moving the copies to a dedicated COPY queue (a real
`D3D12_COMMAND_LIST_TYPE_COPY` queue, 2-frame ring, graphics->copy->publish fence chain) works:
`pending: 600 calls, 1070 parked, 2186 copied` and zero crashes - and the frame rate is still 30,
exactly as with the copies recorded inline on the game's list.

| run | copies | fps |
|---|---|---|
| capture off | - | 35 |
| `NoCopies=1` (hooks + tag parsing, nothing copied) | 0 | **35** |
| inline on the game's list | yes | 30 |
| inline, 1/9 of the bytes (`StreamPercent=300`) | yes | 30 |
| dedicated copy queue, frame end | yes | 30 |

So the cost is the transfer into cross-adapter (system) memory itself, independent of the queue, of
the position in the frame and of the byte count. It cannot be recovered by scheduling; only by
transferring less often.

Two dead ends worth recording, both reproduced:
* recording our own DIRECT list into the game's graphics queue removes CP2077 at the second submit
  (three times, two different implementations) - do not retry;
* a COPY list may not contain `ResourceBarrier` (illegal on COPY type, removes the device); the
  pending path therefore skips transitions entirely.

### perf: the last 5 fps is the cross-adapter copy itself (2026-11-11, final)

Every CPU path was measured and cleared: `frame_tick` 200 us but throttled to 1.7/frame (0.34
ms), `slSetTag` 7 us x6/frame (0.04 ms), publish off, transitions off, duplicate copies off,
unwrap without QueryInterface (the remaining 68 us/submit is the driver's own call).

| run | fps |
|---|---|
| capture off (Host=0) | 35 |
| capture on, `NoCopies=1` (hooks + tag parsing, nothing copied) | **35** |
| capture on, copying MV+depth 4K | 30 |
| same, `StreamPercent=300` (1/9 the bytes) | **30** |

So the cost is a **fixed per-frame cost of copying into a cross-adapter (system-memory)
resource** - independent of size, ~4.8 ms/frame at 4K. That rules out "send fewer bytes" as a
lever and points at the mechanism: either the copy's position inside the game's command list, or
the graphics queue having to flush for a PCIe write.

Next experiments (each one run):
1. record the copies at END of frame on our own list (same queue) - is it the position?
2. record them on the COPY queue at end of frame - is it the graphics queue?

### perf attribution: the 5 fps is the object wrapper (2026-11-11 late)

Measured on CP2077, one variable per run (A = capture off, 35 fps):

| variable | fps | conclusion |
|---|---|---|
| MV only (63 MB) vs MV+depth (126 MB) | 30 / 30 | copy bytes are free |
| `NoTransitions=1` | 30 | transitions are not the cost |
| per-frame dedupe | 30 | duplicate copies are not the cost |
| `FgCut=0` | 30 | the FG break is not the cost |
| FSR-FG proxy removed | 30 | no leftover proxy cost |
| `SubmitPathCopy=0` | 30 | the old submit-path copies are not the cost |
| `PublishOff=1` | 30 | publishing is not the cost |
| `WrapLists=0` | 30 | wrapping command lists is not the cost |
| `NoWrap=1` (function hooks only) | **35** | the cost is wrapping device/queue objects |

- So every D3D12 call the game makes through our wrapped device/queue pays one extra
  dispatch; at 4K CP2077 that costs ~5 fps. The data path itself (4K MV+depth copies,
  publishing) is measurably free, which is why depth can stay enabled.
- New switches left in place: `NoTransitions`, `SubmitPathCopy`, `PublishOff`, `WrapLists`,
  `PresentHook`, `RawVtableHooks`, `RecordList`, `HookDlssgInstances`, `OffloadCopy` - all
  default to the safe value.
- Next step for the last 5 fps: hook `D3D12CreateDevice` + the queue's `ExecuteCommandLists`
  at function level and wrap nothing, so the game's calls go straight to d3d12.

### native MV/depth via the tag command buffer; pipeline review (2026-11-11)

- Breakthrough after a day of GPU crashes: the data and a LEGAL command list arrive together in
  `slSetTag`'s own argument (`cmd`), exactly as OptiScaler's `reportResource(tag, cmdBuffer)`
  does. Recording there, with the tag's declared state as the barrier source, finally produced
  `[mv]`/`[depth] EvaluateFeature copy 3840x2160` and a working 2x bridge on CP2077 with the own
  host and no ReShade: xN=2.00, xNmiss=0, 63.3 MB/frame (1.8 GB/s), 0 crashes.
- Everything before that recorded at ExecuteCommandLists (a list the game had already closed) or
  guessed states; both remove the device. Those paths stay behind switches, default off.
- `docs/design/PIPELINE_REVIEW.md`: where the remaining cost actually is (producer copies are
  0.5% of local bandwidth; the bridge is lean) and the three colour options - WGC (cheap, UI
  baked), tag HUDless+UI (OptiScaler model, correct UI), Present image (blocked by sl.dlss_g).
- `docs/design/OPTISCALER_FG_INPUTS.md`: the validity/type/flag model we adopted, with the three
  per-title switches (`FGVelocityValidNow` / `FGDepthValidNow` / `FGHudlessValidNow`).

### policy: FG follows data completeness; barrier-time capture is next (2026-11-11)

- Second violation of the same ReShade rule (a 2-frame "fresh hint" instead of an observed
  barrier) crashed CP2077 exactly like the first one. Reverted; the rule is now written down in
  `HOST_REFSHADE.md` section 7: ReShade never synthesises a StateBefore, it only reports the
  pair the game itself declared.
- Decided the shape of the real fix (section 8): capture inside `on_barrier` when the resource
  actually reaches a readable state, and let *data completeness* decide whether FG runs at all -
  menus/loading screens legitimately have no FG data and must fall back to 1x instead of
  inventing frames.

### StreamHeight removed; streams follow the render size (2026-11-11)

- `[capture] StreamHeight` is gone (with its clamp, its log field and its mention in the deploy
  scripts). It existed to cap MV/depth at 1440p so the render card spent less on the copy -
  the opposite of the current design, and it made the shared block disagree with the render
  size whenever the DLSS mode changed, after which every copy was rejected for the session.
- Streams are now sized from the render size (optionally narrowed by `StreamPercent` > 100).
  Cost at 4K: MV 63 MB + depth 63 MB per real frame instead of 28 MB - accepted, the bus has
  the headroom at 3x/4x (TRANSFER_V2 section 13).

### bisect: Host=0 alone is stable; ReShade design notes (2026-11-11)

- `Host=0` (unlock proxy only) enters the world and stays up, so the GPU crashes come from our
  capture/hook layer, not from ArchSpoof/MinArchPatch.
- Reverted `track_observed`: the tag's declared state is a HINT again, and only barriers the
  wrapper actually observed are trusted. Trusting the declaration let a stale state drive the
  transition - which is exactly what ReShade's design forbids.
- `docs/design/HOST_REFSHADE.md` maps six ReShade design rules onto our files, with the two we
  still violate: submitting inside Present (ReShade records there and flushes inside the game's
  next ExecuteCommandLists) and patching d3d12's shared functions instead of an object's vtable.
- New switches with safe defaults: `[proxy] PresentHook=0`, `RawVtableHooks=0`.

### Present copy: allocator ring with fence waits, like ReShade (2026-11-11)

- The first version reused ONE allocator and reset it on every present without checking the
  GPU: resetting an allocator whose list is still executing is undefined behaviour, and CP2077
  died with a GPU crash right after `present: own command list ready`.
- Now an 8-frame ring of (allocator, list, fence) exactly like ReShade's immediate command list
  (`d3d12_impl_command_list_immediate.cpp`): before an allocator is reset the matching fence is
  waited on, the list is submitted on the game queue and the fence is signalled after it.
- ReShade's other detail is also worth copying eventually: it only *records* in Present and
  flushes inside the game's next `ExecuteCommandLists`. We still submit from Present, which is
  ordered correctly (our copy lands before the real Present) now that the ring is safe.

### Present hook: correct vtable slots + queue plumbing (2026-11-11)

- `IDXGISwapChain::Present` is slot **7** and `Present1` is **21** (8 is GetBuffer); the first
  attempt used 8/22 and hooked GetBuffer with Present's signature, which crashed the game at
  start-up. Fixed, with the layout spelled out in the comment this time.
- `g_queue` was never set outside a hook that is not installed, so `install_present_hook()`
  bailed out (its probe swap chain needs a queue) and every log line showed
  `queue=0000000000000000`. The wrapper layer now publishes the first DIRECT queue through
  `MgpuHostSetQueue`, so the present-time colour path can come up.

### own-host Present hook: colour from the presented image (2026-11-11)

- `IDXGISwapChain::Present`/`Present1` are hooked from inside the host (address discovered from
  a throwaway swap chain, since Present is a class method and the address serves every swap
  chain in the process). On each present the host records a copy of the back buffer into the
  colour slot on its own list and submits that list to the game queue right before the real
  present: PRESENT -> COPY_SOURCE -> PRESENT, nothing else.
- The core side is one export (`MgpuHostRecordPresentCopy`) so the slot ownership stays in one
  place; it also refreshes the published colour geometry and marks the frame as copied.
- This is the colour source that does not depend on the HUDless tag's lifetime (CP2077 tags it
  `onlyValidNow`, which cannot be copied a frame later) and the one that carries the UI - so
  `ui = present - hudless` becomes possible next.

### submit hook fixed; HUDless lifetime guarded (2026-11-11)

- The game can hold an ID3D12Device newer than this SDK declares (Device10+), and then its
  `CreateCommandQueue`/`CreateCommandList` never reach the wrapper: "device wrapped" in the log
  but not one `wrap:` line, no submits, nothing published, consumer waiting forever. Both calls
  are now also hooked on the **real device's vtable** (slots 8/9), with a thread-local so the
  wrapper's own calls are not wrapped twice. Verified on CP2077: 8 queues wrapped through that
  path, `wrap: ExecuteCommandLists` visible, publishing resumed.
- `HUDLESS_COLOR` is only used when its declared lifetime allows it. CP2077 tags it
  `onlyValidNow`, so copying it a frame later reads recycled memory - that removed the device
  with `0x887a0001` (GPU hang) right after the first HUDless copy. The capture now refuses
  lc=0 and says so once.

### HUDless + separate UI, no back buffer, no WGC (2026-11-11)

- The colour stream is now the **HUDless** picture whenever the FG declaration carries one
  (`NVSDK_NGX_DLSSG_Parameter_HUDLess`, or Streamline's `HUDLESS_COLOR` tag); the back buffer
  stays only as the fallback for titles that do not hand it over.
- A **fourth stream** carries the UI colour+alpha on its own (protocol slot 3,
  `[capture] Ui` / `UiFormat`). The capture side was already harvesting the resource; it now
  copies it into the shared heap and publishes geometry for it.
- HUDless is used **only when `Ui=1`**: HUDless alone would drop the UI from the consumer's
  output, which reads as a game bug rather than a half-enabled feature.
- Bridge: places a view of stream 3, copies it per frame, and tags
  `XEFG_SWAPCHAIN_RES_UI`; `uiMode` becomes AUTO only when that stream actually exists.
- `host_smoke`-style safety: a format mismatch on either colour or UI logs the exact ini line
  to set (`ColorFormat=` / `UiFormat=`), because a copy cannot convert.

### colour on our own link, WGC off (2026-11-11)

- `Color=1` is on for CP2077: the producer copies the game's final colour into stream[0] on the
  same cross-adapter slot as MV/depth, so the bridge no longer needs a window capture at all
  (`--fg-colour shared` drives frames from the producer's publications). That removes the
  one-frame-late colour age, the D3D11 ring and the keyed-mutex hop.
- New `[capture] ColorFormat` (0 = R10G10B10A2_UNORM, the historical value): the colour slot's
  format must match the back buffer, because a copy cannot convert. A mismatch now logs the
  exact ini line to set (`[capture] ColorFormat=<fmt>`) instead of just "skipped".
- The capture still takes the *back buffer* (UI included). Hudless (pre-UI) + UI as separate
  streams is the next step; XeFG wants those split.
- Run script updated: `run_cp_own.bat` now passes `--fg-colour shared` and
  `--streams=mv,depth,color`.

### producer no longer obeys the consumer (2026-11-11)

- `[capture] FollowConsumer` (default **0**): the producer transfers everything it has and
  never looks at `request_mask` for that decision - asking it to obey another process made its
  per-frame path depend on that process's state, which is the coupling this pipeline exists to
  avoid. `FollowConsumer=1` restores v14 for A/B testing.
- The raw bits (`kRequestRawMv/Depth`) stay consumer-driven on purpose: they do not change
  what travels, only who resamples it afterwards, and only the consumer knows if it has the
  prepare pass.
- The switch board is still reported (`active_mask`, `stream_bytes`) so cost stays measurable;
  it is now an observation instead of a gate. Bandwidth is the consumer's to balance.
- Log line added: `streams: the producer sends everything it has (FollowConsumer=0)`.

### CP2077 runs end to end on the own host (2026-11-11)

- Two real bugs found by the CP2077 startup hang:
  1. `D3D12CreateDevice(..., nullptr)` **probe calls** were wrapped; `QueryInterface(iid, nullptr)`
     then returned E_POINTER to the game, which hung it. They are now forwarded untouched.
  2. `NoWrap=1` never applied - the code built `mgpu_capture_proxy.ini` instead of
     `mgpu_proxy.ini`.
- Streamline's `slSetTag` declares the resource *and* its state, so the copy path treats it as
  an observation (`track_observed`) instead of a hint. Without it CP2077's motion vectors were
  skipped for the whole session ("not the watched resource"): RED4 tags MV as UAV and never
  transitions it, so the own host had never seen a barrier. `MvFromUav=1` is on for that.
- Measured: `[mv] convert pass 3840x2160 -> 2560x1440` (state 0xC0), `stats: copied 2993/3001`
  (skipped 0), record 0.021 ms/frame, 0 faults / 0 device removals. Bridge: 30.4 fps input ->
  61.9 fps out (2x), presents 80/80, drops 0, 28.1 MB/frame = 0.9 GB/s of bus.
- Open: the consumer still reads `jitter=(0.000, 0.000)` from the shared record (producer logs
  real jitter), cadence is `NOT-EXACT` at 2x on 120 Hz, and the MV probe never measured.
  A per-window dump of the raw frame record was added to tell "producer sent zero" apart from
  "bridge paired with the wrong slot".

### test bed locked to Cyberpunk 2077 (2026-11-11)

- CP2077 (`E:\SteamLibrary\...\bin\x64`) is now the only test environment. The own-host
  stack is deployed there: `version.dll` (ArchSpoof + MinArchPatch + Host=1),
  `mgpu_capture.dll`, `mgpu_proxy.ini`, `mgpu_capture.ini` with `SlInput=1 FgSniff=1
  FgNgxHooks=1 FgCut=6 DepthDirect=1`.
- ReShade is only renamed (`dxgi.dll.reshade_off`, `game_capture.addon64.off`), and
  `dlssg_sm86` + `version.dll.sm86` are left untouched as the fallback unlocker.
- One command each way: `scripts/deploy_cp2077_own.sh` / `scripts/restore_cp2077_reshade.sh`.
  Run script: `C:\temp\mgpu\bin\run_cp_own.bat`.
- Capture log now starts with a build stamp (`build <date> <time>`), so a log can always be
  attributed to a binary - we lost a round earlier arguing about data from the old build.
- Stack, in-game settings and the unlock -> break -> capture chain documented in
  `docs/design/TESTBED_CP2077.md`.

### live stream switches + measured link budget (2026-11-11)

- Hotkeys `Ctrl+Alt+1..5` toggle mv / depth / ui / color / rawmv, `Ctrl+Alt+0` toggles the
  set. They write the same `request_mask` the CLI does, so the producer reacts on the next
  frame: no restart, no re-layout.
- OSD now shows the measured cross-adapter budget: `in 129MB 5.2G 40%`, computed from the
  producer's `stream_bytes` times the REAL-frame rate (3x/4x move the same bytes, more
  frames). `--link=13` declares the link bandwidth for that percentage.
- rawdepth ruled out by measurement: `host_smoke --cross-adapter` shows 8-byte depth formats
  (`R32G8X24_TYPELESS`, `D32_FLOAT_S8X24_UINT`) are rejected on a cross-adapter heap, so the
  depth stream must be converted on the game's GPU (already the cheapest path after S1).

### consumer-side prepare pass (dumb producer, 2026-11-11)

- The second GPU now does the resampling/format work the game GPU used to do: new prepare
  pass in the bridge (`ps_prep_mv` bilinear for motion vectors, `ps_prep_depth` nearest for
  depth), sharing the convert pass's root signature and running once per real frame.
- Producer: with `kRequestRawMv` the MV stream keeps the source's size/format and travels as a
  1:1 copy (`[mv] raw 1:1 copy`); the switch board is now resolved *before* the shared block is
  sized, otherwise the raw bits would size the stream wrong.
- Enable with `--streams=mv,rawmv`; without it nothing changes. Measured trade-off: the game
  GPU stops sampling 66 MB/frame, the bus carries 51 MB more per frame.

### dumb-producer switches (protocol v15, 2026-11-11)

- New request bits `kRequestRawMv` / `kRequestRawDepth`: the producer then does a 1:1 copy at
  the source's own resolution/format and the **second GPU** resamples/converts. Implemented
  and wired for MV (`--streams=mv,rawmv`); depth needs a `R32G8X24_TYPELESS` cross-adapter
  slot that has not been placed yet, so its bit is declared but the producer still converts.
- Nothing changes by default: `--streams=all` deliberately does NOT include the raw bits,
  because each one trades main-GPU time for cross-adapter bytes (+51 MB/frame for MV,
  +14.8 MB/frame for depth). Trade-off table in `docs/design/TRANSFER_V2.md` section 11.

### S1: depth transfer loses 59 MB/frame (2026-11-11)

- The depth source is a *typed* `D32_FLOAT_S8X24_UINT`, which the old code sent down the
  "copy into a typeless texture first" path - 29.5 MB read + 29.5 MB written, then the
  conversion pass on top. It now samples the game's texture in place (the SRV view
  `R32_FLOAT_X8X24_TYPELESS` is in the same format family), in state
  `DEPTH_READ|PIXEL_SHADER_RESOURCE`. `DepthDirect=0` restores the old path.
- Motion vectors cannot move to the copy engine: the source is the upscaler's hi-res field
  (3840x2160) and the slot holds render size (2558x1438), and a copy cannot rescale. The
  pass stays, and now says so in the log instead of looking like an oversight.
- Findings and the S2 decision table (publish MV at source size vs. resample on the game
  side) written into `docs/design/TRANSFER_V2.md` section 10.

### per-stream switch board (protocol v14, 2026-11-11)

- Every non-essential stream is now runtime-switchable without restarting anything:
  `SharedControl.request_mask` (written by the consumer at any time) with bits
  `mv / depth / color / ui / meta`. The producer reads it once per frame, and a cleared bit
  means zero copies, zero bandwidth, zero GPU time - not a hidden copy.
- The producer echoes `active_mask` plus `stream_bytes[3]`, so cost is measured rather than
  estimated, and a consumer that only knows the old `consumer_request` bit keeps working.
- Bridge: `--streams=all|none|mv,depth,color,ui,meta`. `--no-mv/--no-depth/--fg-of` fold into
  the same mask, so there is exactly one place that decides what crosses the bus.
- Design and cost table: `docs/design/TRANSFER_V2.md` (full-precision transfer, strict
  per-slot frame sync, switch board).

### own host: no-ReShade path runs on Wuchang (2026-11-11)

- `version.dll` + our D3D12 wrapper host now carries the capture core in the real game with
  ReShade completely absent: enter world, 174900 submissions, 4680 frames published, 0 faults,
  0 slow `frame_tick`, shared segment (253.6 MB / 4 slots) built by the host timer thread.
- Fixed the loader-lock deadlock: module notifications are queued again, drained every 5 ms by
  the host's own thread (installing MinHook hooks from inside the loader hook stalled the
  render thread for 120 s).
- Fixed the world-load freeze: `ExecuteCommandLists` unwrapped at most 8 command lists and fell
  back to forwarding our **wrappers** beyond that; UE submits more while loading, so the driver
  faulted with 0xC0000005. Now unwraps any count via `_alloca`, and the SEH log reports the
  fault address (filter/handler split, since `GetExceptionInformation` is filter-only).
- Moved the 253 MB shared-segment allocation out of the frame path: the frame only raises a
  flag, the host timer thread allocates - the same split ReShade makes (see FG_CLAIM 25).
- Retrospective of the ReShade-hosted pipeline (event surface, per-frame timing, the three
  disciplines that made it stable) written into `docs/design/FG_CLAIM.md` section 25.

## v0.15.15 —— 精简 + 性能：先量后改（2026-11-11）

### 实测（明末，11 分钟会话）

```text
add-on 每帧：begin 0.094 + barrier 0.028 + present 0.017 ≈ 0.14ms（一帧 25ms ⇒ 0.55%）
             热路径 0 次堆分配；barrier 653 次/帧只是定长表比对
bridge 每帧：13.5ms（present 11.3 是等 vblank，不占 CPU）vs 输入 23.4ms ⇒ ~10ms 余量
```

### 改掉的真问题

- `in_fg.cpp` 的 DLSS-G evaluate 钩子**每帧读一次 ini 文件**（`resolve_ini_path` +
  `GetPrivateProfileIntA`）→ 改 `config_int_once()`（定长表 + SRWLOCK 记忆化）+ 钩子内 static；
- 四个源里 12 处 `config_int` 统一走记忆化版本，后续新钩子不会重踩；
- 退役产物移出构建：`src/fg_stubs/retired/dlssg/`（DLSS-G 文件替换，签名门）、
  `src/version_proxy/legacy/version.cpp`（第一版 1000 行）⇒ `build.bat` 只编 FSR + XeSS-FG，
  `build_proxy.sh` 不再 stage 无用文件。两处都留 README 说明"为什么退役、别再加回来"。

### 没动的（带代价，见 FG_CLAIM §24）

颜色改共享流可砍 `game->wgc 6.3ms`，但 §16 的 XeFG HUDLESS 分配错误未解决；深度总线
14MB/帧属画质取舍；池深已是最优（`--slack 0` 排空并只留最新帧）。

## v0.15.14 —— XeSS-FG 代理 v1（三个后端全部落地）（2026-11-11）

### 新增

```text
src/fg_stubs/xess/xefg_stub.cpp       libxess_fg.dll 代理（provider 形态，690 行）
src/fg_stubs/xess/libxess_fg.def      31 个导出 = 真库 v1.3.1 的全集
src/fg_stubs/xess/libxess_fg.rc       版本资源 1.3.1.0
src/fg_stubs/xess/libxess_fg.ini.example   [stub] ForwardInit=0/1
src/fg_stubs/build.bat                三份一起来：FSR / DLSS-G（已退役）/ XeSS-FG
tests/xefg_smoke/xefg_smoke.cpp       API 级冒烟测试（按集成的真实调用顺序）
scripts/build_xefg_smoke.sh           编译上面那个用例
scripts/deploy_fg_stub.sh             一条命令装/还原（自动备份 .orig、写 ini）
```

### 形态

不是透明代理，而是 **provider**：句柄由我们分配，`InitFromSwapChain(Desc)` 接受但不转发
（真库永远拿不到 swapchain ⇒ 没有自己的节拍 ⇒ 游戏自己 present，1x），
`TagFrameConstants` / `D3D12TagFrameResource` 记录 view/proj 矩阵、jitter、MV scale、
reset、frame time 与 HUDLESS/DEPTH/MV/UI/BACKBUFFER 的指针与 extent，
其余 setter 记录 + 成功，AIL 三件转发 `.orig`。

### 自检

```text
LoadLibraryW -> handle 非 0；31 个导出全部可解析（抽查 6 个关键项）
xefg_stub_mgpu.log: "xess-fg stub loaded (provider: records declarations, never takes the
swapchain over; ForwardInit=0)"
版本资源 FileVersion 1.3.1.0
```

### API 级冒烟测试（没有 XeFG 场景时的替代验证）

原生支持 XeFG 的游戏太少（OptiScaler 同样只走 FSR / DLSS-G），所以把"加载与调用不报错"
做成用例：`tests/xefg_smoke/xefg_smoke.cpp` 按集成的真实顺序调用整套 API，
`scripts/build_xefg_smoke.sh` 编译（头文件随源码拷到 Windows 目录，避免 UNC 的 include）。

```text
真库 1.3.1（基线）       -> 多处 r=-3/-4（没有真队列/swapchain 时按调用顺序拒绝）⇒ 用例有效
我们的 stub（.orig=真库） -> 0 unexpected，0 missing export
                            status: presented=1 result=0 enabled=1
日志: init(desc) takeover NOT forwarded / const#1 jitter+mvScale+reset+dt / res: DEPTH MV HUDLESS 各自 extent
```

⇒ 加载、句柄生命周期、记录逻辑都验证过了；等有真实 XeFG 场景再验数据正确性与 1x 呈现。

待现场验收（用户提供 XeFG 场景）：`init: ... takeover NOT forwarded` + `const#1` + `res:`，
bridge 侧 `input≈1x`。依据与部署步骤：`docs/design/FG_CLAIM.md` §23。

## v0.15.13 —— DLSS-G 文件替换式代理作废（签名门），路线定为"接管加载 + 内存补丁"（2026-11-11）

### 结论

**Streamline/NGX 只加载签名有效的 `nvngx_dlssg.dll`**，手写代理放进游戏目录不会被加载：

```text
23 KB 代理（有版本资源 + 53 导出 + 全部转发）
  游戏内 "帧生成硬件不支持"，代理 DllMain 从未执行（无日志）
同一 DLL 独立 LoadLibraryW → 成功（代码无问题）
同位置换回原版 310.2.1     → FG 可开
Get-AuthenticodeSignature: sm86 随包 runtime = Valid，我方 = NotSigned
```

sm86 从来不替换文件：它随包**未修改的原版运行库**，由 `VERSION.dll` 接管这次 `LoadLibrary`，
改动全在映射后的镜像上（Detours + RVA 常量）。我方照此定案：

```text
文件不动 + 最小 version.dll（已实测让 FG 可开）+ add-on 进程内打补丁 + 第二张 GPU 出帧
```

- 四个游戏里铺过的 stub 已全部回收（0 处残留，全部 ≥7.4 MB 原版）；
- `src/fg_stubs/dlssg/` 保留源码作记录；XeSS-FG（`libxess_fg.dll`，无签名门）照 FSR 模板继续。
- 依据：`docs/design/FG_CLAIM.md` §22。

### 端到端验收（明末，同夜 04:15）

纯自有配置（`version.dll` = 最小代理；`nvngx_dlssg.dll` = 原版 310.2.1；`dlssg_sm86 Enabled=0`；
`FgSniff=1 FgSuppressNgx=0 FgProvide=0 SlInput=1 Color=0`）：

```text
游戏内 DLSS Frame Generation 可打开；无 error/crash
add-on:  fg: DLSSG hooks active (4 installed) + harvest core complete (mv/depth/camera/clip)
bridge:  input 18.7..33.9ms  ->  avg 2.00x  ->  77..101 fps  (58/58 窗口)  drops=0  presents=80/80
         render 2558x1438 -> output 3840x2160, lane sl/streamline, 颜色走 WGC
verify_pipeline.py: 9/9 PASS（api / flags 0x3191 / render / output / mv_unit / depth reversed /
                     mv_scale 2558,1438 / stream / pairing）
```

即：**从"我们自己打开 FG 可用性"到"第二张 GPU 出帧"的全链路，在不借 sm86 的前提下成立**，
游戏侧始终保持 1x（本次会话 `dlssg.eval` 一次都没出现——原版运行库在 Ampere 上不会去出帧）。

## v0.15.12 —— FSR-FG 代理后端 v1 验收通过（2026-11-11）

### 结果（CP2077 + RTX 3080 Ti）

```text
我们的 amd_fidelityfx_framegeneration_dx12.dll（14 KB 代理，装进 bin/x64）
   configure(FG effect) 吞掉      → 真运行库不接管 swapchain → 游戏 1x 呈现
   dispatch(FRAMEGENERATION) OK   → 不生成任何帧
   dispatch(PREPARE) 转发+记录    → 声明数据完整（mv=uv / camera=declared / depth=forward）
bridge: input=25.5ms(=1x, ~39fps) → avg 2.00x → 78.4 fps out @120Hz, drops=0, presents=80/80
verify_pipeline.py: 9/9 PASS
```

**游戏只出真实帧，插帧由第二张 GPU 完成，且不经过颜色通路**（WGC 抓到的就是干净的 1x）。

### 新增文件

- `src/fg_stubs/fsr/framegen_stub.cpp`（约 250 行）：ffx-api 代理，只有两类调用被过滤；
- `src/fg_stubs/build.bat` + `scripts/build_fg_stubs.sh`：构建产物落到 `build/`；
- 部署到 CP2077 时原件备份为 `amd_fidelityfx_framegeneration_dx12.dll.orig`。

### 三个坑（每个都对应一次崩溃，已修）

| 现象 | 原因 | 修法 |
|---|---|---|
| `DEP at 0x0` | `ffxQuery` 回 OK 却不填输出 → 游戏调空回调 | 未实现类型回 `UNKNOWN_DESCTYPE` |
| `DEP at 0x0` | 空壳拒绝 upscale 上下文，超分被抢走 | 改成代理，转发给 `.orig` |
| `read at 0x28` | 只吞 `0x20002`，其它 FG configure 转发给"无 swapchain"的真运行库 | 按 effect 号吞掉全部 FG configure |

### 数据语义修复

- `mv_unit`：`motionVectorScale == renderSize` ⇒ UV（之前误标 pixels，scale 会被再乘一次分辨率）；
- 相机平面反向（CP2077 `near=16000/far=0`）→ 自动交换，不再判成"无相机"。

### 另外

- `scripts/verify_pipeline.py`：深度方向改为优先比对 bridge 的 `producer semantics` 行，
  并用"最后一次发布"而不是"历史集合"（跨配置日志会产生假失败）；
- 明末/CP2077/PBZ/Khazan 的 FG 可用性仍由各自的最小代理或 sm86 提供，本轮未改动。

## v0.15.11 —— 最小代理成功：300 行复刻 sm86 的核心机制（2026-11-11）

### 结果

**明末**：`dlssg_sm86 Enabled=0`，只用我们的 `version.dll`（27 KB）→ **游戏内 DLSS Frame Generation 可开**，
add-on 同时拿到完整数据：

```text
arch: physical architecture is 0x170
arch: answering 0x90 instead of 0x170       ← sl.common 在 slInit 里比较的值
harvest: mv=y depth=y camera=y clip=y hudless=y
input source: switching ngx -> sl
```

**CP2077**：同样正常启动 + 出现 `NVIDIA DLSS Frame Generation`（此前"全功能代理"下崩了三次）。

### 只保留两件事

```text
① NvAPI_GPU_GetArchInfo → 0x190（否则 slInit 里 sl.dlss_g 被卸载，游戏不给选项）
② nvngx_dlssg 里 `mov eax,0x190; ret` 的立即数 → 物理架构 0x170
   （sm86 的 coreArch 手法；核心在 slInit 内的比较只有改常量才能过）
```

### 删除的四个子系统（都是猜测，且都出过故障）

| 删除 | 曾经造成的故障 |
|---|---|
| NGX 能力/需求/Init 钩子 | CP2077 `0x400` 崩溃，且并非必需 |
| 版本号改写 | 版本块越界写 → 堆损坏 → 崩溃 |
| DXGI 适配器 vtable | 首版截断 vtable → fatal error（读了适配器，但不是闸门） |
| `GetProcAddress` 钩子 | loader lock 死锁 → 进程卡死 |

### 全链路闭合（明末，同一晚）

```text
代理   answering 0x90 instead of 0x170（sm86 Enabled=0）
add-on input source: sl   harvest: mv depth camera clip 全 y
bridge input=22.9ms(=1x) -> avg 2.00x -> 87.3 fps out @120Hz  drops=0
verify 9/9 PASS
```

游戏输出因为运行库出不了插值帧而天然是 1x，**颜色继续走 WGC**，不需要共享颜色通道。

### 文件

- 新增 `src/version_proxy/version_min.cpp`（300 行，替代原 900 行的 `version.cpp`，后者保留作实验记录）；
- `scripts/build_proxy.sh` 现在编译 `version_min.cpp`；
- `mgpu_proxy.ini` 只剩两个开关：`ArchSpoof` / `MinArchPatch`（默认都开）；
- 已部署：明末、CP2077、PBZ、Khazan（各自的 sm86 代理均改名 `.sm86` 停放，`Enabled=0`）。

## v0.15.10 —— 收权：FG 可用性交回 sm86，我们只做数据与出帧（2026-11-11）

> 结论性的一轮。昨晚那套"从外部把 FG 说成可用"的代理线（`src/version_proxy/`）**停用**，
> 三个游戏全部恢复成 `sm86 + 我们的 add-on（只读采集）`。

### 为什么

| 标题 | 我们的代理实测 |
|---|---|
| Wuchang | 架构 `0xD8265D24`、插件版本 310.9.16、适配器名/ID 全部伪装命中 → **仍无 FG**；它连 NGX/Streamline 都不问就返回"硬件不支持" |
| CP2077 | 连续三次启动崩溃（`0xFFFFFFFF`、`0x400`×2），最后一次连日志都没有 |
| PBZ | 唯一"成功"：能力被我们翻成 `Available=1`，游戏内 FG 打开（`mode=1`），但该题本就支持 FSR-FG，无法归因 |

sm86 的文档给出了根因（ARCHITECTURE.md 633/637）：运行库把最低架构**硬编码成 `0x190`**，
NGX 核心在 **`slInit` 内**拿它和自己的物理架构比 → `FeatureInitResult=0xbad0000b` → `Available=0`。
**那次比较早于任何外部注入点**，所以"从外面回答"注定无效；sm86 之所以行，是它**替换了运行库本体并改其内部答复**。

### 分层（唯一支持路径）

```text
FG 可用性  → dlssg_sm86（原样使用，不重写、不替换）
数据采集   → 我们的 add-on：FgSniff=1 SlInput=1，FgProvide=0（只观察）
出帧       → 我们的 bridge（独立 GPU 上重放 XeFG）
```

### 代码与配置

- 三个游戏的 `version.dll` → `version.dll.mgpu-off`，sm86 原代理已还原；`dlssg_sm86 Enabled=1`；
  明末的引擎 `nvngx_dlssg.dll` 也还原成 310.2.1（`.orig`）；
- add-on：`FgProvide=0 / FgSuppressNgx=0 / Color=0`；
- `src/version_proxy/` 保留（带 `mgpu_proxy.ini` 四个开关，默认安全态），文件头记下三个崩溃点：
  NVAPI id 签名不符、早期预加载 `_nvngx`、版本块越界写；
- 知识性结论已固化进 `docs/design/FG_CLAIM.md` §17–§19：
  真实 NVAPI id `0xD8265D24`、Streamline 闸门在 `slInit`、运行库最低架构硬编码 `0x190`。

## v0.15.9 —— 不借 sm86 的第二块：`version.dll` 代理（架构答复 + 时机）（2026-11-10）

> **实验代码，未发布**。默认路径不变：没有 `version.dll` 时一切照旧。

### 结论先行：add-on 一定来不及

```
23:39:41  Streamline slInit（判定 sl.dlss_g "本平台不支持" 并卸载插件）
23:39:48  我们的 nvapi_QueryInterface 才钩上      ← 晚 7 秒，整个会话没人问架构
```

`sl.interposer.dll` 里有 `getNVDAArchitecture` / `NvAPI_GPU_GetArchInfo(...)` /
`Feature '%s' is not supported on this platform` 这些字符串，sm86 的文档也写明
**slInit 里就拿架构比 AD100（0x190）**。所以需要更早的注入点 → `version.dll` 代理。

### 新增 `src/version_proxy/version.dll`（约 350 行，不做任何 FG）

```text
· 只做一件事：把 nvapi 返回的 NV_GPU_ARCH_INFO.arch 从 0x170 改成 0x190
· 进程启动 ~1.2 s 武装（DllMain 起线程轮询 nvapi64 / nvapi64_impl）
· 转发 version.dll 的 16 个导出到 system32\version.dll
· 不伪装设备名，不跑 NVIDIA 的任何代码
· 拒绝列表（硬要求，sm86 #535/#538/#540/#542）：_nvngx / nvngx_* / nvapi* /
  DriverStore / \NVIDIA\NGX\models\（models\sl_* 除外）看真实架构
```

### 关键发现：公开文档的 NVAPI id 是错的

带调用者的 sweep 证明 `0x2D3A1A3A` **不在** sl.interposer 问的 7 个 id 里（也不在其字节中）：

```text
sl.interposer.dll → 0150E828, E5AC921F, D8265D24, 593E8644, 33C7358C, 2926AAAD, AD298D3F
```

于是改成**通用包装 + 结构自证**（调用后看两个参数指向的结构首 dword 是否 0x20010），
命中即改 arch 字节，并把 id 写进日志：

```text
arch: spoofed through NVAPI id 0xD8265D24 (struct was arg2)   ← 真正的 GetArchInfo
```

### 状态

- ✅ 架构伪装生效、且完全不依赖 sm86；add-on 侧补了核心的 `GetFeatureRequirements`；
- ❌ `sl.dlss_g.dll` 仍未进进程 → Streamline 还有第二个否决条件（插件自身能力检查 /
  宿主未通过 `slSetFeatureLoaded` 请求 / 另一条读架构路径）；
- ⚠️ 顺带实测两条失败路径（都已回退，代码里留了注释）：
  - 把包装器放宽到"Streamline 问的任意 id"（6 参数转发）→ 游戏启动即退出，
    因此放在 `MGPU_PROXY_WRAP_STREAMLINE 0` 后面，默认关；
  - 在 DllMain 里再起一个线程轮询模块表 → 同样表现为启动即退，已移除。
- 另：最后一次验证时**即使换回 sm86 原有 `VERSION.dll`、完全不加载我们的代理，游戏也起不来**
  → 那是 Steam 的启动状态问题，不是代理的问题（需要手动启动）。

详见 `docs/design/FG_CLAIM.md` §17。

## v0.15.8 —— 实验 A：颜色也走我们的共享流（接管输出的第一段）（2026-11-10）

> **未发布的实验代码**，全部在开关后面，默认行为与 0.15.7 一致：
> add-on `Color=0`（默认关）、bridge `--fg-colour shared`（默认不走）。

### 动机

`FgSuppressNgx=1` 把游戏自己的 FG 内容切断后，实测：**屏幕黑、RTSS 显示 2x、bridge 仍 2.00x 出帧**
—— 出帧节拍住在 FG swapchain 里，而颜色一直来自 WGC 抓游戏窗口（抓到的正是黑帧）。
所以要"游戏只出真实帧、我们出插帧"，必须自己把颜色运过来。

### add-on

- **颜色槽位规格修正**（真 bug）：`stream_specs()` 写死 `5120x2880`，比 `kStreamMaxW/H` 还大，
  而 `reserve_bytes()` 按预留算 → `used > reserve` → **只要 `Color=1` 整块共享内存就被禁用**。
  现在内容尺寸 = 真实后台缓冲，容量 = `kStreamMaxW/H`，逐帧发布 `streams[0].width/height`。
- 拷贝 `CopyResource` → `CopyTextureRegion` + 内容矩形，后台缓冲不必等于槽位尺寸（同 v9 规则）。
- 实测：`[color] CopyTextureRegion 3840x2160`，`shared ready: used=380.3 MB`。

### bridge

- `--fg-colour shared`：不启动 WGC，颜色从 `stream[0]` 跨卡拷进颜色汇（与 MV/depth 同一条路），
  **帧节拍改由生产端 `frames[].published` 驱动**。
- 颜色汇格式跟随来源（共享模式 `R10G10B10A2` = 游戏后台缓冲原样 HDR10 PQ）；
  convert pass 新增 `mode 3`（源已是显示编码 → 直通，避免 PQ 二次编码）。

### 结果

```text
colour sink 3840x2160 fmt 24 (HDR10 PQ, straight from the add-on) shared to D3D12
--fg-colour shared: copying 3840x2160 from the producer's colour slot
--fg-colour shared: driving frames from the producer's publications (4 slots)
cadence: input=26.0ms = 3.12 vblanks -> interp=0 (avg 2.00x, 76.9 fps out) drops=0
```

`input` 从 14 ms（游戏 FG 的 2x 节拍）变成 26 ms（**真实帧节拍**）—— 节拍这一关过了。

**遗留**：共享颜色模式下 XeFG 报 `Failed to allocate memory for a copy of HUDLESS_COLOR` /
`Resource pointer is null.`，而同一份 bridge 的 WGC 模式 0 条。候选原因与复现命令见
`docs/design/FG_CLAIM.md` §16。

## v0.15.7 —— 黑屏找到真凶：我们的 NGX 钩子和 sm86 解锁器抢同一段 prologue（2026-11-10）

明末（Wuchang）实测，同一份游戏内设置下只改 `game_capture.ini`：

| 配置 | 游戏自己的 FG | 我们的 `sl` 数据 | 画面 |
|---|---|---|---|
| `FgSniff=1 SlInput=1` | feature 建了、每帧 evaluate，但出不了帧 | 完整 | **全黑** |
| `FgSniff=0 SlInput=0` | 正常 2x | 无 | 正常 |
| `FgSniff=1 SlInput=1 **FgNgxHooks=0**` | 正常 2x | **完整** | **正常** |

`dlssg_sm86` 是在 `nvngx_dlssg.dll` **内部**打补丁做帧生成的（`"hooks":{"evaluate":
["0x370c0","0x376aa"]}`，`runtime_redirect mode=bundled`，`kernel_pack image=cubin_sm86
kernels_matched=71/71`）—— Ampere 上只有它那份重编译副本才出帧。我们再去 MinHook 同一模块的
`DLSSG_NVSDK_NGX_D3D12_EvaluateFeature`，两套内联钩子写同一段 prologue ⇒ 我们的
`dlssg.eval` 照常打印、它的帧生成废掉 ⇒ 全黑。

### 新增 `[capture] FgNgxHooks`（默认 1，保持旧行为）

`FgNgxHooks=0` 不钩 `nvngx_dlssg.dll` 的 `DLSSG_*` 导出。`sl` 车道不需要它
（`slSetConstants`/`slSetTag` 给的是同样的数字）。

```text
装了 sm86 之类解锁器的游戏：FgSniff=1 + SlInput=1 + FgNgxHooks=0
→ sl 数据完整 + 游戏自己插帧正常 + 画面正常
```

- add-on `b17f3e0decd0958febb01ddf0e22cbeb`；安装器保留键从 5 个变 6 个（加 `FgNgxHooks`）；
- 该配置下明末 `verify_pipeline.py` **9/9 PASS**，bridge `2.00x` / `142.2 fps @120Hz` /
  `drops=0`；详表见 `docs/design/FG_CLAIM.md` §15.4。

## v0.15.6 —— DLSS-G 提到最高优先级 + 逐字段核对工具（2026-11-10）

### 优先级：`sl`（DLSS-G）第一

```text
旧： xefg → sl → ffxfg → ngx → ffx → xess
新： sl → xefg → ffxfg → ngx → ffx → xess
```

理由：DLSS-G 是主流、是唯一在三款引擎上都验证过的车道，声明也最全（相机 + clip +
token + HUDLESS）。其余车道存在的意义是"别的集成方式下不要瞎"，不是盖过能用的那条。

### 新增 `scripts/verify_pipeline.py`：逐字段核对"我们发布的"与"下游用的"

```bash
python3 scripts/verify_pipeline.py <game_capture.log> <bridge_*.txt>
```

把 add-on 当生产者、bridge 当消费者，逐项比对 `api` / `flags` / `render` / `output` /
`mv_unit` / `depth→initFlags` / `mv_scale` / 流尺寸 / `frame-key` 配对，输出 PASS/FAIL 表，
退出码即结论。判定用"下游的值必须出现在上游发布过的值集合里"（两份日志跨多次配置变更，
逐条等值会假失败）。

实测：CP2077 **9/9 PASS**（`sl/streamline`、`0x3191`、uv、reversed→`inverted_depth=1`、
`mvscale=(3840,2160)`、`2560x1440` 流、`frame-key pair`）。

判定规则经过三次实测打磨，才不会被"日志口径"骗：

| 最初的写法 | 为什么会假失败 | 现在 |
|---|---|---|
| 上游"最后一条"对比下游值 | 两份日志跨多次配置变更（PRAGMATA：2228x1254 vs 更早的 2560x1440） | 下游值必须在**上游发布过的值集合**里 |
| 字符串比 mv_scale | 上游 6 位小数 / 下游 4 位 | 数值比，容差 0.5% |
| 严格要求相等 | 明末把 2558x1439 的标签裁成 2558x1438 的偶高流，差 1 行（0.07%） | 容差内 PASS 并打印偏差 |

**明末（Wuchang）实测 9/9 PASS**：`sl/streamline`、`0x3191`、`2558x1438`、`uv`、
`inverted_depth=1`、`mvscale≈(2558,1439)`（偏差 0.07%，偶高裁剪）、`token=n`⇒无 `frame-key`。
同一次会话里还观察到 `input source: switching ngx -> sl` —— 新的 `sl` 最高优先级在明末生效。

> 明末这次是**游戏内 FG 打开**才有 `sl` 数据，代价是画面全黑（shim 的 DLSS-G 输出在 HDR10 上
> 不成立）。完整对照与结论见 `docs/design/FG_CLAIM.md` §15.3。

### 顺手补的日志缺口（由上面的核对暴露）

`input_flags` 会在运行中变化（相机矩阵出现时才置 `HasMatrix`），而 producer 行只在初始化
打过一次 —— 核对时出现"下游 0x81 / 上游只记录过 0x35"。现在变化时补一行：

```text
producer flags -> 0x81 (has_matrix=1 has_camera=0 mv_active=1)
```

- add-on `e0f5836c6a060332487621168d65fb19`；六处游戏目录已同步。

## v0.15.4 —— `SlFgOff=1` 不能当默认：会黑屏但仍按补帧节奏跑（2026-11-10）

> 用户实测两种形态，根因相同：**`SlFgOff` 只改写"游戏递给 Streamline 的选项"，我们不拥有 present 链路。**

| 形态 | 现象 | 原因 |
|---|---|---|
| 题自己管 present | **黑屏**，但 fps 显示的是**补帧后**的数值 | 我们让 SL 不再出帧，可游戏/它自己的 FG 仍按 2x/3x 节奏 present |
| FG 在 SL 下面被模拟（`dlssg_sm86` + `Engine=XeSS`） | 输出**依旧是补帧后**的画面 | 改写 SL 的 `mode` 管不到 `nvngx_dlssg` 那一层，模拟器照旧插帧 |

⇒ 默认配置**改回"只读不写"**：

```ini
SlInput=1     ; 读游戏的 FG 声明
SlFgOff=0     ; 不改写游戏选项（默认）
Claim=0       ; 不假装 FG 可用
```

并且 add-on 在 `SlFgOff=1` 时启动会打一行显式警告：

```text
WARNING: SlFgOff=1 rewrites the game's DLSS-G options. A title that owns its present path -
or emulates FG below Streamline (dlssg_sm86 with Engine=XeSS) - can go BLACK while still
running at the FG cadence. Prefer turning the game's own frame generation off in its menu.
```

**正确做法（写进 `FG_CLAIM.md` §13.2）**：

1. 停游戏自己的 FG —— **用它自己的菜单**（最可靠，present 链路一致）；
2. `SlFgOff=1` 只在**已验证**的题上开（PRAGMATA 实测可用），且不要和"模拟型 FG"混用；
3. `FgSuppressNgx=1` 继续默认关：CP2077 上它同样会黑屏。

- 包内默认 ini / `deploy_game.sh` / 六个游戏目录的 ini 已统一为 `SlInput=1 / SlFgOff=0 / Claim=0`；
- add-on `ea8606b0e406e4da3a42a03f4a7a5799`。

## v0.15.3 —— 默认配置改成"只让我们的 FG 出帧"（2026-11-10）

> 起因：装完包之后 `game_capture.ini` 是"只玩不抓"的默认值（`Log=0`、`SlInput=0`、
> `FgSniff=0`），于是游戏自己开 FG 时我们**既不抓也不管** —— PBZ、Wuchang 各踩一次。

- 包内默认 ini / `deploy_game.sh` / 安装器写入的配置，统一改成：

  ```ini
  SlInput=1     ; 读游戏的 FG 声明（lane 1 的数据面）
  SlFgOff=1     ; 把游戏请求的 DLSS-G mode 改写成 eOff —— 它不出帧
  Claim=0       ; 不假装 FG 可用（那会让画面重复/黑，仅取数用）
  ```

  即**默认"游戏的 FG 全停，只有我们的 FG 出帧"**，但声明照旧读，数据不受影响；
  `Log` 仍默认 0（正常玩不落盘），要看日志自己开。

- `packaging/reshade_pack/README.txt` 增加"想抓 FG 数据"一节，写明这三个键。
- 已在 CP2077 / PRAGMATA（两处）/ PBZ / Wuchang / Khazan 的 ini 上统一生效。

> 例外（已在 `FG_CLAIM.md` §12/§14 记录）：PBZ 的 FG 是 `dlssg_sm86` 在 `nvngx_dlssg` 层
> 用 XeSS 模拟的，`SlFgOff` 管不到那一层；要真停得改 `dlssg_sm86.ini` 的 `Enabled=0`
> （代价是这台 3080 Ti 上就没有 DLSS-G 可读了）。

## v0.15.2 —— 重建 UI（它把协议版本编进去了）+ 0.15.1 的安装器修复（2026-11-10）

> 症状：add-on 升到协议 **14** 之后，UI 仍然提示 **"版本不符（producer v14 / expect v13）"**。

原因：`mgpu_ui.exe` 在编译时就把 `mgpu::kSharedVersion` 编进去了（`src/mgpu_ui/mgpu_ui.cpp`
里 `v->version == mgpu::kSharedVersion` 是硬比较），而 0.15.0/0.15.1 的包里带的是 10:54 的
旧产物（v13）。**协议一变就必须连 UI 一起重建**，这条以前没写进流程。

- `build/mgpu_ui.exe` 重建：`475cd911…` → `5822cb8a24d36d3bfe9dcdf3fc07405a`；
- 同步部署到 `C:\temp\mgpu\bin\mgpu_ui.exe`；
- 包重新发布（`dist/mgpu-0.15.2.zip`），三件产物同版：
  bridge `c9162063` / add-on `c3234be8` / ui `5822cb8a`。

## v0.15.1 —— 安装器不再吞掉 `game_capture.ini` 的操作键（2026-11-10）

> 实测（PBZ）：用 0.15.0 的 `game-pack` 安装后，`game_capture.ini` 被整份覆盖回默认
> （`Log=0`、没有 `SlInput`/`SlFgOff`/`Claim`）⇒ **DLSS-G 抑制和日志全没了**，
> 表现就是"新版本装了，但游戏自己的 FG 照旧生效"。

- 安装器 `Install-One`：覆盖 `game_capture.ini` 时**保留旧的五个操作键**
  （`Log` / `FgSniff` / `SlInput` / `SlFgOff` / `Claim`），其余仍用本包默认，
  并打印 `保留旧配置: Log=1 FgSniff=1 SlInput=1 ...`；
- 包内默认 ini 补上 v14 的三个键（`SlInput=0` / `SlFgOff=0` / `Claim=0`，带注释），
  新装的机器也能看到这些开关存在。

## v0.15.0 —— 四条 FG 车道 + 动态输入（协议 v14，2026-11-10）

> 本版把"逐游戏探 SR"换成了"按 FG 契约取数 + 运行时判定"。实机覆盖：

| 游戏 | 引擎 | 走到的车道 | 关键结论 |
|---|---|---|---|
| Cyberpunk 2077 | RED4 | `sl`（DLSS-G） | 游戏自己的投影（`fovY=51.01°`）、`frame-key pair`、4K 原生 2x **120.0 fps 整档**（`gpu 1.9–2.6ms`、`present 5.1–7.2ms`）。开光追时 MV 变 `RGBA16F` + 处于 UAV，已加防护不再打掉设备 |
| The First Berserker: Khazan | UE 4.27 | `ngx`（DLSS-SR） | SL 只做 FG、且切档位后停发；MV 实测 `69px`（像素制）；4K 2x `gpu` 中位 7.3ms |
| PRAGMATA | RE Engine | `sl`（DLSS-G） | 第一个真正 `input source: sl` 的题；`reset=1` 首次抓到；`validUntilEvaluate` 的 DEPTH；FG 开 + `SlFgOff=1` 拿到全套声明而游戏不出帧 |
| Phantom Blade Zero | UE 5.7 | `ngx → sl` **动态切换** | `input source: switching ngx -> sl (a higher-priority lane is live)` 与静默回落都实测到；lifecycle 是第三种 `onlyValidNow` |
| 说明：原生 FSR-FG / XeSS-FG | — | `ffxfg` / `xefg` | 代码就绪（含覆盖度自报 `XeSS-FG data coverage 6/6`），**待样本**；PRAGMATA 的 FSR-FG 只发 generation 不发 PREPARE，sm86 用私有 libxess_fg，都不可用 |

> 设计：`docs/design/FG_CLAIM.md` §11/§12；协议：`docs/design/PROTOCOL.md` 的版本表

协议 **v13 → v14**，只追加两个 `uint32`（`ProducerInfo.declared_frames` / `harvest_flags`）
和一个 flag 位，**不新增纹理流**。

### 四条 FG 车道（通用工具的核心）

| 车道 | 输入源名 | 状态 |
|---|---|---|
| 游戏跑 DLSS-G | `sl` / `ngx` | ✅ 在用（CP2077、PRAGMATA） |
| 游戏跑 XeSS-FG | `xefg` | ✅ 新增，**零转换**（XeFG 的原生输入就是我们的输入） |
| 游戏跑 FSR-FG | `ffxfg` | ✅ 新增，解析 `ffxDispatchDescFrameGenerationPrepare` |
| 游戏没有 FG | `ngx` / `ffx` / `xess` / `depth` | ✅ 现状 |

注册顺序即优先级：`xefg → sl → ffxfg → ngx → ffx → xess`。`Input=auto` 仍然是
"谁先给出活帧就用谁"，配合 250 ms 新鲜度门控，游戏中途切换 FG 类型会在 90 帧内自动重新挑选。

### 用户可见的变化

- **源停发时不再重放旧帧**：add-on 发现源超过 250 ms 没有新帧，就打
  `input: source sl is STALE - the last frame is being replayed, consumer should fall back`
  并置 `kInputFlagMvSuspect`，bridge 会提示改用 `--fg-of` / `--no-mv`。
  （实测踩过：PRAGMATA 的 Streamline 停止发布后，bridge 继续消费同一帧 MV/深度 2.5 分钟，
  表现为插帧"双影"。）
- **能看到"游戏自己出了几帧"**：bridge 启动时打印
  `producer declaration: framesToGenerate=N <== the game itself is generating frames`
  和 `harvest=0x...`（mv/depth/cam/clip/token/reset/hudless/ui 的位图）。
- **多 viewport 不再串数据**：分屏 / 反射探针这类一帧里会有多个 viewport 的题目，
  add-on 只采纳一个（另一个在当前 viewport 静默 250 ms 后才接管）。
- **车道优先级改成"持续判定"**（PBZ 实测暴露）：以前某个源一旦被钉住，只要它还在出数就
  永远不换 —— PBZ 上 `sl.const`/`sl.tags` 一直在出，`input source` 却还是 `ngx`。
  现在每 120 帧复查一次"更高优先级的车道是否活着"，是就切，并打
  `input source: switching ngx -> sl (a higher-priority lane is live)`；
  `ProducerInfo.input_api` 也改成每帧发布，bridge 的 `api=...` 会跟着变。
- **游戏显式 release DLSS-G feature 时立刻停发**：新增 `NVSDK_NGX_D3D12_ReleaseFeature`
  （含 `DLSSG_` 前缀版）钩子，打 `fg: the game released its DLSS-G feature - capture
  invalidated`，不再等 250 ms 新鲜度门控（那是兜底，不是检测手段）。
- **光追下不再把设备打掉**（CP2077）：开光追后 MV 的**像素格式**从 `R16G16_FLOAT(fmt 34)`
  变成 `R16G16B16A16_FLOAT(fmt 10)`，而转换 pass 的 SRV 是写死的 RG16F —— typed view
  格式不匹配 ⇒ `DXGI_ERROR_DEVICE_REMOVED / INVALID_CALL` ⇒ 游戏闪退。
  现在 SRV 跟着源格式走；另外 MV / 深度 / 裁切拷贝三处都拒绝 `UNORDERED_ACCESS(0x8)` 的源
  （`skipped: left in UNORDERED_ACCESS ... Use --fg-of or --no-mv`），
  并加了 `[capture] MvFromUav=0|1`（默认 0）用于验证那一条是否仍然致命。

### 协议 v13：Streamline 输入源 + frame_key↔WGC 配对（同一版本内）

> 阶段记录：`docs/findings/FG_INPUT_V13_2077.md`；设计：`docs/design/FG_INPUT_PLAN.md`

### 用户可见的变化

- **CP2077 上 `--fg-fov` 可以退役**：投影来自游戏自己的 `cameraViewToClip`，
  near/far/FOV 也来自 `sl::Constants`。
- **MV 尺度不再被 CP2077 的 `mvecScale=(1,1)` 误导**：用同一帧 DLSS 的
  `MV.Scale=(3840,2160)` 侧通道修正，在线 probe 独立判成 `uv`。
- **颜色 ↔ MV/深度 的配对更稳**：新增 `frame_key` + `frame_qpc_ms`，bridge 用
  WGC `SystemRelativeTime` 选“最新且不晚于它”的槽位；不匹配时回退轮转。
  `--no-pair-key` 可关闭。
- **`resetHistory` 接通**：`sl::Constants.reset` / `DLSSG.Reset` 进入
  `FrameShared.fg_flags`，bridge 写 `cd.resetHistory`。

### 实现

- 协议 v12 → **v13**，只在 `FrameShared` 末尾追加元数据，**不新增纹理流**：
  `clip_to_prev_clip` / `prev_clip_to_clip` / `frame_key` / `frame_qpc_ms` /
  `present_id` / `fg_flags` / `mv_invalid_value` / camera vectors。
- `in_fg.cpp`：`[capture] SlInput=1` 把只读 Streamline 嗅探器变成输入源；
  `slSetConstants` / `slSetTag*` / `slEvaluateFeature` 产出 `FrameInputs`。
- `in_ngx.cpp`：发布 DLSS `MV.Scale` 侧通道；`FgSuppressNgx` / `slGetPluginFunction`
  保留为实验入口。
- `game_capture.cpp`：发布 v13 字段；源自带 view-to-clip 时直接使用。
- `bridge.cpp`：v13 配对 + `resetHistory`；`--no-pair-key`。

### 验证结论：颜色不入槽也能走通全链路（PRAGMATA，2026-11-10）

```text
游戏内 FG 开（present 50 → 77） + 我们的 bridge 2x
  → add-on 拿到"FG 激活"状态下的完整数据
    harvest: mv=y depth=y camera=y clip=y token=y reset=y hudless=y ui=y scaling=io expose=y
  → 颜色仍由 WGC 抓游戏窗口（producer 侧零额外拷贝，§9 的占用账成立）
  → bridge 用这些数据做自己的插帧上屏（xN=2.00、presents=80/80）
```

⇒ **"我们在上面再插一层"这件事不需要颜色入槽**：FG 契约提供数据面、WGC 提供画面、
bridge 负责输出。HUDless/UI 只有 FG 管线初始化后才出现，这也是"必须让游戏相信 FG 在跑"
（或至少初始化过）的实证；`SlFgOff` 决定游戏自己出的帧要不要（0=要，1=只要管线/不要帧）。

### 限制（不是 bug）

- **场景 2（Khazan / UE 4.27）**：Streamline 只服务 DLSS-G，DLSS-SR 走游戏自带的
  NGX，输入源是 `ngx`；SL 的 DEPTH/MV/HUDLESS 三个 tag 每帧都在，但 FG 管线一重建
  SL 就停止发布（详见 `docs/findings/FG_INPUT_V13_KHAZAN.md`）。
- **`LoaderHook=0` 的标题曾经装不上任何 Streamline 钩子**（PRAGMATA，RE Engine）：
  `in_fg.cpp` 没调 `ensure_minhook()`，`MH_CreateHook` 全部返回
  `MH_ERROR_NOT_INITIALIZED`。已修；修完 PRAGMATA 成为第一个 `input source: sl` 的游戏
  （`docs/findings/FG_INPUT_V13_PRAGMATA.md`）。
- **CP2077 不要用 `FgSuppressNgx=1`**：它挡不住 SL present 链路，会黑屏且游戏仍按
  FG 节奏跑。正确做法是游戏内关 DLSS Frame Generation，bridge 做唯一 2x。
- `SlFgOff` 在 CP2077 上不可达（`slDLSSGSetOptions` 没经过当前 hook 路径）。
- CP2077 的 SL `DEPTH` tag 不是每帧都出现；当前会保留上一份 depth 指针，
  **深度新鲜度仍未证明**。
- `clip_to_prev_clip` / `prev_clip_to_clip` / camera vectors / invalid marker /
  dilated/3D flags 只发布，不消费；HUDless/UI 仍未做。
- 颜色仍走 WGC；4K 在 6750 GRE 上 `--den 1` 实测 `gpu` 0.7–9.8ms、`present`
  6–9ms、80/80 presents（前提是游戏 FG 已关闭）。

## v0.14.0 —— 多游戏通用：读游戏自己说的 + WGC 节流解除（2026-11-10）

**这一版有两条主线。** 一是"别再逐游戏猜参数"：MV 单位/符号/深度方向/投影全部改成**读游戏自己的声明**
（猜不出来的才由在线探针兜底）。二是把一整天排查出来的根因修掉 —— **WGC 捕获被默认节流在 ~60 fps**，
这是此前"怎么调都差一点"的真正原因。

### 用户可见的变化

| 以前要试 / 别扭的地方 | 现在 |
|---|---|
| `--mv-flip` 逐游戏试 MV 符号 | 按 API 契约（上一帧−当前帧），**不用开** |
| `--fg-mv-scale` 逐游戏试单位 | 从 `MV.Scale` / XeSS 速度空间推导（`≈1`=像素、`≈渲染宽`=UV、`≈宽/2`=NDC） |
| `--fg-depth-inv` 逐游戏试深度方向 | 用游戏的 `DepthInverted`，缺失时由探针实测 |
| `--fg-fov` 猜投影 | 用**游戏自己的投影矩阵**；游戏什么都不给才回退 `--fg-fov` |
| 高帧数场景"只有 60 fps" | **WGC 节流默认关闭**（见下） |
| 插帧在高速场景反而拖累 | UI「模式」新增 **关闭 (1:1 直通)**；bridge 会主动提示该切了 |

### **最关键的一条**：WGC 捕获节流默认关闭

`GraphicsCaptureSession::MinUpdateInterval` 默认是一个合成器周期（≈16.6 ms）⇒ WGC 最多交付
~60 帧/秒，**游戏跑再快也只能拿到 60**。实测设为 0 后"操作瞬间流畅跟手了很多"，且此前所有
捕获率测量都卡在 59.6 fps 也由此解释。现在 bridge 默认 0（`--wgc-minint 16` 可恢复旧行为）。

### 稳定性

- **add-on 热卸载崩溃**（`std::mutex` 已析构后仍被回调 → 空指针）：改 `SRWLOCK` + `g_addon_alive`
  守卫。这是明末"装上 add-on 就启动崩溃"的根因；
- loader 钩子改为**只入队**、在帧内排空（不在 `LoadLibrary` 里装 MinHook 跳板）；
- `sl.interposer` 钩子收回为**仅 `FgSniff=1` 时安装**（相机默认走 XeFG frame constants）。

### 已知边界（不是 bug）

- **非整数倍率 + 固定刷新率无解**（如 53 fps 在 120 Hz 上 = 2.26 帧/输入）：只能靠面板 VRR
  或把输入锁到刷新率的整数分之一；
- 游戏自身出现 100 ms 级的洞时，任何插帧都补不回来（内容缺失）；
- 个别游戏的 FSR 运行时会**在销毁时 double-free**（影之刃零实测），这类标题改用 DLSS / XeSS。

### 升级提示

- `bridge.exe` / `game_capture.addon64` / `mgpu_ui.exe` **必须同版**（协议 v12，版本不符会明确提示）；
- `game_capture.ini` 新增 `StreamlineCamera`（默认 1）；
- 旧配方里的 `--mv-flip` / `--fg-mv-scale` / `--fg-depth-inv` **留着也无害**，但已不再需要。

下面是本次的详细记录（按时间顺序，含实测数据）。

### add-on：只读抓游戏自己的帧生成（`[capture] FgSniff=1`，2026-11-10）

**一句话**：新增一个**只读诊断**开关，把游戏交给它自己**帧生成**的那套参数打进 add-on 日志 ——
**投影矩阵 / `clipToPrevClip` / MV 尺度 / jitter / 深度方向 / MV 与深度的真实分辨率**。
只改 add-on，不改 bridge、不动共享内存协议、不发布任何数据。

- 三条路径按游戏实际用哪个自动分流：Streamline（`sl.interposer` 的 `slSetConstants` /
  `slSetTag*` / `slEvaluateFeature` / `slDLSSGGetState`）、原生 DLSS-G（`nvngx_dlssg`
  的 `DLSSG.*` 参数）、集成式 XeSS-FG（`xefgSwapChainTagFrameConstants` /
  `TagFrameResource` / `InitFrom*`）。
- **纯观察者**：每个钩子把原调用原样转发，参数一个字节都不改；`poll()` 恒返回 `false`
  ⇒ 永远不会被 `Input=auto` 钉住，也不会往共享块里写东西。
- **默认关**（`FgSniff=0`），需要同时 `Log=1`。日志里每个挂点都会打"装了几个 / 有几个没导出"，
  第一次调用一定打印 `#1`；只有"hooks active"却没有 `#1`，才说明这条路径这个游戏没走。
- 结构体按 Streamline 2.x 镜像（编译期 `sizeof` 断言 + 运行期 GUID 校验），对不上就打原始
  字节、不解释。`nvngx_dlssg` 的导出名是带前缀的 `DLSSG_NVSDK_NGX_D3D12_*`，两种都试。
- 用途：UE5 那条路拿不到相机（`--fg-fov` 只能猜），而这里游戏自己给的就是真值；顺带能一次
  定死"UE5 报 `MV.Scale=1.0` 到底是什么单位"（`docs/findings/FG_SNIFF.md` §4）。

`build_addon.sh` 通过 `unity.cpp` 自动带上新文件，构建 0 warning / 0 error。

### add-on：有效区裁切 + 在线 MV 单位探针（2026-11-10）

**一句话**：两个只读改动，把"每款游戏都要人猜 MV 单位/深度尺寸"变成**运行时量出来**。

1. **`crop:` 有效区裁切**。有的题目把深度/MV 纹理开得比渲染分辨率大（PBZ：深度
   **2564×1444**，渲染 **2562×1441**，SL 也只声明 2562×1441 这个子矩形）。原来的
   `ds_run` 按整张纹理归一化采样 ⇒ 场被拉伸 0.08%，越靠边越偏 1–2 像素。现在 `uvMax`
   塞进原本空着的两个 32-bit 常量槽（根签名/PSO 都不动），只在小幅超出（≤16 px）时才裁，
   超出更多就保持旧行为并打一行警告。**代价 0**。日志：`crop: depth tex 2564x1444 -> valid 2562x1441 (uvMax=…)`
2. **`probe:` 在线 MV 单位探针**。每 120 帧把 MV 流左上 64×64 拷到一个 READBACK 缓冲
   （拷贝正好在 `ds_run` 里 MV 已经是 `COPY_SOURCE` 的那一刻），用独立 fence 在 present
   里 Signal、下一帧才 Map ⇒ **完全不阻塞**，16 KB 读回。CPU 统计有效/invalid/large 比例
   与平均幅值，除以 `mvScale` 还原成游戏原样后判定 `unit=pixels|UV|ambiguous`。
   **符号**还没做（需要 prev-depth ping-pong 做 ±mv 残差），日志里明确写 `sign=unknown`
   而不是猜。

依据与后续路线（A-v2 符号 / 质量分 / profile 缓存 / 协议 v11 真投影）：
`docs/findings/INPUT_SEMANTICS.md`。

### add-on：输入语义归一化 + 三个源的声明映射（2026-11-10）

**一句话**：把"每个游戏都要人猜"的那部分，从散装 flag 变成我们自己的一套**归一化字段**，
三个源各自把 API 的声明翻进来，翻不出来的留 `unknown` 交给探针。

- `inputs.h`：`FrameInputs` 加 `MvUnit{mv pixels/uv/ndc}`、`MvSign{unknown/prev-cur/cur-prev}`、
  `CameraSource{none/declared/matrix}`、`invalid_marker`、`valid_w/h`（**不进共享协议**，
  本轮只用于日志 + 交叉验证）。
- 映射（纯查表）：
  - **NGX**：`MV.Scale > 1.5` ⇒ `uv`（"乘 2560 得像素"就是声明），否则 `unknown`（PBZ 报 1.0）；
    相机 `none`（NGX 不导出相机）。
  - **XeSS**：`USE_NDC_VELOCITY ? ndc : uv` —— 三家唯一明说速度空间的 API。
  - **FSR 2.2/3.x**：`uv` + `camera=declared`（它导出真实 near/far/vfov）。
  - 全部：`mv_sign=unknown` —— 没有任何 API 声明方向，等 ±mv 残差那一版。
- 日志：`semantics: mv=uv sign=unknown camera=declared depth=reversed valid=2562x1441`
  （每变化一次一行），以及 `probe:` 里新增 `declared=…` 与
  `(agrees|DISAGREES with the declaration)` —— 声明与实测第一次有了对照。

### add-on：A-v2 符号测量（±mv 深度残差，2026-11-10）

**一句话**：`mv_sign` 从 `unknown` 变成**量出来**。探针每轮连拍两帧（gen0 = 上一帧、gen1 = 当前帧），
MV 与深度在**同样 4 个位置**各取 128×128（4 块覆盖上中/下中/左中/右中），CPU 上把上一帧深度沿
`+mv` / `−mv` 重采样，比较与当前帧深度的相对残差：

- `+mv` 对齐更好 ⇒ `prev-cur`（XeFG/FSR 要的那种）；
- `−mv` 更好 ⇒ `cur-prev`（游戏向量反了，等于自动判出 `--mv-flip` 的必要性）；
- 残差比值 ≥0.85 ⇒ `unknown`（画面太静，不能下结论），并把 `quality=1−ratio` 打出来 —— 这是
  A-v3 质量分要用的那个数。

日志：`probe: sign used=… plus=… minus=… ratio=… -> prev-cur (+mv aligns)  quality=0.59`

代价：每 2 s 多 8 次 64 KB 的 `CopyTextureRegion` + 一次 Map（同一个 fence，不阻塞），
`kProbeDim` 从 64 提到 128 给快速平移留出余量。
**前提**：要有深度（PBZ 的 XeSS 段没有 ⇒ 不会有这行）、画面在动、两帧都被拷到。

### 实验结论：k×vblank 批次预算更差，倍率黏性也不做（2026-11-10 07:55）

对动态逐帧做了最后两个实验，结论是**保持现状**，把非整数倍率的残留起伏交给面板 VRR。

**① `--cad-batch-rt`（批次预算 = k×vblank + 用游戏端 present 间隔当输入节奏）：实测更差。**

```
out: presents=128 vblanks=128   ✓
out: presents=140 vblanks=154   ← 14 个空刷新周期
out: presents=135 vblanks=156   ← 21 个（14%）
```

原因：输入 **29.15 ms = 3.50 个 vblank**（最坏的分数比例，正好一半）。k=3 时预算 25.0 ms 比实际间隔
短 4.15 ms ⇒ 空洞；k=4 时多 4.15 ms ⇒ 挤压。"每帧恒 8.33 ms"和"不留空洞"在**固定刷新屏 + 半 vblank
比例**下不能同时成立。用间隔喂（现状）虽然间距略有起伏，但预算与间隔一致、**没有空洞**，是更好的取舍。
⇒ 开关保留但默认关闭，不设成默认。

**② `--cadence-hold`（倍率黏性）：不做。** 数据显示倍率每窗口切换 25~30 次（约 40 个真实帧），
确实偏多；用户决定不再调，交给面板 VRR 吸收。

**最终形态**：动态逐帧 = 均值定基调 + 相位累加（Bresenham）+ 突变/掉帧补偿 + 有效区约束；
非整数倍率的残留交给 **VRR（显示器侧开启即可，不需要 `--vrr`）**，或锁帧到刷新率的整数分之一。


### 详细：WGC 节流（MinUpdateInterval）实测与取舍

`GraphicsCaptureSession` 的 `MinUpdateInterval` 默认是一个合成器周期（≈16.6 ms），**WGC 因此
最多每 16.6 ms 交付一帧 ⇒ 捕获被钉在 ~60 fps**，游戏跑得再快也只能拿到 60。

**实测**：设为 0 之后（PBZ）"操作瞬间流畅跟手了很多"；随后对比 `--no-fg` 与 2x 两种配方，
用户确认"体验明显变好"。回头看，这一天里所有捕获率测量都卡在 59.6 fps、`--no-fg` 直通也顿、
各种 cadence 调整都"治不好"——根因就在这里：**我们一直在用被削到 60 fps 的输入做分析**。

改动：

- **bridge 默认 `--wgc-minint 0`（不节流）**；`--wgc-minint 16` 恢复旧的 ~60 fps 上限（仅命令行）；
- **UI 不提供该选项**（默认即正确值）；
- **UI「模式」新增「关闭 (1:1 直通)」**（`--no-fg`）：节流解除后输入常到 70~110 fps，
  此时 XeFG 最少 2x 只会让面板显示不了的帧变成重复帧 —— 直通最跟手；
- bridge 检测到"输入 ≥ 面板一半"时会**提示一次**并建议改用直通模式；
- 启动日志固定打印 `[bridge] WGC MinUpdateInterval = 0 ms (accepted)`。


**UI 里的 VRR 开关已撤销。** 一整天的实测结论是：自研路径（填满 vblank 的动态逐帧）整体优于
VRR 均匀间隔 —— VRR 没有解决卡顿，因为瓶颈不在 pacing，而在输入帧率自身的抖动
（见下一节的直通对照）。`--vrr` 参数保留在 `bridge.exe` 上仅供实验，不再出现在 UI 里；
同一天从 UI 撤下的还有"抗丢帧 (A/B)"（对插帧是负优化）。


用 `--no-fg`（1:1 直通）做对照，日志给出了明确答案：

| 指标 | 实测 | 预算 |
|---|---|---|
| `age`（游戏出图 → 我们拿到） | **2.2 ms** | — |
| `present`（等 vblank） | **1~2 ms**（max 16） | 8.3 ms |
| `total`（每帧总耗时） | **2.5 ms** | 8.3 ms |

**我们的捕获/上屏链路是干净的**；同一次会话里游戏自己的节奏是
`avg p50 = 52.7 fps / recent p50 = 55.7 / 标准差 8.3`（还出现过 183 ms 的洞）。

结论（数学必然，不是实现缺陷）：

- 53 fps = 每帧 18.8 ms = **2.26 个刷新周期**；
- 固定 120 Hz 屏上，不插帧 → 每帧占 2/3 周期交替 = 顿；
- 插帧填满 → 每输入要 2.26 帧，只能 2/3 交替 ⇒ **运动速度在 ±33% 之间跳**，仍是顿；
- **非整数倍率 + 固定刷新率无解**。DLSS-G 的对策同样是"锁基础帧率"或"VRR"，它没有算法上的解。

因此本项目的正确用法：

| 场景 | 用法 |
|---|---|
| 面板支持 VRR | 开 UI 的 **VRR 均匀间隔**：k 固定、间隔恒定（例 53 fps → 2 帧/输入 → 106 Hz，每帧 9.4 ms） |
| 固定刷新率 | **RTSS 锁到刷新率的整数分之一**（120 Hz → 60 或 40），倍率才是整数 2x/3x |
| 输入本身在抖/有大洞 | 无解：插帧不能补出不存在的时间，只能降画质让输入先稳住 |

后续不再在这条线上继续调 cadence —— 数据表明路径耗时 2.5 ms、`presents ≈ expected`，
缺口全部来自源端节奏。


**症状**：动态逐帧开了以后"看着不流畅、偶尔像少了几帧跳过去"，锁不锁帧都一样。
日志给出了**可量化**的原因，一共三层：

1. **两个估计器互相矛盾**：累加器用"最后一帧 + 8 帧 EMA"，日志却打 16 样本中位数，
   于是出现 `input=25.0ms = 3.00 vblanks -> avg 5.00x, 200.0 fps`（3 个 vblank 发 5 帧）。
2. **倍率没有"经过多少 vblank"的上限**，还会被单帧尖峰分支（`rt=41.7ms = 5 vblanks`）
   喂一个和批次不匹配的时长 ⇒ 3 帧铺 5 个周期 ⇒ 一半周期两帧、一半空着。
3. 修完上面两条后剩下 **12% 的空刷新周期**（`out: presents=120 vblanks=135`），
   根因是**右偏分布下中位数低估速率**：中位数永远 25.0ms，而真实均值 28.1ms（3.375 vblanks）。

**改动**：

- 累加器与日志统一用**同一个估计器**；倍率硬上限 = `ceil(经过的 vblank)`；相位夹到 ±0.5
  （Bresenham），倍率只能一档一档变；去掉叠加 EMA（中位数外面再套 EMA 会多 0.5~0.8 s 滞后）。
- cadence 模式下 `frameRenderTime` 直接喂"决定批次的同一个间隔"，并**停用单帧尖峰分支**
  （波动已被累加器吸收，那个分支在持续抖动下会每帧触发）。
- `t_in` 从中位数改为**均值**（16 样本环，单样本上限 3× 中位数）。
  → `out: presents` 与 `vblanks` 从此对得上（122/120、135/134、141/140、129/129）。
- 新增 `--vrr` + UI 开关 **VRR 均匀间隔**：面板跟随（G-Sync/FreeSync）时不再追求填满每个
  vblank，而是固定 k 帧、间隔均匀（例：28.6 ms × 3 帧 = 105 Hz，每帧恒 9.5 ms），
  消掉"周期满但速度抖"的 3/4 交替。开了它就不再发 `--fg-out-hz`（两者互斥）。
- 修掉一个 UI bug：`运动矢量` 选项的读取范围检查写死 `>2 归零`，导致选了"AMD 光流"下次启动
  变成"关闭"。
- 新增两个默认规则：`*-Win64-Shipping.exe`（UE）**自动改用光流 MV**；MV 不可用（协议第 19 位）
  时自动回退光流（`--no-of-fallback` 可关）。


**症状**：明末（Wuchang: Fallen Feathers）装上 add-on 后**启动即崩**，UE 报
`EXCEPTION_ACCESS_VIOLATION reading address 0x0`。UE 自己的 minidump 栈：

```
msvcp140!mtx_do_lock+0x74
game_capture+0x2964      ← 我们
game_capture+0xab2c
dxgi!… → amd_fidelityfx_dx12!ffxQuery → Project_Plague（**RHI 渲染线程**）
```

**根因**：ReShade 会**反复加载/卸载** add-on（PBZ 一次启动装 5 次可证）。`std::mutex` 有析构函数，
析构后内部指针变成 `nullptr`，下一次 `lock()` 就是读地址 0。而我们的 `LoadLibraryW` 钩子在
`AddonUninit` 之后仍可能被游戏触发 → 走进 `drain_pending_modules → on_module → lock(g_hook_mutex)`。

**修法**：

1. `g_hook_mutex` / `g_log_mutex`：`std::mutex` → **`SRWLOCK`**（`SRWLOCK_INIT`，POD、无析构）；
2. 新增 `g_addon_alive`：`AddonInit` 末尾置位、`AddonUninit` **第一行**清零，
   `notify_sources()` / `drain_pending_modules()` 先检查 —— 迟到的回调直接返回，不再碰
   `g_sources`（同样是带析构的 `std::vector`）。

顺带（同一轮，为定位这条崩溃）：loader 钩子改为**只入队**、在 `on_begin_effects` 排空
（不在 `LoadLibrary` 里装 MinHook 跳板）；初始化时的模块扫描延后到第一帧；
`sl.interposer` 的钩子收回为**仅 `FgSniff=1` 时安装**（相机默认走 XeFG frame constants）。

**验证**：明末从"启动即崩"变为**正常进入并用自己的 MV 出帧**。


最后一个"发出去没人用"的协议位。生产端在 `semantics` 里标记自己的 MV 不可用时，消费端**这一帧**
改用 bridge 自己的光流，而不是把垃圾矢量喂给 XeFG。

- 触发条件严格：`kSemQualityKnown` **且** `kSemMvUsable` 为 0 —— 即探针真的测过并判定不可用。
  没有判定（未测/未声明）时行为完全不变。
- **不丢生产端 MV**：与 `--fg-of` 不同，这条回退不会取消 `kRequestMv`
  （否则就再也切不回来了）。判定恢复可用即当帧切回。
- 每帧状态机：`g_of_active`（本帧用谁）+ `g_of_fresh`（本帧光流是否真的跑了）。
  打标签和 `motionVectorScale` 都要求 `fresh`，避免把上一帧的旧光流场当成本帧的。
- `--no-of-fallback` 关掉这条回退（等价于旧行为）。
- 日志（每次切换只打一次）：
  `[bridge] producer marked its motion vectors UNUSABLE (quality=38/255) - falling back to our optical flow until it says otherwise`

**现状说明**：因为我们改成了"声明优先"，符号测试在单位已声明时会被跳过 ⇒ `kSemQualityKnown`
只在"单位未声明、探针真的跑了"的源上出现。所以在 PBZ/DLSS 这类声明齐全的标题上，这条回退
**不会触发**（也就不会有副作用）；它服务于 XeSS-无深度/无声明那类真正需要兜底的场景。


**结果先讲**：投影矩阵通了。

```
camera: state enabled=1 src_camera=0 read=1 tick=26783937 age=31ms shape=1 raw_tick=26783937 seq=4 -> USED
camera: using the game's own projection matrix (Streamline constants / XeFG frame constants)
  rows [+0.9742 0 0 0] [0 +1.7321 0 0] [0 0 0 -1.0000] [0 0 10.0000 0]
  fovY=60.00deg aspect=1.778 - --fg-fov is no longer used for this title
[bridge] projection: from the game's own view-to-clip (v12): fovY=60.00deg aspect=1.778
```

**根因（本轮最重要的一条架构事实）**：`raw_tick=0 seq=0` 而写者明明执行了 —— 说明读写不在同一份内存。
日志里早就写着 `init_swapchain resize=0/1`，PBZ 启动时重建 swapchain，**ReShade 会重载 add-on**：

- 旧实例的 XeFG 钩子仍挂在 `libxess_fg.dll` 上 → 它调用 `publish_fg_camera`，写**旧实例的**全局；
- 新实例接管 present 路径 → 读**新实例的**全局（永远是 0）。

旁证：同一次启动里 `fg sniffer: ON` 打了 **5 次**（04:26:05–04:26:10），就是 5 次装载。

**修法**：交接改用**命名共享内存** `Local\mgpu_fg_camera_v1`（`CreateFileMappingA` 匿名页），
同会话内所有实例共享同一段字节；`seq` 用 `InterlockedIncrement64` 包住。映射失败时回落到进程内静态槽。

**这条事实的影响面不止相机**：
1. 任何"必须跨实例"的状态都不能用 `static`（我们的 `g_sem`、探针状态在重载后会重置 —— 目前无害，
   因为新实例会重新测/重新发布，但要知道它会重置）；
2. 钩子是**旧实例装的**，`MH_CreateHook` 的"已装过就跳过"守卫是**每实例一份**，
   重载后新实例会试图再钩一次（MinHook 返回 ALREADY_CREATED，我们当"已装"处理）；
3. 排查时看到"日志出现两次/五次"先想重载，别怀疑逻辑。

顺带修掉一处日志显示错误：XeFG 不传 near/far/fov（都是 0），那行曾打成 `fovY=0.00deg`，
现在从矩阵反算显示（bridge 一直是对的）。

**关于 60.00°**：这正是"用游戏自己给的"的结果 —— 它和早先 SL constants 推出的 46.7° 不一致，
按你的决定仍以游戏的为准（游戏自己的 FG 就用这个值，我们与它保持一致）。想改回测量值：
`StreamlineCamera=0` + `--fg-fov 46.7`。


用户拍板："直接默认采用游戏自身支持的方案。" 落到实现上就是两条：

1. **不再用 `--fg-fov` 质疑游戏的矩阵**。之前我在文档里提过"若 FOV 与命令行差太多就保留命令行值" ——
   这个想法被否决了。理由站得住：那份矩阵是游戏**给自己 FG 用的**，我们猜的 60° 没有资格覆盖它。
   现在的规则是"有就用，且只用它"。
2. **差异要可见，不要静默**：bridge 在两者相差 >5° 时打一行 note（只打一次），
   明确写"keeping the game's"。这样遇到"占位 60°"这类集成方，日志能一眼看出来，
   想改回命令行猜测只需 `StreamlineCamera=0`。

配套：`camera:` 相关的一次性诊断（`not used (<原因>)`、`camera source = …`）也进了这一版，
下轮跑完就能定位"矩阵为什么没被采用" —— 这是当前唯一未知项。


**实跑发现**：`03:59` 那次会话里 **没有任何 `sl.const` 行** —— PBZ 这次的 FG 是游戏自己经
**XeSS-FG** 出的（`fg: xefg.const #1 ... jit=(-0.438,+0.389)px mvs=(1.000,1.000)`），
根本没走 Streamline。于是 v12 的矩阵永远是空的，`--fg-fov` 还在顶着。

**但这正是好消息**：XeFG 的 `xefg_swapchain_frame_constant_data_t` 里**本来就有**

```c
float viewMatrix[16];        // row-major
float projectionMatrix[16];  // row-major, jitter 走单独的 jitterOffsetX/Y
```

而我们早就钩着 `xefgSwapChainTagFrameConstants`（只用来打日志）。现在同一处顺手把它交给
`publish_fg_camera()`，于是 v12 有**两条独立来源**：

| 来源 | 何时有 | 备注 |
|---|---|---|
| Streamline `sl::Constants.cameraViewToClip` | 游戏用 SL 出 FG（DLSS-G） | 官档注明无 jitter |
| **XeFG `frameConstantData.projectionMatrix`** | 游戏自己经 XeSS-FG 出 FG（unlocker） | jitter 单独传，投影本身是干净的 |

**仍然存在的空档**（如实记录）：如果游戏既不用 SL 也不开任何 FG（纯 DLSS-SR 超分），
我们**拿不到任何相机**，`--fg-fov` 继续生效。这是数据不存在，不是我们没读。

同一轮还确认了两件事（都在 v11/v12 的改动范围内）：
`probe: mv ... declared=pixels (agrees with the declaration)`（in_xess/in_ngx 的声明修正生效）、
`[bridge] producer semantics: mv=pixels(declared) sign=prev-cur(declared) depth=reversed(measured)`
（声明已能发布到消费端）。


**要解决的问题**：DLSS-SR 不导出相机，所以所有 NGX 标题的投影一直是 `--fg-fov` 猜出来的
（默认 60°），而 PBZ 实测垂直 FOV 是 **46.7°** —— 差 13°，直接体现在插帧的对齐上。
这是"探针/表"都解决不了的那一类：数据就在游戏手里，只是没人读。

**做法**（协议 v12，追加在 `FrameShared` 末尾，v11 之前的偏移不变）：

- `in_fg.cpp`（诊断用 Streamline 嗅探器）在 `slSetConstants` 里把 `sl::Constants.camera_view_to_clip`
  交给主机。官档说明该矩阵**不含 TAA jitter**，正是消费端需要的形态。
  转发发生在**任何日志门之前**（`want_line()` 会早退，而日志默认还是关的）。
- `inputs.h` / `game_capture.cpp`：新增 `FrameInputs.view_to_clip[16]`、`kInputFlagHasMatrix`，
  以及一对无锁交接函数 `publish_fg_camera()` / `fg_camera()`（序号计数器防撕裂）。
- 生产端**只在源自己没有相机时**才填（`kInputFlagHasCamera` 优先，FSR 3.1 那条路不受影响），
  并且**不往 `g_inputs.flags` 里塞持久位** —— 否则嗅探器安静下来之后矩阵会僵住。
  改成每帧判断 + 发布**采集时刻** `matrix_tick_ms`，消费端超 500 ms 就当没有。
- 合法性检查复用了 OptiScaler 的形状测试（w 是 ±z、无 w 平移、xy 不混），
  一个"被转过的相机矩阵"或垃圾缓冲都过不了。
- `bridge.cpp`：有新鲜矩阵时**从它反算 FOV/aspect**（`fovY = 2·atan(1/m[5])`、`aspect = m[5]/m[0]`），
  再走既有的 `approx_projection[_rz]`；near/far 仍取源声明。**不是**把矩阵原样交给 XeFG ——
  核实 OptiScaler 后改的主意：他们拿到同一个矩阵也只用来反算，且注明"UE 传的 `cameraViewToClip`
  不可信"（`inputs/FG/Streamline_Inputs.cpp:207`）。原样直喂留 `--fg-use-matrix` 做 A/B。
  `--fg-no-proj` 仍然最高优先。
- `[capture] StreamlineCamera=0` 可以关掉这个交接（嗅探器本身继续为日志服务）。
- **钩子门控也改了**：以前 Streamline 钩子只在 `FgSniff=1` 时装；现在"只为相机"也要装
  （`StreamlineCamera=1` 且 `FgSniff=0` 时只装 `sl.interposer` 与 `libxess_fg` 两族，
  DLSS-G 钩子仍然只服务日志）。否则默认配置下这个特性永远不会生效 —— 这是落地时差点漏掉的一环。

验证手段（下次进游戏看两行）：
```
camera: using the game's own view-to-clip matrix (Streamline constants)
  rows [...] zn=0.1000 zf=75000.0 fovY=46.70deg - --fg-fov is no longer used for this title
[bridge] projection: using the game's own view-to-clip (v12), not --fg-fov
```



**起因**：±mv 探针的判据越修越苛刻（残差求和 → 胜率 → z 分数 + 残差门 → 256 patch + 梯度门），
而它本来就不该是主判据。查同仓的 OptiScaler 源码（`temp/optiscaler_src`，同为 GPL-3.0）得到三个实证：

1. **他们的游戏表里没有我们要的东西**：`OptiScaler/misc/GameConfigs.cpp`（106 条）只调
   `DxgiSpoofing` / `FGAllowedFrameAhead` / `ColorResourceBarrier` / `RestoreComputeSignature`
   这类"管道"开关，**零条**涉及 MV 单位、MV 符号或深度方向。
2. **单位他们根本不判**：`NVSDK_NGX_Parameter_MV_Scale_X/Y` 直接抄进 FSR/XeSS 的
   `motionVectorScale`（`upscalers/fsr2_212/FSR2Feature_Dx12_212.cpp:255-266`），
   深度方向用 init flag（`DepthInverted()`）。即"信声明、不测量"。
3. **按 exe 的表对 PBZ 会是错的**：同一个 exe，FSR 会话声明 uv（`MV.Scale≈2880`，实测 uv），
   DLSS 会话声明 pixels（`MV.Scale=1.0`，实测 pixels）—— 表无法表达"随 API/档位变"。

**所以复用他们的表解决不了问题，问题是我们的读法错了。** 关键一行是
`pixels = stored × MV.Scale`（DLSS 就是这么消费的）：于是 `MV.Scale=1.0` 不是"不可知"，
而是"已经是像素"。两段 PBZ 会话 2/2 与实测一致。

**改动**：

- `in_ngx.cpp`：由声明推导单位——`scale≈1` → pixels、`≈渲染宽` → uv、`≈宽/2` → ndc；
  符号按 API 契约直接写 `MvSign::prev_minus_cur`（所有超分 API 都定义成"上一帧−当前帧"，
  游戏若反过来会弄坏它自己的 DLSS；`MvFlip=1` 保留给少数说谎的标题）。
- `probe:` 单位**不再覆盖声明**，只在声明缺失时发布；单位已声明时 **±mv 符号测试直接跳过**，
  日志一行：`probe: sign skipped (mv unit declared=pixels, sign is the API contract)`。
- `[capture] MvNormalize=auto`：把声明得到的单位变成流里的换算（pixels→÷流宽、ndc→×0.5）。
  **默认仍是 0（原样透传）** —— 它改变的是 bridge 收到的数据，必须是显式动作，不能静默变。

净效果：PBZ 这类"声明齐全"的标题，脆弱路径**完全不参与决策**；探针只在 API 什么都不说时兜底。

### 顺带修掉两个从 OptiScaler 源码里发现的缺口

对着他们的 `inputs/FG/XeFG_Inputs_Dx12.cpp:514` 逐行核对，确认 XeFG 的口径是
`motionVectorScale = 每个 velocity 单位等于多少像素`，且 **NDC 时 Y 要取负**
（`mvScaleY *= size.y * -0.5f`）。于是：

- **发布的 `mv_scale` 现在按流里的实际单位折算**。之前它只是转发游戏声明；一旦开了
  `MvNormalize`（流改成 UV），XeFG 拿到的速度会差 ~2560 倍 —— 画质静默劣化，最难查的那种。
- **`mv_unit == ndc` 时 `mv_scale_y` 取负**（原来只乘 2）。bridge 侧的
  `XEFG_SWAPCHAIN_INIT_FLAG_USE_NDC_VELOCITY` 还没接，已列进
  `docs/findings/OPTISCALER_QUIRKS.md` §5 的第一条。

对照分析全文（105 条表怎么分类、哪 8 条能搬、我们缺什么）见 `docs/findings/OPTISCALER_QUIRKS.md`。

### XeSS 实跑抓到的四个问题（2026-11-10 03:49，已部署）

`03:45` 那次 XeSS 会话暴露了几件只有真跑才会出现的事：

```
xess: first execute render=2562x1441 output=3840x2160 depth=0000000000000000 mv=… reset=1
       (HIGH_RES_MV: no depth, publishing MV only)
semantics: mv=uv sign=unknown camera=none depth=forward valid=2562x1441      ← 两个字段都是错的
crop: mv tex 3840x2160 vs reported 2562x1441 - excess too large, sampling the whole texture
probe: mv mean|v|=(44.848,3.113) -> unit=pixels  declared=uv  (DISAGREES with the declaration)
mvscale=(1.000000, 1.000000)                                                  ← 同一条日志里
```

1. **`in_xess` 把单位判错了**：`USE_NDC_VELOCITY` 没置位时它声明 `uv`，但 XeSS 的默认速度空间是
   **像素**。证据就在同一份日志里：`mean|v|=44.8` + `mvscale=(1,1)` —— 44 不可能是 UV。
   已改成 `ndc : pixels`。这是"声明优先"之后的第一个真实收益：探针当场指出了声明的错，改一行即可。
2. **`sign=unknown` 是自找的**：`in_xess`/`in_ffx` 还写着 `unknown`，只有 `in_ngx` 声明了契约符号。
   三个源现在统一声明 `prev_minus_cur`，并且**声明也会发布到 v11 的 semantics 字里**
   （以前只有探针能写那两位，所以"声明齐全"的源在 bridge 侧反而显示 unknown）。
3. **`depth=forward` 是假话**：XeSS 这条路径**根本没有深度纹理**，日志却按"没有 InvertedDepth 标志
   = forward"打印。现在打印 `depth=n/a (no depth)`。
4. **`crop:` 的措辞误导**：MV 3840×2160 vs 渲染 2562×1441 时我们**并不是放弃**，而是 `uvMax=1.0`
   把整张贴图重采样到渲染网格 —— 这正是 OptiScaler `ForceFGRenderSizeMVs` 想要的"FG 要渲染尺寸的 MV"。
   所以 `OPTISCALER_QUIRKS.md` §3.2 里"待做 MvResample"这一项**其实早就实现了**，只是日志写着
   "excess too large"，看起来像失败。已改为 `resampling the whole texture`，并把该行动项标记为"无需改动"。


| # | 改动 | 位置 | 为什么 |
|---|---|---|---|
| 1 | `mv_unit == ndc` → 置 `kInputFlagMvNdc`；bridge 据此设 `XEFG_SWAPCHAIN_INIT_FLAG_USE_NDC_VELOCITY` | `game_capture.cpp`、`bridge.cpp` | 不声明的话 XeFG 把 [-1,1] 的 NDC 速度当 UV 读（XeSS 的 `USE_NDC_VELOCITY` 正是这种源） |
| 4 | `offload_same_device()`：队列/资源不同 device 时回落到游戏队列并记日志 | `game_capture.cpp` | 双 GPU 进程里混 device 是设备移除，崩在离拷贝很远的地方（OptiScaler 的 `CreateSLOnThe2ndDevice` 同类坑） |
| 5 | `sl.tags` 日志打出 lifecycle 名字，`eValidUntilEvaluate` 的 depth/MV 标注 `[staged for Evaluate - not read as input]` | `in_fg.cpp` | `lc=2` 这种数字看不出"SL 还可能改写它"的意思 |

构建：add-on 0 警告、bridge 0 警告，两者都已覆盖到 `F:\...\PBZ\platform\steam\`。


**1. 三次崩溃的归因：不是我们。** 用 `cdb` 分析 `PBZ.exe.25748.dmp`（切档位那次）：

```
FAILURE_BUCKET_ID: HEAP_CORRUPTION_ACTIONABLE_BlockNotBusy_DOUBLE_FREE_c0000374_dxgi.dll!Unknown
  … ntdll!RtlFreeHeapFast ← dxgi ← D3D12Core!CDestructionNotifier::Destroy
  ← D3D12Core!CLayeredObject<CBackingCommandAllocator>::Release ← dxgi
  ← amd_fidelityfx_upscaler_dx12+0x279a ← …_loader_dx12 ← PBZ
```

游戏自带的 FSR 运行时在**销毁路径**里把同一块 D3D12 command allocator 释放了两次；
栈里没有 `game_capture` / `bridge` / `ReShade`。PBZ 里**改用 DLSS / XeSS 后即可正常进入游戏**。
细节与证据：`docs/findings/PBZ_NOTES.md`。

**2. `probe: sign` 的第二次修正：多尺度搜索。** 上一版把流值当像素偏移
（`× kProbeDim / render`），对 UV 源方向是反的 ⇒ 位移≈0 ⇒ `plus`/`minus` 几乎相同。
现在对三种候选尺度（pixels / uv / ndc）各算一遍 ±mv 的深度残差，取**对齐最好**的那个：

```
probe: sign scale=uv used=… static=… plus=… minus=… ratio=0.43 -> prev-cur (+mv aligns)  quality=0.57
```

胜出的尺度同时就是**单位的第二份独立证据**（`kSemMvUnit*` 会被它覆盖），
`ratio` 决定符号与质量分。日志新增 `scale=` 字段。

**3. 模块表饱和的修复（03:01 已部署）**：`in_fg.cpp` 的模块登记表 4→8 槽 + 新增"已忽略"表，
并给每个入口加了"已装过就跳过"的守卫 —— 复现证据是同一会话里 `fg sniffer:` 从 48 次降到 9 次。

**4. 符号判定改成"配对胜率"（A-v3，03:19 已部署）**：残差求和会被少数去遮挡/远平面点主导 ——
PBZ 实测 `plus=431.7979 minus=539.9221`（22,206 个采样）其实是 **20%** 的差，但除以样本数只剩
1.9%，于是被 0.85 的阈值判成 `ambiguous (need motion)`，白扔了一轮数据。
现在逐纹素投票："±mv 哪边把上一帧深度对齐得更好"，`quality = 2×|胜率−0.5|`：

```
probe: sign scale=pixels used=22206 static=3150 wins=68.6% mean|d| plus=0.0195 minus=0.0243 -> prev-cur (+mv aligns)  quality=0.37
```

配套的三条纪律：小于 1/4 纹素的运动（两侧落在同一批纹素里，只有插值噪声）单独计入 `static=`；
两个极点（反向 Z 天空=0 / 正向 Z 天空=1）都不参与投票；`margin > 3%` 才下结论（22k 票上已不可能是噪声）。

**5. 胜率版第一次实跑就暴露了选尺度的缺陷（03:23 修复，待部署）**：03:21 那行是

```
probe: sign scale=uv used=446 static=1 wins=40.4% mean|d| plus=0.00323 minus=0.00322 -> cur-prev  quality=0.19
```

`plus` 与 `minus` 只差 **0.3%**（等于没信号），却因为小样本（446 票）的胜率更极端，**把 22,206 票的
`pixels` 挤掉了** —— 排序按 `|胜率−0.5|` 没考虑样本量。修法两条：

- 排序改用**二项 z 分数** `(wins − n/2)/√(n/4)`，并要求 `used ≥ 2000`；
- 再加一道**残差差门**：胜出方向的 `mean|d|` 至少要差 2%，否则写
  `unknown (votes lean one way but both offsets fit equally well)` —— 03:21 那种 0.3% 会被直接判掉。

日志因此多了 `z=` 和 `big=`（被 16 纹素上限甩掉的 texel 数，能一眼看出哪个尺度把偏移放大了）。

**6. 03:26 又跑出两行，暴露 patch 尺寸不够（03:29 修）**：

```
probe: sign scale=ndc used=16565 static=502 big=44432 wins=65.7% z=40.3 plus=0.00137 minus=0.00137 -> unknown (votes lean one way but both offsets fit equally well)
probe: sign scale=uv  used=14497 static=842 big=46222 wins=77.3% z=65.6 plus=0.00214 minus=0.00222 -> prev-cur
```

`big=46,222 / 63,504` —— **73% 的 texel 位移超过 16 纹素上限**。这次 `probe: mv` 报的是
`mean|v|=(33~56, …)`，正常转视角的运动量就是 33~56 像素，128 的 patch + 16 上限
把**正确尺度**饿死了，投票全来自近乎静止的天空/UI（所以 `mean|d|` 只有 0.2%）。

四处修改：

- `kProbeDim` **128 → 256**，上限改为 `kProbeDim/4 = 64`（缓冲区 4 MB，每 2 s 拷 4 MB）；
- warp 采样点必须都落在 patch 内，否则计入 `out=`（以前靠 clamp，边缘 texel 是错的）；
- 新增**深度梯度门**：`slope_per_texel × |offset| < 2% × depth` 的 texel 不投票，计入 `flat=`。
  平坦墙面上 ±mv 采样结果完全相同，投票只是舍入噪声 —— 03:21 的
  `plus=0.00323 minus=0.00322` 就是这么来的；
- 新日志字段：`used= static= flat= big= out=`，一眼看出票是从哪一步漏掉的；
- 计算量反而降了：**循环步长 2**（场是空间相关的，隔一个 texel 结论一样），梯度用
  `dp_at(x±2)` 代替 4 次双线性。256² × 4 spots × 3 尺度全双线性会在每 2 s 的 present 回调里
  造成可见抖动，现在相当于 128 版的 ~1.5 倍。

好消息是**残差门当场生效**了：03:26 第一行 votes 65.7%、z=40.3，但两侧 `mean|d|` 一模一样
（`0.00137 / 0.00137`），被判成 `unknown (votes lean one way but both offsets fit equally well)` ——
换上一版这里就会错误地宣布 `cur-prev`。

**一句话**：探针量出来的东西不再只躺在 add-on 日志里，而是**随每帧一起发布**给消费端。

- `mgpu_shared.h`：`kSharedVersion` 10 → **11**；`FrameShared` 末尾追加
  `semantics`（打包好的 `kSem*` 位）+ `valid_w/h`（有效子矩形）。**只追加**，v10 及以前的偏移不变。
- add-on：`probe_report()` 把三项结论写进 `g_sem`（MV 单位 / MV 方向 / 深度方向 + 质量分
  `1−残差比` + `MV usable` 位），发布时写进每一帧。
- bridge：新增一行（**只记录、不改行为**，开关仍然优先）：
  ```
  [bridge] producer semantics: mv=pixels(measured) sign=prev-cur(measured) depth=reversed(measured)
           quality=148/255  [MV usable] valid=2562x1441
  ```
- 规矩照旧（`docs/design/PROTOCOL.md` §6）：两端必须同一次构建，版本不符硬拒绝。
  已同时部署新 `bridge.exe`（旧版备份 `bridge.exe.before_v11`）。

## v0.13.2 —— 并行化复审：所有"还能并行"的地方都试过了，结论是默认全关（2026-11-09）

**一句话**：把 fg 链路上还能并行/异步的地方全部实现并实测了一遍 —— 3 个默认关的新开关
（跨卡拷贝走 COPY 队列、MV/深度提前一帧、add-on 把跨卡写搬出游戏队列）、1 个抗丢帧模式，
外加 3 个真修复。**新开关全部默认关闭，默认行为与本版之前完全一致**；每个开关的实测数据、
为什么留/为什么不用，都在 `docs/findings/PERF_BASELINE.md` §3.28–§3.30。

### 修掉的问题（默认生效）

1. **`--fg-warp` 两条 list 共用一份 command allocator。** 这条实验路径一个输入帧提交两条 list
   （插值中点 + 真帧），而中点那次 `Present()` 只是把 list 排进队列；原代码在两次提交之间
   `Reset()` 同一个 allocator —— D3D12 里对**还在执行**的 allocator 做 Reset 是未定义行为。
   现在两对 `(allocator, list)` 每帧轮换，靠下一帧开头的 `gpu_throttle()` 保证换进来的那对已退休；
   顺带修掉 `back_buffer()` 失败时第二条 list 往**已关闭**的 list 里录的问题。
2. **`WGC sees 60 fps but the game only renders 38.4 fps` 是假警报。** 它拿我们的**中位数**
   （16.7 ms）去比生产端的**一次原始采样**（26 ms）；真实情况是游戏 ~44 fps 落在 120 Hz 面板上，
   帧间隔在 2/3/4 个 vblank 之间正常跳动，两边**中位数**都只是"最常见的那个桶"。
   这条误报曾把排查引向"游戏自带 FG"（实测游戏 FG 是关的）。现在两边都取 16 帧中位数再比，
   并把 **DWM 重复合成**列为第二种可能原因（我们的顶层覆盖窗口会让游戏窗口失去独立翻转）。
3. **UI 的两个死键接上了。** `mgpu_ui.ini` 里的 `sdr` 与 `slack1` 一直存在，但
   `ID_SDR` / `ID_SLACK` **不在任何控件表里**、`build_cmdline` 也从不发对应开关 —— 等于摆设。

### 新增 UI 控件

| 位置 | 控件 | 作用 |
|---|---|---|
| 捕获 → **色彩空间** | `HDR10（默认）` / `SDR（BT.709）` | 选 SDR 发 `--sdr`：bridge 走 BT.709 + gamma 2.2，并把交换链色彩空间声明成 SDR。我们抓的是 scRGB(FP16)，HDR 源永不丢；在 HDR 显示器上选 SDR 会发灰，那是模式本身 |
| 帧生成 → **抗丢帧 (A/B)** | 开 / 关 | 发 `--slack-auto`：当排队的抓取帧确实带着**新内容**时，多留一帧而不是丢掉（代价是那一帧 +1 个间隔延迟） |

### 新增命令行开关（默认全关）

| 开关 | 作用 | 实测结论 |
|---|---|---|
| `--input-copy-queue` | MV/深度的跨卡拷贝走独立 COPY 队列，与颜色 convert draw 重叠 | `--den 2` 测不出来（重叠窗口只有 0.2–0.5 ms）；`--den 1` 单趟 A/B 有 −1…−2 ms 但体感更差（空刷新 34 vs 13）。**不用** |
| `--prefetch-inputs`（隐含上一条） | 把 slot N+1 的 MV/深度放在帧 N 的 Present 之后拷贝 | 机制 100% 命中（`pf 40/0`），但空刷新 0.32 → **2.90/窗**、15/60 窗口空 >4 个：搬运挪出帧外 → 循环跑赢 pacer → `--interp-cadence` 少要帧。**不用** |
| `[capture] OffloadQueue=1`（add-on） | 跨卡拷贝 + 我们自己 RT 的反向状态转换搬到 add-on 自己的队列 | 功能验收通过（`offload=9987`、`stalls=0`、`fail=0`、6 分钟无异常、回退链一次没触发），**收益未量到**（对照被场景漂移吃掉）。**不用**，游戏侧 add-on 已回滚 |
| `--slack-auto` | 抗丢帧：真有新内容时多留一帧 | 缺省关。实测它的 `held` 只有 3 次而丢了 20 次 ⇒ 20 次丢弃里生产端根本没发布新帧 = **重复帧**。建议**开着**（几乎零成本），但不是必须 |

### 顺手量到的事实（以后不用重测）

- **OSD 的 `drop N` 大多不是"掉帧"**：`--slack-auto` 的判据证明那些被丢弃的抓取帧里
  生产端 `frame_published` 没有变化 —— 是 DWM 把同一帧重复合成上来的，丢掉不损失画面。
  新日志把丢弃拆成 `[K real loss / M-K duplicate]`，**只有 K 是真丢内容**。
- **手工分次跑 A/B 在本机不成立**：同一配置两趟 `total` 差 **3.2 ms**、`age` 差 6 倍（7.9 vs 48.5 ms），
  比被测效应还大。必须用 `bin\12 - automatic A-B-C-A 4K (60s each).bat` 这种**自动连续**跑法。
- **我们自己没有任何硬编码帧率上限**：`--lock` / `--fg-out-hz` / `--pace` 默认全关，UI 里那行
  "建议锁帧"只是提示（不会传给 bridge）；交换链是 `Present(0)` + BufferCount 2 + MaxFrameLatency 1
  （限的是在飞帧数，不是帧率）。日志里的 `in=25 ms` 就是 RTSS 锁的 40 fps。

### 新增配方脚本

```
9  - 4K out 3x (A).bat                     4K 原生输出基线（--den 1 --interp 3）
10 - 4K out 3x + cq (B).bat                + --input-copy-queue
11 - 4K out 3x + prefetch (C).bat          + --prefetch-inputs
12 - automatic A-B-C-A 4K (60s each).bat   自动连续跑四段（本机唯一可信的 A/B 方式）
13 - smoothest + anti-drop (A-B).bat       配方 5 + --slack-auto
8  - smoothest + copy queue (A-B).bat      配方 5 + --input-copy-queue
```

### 这一版真正可用的东西

`--fg-warp` 的双 allocator 修复（真隐患）、`breakdown` 的 `cq=` 字段、
`collect_retired_inputs()` 的 swap-remove、UI 的「色彩空间」与「抗丢帧」两个控件、
`--slack-auto`、以及上面那条**假警报的修正**。其余全部默认关。

## v0.13.1 —— RE Engine / PRAGMATA 支持（2026-11-09，已实测通过）

**结论**：PRAGMATA 这条路走通了 —— `input source: ngx`、`shared ready`、bridge 正常出帧、
热键可以启动。排查过程中修掉 4 个真问题，其中两个属于"看起来在跑、其实什么都没发生"的静默失效。

### 修掉的问题

1. **NGX 入口不一定在驱动 shim 上。** RE Engine（Streamline）的 NGX 入口来自
   **游戏目录自带的 `nvngx_dlss.dll`**；旧判据只认 `_nvngx.dll` ⇒ 一个钩子都没装
   （ReShade 仍然报告 add-on 注册成功，但零数据）。现在判据 = 文件名含 `nvngx`
   ＋ **真的导出 `NVSDK_NGX_D3D12_EvaluateFeature`** ＋ 排除 `dlssg/dlssd/dlssnr`
   （CP2077 那些只读版本号的副本仍被第 3 条挡掉）。顺带：一个 provider 挂上就停，
   不再对转发到同一地址的 shim 重复挂载并打出 `NGX hooks FAILED`。
2. **采集暂停按帧算是慢加载游戏的灾难。** `init_swapchain` 之后 `+120 帧`，而 PRAGMATA
   加载期只有 **0.8 fps** ⇒ 实际暂停 **150 秒**，整个启动阶段不产出任何数据。
   现在改成「帧数或 **8 秒**，先到先算」。
3. **`LoadLibrary` 钩子拖慢 RE Engine 的加载**（用户 A/B：关掉 add-on 加载就正常）。
   新增 `[capture] LoaderHook=0` —— 完全不碰 `kernel32`，改为每 120 帧扫一次模块表。
   同时加了回调计时：实测我们的回调合计 **0.17 ms/帧**（barrier 0.02 ms/帧，约 400 次/帧）
   ⇒ "慢"确实来自钩子本身，不是回调里的活。
4. **游戏可能不从你改的目录读文件。** PRAGMATA 实际从
   `E:\PRAGMATA\_storage_\game_capture.addon64` 加载（进程级重定向），于是它找的 ini 是
   `_storage_\game_capture.ini` —— 我们改根目录那份**从来没被读过**，`Log` 一直是默认 0，
   所以"日志一行都没有"。
   - add-on：本地 ini 不存在时回退到 `%LOCALAPPDATA%\mgpu\game_capture.ini`；日志打不开时
     回退到 `%LOCALAPPDATA%\mgpu\game_capture-<pid>.log`；
   - 新增**面包屑** `%LOCALAPPDATA%\mgpu\breadcrumb.log`（每次 `AddonInit` 写一行：add-on
     的真实路径、ini 路径、`Log` 值、日志是否打开）—— 这次就是它一眼定位的；
   - 安装器新增 **6/6 镜像目录**：任何子目录里已有同名 add-on 或同名代理 DLL，就把
     add-on + ini 也放一份过去（`_storage_` 这类容器从此自动覆盖）。

### 实测（PRAGMATA）

```
config: ... SkipMv=0 DepthConvert=1 ... LoaderHook=0 Input=auto (path e:\pragmata\_storage_\game_capture.ini)
NGX provider: ...\_nvngx.dll   ->   NGX hooks active
dlss feature flags 0xB: MVJittered=0 MVLowRes=1 DepthInverted=1
input source: ngx (api 1) - pinned for this process
shared ready: name=Local\mgpu_mailbox_v1 pid=23056 used=253.6 MB reserved=253.6 MB slots=4
perf: frames=10680  begin=20ms  barrier=2.4ms (41k calls / 241k items)  present=1.4ms
```

排查全过程（含"GPU 0% 不是掉驱动"、"热键其实收到了"两次误判的证伪）见
`docs/findings/RE_ENGINE_NOTES.md` §3.1、§6.1–§6.4。
- 全部通过后：重建发布为 **v0.13.1**（0.13.0 的 add-on 不含以上修复）。

---

## v0.13.0 —— UE5「模型运动伪影」结案（2026-11-09）

**一句话**：伪影的主因是**上采样倍率**，不是运动矢量。游戏内 **DLAA（或最高画质档）+ `--interp 1 --fg-of`**
是本次唯一实测"干净"的组合；UE5 交给 NGX 的那条 MV 流是坏数据，不要用。
（完整证据链、排除表、dump 数字：`docs/findings/UE5_MV_OPTISCALER.md` §7–§8。）

**用户能感知的变化**

- **新配方**：`--interp 1 --fg-of` + 游戏内 DLAA（4K 下 26→52 fps，实测无模型边缘伪影）。
  DLSS 性能档（720p→4K，3 倍放大）**连不插帧的游戏本身**都有一点伪影，那部分不该算在插帧头上。
- **`--fg-of` 真的能用了**：以前 `motionVectorScale` 透传游戏的 `MV.Scale`（PBZ 报 1.0），
  库把 screen-UV 的 0.009 当成 0.009 像素 ⇒ 光流等价于没生效。现在用 MV 纹理尺寸
  （日志 `mvscale=(1278.0000, 720.0000)`）。**这条影响所有用 `--fg-of` 的配方（含 3x/4x 推荐档），
  `MV.Scale=1.0` 的游戏以前等于没开光流。**
- **add-on 开始读游戏声明的 DLSS flags** 并转发 `MVJittered` / `MVLowRes` / **`DepthInverted`（新增）**：
  日志 `dlss feature flags 0x4B: MVJittered=0 MVLowRes=1 DepthInverted=1`（需 `game_capture.ini` 的 `Log=1`）。
  PBZ 声明 `MVJittered=0`，所以"UE5 的 MV 带抖动、开 `JITTERED_MV` 就能修"这条**被证伪**。
- 新开关：`--fg-mv-scale raw|px|<float>`、`--fg-no-jittered-mv`；
  `--fg-jitter-zero` 重新生效（我们的颜色是上采样后的画面，jitter 已解掉，这是它的 A/B 对照）。
- `scripts/analyse_dump.py` 新增 **units fit**（拟合 `motion_px = mv_raw × k` 判 MV 单位与配对）；
  新配方脚本 `scripts/recipes/run_ue5_mv_ab.bat`（`auto/jit/nojit/depthinv/jitdepth/mvscale/nomv/of/ofsign/offov/jz/nodepth/only/dump`）。

**实测（PBZ，2x @ 60 锁帧，`cadence` 精确）**

| 档 | 结果 |
|---|---|
| 游戏 MV（`auto`）/ `jit` / `depthinv` | 伪影 + **闪烁** |
| `--no-mv` | 不闪，仍有边角伪影 |
| `--fg-of`（scale 修好前 / 后） | 与 `--no-mv` 相同 / 仍相同 ⇒ 伪影与 MV 无关 |
| **DLAA + `--fg-of`** | **干净** |

dump 出来的游戏 MV：0.5% NaN/inf、17% 饱和到 fp16 上限（±65504）、x 中位 0 / y 中位 +31、
与真实位移相关性 ≈0 —— 不是可用的速度场（与 2026-11-07 对 PBZ 的旧结论一致）。

---

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
