// Copyright 2026 Azahar GT6 fork
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>
#include <fmt/format.h>
#if defined(__linux__) || defined(__ANDROID__)
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif
#ifdef __ANDROID__
#include <dlfcn.h>
#endif
#include "common/frame_pacing.h"
#include "common/logging/log.h"
#include "common/perf_log.h"
#include "common/thread.h"

namespace Common::FramePacing {

namespace {

/// About 75 % of one 3DS frame (1 / 59.83 s). Work is reported against a target with headroom so
/// the system does not clock down so far that an ordinary load spike misses the frame.
[[maybe_unused]] constexpr std::chrono::nanoseconds TargetWork{12'500'000};

/// After this many failed reports in a row the hint session is given up on.
[[maybe_unused]] constexpr int MaxReportFailures = 60;

std::atomic<bool> g_raise_priority{true};
std::atomic<bool> g_performance_hints{true};

std::mutex g_threads_mutex;
std::vector<std::int32_t> g_threads;

std::int32_t CurrentTid() {
#if defined(__linux__) || defined(__ANDROID__)
    return static_cast<std::int32_t>(::syscall(SYS_gettid));
#else
    return -1;
#endif
}

[[maybe_unused]] bool ThreadAlive(std::int32_t tid) {
#if defined(__linux__) || defined(__ANDROID__)
    return ::access(fmt::format("/proc/self/task/{}", tid).c_str(), F_OK) == 0;
#else
    return false;
#endif
}

int CurrentNice() {
#if defined(__linux__) || defined(__ANDROID__)
    return ::getpriority(PRIO_PROCESS, 0);
#else
    return 0;
#endif
}

#ifdef __ANDROID__
using GetManagerFn = void* (*)();
using CreateSessionFn = void* (*)(void* manager, const std::int32_t* thread_ids, std::size_t size,
                                  std::int64_t initial_target_work_duration_nanos);
using ReportFn = int (*)(void* session, std::int64_t actual_duration_nanos);
using CloseFn = void (*)(void* session);
#endif

enum class HintState : int {
    NotStarted,
    Active,
    Unavailable,
};

/// ADPF session state. Only the emulation thread touches it, apart from Describe(), which is
/// called from the same thread.
struct Hints {
    std::atomic<int> state{static_cast<int>(HintState::NotStarted)};
    void* session = nullptr;
#ifdef __ANDROID__
    ReportFn report = nullptr;
    CloseFn close = nullptr;
#endif
    int failures = 0;
    std::chrono::steady_clock::time_point work_begin{};
    std::string reason;
};

Hints& H() {
    // Intentionally leaked so nothing runs at exit.
    static Hints* const hints = new Hints;
    return *hints;
}

bool CreateSession(Hints& h) {
#ifdef __ANDROID__
    // minSdk is 29 but the hint API is API 33, so look the functions up instead of linking them.
    void* const lib = ::dlopen("libandroid.so", RTLD_NOW);
    if (lib == nullptr) {
        h.reason = "libandroid.so could not be opened";
        return false;
    }
    const auto get_manager =
        reinterpret_cast<GetManagerFn>(::dlsym(lib, "APerformanceHint_getManager"));
    const auto create_session =
        reinterpret_cast<CreateSessionFn>(::dlsym(lib, "APerformanceHint_createSession"));
    h.report =
        reinterpret_cast<ReportFn>(::dlsym(lib, "APerformanceHint_reportActualWorkDuration"));
    h.close = reinterpret_cast<CloseFn>(::dlsym(lib, "APerformanceHint_closeSession"));
    if (get_manager == nullptr || create_session == nullptr || h.report == nullptr ||
        h.close == nullptr) {
        h.reason = "the ADPF functions are missing (they need Android 13)";
        return false;
    }
    void* const manager = get_manager();
    if (manager == nullptr) {
        h.reason = "no performance hint manager on this device";
        return false;
    }

    std::vector<std::int32_t> tids;
    {
        std::scoped_lock lock{g_threads_mutex};
        for (const std::int32_t tid : g_threads) {
            // A thread of an earlier renderer may be gone; a dead id makes the whole call fail
            if (ThreadAlive(tid)) {
                tids.push_back(tid);
            }
        }
    }
    const std::int32_t self = CurrentTid();
    if (std::find(tids.begin(), tids.end(), self) == tids.end()) {
        tids.push_back(self);
    }

    h.session = create_session(manager, tids.data(), tids.size(), TargetWork.count());
    if (h.session == nullptr) {
        h.reason = "the system refused to create a hint session";
        return false;
    }
    LOG_INFO(Common, "ADPF hint session created for {} threads, target {} ms", tids.size(),
             TargetWork.count() / 1'000'000.0);
    if (PerfLog::IsEnabled()) {
        PerfLog::Event(fmt::format("adpf_session_created threads={}", tids.size()));
    }
    return true;
#else
    h.reason = "not running on Android";
    return false;
#endif
}

void CloseSession(Hints& h) {
#ifdef __ANDROID__
    if (h.session != nullptr && h.close != nullptr) {
        h.close(h.session);
    }
#endif
    h.session = nullptr;
}

} // namespace

void Configure(const Options& options) {
    g_raise_priority.store(options.raise_priority, std::memory_order_relaxed);
    g_performance_hints.store(options.performance_hints, std::memory_order_relaxed);
}

Options GetOptions() {
    return Options{
        .raise_priority = g_raise_priority.load(std::memory_order_relaxed),
        .performance_hints = g_performance_hints.load(std::memory_order_relaxed),
    };
}

void OnThreadStart(const char* role) {
    const std::int32_t tid = CurrentTid();
    if (tid >= 0) {
        std::scoped_lock lock{g_threads_mutex};
        if (std::find(g_threads.begin(), g_threads.end(), tid) == g_threads.end()) {
            g_threads.push_back(tid);
        }
    }
    if (!g_raise_priority.load(std::memory_order_relaxed)) {
        return;
    }
    const int before = CurrentNice();
    SetCurrentThreadPriority(ThreadPriority::High);
    const int after = CurrentNice();
    if (after < before) {
        LOG_INFO(Common, "{} thread (tid {}): nice {} -> {}", role, tid, before, after);
    } else {
        LOG_WARNING(Common, "{} thread (tid {}): priority could not be raised (nice stays {})",
                    role, tid, after);
    }
    if (PerfLog::IsEnabled()) {
        PerfLog::Event(fmt::format("priority_{} nice={}->{}", role, before, after));
    }
}

void FrameWorkBegin() {
    if (!g_performance_hints.load(std::memory_order_relaxed)) {
        return;
    }
    H().work_begin = std::chrono::steady_clock::now();
}

void FrameWorkEnd() {
    if (!g_performance_hints.load(std::memory_order_relaxed)) {
        return;
    }
    Hints& h = H();
    if (h.work_begin == std::chrono::steady_clock::time_point{}) {
        return;
    }
    const auto work = std::chrono::steady_clock::now() - h.work_begin;
    h.work_begin = {};

    const int state = h.state.load(std::memory_order_relaxed);
    if (state == static_cast<int>(HintState::Unavailable)) {
        return;
    }
    if (state == static_cast<int>(HintState::NotStarted)) {
        if (!CreateSession(h)) {
            h.state.store(static_cast<int>(HintState::Unavailable), std::memory_order_relaxed);
            LOG_WARNING(Common, "Performance hints unavailable: {}", h.reason);
            if (PerfLog::IsEnabled()) {
                PerfLog::Event("adpf_unavailable");
            }
            return;
        }
        h.state.store(static_cast<int>(HintState::Active), std::memory_order_relaxed);
    }

#ifdef __ANDROID__
    const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(work).count();
    if (h.report(h.session, nanos) != 0) {
        if (++h.failures >= MaxReportFailures) {
            h.reason = "reporting kept failing";
            CloseSession(h);
            h.state.store(static_cast<int>(HintState::Unavailable), std::memory_order_relaxed);
            LOG_WARNING(Common, "Performance hints given up: {}", h.reason);
        }
    } else {
        h.failures = 0;
    }
#else
    static_cast<void>(work);
#endif
}

void EndSession() {
    Hints& h = H();
    CloseSession(h);
    h.state.store(static_cast<int>(HintState::NotStarted), std::memory_order_relaxed);
    h.failures = 0;
    h.work_begin = {};
    h.reason.clear();
}

std::string Describe() {
    const Options options = GetOptions();
    const Hints& h = H();
    std::string adpf;
    if (!options.performance_hints) {
        adpf = "off";
    } else {
        switch (static_cast<HintState>(h.state.load(std::memory_order_relaxed))) {
        case HintState::NotStarted:
            adpf = "not_started";
            break;
        case HintState::Active:
            adpf = "active";
            break;
        case HintState::Unavailable:
            adpf = fmt::format("unavailable ({})", h.reason);
            break;
        }
    }
    return fmt::format("raise_priority={} performance_hints={} adpf={}", options.raise_priority,
                       options.performance_hints, adpf);
}

} // namespace Common::FramePacing
