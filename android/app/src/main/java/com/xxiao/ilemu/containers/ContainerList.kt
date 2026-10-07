package com.xxiao.ilemu.containers

import android.content.Context
import android.widget.Button
import android.widget.LinearLayout
import android.widget.TextView
import com.google.android.material.card.MaterialCardView
import com.xxiao.ilemu.firmware.DownloadState

internal class ContainerList(private val context: Context, private val list: LinearLayout) {
    fun render(entries: List<Container>, start: (Container) -> Unit, settings: (Container) -> Unit,
        download: (Container) -> Unit, pause: () -> Unit, delete: (Container) -> Unit) {
        // Avoid replacing focused controls on every progress update.
        val signature = entries.joinToString { it.json().toString() + ContainerTasks.isBusy(it.id) +
            if (it.ready) "" else DownloadState.read(ContainerRepository(context), it).toString() }
        if (list.tag == signature) return
        list.tag = signature
        list.removeAllViews()
        entries.forEach { entry ->
            val busy = ContainerTasks.isBusy(entry.id)
            val content = LinearLayout(context).apply {
                orientation = LinearLayout.VERTICAL
                val padding = (12 * resources.displayMetrics.density).toInt()
                setPadding(padding, padding, padding, padding)
            }
            content.addView(TextView(context).apply { text = entry.name; textSize = 22f })
            content.addView(TextView(context).apply {
                text = "${entry.device} · iOS ${entry.version} (${entry.build})\n" +
                    (if (entry.ready) (if (busy) "运行或同步中" else "运行固件已就绪")
                    else DownloadState.read(ContainerRepository(context), entry).toString()) +
                    (if (entry.storageTree.isEmpty()) "\nApp 存储" else "\n所选目录存储" ) +
                    (if (entry.dirty) " · 数据等待同步" else "") +
                    entry.error.takeIf { it.isNotEmpty() }?.let { "\n$it" }.orEmpty()
            })
            val actions = LinearLayout(context)
            fun button(label: String, enabled: Boolean = !busy, action: () -> Unit) {
                actions.addView(Button(context).apply { text = label; isEnabled = enabled; setOnClickListener { action() } },
                    LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
            }
            if (entry.ready) button("启动") { start(entry) }
            else if (busy) button("暂停", true) { pause() }
            else button("继续") { download(entry) }
            button("设置") { settings(entry) }
            button("删除") { delete(entry) }
            content.addView(actions)
            list.addView(MaterialCardView(context).apply { addView(content) }, LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { bottomMargin = 24 })
        }
    }
}
