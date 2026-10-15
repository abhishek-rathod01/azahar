#!/usr/bin/env bash
# Builds the real src/common/perf_log.cpp against host stubs and exercises it.
# Usage: tools/gt6/perf_log_host_test/run.sh [thread|address|undefined|none]
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
san="${1:-none}"
out="$(mktemp -d)"
flags=(-std=c++20 -O1 -g -Wall -Wextra -DFMT_HEADER_ONLY -I"$here/stubs" -I"$root/src" -I"$root/externals/fmt/include" -pthread)
[ "$san" != none ] && flags+=("-fsanitize=$san" -fno-omit-frame-pointer)
g++ "${flags[@]}" "$here/test_main.cpp" "$root/src/common/perf_log.cpp" -o "$out/test"
for sc in disabled badpath basic overflow noframes; do
    d="$out/$sc"; mkdir -p "$d"
    "$out/test" "$sc" "$d"
    case "$sc" in basic|overflow|noframes) python3 -I "$here/check_csv.py" "$sc" "$d";; esac
done
echo "ALL SCENARIOS PASSED (sanitizer: $san)"
