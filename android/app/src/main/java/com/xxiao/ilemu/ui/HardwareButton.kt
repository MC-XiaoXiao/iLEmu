package com.xxiao.ilemu.ui

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RectF
import android.view.HapticFeedbackConstants
import android.view.MotionEvent
import android.widget.Toast
import androidx.appcompat.widget.AppCompatButton
import com.xxiao.ilemu.AppSettings

internal enum class KeyShape { Home, Side, Top, RockerUpper, RockerLower }

/** Physical-size artwork inside a transparent, accessible 48dp touch target. */
@SuppressLint("ViewConstructor")
internal class HardwareButton(context: Context, label: String, private val key: String,
    private val send: (String) -> Boolean, private val shape: KeyShape,
    private val inwardDirection: Float = 1f) : AppCompatButton(context) {
    private var down = false
    private val density = resources.displayMetrics.density
    private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val outline = RectF()
    private val keyOutline = Path()
    private val highlight = PressHighlight(this)
    private var artworkWidth = 0f
    private var artworkHeight = 0f
    fun setArtworkSize(width: Float, height: Float) { artworkWidth = width; artworkHeight = height }
    init {
        text = ""; contentDescription = label
        minWidth = 0; minimumWidth = 0; minHeight = 0; minimumHeight = 0
        setPadding(0, 0, 0, 0); setBackgroundColor(Color.TRANSPARENT)
        // Accessibility and keyboard activation inject a complete firmware press.
        setOnClickListener {
            feedback(true); postDelayed({ feedback(false) }, 100)
            if (AppSettings(context).haptics) performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP)
            if (!send(key)) unavailable()
        }
    }
    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        val cx = width / 2f; val cy = height / 2f
        val w = artworkWidth; val h = artworkHeight
        canvas.save()
        // Only the exposed artwork sinks into the enclosure; the touch target stays fixed.
        val inset = minOf(density, if (shape == KeyShape.Top) h * .25f else w * .2f) * highlight.progress
        when (shape) {
            KeyShape.Home -> Unit
            KeyShape.Top -> canvas.translate(0f, inset)
            else -> canvas.translate(inset * inwardDirection, 0f)
        }
        outline.set(cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2)
        val radius = if (shape == KeyShape.Home) w / 2 else 1.5f * density
        paint.style = Paint.Style.FILL
        paint.color = highlight.color(if (shape == KeyShape.Home) 0xff1b1b1b.toInt() else 0xffa0a0a0.toInt(), 0xffdedede.toInt())
        if (shape == KeyShape.Home) canvas.drawRoundRect(outline, radius, radius, paint)
        else {
            val top = if (shape != KeyShape.RockerLower) radius else 0f
            val bottom = if (shape != KeyShape.RockerUpper) radius else 0f
            val radii = if (shape == KeyShape.Top) {
                // Keep the root square and deep enough to meet a curved enclosure edge.
                outline.bottom += radius * 2
                floatArrayOf(radius, radius, radius, radius, 0f, 0f, 0f, 0f)
            } else if (inwardDirection > 0) floatArrayOf(top, top, 0f, 0f, 0f, 0f, bottom, bottom)
            else floatArrayOf(0f, 0f, top, top, bottom, bottom, 0f, 0f)
            keyOutline.reset(); keyOutline.addRoundRect(outline, radii, Path.Direction.CW)
            canvas.drawPath(keyOutline, paint)
        }
        if (shape == KeyShape.Home) {
            paint.style = Paint.Style.STROKE; paint.strokeWidth = density
            paint.color = 0xff626262.toInt(); canvas.drawRoundRect(outline, radius, radius, paint)
            val half = w * .14f
            paint.color = 0xffb0b0b0.toInt()
            canvas.drawRoundRect(cx - half, cy - half, cx + half, cy + half, 2 * density, 2 * density, paint)
        }
        canvas.restore()
    }
    private fun feedback(pressed: Boolean) {
        isPressed = pressed; invalidate()
        highlight.set(pressed)
    }
    private fun unavailable() { Toast.makeText(context, "模拟器尚未就绪，请稍候", Toast.LENGTH_SHORT).show() }
    fun release() {
        if (down) send("button $key up")
        down = false; feedback(false)
    }
    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(event: MotionEvent): Boolean {
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                if (!isEnabled) return false
                if (!send("button $key down")) { unavailable(); return true }
                down = true; feedback(true)
                if (AppSettings(context).haptics) performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP)
                parent.requestDisallowInterceptTouchEvent(true)
                return true
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> { release(); return true }
            MotionEvent.ACTION_MOVE -> {
                if (event.x < 0 || event.y < 0 || event.x > width || event.y > height) release()
                return true
            }
        }
        return true
    }
    override fun onDetachedFromWindow() { release(); highlight.close(); super.onDetachedFromWindow() }
}
