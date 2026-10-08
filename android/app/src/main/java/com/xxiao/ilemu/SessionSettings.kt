package com.xxiao.ilemu

import android.content.Context
import java.io.File
import com.xxiao.ilemu.containers.*

internal data class SessionSettings(val container: Container) {
    fun bootArguments(context: Context): Array<String> = arrayOf(
        "boot", "--rootfs", ContainerRepository(context).rootfs(container).path,
        "--device", container.device,
        "--host-cache", ContainerRepository(context).cache(container).path,
        "--activation", "activated", "--network", "isolated",
        "--gles-backend", "vulkan", "--display", if (container.headless) "headless" else "sdl",
        "--cores", container.cores.toString(), "--jit-cache-mib", container.jitCacheMiB.toString(), "--jit-cache-budget-mib", "256",
        "--jit-artifact-disk-mib", "128",
        "--control-stdin"
    )

    companion object {
        fun smokeArguments(): Array<String> = arrayOf("smoke", "--cores", "2", "--jit-cache-mib", "16")
    }
}
