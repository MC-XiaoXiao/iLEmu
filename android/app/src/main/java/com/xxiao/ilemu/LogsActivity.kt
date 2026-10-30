package com.xxiao.ilemu

import android.os.Bundle
import android.graphics.Typeface
import androidx.appcompat.app.AppCompatActivity
import com.xxiao.ilemu.containers.ContainerRepository
import com.xxiao.ilemu.ui.ScreenUi

class LogsActivity : AppCompatActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        AppSettings(this).applyAppearance(); super.onCreate(savedInstanceState)
        val id = intent.getStringExtra("id") ?: run { finish(); return }
        val entry = runCatching { ContainerRepository(this).get(id) }.getOrElse { finish(); return }
        val ui = ScreenUi(this)
        val body = ui.screen(this, "运行日志", entry.name + " · 最近 64 KiB")
        val log = ui.text("", ScreenUi.Type.CAPTION).apply { typeface = Typeface.MONOSPACE; setTextIsSelectable(true) }
        body.addView(ui.button("刷新日志") { log.text = SessionLogs.tail(SessionLogs.file(this, id)) })
        body.addView(log)
        log.text = SessionLogs.tail(SessionLogs.file(this, id))
    }
}
