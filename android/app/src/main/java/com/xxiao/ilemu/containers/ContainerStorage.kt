package com.xxiao.ilemu.containers

import android.content.Context
import android.net.Uri
import androidx.documentfile.provider.DocumentFile
import org.json.JSONObject
import java.io.File
import java.util.UUID

internal class ContainerStorage(context: Context) {
    private val context = context.applicationContext
    private val repository = ContainerRepository(context)
    private val resolver = context.contentResolver

    private fun tree(container: Container): DocumentFile = DocumentFile.fromTreeUri(context, Uri.parse(container.storageTree))
        ?.also { require(it.canWrite()) { "所选运行目录不可写，请重新授权" } } ?: error("运行目录不可用")

    private fun folder(container: Container): DocumentFile {
        val root = tree(container)
        val name = "ilemu-${container.id}"
        return root.findFile(name) ?: root.createDirectory(name) ?: error("无法创建容器目录")
    }

    fun checkpoint(container: Container, dataOnly: Boolean = false) {
        if (container.storageTree.isEmpty()) return
        val folder = folder(container)
        val previous = snapshot(folder)
        val generation = UUID.randomUUID().toString()
        val rootName = if (dataOnly && previous != null) previous.getString("root") else "rootfs-$generation.tar"
        val dataName = if (dataOnly && previous != null) "data-$generation.tar" else ""
        val created = mutableListOf<DocumentFile>()
        try {
            val source = if (dataName.isNotEmpty()) File(repository.rootfs(container), "private/var") else repository.rootfs(container)
            val archive = folder.createFile("application/x-tar", if (dataName.isNotEmpty()) dataName else rootName)
                ?: error("无法创建容器归档")
            created.add(archive)
            require(archive.name == (if (dataName.isNotEmpty()) dataName else rootName)) { "目录提供者更改了归档名称" }
            RootfsArchive.write(source, resolver.openOutputStream(archive.uri, "w") ?: error("无法写入容器归档"))
            val journal = folder.createFile("application/json", "snapshot-$generation.json") ?: error("无法写入容器元数据")
            created.add(journal)
            val state = JSONObject().put("container", container.json()).put("root", rootName).put("data", dataName)
                .put("generation", System.currentTimeMillis())
            resolver.openOutputStream(journal.uri, "w")!!.use { it.write(state.toString().toByteArray()) }
            // A completed journal commits the new generation. Old generations are
            // discarded only after it is readable, leaving interruption recoverable.
            val committed = snapshot(folder)
            require(committed?.getString("root") == rootName && committed.optString("data") == dataName)
            val retained = setOf(rootName, dataName, journal.name, "firmware.ipsw")
            folder.listFiles().filter { it.name !in retained && (it.name?.startsWith("rootfs-") == true ||
                it.name?.startsWith("data-") == true || it.name?.startsWith("snapshot-") == true) }.forEach { it.delete() }
        } catch (error: Exception) {
            created.forEach { it.delete() }
            throw error
        }
    }

    fun exportFirmware(container: Container, firmware: File) {
        if (container.storageTree.isEmpty()) return
        val folder = folder(container)
        val staged = folder.createFile("application/octet-stream", "firmware-${UUID.randomUUID()}.ipsw") ?: error("无法保存固件")
        try {
            resolver.openOutputStream(staged.uri, "w")!!.use { output -> firmware.inputStream().use { it.copyTo(output) } }
            folder.findFile("firmware.ipsw")?.let { check(it.delete()) }
            check(staged.renameTo("firmware.ipsw")) { "无法完成固件保存" }
        } catch (error: Exception) { staged.delete(); throw error }
    }

    fun delete(container: Container) {
        if (container.storageTree.isEmpty()) return
        val owned = tree(container).findFile("ilemu-${container.id}") ?: return
        check(owned.delete()) { "无法删除所选目录中的容器" }
    }

    fun snapshot(folder: DocumentFile): JSONObject? = folder.listFiles()
        .filter { it.isFile && it.name?.startsWith("snapshot-") == true && it.name?.endsWith(".json") == true }
        .mapNotNull { document -> runCatching {
            JSONObject(PosixFiles.readLimited(resolver.openInputStream(document.uri)!!, 65536).toString(Charsets.UTF_8)).also { j ->
                require(j.getString("root").matches(Regex("rootfs-[a-f0-9-]+\\.tar")))
                require(j.optString("data").isEmpty() || j.getString("data").matches(Regex("data-[a-f0-9-]+\\.tar")))
                require(folder.findFile(j.getString("root")) != null)
                require(j.optString("data").isEmpty() || folder.findFile(j.getString("data")) != null)
            }
        }.getOrNull() }.maxByOrNull { it.getLong("generation") }

    fun restore(folder: DocumentFile, destination: File): JSONObject {
        val state = snapshot(folder) ?: error("目录中没有完整的容器快照")
        RootfsArchive.extract(resolver.openInputStream(folder.findFile(state.getString("root"))!!.uri)!!, destination)
        if (state.optString("data").isNotEmpty()) {
            val data = File(destination, "private/var")
            PosixFiles.delete(data)
            RootfsArchive.extract(resolver.openInputStream(folder.findFile(state.getString("data"))!!.uri)!!, data)
        }
        return state.getJSONObject("container")
    }
}
