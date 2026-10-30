package com.xxiao.ilemu

import android.content.ContentValues
import android.content.Context
import android.os.Build
import android.os.Environment
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.provider.MediaStore
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.UUID
import java.util.concurrent.Executors

/** Reuses the emulator's atomic PNG snapshot command; Android only publishes the file. */
internal class SessionScreenshots(context: Context, private val command: (String) -> Boolean) {
    private val app = context.applicationContext
    private val executor = Executors.newSingleThreadExecutor()
    private val main = Handler(Looper.getMainLooper())
    private var busy = false
    fun capture(completed: (Result<String>) -> Unit) {
        if (busy) return
        val pending = File(app.cacheDir, "screenshots/${UUID.randomUUID()}.png")
        pending.parentFile?.mkdirs()
        if (!command("snapshot ${pending.path}")) {
            completed(Result.failure(IllegalStateException("模拟器尚未就绪，请稍候"))); return
        }
        busy = true
        executor.execute {
            val result = runCatching {
                val deadline = SystemClock.elapsedRealtime() + 10000
                while (!pending.isFile) {
                    check(NativeBridge.isRunning() && SystemClock.elapsedRealtime() < deadline) { "未能取得画面，请稍后重试" }
                    Thread.sleep(50)
                }
                publish(pending)
            }
            pending.delete(); File(pending.path + ".tmp").delete()
            main.post { busy = false; completed(result) }
        }
    }
    private fun publish(file: File): String {
        val stamp = SimpleDateFormat("yyyyMMdd-HHmmss", Locale.ROOT).format(Date())
        val name = "iLEmu-$stamp-${UUID.randomUUID().toString().take(4)}.png"
        if (Build.VERSION.SDK_INT >= 29) {
            val resolver = app.contentResolver
            val values = ContentValues().apply {
                put(MediaStore.Images.Media.DISPLAY_NAME, name)
                put(MediaStore.Images.Media.MIME_TYPE, "image/png")
                put(MediaStore.Images.Media.RELATIVE_PATH, "${Environment.DIRECTORY_PICTURES}/iLEmu")
                put(MediaStore.Images.Media.IS_PENDING, 1)
            }
            val uri = resolver.insert(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, values) ?: error("无法创建截图")
            try {
                val output = resolver.openOutputStream(uri) ?: error("无法保存截图")
                output.use { target -> file.inputStream().use { it.copyTo(target) } }
                resolver.update(uri, ContentValues().apply { put(MediaStore.Images.Media.IS_PENDING, 0) }, null, null)
            } catch (error: Exception) { resolver.delete(uri, null, null); throw error }
            return "截图已保存到相册 · iLEmu"
        }
        // Legacy Android can save without asking for broad storage access.
        val directory = app.getExternalFilesDir(Environment.DIRECTORY_PICTURES) ?: File(app.filesDir, "pictures")
        directory.mkdirs(); file.copyTo(File(directory, name))
        return "截图已保存到 App 图片目录"
    }
    fun close() { executor.shutdown() }
}
