package com.xxiao.ilemu.containers

import org.json.JSONObject
import java.util.UUID

internal data class Container(
    val id: String = UUID.randomUUID().toString(),
    val name: String,
    val device: String,
    val version: String = "",
    val build: String = "",
    val ready: Boolean = false,
    val legacy: Boolean = false,
    val storageTree: String = "",
    val headless: Boolean = false,
    val cores: Int = 1,
    val jitCacheMiB: Int = 16,
    val dirty: Boolean = false,
    val error: String = "",
    val decryptionKey: String = "",
    val firmware: JSONObject? = null
) {
    fun json() = JSONObject().put("id", id).put("name", name).put("device", device)
        .put("version", version).put("build", build).put("ready", ready).put("legacy", legacy)
        .put("storageTree", storageTree).put("headless", headless).put("cores", cores)
        .put("decryptionKey", decryptionKey).put("jitCacheMiB", jitCacheMiB).put("dirty", dirty).put("error", error)
        .put("firmware", firmware)

    companion object {
        fun fromJson(j: JSONObject) = Container(
            id = j.getString("id").also { UUID.fromString(it) }, name = j.getString("name"),
            device = j.getString("device"), version = j.optString("version"), build = j.optString("build"),
            ready = j.optBoolean("ready"), legacy = j.optBoolean("legacy"),
            storageTree = j.optString("storageTree"), headless = j.optBoolean("headless"),
            cores = j.optInt("cores", 1), jitCacheMiB = j.optInt("jitCacheMiB", 16),
            dirty = j.optBoolean("dirty"), error = j.optString("error"), decryptionKey = j.optString("decryptionKey"), firmware = j.optJSONObject("firmware")
        )
    }
}
