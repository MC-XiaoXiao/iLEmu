package com.xxiao.ilemu.ui

import android.animation.ValueAnimator
import android.view.View
import androidx.core.graphics.ColorUtils

/** Shared highlight timing; callers can also use the progress for mechanical feedback. */
internal class PressHighlight(private val view: View) {
    private var amount = 0f
    private var pressed = false
    private var animator: ValueAnimator? = null
    val progress get() = amount
    fun set(value: Boolean) {
        if (pressed == value) return
        pressed = value
        animator?.cancel()
        animator = ValueAnimator.ofFloat(amount, if (value) 1f else 0f).apply {
            duration = if (value) 65L else 150L
            addUpdateListener { amount = it.animatedValue as Float; view.invalidate() }
            start()
        }
    }
    fun color(idle: Int, highlighted: Int) = ColorUtils.blendARGB(idle, highlighted, amount)
    fun close() { animator?.cancel(); animator = null; amount = 0f; pressed = false }
}
