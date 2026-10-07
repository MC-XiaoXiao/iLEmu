package com.xxiao.ilemu.containers

import android.os.Handler
import android.os.Looper
import androidx.lifecycle.MutableLiveData
import androidx.lifecycle.ViewModel

// Keep long imports/snapshots attached to a screen's lifecycle across rotation.
internal class ContainerOperation : ViewModel() {
    data class State(val working: Boolean = false, val completed: Boolean = false, val error: Throwable? = null)
    val state = MutableLiveData(State())

    fun start(id: String, operation: () -> Unit, saved: () -> Unit) {
        check(state.value?.working != true)
        state.value = State(working = true)
        ContainerTasks.run(id, operation) { error ->
            Handler(Looper.getMainLooper()).post {
                val result = error ?: runCatching(saved).exceptionOrNull()
                state.value = State(completed = result == null, error = result)
            }
        }
    }
}
