# 部署包（ReShade add-on）：`dist/mgpu_reshade_pack.zip`

给"任意一款 64 位游戏"装 mgpu 需要的那一半，**拖一下就装、拖一下就卸**，不需要 WSL、
不需要先跑一次游戏、不需要手改 ini。

```bash
./scripts/make_addon_package.sh          # 生成 dist/mgpu_reshade_pack/ + .zip
SKIP_TOOLS=1 ./scripts/make_addon_package.sh   # 只要 add-on，不带 bridge/UI
RESHADE_SRC=/path/ReShade64.dll ./scripts/make_addon_package.sh
```

## 包里有什么

| 路径 | 作用 | 装进游戏目录后 |
|---|---|---|
| `install.bat` / `install.ps1` | 安装器（拖 exe 或传目录） | 不进游戏目录 |
| `uninstall.bat` / `uninstall.ps1` | 卸载器（全部还原） | 不进游戏目录 |
| `README.txt` | 中文说明 | 不进游戏目录 |
| `files/ReShade64.dll` | ReShade 6.8（add-on 的 ABI 宿主） | → `dxgi.dll`（可换 `d3d11/d3d12/opengl32/version`） |
| `files/game_capture.addon64` | 抓取端（共享内存 producer） | → 同名 |
| `files/game_capture.ini` | `Color=0 / StreamPercent=100 / SkipMv=1 / Input=auto` | → 同名（add-on 读自己旁边的 ini） |
| `files/reshade-shaders/Shaders/mgpu_depth_tap.fx` | 空转效果，只为绑深度 | → `reshade-shaders\Shaders\` |
| `files/ReShadePreset.ini` | 启用 `MGPU_DepthTap` | → 同名（已有的先备份 `.mgpu_bak`） |
| `files/ReShade.ini` | `EffectSearchPaths` / `PresetPath` | 有则备份后补这两个键 |
| `tools/` | `mgpu_ui.exe` `bridge.exe` `libxess_fg.dll` `libxell.dll` | 留在包里直接跑 |
| `md5.txt` | 全部文件的 md5 |  |

## 在 Windows 上装

```
把游戏的 .exe 拖到 install.bat 上            ← 最省事
install.bat "D:\Games\XXX\bin\x64"          ← 传目录
install.bat "D:\Games\XXX" d3d11            ← 换代理 DLL 名
```

目录必须是**直接包含游戏 exe** 的那一层（`Cyberpunk 2077\bin\x64`、`PBZ\platform\steam`）。

安装器做的事（全部可逆）：

1. 目标目录已有 `dxgi.dll` 且不是我们这份 → 备份成 `dxgi.dll.mgpu_bak` 再写 ReShade；
2. 放 add-on、`game_capture.ini`、shader、预设（旧的一律先 `.mgpu_bak`）；
3. `ReShade.ini` 存在则只补 `EffectSearchPaths` / `PresetPath`（`ReShade.ini.mgpu_bak` 留底），
   不存在则写一份完整的（`[OVERLAY] TutorialProgress=4`，跳过首次弹窗）；
4. 装完做 SHA256 校验，不一致会明确告警（Steam 库走 9p/NTFS 时 `cp` 有时"报告成功但没落盘"）。

游戏开着会直接拒绝安装（DLL 被占用，写盘必然失败），`-Force` 可强行继续。

## 卸载

```
uninstall.bat "D:\Games\XXX\bin\x64"
```

`.mgpu_bak` 有就还原，没有就按 SHA256 确认"是我们放的那份"才删；`reshade-shaders` 空目录
一并清理。**装上→卸掉，目录内容和装之前一致**（已实测）。

## 三条实测过的坑

1. **`.ps1` 必须带 UTF-8 BOM**：PowerShell 5.1 无 BOM 时按 ANSI(GBK) 解析，脚本里任何中文
   都会变成语法错误。`make_addon_package.sh` 每次打包都会强制补 BOM + CRLF。
2. **`.bat` 保持纯 ASCII**：BOM 会让 cmd 第一行报错，中文注释在 GBK 控制台下是乱码。
3. **必须有一个启用的效果**：add-on 的每帧逻辑跑在 `reshade_begin_effects` 里，ReShade 只有
   在"至少有一个启用效果"时才会调它。所以 `ReShadePreset.ini` 里的
   `Techniques=MGPU_DepthTap@mgpu_depth_tap.fx` 不能省，否则 add-on 加载成功但什么都不做，
   bridge 会一直等 `Local\mgpu_mailbox_v1`。

## 和 `scripts/deploy_game.sh` 的关系

`deploy_game.sh` 是 WSL 里的**开发期**脚本：从 CP2077 目录拿 ReShade、要求游戏先跑一次生成
`ReShade.ini`、只走 `dxgi`。新的部署包是给"交给别人/换机器/多游戏"用的成品：
自带 ReShade + ini 模板、纯 Windows、可卸载、带校验。日常开发仍旧用 `deploy_game.sh` 或
`deploy_addon.sh` 更快。

## 验证（每次改包应当重跑）

```bash
# 假游戏目录 + 一个假 dxgi.dll + 一份外来 ReShade.ini/预设
mkdir -p /mnt/c/temp/mgpu/testgame
cp build/m0_probe.exe /mnt/c/temp/mgpu/testgame/FakeGame.exe
cp build/wgc_probe.exe /mnt/c/temp/mgpu/testgame/dxgi.dll
printf '[GENERAL]\r\nNoDebugInfo=1\r\n' > /mnt/c/temp/mgpu/testgame/ReShade.ini
cp -r dist/mgpu_reshade_pack /mnt/c/temp/mgpu/pack_test
powershell.exe -NoProfile -ExecutionPolicy Bypass -File '<STAGE_DIR>\pack_test\install.ps1' \
    -GameDir '<STAGE_DIR>\testgame' -NoPause
powershell.exe -NoProfile -ExecutionPolicy Bypass -File '<STAGE_DIR>\pack_test\uninstall.ps1' \
    -GameDir '<STAGE_DIR>\testgame' -NoPause
# 期望：addon/ini/shader/预设/ReShade.ini 都还原或删除，dxgi.dll 的 md5 回到原来那个假文件
```
