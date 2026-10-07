package com.xxiao.ilemu

import android.widget.ScrollView
import android.widget.TextView
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import java.io.File
import java.io.RandomAccessFile

internal object DiagnosticsDialog {
    fun show(activity: AppCompatActivity, smoke: () -> Unit) {
        val text = listOf("session.log", "native.log").mapNotNull { name ->
            val file = File(activity.filesDir, name)
            if (!file.isFile) return@mapNotNull null
            runCatching {
                RandomAccessFile(file, "r").use { input ->
                    input.seek((input.length() - 32768).coerceAtLeast(0))
                    val bytes = ByteArray((input.length() - input.filePointer).toInt())
                    input.readFully(bytes)
                    "$name\n${bytes.toString(Charsets.UTF_8)}"
                }
            }.getOrElse { "$name: ${it.message}" }
        }.joinToString("\n\n").ifEmpty { activity.getString(R.string.no_log) }
        val view = TextView(activity).apply { setTextIsSelectable(true); setPadding(24, 16, 24, 16); this.text = text }
        AlertDialog.Builder(activity).setTitle("诊断与运行日志")
            .setView(ScrollView(activity).apply { addView(view) })
            .setPositiveButton("关闭", null).setNeutralButton("检查 CPU") { _, _ -> smoke() }.show()
    }
}
