// Copyright 2025-2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

package org.citra.citra_emu.utils
import android.app.Activity
import android.os.Build
import kotlin.math.abs

object RefreshRateUtil {
    // Display modes report their rate as a float that is not always exactly 60.0
    private const val SIXTY_HZ_TOLERANCE = 0.5f

    // Since Android 15, the OS automatically runs apps categorized as games with a
    // 60hz refresh rate by default, regardless of the refresh rate set by the user.
    //
    // This function sets the refresh rate to either the maximum allowed refresh rate or
    // 60hz depending on the value of the `sixtyHz` parameter.
    //
    // Note: This isn't always the maximum refresh rate that the display is *capable of*,
    // but is instead the refresh rate chosen by the user in the Android system settings.
    // For example, if the user selected 120hz in the settings, but the display is capable
    // of 144hz, 120hz will be treated as the maximum within this function.
    //
    // The preference is assigned back to the window (rather than only modified in place) so
    // that it also takes effect when the window is already on screen, e.g. when this is
    // called again from onResume or onWindowFocusChanged.
    fun enforceRefreshRate(activity: Activity, sixtyHz: Boolean = false) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) {
            return
        }

        val display = activity.display
        val window = activity.window

        display?.let {
            // Get all supported modes and find the one with the highest refresh rate
            val supportedModes = it.supportedModes
            val maxRefreshRate = supportedModes.maxByOrNull { mode -> mode.refreshRate }

            if (maxRefreshRate == null) {
                return
            }

            var newModeId: Int?
            if (sixtyHz) {
                // The mode closest to 60hz at the current resolution, so that changing the
                // refresh rate can never change the resolution as well
                val current = it.mode
                newModeId = supportedModes
                    .filter { mode ->
                        mode.physicalWidth == current.physicalWidth &&
                            mode.physicalHeight == current.physicalHeight &&
                            abs(mode.refreshRate - 60f) < SIXTY_HZ_TOLERANCE
                    }
                    .minByOrNull { mode -> abs(mode.refreshRate - 60f) }
                    ?.modeId
            } else {
                // Set the preferred display mode to the one with the highest refresh rate
                newModeId = maxRefreshRate.modeId
            }

            if (newModeId == null) {
                return
            }

            val attributes = window.attributes
            attributes.preferredDisplayModeId = newModeId
            window.attributes = attributes
        }
    }
}
