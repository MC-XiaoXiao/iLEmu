package com.xxiao.ilemu.ui

import android.view.Gravity
import android.view.View
import android.widget.LinearLayout
import com.google.android.material.card.MaterialCardView
import com.google.android.material.progressindicator.LinearProgressIndicator
import com.xxiao.ilemu.ContainerActions
import com.xxiao.ilemu.NativeBridge
import com.xxiao.ilemu.R
import com.xxiao.ilemu.containers.Container
import com.xxiao.ilemu.containers.ContainerRepository
import com.xxiao.ilemu.containers.ContainerTasks
import com.xxiao.ilemu.firmware.DownloadState
import com.xxiao.ilemu.firmware.FirmwareMetadata

/** Read-only summary with independent controls; only the play icon starts a session. */
internal class ContainerCard(private val ui: ScreenUi, parent: LinearLayout, private val actions: ContainerActions) {
    private data class Presentation(val name: String, val device: String, val version: String, val build: String,
        val ready: Boolean, val canPrepare: Boolean, val busy: Boolean, val running: Boolean, val status: String, val progress: Int?)
    private val models = FirmwareMetadata.models().associate { it.identifier to it.name }
    private var rendered: Presentation? = null
    private var entry: Container? = null
    private val group = ui.column().also { parent.addView(it, LinearLayout.LayoutParams(-1, -2).apply { bottomMargin = ui.dp(16) }) }
    private val content = ui.card(group)
    private val card = content.parent as MaterialCardView
    private val title = ui.heading("", ScreenUi.Type.SUBTITLE)
    private val identity = ui.text("", ScreenUi.Type.BODY, true)
    private val configuration = ui.text("", ScreenUi.Type.CAPTION, true)
    private val status = ui.text("", ScreenUi.Type.BODY)
    private val progress = LinearProgressIndicator(ui.context).apply { visibility = View.GONE }
    private val rail = ui.row().apply { gravity = Gravity.END }
    private val primary = icon(R.drawable.ic_play, "启动") { actions.primary(it) }
    private val settings = icon(R.drawable.ic_settings, "设置") { actions.settings(it) }
    private val logs = icon(R.drawable.ic_logs, "日志") { actions.logs(it) }
    private val delete = icon(R.drawable.ic_delete, "删除") { actions.delete(it) }
    init {
        content.setPadding(ui.dp(16), ui.dp(16), ui.dp(16), ui.dp(8))
        content.importantForAccessibility = View.IMPORTANT_FOR_ACCESSIBILITY_NO
        listOf(title, identity, configuration, status).forEach { it.importantForAccessibility = View.IMPORTANT_FOR_ACCESSIBILITY_NO }
        content.addView(title)
        content.addView(identity, LinearLayout.LayoutParams(-1, -2).apply { topMargin = ui.dp(8) })
        content.addView(configuration, LinearLayout.LayoutParams(-1, -2).apply { topMargin = ui.dp(8) })
        content.addView(status, LinearLayout.LayoutParams(-1, -2).apply { topMargin = ui.dp(8) })
        content.addView(progress, LinearLayout.LayoutParams(-1, -2).apply { topMargin = ui.dp(12) })
        (card.layoutParams as LinearLayout.LayoutParams).bottomMargin = 0
        (delete.layoutParams as LinearLayout.LayoutParams).marginStart = ui.dp(12)
        content.addView(rail, LinearLayout.LayoutParams(-1, ui.dp(48)).apply { topMargin = ui.dp(8) })
    }
    private fun icon(resource: Int, label: String, action: (Container) -> Unit) = ui.iconButton(resource, label) {
        entry?.let(action)
    }.also { rail.addView(it, LinearLayout.LayoutParams(ui.dp(48), ui.dp(48))) }
    fun update(value: Container, repository: ContainerRepository) {
        entry = value
        val busy = ContainerTasks.isBusy(value.id)
        val running = value.ready && busy && NativeBridge.ownsRunningSession(value.id)
        val download = if (!value.ready || value.firmware != null) DownloadState.read(repository, value) else null
        val state = when {
            busy -> if (value.ready) (if (running) "运行中" else "正在同步数据…") else download!!.summary
            value.error.isNotEmpty() -> value.error
            value.dirty -> "数据等待同步"
            value.ready -> download?.warning.orEmpty()
            else -> download!!.summary
        }
        val presentation = Presentation(value.name, value.device, value.version, value.build,
            value.ready, value.firmware != null, busy, running, state, download?.progressPercent)
        if (rendered == presentation) return
        rendered = presentation
        title.text = value.name
        identity.text = models[value.device]?.let { "$it(${value.device})" } ?: value.device
        configuration.text = "iOS ${value.version.ifEmpty { "—" }}" + value.build.takeIf { it.isNotEmpty() }?.let { "($it)" }.orEmpty()
        status.text = state; status.visibility = if (state.isEmpty()) View.GONE else View.VISIBLE
        val preparing = busy && !value.ready
        progress.visibility = if (preparing) View.VISIBLE else View.GONE
        if (preparing) {
            progress.isIndeterminate = presentation.progress == null
            presentation.progress?.let { progress.setProgressCompat(it, true) }
            progress.contentDescription = state
        }
        card.isClickable = false; card.isFocusable = true
        card.contentDescription = listOf(value.name, identity.text, configuration.text, state).filter { it.isNotEmpty() }.joinToString("，")
        val (image, label) = when {
            running -> R.drawable.ic_stop to "停止"
            value.ready -> R.drawable.ic_play to "启动"
            busy -> R.drawable.ic_pause to "暂停准备"
            else -> R.drawable.ic_download to "继续准备"
        }
        primary.setIconResource(image); primary.contentDescription = "$label ${value.name}"; primary.tooltipText = label
        primary.isEnabled = if (value.ready) !busy || running else busy || value.firmware != null
        settings.isEnabled = !busy; delete.isEnabled = !busy
        listOf(primary, settings, logs, delete).forEach { it.alpha = if (it.isEnabled) 1f else .3f }
        settings.contentDescription = "设置 ${value.name}"; logs.contentDescription = "日志 ${value.name}"; delete.contentDescription = "删除 ${value.name}"
    }
}
