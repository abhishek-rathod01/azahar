// Copyright 2025-2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

package org.citra.citra_emu.utils
import android.app.Activity
import android.hardware.display.DisplayManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.view.Display
import java.lang.ref.WeakReference
import kotlin.math.abs

object RefreshRateUtil {
    // Display modes report their rate as a float that is not always exactly 60.0
    private const val SIXTY_HZ_TOLERANCE = 0.5f

    // While emulating, the system can still switch the display away from 60hz (thermal or
    // battery policy, another window, ...). The listener below re-applies 60hz, but gives up
    // after a few attempts per resume so that it can never become a fight with the system.
    private const val MAX_REASSERTS = 5
    private const val MIN_REASSERT_INTERVAL_MS = 2000L

    private var displayListener: DisplayManager.DisplayListener? = null
    private var lastReassertMs = 0L
    private var reasserts = 0

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

    // Call from onResume of the emulation screen; pair with stopEnforcingSixtyHz in onPause.
    fun startEnforcingSixtyHz(activity: Activity) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R || displayListener != null) {
            return
        }
        val displayManager = activity.getSystemService(DisplayManager::class.java) ?: return
        val activityRef = WeakReference(activity)
        reasserts = 0
        lastReassertMs = 0L

        val listener = object : DisplayManager.DisplayListener {
            override fun onDisplayAdded(displayId: Int) {
                // No op
            }

            override fun onDisplayRemoved(displayId: Int) {
                // No op
            }

            override fun onDisplayChanged(displayId: Int) {
                if (displayId != Display.DEFAULT_DISPLAY) {
                    return
                }
                val target = activityRef.get() ?: return
                val refreshRate = target.display?.mode?.refreshRate ?: return
                if (abs(refreshRate - 60f) < SIXTY_HZ_TOLERANCE) {
                    return
                }
                val now = SystemClock.elapsedRealtime()
                if (reasserts >= MAX_REASSERTS || now - lastReassertMs < MIN_REASSERT_INTERVAL_MS) {
                    return
                }
                reasserts++
                lastReassertMs = now
                Log.info(
                    "[RefreshRateUtil] Display is at $refreshRate Hz while emulating, " +
                        "re-applying 60 Hz ($reasserts/$MAX_REASSERTS)"
                )
                enforceRefreshRate(target, sixtyHz = true)
            }
        }
        displayManager.registerDisplayListener(listener, Handler(Looper.getMainLooper()))
        displayListener = listener
    }

    fun stopEnforcingSixtyHz(activity: Activity) {
        val listener = displayListener ?: return
        displayListener = null
        activity.getSystemService(DisplayManager::class.java)?.unregisterDisplayListener(listener)
    }
}
