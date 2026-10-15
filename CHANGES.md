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
| `ci` | `gt6-android.yml`: Android-only, signs with repo secrets, uploads `azahar-gt6-<short-sha>` | Phone-only workflow: the APK comes from Actions. Never publishes an APK signed with the debug key. |
| `ci` | `"!gt6"` added to the push branches of `build.yml` and `libretro.yml` | Both upstream workflows otherwise build on every push to every branch. Confirmed on GitHub: only `gt6-android` and the cheap `format` check run. |
| `common`, `video_core`, `android` | Performance Log (see below) | Needed to find the cause of the jitter bug with data instead of guesses. Off by default. |
| `tools/gt6` | Termux helper `gt6` + host test for the logger | Collects what the app cannot see (CPU frequencies, SurfaceFlinger, thermal service, crash buffer) and automates stability tests. |

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
