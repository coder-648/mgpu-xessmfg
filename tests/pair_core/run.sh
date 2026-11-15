#!/usr/bin/env bash
# 配对核心（精确 ID + ms12 校验 + ACK 语义）的纯计算仿真。
#   tests/pair_core/run.sh
set -euo pipefail
cd "$(dirname "$0")/../.."
out="${TMPDIR:-/tmp}/mgpu_pair_core_sim"
g++ -std=c++17 -O2 -Wall -Wextra tests/pair_core/sim.cpp -o "$out"
"$out"
