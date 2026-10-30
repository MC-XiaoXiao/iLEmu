package com.xxiao.ilemu.ui

import android.content.res.ColorStateList
import android.view.Gravity
import android.view.View
import android.widget.LinearLayout
import androidx.appcompat.widget.AppCompatImageButton
import androidx.core.content.ContextCompat
import com.google.android.material.card.MaterialCardView
import com.xxiao.ilemu.ContainerActions
import com.xxiao.ilemu.NativeBridge
import com.xxiao.ilemu.R
import com.xxiao.ilemu.containers.Container
import com.xxiao.ilemu.containers.ContainerRepository
import com.xxiao.ilemu.containers.ContainerTasks
import com.xxiao.ilemu.firmware.DownloadState

/** Read-only summary with independent controls; only the play icon starts a session. */
internal class ContainerCard(private val ui: ScreenUi, parent: LinearLayout, private val actions: ContainerActions) {
    private var entry: Container? = null
    private val group = ui.column().also { parent.addView(it, LinearLayout.LayoutParams(-1, -2).apply { bottomMargin = ui.dp(16) }) }
    private val content = ui.card(group)
    private val card = content.parent as MaterialCardView
    private val title = ui.heading("", 20)
    private val identity = ui.text("", 14, true)
    private val configuration = ui.text("", 13, true)
    private val status = ui.text("", 13)
    private val rail = ui.row().apply { gravity = Gravity.END }
    private val primary = icon(R.drawable.ic_play, "启动") { actions.primary(it) }
    private val settings = icon(R.drawable.ic_settings, "设置") { actions.settings(it) }
    private val logs = icon(R.drawable.ic_logs, "日志") { actions.logs(it) }
    private val delete = icon(R.drawable.ic_delete, "删除") { actions.delete(it) }
    init {
        content.setPadding(ui.dp(18), ui.dp(18), ui.dp(18), ui.dp(8))
        content.importantForAccessibility = View.IMPORTANT_FOR_ACCESSIBILITY_NO
        listOf(title, identity, configuration, status).forEach { it.importantForAccessibility = View.IMPORTANT_FOR_ACCESSIBILITY_NO }
        content.addView(title)
        content.addView(identity, LinearLayout.LayoutParams(-1, -2).apply { topMargin = ui.dp(5) })
        content.addView(configuration, LinearLayout.LayoutParams(-1, -2).apply { topMargin = ui.dp(10) })
        content.addView(status, LinearLayout.LayoutParams(-1, -2).apply { topMargin = ui.dp(10) })
        (card.layoutParams as LinearLayout.LayoutParams).bottomMargin = 0
        (delete.layoutParams as LinearLayout.LayoutParams).marginStart = ui.dp(12)
        content.addView(rail, LinearLayout.LayoutParams(-1, ui.dp(48)).apply { topMargin = ui.dp(6) })
    }
    private fun icon(resource: Int, label: String, action: (Container) -> Unit) = AppCompatImageButton(ui.context).apply {
        setImageResource(resource); imageTintList = ColorStateList.valueOf(ui.ink)
        val value = android.util.TypedValue()
        ui.context.theme.resolveAttribute(android.R.attr.selectableItemBackgroundBorderless, value, true)
        background = ContextCompat.getDrawable(ui.context, value.resourceId)
        contentDescription = label; tooltipText = label
        setPadding(ui.dp(13), ui.dp(13), ui.dp(13), ui.dp(13))
        setOnClickListener { entry?.let(action) }
        rail.addView(this, LinearLayout.LayoutParams(ui.dp(48), ui.dp(48)))
    }
    fun update(value: Container, repository: ContainerRepository) {
        entry = value
        title.text = value.name
        identity.text = "${value.device}  ·  iOS ${value.version.ifEmpty { "—" }}"
        configuration.text = "${value.build}  ·  ${value.cores} 核"
        val busy = ContainerTasks.isBusy(value.id)
        val running = value.ready && busy && NativeBridge.ownsRunningSession(value.id)
        val state = when {
            busy -> if (value.ready) (if (running) "运行中" else "正在同步数据") else DownloadState.read(repository, value).toString()
            value.error.isNotEmpty() -> value.error
            value.dirty -> "数据等待同步"
            value.ready -> ""
            else -> DownloadState.read(repository, value).toString()
        }
        status.text = state; status.visibility = if (state.isEmpty()) View.GONE else View.VISIBLE
        card.isClickable = false; card.isFocusable = true
        card.contentDescription = listOf(value.name, identity.text, configuration.text, state).filter { it.isNotEmpty() }.joinToString("，")
        val (image, label) = when {
            running -> R.drawable.ic_stop to "停止"
            value.ready -> R.drawable.ic_play to "启动"
            busy -> R.drawable.ic_pause to "暂停准备"
            else -> R.drawable.ic_download to "继续准备"
        }
        primary.setImageResource(image); primary.contentDescription = "$label ${value.name}"; primary.tooltipText = label
        primary.isEnabled = if (value.ready) !busy || running else busy || value.firmware != null
        settings.isEnabled = !busy; delete.isEnabled = !busy
        listOf(primary, settings, logs, delete).forEach { it.alpha = if (it.isEnabled) 1f else .3f }
        settings.contentDescription = "设置 ${value.name}"; logs.contentDescription = "日志 ${value.name}"; delete.contentDescription = "删除 ${value.name}"
    }
}
