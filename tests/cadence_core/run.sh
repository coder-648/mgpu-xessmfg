#!/usr/bin/env bash
# 动态逐帧控制器（任意上限）的纯计算仿真：构建 + 运行。
#   tests/cadence_core/run.sh
set -euo pipefail
cd "$(dirname "$0")/../.."
out="${TMPDIR:-/tmp}/mgpu_cadence_sim"
g++ -std=c++17 -O2 -Wall -Wextra tests/cadence_core/sim.cpp -o "$out"
"$out"
