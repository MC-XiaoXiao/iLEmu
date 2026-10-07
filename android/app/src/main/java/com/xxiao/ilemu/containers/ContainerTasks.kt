package com.xxiao.ilemu.containers

import android.content.Context
import com.xxiao.ilemu.NativeBridge
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.Executors

// Host file work never runs on the Android UI or emulator execution threads.
internal object ContainerTasks {
    private val busy = ConcurrentHashMap.newKeySet<String>()
    private val executor = Executors.newSingleThreadExecutor()
    fun isBusy(id: String) = busy.contains(id)
    fun reserve(id: String): Boolean = busy.add(id)
    fun release(id: String) { busy.remove(id) }

    fun run(id: String, operation: () -> Unit, completed: (Throwable?) -> Unit) {
        check(reserve(id)) { "容器正在使用中" }
        executor.execute {
            val result = runCatching(operation)
            release(id)
            completed(result.exceptionOrNull())
        }
    }

    fun sessionEnded(context: Context, id: String) {
        // Activity destruction can precede SDL_main returning. Keep ownership
        // until the native guest has stopped writing its data.
        executor.execute {
            try {
                while (NativeBridge.isRunning()) Thread.sleep(100)
                val repository = ContainerRepository(context)
                val container = repository.get(id)
                if (container.storageTree.isNotEmpty()) {
                    ContainerStorage(context).checkpoint(container, dataOnly = true)
                    repository.update(id) { it.copy(dirty = false, error = "") }
                }
            } catch (error: Exception) {
                runCatching { ContainerRepository(context).update(id) { it.copy(error = "目录同步失败：${error.message}") } }
            } finally { release(id) }
        }
    }
}
