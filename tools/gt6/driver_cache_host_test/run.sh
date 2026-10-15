#!/usr/bin/env bash
# Builds the real vk_driver_cache_footer.h against xxHash and exercises it.
# Usage: tools/gt6/driver_cache_host_test/run.sh [address|undefined|none]
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
san="${1:-none}"
out="$(mktemp -d)"
flags=(-std=c++20 -O1 -g -Wall -Wextra -I"$root/src" -I"$root/externals/xxHash")
[ "$san" != none ] && flags+=("-fsanitize=$san" -fno-omit-frame-pointer)
gcc -O1 -c -I"$root/externals/xxHash" "$root/externals/xxHash/xxhash.c" -o "$out/xxhash.o"
g++ "${flags[@]}" "$here/test_main.cpp" "$out/xxhash.o" -o "$out/test"
"$out/test"
echo "DRIVER CACHE FOOTER TESTS PASSED (sanitizer: $san)"
