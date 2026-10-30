package com.xxiao.ilemu

import android.content.Context
import androidx.appcompat.app.AppCompatDelegate

internal class AppSettings(context: Context) {
    private val preferences = context.applicationContext.getSharedPreferences("app-settings", Context.MODE_PRIVATE)
    var appearance: Int
        get() = preferences.getInt("appearance", AppCompatDelegate.MODE_NIGHT_FOLLOW_SYSTEM)
        set(value) { preferences.edit().putInt("appearance", value).apply(); AppCompatDelegate.setDefaultNightMode(value) }
    var keepAwake: Boolean
        get() = preferences.getBoolean("keepAwake", true)
        set(value) { preferences.edit().putBoolean("keepAwake", value).apply() }
    var haptics: Boolean
        get() = preferences.getBoolean("haptics", true)
        set(value) { preferences.edit().putBoolean("haptics", value).apply() }
    var defaultCores: Int
        get() = preferences.getInt("defaultCores", 1).coerceIn(1, 8)
        set(value) { preferences.edit().putInt("defaultCores", value.coerceIn(1, 8)).apply() }
    var defaultJit: Int
        get() = preferences.getInt("defaultJit", 16).takeIf { it in listOf(8,16,32,64,128) } ?: 16
        set(value) { preferences.edit().putInt("defaultJit", value).apply() }
    var downloadThreads: Int
        get() = preferences.getInt("downloadThreads", 4).coerceIn(1, 8)
        set(value) { preferences.edit().putInt("downloadThreads", value.coerceIn(1, 8)).apply() }
    fun applyAppearance() { if (AppCompatDelegate.getDefaultNightMode() != appearance) AppCompatDelegate.setDefaultNightMode(appearance) }
}
