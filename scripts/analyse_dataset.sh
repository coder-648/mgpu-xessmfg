#!/usr/bin/env bash
# 分析 capture_dataset.ps1 抓下来的数据集（在 WSL / Linux 里跑）。
#
#   bash scripts/analyse_dataset.sh <dataset-dir>            # 所有 seg-*
#   bash scripts/analyse_dataset.sh <dataset-dir>/seg-street # 单个 segment
#   LIMIT=8 bash scripts/analyse_dataset.sh <dir>            # 只抽 8 帧（快速看一遍）
#
# 三件事，按顺序：
#   1. scripts/lossless_ratio.py   快速一维近似 + VERDICT（纯 python，无依赖）
#   2. tests/lossless_bench/run.sh 精确码流（g++ 编一次，含 bit-exact 回环）
#   3. scripts/analyse_dump.py     配对 / MV 语义（需要 numpy，没有就跳过）
#
# 结果同时写到 <dataset-dir>/report.txt。
set -uo pipefail
cd "$(dirname "$0")/.."

root="${1:-}"
if [ -z "$root" ] || [ ! -d "$root" ]; then
    echo "usage: bash scripts/analyse_dataset.sh <dataset-dir>" >&2
    exit 2
fi

segs=()
if compgen -G "$root/*.raw" >/dev/null; then
    segs=( "$root" )                      # 直接给了一个 segment 目录
else
    for d in "$root"/seg-*; do [ -d "$d" ] && segs+=( "$d" ); done
fi
if [ ${#segs[@]} -eq 0 ]; then
    echo "no seg-* (or *.raw) under $root" >&2
    exit 2
fi

report="$root/report.txt"        # 数据集根目录或单个 seg 目录都写在自己下面

ratio() {
    if [ -n "${LIMIT:-}" ]; then
        python3 scripts/lossless_ratio.py "$1" --frames "$LIMIT"
    else
        python3 scripts/lossless_ratio.py "$1"
    fi
}

run() {
    echo "=============================================================="
    echo "## $1"
    echo "=============================================================="
    shift
    "$@" || echo "  (failed: $* -- see the message above)"
    echo
}

tmpd="$(mktemp -d)"
# 精确码流 + 汇聚成 p5/p50/p95（gate 1 的判据就是这些分位数）
codec() {
    local out="$tmpd/$(basename "$1").codec.txt"
    bash tests/lossless_bench/run.sh "$1" 2>&1 | tee "$out"
    echo "---- aggregate (exact codec) ----"
    python3 tests/lossless_bench/ratio_stats.py "$out"
    echo
}

{
    echo "dataset : $root"
    echo "date    : $(date -Is)"
    echo "codec   : $(git --no-pager log --oneline -1 2>/dev/null || echo 'n/a')"
    echo

    for seg in "${segs[@]}"; do
        [ -d "$seg" ] || continue
        echo
        echo "##############################################################"
        echo "# $seg"
        echo "##############################################################"
        run "ratio estimate (1-D, fast)" ratio "$seg"
        run "exact fixed-rate codec (gate 1 truth)" codec "$seg"
        if python3 -c 'import numpy' 2>/dev/null; then
            run "pairing / MV semantics" python3 scripts/analyse_dump.py "$seg"
        else
            echo "## pairing / MV semantics: skipped (no numpy: pip install numpy)"
            echo
        fi
    done
} 2>&1 | tee "$report"

rm -rf "$tmpd"
echo "report -> $report"
