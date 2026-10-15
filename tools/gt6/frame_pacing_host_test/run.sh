#!/usr/bin/env bash
# Builds the real src/common/frame_pacing.cpp (and perf_log.cpp, which it reports to) against
# host stubs and exercises it.   Usage: run.sh [thread|address|undefined|none]
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
san="${1:-none}"
out="$(mktemp -d)"
flags=(-std=c++20 -O1 -g -Wall -Wextra -DFMT_HEADER_ONLY -I"$here/stubs" -I"$root/src" -I"$root/externals/fmt/include" -pthread)
[ "$san" != none ] && flags+=("-fsanitize=$san" -fno-omit-frame-pointer)
g++ "${flags[@]}" "$here/test_main.cpp" "$root/src/common/frame_pacing.cpp" "$root/src/common/perf_log.cpp" -o "$out/test"
"$out/test" 2>&1 | grep -v '^\[[IW]\]' || true
"$out/test" >/dev/null 2>&1
echo "FRAME PACING TESTS PASSED (sanitizer: $san)"
