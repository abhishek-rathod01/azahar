// Copyright 2026 Azahar GT6 fork
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

#pragma once

#include <cstddef>
#include <cstring>
#include <span>
#include "common/common_types.h"
#include "common/hash.h"

namespace Vulkan {

/// The driver pipeline cache blob is stored followed by this footer. A file that was cut short
/// (the process was killed while writing, storage ran out, ...) fails the size/hash check and is
/// rebuilt, instead of being handed to the GPU driver, which is not required to cope with a
/// truncated blob.
struct DriverCacheFooter {
    u32 magic;
    u32 version;
    u64 data_size;
    u64 data_hash;
};
static_assert(sizeof(DriverCacheFooter) == 24);

constexpr u32 DRIVER_CACHE_MAGIC = 0x43505A41; // "AZPC"
constexpr u32 DRIVER_CACHE_VERSION = 1;

inline DriverCacheFooter MakeDriverCacheFooter(std::span<const u8> data) {
    return DriverCacheFooter{
        .magic = DRIVER_CACHE_MAGIC,
        .version = DRIVER_CACHE_VERSION,
        .data_size = data.size(),
        .data_hash = Common::ComputeHash64(data.data(), data.size()),
    };
}

/// Returns the driver blob inside a cache file image, or an empty span if the footer is missing
/// or does not match the data (corrupt, truncated, or written by an older build).
inline std::span<const u8> ExtractDriverCacheData(std::span<const u8> file) {
    if (file.size() < sizeof(DriverCacheFooter)) {
        return {};
    }
    const std::size_t data_size = file.size() - sizeof(DriverCacheFooter);
    DriverCacheFooter footer;
    std::memcpy(&footer, file.data() + data_size, sizeof(footer));
    if (footer.magic != DRIVER_CACHE_MAGIC || footer.version != DRIVER_CACHE_VERSION ||
        footer.data_size != data_size) {
        return {};
    }
    if (footer.data_hash != Common::ComputeHash64(file.data(), data_size)) {
        return {};
    }
    return file.first(data_size);
}

} // namespace Vulkan
