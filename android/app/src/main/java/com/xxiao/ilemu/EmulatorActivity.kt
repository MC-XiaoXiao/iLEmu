package com.xxiao.ilemu

import android.annotation.SuppressLint
import android.app.AlertDialog
import android.os.Build
import android.os.Bundle
import android.view.Gravity
import android.view.KeyEvent
import android.view.ViewGroup
import android.view.WindowManager
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import android.window.OnBackInvokedCallback
import android.window.OnBackInvokedDispatcher
import androidx.appcompat.widget.AppCompatImageButton
import android.content.res.ColorStateList
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
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        if (mBrokenLibraries) return
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
            setBackgroundColor(0xff101010.toInt())
        }
        val toolbar = LinearLayout(this).apply { gravity = Gravity.CENTER_VERTICAL; setPadding(dp(16), dp(8), dp(16), dp(8)) }
        toolbar.addView(TextView(this).apply {
            text = entry?.name ?: "iLEmu"
            textSize = 18f; setTextColor(0xffe7e7e7.toInt()); maxLines = 1
            ellipsize = android.text.TextUtils.TruncateAt.END
        }, LinearLayout.LayoutParams(0, -2, 1f))
        screenshots = SessionScreenshots(this, ::sendCommand)
        toolbar.addView(toolbarIcon(R.drawable.ic_camera, "截图") {
            screenshots?.capture { result ->
                if (!isFinishing && !isDestroyed) Toast.makeText(this,
                    result.getOrElse { "截图失败：${it.message}" }, Toast.LENGTH_LONG).show()
            }
        })
        toolbar.addView(toolbarIcon(R.drawable.ic_more, "更多操作", ::showMenu))
        toolbar.addView(toolbarIcon(R.drawable.ic_close, "关闭模拟器", ::confirmStop))
        root.addView(toolbar)
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
    private fun toolbarIcon(icon: Int, label: String, action: () -> Unit) = AppCompatImageButton(this).apply {
        setImageResource(icon); imageTintList = ColorStateList.valueOf(0xffdddddd.toInt())
        val value = android.util.TypedValue()
        theme.resolveAttribute(android.R.attr.selectableItemBackgroundBorderless, value, true)
        background = ContextCompat.getDrawable(this@EmulatorActivity, value.resourceId)
        setPadding(dp(13), dp(13), dp(13), dp(13))
        contentDescription = label; tooltipText = label
        layoutParams = LinearLayout.LayoutParams(dp(48), dp(48))
        setOnClickListener { action() }
    }
    private fun dp(value: Int) = (value * density).toInt()
    private fun sendCommand(command: String): Boolean = NativeBridge.command(File(filesDir, "control.fifo").path, command, command == "quit")
    private fun showMenu() {
        AlertDialog.Builder(this).setTitle("容器操作")
            .setItems(arrayOf("唤醒并解锁", "切换响铃 / 静音", "查看运行日志", "运行配置", "关闭模拟器")) { _, which ->
                when (which) {
                    0 -> if (!sendCommand("unlock")) Toast.makeText(this, "模拟器尚未就绪", Toast.LENGTH_SHORT).show()
                    1 -> showRinger()
                    2 -> showLog()
                    3 -> {
                        val entry = containerId?.let { runCatching { ContainerRepository(this).get(it) }.getOrNull() }
                        AlertDialog.Builder(this).setTitle("运行配置").setMessage(
                            "${entry?.device ?: "诊断会话"}\nVulkan · 已激活 · 隔离网络\n" +
                                (entry?.let { "${it.cores} 核 · JIT ${it.jitCacheMiB} MiB\n\n关闭后可在容器设置中修改。" } ?: "")
                        ).setPositiveButton("关闭", null).show()
                    }
                    4 -> confirmStop()
                }
            }.setNegativeButton("返回画面", null).show()
    }
    private fun showRinger() {
        AlertDialog.Builder(this).setTitle("响铃开关")
            .setSingleChoiceItems(arrayOf("响铃", "静音"), if (controls.ringerSilent) 1 else 0) { dialog, which ->
                if (!controls.setRinger(which == 1)) Toast.makeText(this, "模拟器尚未就绪", Toast.LENGTH_SHORT).show()
                dialog.dismiss()
            }.setNegativeButton("取消", null).show()
    }
    private fun showLog() {
        val log = TextView(this).apply {
            textSize = 11f; typeface = android.graphics.Typeface.MONOSPACE; setTextIsSelectable(true)
            setPadding(dp(16), dp(12), dp(16), dp(12)); text = SessionLogs.tail(SessionLogs.file(this@EmulatorActivity, containerId))
        }
        val dialog = AlertDialog.Builder(this).setTitle("运行日志 · 最近 64 KiB")
            .setView(ScrollView(this).apply { addView(log) }).setPositiveButton("关闭", null).setNeutralButton("刷新", null).create()
        dialog.setOnShowListener { dialog.getButton(AlertDialog.BUTTON_NEUTRAL).setOnClickListener { log.text = SessionLogs.tail(SessionLogs.file(this, containerId)) } }
        dialog.show()
    }
    private fun confirmStop() {
        if (stopDialog?.isShowing == true) return
        frame?.releaseButtons()
        stopDialog = AlertDialog.Builder(this).setTitle("关闭模拟器？").setMessage("结束当前运行并保存容器数据。")
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
