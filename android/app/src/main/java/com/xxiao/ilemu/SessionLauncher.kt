package com.xxiao.ilemu

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Bundle
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import com.xxiao.ilemu.containers.*

/** Owns permission and process-wide session reservation, independent of screen layout. */
internal class SessionLauncher(private val activity: AppCompatActivity) {
    private var pendingArguments: Array<String>? = null
    private var pendingId: String? = null
    private val permission = activity.registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) {
        val arguments = pendingArguments; val id = pendingId
        pendingArguments = null; pendingId = null
        if (arguments != null) launch(arguments, id)
    }
    fun restore(state: Bundle?) {
        pendingArguments = state?.getStringArray("pendingArguments"); pendingId = state?.getString("pendingContainerId")
    }
    fun save(state: Bundle) {
        state.putStringArray("pendingArguments", pendingArguments); state.putString("pendingContainerId", pendingId)
    }
    fun launch(arguments: Array<String>, id: String? = null) {
        val prefs = activity.getSharedPreferences("MainActivity", 0)
        if (arguments.firstOrNull() == "boot" && activity.checkSelfPermission(Manifest.permission.ACCESS_COARSE_LOCATION) != PackageManager.PERMISSION_GRANTED &&
            !prefs.getBoolean("locationPermissionRequested", false)) {
            pendingArguments = arguments; pendingId = id
            prefs.edit().putBoolean("locationPermissionRequested", true).apply()
            permission.launch(arrayOf(Manifest.permission.ACCESS_FINE_LOCATION, Manifest.permission.ACCESS_COARSE_LOCATION)); return
        }
        var reserved = false
        var nativeReserved = false
        try {
            if (id != null) { check(ContainerTasks.reserve(id)) { "容器正在使用中" }; reserved = true }
            check(NativeBridge.prepareSession(id)) { activity.getString(R.string.session_stopping) }
            nativeReserved = true
            if (id != null) ContainerRepository(activity).update(id) { it.copy(dirty = it.storageTree.isNotEmpty(), error = "") }
            activity.startActivity(Intent(activity, EmulatorActivity::class.java).putExtra("arguments", arguments).putExtra("containerId", id))
        } catch (error: Throwable) {
            if (nativeReserved) NativeBridge.releaseActivity()
            if (reserved) ContainerTasks.release(id!!)
            Toast.makeText(activity, error.message, Toast.LENGTH_LONG).show()
        }
    }
}
