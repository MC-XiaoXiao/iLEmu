package com.xxiao.ilemu.firmware

import android.util.AtomicFile
import com.xxiao.ilemu.containers.Container
import com.xxiao.ilemu.containers.ContainerRepository
import org.json.JSONObject
import java.io.File

internal data class DownloadState(val phase: String = "等待下载", val bytes: Long = 0, val total: Long = 0,
    val connections: Int = 0, val resumed: Boolean = false, val error: String = "") {
    // Byte counts describe the transfer only; later preparation tools have no total.
    val progressPercent: Int?
        get() = if (phase == "下载中" && total > 0)
            (bytes.toDouble() / total * 100).toInt().coerceIn(0, 100) else null
    val summary: String
        get() = (when (phase) {
            "连接中" -> "正在连接…"
            "下载中" -> "正在下载固件…"
            "校验中" -> "正在校验固件…"
            "保存固件", "提取系统镜像", "获取系统镜像解密信息", "解密系统镜像", "展开 DMG 分区", "解包运行固件" -> "正在$phase…"
            else -> phase
        }) + if (error.isNotEmpty()) "\n$error" else ""
    fun json() = JSONObject().put("phase", phase).put("bytes", bytes).put("total", total)
        .put("connections", connections).put("resumed", resumed).put("error", error)
    override fun toString() = "$phase · ${bytes / (1024 * 1024)} / ${total / (1024 * 1024)} MB" +
        (if (connections > 0) " · $connections 连接" else "") + (if (resumed) " · 续传" else "") +
        (if (error.isNotEmpty()) "\n$error" else "")
    companion object {
        @Synchronized fun read(repository: ContainerRepository, container: Container): DownloadState {
            val file = AtomicFile(File(repository.home(container), "download.json"))
            return runCatching {
                val j = JSONObject(file.openRead().use { it.readBytes().toString(Charsets.UTF_8) })
                DownloadState(j.getString("phase"), j.getLong("bytes"), j.getLong("total"), j.getInt("connections"), j.getBoolean("resumed"), j.optString("error"))
            }.getOrElse { DownloadState(total = container.firmware?.optLong("size") ?: 0) }
        }
        @Synchronized fun write(repository: ContainerRepository, container: Container, state: DownloadState) {
            repository.home(container).mkdirs()
            val file = AtomicFile(File(repository.home(container), "download.json"))
            val output = file.startWrite()
            try { output.write(state.json().toString().toByteArray()); file.finishWrite(output) }
            catch (error: Exception) { file.failWrite(output); throw error }
        }
    }
}
