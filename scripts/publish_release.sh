#!/usr/bin/env bash
# ============================================================================
#  发布 lock_frame 到 GitHub Releases（本地包已在 dist/ 里时用）
#
#  前置：tag 已推送到 origin，dist/lock_frame-<ver>.zip 与
#        dist/lock_frame-<ver>-src.zip 已生成（见 lock_frame/release.sh）。
#
#  用法：
#      GITHUB_TOKEN=ghp_xxx ./scripts/publish_release.sh 1.2.7
#      GITHUB_TOKEN=ghp_xxx ./scripts/publish_release.sh 1.2.7 --update
#      GITHUB_TOKEN=ghp_xxx ./scripts/publish_release.sh 1.2.7 --title "lock_frame 1.2.7：..."
#      ./scripts/publish_release.sh 1.2.7 --dry-run     # 只打印将要做什么
#
#  说明：
#    * token 需要 repo 权限（创建 release / 上传资产）。
#    * release 名称与说明取自 lock_frame/docs/RELEASE_NOTES_<ver>.md。
#    * 资产名与以往一致：lock_frame-<ver>.zip、lock_frame-<ver>-src.zip。
#    * 默认 release 已存在时直接失败；--update 只补传缺失资产，不改正文。
# ============================================================================
set -euo pipefail

REPO_SLUG="${REPO_SLUG:-coder-648/mgpu-xessmfg}"
API="https://api.github.com/repos/$REPO_SLUG"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

say()  { printf '[publish] %s\n' "$*"; }
warn() { printf '[publish] WARN: %s\n' "$*" >&2; }
die()  { printf '[publish] ERROR: %s\n' "$*" >&2; exit 1; }

VER=""
TITLE=""
UPDATE=0
DRY_RUN=0
expect_value=0
for arg in "$@"; do
    if [ "$expect_value" = 1 ]; then TITLE="$arg"; expect_value=0; continue; fi
    case "$arg" in
        --update)  UPDATE=1 ;;
        --dry-run) DRY_RUN=1 ;;
        --title)   expect_value=1 ;;
        -h|--help)
            sed -n '2,21p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        -*)
            die "unknown option: $arg" ;;
        *)
            [ -z "$VER" ] || die "only one version argument is accepted"
            VER="$arg" ;;
    esac
done
[ "$expect_value" = 0 ] || die "--title needs a value"

if [ -z "$VER" ]; then
    VER="$(sed -n 's/^#define LOCK_FRAME_VERSION_STRING "\(.*\)"/\1/p' \
           "$ROOT/lock_frame/version.h" | head -1)"
fi
[ -n "$VER" ] || die "cannot read version (pass it as the first argument)"
TAG="v$VER"

ZIP="$ROOT/dist/lock_frame-$VER.zip"
SRC_ZIP="$ROOT/dist/lock_frame-$VER-src.zip"
NOTES="$ROOT/lock_frame/docs/RELEASE_NOTES_$VER.md"

[ -f "$ZIP" ] || die "missing $ZIP (run lock_frame/release.sh)"
[ -f "$SRC_ZIP" ] || die "missing $SRC_ZIP (run lock_frame/release.sh)"
[ -f "$NOTES" ] || die "missing $NOTES"

# release 标题：默认取 RELEASE_NOTES 的一级标题（去掉日期后缀），可用 --title 覆盖。
if [ -z "$TITLE" ]; then
    TITLE="$(sed -n '1s/^# //p' "$NOTES" | sed 's/[[:space:]]*[（(][0-9-]*[)）][[:space:]]*$//')"
fi
[ -n "$TITLE" ] || TITLE="lock_frame $VER"

say "version  : $VER (tag $TAG)"
say "title    : $TITLE"
say "notes    : $NOTES ($(wc -c <"$NOTES") bytes)"
say "assets   : $(basename "$ZIP") ($(du -h "$ZIP" | cut -f1)), $(basename "$SRC_ZIP") ($(du -h "$SRC_ZIP" | cut -f1))"

# tag 必须已经在远端，否则 release 会指向一个提交之外的 tag。
if [ "$DRY_RUN" = 1 ]; then
    say "dry-run: would check tag $TAG on origin, create the release, upload 2 assets"
    exit 0
fi

TOKEN="${GITHUB_TOKEN:-${GH_TOKEN:-}}"
[ -n "$TOKEN" ] || die "set GITHUB_TOKEN (repo scope) - no gh CLI needed"

api() { # api <method> <url> [extra curl args...]
    local method="$1" url="$2"; shift 2
    curl -sS -m 120 -X "$method" \
        -H "Authorization: Bearer $TOKEN" \
        -H "Accept: application/vnd.github+json" \
        -H "X-GitHub-Api-Version: 2022-11-28" \
        "$@" "$url"
}

json_get() { # json_get <field> ;  stdin = JSON
    python3 -c 'import json,sys; d=json.load(sys.stdin); print(d.get(sys.argv[1], "") or "")' "$1"
}

say "checking tag $TAG on origin"
git -C "$ROOT" ls-remote --tags origin "refs/tags/$TAG" | grep -q "refs/tags/$TAG" \
    || die "tag $TAG is not pushed to origin (git push origin $TAG)"

RELEASE_ID=""
EXISTING="$(api GET "$API/releases/tags/$TAG")"
EXISTING_ID="$(printf '%s' "$EXISTING" | json_get id 2>/dev/null || true)"
if [ -n "$EXISTING_ID" ]; then
    RELEASE_ID="$EXISTING_ID"
    if [ "$UPDATE" = 0 ]; then
        die "release $TAG already exists (id $RELEASE_ID); re-run with --update"
    fi
    say "release exists (id $RELEASE_ID), adding missing assets"
else
    say "creating release $TAG"
    PAYLOAD="$(mktemp)"
    trap 'rm -f "$PAYLOAD"' EXIT
    python3 - "$TAG" "$TITLE" "$NOTES" >"$PAYLOAD" <<'PYJSON'
import json, sys
tag, title, notes_path = sys.argv[1:4]
with open(notes_path, encoding="utf-8") as f:
    body = f.read()
print(json.dumps({
    "tag_name": tag,
    "target_commitish": "main",
    "name": title,
    "body": body,
    "draft": False,
    "prerelease": False,
}, ensure_ascii=False))
PYJSON
    RESP="$(api POST "$API/releases" \
        -H 'Content-Type: application/json' \
        --data-binary @"$PAYLOAD")"
    RELEASE_ID="$(printf '%s' "$RESP" | json_get id 2>/dev/null || true)"
    [ -n "$RELEASE_ID" ] || die "create release failed: $RESP"
    say "created release id $RELEASE_ID"
fi

UPLOAD_URL="https://uploads.github.com/repos/$REPO_SLUG/releases/$RELEASE_ID/assets"
EXISTING_ASSETS="$(api GET "$API/releases/$RELEASE_ID/assets?per_page=100")"

for f in "$ZIP" "$SRC_ZIP"; do
    name="$(basename "$f")"
    if printf '%s' "$EXISTING_ASSETS" | grep -q "\"name\": *\"$name\""; then
        say "asset $name already uploaded - skip"
        continue
    fi
    say "uploading $name ($(du -h "$f" | cut -f1))"
    RESP="$(api POST "$UPLOAD_URL?name=$name" \
        -H 'Content-Type: application/zip' \
        --data-binary @"$f")"
    URL="$(printf '%s' "$RESP" | json_get browser_download_url)"
    [ -n "$URL" ] || die "upload $name failed: $RESP"
    say "  -> $URL"
done

say "done: https://github.com/$REPO_SLUG/releases/tag/$TAG"
