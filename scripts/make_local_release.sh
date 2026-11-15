#!/usr/bin/env bash
# ===========================================================================
#  make_local_release.sh - assemble a LOCAL test build (nothing is published).
#
#    ./scripts/make_local_release.sh            -> dist/mgpu-local-<date>[-<sha>]/
#    MGPU_VERSION=... ./scripts/make_local_release.sh
#
#  Layout:
#    bin/     bridge.exe, mgpu_ui.exe, libxess_fg.dll, libxell.dll
#    game/    dxgi.dll, nvngx_dlssg.dll (= fakerFG), xess_mfg_producer.dll, fakerFG.ini
#    install.bat / uninstall.bat   drag a game folder (or exe) onto install.bat
#    README.md                     what this build is, how to run it, what to test
#    SHA256SUMS.txt
#
#  Everything is built from the current tree first, so the package always matches
#  the sources. Re-runnable: it deletes its own dist directory.
# ===========================================================================
set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SHA="$(git -C "$REPO" rev-parse --short HEAD 2>/dev/null || echo nogit)"
STAMP="$(date +%Y%m%d-%H%M)"
VER="${MGPU_VERSION:-local-$STAMP-$SHA}"
OUT="$REPO/dist/mgpu-$VER"
ZIP="$REPO/dist/mgpu-$VER.zip"

say() { printf '[release] %s\n' "$*"; }
die() { printf '[release] ERROR: %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------- 1. build
say "building fakerFG.dll"
"$REPO/scripts/build_faker_fg.sh" >/tmp/mgpu_rel_fg.log 2>&1 || { tail -30 /tmp/mgpu_rel_fg.log; die "fakerFG build failed"; }
say "building xess_mfg_producer.dll"
"$REPO/scripts/build_producer.sh" >/tmp/mgpu_rel_prod.log 2>&1 || { tail -30 /tmp/mgpu_rel_prod.log; die "producer build failed"; }
say "building bridge.exe"
"$REPO/scripts/build_bridge.sh" >/tmp/mgpu_rel_bridge.log 2>&1 || { tail -30 /tmp/mgpu_rel_bridge.log; die "bridge build failed"; }
say "building mgpu_ui.exe"
"$REPO/scripts/build_ui.sh" >/tmp/mgpu_rel_ui.log 2>&1 || { tail -30 /tmp/mgpu_rel_ui.log; die "UI build failed"; }

FG="$REPO/build/fakerFG.dll"
PROD="$REPO/build/xess_mfg_producer.dll"
BRIDGE="$REPO/build/bridge.exe"
UI="$REPO/build/mgpu_ui.exe"
for f in "$FG" "$PROD" "$BRIDGE" "$UI" \
         "$REPO/third_party/xess/bin/libxess_fg.dll" \
         "$REPO/third_party/xess/bin/libxell.dll"; do
    [ -f "$f" ] || die "missing $f"
done

say "fakerFG   $(stat -c%s "$FG") B  $(md5sum "$FG" | cut -d' ' -f1)"
say "producer  $(stat -c%s "$PROD") B  $(md5sum "$PROD" | cut -d' ' -f1)"
say "bridge    $(stat -c%s "$BRIDGE") B  $(md5sum "$BRIDGE" | cut -d' ' -f1)"
say "ui        $(stat -c%s "$UI") B  $(md5sum "$UI" | cut -d' ' -f1)"

# ---------------------------------------------------------------- 2. layout
rm -rf "$OUT" "$ZIP"
mkdir -p "$OUT/bin" "$OUT/game"

cp -f "$BRIDGE" "$UI" "$OUT/bin/"
cp -f "$REPO/third_party/xess/bin/libxess_fg.dll" "$REPO/third_party/xess/bin/libxell.dll" "$OUT/bin/"

cp -f "$FG" "$OUT/game/dxgi.dll"
cp -f "$FG" "$OUT/game/nvngx_dlssg.dll"
cp -f "$PROD" "$OUT/game/xess_mfg_producer.dll"
cp -f "$REPO/packaging/reshade_pack/fakerFG.ini" "$OUT/game/fakerFG.ini"

# ---------------------------------------------------------------- 3. installers
cat > "$OUT/install.bat" <<'BAT'
@echo off
setlocal enabledelayedexpansion
rem Drag the game folder (or its .exe) onto this file. Nothing is deleted:
rem existing dxgi.dll / nvngx_dlssg.dll are renamed to *.mgpu_bak first.
if "%~1"=="" goto usage
set "G=%~1"
if exist "!G!\" (set "G=!G!") else (set "G=%~dp1")
if not exist "!G!" goto usage

echo [mgpu] target: !G!
rem --- keep the title's own DLSS-G runtime once
if exist "!G!nvngx_dlssg.dll" if not exist "!G!nvngx_dlssg_real.dll" (
    findstr /C:"MgpuFgGetApi" "!G!nvngx_dlssg.dll" >nul 2>&1
    if errorlevel 1 (
        echo [mgpu] nvngx_dlssg.dll -^> nvngx_dlssg_real.dll
        move /y "!G!nvngx_dlssg.dll" "!G!nvngx_dlssg_real.dll" >nul
    )
)
rem --- back up whatever is there now (only once)
if exist "!G!dxgi.dll" if not exist "!G!dxgi.dll.mgpu_bak" (
    findstr /C:"MgpuFgGetApi" "!G!dxgi.dll" >nul 2>&1
    if errorlevel 1 copy /y "!G!dxgi.dll" "!G!dxgi.dll.mgpu_bak" >nul
)
if exist "!G!nvngx_dlssg.dll" if not exist "!G!nvngx_dlssg.dll.mgpu_bak" (
    findstr /C:"MgpuFgGetApi" "!G!nvngx_dlssg.dll" >nul 2>&1
    if errorlevel 1 copy /y "!G!nvngx_dlssg.dll" "!G!nvngx_dlssg.dll.mgpu_bak" >nul
)

copy /y "%~dp0game\dxgi.dll"              "!G!dxgi.dll" >nul
copy /y "%~dp0game\nvngx_dlssg.dll"       "!G!nvngx_dlssg.dll" >nul
copy /y "%~dp0game\xess_mfg_producer.dll" "!G!xess_mfg_producer.dll" >nul
if not exist "!G!fakerFG.ini" copy /y "%~dp0game\fakerFG.ini" "!G!fakerFG.ini" >nul

rem --- the old ReShade add-on must not run next to the producer
if exist "!G!game_capture.addon64" (
    echo [mgpu] retiring game_capture.addon64
    move /y "!G!game_capture.addon64" "!G!game_capture.addon64.retired" >nul
)
echo [mgpu] installed. Start the game, then run bin\bridge.exe --game <exe>
echo [mgpu] logs: ^<exe^>_fakerFG.log and ^<exe^>_mfg_producer.log next to the exe
exit /b 0

:usage
echo usage: drag the game folder (or its .exe) onto install.bat
exit /b 1
BAT

cat > "$OUT/uninstall.bat" <<'BAT'
@echo off
setlocal enabledelayedexpansion
if "%~1"=="" goto usage
set "G=%~1"
if exist "!G!\" (set "G=!G!") else (set "G=%~dp1")
if not exist "!G!" goto usage
echo [mgpu] restoring !G!
if exist "!G!dxgi.dll.mgpu_bak" (
    del /q "!G!dxgi.dll" 2>nul
    move /y "!G!dxgi.dll.mgpu_bak" "!G!dxgi.dll" >nul
)
if exist "!G!nvngx_dlssg_real.dll" (
    del /q "!G!nvngx_dlssg.dll" 2>nul
    move /y "!G!nvngx_dlssg_real.dll" "!G!nvngx_dlssg.dll" >nul
) else if exist "!G!nvngx_dlssg.dll.mgpu_bak" (
    del /q "!G!nvngx_dlssg.dll" 2>nul
    move /y "!G!nvngx_dlssg.dll.mgpu_bak" "!G!nvngx_dlssg.dll" >nul
)
del /q "!G!xess_mfg_producer.dll" 2>nul
if exist "!G!game_capture.addon64.retired" (
    move /y "!G!game_capture.addon64.retired" "!G!game_capture.addon64" >nul
)
echo [mgpu] done (fakerFG.ini left in place; delete it to reset the config)
exit /b 0

:usage
echo usage: drag the game folder (or its .exe) onto uninstall.bat
exit /b 1
BAT

# ---------------------------------------------------------------- 4. README
{
printf '# mgpu 本地测试版 %s\n\n' "$VER"
cat <<'TXT'
> 这是**本地测试包**，不对外发布。二进制全部由当前源码现编。
> 游戏侧已彻底去掉 ReShade add-on：`fakerFG.dll` 拥有 FG 管线，`xess_mfg_producer.dll` 只负责搬运。

## 包里有什么

```
bin\bridge.exe             副卡插帧 + 覆盖窗口（消费端）
bin\mgpu_ui.exe            前端（可选）
bin\libxess_fg.dll         副卡运行时（bridge 依赖）
bin\libxell.dll

game\dxgi.dll              = fakerFG.dll
game\nvngx_dlssg.dll       = fakerFG.dll（同一份，两个名字）
game\xess_mfg_producer.dll 传输模块，由 fakerFG 加载
game\fakerFG.ini           默认配置（Mode=consume）
```

## 安装 / 卸载

把游戏目录（或游戏 exe）拖到 `install.bat` 上；拖到 `uninstall.bat` 上还原。
安装只做三件事：备份原有 `dxgi.dll` / `nvngx_dlssg.dll`（`*.mgpu_bak`）、放三份文件、
把残留的 `game_capture.addon64` 改名 `.retired`。**不删除任何东西**。

## 跑起来

1. 启动游戏（Steam 或直接 exe，工作目录必须是 exe 所在目录）；
2. 需要双卡插帧时再启动：`bin\bridge.exe --game <游戏 exe 名>`；
3. 日志就在游戏 exe 旁边：`<exe>_fakerFG.log`、`<exe>_mfg_producer.log`。

## 两种模式（`fakerFG.ini` 的 `Mode=`）

| 值 | 游戏自己的 DLSS-G | 谁补帧 | 用途 |
|---|---|---|---|
| `local` | 被拒（不会偷偷跑） | fakerFG / XeFG，渲染卡 | **自检平台**：整条 FG 由我们跑完 |
| `consume` | 被拒 | 副卡 bridge | **双卡**：拿全输入、拒绝执行、原生帧直接上屏 |
| `idle` | 被拒 | 没人（1X） | 验证"不生成也不黑屏" |

切换要重开游戏（配置只在启动时读一次）。

## 本版本重点

- **模式语义修正**：拒绝 DLSS-G 契约时不再转发给 `nvngx_dlssg_real.dll`。以前 `local` 里游戏
  自己的 FG 仍在出帧（与 XeFG 并行），`consume` 里白烧渲染卡。现在只有 `off`/`observe`/
  `Passthrough=1` 才允许转发。
- **新的生产端**：旧 `game_capture`（ReShade add-on）已删除。`xess_mfg_producer.dll` 订阅
  `MgpuFgGetApi` v2 的同步 sink，在标题仍打开的命令表上录 barrier + 拷贝，Present 后 signal。
- **`StreamPercent`**：跨卡流尺寸（100 = 1:1；150 ≈ 2/3；200 = 1/2；400 = 1/4）。
  MV 和 depth 用一次 compute pass 缩到同一尺寸再传。4K：100 → 63.3 MB/帧，150 → 28.1 MB。
- **`Publish=0/1`**：测量开关。0 = 完全不发布（不拍快照、不回调、不传输），用来量发布路径的代价。
- **消费端自动 MV**：UI 不再提供"运动矢量"选项，bridge 每帧按 producer 实际发布的记录决定
  （日志 `mv: auto -> …`）。`--no-mv` / `--fg-of` / `--mv-flip` 仍可手动覆盖。
- **无消费请求就不拷**：bridge 没运行时 `request_mask = 0`，producer 不拷贝（日志
  `transport idle`）。注意：bridge 退出后共享内存里的掩码不会自动清零，想要干净的
  "无消费"基线请**先启动游戏、后启动 bridge**（或重开游戏）。

## 已验证（CP2077 4K，RTX 3080 Ti 渲染 + RX 6750 GRE 插帧）

- `local`：31.9 ms/帧，`fgEnabled=1`（XeFG 2X）
- `consume`：21.6 ms/帧，`fgEnabled=0`（1X，无补帧开销），bridge `presented=2 fg=1 SUCCESS`
- `Publish=0/1` 差值：local ≈ +2.5 ms/帧，consume ≈ +4.8 ms/帧（= 63.3 MB 的拷贝成本）
- `StreamPercent=150`：`bus: 28.1 MB/frame`，缩放日志 `downscale: mv/depth 3840x2160 -> 2560x1440`
- 自动 MV：`mv: auto -> the producer's motion vectors (producer flags=0x2051, mv slot 2560x1440)`

## 已知限制

- 4K 1:1 时拷贝约 2.5–4.8 ms/帧；先试 `StreamPercent=150`（≈1.2–2.2 ms），200 更省但画质下降；
- UE5 一类 tag 列表是 COPY 类型的标题不能缩放，保持 `StreamPercent=100`（日志会提示）；
- `StreamPercent`/`Publish`/`Mode` 都只在启动时读一次；
- 其它引擎（Wuchang / PBZ / Khazan / PRAGMATA）在本轮"拒绝 vs 转发"改动后**尚未回归**；
- `mgpu_ui.exe` 的界面流程本次未手测（bridge 命令行已实测）。

## 排障速查

| 现象 | 看什么 |
|---|---|
| 没有 `<exe>_mfg_producer.log` | `xess_mfg_producer.dll` 是否在 exe 同级；`fakerFG.ini` 是否完好 |
| producer 日志只有 `subscribed` | 游戏还没跑到超分（读档 / 在游戏里开 DLSS） |
| `producer: no consumer requested a stream` | 正常：没起 bridge（或 bridge 已退出） |
| `mv: auto -> optical flow / zeroed` | producer 没有发布 MV（降采样失败、`--no-mv`、或还没出第一帧） |
| `ngx: DLSS-G CreateFeature refused (mode=…)` | 正常：这是"游戏自己的 FG 被拒"的那一行 |
| 画面仍是原生帧率 | 双卡模式需要 bridge 在跑；或 `Mode=idle` |
TXT
} > "$OUT/README.md"

# ---------------------------------------------------------------- 5. checksums + zip
# cmd.exe parses multi-line ( ... ) blocks reliably only with CRLF endings.
sed -i 's/$/\r/' "$OUT/install.bat" "$OUT/uninstall.bat"
( cd "$OUT" && find . -type f ! -name SHA256SUMS.txt | sort | while read -r f; do
    printf '%s  %s\n' "$(sha256sum "$f" | cut -d' ' -f1)" "${f#./}"
done > SHA256SUMS.txt )

say "checksums: $(grep -c '' "$OUT/SHA256SUMS.txt") files"
( cd "$REPO/dist" && rm -f "$(basename "$ZIP")" && zip -qr "$(basename "$ZIP")" "$(basename "$OUT")" )
say "package: $OUT"
say "zip    : $ZIP ($(stat -c%s "$ZIP") B)"

