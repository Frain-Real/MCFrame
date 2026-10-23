#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
build="${1:-$root/.build/coverage}"
cmake -S "$root" -B "$build" -DMCU_TARGET=mock -DCMAKE_BUILD_TYPE=Debug \
      -DMCFRAME_COVERAGE=ON
cmake --build "$build" --parallel
ctest --test-dir "$build" --output-on-failure
command -v gcovr >/dev/null || {
    echo "gcovr не установлен: sudo apt-get install gcovr" >&2
    exit 0
}
gcovr --root "$root" --object-directory "$build" \
      --exclude 'tests/.*' --exclude '.*/tests/.*' --print-summary
