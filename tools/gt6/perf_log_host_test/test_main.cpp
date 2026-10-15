// Host-side exercise of src/common/perf_log.cpp (the real, unmodified file).
// Usage: test_main <scenario> <output_dir>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include "common/perf_log.h"

using namespace std::chrono_literals;
namespace PL = Common::PerfLog;

static long long g_emu_us = 0;

static void RunFrames(int n, std::chrono::microseconds work, std::chrono::microseconds sleep) {
    for (int i = 0; i < n; ++i) {
        std::this_thread::sleep_for(work);
        PL::FrameReady();
        std::this_thread::sleep_for(sleep);
        g_emu_us += 16715; // ~59.83 Hz of emulated time per frame
        PL::FrameDone(g_emu_us);
    }
}

int main(int argc, char** argv) {
    if (argc < 3) {
        return 2;
    }
    const std::string scenario = argv[1];
    const std::string dir = argv[2];

    if (scenario == "disabled") {
        // Nothing must happen, nothing must crash, IsEnabled stays false.
        PL::FrameReady();
        PL::FrameDone(1);
        PL::Event("ignored");
        PL::SetDisplayState(60.f, 1, 16.6f);
        PL::End();
        std::printf("enabled=%d\n", PL::IsEnabled() ? 1 : 0);
        return PL::IsEnabled() ? 1 : 0;
    }

    if (scenario == "badpath") {
        const bool ok = PL::Begin(dir + "/does/not/exist", [] { return std::string(); });
        std::printf("begin_ok=%d enabled=%d\n", ok ? 1 : 0, PL::IsEnabled() ? 1 : 0);
        return (ok || PL::IsEnabled()) ? 1 : 0;
    }

    if (!PL::Begin(dir, [] { return std::string("graphics_api=Vulkan\nvsync=0\nline with, comma"); })) {
        return 3;
    }

    if (scenario == "basic") {
        PL::Event("surface_changed");
        PL::SetDisplayState(60.0f, 2, 16.67f);
        PL::SetThermalState(1, 0.85f, 31.5f);
        RunFrames(5, 2ms, 14ms);
        // Events from other threads while frames are being produced
        std::thread t([] {
            for (int i = 0; i < 20; ++i) {
                PL::Event(i == 10 ? "MARK" : "worker,event\nwith junk");
                std::this_thread::sleep_for(3ms);
            }
        });
        RunFrames(120, 3ms, 13ms);
        t.join();
        PL::SetDisplayState(120.0f, 3, 8.33f); // display mode drift mid-session
        PL::Event("display_mode_changed");
        RunFrames(30, 3ms, 13ms);
        PL::Event("emu_pause");
        std::this_thread::sleep_for(500ms); // a pause: huge gap that must NOT become a frame
        PL::ResetFrameTiming();
        PL::Event("emu_unpause");
        RunFrames(30, 3ms, 13ms);
        PL::SetThermalState(-1, std::nanf(""), std::nanf("")); // unknown values -> empty fields
        RunFrames(10, 3ms, 13ms);
    } else if (scenario == "overflow") {
        // Producer much faster than the writer can drain: rows must be dropped, never blocked.
        RunFrames(200000, 0us, 0us);
    } else if (scenario == "noframes") {
        PL::Event("only_event");
        std::this_thread::sleep_for(3500ms); // longer than the header timeout
    }
    PL::End();
    PL::End(); // double End must be harmless
    std::printf("done enabled=%d\n", PL::IsEnabled() ? 1 : 0);
    return 0;
}
