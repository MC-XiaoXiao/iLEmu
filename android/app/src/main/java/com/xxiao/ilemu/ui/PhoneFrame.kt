package com.xxiao.ilemu.ui

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Canvas
import android.graphics.RectF
import android.view.View
import android.view.ViewGroup
import com.xxiao.ilemu.SessionControls
import kotlin.math.min
import kotlin.math.abs

/** Model-specific vector front panel around an unchanged SDL display surface. */
@SuppressLint("ViewConstructor")
internal class PhoneFrame(context: Context, private val surface: View, private val aspect: Float,
    private val controls: SessionControls, private val chassis: DeviceChassis) : ViewGroup(context) {
    private val renderer = ChassisRenderer(chassis)
    private val shell = RectF()
    private val screen = RectF()
    private val density = resources.displayMetrics.density
    private val home = HardwareButton(context, "Home 键", "home", controls::command, KeyShape.Home)
    private val lock = HardwareButton(context, "电源 / 锁屏键", "lock", controls::command, KeyShape.Top)
    private val up = HardwareButton(context, "音量增加键", "volume-up", controls::command, chassis.volumeUp?.shape ?: KeyShape.Side,
        if ((chassis.volumeUp?.x ?: 0f) > .5f) -1f else 1f)
    private val down = HardwareButton(context, "音量减小键", "volume-down", controls::command, chassis.volumeDown?.shape ?: KeyShape.Side,
        if ((chassis.volumeDown?.x ?: 0f) > .5f) -1f else 1f)
    private val ringer = RingerSwitch(context, controls, (chassis.ringer?.x ?: 0f) > .5f)
    private val buttons = listOf(home, lock, up, down)
    init {
        setWillNotDraw(false); clipChildren = false
        addView(surface); buttons.forEach(::addView); addView(ringer)
        up.visibility = if (chassis.volumeUp == null) GONE else VISIBLE
        down.visibility = if (chassis.volumeDown == null) GONE else VISIBLE
        ringer.visibility = if (chassis.ringer == null) GONE else VISIBLE
        controls.onRingerChanged = ringer::update
        controls.onRingerChanged?.invoke(controls.ringerSilent)
    }
    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        val w = MeasureSpec.getSize(widthMeasureSpec); val h = MeasureSpec.getSize(heightMeasureSpec)
        setMeasuredDimension(w, h)
        val availableWidth = (w - 52 * density).coerceAtLeast(1f)
        val availableHeight = (h - 52 * density).coerceAtLeast(1f)
        val bodyWidth = min(availableWidth, availableHeight / chassis.height)
        val bodyHeight = bodyWidth * chassis.height
        val sw = bodyWidth * chassis.screenWidth
        val sh = sw / aspect
        shell.set((w - bodyWidth) / 2, (h - bodyHeight) / 2, (w + bodyWidth) / 2, (h + bodyHeight) / 2)
        screen.set((w - sw) / 2, shell.top + bodyWidth * chassis.screenTop,
            (w + sw) / 2, shell.top + bodyWidth * chassis.screenTop + sh)
        surface.measure(exact(screen.width().toInt()), exact(screen.height().toInt()))
        val sideKeys = listOfNotNull(chassis.ringer, chassis.volumeUp, chassis.volumeDown)
        fun targetHeight(key: ChassisKey): Int {
            val nearest = sideKeys.filter { it !== key && abs(it.x - key.x) < .1f }
                .minOfOrNull { abs(it.y - key.y) * bodyWidth } ?: Float.MAX_VALUE
            // Adjacent parts of a rocker must not steal each other's touch events.
            return min(maxOf(48 * density, bodyWidth * key.height + 8 * density), nearest).toInt()
        }
        fun key(view: HardwareButton, artworkWidth: Float, artworkHeight: Float, height: Int? = null) {
            view.setArtworkSize(artworkWidth, artworkHeight)
            view.measure(exact(maxOf(48 * density, artworkWidth + 8 * density).toInt()),
                exact(height ?: maxOf(48 * density, artworkHeight + 8 * density).toInt()))
        }
        key(home, bodyWidth * chassis.homeDiameter, bodyWidth * chassis.homeDiameter)
        key(lock, bodyWidth * chassis.power.width, bodyWidth * chassis.power.height)
        chassis.volumeUp?.let { key(up, bodyWidth * it.width, bodyWidth * it.height, targetHeight(it)) }
        chassis.volumeDown?.let { key(down, bodyWidth * it.width, bodyWidth * it.height, targetHeight(it)) }
        chassis.ringer?.let {
            ringer.setArtworkSize(bodyWidth * it.width, bodyWidth * it.height)
            ringer.measure(exact((48 * density).toInt()), exact(targetHeight(it)))
        }
    }
    private fun exact(value: Int) = MeasureSpec.makeMeasureSpec(value.coerceAtLeast(1), MeasureSpec.EXACTLY)
    override fun onLayout(changed: Boolean, left: Int, top: Int, right: Int, bottom: Int) {
        surface.layout(screen.left.toInt(), screen.top.toInt(), screen.right.toInt(), screen.bottom.toInt())
        place(home, shell.centerX(), (screen.bottom + shell.bottom) / 2)
        fun position(view: View, key: ChassisKey) = place(view,
            shell.left + shell.width() * key.x, shell.top + shell.width() * key.y)
        position(lock, chassis.power)
        chassis.volumeUp?.let { position(up, it) }
        chassis.volumeDown?.let { position(down, it) }
        chassis.ringer?.let { position(ringer, it) }
    }
    private fun place(view: View, cx: Float, cy: Float) {
        val l = (cx - view.measuredWidth / 2).toInt(); val t = (cy - view.measuredHeight / 2).toInt()
        view.layout(l, t, l + view.measuredWidth, t + view.measuredHeight)
    }
    override fun dispatchDraw(canvas: Canvas) {
        val time = drawingTime
        // The enclosure masks the inner halves, giving side/top keys a straight root.
        listOf(lock, up, down, ringer).forEach { if (it.visibility == VISIBLE) drawChild(canvas, it, time) }
        renderer.draw(canvas, shell, screen)
        drawChild(canvas, surface, time)
        drawChild(canvas, home, time)
    }
    fun releaseButtons() { buttons.forEach { it.release() } }
}
