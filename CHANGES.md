# Azahar GT6 — changes

A personal fork of [Azahar](https://github.com/azahar-emu/azahar) for one phone
(Realme GT 6, Snapdragon 8s Gen 3, Adreno 735, 120 Hz LTPO). Priorities in order:
**stable**, **smooth**, **faster**. A speed gain that costs stability or smoothness is reverted.

Base: upstream tag **2126.2** (`219d743`, 2026-10-08), the newest stable tag when the fork was
created. Branch: `gt6`. Every change is its own commit; upstream files are touched as little as
possible so that rebasing onto a newer tag stays cheap.

> **Status: work in progress.** No performance or jitter measurements have been taken yet, so
> there are no before/after numbers below. They are added only from raw files uploaded from the
> phone (see "Before / after").

## What changed and why

| Commit area | Change | Why |
|---|---|---|
| `android` | ABI filter is `arm64-v8a` only | Target phone is arm64; roughly halves native build time and APK size. |
| `android` | `applicationId` = `org.azahar_emu.azahar.gt6` | Installs next to the official app for A/B comparison. No provider/authority uses the id. |
| `android` | App label "Azahar Optimised" (APK file `azahar-optimised-<short-sha>.apk` inside the `azahar-gt6-<short-sha>` artifact) | Tell the two apps apart. The package id stays `org.azahar_emu.azahar.gt6`. |
| `ci` | `gt6-android.yml`: Android-only; uploads `azahar-gt6-<short-sha>[-testkey]`. With the three repo secrets it signs with the permanent release key (and fails if the result is debug-signed); without them it signs with a throw-away test key kept in the Actions cache. A manual run with *release* ticked also publishes a GitHub pre-release | Phone-only workflow: the APK comes from Actions, as a direct download from the Releases page. The test key keeps consecutive builds updatable while its cache entry lives (7 days idle); the permanent key needs the secrets. |
| `ci` | `gt6-publish.yml`: publishes the APK of an earlier `gt6-android` run as a pre-release without rebuilding (optional `target` input for the tag) | A rebuild gets a new version code (build time) and could break the update order. Observed: `gh release create` with the build token was refused (HTTP 403) in 3 of 3 attempts when the tag target was an older commit and succeeded in 2 of 2 attempts when the target was the branch tip; the cause is not established (3 and 2 attempts are too few to call it a rule). |
| `ci` | `"!gt6"` added to the push branches and `"!gt6-*"` to the tags of `build.yml` and `libretro.yml` | Both upstream workflows otherwise build on every push to every branch and on every tag. Branch part confirmed on GitHub: only `gt6-android` and the cheap `format` check run. |
| `common`, `video_core`, `android` | Performance Log (see below) | Needed to find the cause of the jitter bug with data instead of guesses. Off by default. |
| `tools/gt6` | Termux helper `gt6` + host test for the logger | Collects what the app cannot see (CPU frequencies, SurfaceFlinger, thermal service, crash buffer) and automates stability tests. |
| `tools/gt6` | `analyze_perf.py` (+11 unit tests) | Computes average FPS, p99, 1 % low, stutters and jitter episodes (p99 > 25 ms for > 2 s) from the logs; the tests found and fixed two of its own bugs before it saw real data. |
| `android` (F1) | Re-apply the 60 Hz display mode on resume and window focus, robust mode matching, `Surface.setFrameRate(60, FIXED_SOURCE, ALWAYS)` on each surface change (API 31+), and a display listener that re-asserts 60 Hz while emulating (max 5 times, 2 s apart) | H1: nothing re-asserted 60 Hz after the one call in `onCreate`; the old code matched the mode with `refreshRate == 60f` and silently did nothing if no mode matched exactly. Menus keep requesting the maximum rate. |
| `video_core` (F2) | Rebuild a stale swapchain on the current Android surface instead of waiting for a new one; rebuild a *suboptimal* one at most once per second | Upstream's recreate path waited for a **different** surface, which never comes when the same surface merely changed size, transform or display mode (split-screen resize), so presentation stalled until the surface was recreated, which is what the Settings round trip does. Not confirmed as the cause of the jitter. |
| `video_core` | `Swapchain::Destroy()` iterates the semaphore vectors instead of the stale `image_count` | After an early return in `Create()` the vectors are empty but `image_count` is not, so the next `Destroy()` read past the end (undefined behaviour). Upstream is exposed after any surface loss inside `Create()`. |
| `common`, `android` (F3) | `Common::SetCurrentThreadPriority` works on Android (nice values); the emulation, Vulkan worker and present threads ask for High (nice -2); each frame's work time is reported through ADPF (`dlsym`-loaded, API 33+). Two switches in Settings, Debug (default on) | The old function was never called and is a no-op for `SCHED_OTHER`. The switches let the same APK be compared with and without each measure. The log header and events record what was active. |
| `video_core` | Driver pipeline cache: integrity footer (size + XXH3), atomic save (temp file + rename), exception-safe read-back | The cache was written in place and validated by its 32-byte Vulkan header only, so a file cut short by a kill or full storage was handed to the GPU driver. Host tests cover every truncation point and single-bit flip. |
| `android` | `unPauseEmulation`, `stopEmulation` and `run()` take `paused_mutex` before `notify_all()` (`WakeEmulationThread()`) | The paused loop checks its predicate and blocks while holding that mutex; notifying without it can lose the wake-up and leave emulation asleep. **Hazard fix, not an observed bug**: a 60,000-attempt stress test never lost a wake-up with the old code either (the window is nanoseconds). |

### Performance Log

Settings → Debug → *Performance Log (GT6)*. Off by default; read when a game starts. When on,
the game writes `log/perf_<time>.csv` in the user folder, one row per emulated frame, plus
named events. Columns:

`t_ms, frame_ms, work_ms, sleep_ms, speed_pct, refresh_hz, mode_id, vsync_ms, cpu, thermal, headroom, batt_c, event`

* `frame_ms` is the time between two frame-limiter exits. It splits into `work_ms` (emulation and
  rendering) and `sleep_ms` (time the frame limiter waited). Slow `work_ms` points at CPU placement
  or thermals; erratic `sleep_ms` with normal `work_ms` points at pacing.
* `vsync_ms` is the real display cadence measured from the Choreographer callback, independent of
  what the display mode claims.
* Events: `surface_changed`, `surface_destroyed`, `swapchain_created` (with the Vulkan present
  mode), `swapchain_acquire_*` / `swapchain_present_*` (why a recreation was requested),
  `display_mode_changed`, `emu_pause`, `emu_unpause`, `fragment_pause`, `fragment_resume`,
  `window_focus_gained/lost`, `session_start/end`, `MARK`.
* A **MARK** button is drawn on the game screen while logging, so a jitter episode can be marked
  without leaving the game (leaving the game is what hides the bug).
* Header lines (`#`) record the *effective* settings and device info, so every log documents what
  it was captured with.
* The emulation thread only copies a fixed-size record into a lock-free ring buffer; a background
  thread formats and writes. Nothing on the emulation thread blocks, allocates or does I/O. When
  off, each hook costs one relaxed atomic load.

Verification of the logger so far: the native core (`src/common/perf_log.cpp`) is compiled
unmodified on a Linux host and exercised by `tools/gt6/perf_log_host_test/run.sh` (disabled,
unwritable path, a 193-frame run with cross-thread events and a pause, a 200k-frame overflow, a
no-frame run), with output validated by an independent Python parser, clean under
ThreadSanitizer, AddressSanitizer and UBSan. The Kotlin/JNI/Android side is checked with ktlint
and by CI compilation only; **it has not been run on a device yet**.

## Re-verification of the brief's facts (done against 2126.2)

The brief's facts were written from 2126.1. Each was re-checked in the 2126.2 source.

**Differences from the brief**

1. The newest stable tag is **2126.2**, not 2126.1 (2126.1.1 and 2126.1.2 also exist). The fork is
   based on 2126.2.
2. **Async shader compilation is OFF by default**, not ON. `Settings::async_shader_compilation`
   defaults to `false` in C++ (`settings.h`) and `ASYNC_SHADERS` to `false` in Kotlin
   (`BooleanSetting.kt`). The "(default) On" text in `default_ini.h` is documentation only: that
   file writes `key =` with an empty value, so `GetBoolean(..., default)` falls back to the C++
   default.
3. **Accurate shader multiplication** has conflicting defaults: C++ `true`, Kotlin `false`, and the
   ini template leaves it empty. Which one applies on a fresh install depends on whether the
   settings screen has written it. Not resolved by reading; the Performance Log header records the
   effective value.
4. 2126.2 changed `use_skip_duplicate_frames` to default **false** (C++ and Kotlin; it was true in
   2126.1).
5. 2126.2 routes the frame limiter's wait through `ProcessPendingWorkUntil(deadline)` (a condition
   variable wait that also services GDB/RPC requests) instead of `sleep_for`. On Android with no
   GDB stub and the RPC server off this waits until the deadline, equivalent to the old sleep.
   `Event::WaitUntil` clears the event on wake, so the loop cannot spin.
6. `libretro.yml` also builds on every push (the brief only mentions `build.yml`). Both are excluded.
7. A `DisplayListener` exists in `SecondaryDisplay.kt` (secondary screen only); nothing listens to
   the primary display's mode.
8. `RefreshRateUtil` matches the 60 Hz mode with an exact float comparison
   (`mode.refreshRate == 60f`) and silently returns if none matches.

**Confirmed unchanged**: Gradle 8.14.5, AGP 8.13.2, Kotlin 2.0.20, NDK 27.3.13750724,
compileSdk 35 / targetSdk 37 / minSdk 29, the signing environment variables, build types and
flavours, `abiFilter`, LTO default-on for release builds, `ENABLE_NATIVE_OPTIMIZATION` and
`CITRA_ADDRESS_SANITIZE` options, hardening flags (`-fstack-protector-strong`,
`-fstack-clash-protection`, `_FORTIFY_SOURCE=2`), the `NativeEmulation` thread, single-threaded
`System::RunLoop`, `SetCurrentThreadPriority` never called (and a no-op on POSIX), `VulkanWorker`
and `VulkanPresent` threads, the pipeline-cache worker pool size, vsync default off on Android,
present mode Immediate/Mailbox, `SCREEN_REFRESH_RATE`, `preferredDisplayModeId` set once by
`EmulationActivity`, no `Surface.setFrameRate`, `IsLowRefreshRate()` having no Android branch,
`appCategory="game"` with `game_mode_config.xml`, dynarmic using page tables.

**Not individually re-checked**: the master-semaphore wait thread; dynarmic's "default safe
optimisations".

**Found while reading, relevant to the jitter bug**: on Android the frame is presented from the
Choreographer callback (`NativeLibrary.doFrame()` → `TryPresenting()`), i.e. paced by the real
display refresh, while emulation is paced separately by the software frame limiter. And
`Swapchain::AcquireNextImage` already requests a swapchain recreation on `VK_SUBOPTIMAL_KHR`,
so part of fix F2 exists upstream.

## Upstream fixes considered and not taken

Looked at what landed on upstream `master` after the 2126.2 tag (the checkout was shallow, so this
is a review of the commits fetched on 2026-10-14, not an exhaustive history):

| Upstream commit | What | Decision |
|---|---|---|
| `f718556` android: fix closing a game launched from a home screen shortcut (#2658) | one-line extra-key typo | Not taken: not a crash, and shortcuts are not in the test plan. Trivial to add later. |
| `7ea9cee` android: fix black screen when relaunching a game from a shortcut (#2656) | relaunch handling plus a wait for the secondary-screen surface | Not taken: it makes **every** game start wait up to 2 s for a secondary-screen surface, which a phone may never get. Fixes a shortcut-only path. |
| `2061a99` android: reset turbo when a game starts (#2657) | behaviour fix | Not taken: not a stability fix. |
| `847d329` shader_jit: masked write optimizations (#2597) | performance | Candidate for the performance phase, to go through the 2 % gate. |
| `bfa7687`, `fdc7f7a` Vulkan rendering fixes (proctex LUT upload, blend min/max) | correctness | Not taken yet; take if a game shows the symptom. |

## Before / after

No measurements yet. Planned method: 3 × 5 min runs per build from the same in-game save with
5 min cooldowns, plus one 15 min sustained run per milestone; average FPS, 1 % low FPS, p99 frame
time and stutter count computed from the raw CSVs.

## Jitter bug: root cause and fix

Not yet diagnosed. Hypotheses (unconfirmed): H1 refresh rate drifts mid-game; H2 pacing drift
between the ~59.83 Hz limiter and the display-paced present path; H3 the emulation thread lands on
slow cores; H4 thermal throttling. The Performance Log and the Termux logger exist to separate
them.

## Known issues

See [KNOWN_ISSUES.md](KNOWN_ISSUES.md).

## How to rebuild

* CI: push to `gt6` (or run the *gt6-android* workflow manually). Needs repo secrets
  `ANDROID_KEYSTORE_B64`, `ANDROID_KEYSTORE_PASS`, `ANDROID_KEY_ALIAS`. Without them the
  workflow still compiles but publishes no APK. The APK is the artifact `azahar-gt6-<short-sha>`.
* Local: `cd src/android && ./gradlew assembleVanillaRelease` with JDK 17, NDK 27.3.13750724 and
  CMake 3.25+ (CI uses 3.30.3).
* Logger host test: `tools/gt6/perf_log_host_test/run.sh [thread|address|undefined|none]`.

## How to update to a newer upstream release

Fetch upstream tags, rebase `gt6` onto the newest stable tag, re-verify any facts above that the
update touched, rebuild through CI, and run the short regression round.
