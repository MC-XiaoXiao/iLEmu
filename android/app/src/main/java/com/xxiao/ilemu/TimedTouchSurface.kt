package com.xxiao.ilemu

import android.content.Context
import android.os.Build
import android.view.InputDevice
import android.view.MotionEvent
import android.view.View
import org.libsdl.app.SDLActivity
import org.libsdl.app.SDLSurface

internal class TimedTouchSurface(context: Context) : SDLSurface(context) {
    private var downTime = -1L
    private var deviceId = 0
    private var fingerId = 0
    private var lastSampleTimeNs = 0L

    override fun onTouch(view: View, event: MotionEvent): Boolean {
        val action = event.actionMasked
        if (action == MotionEvent.ACTION_DOWN) {
            downTime = event.downTime
            deviceId = event.deviceId
            fingerId = event.getPointerId(0)
            lastSampleTimeNs = 0L
        }

        // Leave other sources and multi-pointer sequences to SDLSurface.
        // Once the gesture leaves this path, a later MOVE cannot reenter it.
        if (downTime != event.downTime || deviceId != event.deviceId ||
            event.source != InputDevice.SOURCE_TOUCHSCREEN || event.pointerCount != 1 ||
            event.getPointerId(0) != fingerId || event.getToolType(0) != MotionEvent.TOOL_TYPE_FINGER ||
            (action != MotionEvent.ACTION_DOWN && action != MotionEvent.ACTION_MOVE &&
                action != MotionEvent.ACTION_UP) || mWidth <= 0f || mHeight <= 0f ||
            !NativeBridge.beginAndroidTouchBatch()) {
            downTime = -1L
            return super.onTouch(view, event)
        }

        // Match SDLSurface's remapping of negative Android device IDs.
        val sdlDeviceId = if (deviceId < 0) deviceId - 1 else deviceId
        try {
            if (action == MotionEvent.ACTION_MOVE) {
                for (index in 0 until event.historySize) {
                    val sampleTimeNs = if (Build.VERSION.SDK_INT >= 34)
                        event.getHistoricalEventTimeNanos(index)
                    else event.getHistoricalEventTime(index) * 1_000_000L
                    if (sampleTimeNs >= lastSampleTimeNs)
                        submit(sdlDeviceId, action, sampleTimeNs, event.getHistoricalX(0, index),
                            event.getHistoricalY(0, index), event.getHistoricalPressure(0, index))
                }
            }
            val sampleTimeNs = if (Build.VERSION.SDK_INT >= 34) event.eventTimeNanos
                else event.eventTime * 1_000_000L
            submit(sdlDeviceId, action, sampleTimeNs, event.x, event.y, event.pressure)
            if (action == MotionEvent.ACTION_DOWN) {
                // Deliver the rest of this drag without waiting for Android's next vsync.
                requestUnbufferedDispatch(event)
            }
        } finally {
            NativeBridge.endAndroidTouchBatch()
            if (action == MotionEvent.ACTION_UP) downTime = -1L
        }
        return true
    }

    private fun submit(sdlDeviceId: Int, action: Int, sampleTimeNs: Long,
        x: Float, y: Float, pressure: Float) {
        val normalizedX = x / mWidth
        val normalizedY = y / mHeight
        val sourceTimeNs = if (sampleTimeNs >= lastSampleTimeNs) sampleTimeNs else 0L
        lastSampleTimeNs = maxOf(lastSampleTimeNs, sampleTimeNs)
        NativeBridge.annotateAndroidTouchSample(sdlDeviceId, fingerId, action, sourceTimeNs,
            normalizedX, normalizedY)
        SDLActivity.onNativeTouch(sdlDeviceId, fingerId, action, normalizedX, normalizedY,
            pressure.coerceAtMost(1f))
    }
}
