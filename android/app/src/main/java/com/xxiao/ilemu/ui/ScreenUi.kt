package com.xxiao.ilemu.ui

import android.app.Activity
import android.content.Context
import android.view.Gravity
import android.view.View
import android.widget.*
import androidx.appcompat.app.AppCompatActivity
import androidx.appcompat.view.ContextThemeWrapper
import androidx.core.widget.doAfterTextChanged
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowCompat
import com.google.android.material.appbar.MaterialToolbar
import com.google.android.material.button.MaterialButton
import com.google.android.material.card.MaterialCardView
import com.google.android.material.divider.MaterialDivider
import com.google.android.material.color.MaterialColors
import com.google.android.material.textfield.TextInputEditText
import com.google.android.material.textfield.TextInputLayout
import com.google.android.material.textview.MaterialTextView
import com.xxiao.ilemu.R
import com.google.android.material.R as MaterialR

/** Shared Material 3 components for the host application's screens. */
internal class ScreenUi(val context: Context) {
    enum class Type(val appearance: Int) {
        TITLE(MaterialR.style.TextAppearance_Material3_TitleLarge),
        SUBTITLE(MaterialR.style.TextAppearance_Material3_TitleMedium),
        SECTION(MaterialR.style.TextAppearance_Material3_TitleSmall),
        BODY(MaterialR.style.TextAppearance_Material3_BodyMedium),
        LIST(MaterialR.style.TextAppearance_Material3_BodyLarge),
        CAPTION(MaterialR.style.TextAppearance_Material3_BodySmall)
    }
    fun dp(value: Int) = (value * context.resources.displayMetrics.density).toInt()
    val ink get() = MaterialColors.getColor(context, MaterialR.attr.colorOnSurface, 0xff242424.toInt())
    val muted get() = MaterialColors.getColor(context, MaterialR.attr.colorOnSurfaceVariant, 0xff626262.toInt())
    fun column(padding: Int = 0) = LinearLayout(context).apply {
        orientation = LinearLayout.VERTICAL
        setPadding(dp(padding), dp(padding), dp(padding), dp(padding))
    }
    fun row() = LinearLayout(context).apply { gravity = Gravity.CENTER_VERTICAL }
    fun text(value: String, type: Type = Type.BODY, secondary: Boolean = false) = MaterialTextView(context).apply {
        setTextAppearance(type.appearance)
        text = value; setTextColor(if (secondary) muted else ink)
    }
    fun heading(value: String, type: Type = Type.TITLE) = text(value, type)
    fun button(label: String, primary: Boolean = false, action: () -> Unit) = MaterialButton(
        context, null, if (primary) MaterialR.attr.materialButtonStyle else MaterialR.attr.materialButtonOutlinedStyle
    ).apply {
        text = label; minHeight = dp(48)
        setOnClickListener { action() }
    }
    fun iconButton(resource: Int, label: String, action: () -> Unit) = MaterialButton(context, null, R.attr.ilemuIconButtonStyle).apply {
        setIconResource(resource); contentDescription = label; tooltipText = label
        setOnClickListener { action() }
    }
    fun group(parent: LinearLayout) = column().also { parent.addView(it, LinearLayout.LayoutParams(-1, -2)) }
    fun section(parent: LinearLayout, label: String): LinearLayout {
        if (parent.childCount > 0) parent.addView(MaterialDivider(context), LinearLayout.LayoutParams(-1, -2).apply {
            topMargin = dp(24); bottomMargin = dp(16)
        })
        parent.addView(text(label, Type.SECTION).apply {
            setTextColor(MaterialColors.getColor(context, MaterialR.attr.colorPrimary, ink))
        }, LinearLayout.LayoutParams(-1, -2).apply { bottomMargin = dp(8) })
        return group(parent)
    }
    /** Material list item: headline, supporting text and the theme's standard state layer. */
    fun preference(parent: LinearLayout, title: String, summary: String, action: (MaterialTextView) -> Unit) {
        val supporting = text(summary, Type.BODY, true)
        val content = column().apply { addView(text(title, Type.LIST)); addView(supporting) }
        parent.addView(row().apply {
            minimumHeight = dp(72)
            setPadding(0, dp(8), 0, dp(8))
            val value = android.util.TypedValue()
            context.theme.resolveAttribute(android.R.attr.selectableItemBackground, value, true)
            setBackgroundResource(value.resourceId)
            isFocusable = true
            ViewCompat.setScreenReaderFocusable(this, true)
            addView(content, LinearLayout.LayoutParams(-1, -2))
            setOnClickListener { action(supporting) }
        }, LinearLayout.LayoutParams(-1, -2))
    }
    fun card(parent: LinearLayout): LinearLayout {
        val content = column(16)
        parent.addView(MaterialCardView(context).apply { addView(content) },
            LinearLayout.LayoutParams(-1, -2).apply { bottomMargin = dp(12) })
        return content
    }
    fun label(parent: LinearLayout, title: String, description: String = "") {
        parent.addView(heading(title, Type.SUBTITLE))
        if (description.isNotEmpty()) parent.addView(text(description, Type.BODY, true), LinearLayout.LayoutParams(-1, -2).apply { topMargin = dp(4) })
    }
    fun field(parent: LinearLayout, label: String, view: View) {
        val field = when (view) {
            is TextInputEditText -> TextInputLayout(context).apply {
                placeholderText = view.hint; view.hint = null
                addView(view, LinearLayout.LayoutParams(-1, -2))
            }
            else -> view
        }
        if (field is TextInputLayout) field.hint = label
        parent.addView(field, LinearLayout.LayoutParams(-1, -2).apply { topMargin = dp(16); bottomMargin = dp(8) })
    }
    fun choices() = ChoiceField(context)
    fun edit(hint: String, max: Int) = TextInputEditText(
        ContextThemeWrapper(context, MaterialR.style.ThemeOverlay_Material3_TextInputEditText_OutlinedBox)
    ).apply {
        this.hint = hint; isSingleLine = true
        filters = arrayOf(android.text.InputFilter.LengthFilter(max))
        importantForAutofill = View.IMPORTANT_FOR_AUTOFILL_NO
    }
    fun systemBars(activity: Activity) {
        val light = MaterialColors.isColorLight(MaterialColors.getColor(context, MaterialR.attr.colorSurface, 0))
        WindowCompat.getInsetsController(activity.window, activity.window.decorView).apply {
            isAppearanceLightStatusBars = light
            isAppearanceLightNavigationBars = light
        }
    }
    fun screen(activity: AppCompatActivity, title: String, subtitle: String, back: Boolean = true): LinearLayout {
        activity.setContentView(R.layout.host_screen)
        systemBars(activity)
        val root = activity.findViewById<View>(R.id.host_root)
        activity.findViewById<MaterialToolbar>(R.id.host_toolbar).apply {
            this.title = title
            if (!back) this.subtitle = subtitle
            if (back) {
                setNavigationIcon(R.drawable.ic_back)
                navigationContentDescription = "返回"
                setNavigationOnClickListener { activity.onBackPressedDispatcher.onBackPressed() }
            }
        }
        val content = activity.findViewById<LinearLayout>(R.id.host_content)
        if (back && subtitle.isNotEmpty()) content.addView(text(subtitle, Type.BODY, true), LinearLayout.LayoutParams(-1, -2).apply { bottomMargin = dp(24) })
        ViewCompat.setOnApplyWindowInsetsListener(root) { view, insets ->
            val bars = insets.getInsets(WindowInsetsCompat.Type.systemBars() or WindowInsetsCompat.Type.displayCutout())
            view.setPadding(bars.left, bars.top, bars.right, bars.bottom); insets
        }
        return content
    }
    fun notice(parent: LinearLayout) = text("", Type.BODY, true).apply {
        accessibilityLiveRegion = View.ACCESSIBILITY_LIVE_REGION_POLITE
        visibility = View.GONE
        doAfterTextChanged { visibility = if (it.isNullOrEmpty()) View.GONE else View.VISIBLE }
        setTextIsSelectable(true); parent.addView(this)
    }
}
