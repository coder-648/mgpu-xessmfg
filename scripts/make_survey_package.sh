#!/usr/bin/env bash
# ===========================================================================
#  Build + assemble the multi-game capture survey package.
#    ./scripts/make_survey_package.sh [tag]      -> build/pkg/mgpu_survey_<tag>.zip
#
#  Contents: version.dll (proxy), mgpu_capture.dll (core + own host glue),
#  mgpu_capture.ini, mgpu_proxy.ini, README.txt, collect_logs.bat.
#
#  The ini shipped here is deliberately different from the CP2077 working
#  config: it turns colour/UI on and FG cutting off, because the point of the
#  survey is to find out what each game actually provides, not to save fps.
# ===========================================================================
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TAG="${1:-$(date +%Y%m%d)}"
OUT="$REPO/build/pkg/mgpu_survey_$TAG"

echo "[1/4] building capture dll"
"$REPO/scripts/build_capture_dll.sh" > /tmp/pkg_cap.log 2>&1 || {
    iconv -f GBK -t UTF-8 -c /tmp/pkg_cap.log | tail -20; exit 1; }

echo "[2/4] building proxy"
"$REPO/scripts/build_proxy.sh" > /tmp/pkg_proxy.log 2>&1 || {
    iconv -f GBK -t UTF-8 -c /tmp/pkg_proxy.log | tail -20; exit 1; }

[ -f "$REPO/build/version.dll" ]       || { echo "no version.dll"; exit 1; }
[ -f "$REPO/build/mgpu_capture.dll" ]  || { echo "no mgpu_capture.dll"; exit 1; }

echo "[3/4] assembling $OUT"
rm -rf "$OUT"; mkdir -p "$OUT"
cp "$REPO/build/version.dll" "$REPO/build/mgpu_capture.dll" "$OUT/"

python3 - "$REPO" "$OUT" <<'PYEOF'
import io, os, re, sys

repo, out = sys.argv[1], sys.argv[2]

# The survey build wants everything visible and nothing interfering:
#   Color/Ui on   -> discover whether the game hands over HUDless/UI at all
#   FgCut off     -> never disable the game's own frame generation
#   StreamPercent -> 1/2 MV/depth, which keeps the cost near half an fps
SURVEY = (('Color', '1'), ('ColorFormat', '27'), ('Ui', '1'),
          ('FgCut', '0'), ('StreamPercent', '200'))

cap = io.open(repo + '/scripts/templates/mgpu_capture.ini', encoding='utf-8').read()
for key, val in SURVEY:
    cap, n = re.subn(r'(?m)^' + key + r'=\d+', key + '=' + val, cap)
    if n != 1:
        sys.exit('template has no single "%s=" line' % key)
cap = cap.replace('mgpu_capture.ini - game-side capture',
                  'mgpu_capture.ini - game-side capture (survey %s)' % os.path.basename(out))
px = io.open(repo + '/scripts/templates/mgpu_proxy.ini', encoding='utf-8').read()

def w(name, text):
    # The core reads the ini with GetPrivateProfile*A -> ANSI/GBK + CRLF.
    io.open(os.path.join(out, name), 'wb').write(
        text.replace('\n', '\r\n').encode('gbk', errors='replace'))

w('mgpu_capture.ini', cap)
w('mgpu_proxy.ini', px)
print('   inis written')
PYEOF

# README is kept in UTF-8 in the repo but shipped as GBK+CRLF, so it opens
# correctly in Notepad on a Chinese-locale Windows without any guessing.
iconv -f UTF-8 -t GBK -c "$REPO/packaging/survey/README.txt" | sed 's/$/\r/' > "$OUT/README.txt"
sed 's/\r$//; s/$/\r/' "$REPO/packaging/survey/collect_logs.bat" > "$OUT/collect_logs.bat"

echo "[4/4] zipping"
( cd "$REPO/build/pkg" && rm -f "mgpu_survey_$TAG.zip" "mgpu_survey_$TAG.zip.sha256" \
  && zip -qr "mgpu_survey_$TAG.zip" "mgpu_survey_$TAG" \
  && sha256sum "mgpu_survey_$TAG.zip" > "mgpu_survey_$TAG.zip.sha256" )

ls -la "$REPO/build/pkg/mgpu_survey_$TAG.zip"
cat "$REPO/build/pkg/mgpu_survey_$TAG.zip.sha256"
