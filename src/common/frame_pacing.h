// Copyright 2026 Azahar GT6 fork
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

#pragma once

#include <chrono>
#include <string>

/**
 * CPU scheduling help for the frame-critical threads (GT6 fork).
 *
 * Two independent, individually switchable measures:
 *  - raise_priority: lower the nice value of the emulation, Vulkan worker and present threads.
 *  - performance_hints: report each frame's actual work duration to the Android Dynamic
 *    Performance Framework (ADPF, API 33+, looked up at run time because minSdk is 29), so the
 *    system can pick CPU frequencies and cores for the frame deadline.
 *
 * Everything degrades to a no-op where unsupported, and every failure is logged.
 */
namespace Common::FramePacing {

struct Options {
    bool raise_priority = true;
    bool performance_hints = true;
};

/// Call before the emulation thread starts. Safe to call again between games.
void Configure(const Options& options);

[[nodiscard]] Options GetOptions();

/// Call once at the start of every frame-critical thread. Registers the thread for the hint
/// session and, if enabled, raises its priority. `role` is only used for logging.
void OnThreadStart(const char* role);

/// Emulation thread, once per frame: FrameWorkBegin right after the frame limiter returned,
/// FrameWorkEnd right before it waits again. The difference is reported as the frame's work.
void FrameWorkBegin();
void FrameWorkEnd();

/// Emulation thread, when emulation stops: closes the hint session.
void EndSession();

/// One line describing what is active, for the Performance Log header.
[[nodiscard]] std::string Describe();

} // namespace Common::FramePacing
