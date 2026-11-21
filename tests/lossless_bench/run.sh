#!/usr/bin/env bash
# 无损块码流的参考实现：构建 + 回环自检（+ 可选：在真实 dump 上量真实比率）。
#   tests/lossless_bench/run.sh                 # 合成用例 + 300 组 fuzz
#   tests/lossless_bench/run.sh DIR [DIR...]    # 额外在这些 --dump 目录上跑真实帧
#
# 对应设计文档：docs/design/LOSSLESS_TRANSPORT.md §6.1 / §8（gate 1）。
set -euo pipefail
cd "$(dirname "$0")/../.."
out="${TMPDIR:-/tmp}/mgpu_fr_codec_test"
g++ -std=c++17 -O2 -Wall -Wextra -Itests/lossless_bench \
    tests/lossless_bench/fr_codec_test.cpp -o "$out"
"$out" --self

for d in "$@"; do
    echo
    echo "== $d"
    "$out" "$d"/*.raw || true          # 跳过 non-raw（目录里通常还有 *_meta.txt）
done
