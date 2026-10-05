package com.xxiao.ilemu

import android.content.Intent
import android.os.Bundle
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import com.xxiao.ilemu.databinding.ActivityMainBinding
import java.io.File
import java.io.RandomAccessFile
import org.json.JSONArray

class MainActivity : AppCompatActivity() {
    private lateinit var binding: ActivityMainBinding

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)
        val padding = (20 * resources.displayMetrics.density).toInt()
        ViewCompat.setOnApplyWindowInsetsListener(binding.root) { view, insets ->
            val bars = insets.getInsets(WindowInsetsCompat.Type.systemBars() or WindowInsetsCompat.Type.displayCutout())
            view.setPadding(padding + bars.left, padding + bars.top, padding + bars.right, padding + bars.bottom)
            insets
        }
        val preferences = getPreferences(MODE_PRIVATE)
        binding.device.setText(preferences.getString("device", "iPhone2,1"))
        binding.rootfsPath.text = File(filesDir, "rootfs").path
        binding.start.setOnClickListener {
            val device = binding.device.text.toString().trim()
            if (device.isEmpty()) {
                binding.device.error = getString(R.string.device_required)
            } else {
                preferences.edit().putString("device", device).apply()
                launchSession(SessionSettings(device, binding.headless.isChecked).bootArguments(this))
            }
        }
        binding.smoke.setOnClickListener { launchSession(SessionSettings.smokeArguments()) }
        binding.refresh.setOnClickListener { refreshLog() }

        // A debug build can run the same CLI commands through adb. Production
        // launches only use the controls above; no external command receiver.
        if (savedInstanceState == null) handleDebugCommand(intent)
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        handleDebugCommand(intent)
    }

    private fun handleDebugCommand(intent: Intent) {
        if (BuildConfig.DEBUG) {
            intent.getStringExtra("arguments")?.let { encoded ->
                try {
                    require(encoded.length <= 65536)
                    val args = JSONArray(encoded)
                    require(args.length() in 1..128)
                    launchSession(Array(args.length()) { args.getString(it) })
                } catch (error: Exception) {
                    binding.log.text = error.message
                }
            }
        }
    }

    override fun onResume() {
        super.onResume()
        refreshLog()
    }

    private fun launchSession(arguments: Array<String>) {
        try {
            if (!NativeBridge.prepareSession()) {
                Toast.makeText(this, R.string.session_stopping, Toast.LENGTH_SHORT).show()
                return
            }
            try {
                startActivity(Intent(this, EmulatorActivity::class.java).putExtra("arguments", arguments))
            } catch (error: Exception) {
                NativeBridge.releaseActivity()
                throw error
            }
        } catch (error: LinkageError) {
            binding.log.text = error.message
        }
    }

    private fun refreshLog() {
        binding.log.text = listOf("session.log", "native.log").mapNotNull { name ->
            val file = File(filesDir, name)
            if (!file.isFile) return@mapNotNull null
            try {
                RandomAccessFile(file, "r").use { input ->
                    input.seek((input.length() - 32768).coerceAtLeast(0))
                    val bytes = ByteArray((input.length() - input.filePointer).toInt())
                    input.readFully(bytes)
                    "$name\n${bytes.toString(Charsets.UTF_8)}"
                }
            } catch (error: Exception) { "$name: ${error.message}" }
        }.joinToString("\n\n").ifEmpty { getString(R.string.no_log) }
    }
}
