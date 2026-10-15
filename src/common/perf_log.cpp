// Copyright 2026 Azahar GT6 fork
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <ctime>
#include <exception>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>
#include <fmt/format.h>
#if defined(__linux__) || defined(__ANDROID__)
#include <sched.h>
#endif
#include "common/file_util.h"
#include "common/logging/log.h"
#include "common/perf_log.h"
#include "common/thread.h"

namespace Common::PerfLog {

namespace detail {
std::atomic<bool> g_enabled{false};
}

namespace {

constexpr std::size_t RingSize = std::size_t{1} << 14; // must be a power of two
constexpr std::size_t MaxPendingEvents = 4096;
constexpr u64 MaxFileBytes = 200ull * 1024 * 1024;
constexpr auto WriterInterval = std::chrono::milliseconds(250);
constexpr auto HeaderTimeout = std::chrono::seconds(3);
constexpr float Unknown = std::numeric_limits<float>::quiet_NaN();

/// One record per frame. Plain data so the emulation thread only has to copy it into the ring.
struct Row {
    s64 t_us;
    float frame_ms;
    float work_ms;
    float sleep_ms;
    float speed_pct;
    float refresh_hz;
    float vsync_ms;
    float headroom;
    float battery_c;
    s32 mode_id;
    s32 cpu;
    s32 thermal;
};

struct EventRecord {
    s64 t_us;
    std::string text;
};

struct Line {
    s64 t_us;
    std::string text;
};

s64 NowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

int CurrentCpu() {
#if defined(__linux__) || defined(__ANDROID__)
    return sched_getcpu();
#else
    return -1;
#endif
}

/// Events end up inside a CSV field: no separators or line breaks allowed.
std::string Sanitize(std::string_view text) {
    std::string out{text};
    std::replace_if(
        out.begin(), out.end(), [](char c) { return c == ',' || c == '\n' || c == '\r'; }, ';');
    return out;
}

void AppendFloat(std::string& out, double value, int precision) {
    if (std::isfinite(value)) {
        fmt::format_to(std::back_inserter(out), "{:.{}f}", value, precision);
    }
}

void AppendInt(std::string& out, s32 value) {
    if (value >= 0) {
        fmt::format_to(std::back_inserter(out), "{}", value);
    }
}

void AppendRow(std::string& out, const Row& r) {
    AppendFloat(out, r.t_us / 1000.0, 3);
    out += ',';
    AppendFloat(out, r.frame_ms, 3);
    out += ',';
    AppendFloat(out, r.work_ms, 3);
    out += ',';
    AppendFloat(out, r.sleep_ms, 3);
    out += ',';
    AppendFloat(out, r.speed_pct, 1);
    out += ',';
    AppendFloat(out, r.refresh_hz, 2);
    out += ',';
    AppendInt(out, r.mode_id);
    out += ',';
    AppendFloat(out, r.vsync_ms, 3);
    out += ',';
    AppendInt(out, r.cpu);
    out += ',';
    AppendInt(out, r.thermal);
    out += ',';
    AppendFloat(out, r.headroom, 2);
    out += ',';
    AppendFloat(out, r.battery_c, 1);
    out += ",\n"; // empty event field
}

void AppendEvent(std::string& out, const EventRecord& e) {
    // t_ms, then 11 empty numeric fields, then the event name: 13 fields in total.
    fmt::format_to(std::back_inserter(out), "{:.3f}", e.t_us / 1000.0);
    out.append(12, ',');
    out += e.text;
    out += '\n';
}

constexpr const char* CsvHeader =
    "t_ms,frame_ms,work_ms,sleep_ms,speed_pct,refresh_hz,mode_id,vsync_ms,cpu,thermal,"
    "headroom,batt_c,event\n";

struct State {
    // Begin/End
    std::mutex lifecycle_mutex;
    std::thread writer;
    std::mutex wake_mutex;
    std::condition_variable wake_cv;
    bool stop = false; // guarded by wake_mutex

    // Output (writer thread only while a session is active)
    std::unique_ptr<FileUtil::IOFile> file;
    std::string start_stamp;
    bool header_written = false;
    bool write_failed = false;
    u64 bytes_written = 0;

    // Header, produced by the emulation thread on the first frame
    HeaderProvider header_provider;
    std::mutex header_mutex;
    std::string header_text;
    std::atomic<bool> header_ready{false};

    // Ring buffer: single producer (emulation thread), single consumer (writer thread)
    std::unique_ptr<Row[]> ring;
    std::atomic<std::size_t> ring_head{0};
    std::atomic<std::size_t> ring_tail{0};
    std::atomic<u64> dropped_rows{0};

    // Events: any thread
    std::mutex event_mutex;
    std::vector<EventRecord> events;
    std::atomic<u64> dropped_events{0};

    std::atomic<s64> origin_us{0};

    // Latest state fed in by the UI thread
    std::atomic<float> refresh_hz{Unknown};
    std::atomic<float> vsync_ms{Unknown};
    std::atomic<s32> mode_id{-1};
    std::atomic<s32> thermal{-1};
    std::atomic<float> headroom{Unknown};
    std::atomic<float> battery_c{Unknown};

    // Emulation thread only
    std::atomic<bool> reset_pending{false};
    bool primed = false;
    s64 ready_us = 0;
    s64 last_done_us = 0;
    s64 last_emu_us = 0;

    void Write(std::string_view text) {
        if (write_failed || text.empty() || !file) {
            return;
        }
        bytes_written += text.size();
        if (bytes_written > MaxFileBytes) {
            write_failed = true;
            LOG_WARNING(Common, "Performance log reached its size limit, no further data written");
            return;
        }
        if (file->WriteString(text) != text.size()) {
            write_failed = true;
            LOG_ERROR(Common, "Failed to write the performance log, no further data written");
        }
    }

    void WriteHeader() {
        std::string out;
        out += fmt::format("# azahar-gt6 performance log, started {}\n", start_stamp);
        if (header_ready.load(std::memory_order_acquire)) {
            std::string text;
            {
                std::scoped_lock lock{header_mutex};
                text = header_text;
            }
            std::size_t pos = 0;
            while (pos < text.size()) {
                const std::size_t end = text.find('\n', pos);
                const std::size_t len = (end == std::string::npos ? text.size() : end) - pos;
                out += "# ";
                out.append(text, pos, len);
                out += '\n';
                pos += len + 1;
            }
        } else {
            out += "# (no frame was rendered, so no settings snapshot is available)\n";
        }
        out += CsvHeader;
        Write(out);
        header_written = true;
    }

    /// Moves everything queued so far to the file. `final` also flushes a pending header.
    void Drain(bool final) {
        if (!header_written) {
            const bool ready = header_ready.load(std::memory_order_acquire);
            const bool timed_out =
                NowUs() - origin_us.load(std::memory_order_relaxed) >=
                std::chrono::duration_cast<std::chrono::microseconds>(HeaderTimeout).count();
            if (!ready && !timed_out && !final) {
                // No frame has been rendered yet. Keep events queued so the header stays first.
                return;
            }
            WriteHeader();
        }

        std::vector<Line> lines;

        std::size_t tail = ring_tail.load(std::memory_order_relaxed);
        const std::size_t head = ring_head.load(std::memory_order_acquire);
        lines.reserve(head - tail);
        for (; tail != head; ++tail) {
            const Row& row = ring[tail & (RingSize - 1)];
            Line line{row.t_us, {}};
            AppendRow(line.text, row);
            lines.push_back(std::move(line));
        }
        ring_tail.store(tail, std::memory_order_release);

        std::vector<EventRecord> pending;
        {
            std::scoped_lock lock{event_mutex};
            pending.swap(events);
        }
        for (const EventRecord& e : pending) {
            Line line{e.t_us, {}};
            AppendEvent(line.text, e);
            lines.push_back(std::move(line));
        }

        std::stable_sort(lines.begin(), lines.end(),
                         [](const Line& a, const Line& b) { return a.t_us < b.t_us; });

        std::string out;
        for (const Line& line : lines) {
            out += line.text;
        }
        Write(out);
        if (file && !write_failed) {
            file->Flush();
        }
    }
};

State& S() {
    // Intentionally leaked: a joinable std::thread member must never be destroyed by exit().
    static State* const state = new State;
    return *state;
}

void WriterMain(State& s) {
    Common::SetCurrentThreadName("PerfLogWriter");
    std::unique_lock lock{s.wake_mutex};
    while (!s.stop) {
        s.wake_cv.wait_for(lock, WriterInterval, [&s] { return s.stop; });
        lock.unlock();
        try {
            s.Drain(false);
        } catch (const std::exception& e) {
            // This is a diagnostic tool: it must never take the game down with it.
            LOG_ERROR(Common, "Performance log writer failed: {}", e.what());
            s.write_failed = true;
        }
        lock.lock();
    }
    lock.unlock();
    try {
        s.Drain(true);
    } catch (const std::exception& e) {
        LOG_ERROR(Common, "Performance log final flush failed: {}", e.what());
        s.write_failed = true;
    }
}

} // namespace

bool Begin(const std::string& dir, HeaderProvider header_provider) {
    State& s = S();
    std::scoped_lock lifecycle{s.lifecycle_mutex};
    if (IsEnabled()) {
        return true;
    }

    std::tm local_time{};
    const std::time_t now = std::time(nullptr);
#ifdef _WIN32
    localtime_s(&local_time, &now);
#else
    localtime_r(&now, &local_time);
#endif
    char stamp[32]{};
    std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &local_time);

    std::string path = dir;
    if (!path.empty() && path.back() != '/' && path.back() != '\\') {
        path += '/';
    }
    path += fmt::format("perf_{}.csv", stamp);

    auto file = std::make_unique<FileUtil::IOFile>(path, "w");
    if (!file->IsGood()) {
        LOG_ERROR(Common, "Could not create performance log {}", path);
        return false;
    }
    LOG_INFO(Common, "Performance log: {}", path);

    s.file = std::move(file);
    s.start_stamp = stamp;
    s.header_provider = std::move(header_provider);
    s.header_written = false;
    s.write_failed = false;
    s.bytes_written = 0;
    s.header_ready.store(false, std::memory_order_relaxed);
    s.header_text.clear();
    s.ring = std::make_unique<Row[]>(RingSize);
    s.ring_head.store(0, std::memory_order_relaxed);
    s.ring_tail.store(0, std::memory_order_relaxed);
    s.dropped_rows.store(0, std::memory_order_relaxed);
    s.dropped_events.store(0, std::memory_order_relaxed);
    {
        std::scoped_lock lock{s.event_mutex};
        s.events.clear();
    }
    s.origin_us.store(NowUs(), std::memory_order_relaxed);
    s.reset_pending.store(false, std::memory_order_relaxed);
    s.primed = false;
    {
        std::scoped_lock lock{s.wake_mutex};
        s.stop = false;
    }
    s.writer = std::thread(WriterMain, std::ref(s));

    detail::g_enabled.store(true, std::memory_order_release);
    return true;
}

void End() {
    State& s = S();
    std::scoped_lock lifecycle{s.lifecycle_mutex};
    if (!IsEnabled()) {
        return;
    }
    detail::g_enabled.store(false, std::memory_order_release);
    {
        std::scoped_lock lock{s.wake_mutex};
        s.stop = true;
    }
    s.wake_cv.notify_all();
    if (s.writer.joinable()) {
        s.writer.join();
    }
    s.Write(fmt::format("# dropped_rows={} dropped_events={}\n",
                        s.dropped_rows.load(std::memory_order_relaxed),
                        s.dropped_events.load(std::memory_order_relaxed)));
    if (s.file) {
        s.file->Flush();
        s.file->Close();
        s.file.reset();
    }
    s.ring.reset();
    s.header_provider = nullptr;
}

void Event(std::string_view name) {
    if (!IsEnabled()) {
        return;
    }
    State& s = S();
    const s64 t = NowUs() - s.origin_us.load(std::memory_order_relaxed);
    std::scoped_lock lock{s.event_mutex};
    if (s.events.size() >= MaxPendingEvents) {
        s.dropped_events.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    s.events.push_back(EventRecord{t, Sanitize(name)});
}

void SetDisplayState(float refresh_hz, int mode_id, float vsync_ms) {
    State& s = S();
    s.refresh_hz.store(refresh_hz, std::memory_order_relaxed);
    s.mode_id.store(mode_id, std::memory_order_relaxed);
    s.vsync_ms.store(vsync_ms, std::memory_order_relaxed);
}

void SetThermalState(int status, float headroom, float battery_celsius) {
    State& s = S();
    s.thermal.store(status, std::memory_order_relaxed);
    s.headroom.store(headroom, std::memory_order_relaxed);
    s.battery_c.store(battery_celsius, std::memory_order_relaxed);
}

void ResetFrameTiming() {
    S().reset_pending.store(true, std::memory_order_relaxed);
}

void FrameReady() {
    if (!IsEnabled()) {
        return;
    }
    S().ready_us = NowUs();
}

void FrameDone(s64 emulated_time_us) {
    if (!IsEnabled()) {
        return;
    }
    State& s = S();
    const s64 now = NowUs();

    if (!s.primed || s.reset_pending.exchange(false, std::memory_order_relaxed)) {
        // First frame of the session, or the first one after a pause/resume: there is no
        // meaningful interval to report yet.
        if (!s.primed && s.header_provider) {
            std::string text;
            try {
                text = s.header_provider();
            } catch (const std::exception& e) {
                text = fmt::format("header provider failed: {}", e.what());
            }
            {
                std::scoped_lock lock{s.header_mutex};
                s.header_text = std::move(text);
            }
            s.header_ready.store(true, std::memory_order_release);
        }
        s.primed = true;
        s.last_done_us = now;
        s.last_emu_us = emulated_time_us;
        return;
    }

    const s64 frame_us = now - s.last_done_us;
    const s64 work_us = std::max<s64>(s.ready_us - s.last_done_us, 0);
    const s64 sleep_us = std::max<s64>(now - s.ready_us, 0);
    const double speed =
        frame_us > 0 ? static_cast<double>(emulated_time_us - s.last_emu_us) * 100.0 / frame_us
                     : 0.0;

    Row row{};
    row.t_us = now - s.origin_us.load(std::memory_order_relaxed);
    row.frame_ms = static_cast<float>(frame_us / 1000.0);
    row.work_ms = static_cast<float>(work_us / 1000.0);
    row.sleep_ms = static_cast<float>(sleep_us / 1000.0);
    row.speed_pct = static_cast<float>(speed);
    row.refresh_hz = s.refresh_hz.load(std::memory_order_relaxed);
    row.vsync_ms = s.vsync_ms.load(std::memory_order_relaxed);
    row.headroom = s.headroom.load(std::memory_order_relaxed);
    row.battery_c = s.battery_c.load(std::memory_order_relaxed);
    row.mode_id = s.mode_id.load(std::memory_order_relaxed);
    row.cpu = CurrentCpu();
    row.thermal = s.thermal.load(std::memory_order_relaxed);

    s.last_done_us = now;
    s.last_emu_us = emulated_time_us;

    if (!s.ring) {
        return;
    }
    const std::size_t head = s.ring_head.load(std::memory_order_relaxed);
    if (head - s.ring_tail.load(std::memory_order_acquire) >= RingSize) {
        s.dropped_rows.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    s.ring[head & (RingSize - 1)] = row;
    s.ring_head.store(head + 1, std::memory_order_release);
}

} // namespace Common::PerfLog
