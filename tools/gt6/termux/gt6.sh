#!/data/data/com.termux/files/usr/bin/bash
# shellcheck disable=SC2016 # the LOOP_* strings are scripts for the phone, intentionally unexpanded
# gt6 - Termux helper for testing Azahar GT6 over the phone's own wireless-debugging adb.
#
# Run `gt6 help`. No root and no computer needed.
#
# Every on-device command is best effort: one that is missing or refused on your ROM is
# recorded in the output and skipped; it never stops the logger. Nothing here modifies the
# phone except `corrupt-cache` (which backs up first) and the app-lifecycle commands.

set -u

GT6_HOME="${GT6_HOME:-$HOME/gt6}"
PKG="${GT6_PKG:-org.azahar_emu.azahar.gt6}"
OUT_ROOT="$GT6_HOME/out"
ADB="${ADB:-adb}"
DOWNLOADS="${GT6_DOWNLOADS:-$HOME/storage/downloads}"

die() {
    echo "gt6: $*" >&2
    exit 1
}

stamp() {
    date +%Y%m%d_%H%M%S
}

need_adb() {
    command -v "$ADB" >/dev/null 2>&1 || die "adb not found. Run: pkg install android-tools"
    [ "$("$ADB" get-state 2>/dev/null)" = "device" ] ||
        die "no device connected. Run 'gt6 help' and follow the pairing steps."
}

# Runs a command on the phone, never failing the caller.
sh_dev() {
    # </dev/null: adb would otherwise swallow the stdin of any surrounding `while read` loop
    "$ADB" shell "$@" 2>&1 </dev/null || true
}

current_dir() {
    [ -f "$OUT_ROOT/current" ] || return 1
    cat "$OUT_ROOT/current"
}

# ---------------------------------------------------------------------------------------
# pairing
# ---------------------------------------------------------------------------------------
cmd_pair() {
    [ $# -eq 2 ] || die "usage: gt6 pair <pairing-port> <6-digit-code>"
    command -v "$ADB" >/dev/null 2>&1 || die "adb not found. Run: pkg install android-tools"
    "$ADB" pair "localhost:$1" "$2"
}

cmd_connect() {
    [ $# -eq 1 ] || die "usage: gt6 connect <wireless-debugging-port>"
    command -v "$ADB" >/dev/null 2>&1 || die "adb not found. Run: pkg install android-tools"
    "$ADB" connect "localhost:$1"
    "$ADB" devices
}

# ---------------------------------------------------------------------------------------
# metadata
# ---------------------------------------------------------------------------------------
write_meta() {
    echo "date_local=$(date '+%Y-%m-%d %H:%M:%S %z')"
    echo "date_epoch=$(date +%s)"
    echo "package=$PKG"
    for prop in ro.product.model ro.product.device ro.build.version.release \
        ro.build.version.sdk ro.build.display.id ro.build.fingerprint ro.soc.model \
        ro.hardware.vulkan; do
        echo "$prop=$(sh_dev getprop "$prop" | tr -d '\r')"
    done
    echo "peak_refresh_rate=$(sh_dev settings get system peak_refresh_rate | tr -d '\r')"
    echo "min_refresh_rate=$(sh_dev settings get system min_refresh_rate | tr -d '\r')"
    echo "low_power=$(sh_dev settings get global low_power | tr -d '\r')"
    echo "--- package"
    sh_dev "dumpsys package $PKG | grep -E 'versionName|versionCode|firstInstallTime|lastUpdateTime'"
    echo "--- battery"
    sh_dev "dumpsys battery | head -n 20"
}

# ---------------------------------------------------------------------------------------
# background logger: one adb process per stream, each running a loop on the phone
# ---------------------------------------------------------------------------------------
LOOP_CPUFREQ='
while true; do
  l="$(date +%s) $(cut -d" " -f1 /proc/uptime)"
  for p in /sys/devices/system/cpu/cpufreq/policy*; do
    l="$l $(basename $p)=$(cat $p/scaling_cur_freq 2>/dev/null)"
  done
  echo "$l"
  sleep 1
done'

LOOP_THERMAL_ZONES='
while true; do
  t=$(date +%s)
  for z in /sys/class/thermal/thermal_zone*; do
    echo "$t $(cat $z/type 2>/dev/null) $(cat $z/temp 2>/dev/null)"
  done
  sleep 5
done'

LOOP_DUMPSYS='
i=0
while true; do
  t=$(date +%s)
  echo "=== thermalservice $t"; dumpsys thermalservice 2>&1
  if [ $((i % 3)) -eq 0 ]; then echo "=== display $t"; dumpsys display 2>&1; fi
  if [ $((i % 6)) -eq 0 ]; then
    echo "=== surfaceflinger $t"; dumpsys SurfaceFlinger 2>&1 | head -c 400000
  fi
  pid=$(pidof __PKG__ 2>/dev/null)
  if [ -n "$pid" ]; then
    echo "=== threads $t pid=$pid"
    ps -T -p $pid -o TID,PSR,PCPU,NAME 2>&1 | head -n 80
  fi
  i=$((i + 1))
  sleep 10
done'

# Presented-frame timestamps as seen by SurfaceFlinger (about 2 s of history per call)
LOOP_LATENCY='
layer=""
while true; do
  t=$(date +%s)
  if [ -z "$layer" ]; then
    layer=$(dumpsys SurfaceFlinger --list 2>/dev/null | grep -F "__PKG__" | grep -F "SurfaceView" | head -n 1)
  fi
  if [ -n "$layer" ]; then
    echo "=== latency $t $layer"
    dumpsys SurfaceFlinger --latency "$layer" 2>&1
  else
    echo "=== latency $t (no SurfaceView layer for __PKG__ yet)"
  fi
  sleep 2
done'

start_stream() {
    # start_stream <name> <loop-script>
    local name="$1" script="${2//__PKG__/$PKG}"
    nohup "$ADB" shell "$script" >"$RUN_DIR/$name.log" 2>&1 &
    echo $! >"$RUN_DIR/pid.$name"
}

cmd_log_start() {
    need_adb
    if current_dir >/dev/null 2>&1; then
        echo "A logger is already marked as running; stopping it first."
        cmd_log_stop
    fi
    local label="${1:-run}"
    RUN_DIR="$OUT_ROOT/$(stamp)_${label}"
    mkdir -p "$RUN_DIR"
    echo "$RUN_DIR" >"$OUT_ROOT/current"
    echo "$RUN_DIR" >"$OUT_ROOT/last"
    write_meta >"$RUN_DIR/meta_start.txt"
    command -v termux-wake-lock >/dev/null 2>&1 && termux-wake-lock

    nohup "$ADB" logcat -b crash -v threadtime >"$RUN_DIR/crash.log" 2>&1 &
    echo $! >"$RUN_DIR/pid.crash"
    start_stream cpufreq "$LOOP_CPUFREQ"
    start_stream thermal_zones "$LOOP_THERMAL_ZONES"
    start_stream dumpsys "$LOOP_DUMPSYS"
    start_stream latency "$LOOP_LATENCY"

    echo "Logger running -> $RUN_DIR"
    echo "Now go and play. When you are done: gt6 log stop"
    echo "(Keep the Termux notification alive; do not swipe Termux away.)"
}

cmd_log_stop() {
    local dir
    dir="$(current_dir)" || die "no logger is running"
    local f
    for f in "$dir"/pid.*; do
        [ -f "$f" ] || continue
        kill "$(cat "$f")" 2>/dev/null || true
        rm -f "$f"
    done
    if "$ADB" get-state >/dev/null 2>&1; then
        write_meta >"$dir/meta_end.txt"
        snapshot_into "$dir" end
    fi
    command -v termux-wake-unlock >/dev/null 2>&1 && termux-wake-unlock
    rm -f "$OUT_ROOT/current"
    echo "Logger stopped. Collected in $dir"
    echo "Next: gt6 pack   (bundles everything, plus the app's own log files, for upload)"
}

# ---------------------------------------------------------------------------------------
# one-shot snapshot
# ---------------------------------------------------------------------------------------
snapshot_into() {
    local dir="$1" label="${2:-snap}"
    local s
    s="$dir/snap_$(stamp)_$label"
    mkdir -p "$s"
    sh_dev dumpsys display >"$s/display.txt"
    sh_dev dumpsys SurfaceFlinger >"$s/surfaceflinger.txt"
    sh_dev dumpsys thermalservice >"$s/thermalservice.txt"
    sh_dev dumpsys activity exit-info "$PKG" >"$s/exit_info.txt"
    sh_dev dumpsys meminfo "$PKG" >"$s/meminfo.txt"
    echo "snapshot -> $s"
}

cmd_snap() {
    need_adb
    local dir
    dir="$(current_dir 2>/dev/null)" || dir="$OUT_ROOT/adhoc_$(stamp)"
    mkdir -p "$dir"
    snapshot_into "$dir" "${1:-snap}"
}

# ---------------------------------------------------------------------------------------
# crash data (ask for the app's own log first; this is the second source)
# ---------------------------------------------------------------------------------------
cmd_crash() {
    need_adb
    local dir
    dir="$OUT_ROOT/crash_$(stamp)"
    mkdir -p "$dir"
    "$ADB" logcat -b crash -d -v threadtime >"$dir/crash_buffer.log" 2>&1 || true
    sh_dev dumpsys activity exit-info "$PKG" >"$dir/exit_info.txt"
    "$ADB" logcat -b main,system -d -t 4000 -v threadtime 2>&1 |
        grep -E "$PKG|FATAL|ANR in|am_crash|am_anr|AndroidRuntime|DEBUG|tombstone" \
            >"$dir/main_filtered.log" || true
    echo "crash data -> $dir"
    echo "Lines in crash buffer: $(wc -l <"$dir/crash_buffer.log")"
}

# ---------------------------------------------------------------------------------------
# stability automation
# ---------------------------------------------------------------------------------------
alive() {
    [ -n "$(sh_dev pidof "$PKG" | tr -d '\r ')" ]
}

launch() {
    sh_dev monkey -p "$PKG" -c android.intent.category.LAUNCHER 1 >/dev/null
}

cmd_monkey() {
    need_adb
    local seed="${1:-1}" count="${2:-3000}"
    local out
    out="$OUT_ROOT/monkey_seed${seed}_$(stamp).log"
    mkdir -p "$OUT_ROOT"
    echo "monkey seed=$seed events=$count on $PKG (open the app to the menus first)"
    "$ADB" shell monkey -p "$PKG" -s "$seed" --throttle 250 --ignore-security-exceptions \
        --monitor-native-crashes -v "$count" >"$out" 2>&1 || true
    grep -E "CRASH|NOT RESPONDING|ANR|Monkey finished|Exception" "$out" | tail -n 20
    if grep -qE "CRASH|NOT RESPONDING|ANR in" "$out"; then
        echo "RESULT seed=$seed: PROBLEM FOUND - see $out"
    else
        echo "RESULT seed=$seed: no crash/ANR reported - $out"
    fi
}

cmd_lifecycle() {
    need_adb
    local loops="${1:-5}" i fails=0
    local out
    out="$OUT_ROOT/lifecycle_$(stamp).log"
    mkdir -p "$OUT_ROOT"
    "$ADB" logcat -b crash -c >/dev/null 2>&1 || true
    check() {
        # check <loop> <step>
        if alive; then
            echo "loop $1 $2: alive" >>"$out"
        else
            echo "loop $1 $2: PROCESS GONE" | tee -a "$out"
            fails=$((fails + 1))
            launch
            sleep 6
        fi
    }
    echo "lifecycle x$loops on $PKG (start a game or sit in the menus first)"
    for i in $(seq 1 "$loops"); do
        sh_dev input keyevent KEYCODE_HOME >/dev/null
        sleep 3
        launch
        sleep 5
        check "$i" home-resume

        sh_dev input keyevent KEYCODE_SLEEP >/dev/null
        sleep 4
        sh_dev input keyevent KEYCODE_WAKEUP >/dev/null
        sleep 1
        sh_dev input keyevent KEYCODE_MENU >/dev/null
        sleep 4
        check "$i" screen-off-on

        sh_dev am send-trim-memory "$PKG" RUNNING_CRITICAL >/dev/null
        sleep 3
        check "$i" trim-memory

        sh_dev input keyevent KEYCODE_HOME >/dev/null
        sleep 3
        sh_dev am kill "$PKG" >/dev/null
        sleep 2
        launch
        sleep 6
        check "$i" background-kill-relaunch
    done
    {
        echo "--- crash buffer after the run"
        "$ADB" logcat -b crash -d -v threadtime 2>&1 || true
    } >>"$out"
    echo "RESULT lifecycle: $fails unexpected process deaths in $loops loops - $out"
}

# ---------------------------------------------------------------------------------------
# shader cache corruption test (backs up first, reversible)
# ---------------------------------------------------------------------------------------
find_shader_dir() {
    if [ -n "${GT6_USERDIR:-}" ]; then
        echo "$GT6_USERDIR/shaders/vulkan"
        return
    fi
    sh_dev "find /sdcard -maxdepth 7 -type d -path '*/shaders/vulkan' 2>/dev/null" |
        tr -d '\r' | grep -F "shaders/vulkan" | head -n 5
}

cmd_corrupt_cache() {
    need_adb
    local dirs
    dirs="$(find_shader_dir)"
    [ -n "$dirs" ] || die "could not find shaders/vulkan on /sdcard. Set GT6_USERDIR to your Azahar user folder, e.g. GT6_USERDIR=/sdcard/azahar-emu gt6 corrupt-cache"
    if [ "$(echo "$dirs" | wc -l)" -gt 1 ]; then
        echo "Several candidates:"
        echo "$dirs"
        die "set GT6_USERDIR to the right user folder and run again"
    fi
    local backup
    backup="/sdcard/Download/gt6-shader-backup/$(stamp)"
    echo "Shader cache : $dirs"
    echo "Backup to    : $backup (restore with: gt6 restore-cache '$backup' '$dirs')"
    sh_dev "mkdir -p '$backup' && cp -r '$dirs'/. '$backup'/"
    sh_dev "find '$dirs' -type f" | tr -d '\r' | while read -r f; do
        [ -n "$f" ] || continue
        sh_dev "s=\$(stat -c %s '$f'); head -c \$((s / 2)) '$f' > '$f.gt6tmp' && mv '$f.gt6tmp' '$f'; echo truncated '$f' \$s '->' \$((s / 2))"
    done
    echo "Done. Now start a game. The app must start normally and rebuild the cache."
    echo "Afterwards run: gt6 crash"
}

cmd_restore_cache() {
    need_adb
    [ $# -eq 2 ] || die "usage: gt6 restore-cache <backup-dir> <shaders/vulkan-dir>"
    sh_dev "rm -rf '$2'/* && cp -r '$1'/. '$2'/"
    echo "restored $1 -> $2"
}

# ---------------------------------------------------------------------------------------
# CPU feature line (runs inside Termux, no adb needed)
# ---------------------------------------------------------------------------------------
cmd_cpuinfo() {
    mkdir -p "$GT6_HOME"
    local out="$GT6_HOME/cpuinfo.txt"
    {
        echo "--- Features (first core)"
        grep -m1 '^Features' /proc/cpuinfo
        echo "--- CPU part per core"
        grep -E '^(processor|CPU part)' /proc/cpuinfo | paste - -
        echo "--- features of the last core"
        grep '^Features' /proc/cpuinfo | tail -n 1
    } >"$out"
    cat "$out"
    echo "saved to $out"
}

# ---------------------------------------------------------------------------------------
# bundle everything for upload
# ---------------------------------------------------------------------------------------
cmd_pack() {
    # gt6 pack [run-folder-name]   (default: the most recent `gt6 log start` run)
    local dir
    if [ $# -ge 1 ]; then
        dir="$OUT_ROOT/$1"
    elif [ -f "$OUT_ROOT/last" ]; then
        dir="$(cat "$OUT_ROOT/last")"
    fi
    [ -n "${dir:-}" ] && [ -d "$dir" ] || die "nothing to pack yet: run 'gt6 log start' first"
    mkdir -p "$dir/app"
    if "$ADB" get-state >/dev/null 2>&1; then
        local f
        for f in $(sh_dev "find /sdcard -maxdepth 8 \( -name 'perf_*.csv' -o -name 'azahar_log.txt' -o -name 'azahar_log.old.txt' \) 2>/dev/null" | tr -d '\r'); do
            "$ADB" pull "$f" "$dir/app/" >/dev/null 2>&1 &&
                echo "pulled $f" || echo "could not pull $f"
        done
    else
        echo "adb not connected: skipping the app's own files (perf_*.csv, azahar_log.txt)."
        echo "Upload those by hand from <Azahar user folder>/log/."
    fi
    local name
    name="gt6-$(basename "$dir").tar.gz"
    tar czf "$GT6_HOME/$name" -C "$OUT_ROOT" "$(basename "$dir")"
    if [ -d "$DOWNLOADS" ]; then
        cp "$GT6_HOME/$name" "$DOWNLOADS/$name"
        echo "READY: Downloads/$name  ($(du -h "$GT6_HOME/$name" | cut -f1))"
    else
        echo "READY: $GT6_HOME/$name"
        echo "Downloads folder not available. Run once: termux-setup-storage   then: gt6 pack"
    fi
}

cmd_help() {
    cat <<EOF
gt6 - Azahar GT6 test helper (package: $PKG; override with GT6_PKG=...)

ONE-TIME SETUP (phone: Settings > About > tap Build number 7x > Developer options)
  1. Developer options > turn on "Wireless debugging" (needs Wi-Fi connected).
  2. Tap "Pair device with pairing code": note the 6-digit code and the pairing port.
       gt6 pair <pairing-port> <code>
  3. Back on the Wireless debugging screen note the main "IP address & Port" port:
       gt6 connect <port>
     (the port changes whenever wireless debugging is switched off/on or Wi-Fi changes)

DURING A TEST
  gt6 log start <label>   start background logging (CPU frequencies, thermals, display, SF)
  gt6 log stop            stop, take final dumps
  gt6 pack [folder]       bundle the last run + the app's perf CSV and log into Download/
  gt6 snap [label]        one-off dumps (display, SurfaceFlinger, thermal, exit-info)

CRASH / STABILITY
  gt6 crash               crash buffer + exit-info + filtered main log
  gt6 monkey <seed> [events]
  gt6 lifecycle [loops]
  gt6 corrupt-cache       truncate the Vulkan shader cache (backs up first)
  gt6 restore-cache <backup-dir> <shaders/vulkan-dir>

OTHER
  gt6 cpuinfo             CPU Features line (needed before choosing compiler flags)
EOF
}

main() {
    local c="${1:-help}"
    shift || true
    mkdir -p "$OUT_ROOT"
    case "$c" in
    pair) cmd_pair "$@" ;;
    connect) cmd_connect "$@" ;;
    log)
        case "${1:-}" in
        start) shift; cmd_log_start "$@" ;;
        stop) cmd_log_stop ;;
        *) die "usage: gt6 log start [label] | gt6 log stop" ;;
        esac
        ;;
    snap) cmd_snap "$@" ;;
    crash) cmd_crash ;;
    monkey) cmd_monkey "$@" ;;
    lifecycle) cmd_lifecycle "$@" ;;
    corrupt-cache) cmd_corrupt_cache ;;
    restore-cache) cmd_restore_cache "$@" ;;
    cpuinfo) cmd_cpuinfo ;;
    pack) cmd_pack "$@" ;;
    help | -h | --help) cmd_help ;;
    *) die "unknown command '$c' (try: gt6 help)" ;;
    esac
}

main "$@"
