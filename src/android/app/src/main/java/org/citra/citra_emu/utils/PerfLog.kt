// Copyright 2026 Azahar GT6 fork
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

package org.citra.citra_emu.utils

import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.display.DisplayManager
import android.os.BatteryManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.PowerManager
import android.view.Display
import android.view.Gravity
import android.view.HapticFeedbackConstants
import android.widget.Button
import androidx.annotation.Keep
import androidx.coordinatorlayout.widget.CoordinatorLayout
import androidx.preference.PreferenceManager
import org.citra.citra_emu.BuildConfig
import org.citra.citra_emu.CitraApplication
import org.citra.citra_emu.R
import org.citra.citra_emu.features.settings.model.Settings

/**
 * Optional per-frame performance log (GT6 fork).
 *
 * The per-frame work happens natively (src/common/perf_log.cpp). This object owns the session
 * lifecycle and feeds in what only the Java side can see: display mode, the real vsync cadence,
 * thermal state and UI events such as surface changes and the MARK button.
 *
 * Off by default. Everything is a no-op unless the "Performance Log" switch was on when the
 * game was started.
 */
@Keep
object PerfLog {
    private const val SAMPLE_INTERVAL_MS = 1000L
    private const val THERMAL_FORECAST_SECONDS = 10
    private const val MARK_BUTTON_SIZE_DP = 56
    private const val MARK_BUTTON_MARGIN_DP = 8

    // Larger gaps are pauses, not display cadence
    private const val MAX_VSYNC_INTERVAL_MS = 100f

    private val mainHandler = Handler(Looper.getMainLooper())

    @Volatile
    private var active = false

    // Everything below is only touched on the main thread
    private var lastVsyncNanos = 0L
    private var vsyncIntervalMs = Float.NaN
    private var lastModeId = -1
    private var lastRefreshHz = Float.NaN
    private var displayListener: DisplayManager.DisplayListener? = null

    private val sampler = object : Runnable {
        override fun run() {
            if (!active) {
                return
            }
            sampleDisplay()
            sampleThermal()
            mainHandler.postDelayed(this, SAMPLE_INTERVAL_MS)
        }
    }

    private external fun nativeBegin(deviceInfo: String): Boolean
    private external fun nativeEnd()
    private external fun nativeEvent(name: String)
    private external fun nativeSetDisplayState(refreshHz: Float, modeId: Int, vsyncMs: Float)
    private external fun nativeSetThermalState(status: Int, headroom: Float, batteryCelsius: Float)
    private external fun nativeResetFrameTiming()

    val isEnabledInPreferences: Boolean
        get() = PreferenceManager.getDefaultSharedPreferences(CitraApplication.appContext)
            .getBoolean(Settings.PREF_GT6_PERF_LOG, false)

    /** Starts a session if the switch is on. Call on the emulation thread before it runs. */
    fun begin() {
        if (active || !isEnabledInPreferences) {
            return
        }
        if (!nativeBegin(deviceInfo())) {
            Log.error("[PerfLog] Could not start the performance log")
            return
        }
        active = true
        event("session_start")
        mainHandler.post { startSampling() }
    }

    /** Ends the session and closes the file. Call on the emulation thread after it stopped. */
    fun end() {
        if (!active) {
            return
        }
        event("session_end")
        active = false
        nativeEnd()
        mainHandler.post { stopSampling() }
    }

    fun event(name: String) {
        if (active) {
            nativeEvent(name)
        }
    }

    fun mark() = event("MARK")

    /** Forget the frame interval across a pause/resume so it is not reported as a long frame. */
    fun resetFrameTiming() {
        if (active) {
            nativeResetFrameTiming()
        }
    }

    /** Main thread, from the Choreographer callback: measures the real display cadence. */
    fun onVsync(frameTimeNanos: Long) {
        if (!active) {
            return
        }
        val last = lastVsyncNanos
        lastVsyncNanos = frameTimeNanos
        if (last == 0L) {
            return
        }
        val intervalMs = (frameTimeNanos - last) / 1_000_000f
        if (intervalMs > MAX_VSYNC_INTERVAL_MS) {
            return
        }
        // Smoothed so a single late callback does not look like a refresh rate change
        vsyncIntervalMs = if (vsyncIntervalMs.isNaN()) {
            intervalMs
        } else {
            vsyncIntervalMs + 0.1f * (intervalMs - vsyncIntervalMs)
        }
    }

    /** Adds a small "MARK" button to the emulation screen so a jitter episode can be logged. */
    fun attachMarkButton(parent: CoordinatorLayout) {
        if (!isEnabledInPreferences) {
            return
        }
        val context = parent.context
        val density = context.resources.displayMetrics.density
        val label = context.getString(R.string.gt6_perf_log_mark)
        val button = Button(context)
        button.text = label
        button.textSize = 11f
        button.alpha = 0.6f
        button.minWidth = 0
        button.minHeight = 0
        button.setPadding(0, 0, 0, 0)
        val params = CoordinatorLayout.LayoutParams(
            (MARK_BUTTON_SIZE_DP * density).toInt(),
            (MARK_BUTTON_SIZE_DP * density).toInt()
        )
        params.gravity = Gravity.TOP or Gravity.END
        params.setMargins(0, (MARK_BUTTON_MARGIN_DP * density).toInt(), 0, 0)
        button.layoutParams = params
        button.setOnClickListener {
            mark()
            it.performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY)
            button.text = "OK"
            button.postDelayed({ button.text = label }, 600)
        }
        parent.addView(button)
    }

    private fun startSampling() {
        val context = CitraApplication.appContext
        lastVsyncNanos = 0L
        vsyncIntervalMs = Float.NaN
        lastModeId = -1
        lastRefreshHz = Float.NaN

        val listener = object : DisplayManager.DisplayListener {
            override fun onDisplayAdded(displayId: Int) {
                // No op
            }

            override fun onDisplayRemoved(displayId: Int) {
                // No op
            }

            override fun onDisplayChanged(displayId: Int) {
                if (displayId == Display.DEFAULT_DISPLAY) {
                    sampleDisplay()
                }
            }
        }
        displayListener = listener
        context.getSystemService(DisplayManager::class.java)
            ?.registerDisplayListener(listener, mainHandler)
        sampler.run()
    }

    private fun stopSampling() {
        mainHandler.removeCallbacks(sampler)
        val listener = displayListener ?: return
        displayListener = null
        CitraApplication.appContext.getSystemService(DisplayManager::class.java)
            ?.unregisterDisplayListener(listener)
    }

    private fun sampleDisplay() {
        val display = CitraApplication.appContext
            .getSystemService(DisplayManager::class.java)
            ?.getDisplay(Display.DEFAULT_DISPLAY)
        var refreshHz = Float.NaN
        var modeId = -1
        if (display != null) {
            val mode = display.mode
            refreshHz = mode.refreshRate
            modeId = mode.modeId
        }
        if (lastModeId != -1 && (modeId != lastModeId || refreshHz != lastRefreshHz)) {
            event("display_mode_changed id=$lastModeId->$modeId hz=$lastRefreshHz->$refreshHz")
        }
        lastModeId = modeId
        lastRefreshHz = refreshHz
        nativeSetDisplayState(refreshHz, modeId, vsyncIntervalMs)
    }

    private fun sampleThermal() {
        val context = CitraApplication.appContext
        val powerManager = context.getSystemService(PowerManager::class.java)
        val status = powerManager?.currentThermalStatus ?: -1
        var headroom = Float.NaN
        if (powerManager != null && Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            headroom = powerManager.getThermalHeadroom(THERMAL_FORECAST_SECONDS)
        }
        nativeSetThermalState(status, headroom, batteryTemperatureCelsius(context))
    }

    private fun batteryTemperatureCelsius(context: Context): Float {
        val intent = context.registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
        val tenths = intent?.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, Int.MIN_VALUE)
            ?: Int.MIN_VALUE
        return if (tenths == Int.MIN_VALUE) Float.NaN else tenths / 10f
    }

    private fun deviceInfo(): String = buildString {
        appendLine("app_version=${BuildConfig.VERSION_NAME}")
        appendLine("git_hash=${BuildConfig.GIT_HASH}")
        appendLine("branch=${BuildConfig.BRANCH}")
        appendLine("application_id=${BuildConfig.APPLICATION_ID}")
        appendLine("build_type=${BuildConfig.BUILD_TYPE}")
        appendLine("device=${Build.MANUFACTURER} ${Build.MODEL}")
        appendLine("hardware=${Build.HARDWARE}")
        appendLine("android_sdk=${Build.VERSION.SDK_INT}")
        appendLine("android_release=${Build.VERSION.RELEASE}")
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            appendLine("soc=${Build.SOC_MANUFACTURER} ${Build.SOC_MODEL}")
        }
    }
}
