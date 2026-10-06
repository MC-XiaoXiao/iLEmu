package com.xxiao.ilemu

import android.Manifest
import android.app.Activity
import android.content.Context
import android.content.pm.PackageManager
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.location.Location
import android.location.LocationListener
import android.location.LocationManager
import android.os.Bundle
import android.os.SystemClock
import android.view.Surface

/** Collects only available foreground hardware. Firmware owns sensor fusion. */
internal class HostSensors(private val context: Activity) : SensorEventListener, LocationListener {
    private val manager = context.getSystemService(SensorManager::class.java)
    private val locations = context.getSystemService(LocationManager::class.java)
    private val types = intArrayOf(Sensor.TYPE_ACCELEROMETER, Sensor.TYPE_GYROSCOPE, Sensor.TYPE_MAGNETIC_FIELD)
    private val providers = mutableSetOf<String>()
    private var started = false

    fun start() {
        if (started) return
        started = true
        types.forEachIndexed { kind, type ->
            val sensor = manager?.getDefaultSensor(type)
            available(kind, sensor != null && manager.registerListener(this, sensor, 20_000))
        }
        startLocation()
    }

    private fun startLocation() {
        val fine = context.checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION) == PackageManager.PERMISSION_GRANTED
        val coarse = context.checkSelfPermission(Manifest.permission.ACCESS_COARSE_LOCATION) == PackageManager.PERMISSION_GRANTED
        if ((!fine && !coarse) || locations == null) return
        // Network works with approximate permission; GPS is used only with
        // precise permission. Do not synthesize a fix when providers are off.
        val candidates = if (fine) listOf(LocationManager.GPS_PROVIDER, LocationManager.NETWORK_PROVIDER)
            else listOf(LocationManager.NETWORK_PROVIDER)
        candidates.forEach { provider ->
            try {
                if (provider in locations.allProviders) {
                    locations.requestLocationUpdates(provider, 1000L, 0f, this)
                    if (locations.isProviderEnabled(provider)) {
                        providers.add(provider)
                        locations.getLastKnownLocation(provider)?.let(::onLocationChanged)
                    }
                }
            } catch (_: SecurityException) {
                providers.remove(provider)
            } catch (_: IllegalArgumentException) {
                providers.remove(provider)
            }
        }
        available(3, providers.isNotEmpty())
    }

    fun stop() {
        if (!started) return
        started = false
        manager?.unregisterListener(this)
        locations?.removeUpdates(this)
        providers.clear()
        for (kind in 0..3) available(kind, false)
    }

    override fun onSensorChanged(event: SensorEvent) {
        if (!started || event.values.size < 3) return
        val kind = types.indexOf(event.sensor.type)
        if (kind < 0) return
        // Android reports proper acceleration; Apple's gravity convention
        // has the opposite sign. Rotate natural host coordinates into the
        // portrait display basis (also supports naturally landscape tablets).
        val scale = if (kind == 0) -1f / SensorManager.GRAVITY_EARTH else 1f
        @Suppress("DEPRECATION")
        val rotation = context.windowManager.defaultDisplay.rotation
        val x = event.values[0]
        val y = event.values[1]
        val displayX = when (rotation) {
            Surface.ROTATION_90 -> -y
            Surface.ROTATION_180 -> -x
            Surface.ROTATION_270 -> y
            else -> x
        }
        val displayY = when (rotation) {
            Surface.ROTATION_90 -> x
            Surface.ROTATION_180 -> -y
            Surface.ROTATION_270 -> -x
            else -> y
        }
        motion(kind, displayX * scale, displayY * scale,
            event.values[2] * scale, event.timestamp, event.accuracy)
    }

    override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) = Unit

    override fun onLocationChanged(fix: Location) {
        if (!started || !fix.hasAccuracy() || fix.provider !in providers) return
        val age = SystemClock.elapsedRealtimeNanos() - fix.elapsedRealtimeNanos
        if (age < 0 || age > 30_000_000_000L) return
        available(3, true)
        val fields = (if (fix.hasAltitude()) 1 else 0) or
            (if (fix.hasVerticalAccuracy()) 2 else 0) or
            (if (fix.hasSpeed()) 4 else 0) or (if (fix.hasBearing()) 8 else 0)
        location(fix.latitude, fix.longitude, fix.accuracy.toDouble(), fix.time / 1000.0,
            fix.elapsedRealtimeNanos, fields, fix.altitude, fix.verticalAccuracyMeters.toDouble(),
            fix.speed.toDouble(), fix.bearing.toDouble())
    }

    override fun onProviderEnabled(provider: String) {
        if (!started) return
        providers.add(provider)
        available(3, true)
    }

    override fun onProviderDisabled(provider: String) {
        if (!started) return
        providers.remove(provider)
        available(3, providers.isNotEmpty())
    }

    @Deprecated("Required by older LocationListener implementations")
    override fun onStatusChanged(provider: String?, status: Int, extras: Bundle?) = Unit

    private external fun available(kind: Int, available: Boolean)
    private external fun motion(kind: Int, x: Float, y: Float, z: Float, timestamp: Long, accuracy: Int)
    private external fun location(latitude: Double, longitude: Double, accuracy: Double,
        unixTime: Double, timestamp: Long, fields: Int, altitude: Double,
        verticalAccuracy: Double, speed: Double, bearing: Double)
}
