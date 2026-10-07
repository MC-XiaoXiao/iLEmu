package com.xxiao.ilemu.containers

import android.content.Context
import android.util.AtomicFile
import com.xxiao.ilemu.firmware.FirmwareMetadata
import org.json.JSONArray
import java.io.File

internal class ContainerRepository(context: Context) {
    private val context = context.applicationContext
    private val index = AtomicFile(File(context.filesDir, "containers.json"))
    private val directory = File(context.filesDir, "containers").apply { mkdirs() }

    fun list(): List<Container> = synchronized(lock) {
        if (!index.baseFile.exists()) return@synchronized emptyList()
        val array = JSONArray(index.openRead().use { it.readBytes().toString(Charsets.UTF_8) })
        List(array.length()) { Container.fromJson(array.getJSONObject(it)) }
    }

    fun get(id: String): Container = list().firstOrNull { it.id == id } ?: error("容器不存在")

    fun save(container: Container) = synchronized(lock) {
        require(container.name.trim().length in 1..80) { "容器名称应为 1–80 个字符" }
        require(container.cores in 1..8 && container.jitCacheMiB in 8..128)
        val entries = list().toMutableList()
        val position = entries.indexOfFirst { it.id == container.id }
        if (position < 0) entries.add(container) else entries[position] = container
        write(entries)
    }

    fun update(id: String, change: (Container) -> Container) = synchronized(lock) {
        save(change(get(id)))
    }

    fun removeOwned(id: String) {
        val container = get(id)
        // A migrated rootfs remains owned by the user; remove only its list entry.
        if (!container.legacy) PosixFiles.delete(home(container))
        PosixFiles.delete(File(context.cacheDir, "containers/$id"))
        synchronized(lock) { write(list().filterNot { it.id == id }) }
    }

    fun home(container: Container) = File(directory, container.id)
    fun rootfs(container: Container) = if (container.legacy) File(context.filesDir, "rootfs") else File(home(container), "rootfs")
    fun cache(container: Container) = if (container.legacy) File(context.cacheDir, "emulator")
        else File(context.cacheDir, "containers/${container.id}")

    fun migrateLegacy() = synchronized(lock) {
        val preferences = context.getSharedPreferences("containers", Context.MODE_PRIVATE)
        if (preferences.getBoolean("legacyRegistered", false)) return@synchronized
        val root = File(context.filesDir, "rootfs")
        if (root.isDirectory) {
            val metadata = FirmwareMetadata.inspectRoot(root)
            val previous = context.getSharedPreferences("MainActivity", Context.MODE_PRIVATE).getString("device", null)
            val device = previous ?: FirmwareMetadata.models().first().identifier
            if (list().none { it.legacy }) save(Container(name = "已有容器", device = device,
                version = metadata.version, build = metadata.build, ready = true, legacy = true))
        }
        preferences.edit().putBoolean("legacyRegistered", true).commit()
    }

    private fun write(entries: List<Container>) {
        val output = index.startWrite()
        try {
            output.write(JSONArray().also { array -> entries.forEach { array.put(it.json()) } }.toString().toByteArray())
            index.finishWrite(output)
        } catch (error: Exception) {
            index.failWrite(output)
            throw error
        }
    }

    companion object { private val lock = Any() }
}
