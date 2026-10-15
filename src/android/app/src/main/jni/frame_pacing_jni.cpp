// Copyright 2026 Azahar GT6 fork
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

#include <jni.h>
#include "common/frame_pacing.h"

extern "C" {

JNIEXPORT void JNICALL Java_org_citra_citra_1emu_utils_FramePacing_nativeConfigure(
    [[maybe_unused]] JNIEnv* env, [[maybe_unused]] jobject obj, jboolean raise_priority,
    jboolean performance_hints) {
    Common::FramePacing::Configure(Common::FramePacing::Options{
        .raise_priority = raise_priority == JNI_TRUE,
        .performance_hints = performance_hints == JNI_TRUE,
    });
}

} // extern "C"
