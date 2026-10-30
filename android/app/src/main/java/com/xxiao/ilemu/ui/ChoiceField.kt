package com.xxiao.ilemu.ui

import android.content.Context
import android.text.InputType
import com.google.android.material.textfield.MaterialAutoCompleteTextView
import com.google.android.material.textfield.TextInputLayout
import com.xxiao.ilemu.R

/** A non-editable Material dropdown; the controller owns the typed choices. */
internal class ChoiceField(context: Context) : TextInputLayout(context, null, R.attr.ilemuDropdownStyle) {
    private val input = MaterialAutoCompleteTextView(this.context).apply {
        inputType = InputType.TYPE_NULL
        isSaveEnabled = false
        setOnItemClickListener { _, _, position, _ ->
            selectedIndex = position
            onSelection?.invoke(position)
        }
    }
    private var labels = emptyList<String>()
    var selectedIndex = 0
        private set
    var onSelection: ((Int) -> Unit)? = null
    init { addView(input, LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT)) }
    fun setOptions(items: List<*>, selection: Int = 0) {
        labels = items.map { it.toString() }
        input.setSimpleItems(labels.toTypedArray())
        select(selection)
    }
    fun select(index: Int) {
        selectedIndex = index.coerceIn(0, (labels.size - 1).coerceAtLeast(0))
        input.setText(labels.getOrNull(selectedIndex).orEmpty(), false)
    }
}
