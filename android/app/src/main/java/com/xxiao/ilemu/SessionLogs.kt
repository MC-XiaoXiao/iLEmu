package com.xxiao.ilemu

import android.content.Context
import com.xxiao.ilemu.containers.ContainerRepository
import java.io.File
import java.io.RandomAccessFile

internal object SessionLogs {
    fun file(context: Context, id: String?): File = if (id == null) File(context.filesDir, "session.log")
        else File(ContainerRepository(context).home(ContainerRepository(context).get(id)), "session.log")
    fun tail(file: File): String = runCatching {
        if (!file.isFile) return "尚无运行日志。启动此容器后，日志会显示在这里。"
        RandomAccessFile(file, "r").use { input ->
            input.seek((input.length() - 65536).coerceAtLeast(0))
            val bytes = ByteArray((input.length() - input.filePointer).toInt()); input.readFully(bytes)
            bytes.toString(Charsets.UTF_8).ifEmpty { "正在等待日志…" }
        }
    }.getOrElse { "日志读取失败：${it.message}" }
}
