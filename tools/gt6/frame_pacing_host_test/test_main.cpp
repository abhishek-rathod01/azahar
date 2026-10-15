// Host-side exercise of src/common/frame_pacing.cpp (the real, unmodified file).
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>
#include "common/frame_pacing.h"
#include "common/thread.h"

namespace FP = Common::FramePacing;

static int g_failures = 0;
#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            std::printf("FAIL line %d: %s  ", __LINE__, #cond);                                    \
            std::printf(__VA_ARGS__);                                                              \
            std::printf("\n");                                                                     \
        }                                                                                          \
    } while (0)

static bool Contains(const std::string& s, const char* part) {
    return s.find(part) != std::string::npos;
}

int main() {
    // defaults: both on
    {
        const auto o = FP::GetOptions();
        CHECK(o.raise_priority && o.performance_hints, "defaults should be on");
    }

    // raising priority asks for High, once per call
    Common::g_priority_calls = 0;
    FP::OnThreadStart("TestRole");
    CHECK(Common::g_priority_calls == 1, "calls=%d", Common::g_priority_calls.load());
    CHECK(Common::g_last_priority == static_cast<u32>(Common::ThreadPriority::High), "level");

    // switched off: no priority request
    FP::Configure({.raise_priority = false, .performance_hints = true});
    Common::g_priority_calls = 0;
    FP::OnThreadStart("TestRole");
    CHECK(Common::g_priority_calls == 0, "priority requested although off");
    CHECK(Contains(FP::Describe(), "raise_priority=false"), "%s", FP::Describe().c_str());

    // performance hints on a host: must degrade gracefully and say why
    FP::Configure({.raise_priority = true, .performance_hints = true});
    CHECK(Contains(FP::Describe(), "adpf=not_started"), "%s", FP::Describe().c_str());
    for (int i = 0; i < 5; ++i) {
        FP::FrameWorkBegin();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        FP::FrameWorkEnd();
    }
    CHECK(Contains(FP::Describe(), "adpf=unavailable (not running on Android)"), "%s",
          FP::Describe().c_str());

    // End without Begin, double End: harmless
    FP::FrameWorkEnd();
    FP::EndSession();
    FP::EndSession();
    CHECK(Contains(FP::Describe(), "adpf=not_started"), "state not reset: %s",
          FP::Describe().c_str());

    // hints off
    FP::Configure({.raise_priority = true, .performance_hints = false});
    FP::FrameWorkBegin();
    FP::FrameWorkEnd();
    CHECK(Contains(FP::Describe(), "adpf=off"), "%s", FP::Describe().c_str());

    // concurrent thread registration
    FP::Configure({.raise_priority = true, .performance_hints = true});
    Common::g_priority_calls = 0;
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([] {
            for (int i = 0; i < 100; ++i) {
                FP::OnThreadStart("Worker");
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }
    CHECK(Common::g_priority_calls == 800, "calls=%d", Common::g_priority_calls.load());

    std::printf("failures=%d\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
