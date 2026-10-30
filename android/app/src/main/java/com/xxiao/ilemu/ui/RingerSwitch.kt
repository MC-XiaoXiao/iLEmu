package com.xxiao.ilemu.ui

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RectF
import android.view.HapticFeedbackConstants
import android.view.accessibility.AccessibilityNodeInfo
import android.widget.Toast
import androidx.appcompat.widget.AppCompatButton
import com.xxiao.ilemu.AppSettings
import com.xxiao.ilemu.SessionControls

/** Standard Button handles click cancellation and accessibility; artwork stays physical-size. */
@SuppressLint("ViewConstructor")
internal class RingerSwitch(context: Context, controls: SessionControls, private val rightSide: Boolean) : AppCompatButton(context) {
    private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val outline = RectF()
    private val clip = Path()
    private lateinit var highlight: PressHighlight
    private var silent = false
    private var artworkWidth = 0f
    private var artworkHeight = 0f
    init {
        highlight = PressHighlight(this)
        text = ""; setPadding(0, 0, 0, 0); setBackgroundColor(Color.TRANSPARENT)
        minWidth = 0; minimumWidth = 0; minHeight = 0; minimumHeight = 0
        contentDescription = "静音开关，响铃"
        setOnClickListener {
            if (AppSettings(context).haptics) performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP)
            if (!controls.toggleRinger()) Toast.makeText(context, "模拟器尚未就绪，请稍候", Toast.LENGTH_SHORT).show()
        }
    }
    fun update(silent: Boolean) {
        this.silent = silent
        contentDescription = if (silent) "静音开关，静音" else "静音开关，响铃"
        invalidate()
    }
    fun setArtworkSize(width: Float, height: Float) { artworkWidth = width; artworkHeight = height }
    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        val x = width / 2f; val y = height / 2f
        val w = artworkWidth; val h = artworkHeight
        outline.set(x - w / 2, y - h / 2, x + w / 2, y + h / 2)
        paint.color = highlight.color(0xff686868.toInt(), 0xffb0b0b0.toInt())
        canvas.drawRoundRect(outline, w / 2, w / 2, paint)
        // Half is hidden beneath the enclosure. Color the left half of the exposed part.
        clip.reset(); clip.addRoundRect(outline, w / 2, w / 2, Path.Direction.CW)
        canvas.save(); canvas.clipPath(clip)
        paint.color = if (silent) 0xffe56a6a.toInt() else 0xff65bc82.toInt()
        val exposedLeft = if (rightSide) x else x - w / 2
        canvas.drawRect(exposedLeft, y - h / 2, exposedLeft + w / 4, y + h / 2, paint)
        canvas.restore()
    }
    override fun onInitializeAccessibilityNodeInfo(info: AccessibilityNodeInfo) {
        super.onInitializeAccessibilityNodeInfo(info)
        info.className = "android.widget.Switch"; info.isCheckable = true; info.isChecked = silent
    }
    override fun drawableStateChanged() {
        super.drawableStateChanged()
        if (::highlight.isInitialized) highlight.set(isPressed)
    }
    override fun onDetachedFromWindow() { highlight.close(); super.onDetachedFromWindow() }
}
