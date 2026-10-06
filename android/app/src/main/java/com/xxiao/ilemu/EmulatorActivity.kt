package com.xxiao.ilemu

import android.annotation.SuppressLint
import android.os.Build
import android.os.Bundle
import android.window.OnBackInvokedCallback
import android.window.OnBackInvokedDispatcher
import android.view.Gravity
import android.view.KeyEvent
import android.view.View
import android.view.WindowManager
import android.widget.Button
import android.widget.LinearLayout
import android.widget.RelativeLayout
import java.io.File
import org.libsdl.app.SDLActivity

class EmulatorActivity : SDLActivity() {
    private var hostSensors: HostSensors? = null
    override fun getLibraries(): Array<String> = arrayOf("SDL2", "ilemu")

    override fun getArguments(): Array<String> {
        val arguments = intent.getStringArrayExtra("arguments") ?: arrayOf("help")
        return arguments + arrayOf("--output", File(filesDir, "session.log").path)
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        if (mBrokenLibraries) return
        if (intent.getStringArrayExtra("arguments")?.firstOrNull() == "boot") {
            hostSensors = HostSensors(this).also { it.start() }
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            onBackInvokedDispatcher.registerOnBackInvokedCallback(
                OnBackInvokedDispatcher.PRIORITY_DEFAULT, backCallback)
        }
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        val height = (56 * resources.displayMetrics.density).toInt()
        val controls = LinearLayout(this).apply {
            id = View.generateViewId()
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER
            setBackgroundColor(0xff202124.toInt())
        }
        controls.addView(AudioControls(this, ::sendCommand), LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, (48 * resources.displayMetrics.density).toInt()))
        val buttons = LinearLayout(this)
        listOf(R.string.home to "home", R.string.lock to "lock", R.string.stop to "quit").forEach { (label, command) ->
            buttons.addView(Button(this).apply {
                setText(label)
                setOnClickListener { sendCommand(command) }
            }, LinearLayout.LayoutParams(0, height, 1f))
        }
        controls.addView(buttons, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, height))
        val container = mLayout as RelativeLayout
        container.addView(controls, RelativeLayout.LayoutParams(RelativeLayout.LayoutParams.MATCH_PARENT, RelativeLayout.LayoutParams.WRAP_CONTENT).apply {
            addRule(RelativeLayout.ALIGN_PARENT_BOTTOM)
        })
        mSurface.layoutParams = RelativeLayout.LayoutParams(RelativeLayout.LayoutParams.MATCH_PARENT, RelativeLayout.LayoutParams.MATCH_PARENT).apply {
            addRule(RelativeLayout.ABOVE, controls.id)
        }
    }

    private fun sendCommand(command: String): Boolean {
        return NativeBridge.command(File(filesDir, "control.fifo").path, command, command == "quit")
    }

    override fun onPause() {
        hostSensors?.stop()
        if (!mBrokenLibraries) sendCommand("quit")
        super.onPause()
    }

    private val backCallback by lazy { OnBackInvokedCallback { sendCommand("quit") } }

    @SuppressLint("GestureBackNavigation") // Hardware keys only; gestures use backCallback.
    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        // SDL's focused surface consumes hardware Back before Activity's
        // legacy callback. Keep it on the same stop path as system gestures.
        if (event.keyCode == KeyEvent.KEYCODE_BACK) {
            if (event.action == KeyEvent.ACTION_UP && !event.isCanceled) sendCommand("quit")
            return true
        }
        return super.dispatchKeyEvent(event)
    }

    override fun onDestroy() {
        hostSensors?.stop()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU && !mBrokenLibraries) {
            onBackInvokedDispatcher.unregisterOnBackInvokedCallback(backCallback)
        }
        try {
            super.onDestroy()
        } finally {
            NativeBridge.releaseActivity()
        }
    }

    @SuppressLint("GestureBackNavigation") // Legacy fallback; modern gestures use backCallback.
    @Deprecated("Legacy Android back navigation")
    override fun onBackPressed() {
        sendCommand("quit")
    }
}
