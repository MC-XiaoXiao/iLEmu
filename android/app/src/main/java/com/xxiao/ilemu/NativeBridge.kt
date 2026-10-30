package com.xxiao.ilemu

internal object NativeBridge {
    init {
        System.loadLibrary("SDL2")
        System.loadLibrary("ilemu")
    }

    // Accessed on the UI thread. SDL owns process-wide Java state until the
    // old Activity has finished onDestroy, even after SDL_main has returned.
    private var activityReserved = false
    private var activeContainerId: String? = null

    fun prepareSession(containerId: String? = null): Boolean {
        if (activityReserved || !prepare()) return false
        activityReserved = true
        activeContainerId = containerId
        return true
    }

    fun releaseActivity() { activityReserved = false; activeContainerId = null }

    fun ownsRunningSession(id: String): Boolean = activityReserved && activeContainerId == id && isRunning()

    external fun isRunning(): Boolean
    private external fun prepare(): Boolean
    external fun command(path: String, command: String, stop: Boolean): Boolean
}
