// Host-test stand-in for common/thread.h: records priority requests instead of applying them.
#pragma once
#include <atomic>
#include "common/common_types.h"

namespace Common {
enum class ThreadPriority : u32 { Low = 0, Normal = 1, High = 2, VeryHigh = 3, Critical = 4 };
inline std::atomic<int> g_priority_calls{0};
inline std::atomic<u32> g_last_priority{99};
inline void SetCurrentThreadPriority(ThreadPriority p) {
    ++g_priority_calls;
    g_last_priority = static_cast<u32>(p);
}
inline void SetCurrentThreadName(const char*) {}
} // namespace Common
