// Copyright 2026 Azahar GT6 fork
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <string_view>
#include "common/common_types.h"

/**
 * Optional per-frame performance log (GT6 fork).
 *
 * When a session is active the emulation thread appends one fixed-size record per frame to a
 * lock-free ring buffer and a background thread formats and writes them to a CSV file, so the
 * emulation thread never blocks on I/O. When no session is active every entry point costs one
 * relaxed atomic load.
 *
 * Threading: Begin/End and FrameReady/FrameDone must all be called from the emulation thread.
 * Event and the Set*State functions may be called from any thread.
 */
namespace Common::PerfLog {

namespace detail {
extern std::atomic<bool> g_enabled;
}

/// True while a session is active. Cheap enough to call every frame.
[[nodiscard]] inline bool IsEnabled() noexcept {
    return detail::g_enabled.load(std::memory_order_relaxed);
}

/// Called once, from the emulation thread, when the first frame is finished. Its result is
/// written as '#'-prefixed comment lines at the top of the file (settings, device info, ...).
using HeaderProvider = std::function<std::string()>;

/// Opens "<dir>perf_<local time>.csv" and starts the writer thread. Returns false and stays
/// disabled if the file cannot be created. Does nothing if a session is already active.
bool Begin(const std::string& dir, HeaderProvider header_provider);

/// Flushes everything and closes the file. Does nothing if no session is active.
void End();

/// Records a named event ("MARK", "surface_changed", ...) with its own timestamp.
void Event(std::string_view name);

/// Latest display state, copied into every frame record. Pass NaN / -1 for unknown values.
void SetDisplayState(float refresh_hz, int mode_id, float vsync_ms);
void SetThermalState(int status, float headroom, float battery_celsius);

/// Drop the interval measurement for the next frame (call after a pause/resume).
void ResetFrameTiming();

/// Called right before the frame limiter waits, and right after it returns.
void FrameReady();
void FrameDone(s64 emulated_time_us);

} // namespace Common::PerfLog
