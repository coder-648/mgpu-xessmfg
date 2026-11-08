#!/usr/bin/env bash
# ===========================================================================
#  Build the complete mgpu release bundle:
#
#      dist/mgpu-<version>/            <- folder, copy anywhere
#      dist/mgpu-<version>.zip
#
#  Layout produced:
#      README.md            使用说明（面向最终用户，= docs/ops/USAGE.md）
#      LICENSE  NOTICE      GPL-3.0 全文 + 第三方署名
#      SHA256SUMS.txt       全部文件的 SHA256
#      bin/                 bridge.exe / mgpu_ui.exe / 两个 XeSS 运行时 DLL
#      bin/*.bat            三份配方脚本 + dump + 启动 UI（双击即可）
#      game-pack/           ReShade add-on 部署包（install.bat 拖 exe 即装）
#      docs/                架构 / 协议 / 配置 / 结论 / 画质变量 / 踩坑清单
#
#  Usage:
#      ./scripts/make_release.sh                 # build whatever is missing, then pack
#      REBUILD=1 ./scripts/make_release.sh       # force a fresh build of all three
#      MGPU_VERSION=0.10-demo ./scripts/make_release.sh
#      RESHADE_SRC=/x/ReShade64.dll ./scripts/make_release.sh
#      SKIP_XESS_DLLS=1 ./scripts/make_release.sh   # leave Intel's DLLs out
#
#  NOTE on libxess_fg.dll / libxell.dll: they are Intel's, under Intel's own
#  licence (third_party/xess/LICENSE.txt) - they are NOT part of this project
#  and a *public* release should leave them out (SKIP_XESS_DLLS=1) and tell the
#  user to take them from the XeSS SDK. This script includes them by default
#  because it also builds your own local bundle. See docs/ops/LEGAL.md 3.
# ===========================================================================
set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STAGE_ROOT="${MGPU_STAGE:-/mnt/c/temp/mgpu}"

BRIDGE="$REPO/build/bridge.exe"
ADDON="$REPO/build/game_capture.addon64"
UI="$REPO/build/mgpu_ui.exe"

SHA="$(git -C "$REPO" rev-parse --short HEAD 2>/dev/null || echo nogit)"
VER="${MGPU_VERSION:-0.10-$SHA}"
OUT="$REPO/dist/mgpu-$VER"
ZIP="$REPO/dist/mgpu-$VER.zip"

say() { printf '[release] %s\n' "$*"; }
die() { printf '[release] ERROR: %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------- 1. build
need_build=0
for f in "$BRIDGE" "$ADDON" "$UI"; do
    [ -f "$f" ] || { need_build=1; say "missing $f"; }
done
if [ "${REBUILD:-0}" = "1" ]; then need_build=1; fi

if [ "$need_build" = "1" ]; then
    say "building bridge.exe"
    "$REPO/scripts/build_bridge.sh" >/tmp/mgpu_release_bridge.log 2>&1 || {
        tail -25 /tmp/mgpu_release_bridge.log; die "bridge build failed"; }
    say "building game_capture.addon64"
    "$REPO/scripts/build_addon.sh" src/game_capture game_capture >/tmp/mgpu_release_addon.log 2>&1 || {
        tail -25 /tmp/mgpu_release_addon.log; die "add-on build failed"; }
    say "building mgpu_ui.exe"
    "$REPO/scripts/build_ui.sh" >/tmp/mgpu_release_ui.log 2>&1 || {
        tail -25 /tmp/mgpu_release_ui.log; die "UI build failed"; }
fi
for f in "$BRIDGE" "$ADDON" "$UI"; do
    [ -f "$f" ] || die "still missing $f"
done
say "bridge   $(md5sum "$BRIDGE" | cut -d' ' -f1)  $(stat -c%s "$BRIDGE") B"
say "add-on   $(md5sum "$ADDON"  | cut -d' ' -f1)  $(stat -c%s "$ADDON") B"
say "ui       $(md5sum "$UI"     | cut -d' ' -f1)  $(stat -c%s "$UI") B"

# ------------------------------------------------- 2. game-side pack (ReShade)
pick_reshade() {
    local c
    for c in "${RESHADE_SRC:-}" "$STAGE_ROOT/ReShade64.dll"; do
        [ -n "$c" ] && [ -f "$c" ] && { printf '%s' "$c"; return 0; }
    done
    return 1
}
RESHADE="$(pick_reshade)" || die \
    "no ReShade 6.8 x64 DLL found - set RESHADE_SRC=/path/to/ReShade64.dll (a game's dxgi.dll works)"
say "ReShade host: $RESHADE"

SKIP_TOOLS=1 RESHADE_SRC="$RESHADE" "$REPO/scripts/make_addon_package.sh" >/tmp/mgpu_release_pack.log 2>&1 || {
    tail -25 /tmp/mgpu_release_pack.log; die "deployment pack failed"; }
PACK="$REPO/dist/mgpu_reshade_pack"
[ -d "$PACK/files" ] || die "deployment pack not produced at $PACK"

# ---------------------------------------------------------------- 3. assemble
rm -rf "$OUT"
mkdir -p "$OUT/bin" "$OUT/docs/design" "$OUT/docs/ops" "$OUT/docs/findings" "$OUT/docs/images"

cp "$BRIDGE" "$ADDON" "$UI" "$OUT/bin/"
if [ "${SKIP_XESS_DLLS:-0}" != "1" ]; then
    for d in libxess_fg.dll libxell.dll; do
        src="$REPO/third_party/xess/bin/$d"
        [ -f "$src" ] || src="$STAGE_ROOT/bin/$d"
        [ -f "$src" ] || die "missing $d (set SKIP_XESS_DLLS=1 to build without it)"
        cp "$src" "$OUT/bin/"
    done
else
    say "SKIP_XESS_DLLS=1: leaving Intel's DLLs out of the bundle"
    cat > "$OUT/bin/README-DLL.txt" <<'TXT'
本包不含 Intel 的 XeSS 运行时（libxess_fg.dll / libxell.dll）——它们属于
Intel，按 Intel 自己的许可分发，不能随本项目一起打包。放好它们才能跑：

  1. 取 Intel XeSS SDK v3.0.2（https://github.com/intel/xess ，tag v3.0.2），
     或任何 XeSS 3 SDK 的 bin/ 目录
  2. 把 libxess_fg.dll 与 libxell.dll 拷进本目录（与 bridge.exe 同一层）
  3. 双击 "0 - UI.bat" 或任一配方脚本

版本提示：
  - 2x（--interp 1）：任意 1.3.x 都能跑
  - 3x 及以上要 --unlock-mfg，它只对实测过的版本成立
    （libxess_fg 1.3.1.78 / libxell 1.3.2.10）；别的版本会拒绝执行并打印
    *_unexpected_bytes —— 这是故意的，不会瞎改
TXT
fi

# The recipe scripts cd into their own folder, so they belong next to bridge.exe.
cp "$REPO/scripts/recipes/run_3x_of.bat"      "$OUT/bin/1 - 3x optical flow.bat"
cp "$REPO/scripts/recipes/run_2x_quality.bat" "$OUT/bin/2 - 2x quality.bat"
cp "$REPO/scripts/recipes/run_3x_nomv.bat"    "$OUT/bin/3 - 3x lowest cost.bat"
cp "$REPO/scripts/recipes/run_dump.bat"       "$OUT/bin/4 - dump frames.bat"
# The smoothest profile: per-frame multiplier (--interp-cadence), one command for
# 2x/3x/4x/dynamic (docs/findings/PERF_BASELINE.md 3.17-3.21).
cp "$REPO/scripts/recipes/run_smooth.bat"     "$OUT/bin/5 - smoothest (cadence).bat"
cp "$REPO/scripts/recipes/run_smooth_batchfeed.bat" "$OUT/bin/6 - cadence batch feed (A-B).bat"
printf '@echo off\r\nrem Start the front-end (bridge.exe must be in this folder).\r\ncd /d "%%~dp0"\r\nstart "" "mgpu_ui.exe"\r\n' \
    > "$OUT/bin/0 - UI.bat"

# --------------------------------------------------------------- 4. game pack
cp -r "$PACK" "$OUT/game-pack"
cat > "$OUT/game-pack/README-BIN.txt" <<'TXT'
这个目录是"游戏里需要的那一半"：把游戏的 .exe 拖到 install.bat 上即可装，
拖到 uninstall.bat 上即可完全卸载。

bridge.exe / mgpu_ui.exe 不在这里，它们在上一层的 bin\ 目录。
这里的 game_capture.addon64 只服务游戏进程，它和 bridge 通过共享显存放数据。
TXT

# ------------------------------------------------------------------ 5. docs
cp "$REPO/LICENSE" "$REPO/NOTICE" "$REPO/ROADMAP.md" "$OUT/"
cp "$REPO/docs/ops/USAGE.md" "$OUT/README.md"
cp "$REPO/docs/images/ui.png" "$OUT/docs/images/"
for f in ARCHITECTURE PROTOCOL INPUT_SOURCES; do
    cp "$REPO/docs/design/$f.md" "$OUT/docs/design/"
done
for f in CONFIG BUILD DEPLOY UI LEGAL; do
    cp "$REPO/docs/ops/$f.md" "$OUT/docs/ops/"
done
for f in CONCLUSIONS QUALITY_VARIABLES FINAL_CONFIG; do
    cp "$REPO/docs/findings/$f.md" "$OUT/docs/findings/"
done

# ------------------------------------------------------------- 6. checksums
( cd "$OUT" && find . -type f ! -name SHA256SUMS.txt | sort | while read -r f; do
    printf '%s  %s\n' "$(sha256sum "$f" | cut -d' ' -f1)" "${f#./}"
done > SHA256SUMS.txt )
say "checksums: $(grep -c '' "$OUT/SHA256SUMS.txt") files"

# -------------------------------------------------------------------- 7. zip
( cd "$REPO/dist" && rm -f "$(basename "$ZIP")" \
  && zip -qr "$(basename "$ZIP")" "$(basename "$OUT")" )
say "folder : $OUT"
say "zip    : $ZIP  ($(stat -c%s "$ZIP") B)"
say "md5    : bridge $(md5sum "$BRIDGE" | cut -d' ' -f1)  addon $(md5sum "$ADDON" | cut -d' ' -f1)  ui $(md5sum "$UI" | cut -d' ' -f1)"
