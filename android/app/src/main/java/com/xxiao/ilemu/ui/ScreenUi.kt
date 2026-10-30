package com.xxiao.ilemu.ui

import android.content.Context
import android.content.res.ColorStateList
import android.graphics.Typeface
import android.view.Gravity
import android.view.View
import android.widget.*
import androidx.appcompat.app.AppCompatActivity
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import com.google.android.material.button.MaterialButton
import com.google.android.material.card.MaterialCardView
import com.google.android.material.color.MaterialColors

/** Small shared vocabulary for the host application's screens. */
internal class ScreenUi(val context: Context) {
    fun dp(value: Int) = (value * context.resources.displayMetrics.density).toInt()
    val ink get() = MaterialColors.getColor(context, com.google.android.material.R.attr.colorOnSurface, 0xff242424.toInt())
    val muted get() = MaterialColors.getColor(context, com.google.android.material.R.attr.colorOnSurfaceVariant, 0xff707070.toInt())
    val accent get() = MaterialColors.getColor(context, com.google.android.material.R.attr.colorPrimary, 0xff303030.toInt())
    fun column(padding: Int = 0) = LinearLayout(context).apply {
        orientation = LinearLayout.VERTICAL
        setPadding(dp(padding), dp(padding), dp(padding), dp(padding))
    }
    fun row() = LinearLayout(context).apply { gravity = Gravity.CENTER_VERTICAL }
    fun text(value: String, size: Int = 15, secondary: Boolean = false) = TextView(context).apply {
        text = value; textSize = size.toFloat(); setTextColor(if (secondary) muted else ink)
        setLineSpacing(dp(3).toFloat(), 1f)
    }
    fun heading(value: String, size: Int = 24) = text(value, size).apply { setTypeface(typeface, Typeface.BOLD) }
    fun button(label: String, primary: Boolean = false, action: () -> Unit) = MaterialButton(
        context, null, if (primary) com.google.android.material.R.attr.materialButtonStyle
        else com.google.android.material.R.attr.materialButtonOutlinedStyle
    ).apply {
        text = label; isAllCaps = false; cornerRadius = dp(8); minHeight = dp(48)
        setOnClickListener { action() }
    }
    fun section(parent: LinearLayout, label: String): LinearLayout {
        parent.addView(heading(label, 14).apply { setTextColor(muted) }, LinearLayout.LayoutParams(-1, -2).apply {
            topMargin = dp(24); bottomMargin = dp(10)
        })
        return card(parent)
    }
    fun card(parent: LinearLayout): LinearLayout {
        val content = column(18)
        parent.addView(MaterialCardView(context).apply {
            radius = dp(10).toFloat(); cardElevation = 0f; strokeWidth = dp(1)
            strokeColor = MaterialColors.getColor(context, com.google.android.material.R.attr.colorOutlineVariant, 0xffdddddd.toInt())
            addView(content)
        }, LinearLayout.LayoutParams(-1, -2).apply { bottomMargin = dp(12) })
        return content
    }
    fun label(parent: LinearLayout, title: String, description: String = "") {
        parent.addView(heading(title, 16))
        if (description.isNotEmpty()) parent.addView(text(description, 13, true), LinearLayout.LayoutParams(-1, -2).apply { topMargin = dp(5) })
    }
    fun field(parent: LinearLayout, label: String, view: View) {
        parent.addView(text(label, 13, true), LinearLayout.LayoutParams(-1, -2).apply { topMargin = dp(12) })
        parent.addView(view, LinearLayout.LayoutParams(-1, -2).apply { bottomMargin = dp(6) })
    }
    fun spinner() = Spinner(context).apply { minimumHeight = dp(48) }
    fun edit(hint: String, max: Int) = EditText(context).apply {
        this.hint = hint; isSingleLine = true; textSize = 16f
        filters = arrayOf(android.text.InputFilter.LengthFilter(max))
        importantForAutofill = View.IMPORTANT_FOR_AUTOFILL_NO
    }
    fun screen(activity: AppCompatActivity, title: String, subtitle: String, back: Boolean = true): LinearLayout {
        val root = column()
        root.setBackgroundColor(MaterialColors.getColor(context, android.R.attr.colorBackground, 0xfff4f4f4.toInt()))
        val top = column(20)
        if (back) top.addView(button("‹  返回") { activity.onBackPressedDispatcher.onBackPressed() }, LinearLayout.LayoutParams(-2, -2))
        top.addView(heading(title, 30))
        if (subtitle.isNotEmpty()) top.addView(text(subtitle, 14, true))
        root.addView(top)
        val content = column(20).apply { setPadding(dp(20), 0, dp(20), dp(24)) }
        root.addView(ScrollView(context).apply { isFillViewport = true; addView(content) }, LinearLayout.LayoutParams(-1, 0, 1f))
        ViewCompat.setOnApplyWindowInsetsListener(root) { view, insets ->
            val bars = insets.getInsets(WindowInsetsCompat.Type.systemBars() or WindowInsetsCompat.Type.displayCutout())
            view.setPadding(bars.left, bars.top, bars.right, bars.bottom); insets
        }
        activity.setContentView(root)
        return content
    }
    fun notice(parent: LinearLayout) = text("", 14, true).apply {
        accessibilityLiveRegion = View.ACCESSIBILITY_LIVE_REGION_POLITE
        setTextIsSelectable(true); parent.addView(this)
    }
}
