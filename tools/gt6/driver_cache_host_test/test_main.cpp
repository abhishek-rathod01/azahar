// Host-side test of src/video_core/renderer_vulkan/vk_driver_cache_footer.h (the real header).
// Decides whether a driver pipeline cache file may be handed to the GPU driver, so it is tested
// exhaustively: every truncation point, every single-bit flip, appended data, old format files.
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>
#include "video_core/renderer_vulkan/vk_driver_cache_footer.h"

using namespace Vulkan;

static int g_failures = 0;
#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            std::printf("FAIL %s:%d: %s  ", __FILE__, __LINE__, #cond);                            \
            std::printf(__VA_ARGS__);                                                              \
            std::printf("\n");                                                                     \
        }                                                                                          \
    } while (0)

static std::vector<u8> MakeFile(const std::vector<u8>& blob) {
    const DriverCacheFooter footer = MakeDriverCacheFooter(blob);
    std::vector<u8> file(blob);
    file.resize(blob.size() + sizeof(footer));
    std::memcpy(file.data() + blob.size(), &footer, sizeof(footer));
    return file;
}

int main() {
    std::mt19937 rng(42);
    auto random_blob = [&](std::size_t n) {
        std::vector<u8> v(n);
        for (auto& b : v) {
            b = static_cast<u8>(rng());
        }
        return v;
    };

    // 1. round trip for many sizes
    int roundtrips = 0;
    for (std::size_t n : {1u, 2u, 31u, 32u, 33u, 255u, 1024u, 4096u, 100000u}) {
        const auto blob = random_blob(n);
        const auto file = MakeFile(blob);
        const auto out = ExtractDriverCacheData(file);
        CHECK(out.size() == blob.size(), "size %zu", n);
        CHECK(out.size() == blob.size() && std::memcmp(out.data(), blob.data(), n) == 0,
              "content %zu", n);
        ++roundtrips;
    }

    // 2. every truncation point of a 2 KiB blob must be rejected
    const auto blob = random_blob(2048);
    const auto file = MakeFile(blob);
    int truncations = 0;
    for (std::size_t keep = 0; keep < file.size(); ++keep) {
        const auto out = ExtractDriverCacheData(std::span<const u8>(file.data(), keep));
        CHECK(out.empty(), "truncated to %zu of %zu was accepted", keep, file.size());
        ++truncations;
    }

    // 3. every single-bit flip of a 256 byte blob (data and footer) must be rejected
    const auto small_file = MakeFile(random_blob(256));
    int flips = 0;
    for (std::size_t i = 0; i < small_file.size(); ++i) {
        for (int bit = 0; bit < 8; ++bit) {
            auto copy = small_file;
            copy[i] ^= static_cast<u8>(1u << bit);
            CHECK(ExtractDriverCacheData(copy).empty(), "flip byte %zu bit %d accepted", i, bit);
            ++flips;
        }
    }

    // 4. appended garbage, prepended garbage, old format (no footer), empty
    {
        auto extended = file;
        extended.push_back(0);
        CHECK(ExtractDriverCacheData(extended).empty(), "appended byte accepted");
        auto prefixed = file;
        prefixed.insert(prefixed.begin(), 0);
        CHECK(ExtractDriverCacheData(prefixed).empty(), "prepended byte accepted");
        CHECK(ExtractDriverCacheData(blob).empty(), "old format (no footer) accepted");
        CHECK(ExtractDriverCacheData({}).empty(), "empty accepted");
    }

    // 5. a footer that is internally consistent except for one field
    {
        auto bad_size = file;
        DriverCacheFooter f = MakeDriverCacheFooter(blob);
        f.data_size += 1;
        std::memcpy(bad_size.data() + blob.size(), &f, sizeof(f));
        CHECK(ExtractDriverCacheData(bad_size).empty(), "wrong size field accepted");
        auto bad_version = file;
        f = MakeDriverCacheFooter(blob);
        f.version += 1;
        std::memcpy(bad_version.data() + blob.size(), &f, sizeof(f));
        CHECK(ExtractDriverCacheData(bad_version).empty(), "wrong version accepted");
    }

    std::printf("roundtrips=%d truncations=%d bitflips=%d failures=%d\n", roundtrips, truncations,
                flips, g_failures);
    return g_failures == 0 ? 0 : 1;
}
