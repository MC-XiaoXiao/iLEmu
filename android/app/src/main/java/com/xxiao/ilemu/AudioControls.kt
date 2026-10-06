package com.xxiao.ilemu

import android.content.Context
import android.graphics.drawable.GradientDrawable
import android.graphics.drawable.LayerDrawable
import android.graphics.drawable.StateListDrawable
import android.view.Gravity
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ToggleButton

/** Physical controls delivered to the firmware through the live command channel. */
internal class AudioControls(context: Context, sendCommand: (String) -> Boolean) : LinearLayout(context) {
    init {
        orientation = HORIZONTAL
        listOf(R.string.audio_volume_down to "volume-down", R.string.audio_volume_up to "volume-up").forEach { (label, command) ->
            addView(Button(context).apply {
                setText(label)
                setOnClickListener { sendCommand(command) }
            }, LayoutParams(0, LayoutParams.MATCH_PARENT, 1f))
        }
        addView(ToggleButton(context).apply {
            val density = resources.displayMetrics.density
            fun indicatorShape(color: Int) = GradientDrawable().apply {
                setColor(context.getColor(color))
            }
            val indicator = StateListDrawable().apply {
                addState(intArrayOf(android.R.attr.state_checked), indicatorShape(R.color.ringer_silent))
                addState(intArrayOf(), indicatorShape(R.color.ringer_ring))
            }
            background = LayerDrawable(arrayOf(Button(context).background, indicator)).apply {
                paddingMode = LayerDrawable.PADDING_MODE_STACK
                val inset = (5 * density).toInt()
                setLayerHeight(1, (3 * density).toInt())
                setLayerGravity(1, Gravity.BOTTOM)
                setLayerInset(1, inset, 0, inset, (8 * density).toInt())
            }
            textOff = context.getString(R.string.audio_ring)
            textOn = context.getString(R.string.audio_mute)
            // Each native session starts with its physical switch in ring mode.
            isChecked = false
            setOnClickListener {
                if (!sendCommand(if (isChecked) "ringer silent" else "ringer ring")) {
                    isChecked = !isChecked
                }
            }
        }, LayoutParams(0, LayoutParams.MATCH_PARENT, 1f))
    }
}
