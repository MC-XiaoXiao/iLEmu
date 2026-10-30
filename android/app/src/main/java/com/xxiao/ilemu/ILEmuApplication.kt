package com.xxiao.ilemu

import android.app.Application

class ILEmuApplication : Application() {
    override fun onCreate() {
        super.onCreate()
        AppSettings(this).applyAppearance()
    }
}
