package com.xxiao.ilemu

import com.google.android.material.appbar.MaterialToolbar
import com.google.android.material.color.MaterialColors
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.xxiao.ilemu.ui.ScreenUi
import android.annotation.SuppressLint
import androidx.appcompat.app.AlertDialog
import android.content.Context
import android.content.res.Configuration
import android.os.Build
import android.os.Bundle
import android.view.KeyEvent
import android.view.ViewGroup
import android.view.WindowManager
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.Toast
import android.window.OnBackInvokedCallback
import android.window.OnBackInvokedDispatcher
import androidx.core.content.ContextCompat
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import com.xxiao.ilemu.containers.ContainerRepository
import com.xxiao.ilemu.containers.ContainerTasks
import com.xxiao.ilemu.firmware.FirmwareMetadata
import com.xxiao.ilemu.ui.PhoneFrame
import com.xxiao.ilemu.ui.DeviceChassis
import org.libsdl.app.SDLActivity
import java.io.File

/** SDL remains the display/input host; Android chrome only sends public control commands. */
class EmulatorActivity : SDLActivity() {
    private var hostSensors: HostSensors? = null
    private var frame: PhoneFrame? = null
    private var screenshots: SessionScreenshots? = null
    private val controls by lazy { SessionControls(::sendCommand) }
    private var stopDialog: AlertDialog? = null
    private val containerId get() = intent.getStringExtra("containerId")
    private val density get() = resources.displayMetrics.density
    override fun getLibraries(): Array<String> = arrayOf("SDL2", "ilemu")
    override fun getArguments(): Array<String> {
        val arguments = intent.getStringArrayExtra("arguments") ?: arrayOf("help")
        val delimiter = if (arguments.firstOrNull() == "boot") arguments.indexOf("--") else -1
        val hostEnd = if (delimiter >= 0) delimiter else arguments.size
        val log = SessionLogs.file(this, containerId).also { it.parentFile?.mkdirs() }
        return arguments.take(hostEnd).toTypedArray() + arrayOf("--output", log.path) + arguments.drop(hostEnd).toTypedArray()
    }
    override fun attachBaseContext(newBase: Context) {
        val nightMode = when (AppSettings(newBase).appearance) {
            1 -> Configuration.UI_MODE_NIGHT_NO
            2 -> Configuration.UI_MODE_NIGHT_YES
            else -> null
        }
        val themed = nightMode?.let {
            val configuration = Configuration(newBase.resources.configuration)
            configuration.uiMode = (configuration.uiMode and Configuration.UI_MODE_NIGHT_MASK.inv()) or it
            newBase.createConfigurationContext(configuration)
        } ?: newBase
        super.attachBaseContext(themed)
    }
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        if (mBrokenLibraries) return
        ScreenUi(this).systemBars(this)
        val booting = intent.getStringArrayExtra("arguments")?.firstOrNull() == "boot"
        if (booting) hostSensors = HostSensors(this).also { it.start() }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU)
            onBackInvokedDispatcher.registerOnBackInvokedCallback(OnBackInvokedDispatcher.PRIORITY_DEFAULT, backCallback)
        val keepAwake = AppSettings(this).keepAwake
        // SDL disables the screensaver during video initialization by default.
        // Apply the host preference through its public hint before SDL_main starts.
        nativeSetenv("SDL_VIDEO_ALLOW_SCREENSAVER", if (keepAwake) "0" else "1")
        if (keepAwake) window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        else window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        val entry = containerId?.let { runCatching { ContainerRepository(this).get(it) }.getOrNull() }
        val args = intent.getStringArrayExtra("arguments").orEmpty()
        val device = entry?.device ?: args.indexOf("--device").takeIf { it >= 0 }?.let { args.getOrNull(it + 1) }
        val model = runCatching { FirmwareMetadata.models().firstOrNull { it.identifier == device } }.getOrNull()
        val aspect = model?.let { it.width.toFloat() / it.height }?.takeIf { it.isFinite() && it > 0 } ?: (2f / 3f)
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(MaterialColors.getColor(this@EmulatorActivity, com.google.android.material.R.attr.colorSurface, 0))
        }
        screenshots = SessionScreenshots(this, ::sendCommand)
        val toolbar = MaterialToolbar(this).apply {
            title = entry?.name ?: "iLEmu"
            listOf(R.drawable.ic_camera to "截图", R.drawable.ic_more to "更多操作", R.drawable.ic_close to "关闭模拟器").forEachIndexed { index, (icon, label) ->
                menu.add(0, index, index, label).apply {
                    this.icon = ContextCompat.getDrawable(this@EmulatorActivity, icon)?.mutate()?.apply {
                        setTint(MaterialColors.getColor(this@EmulatorActivity, com.google.android.material.R.attr.colorOnSurfaceVariant, 0))
                    }
                    contentDescription = label
                    tooltipText = label
                    setShowAsAction(android.view.MenuItem.SHOW_AS_ACTION_IF_ROOM)
                }
            }
            setOnMenuItemClickListener { item ->
                when (item.itemId) {
                    0 -> screenshots?.capture { result ->
                        if (!isFinishing && !isDestroyed) Toast.makeText(this@EmulatorActivity,
                            result.getOrElse { "截图失败：${it.message}" }, Toast.LENGTH_LONG).show()
                    }
                    1 -> showMenu()
                    2 -> confirmStop()
                }
                true
            }
        }
        root.addView(toolbar, LinearLayout.LayoutParams(-1, resources.getDimensionPixelSize(com.google.android.material.R.dimen.m3_appbar_size_compact)))
        (mSurface.parent as ViewGroup).removeView(mSurface)
        frame = PhoneFrame(this, mSurface, aspect, controls, DeviceChassis.forDevice(device))
        root.addView(frame, LinearLayout.LayoutParams(-1, 0, 1f))
        // Keep SDL's root and surface references intact for its IME/lifecycle callbacks.
        mLayout.addView(root, android.widget.RelativeLayout.LayoutParams(-1, -1))
        ViewCompat.setOnApplyWindowInsetsListener(root) { view, insets ->
            val bars = insets.getInsets(WindowInsetsCompat.Type.systemBars() or WindowInsetsCompat.Type.displayCutout())
            view.setPadding(bars.left, bars.top, bars.right, bars.bottom)
            insets
        }
        root.requestApplyInsets()
    }
    private fun dp(value: Int) = (value * density).toInt()
    private fun sendCommand(command: String): Boolean = NativeBridge.command(File(filesDir, "control.fifo").path, command, command == "quit")
    private fun showMenu() {
        MaterialAlertDialogBuilder(this).setTitle("容器操作")
            .setItems(arrayOf("查看运行日志", "运行配置", "关闭模拟器")) { _, which ->
                when (which) {
                    0 -> showLog()
                    1 -> {
                        val entry = containerId?.let { runCatching { ContainerRepository(this).get(it) }.getOrNull() }
                        MaterialAlertDialogBuilder(this).setTitle("运行配置").setMessage(
                            "${entry?.device ?: "诊断会话"}\n" +
                                (entry?.let { "iOS ${it.version} (${it.build})\n${it.cores} 核 · JIT ${it.jitCacheMiB} MiB\n\n关闭后可在容器设置中修改。" } ?: "")
                        ).setPositiveButton("关闭", null).show()
                    }
                    2 -> confirmStop()
                }
            }.setNegativeButton("返回画面", null).show()
    }
    private fun showLog() {
        val log = ScreenUi(this).text("", ScreenUi.Type.CAPTION).apply { typeface = android.graphics.Typeface.MONOSPACE; setTextIsSelectable(true)
            setPadding(dp(16), dp(12), dp(16), dp(12)); text = SessionLogs.tail(SessionLogs.file(this@EmulatorActivity, containerId))
        }
        val dialog = MaterialAlertDialogBuilder(this).setTitle("运行日志 · 最近 64 KiB")
            .setView(ScrollView(this).apply { addView(log) }).setPositiveButton("关闭", null).setNeutralButton("刷新", null).create()
        dialog.setOnShowListener { dialog.getButton(AlertDialog.BUTTON_NEUTRAL).setOnClickListener { log.text = SessionLogs.tail(SessionLogs.file(this, containerId)) } }
        dialog.show()
    }
    private fun confirmStop() {
        if (stopDialog?.isShowing == true) return
        frame?.releaseButtons()
        stopDialog = MaterialAlertDialogBuilder(this).setTitle("关闭模拟器？").setMessage("结束当前运行并保存容器数据。")
            .setNegativeButton("继续运行", null).setPositiveButton("关闭") { _, _ -> sendCommand("quit") }.show()
    }
    override fun onPause() {
        frame?.releaseButtons(); hostSensors?.stop()
        if (!mBrokenLibraries) sendCommand("quit")
        super.onPause()
    }
    private val backCallback by lazy { OnBackInvokedCallback { confirmStop() } }
    @SuppressLint("GestureBackNavigation")
    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (event.keyCode == KeyEvent.KEYCODE_BACK) {
            if (event.action == KeyEvent.ACTION_UP && !event.isCanceled) confirmStop()
            return true
        }
        return super.dispatchKeyEvent(event)
    }
    override fun onDestroy() {
        frame?.releaseButtons(); hostSensors?.stop(); screenshots?.close()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU && !mBrokenLibraries)
            onBackInvokedDispatcher.unregisterOnBackInvokedCallback(backCallback)
        try { super.onDestroy() } finally {
            NativeBridge.releaseActivity()
            containerId?.let { ContainerTasks.sessionEnded(applicationContext, it) }
        }
    }
    @SuppressLint("GestureBackNavigation")
    @Deprecated("Legacy Android back navigation")
    override fun onBackPressed() { confirmStop() }
}
