package com.xxiao.ilemu

import android.app.Application
import com.google.android.material.color.DynamicColors

class ILEmuApplication : Application() {
    override fun onCreate() {
        super.onCreate()
        AppSettings(this).applyAppearance()
        DynamicColors.applyToActivitiesIfAvailable(this)
    }
}
