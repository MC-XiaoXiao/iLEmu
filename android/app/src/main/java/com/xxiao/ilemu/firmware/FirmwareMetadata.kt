package com.xxiao.ilemu.firmware

import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.io.InputStream

internal data class DeviceOption(val identifier: String, val name: String, val board: String) {
    override fun toString() = "$name · $identifier"
}
internal data class FirmwareIdentity(val version: String, val build: String, val devices: List<String>) {
    fun accepts(device: String) = devices.isEmpty() || device in devices
}

internal object FirmwareMetadata {
    init { System.loadLibrary("SDL2"); System.loadLibrary("ilemu") }
    private external fun readPlist(bytes: ByteArray): String?
    private external fun deviceModels(): String
    fun models(): List<DeviceOption> {
        val list = JSONArray(deviceModels())
        return List(list.length()) { list.getJSONObject(it).let { j -> DeviceOption(j.getString("identifier"), j.getString("name"), j.getString("board")) } }
    }
    fun inspect(input: InputStream): FirmwareIdentity {
        return identity(document(input))
    }
    fun document(input: InputStream): JSONObject {
        val bytes = input.use { com.xxiao.ilemu.containers.PosixFiles.readLimited(it, 4 * 1024 * 1024) }
        require(bytes.size <= 4 * 1024 * 1024) { "固件 plist 过大" }
        return JSONObject(readPlist(bytes) ?: error("无法解析固件 plist（支持 XML 和二进制）"))
    }
    fun identity(j: JSONObject): FirmwareIdentity {
        val devices = j.optJSONArray("SupportedProductTypes")?.let { a -> List(a.length()) { a.getString(it) } }
            ?: j.optString("ProductType").takeIf { it.isNotEmpty() }?.let { listOf(it) } ?: emptyList()
        return FirmwareIdentity(j.optString("ProductVersion"), j.optString("ProductBuildVersion"), devices).also {
            require(it.version.isNotBlank() && it.build.isNotBlank()) { "plist 中缺少固件版本或构建号" }
        }
    }
    fun inspectRoot(root: File) = inspect(File(root, "System/Library/CoreServices/SystemVersion.plist").inputStream())
}
