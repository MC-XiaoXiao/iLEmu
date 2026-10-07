package com.xxiao.ilemu

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import com.xxiao.ilemu.containers.*
import com.xxiao.ilemu.databinding.ActivityMainBinding
import com.xxiao.ilemu.firmware.FirmwareDownloadService
import org.json.JSONArray

class MainActivity : AppCompatActivity() {
    private lateinit var binding: ActivityMainBinding
    private lateinit var repository: ContainerRepository
    private var pendingArguments: Array<String>? = null
    private var pendingContainerId: String? = null
    private val handler = Handler(Looper.getMainLooper())
    private val refresh = object : Runnable {
        override fun run() { refreshContainers(); handler.postDelayed(this, 1500) }
    }
    private val locationPermission = registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) {
        val arguments = pendingArguments
        val id = pendingContainerId
        pendingArguments = null
        pendingContainerId = null
        if (arguments != null) launchSession(arguments, id)
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)
        repository = ContainerRepository(this)
        pendingArguments = savedInstanceState?.getStringArray("pendingArguments")
        pendingContainerId = savedInstanceState?.getString("pendingContainerId")
        ViewCompat.setOnApplyWindowInsetsListener(binding.root) { view, insets ->
            val bars = insets.getInsets(WindowInsetsCompat.Type.systemBars() or WindowInsetsCompat.Type.displayCutout())
            val padding = (20 * resources.displayMetrics.density).toInt()
            view.setPadding(padding + bars.left, padding + bars.top, padding + bars.right, padding + bars.bottom)
            insets
        }
        binding.addContainer.setOnClickListener { startActivity(Intent(this, ContainerActivity::class.java)) }
        binding.diagnostics.setOnClickListener {
            DiagnosticsDialog.show(this) { launchSession(SessionSettings.smokeArguments()) }
        }
        try { repository.migrateLegacy() } catch (error: Exception) { binding.message.text = "已有固件识别失败：${error.message}" }
        if (savedInstanceState == null) handleDebugCommand(intent)
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        handleDebugCommand(intent)
    }

    private fun handleDebugCommand(intent: Intent) {
        if (BuildConfig.DEBUG) intent.getStringExtra("arguments")?.let { encoded ->
            try {
                require(encoded.length <= 65536)
                val args = JSONArray(encoded)
                require(args.length() in 1..128)
                launchSession(Array(args.length()) { args.getString(it) })
            } catch (error: Exception) { binding.message.text = error.message }
        }
    }

    private fun refreshContainers() {
        try {
            val entries = repository.list()
            ContainerList(this, binding.containers).render(entries,
                start = { entry -> launchSession(SessionSettings(entry).bootArguments(this), entry.id) },
                settings = { entry -> startActivity(Intent(this, ContainerActivity::class.java).putExtra("id", entry.id)) },
                download = { entry -> runCatching { FirmwareDownloadService.start(this, entry.id) }
                    .onFailure { binding.message.text = it.message } },
                pause = { FirmwareDownloadService.pause(this) },
                delete = ::confirmDelete)
            if (entries.isEmpty()) binding.message.text = "尚无容器，点击添加容器选择或导入固件。"
        } catch (error: Exception) { binding.message.text = error.message }
    }

    private fun confirmDelete(entry: Container) {
        AlertDialog.Builder(this).setTitle("删除 ${entry.name}？")
            .setMessage(if (entry.legacy) "移除列表项，保留原有固件目录。" else "删除此容器的固件、下载文件和数据。")
            .setNegativeButton("取消", null).setPositiveButton("删除") { _, _ ->
                try {
                    ContainerTasks.run(entry.id, {
                        ContainerStorage(this).delete(entry)
                        repository.removeOwned(entry.id)
                    }, { error -> runOnUiThread {
                        binding.message.text = error?.let { "删除失败：${it.message}" } ?: "已删除 ${entry.name}"
                        refreshContainers()
                    } })
                } catch (error: Exception) { binding.message.text = error.message }
            }.show()
    }

    private fun launchSession(arguments: Array<String>, id: String? = null) {
        val preferences = getPreferences(MODE_PRIVATE)
        if (arguments.firstOrNull() == "boot" &&
            checkSelfPermission(Manifest.permission.ACCESS_COARSE_LOCATION) != PackageManager.PERMISSION_GRANTED &&
            !preferences.getBoolean("locationPermissionRequested", false)) {
            pendingArguments = arguments
            pendingContainerId = id
            preferences.edit().putBoolean("locationPermissionRequested", true).apply()
            locationPermission.launch(arrayOf(Manifest.permission.ACCESS_FINE_LOCATION, Manifest.permission.ACCESS_COARSE_LOCATION))
            return
        }
        var reserved = false
        var nativeReserved = false
        try {
            if (id != null) { check(ContainerTasks.reserve(id)) { "容器正在使用中" }; reserved = true }
            if (!NativeBridge.prepareSession()) {
                if (reserved) ContainerTasks.release(id!!)
                Toast.makeText(this, R.string.session_stopping, Toast.LENGTH_SHORT).show()
                return
            }
            nativeReserved = true
            if (id != null) repository.update(id) { it.copy(dirty = it.storageTree.isNotEmpty(), error = "") }
            startActivity(Intent(this, EmulatorActivity::class.java).putExtra("arguments", arguments).putExtra("containerId", id))
        } catch (error: Throwable) {
            if (nativeReserved) NativeBridge.releaseActivity()
            if (reserved) ContainerTasks.release(id!!)
            binding.message.text = error.message
        }
    }

    override fun onResume() { super.onResume(); handler.post(refresh) }
    override fun onPause() { handler.removeCallbacks(refresh); super.onPause() }
    override fun onSaveInstanceState(outState: Bundle) {
        pendingArguments?.let { outState.putStringArray("pendingArguments", it) }
        outState.putString("pendingContainerId", pendingContainerId)
        super.onSaveInstanceState(outState)
    }
}
