package com.xxiao.ilemu.firmware

import android.content.Context
import android.util.AtomicFile
import org.json.JSONObject
import java.io.File
import java.net.HttpURLConnection
import java.net.URL

internal data class FirmwareRelease(val device: String, val version: String, val build: String,
    val url: String, val size: Long, val algorithm: String, val digest: String) {
    override fun toString() = "iOS $version ($build) · ${size / (1024 * 1024)} MB"
    fun json() = JSONObject().put("device", device).put("version", version).put("build", build)
        .put("url", url).put("size", size).put("algorithm", algorithm).put("digest", digest)
    companion object {
        fun fromJson(j: JSONObject) = FirmwareRelease(j.getString("device"), j.getString("version"),
            j.getString("build"), j.getString("url"), j.getLong("size"), j.getString("algorithm"), j.getString("digest"))
    }
}

internal class FirmwareCatalog(context: Context) {
    private val cache = File(context.cacheDir, "firmware-catalog").apply { mkdirs() }
    fun releases(device: String): List<FirmwareRelease> {
        require(FirmwareMetadata.models().any { it.identifier == device }) { "设备未被模拟器支持" }
        val file = AtomicFile(File(cache, "$device.json"))
        val json = try {
            val connection = URL("https://api.ipsw.me/v4/device/$device?type=ipsw").openConnection() as HttpURLConnection
            connection.connectTimeout = 15000
            connection.readTimeout = 30000
            try {
                check(connection.responseCode == 200) { "固件目录请求失败：HTTP ${connection.responseCode}" }
                val bytes = connection.inputStream.use { com.xxiao.ilemu.containers.PosixFiles.readLimited(it, 4 * 1024 * 1024) }
                require(bytes.size <= 4 * 1024 * 1024)
                JSONObject(bytes.toString(Charsets.UTF_8)).also {
                    val output = file.startWrite()
                    try { output.write(bytes); file.finishWrite(output) } catch (e: Exception) { file.failWrite(output); throw e }
                }
            } finally { connection.disconnect() }
        } catch (error: Exception) {
            if (!file.baseFile.exists()) throw error
            JSONObject(file.openRead().use { it.readBytes().toString(Charsets.UTF_8) })
        }
        val firmware = json.getJSONArray("firmwares")
        return (0 until firmware.length()).mapNotNull { index ->
            val j = firmware.getJSONObject(index)
            val sha256 = j.optString("sha256sum")
            val sha1 = j.optString("sha1sum")
            val algorithm = if (sha256.matches(Regex("[a-fA-F0-9]{64}"))) "SHA-256" else "SHA-1"
            val digest = if (algorithm == "SHA-256") sha256 else sha1
            if (!digest.matches(Regex("[a-fA-F0-9]{40}|[a-fA-F0-9]{64}"))) return@mapNotNull null
            val url = URL(j.getString("url").replaceFirst(Regex("^http:"), "https:"))
            if (url.protocol != "https") return@mapNotNull null
            FirmwareRelease(device, j.getString("version"), j.getString("buildid"), url.toString(),
                j.getLong("filesize"), algorithm, digest)
        }
    }
}
