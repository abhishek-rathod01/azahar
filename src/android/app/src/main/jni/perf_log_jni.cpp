// Copyright 2026 Azahar GT6 fork
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

#include <string>
#include <fmt/format.h>
#include <jni.h>
#include "common/file_util.h"
#include "common/perf_log.h"
#include "common/settings.h"
#include "jni/android_common/android_common.h"

namespace {

/// Settings snapshot written at the top of the log. It is built on the emulation thread once the
/// first frame has rendered, so per-game overrides are included. Recording the effective values
/// matters because defaults differ between the C++ settings and the Kotlin settings screen.
std::string BuildHeader(const std::string& device_info) {
    const auto& v = Settings::values;
    std::string out = device_info;
    out += fmt::format("graphics_api={}\n", static_cast<int>(v.graphics_api.GetValue()));
    out += fmt::format("resolution_factor={}\n", v.resolution_factor.GetValue());
    out += fmt::format("frame_limit={}\n", Settings::GetFrameLimit());
    out += fmt::format("use_vsync={}\n", v.use_vsync.GetValue());
    out += fmt::format("use_skip_duplicate_frames={}\n", v.use_skip_duplicate_frames.GetValue());
    out += fmt::format("async_presentation={}\n", v.async_presentation.GetValue());
    out += fmt::format("async_shader_compilation={}\n", v.async_shader_compilation.GetValue());
    out += fmt::format("spirv_shader_gen={}\n", v.spirv_shader_gen.GetValue());
    out += fmt::format("disable_spirv_optimizer={}\n", v.disable_spirv_optimizer.GetValue());
    out += fmt::format("shaders_accurate_mul={}\n", v.shaders_accurate_mul.GetValue());
    out += fmt::format("use_hw_shader={}\n", v.use_hw_shader.GetValue());
    out += fmt::format("use_shader_jit={}\n", v.use_shader_jit.GetValue());
    out += fmt::format("use_disk_shader_cache={}\n", v.use_disk_shader_cache.GetValue());
    out += fmt::format("use_cpu_jit={}\n", v.use_cpu_jit.GetValue());
    out += fmt::format("cpu_clock_percentage={}\n", v.cpu_clock_percentage.GetValue());
    out += fmt::format("is_new_3ds={}\n", v.is_new_3ds.GetValue());
    out += fmt::format("texture_filter={}\n", static_cast<int>(v.texture_filter.GetValue()));
    out += fmt::format("enable_audio_stretching={}\n", v.enable_audio_stretching.GetValue());
    out += fmt::format("enable_realtime_audio={}\n", v.enable_realtime_audio.GetValue());
    return out;
}

} // namespace

extern "C" {

JNIEXPORT jboolean JNICALL Java_org_citra_citra_1emu_utils_PerfLog_nativeBegin(
    JNIEnv* env, [[maybe_unused]] jobject obj, jstring device_info) {
    const std::string info = GetJString(env, device_info);
    const std::string& log_dir = FileUtil::GetUserPath(FileUtil::UserPath::LogDir);
    return Common::PerfLog::Begin(log_dir, [info] { return BuildHeader(info); }) ? JNI_TRUE
                                                                                 : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_org_citra_citra_1emu_utils_PerfLog_nativeEnd(
    [[maybe_unused]] JNIEnv* env, [[maybe_unused]] jobject obj) {
    Common::PerfLog::End();
}

JNIEXPORT void JNICALL Java_org_citra_citra_1emu_utils_PerfLog_nativeEvent(
    JNIEnv* env, [[maybe_unused]] jobject obj, jstring name) {
    if (!Common::PerfLog::IsEnabled()) {
        return;
    }
    Common::PerfLog::Event(GetJString(env, name));
}

JNIEXPORT void JNICALL Java_org_citra_citra_1emu_utils_PerfLog_nativeSetDisplayState(
    [[maybe_unused]] JNIEnv* env, [[maybe_unused]] jobject obj, jfloat refresh_hz, jint mode_id,
    jfloat vsync_ms) {
    Common::PerfLog::SetDisplayState(refresh_hz, mode_id, vsync_ms);
}

JNIEXPORT void JNICALL Java_org_citra_citra_1emu_utils_PerfLog_nativeSetThermalState(
    [[maybe_unused]] JNIEnv* env, [[maybe_unused]] jobject obj, jint status, jfloat headroom,
    jfloat battery_celsius) {
    Common::PerfLog::SetThermalState(status, headroom, battery_celsius);
}

JNIEXPORT void JNICALL Java_org_citra_citra_1emu_utils_PerfLog_nativeResetFrameTiming(
    [[maybe_unused]] JNIEnv* env, [[maybe_unused]] jobject obj) {
    Common::PerfLog::ResetFrameTiming();
}

} // extern "C"
