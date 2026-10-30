package com.xxiao.ilemu.ui

import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF

/** Vector-only enclosure decoration. Display and controls are independent child views. */
internal class ChassisRenderer(private val chassis: DeviceChassis) {
    private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val glass = RectF()
    fun draw(canvas: Canvas, body: RectF, screen: RectF) {
        val unit = body.width()
        val radius = unit * chassis.corner
        val edge = unit * chassis.rim
        paint.color = when (chassis.finish) {
            ChassisFinish.Light -> 0xff92989b.toInt()
            ChassisFinish.Neutral -> 0xff777c80.toInt()
            ChassisFinish.Dark -> 0xff44494e.toInt()
        }
        paint.style = Paint.Style.FILL
        canvas.drawRoundRect(body, radius, radius, paint)
        glass.set(body); glass.inset(edge, edge)
        paint.color = 0xff080a0b.toInt()
        canvas.drawRoundRect(glass, (radius - edge).coerceAtLeast(0f), (radius - edge).coerceAtLeast(0f), paint)
        paint.style = Paint.Style.STROKE; paint.strokeWidth = unit * .002f; paint.color = 0xffa5b0b4.toInt()
        canvas.drawRoundRect(glass, (radius - edge).coerceAtLeast(0f), (radius - edge).coerceAtLeast(0f), paint)
        paint.style = Paint.Style.FILL
        chassis.speakerY?.let { y ->
            val cy = body.top + unit * y
            paint.color = 0xff202527.toInt()
            canvas.drawRoundRect(body.centerX() - unit * .095f, cy - unit * .019f,
                body.centerX() + unit * .095f, cy + unit * .019f, unit * .019f, unit * .019f, paint)
            paint.color = 0xff07090a.toInt()
            canvas.drawRoundRect(body.centerX() - unit * .085f, cy - unit * .009f,
                body.centerX() + unit * .085f, cy + unit * .009f, unit * .009f, unit * .009f, paint)
        }
        chassis.camera?.let { camera ->
            val x = body.left + unit * camera.x; val y = body.top + unit * camera.y
            paint.color = 0xff1a2229.toInt(); canvas.drawCircle(x, y, unit * camera.diameter / 2, paint)
            paint.color = 0xff10171e.toInt(); canvas.drawCircle(x, y, unit * camera.diameter / 3, paint)
        }
        paint.color = 0xff363c3f.toInt()
        paint.style = Paint.Style.STROKE; paint.strokeWidth = unit * .006f
        canvas.drawRect(screen, paint)
        paint.color = Color.BLACK; paint.strokeWidth = unit * .005f
        chassis.antennaY.forEach { y ->
            val cy = body.top + unit * y
            canvas.drawLine(body.left, cy, body.left + edge, cy, paint)
            canvas.drawLine(body.right - edge, cy, body.right, cy, paint)
        }
    }
}
