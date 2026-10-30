package com.xxiao.ilemu

import android.os.Bundle
import android.widget.ArrayAdapter
import androidx.appcompat.app.AppCompatActivity
import com.google.android.material.switchmaterial.SwitchMaterial
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
        appearance.addView(ui.button("外观 · " + when(prefs.appearance) { 1 -> "浅色"; 2 -> "深色"; else -> "跟随系统" }) {
            val modes = listOf(-1, 1, 2)
            androidx.appcompat.app.AlertDialog.Builder(this).setTitle("外观")
                .setSingleChoiceItems(arrayOf("跟随系统", "浅色", "深色"), modes.indexOf(prefs.appearance)) { dialog, which ->
                    dialog.dismiss(); prefs.appearance = modes[which]
                }.setNegativeButton("取消", null).show()
        })
        fun toggle(label: String, checked: Boolean, change: (Boolean) -> Unit) {
            appearance.addView(SwitchMaterial(this).apply {
                text = label; isChecked = checked; minimumHeight = ui.dp(56)
                setOnCheckedChangeListener { _, value -> change(value) }
            })
        }
        toggle("运行时保持屏幕常亮", prefs.keepAwake) { prefs.keepAwake = it }
        toggle("硬件按钮触觉反馈", prefs.haptics) { prefs.haptics = it }
        val defaults = ui.section(body, "新容器默认值")
        ui.label(defaults, "运行参数", "仅用于之后创建的容器，已有容器可单独调整。")
        fun options(label: String, items: List<Int>, current: Int, save: (Int) -> Unit) {
            val spinner = ui.spinner().apply {
                adapter = ArrayAdapter(this@SettingsActivity, android.R.layout.simple_spinner_dropdown_item, items)
                setSelection(items.indexOf(current).coerceAtLeast(0))
                onItemSelectedListener = object : android.widget.AdapterView.OnItemSelectedListener {
                    override fun onNothingSelected(parent: android.widget.AdapterView<*>?) = Unit
                    override fun onItemSelected(parent: android.widget.AdapterView<*>?, view: android.view.View?, position: Int, id: Long) { save(items[position]) }
                }
            }
            ui.field(defaults, label, spinner)
        }
        options("CPU 核数", (1..8).toList(), prefs.defaultCores) { prefs.defaultCores = it }
        options("每执行器 JIT 缓存（MiB）", listOf(8,16,32,64,128), prefs.defaultJit) { prefs.defaultJit = it }
        val about = ui.section(body, "关于")
        ui.label(about, "iLEmu ${BuildConfig.VERSION_NAME}", "固件独立运行 · 默认使用 Vulkan 渲染")
        about.addView(ui.button("诊断与主机日志") { DiagnosticsDialog.show(this) { launcher.launch(SessionSettings.smokeArguments()) } })
    }
    override fun onSaveInstanceState(outState: Bundle) { launcher.save(outState); super.onSaveInstanceState(outState) }
}
