#!/usr/bin/env bash
# ============================================================================
#  lock_frame 正式版打包
#
#    ./lock_frame/release.sh                 # 用现有 build/ 产物打包
#    BUILD=1 ./lock_frame/release.sh         # 先构建 consumer/producer/fakerFG/ui
#    VERSION=1.0.0 ./lock_frame/release.sh   # 覆盖 version.h 里的版本号
#    WITH_XESS=1 ./lock_frame/release.sh     # 把 Intel 运行时一起放进包里
#    WITH_THIRD_PARTY=1 ./lock_frame/release.sh  # 源码包也放入本地 third_party 副本
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

OUT="$REPO/dist/lock_frame-$VER"
ZIP="$REPO/dist/lock_frame-$VER.zip"
SRC_ZIP="$REPO/dist/lock_frame-$VER-src.zip"

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
cp -f "$HERE/run_ui.bat"                      "$OUT/"
cp -f "$HERE/run_2x.bat"                      "$OUT/"
cp -f "$HERE/run_3x.bat"                      "$OUT/"
cp -f "$HERE/run_4x.bat"                      "$OUT/"
cp -f "$HERE/run_30x4.bat"                    "$OUT/"
cp -f "$HERE/run_40x3.bat"                    "$OUT/"
cp -f "$HERE/deploy_game.bat"                 "$OUT/"
cp -f "$HERE/collect_diagnostics.bat"         "$OUT/"
cp -f "$HERE/README.md"                       "$OUT/README.md"
# 发布说明按版本取；没有对应文件时退回 1.0.0（保持旧版本可复现打包）。
if [ -f "$HERE/docs/RELEASE_NOTES_$VER.md" ]; then
    cp -f "$HERE/docs/RELEASE_NOTES_$VER.md"  "$OUT/RELEASE_NOTES.md"
else
    cp -f "$HERE/docs/RELEASE_NOTES_1.0.0.md" "$OUT/RELEASE_NOTES.md"
fi
cp -f "$REPO/CHANGELOG.md"                    "$OUT/CHANGELOG.md"
cp -f "$REPO/LICENSE"                         "$OUT/"
cp -f "$REPO/NOTICE"                          "$OUT/"

# --- optional Intel runtime --------------------------------------------------
# Intel's licence allows unmodified redistribution with the notice. The default
# package leaves them out so the user obtains them from Intel; WITH_XESS=1 is
# for a self-contained local build.
if [ "${WITH_XESS:-0}" = "1" ]; then
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

cat > "$OUT/docs/DEPLOY.md" <<'EOF'
# 部署到游戏

1. 把整个发布包解压到一个可写目录（不要只解压到游戏目录）。
2. 以管理员/普通权限运行：

   ```bat
   deploy_game.bat "F:\steam\steamapps\common\PBZ\platform\steam"
   ```

   它会：
   - 把游戏自己的 `nvngx_dlssg.dll` 保留为 `nvngx_dlssg_real.dll`；
   - 备份原有 `dxgi.dll` 为 `dxgi.dll.bak`；
   - 写入 `fakerFG.dll` / `dxgi.dll` / `nvngx_dlssg.dll` / `xess_mfg_producer.dll`；
   - 只在缺失时放入 `fakerFG.ini`。
3. 把 `libxess_fg.dll` / `libxell.dll` 放到**发布包目录**（与 `lock_frame_ui.exe`
   同目录），不要放进游戏目录。
4. 启动游戏，进到画面并开启 DLSS Frame Generation；再运行 `lock_frame_ui.exe`。
5. 在 UI 里选择游戏窗口、固定倍率、fakerFG 输入源，点击启动。

卸载见 `UNINSTALL.md`。
EOF

# --- docs --------------------------------------------------------------------
cat > "$OUT/docs/SUPPORTED-GAMES.md" <<'EOF'
# 已验证游戏

首版正式支持 **FgInput=DLSSG**：

- Cyberpunk 2077
- PBZ
- RED 引擎（已实测）
- UE5（已实测）
- UE4（已实测）

`NVNGX` / `Upscaler` / `FSR` 为实验性车道：代码可用，但请按游戏逐一验证。
只支持无边框/窗口化全屏；不要在带反作弊的在线游戏中使用。
EOF

cat > "$OUT/docs/TROUBLESHOOTING.md" <<'EOF'
# 排障

1. **UI 显示“缺少 libxess_fg.dll”**
   把 Intel XeSS 的 `libxess_fg.dll`、`libxell.dll` 放到 UI 同目录。
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

cat > "$OUT/docs/UNINSTALL.md" <<'EOF'
# 卸载

1. 从游戏目录删除 `fakerFG.dll`、`dxgi.dll`、`xess_mfg_producer.dll`
   （如果这些文件是本项目放入的）。
2. 恢复部署前的备份（如有 `*.bak` / `nvngx_dlssg_real.dll`）。
3. 删除 `lock_frame.exe`、`lock_frame_ui.exe`、`fakerFG.ini` 以及本项目生成的
   `*_fakerFG.log` / `*_mfg_producer.log` / `lock_frame_ui*.txt`。
EOF

cat > "$OUT/README.txt" <<EOF
lock_frame $VER（共享协议 v$PROTO）

1. 解压到一个可写目录（不要只解压到游戏目录）。
2. 运行：
     deploy_game.bat "游戏 exe 目录"
3. 把 libxess_fg.dll / libxell.dll 放到本目录（与 lock_frame_ui.exe 同目录）。
4. 启动游戏，进画面，开启 DLSS Frame Generation。
5. 运行 lock_frame_ui.exe：
     - 选择游戏窗口
     - 固定倍率：2x / 3x / 4x（120Hz 面板分别锁 60/40/30 fps）
     - fakerFG 输入源：DLSSG（推荐）
     - 点启动
6. 排障：docs/TROUBLESHOOTING.md；反馈前运行 collect_diagnostics.bat。

文件：
  lock_frame.exe / lock_frame_ui.exe   消费端与前端
  fakerFG.dll                         游戏侧代理（deploy_game 复制为 dxgi.dll 等）
  xess_mfg_producer.dll               跨卡传输
  fakerFG.ini                         游戏侧配置模板（deploy_game 只在缺失时放入）

许可证：GPL-3.0，见 LICENSE / NOTICE。对应源码见同版本 -src.zip。
EOF

# --- manifest + checksums ----------------------------------------------------
python3 "$HERE/scripts/write_manifest.py" \
    --root "$OUT" --version "$VER" --protocol "$PROTO" --git "$SHA" \
    --toolchain "${MGPU_FXC:-/mnt/c/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64/fxc.exe}"

( cd "$OUT" && find . -type f ! -name SHA256SUMS.txt -print0 \
    | sort -z | xargs -0 sha256sum > SHA256SUMS.txt )

# --- source package (GPL-3.0) ------------------------------------------------
say "packing source (GPL-3.0)"
SRC_TMP="$REPO/dist/.lock_frame-src-$VER"
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
( cd "$REPO/dist" && zip -qr "$(basename "$ZIP")" "$(basename "$OUT")" )

# --- validation --------------------------------------------------------------
python3 "$HERE/scripts/check_release.py" "$OUT" --protocol "$PROTO" --version "$VER"

say "folder  : $OUT"
say "zip     : $ZIP  ($(du -h "$ZIP" | cut -f1))"
say "source  : $SRC_ZIP  ($(du -h "$SRC_ZIP" | cut -f1))"
say "next    : review $OUT/README.md and copy the runtime files into the game directory"

