// Copyright 2026 Azahar GT6 fork
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

package org.citra.citra_emu.utils

import androidx.annotation.Keep
import androidx.preference.PreferenceManager
import org.citra.citra_emu.CitraApplication
import org.citra.citra_emu.features.settings.model.Settings

/**
 * Switches for the CPU scheduling help in src/common/frame_pacing.cpp: a higher thread priority
 * for the emulation / Vulkan threads and Android performance hints (ADPF). Both are on by
 * default and can be turned off in Settings > Debug to compare on the same build.
 */
@Keep
object FramePacing {
    private external fun nativeConfigure(raisePriority: Boolean, performanceHints: Boolean)

    private val preferences
        get() = PreferenceManager.getDefaultSharedPreferences(CitraApplication.appContext)

    /** Pushes the current switches to the native side. Call before a game starts. */
    fun configure() {
        nativeConfigure(
            preferences.getBoolean(Settings.PREF_GT6_RAISE_PRIORITY, true),
            preferences.getBoolean(Settings.PREF_GT6_PERFORMANCE_HINTS, true)
        )
    }
}
