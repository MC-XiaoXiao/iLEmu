package com.xxiao.ilemu.ui

import android.content.ActivityNotFoundException
import android.content.Intent
import android.net.Uri
import android.text.SpannableString
import android.text.Spanned
import android.text.style.ForegroundColorSpan
import android.text.style.TextAppearanceSpan
import android.widget.LinearLayout
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.materialswitch.MaterialSwitch
import com.xxiao.ilemu.AppSettings
import com.xxiao.ilemu.BuildConfig
import com.xxiao.ilemu.R

/** Preference content shared with the main settings destination. */
internal class AppSettingsContent(activity: AppCompatActivity, body: LinearLayout, onDiagnostics: () -> Unit) {
    private val ui = ScreenUi(activity)
    init {
        val prefs = AppSettings(activity)
        val appearance = ui.section(body, "外观与交互")
        val modes = listOf(-1, 1, 2)
        val appearances = arrayOf("跟随系统", "浅色", "深色")
        ui.preference(appearance, "外观", appearances[modes.indexOf(prefs.appearance).coerceAtLeast(0)]) { _ ->
            MaterialAlertDialogBuilder(activity).setTitle("外观")
                .setSingleChoiceItems(appearances, modes.indexOf(prefs.appearance)) { dialog, which ->
                    dialog.dismiss(); prefs.appearance = modes[which]
                }.setNegativeButton("取消", null).show()
        }
        fun toggle(label: String, description: String, checked: Boolean, change: (Boolean) -> Unit) {
            appearance.addView(MaterialSwitch(activity).apply {
                text = SpannableString("$label\n$description").apply {
                    setSpan(TextAppearanceSpan(activity, com.google.android.material.R.style.TextAppearance_Material3_BodyMedium), label.length + 1, length, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
                    setSpan(ForegroundColorSpan(ui.muted), label.length + 1, length, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
                }
                isChecked = checked; minimumHeight = ui.dp(88)
                setTextAppearance(com.google.android.material.R.style.TextAppearance_Material3_TitleLarge)
                setOnCheckedChangeListener { _, value -> change(value) }
            })
        }
        toggle("运行时保持屏幕常亮", "模拟器运行时保持屏幕点亮", prefs.keepAwake) { prefs.keepAwake = it }
        toggle("硬件按钮触觉反馈", "模拟实体按键时提供振动反馈", prefs.haptics) { prefs.haptics = it }
        val defaults = ui.section(body, "新容器默认值")
        defaults.addView(ui.text("仅用于之后创建的容器。", ScreenUi.Type.BODY, true))
        fun options(label: String, items: List<Int>, current: Int, save: (Int) -> Unit) {
            ui.preference(defaults, label, current.toString()) { summary ->
                MaterialAlertDialogBuilder(activity).setTitle(label)
                    .setSingleChoiceItems(items.map(Int::toString).toTypedArray(), items.indexOf(summary.text.toString().toInt())) { dialog, which ->
                        save(items[which]); summary.text = items[which].toString(); dialog.dismiss()
                    }.setNegativeButton("取消", null).show()
            }
        }
        options("CPU 核数", (1..8).toList(), prefs.defaultCores) { prefs.defaultCores = it }
        options("每执行器 JIT 缓存（MiB）", listOf(8,16,32,64,128), prefs.defaultJit) { prefs.defaultJit = it }
        val about = ui.section(body, "关于")
        about.addView(ui.heading("iLEmu ${BuildConfig.VERSION_NAME}"))
        ui.preference(about, activity.getString(R.string.github_repository), "MC-XiaoXiao/iLEmu") { _ ->
            try {
                activity.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(activity.getString(R.string.github_repository_url))))
            } catch (_: ActivityNotFoundException) {
                Toast.makeText(activity, "没有可用的浏览器", Toast.LENGTH_SHORT).show()
            }
        }
        ui.preference(about, "诊断与主机日志", "查看 App 日志与诊断信息") { _ ->
            onDiagnostics()
        }
    }
}
