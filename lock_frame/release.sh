#!/usr/bin/env bash
# ============================================================================
#  lock_frame 正式版打包
#
#    ./lock_frame/release.sh                 # 用现有 build/ 产物打包
#    BUILD=1 ./lock_frame/release.sh         # 先构建 consumer/producer/fakerFG/ui
#    VERSION=1.0.0 ./lock_frame/release.sh   # 覆盖 version.h 里的版本号
#    WITH_XESS=1 ./lock_frame/release.sh     # 把 Intel 运行时一起放进包里
#    WITH_MFG_ASSETS=1 ./lock_frame/release.sh # 本地包带上 MFG 实验资产（不公开分发）
#    WITH_THIRD_PARTY=1 ./lock_frame/release.sh  # 源码包也放入本地 third_party 副本
#    DIST_DIR=/mnt/c/Users/.../Downloads ./lock_frame/release.sh  # 输出到指定目录
#
#  产物：
#    dist/lock_frame-<ver>/                 完整运行包（不含 Intel DLL）
#    dist/lock_frame-<ver>.zip
#    dist/lock_frame-<ver>-src.zip          GPL-3.0 对应源码包
#    dist/lock_frame-<ver>/manifest.json
#    dist/lock_frame-<ver>/SHA256SUMS.txt
# ============================================================================
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"

say() { printf '[release] %s\n' "$*"; }
die() { printf '[release] ERROR: %s\n' "$*" >&2; exit 1; }

# Version: version.h is the single source of truth.
VER="${VERSION:-$(sed -n 's/^#define LOCK_FRAME_VERSION_STRING "\(.*\)"/\1/p' "$HERE/version.h" | head -1)}"
[ -n "$VER" ] || die "cannot read LOCK_FRAME_VERSION_STRING from version.h"
PROTO="$(sed -n 's/^inline constexpr uint32_t kSharedVersion = \([0-9]*\);/\1/p' "$HERE/include/mgpu_shared.h" | head -1)"
[ -n "$PROTO" ] || die "cannot read kSharedVersion from include/mgpu_shared.h"
SHA="$(git -C "$REPO" rev-parse --short HEAD 2>/dev/null || echo nogit)"

DIST="$(mkdir -p "${DIST_DIR:-$REPO/dist}" && cd "${DIST_DIR:-$REPO/dist}" && pwd)"
OUT="$DIST/lock_frame-$VER"
ZIP="$DIST/lock_frame-$VER.zip"
SRC_ZIP="$DIST/lock_frame-$VER-src.zip"

if [ "${BUILD:-0}" = "1" ]; then
    say "building all artifacts"
    "$HERE/build.sh" all
fi

for f in lock_frame.exe lock_frame_ui.exe fakerFG.dll xess_mfg_producer.dll; do
    [ -f "$REPO/build/$f" ] || die "missing build/$f (run BUILD=1 $0)"
done

say "version $VER, shared protocol v$PROTO, git $SHA"
rm -rf "$OUT" "$ZIP" "$SRC_ZIP"
mkdir -p "$OUT/docs" "$OUT/profiles"

# --- runtime payload ---------------------------------------------------------
cp -f "$REPO/build/lock_frame.exe"            "$OUT/"
cp -f "$REPO/build/lock_frame_ui.exe"         "$OUT/"
cp -f "$REPO/build/fakerFG.dll"               "$OUT/"
cp -f "$REPO/build/xess_mfg_producer.dll"      "$OUT/"
cp -f "$HERE/fakerFG.ini"                     "$OUT/"
cp -f "$HERE/fakerFG.full.ini"                "$OUT/"
cp -f "$HERE/collect_diagnostics.bat"         "$OUT/"
cp -f "$HERE/README.md"                       "$OUT/README.md"
# 发布说明按版本取；没有对应文件时退回 1.0.0（保持旧版本可复现打包）。
if [ -f "$HERE/docs/RELEASE_NOTES_$VER.md" ]; then
    cp -f "$HERE/docs/RELEASE_NOTES_$VER.md"  "$OUT/RELEASE_NOTES.md"
else
    cp -f "$HERE/docs/RELEASE_NOTES_1.0.0.md" "$OUT/RELEASE_NOTES.md"
fi
cp -f "$REPO/LICENSE"                         "$OUT/"
cp -f "$REPO/NOTICE"                          "$OUT/"
cp -f "$REPO/docs/design/UI_PATCH_PERF.md"    "$OUT/docs/UI_PATCH_PERF.md"

# --- optional Intel runtime --------------------------------------------------
# Intel's licence allows unmodified redistribution with the notice. Since 1.2.0 the
# default package is self-contained (includes the runtime + Intel licence); set
# WITH_XESS=0 for a slim package that asks the user to obtain them from Intel.
if [ "${WITH_XESS:-1}" = "1" ]; then
    for dll in libxess_fg.dll libxell.dll; do
        [ -f "$REPO/third_party/xess/bin/$dll" ] || die "missing third_party/xess/bin/$dll"
        cp -f "$REPO/third_party/xess/bin/$dll" "$OUT/"
    done
    cp -f "$REPO/third_party/xess/LICENSE.txt" "$OUT/INTEL-XESS-LICENSE.txt"
else
    cat > "$OUT/docs/RUNTIME.md" <<'EOF'
# XeSS 运行时

正式包不内置 `libxess_fg.dll` / `libxell.dll`。请从 Intel XeSS SDK 获取与你
的驱动匹配的版本，放到 `lock_frame_ui.exe` 同目录。Intel 许可允许原样再分发，
但必须保留其版权与许可文本；本包不修改这两个 DLL。
EOF
fi

# --- docs --------------------------------------------------------------------
cat > "$OUT/docs/SUPPORTED-GAMES.md" <<'EOF'
# 已验证游戏

首版正式支持 **FgInput=DLSSG**：

- Cyberpunk 2077
- PBZ
- Wuchang: Fallen Feathers
- RED 引擎（已实测）
- UE5（已实测）
- UE4（已实测）

`NVNGX` / `Upscaler` / `FSR` 车道在当前构建中**未实现**：`FgInput` 固定为
`DLSSG`，ini 里写其他值会被强制回 `DLSSG`（`FgInput=NoFG` 仍是显式禁用）。
只支持无边框/窗口化全屏；不要在带反作弊的在线游戏中使用。

游戏侧配置有两个模板：

- `fakerFG.ini`：发布精简版（UI 部署时默认使用）；
- `fakerFG.full.ini`：测试全参数版（UI →「游戏侧配置 → 配置版本」选「完整（测试）」）。
EOF

cat > "$OUT/docs/TROUBLESHOOTING.md" <<'EOF'
# 排障

1. **提示缺少 libxess_fg.dll / libxell.dll**
   本包默认自带这两个 DLL；如果手动删掉了，从原压缩包重新解压一份放回 UI 同目录。
2. **启动后没有画面 / 一直 1X**
   确认游戏目录的 `fakerFG.ini` 是 `FgInput=DLSSG`、`FgOutput=Capture`、
   `Passthrough=0`；确认游戏已开启 DLSS Frame Generation（或对应输入 API）。
3. **3x/4x 实际只有 2x**
   看 `<游戏>_fakerFG.log` 和 consumer 日志里的 `maxSupportedInterpolations`；
   `--unlock-mfg` 只对实测过的 libxess_fg/libxell 版本有效。
4. **卡顿 / 配对下降**
   先把游戏锁到建议输入帧率；看 consumer 的 `pair:` 行和 `mvN` OSD。
5. **SmartScreen 拦截**
   本包未签名时，用 `SHA256SUMS.txt` 校验完整性；不要从非官方来源下载。
6. **需要反馈问题**
   运行 `collect_diagnostics.bat`，把生成的 zip 发给支持者；它只收集日志、
   版本、系统/显卡/显示模式信息，不联网、不上传。
EOF

# --- optional MFG experiment assets (local research only) -------------------
# The 54 portable programs + 7 replacement shaders are derived from Intel's
# XeSS SDK. They are a local research asset, not part of the public package.
if [ "${WITH_MFG_ASSETS:-0}" = "1" ]; then
    [ -d "$REPO/build/xefg_programs" ] || die "missing build/xefg_programs (run scripts/extract_mfgunlocker_assets.py)"
    say "including local MFG experiment assets (do not redistribute)"
    cp -a "$REPO/build/xefg_programs" "$OUT/"
    cp -f "$HERE/xefg_mfg/NOTICE.md" "$OUT/docs/MFG-PORT-NOTICE.md"
fi

# --- manifest + checksums ----------------------------------------------------
python3 "$HERE/scripts/write_manifest.py" \
    --root "$OUT" --version "$VER" --protocol "$PROTO" --git "$SHA" \
    --toolchain "${MGPU_FXC:-/mnt/c/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64/fxc.exe}"

( cd "$OUT" && find . -type f ! -name SHA256SUMS.txt -print0 \
    | sort -z | xargs -0 sha256sum > SHA256SUMS.txt )

# --- source package (GPL-3.0) ------------------------------------------------
say "packing source (GPL-3.0)"
SRC_TMP="$DIST/.lock_frame-src-$VER"
rm -rf "$SRC_TMP"; mkdir -p "$SRC_TMP"
cp -a "$HERE" "$SRC_TMP/lock_frame"
cp -a "$REPO/src" "$SRC_TMP/src"
cp -a "$REPO/scripts" "$SRC_TMP/scripts"
cp -a "$REPO/docs" "$SRC_TMP/docs"
if [ "${WITH_THIRD_PARTY:-0}" = "1" ]; then
    mkdir -p "$SRC_TMP/third_party"
    for d in ffx streamline minhook dlss; do
        [ -d "$REPO/third_party/$d" ] || continue
        cp -a "$REPO/third_party/$d" "$SRC_TMP/third_party/"
    done
fi
cp -f "$REPO/LICENSE" "$REPO/NOTICE" "$SRC_TMP/"
find "$SRC_TMP" -type d \( -name __pycache__ -o -name temp -o -name build -o -name dist \) \
    -prune -exec rm -rf {} +
find "$SRC_TMP" -type f \( -name '*.bak*' -o -name '*.log' -o -name '*.zip' \) -delete
cat > "$SRC_TMP/SOURCE-NOTES.md" <<EOF
# 对应源码（GPL-3.0）

- 版本：$VER
- 共享协议：v$PROTO
- git：$SHA
- 构建：见 \`lock_frame/build.sh\` 与 \`docs/ops/BUILD.md\`
- 默认**不包含** \`third_party/\`；按 \`docs/ops/DEPENDENCIES.md\` 获取
  ffx / streamline / minhook / dlss 与 Intel XeSS SDK。
  打包时可用 \`WITH_THIRD_PARTY=1\` 把本地副本一起放进源码包。
- Intel XeSS SDK（\`third_party/xess/inc\` 与 lib/bin）需自行从 Intel 获取；
  本包不包含 Intel 的专有头文件或二进制运行时。
- 第三方许可按 \`NOTICE\` 与 \`docs/ops/LEGAL.md\` 执行。
EOF
( cd "$SRC_TMP" && zip -qr "$SRC_ZIP" . )
rm -rf "$SRC_TMP"

# --- main zip ----------------------------------------------------------------
( cd "$DIST" && zip -qr "$(basename "$ZIP")" "$(basename "$OUT")" )

# --- validation --------------------------------------------------------------
python3 "$HERE/scripts/check_release.py" "$OUT" --protocol "$PROTO" --version "$VER"

say "folder  : $OUT"
say "zip     : $ZIP  ($(du -h "$ZIP" | cut -f1))"
say "source  : $SRC_ZIP  ($(du -h "$SRC_ZIP" | cut -f1))"
say "next    : review $OUT/README.md and copy the runtime files into the game directory"

