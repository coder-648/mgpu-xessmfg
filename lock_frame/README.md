# lock_frame（双显卡插帧工具）

**双显卡插帧工具。** 游戏在自己的显卡上渲染，`lock_frame` 在另一块显卡上用
XeSS-FG 插帧，再叠加回游戏窗口。支持固定 **2x / 3x / 4x**。

本包**自带 Intel 运行时**（`libxess_fg.dll` / `libxell.dll`），解压即用。
安装时这两个 DLL 会被写入 `<游戏>\fakerFG\`，供游戏进程里的 XeFG 使用
（1.2.1 修复：此前只放在工具目录，游戏会加载它自带的旧版运行时）。

---

## 一、解压与启动

1. 把压缩包解压到任意可写目录，例如 `D:\lock_frame`。
   **不要只解压到游戏目录里。**
2. 双击 `lock_frame_ui.exe`。
3. 第一次启动会出现欢迎页，点「知道了，开始」。

需要：Windows 10 1903+ / Windows 11、D3D12、游戏支持 DLSS 帧生成、无边框/窗口化全屏。
仅限单机/离线游戏，**不要用于带反作弊的在线游戏**。

## 二、五步上手

1. 「当前游戏」卡片右上角 → **选择游戏** → **添加 exe…**
   选中游戏的主程序（如 `Cyberpunk2077.exe`），然后在列表里点它一下。
2. 回到主界面 → **安装 / 应用配置**
   （会**覆盖**游戏目录里的 `fakerFG.dll` / `dxgi.dll` / `nvngx_dlssg.dll` /
   `xess_mfg_producer.dll`，并按所选版本重写 `fakerFG.ini`；原 `dxgi.dll` /
   `nvngx_dlssg.dll` / `fakerFG.ini` 会先备份，可还原。`version.dll`（dlssg_sm86）
   会被改名为 `version.dll.bak_mgpu`，避免抢 `nvngx_dlssg.dll`）。
3. 启动游戏，进入实际画面，开启 **DLSS Frame Generation**（无边框/窗口化全屏）。
4. 回到工具点 **刷新**；状态变成「就绪」（窗口 + producer 已就绪）。
5. 选倍率（120Hz 面板：2x 锁 60 / 3x 锁 40 / 4x 锁 30 fps），点 **启动补帧**。

想细调（GPU、色彩、帧池、热键、OSD…）点同一行的 **高级设置**，改的是**当前游戏**的配置。
随时可以点 **还原 / 移除组件** 撤销安装。

## 三、文件与配置

| 文件 | 作用 |
|---|---|
| `lock_frame_ui.exe` | 图形界面（本工具） |
| `lock_frame.exe` | 帧生成消费端（由 UI 调用） |
| `fakerFG.dll` / `xess_mfg_producer.dll` | 游戏侧组件（安装时按需复制） |
| `fakerFG.ini` | 游戏侧配置模板（发布精简版，部署时放入） |
| `fakerFG.full.ini` | 游戏侧配置模板（测试全参数版，「配置版本 → 完整」时写入） |
| `libxess_fg.dll` / `libxell.dll` | Intel XeSS 运行时（2x 只需前者，3x/4x 需要两者） |
| `lock_frame_ui.ini` | 索引 + 全局 UI 偏好（语言/缩放/最近游戏） |
| `profiles\<游戏名>.ini` | **每个游戏一份完整配置**（倍率/输入源/MV/GPU/OSD…） |
| `lock_frame_ui.log` / `lock_frame_ui_HHMMSS.txt` | 日志（保留最近 8 份运行日志） |
| `collect_diagnostics.bat` | 生成诊断包（本地，不上传） |

游戏目录里还会有：`fakerFG.ini`（游戏侧配置）与 `fakerFG.ini.bak_<时间>`（最近 8 份备份）。

## 四、常见问题

- **「启动补帧」是灰的**：看按钮上方提示——没选游戏 / 没安装 / 游戏没跑 / producer 未加载。
- **提示「游戏目录 fakerFG 里缺少 libxess_fg.dll」**：点一次 **重新应用配置**，
  它会把本包的 Intel 运行时写进 `<游戏>\fakerFG\`；然后重启游戏。
- **提示"请先关闭游戏"**：安装/还原会替换 `dxgi.dll`，必须完全退出游戏（含托盘）。
- **3x/4x 实际只有 2x**：确认 `libxell.dll` 在工具目录；部分游戏只支持 2x。
- **画面撕裂/菜单点不动**：高级设置里检查 `分层窗口 / 输入转发 / OSD`，或先试 2x。
- **改了配置没生效**：游戏侧 `fakerFG.ini` 每次进游戏只读一次，改完要重启游戏。
- **想恢复原样**：点「还原 / 移除组件」（会恢复游戏原来的 `dxgi.dll` / `nvngx_dlssg.dll` /
  `version.dll` 并删除组件；`fakerFG.ini` 保留）。
- **UI 合成（HUDless）**：**实验性，默认关闭**。开启后插值帧的 HUD 不残影，但边缘/半透明
  UI 仍可能出现拖影或伪影，只在 A/B 时打开（改完需重启游戏）。
- **反馈问题**：运行 `collect_diagnostics.bat`，把 zip 和日志一起提供。

更多排障见 `docs/TROUBLESHOOTING.md`；玩法与原理见 `docs/SUPPORTED-GAMES.md` 与本仓库文档。

## 五、卸载

- 只卸载某个游戏：选中它 →「还原 / 移除组件」；
- 完全卸载：删掉整个工具目录即可（不写注册表、不写 AppData）；
- 游戏目录里如需手工清理：删除 `dxgi.dll`、`fakerFG.dll`、`nvngx_dlssg.dll`、
  `xess_mfg_producer.dll`，删除 `fakerFG\` 子目录（安装时写入的 Intel 运行时），
  并把 `dxgi.dll.bak` / `nvngx_dlssg_real.dll` 改回原名。

## 六、许可

- 本体：GPL-3.0（见 `LICENSE` / `NOTICE`），对应源码见同版本 `-src.zip`。
- 随包 Intel XeSS 运行时按 Intel 许可原样再分发（见 `INTEL-XESS-LICENSE.txt`），未修改。
