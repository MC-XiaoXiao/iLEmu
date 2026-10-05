package com.xxiao.ilemu

import android.content.Context
import java.io.File

internal data class SessionSettings(val device: String, val headless: Boolean) {
    fun bootArguments(context: Context): Array<String> = arrayOf(
        "boot", "--rootfs", File(context.filesDir, "rootfs").path,
        "--device", device,
        "--host-cache", File(context.cacheDir, "emulator").path,
        "--activation", "activated", "--network", "isolated",
        "--gles-backend", "vulkan", "--display", if (headless) "headless" else "sdl",
        "--cores", "1", "--jit-cache-mib", "16", "--jit-cache-budget-mib", "256",
        "--jit-artifact-memory-mib", "16", "--jit-artifact-disk-mib", "128",
        "--control-stdin"
    )

    companion object {
        fun smokeArguments(): Array<String> = arrayOf("smoke", "--cores", "2", "--jit-cache-mib", "16")
    }
}
