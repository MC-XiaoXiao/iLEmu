package com.xxiao.ilemu.ui

import android.view.View
import android.view.ViewGroup
import android.widget.LinearLayout
import androidx.recyclerview.widget.DiffUtil
import androidx.recyclerview.widget.RecyclerView
import com.xxiao.ilemu.ContainerActions
import com.xxiao.ilemu.containers.Container
import com.xxiao.ilemu.containers.ContainerRepository

/** The library creates card views only as they enter the viewport. */
internal class ContainerList(
    private val ui: ScreenUi,
    private val header: LinearLayout,
    private val actions: ContainerActions,
    private val repository: ContainerRepository
) : RecyclerView.Adapter<ContainerList.Holder>() {
    private data class Row(val container: Container, val presentation: ContainerCard.Presentation,
        val savedState: String)
    private var rows = emptyList<Row>()

    open class Holder(view: View) : RecyclerView.ViewHolder(view)
    private inner class CardHolder(body: LinearLayout) : Holder(body) {
        val card = ContainerCard(ui, body, actions)
    }

    override fun getItemCount() = 1 + rows.size.coerceAtLeast(1)
    override fun getItemViewType(position: Int) = when {
        position == 0 -> 0
        rows.isEmpty() -> 1
        else -> 2
    }
    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): Holder {
        val body = if (viewType == 0) header else ui.column()
        body.layoutParams = RecyclerView.LayoutParams(-1, -2)
        return when (viewType) {
            0 -> Holder(body)
            1 -> Holder(body).also {
                ui.label(ui.card(body), "从一部 iPhone 开始", "点击右下角的加号，选择固件或导入已准备的固件目录。")
            }
            else -> CardHolder(body)
        }
    }
    override fun onBindViewHolder(holder: Holder, position: Int) {
        if (holder is CardHolder) {
            val row = rows[position - 1]
            holder.card.update(row.container, row.presentation)
        }
    }

    fun update(entries: List<Container>) {
        val previous = rows
        val next = entries.map {
            Row(it, ContainerCard.presentation(it, repository), it.json().toString())
        }
        val changes = DiffUtil.calculateDiff(object : DiffUtil.Callback() {
            override fun getOldListSize() = 1 + previous.size.coerceAtLeast(1)
            override fun getNewListSize() = 1 + next.size.coerceAtLeast(1)
            override fun areItemsTheSame(oldPosition: Int, newPosition: Int): Boolean {
                if (oldPosition == 0 || newPosition == 0) return oldPosition == newPosition
                return previous.getOrNull(oldPosition - 1)?.container?.id ==
                    next.getOrNull(newPosition - 1)?.container?.id
            }
            override fun areContentsTheSame(oldPosition: Int, newPosition: Int): Boolean {
                if (oldPosition == 0) return true
                val old = previous.getOrNull(oldPosition - 1)
                val new = next.getOrNull(newPosition - 1)
                return old?.presentation == new?.presentation && old?.savedState == new?.savedState
            }
        })
        rows = next
        changes.dispatchUpdatesTo(this)
    }
}
