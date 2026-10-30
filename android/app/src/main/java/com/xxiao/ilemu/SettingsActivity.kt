package com.xxiao.ilemu

import com.google.android.material.dialog.MaterialAlertDialogBuilder
import android.os.Bundle
import androidx.appcompat.app.AppCompatActivity
import com.google.android.material.materialswitch.MaterialSwitch
import com.xxiao.ilemu.ui.ScreenUi

class SettingsActivity : AppCompatActivity() {
    private val launcher = SessionLauncher(this)
    override fun onCreate(savedInstanceState: Bundle?) {
        AppSettings(this).applyAppearance()
        super.onCreate(savedInstanceState)
        launcher.restore(savedInstanceState)
        val prefs = AppSettings(this)
        val ui = ScreenUi(this)
        val body = ui.screen(this, "App 设置", "适用于此设备上的所有容器")
        val appearance = ui.section(body, "外观与交互")
        val modes = listOf(-1, 1, 2)
        val appearances = arrayOf("跟随系统", "浅色", "深色")
        ui.preference(appearance, "外观", appearances[modes.indexOf(prefs.appearance).coerceAtLeast(0)]) { _ ->
            MaterialAlertDialogBuilder(this).setTitle("外观")
                .setSingleChoiceItems(appearances, modes.indexOf(prefs.appearance)) { dialog, which ->
                    dialog.dismiss(); prefs.appearance = modes[which]
                }.setNegativeButton("取消", null).show()
        }
        fun toggle(label: String, checked: Boolean, change: (Boolean) -> Unit) {
            appearance.addView(MaterialSwitch(this).apply {
                text = label; isChecked = checked; minimumHeight = ui.dp(56)
                setOnCheckedChangeListener { _, value -> change(value) }
            })
        }
        toggle("运行时保持屏幕常亮", prefs.keepAwake) { prefs.keepAwake = it }
        toggle("硬件按钮触觉反馈", prefs.haptics) { prefs.haptics = it }
        val defaults = ui.section(body, "新容器默认值")
        defaults.addView(ui.text("仅用于之后创建的容器。", ScreenUi.Type.BODY, true))
        fun options(label: String, items: List<Int>, current: Int, save: (Int) -> Unit) {
            ui.preference(defaults, label, current.toString()) { summary ->
                MaterialAlertDialogBuilder(this).setTitle(label)
                    .setSingleChoiceItems(items.map(Int::toString).toTypedArray(), items.indexOf(summary.text.toString().toInt())) { dialog, which ->
                        save(items[which]); summary.text = items[which].toString(); dialog.dismiss()
                    }.setNegativeButton("取消", null).show()
            }
        }
        options("CPU 核数", (1..8).toList(), prefs.defaultCores) { prefs.defaultCores = it }
        options("每执行器 JIT 缓存（MiB）", listOf(8,16,32,64,128), prefs.defaultJit) { prefs.defaultJit = it }
        val about = ui.section(body, "关于")
        ui.label(about, "iLEmu ${BuildConfig.VERSION_NAME}")
        ui.preference(about, "诊断与主机日志", "查看 App 日志与诊断信息") { _ ->
            DiagnosticsDialog.show(this) { launcher.launch(SessionSettings.smokeArguments()) }
        }
    }
    override fun onSaveInstanceState(outState: Bundle) { launcher.save(outState); super.onSaveInstanceState(outState) }
}
