package com.xxiao.ilemu.firmware

import android.content.Context
import android.net.Uri
import androidx.documentfile.provider.DocumentFile
import com.xxiao.ilemu.containers.*
import java.io.File

internal class FirmwareImporter(context: Context) {
    private val context = context.applicationContext
    private val repository = ContainerRepository(context)

    fun importLocal(container: Container, source: File) {
        require(source.isDirectory) { "请选择已准备的固件目录" }
        installPrepared(container) { target -> PosixFiles.copy(source, target) }
    }

    fun importTree(container: Container, uri: Uri) {
        val source = DocumentFile.fromTreeUri(context, uri) ?: error("固件目录不可用")
        val storage = ContainerStorage(context)
        if (storage.snapshot(source) != null) {
            installPrepared(container) { storage.restore(source, it) }
            return
        }
        val root = findRoot(source)
        if (root != null) {
            installPrepared(container) { target -> copy(root, target, 0) }
            return
        }
        val (directory, plist) = findManifest(source) ?: error("目录中未找到 SystemVersion.plist、BuildManifest.plist 或 Restore.plist")
        val manifest = FirmwareMetadata.document(context.contentResolver.openInputStream(plist.uri)!!)
        val identity = FirmwareMetadata.identity(manifest)
        val entry = container.copy(version = identity.version, build = identity.build)
        val preparation = FirmwarePreparation(context)
        val image = descend(directory, preparation.rootImage(manifest, entry.device).split('/'))
            ?: error("目录缺少 plist 指定的系统镜像")
        preparation.prepareImage(entry, manifest, image.length(), { context.contentResolver.openInputStream(image.uri)!! })
    }

    fun identity(uri: Uri): FirmwareIdentity {
        val tree = DocumentFile.fromTreeUri(context, uri) ?: error("固件目录不可用")
        ContainerStorage(context).snapshot(tree)?.let { state ->
            val j = state.getJSONObject("container")
            return FirmwareIdentity(j.getString("version"), j.getString("build"), listOf(j.getString("device")))
        }
        val root = findRoot(tree)
        val plist = if (root != null) descend(root, listOf("System", "Library", "CoreServices", "SystemVersion.plist"))!!
            else findManifest(tree)?.second ?: error("目录中未找到固件版本 plist")
        return FirmwareMetadata.inspect(context.contentResolver.openInputStream(plist.uri)!!)
    }

    fun installPrepared(container: Container, populate: (File) -> Unit) {
        check(!container.legacy) { "已有固件保持原位；请新建容器后导入" }
        val home = repository.home(container).apply { mkdirs() }
        val target = File(home, "rootfs")
        require(!target.exists()) { "容器已有运行固件" }
        val stage = File(home, "rootfs-import")
        PosixFiles.delete(stage)
        try {
            populate(stage)
            val identity = FirmwareMetadata.inspectRoot(stage)
            require(identity.accepts(container.device)) { "固件目标设备与容器不符" }
            require(File(stage, "usr/lib/dyld").isFile && File(stage, "System/Library/LaunchDaemons").isDirectory) { "目录不是完整可运行的固件" }
            if (container.version.isNotEmpty()) require(container.version == identity.version && container.build == identity.build) { "导入的固件版本与所选版本不符" }
            check(stage.renameTo(target)) { "无法完成固件导入" }
            val updated = container.copy(version = identity.version, build = identity.build, ready = true, error = "")
            // Commit the list entry only when the directory and chosen storage are complete.
            try {
                ContainerStorage(context).checkpoint(updated)
                repository.save(updated)
            } catch (error: Exception) { PosixFiles.delete(target); throw error }
        } finally { PosixFiles.delete(stage) }
    }

    private fun descend(root: DocumentFile, parts: List<String>): DocumentFile? {
        var node = root
        for (part in parts) node = node.findFile(part) ?: return null
        return node
    }
    private fun findRoot(root: DocumentFile, depth: Int = 0): DocumentFile? {
        if (descend(root, listOf("System", "Library", "CoreServices", "SystemVersion.plist")) != null) return root
        if (depth >= 3) return null
        return root.listFiles().filter { it.isDirectory }.firstNotNullOfOrNull { findRoot(it, depth + 1) }
    }
    private fun findManifest(root: DocumentFile, depth: Int = 0): Pair<DocumentFile, DocumentFile>? {
        (root.findFile("BuildManifest.plist") ?: root.findFile("Restore.plist"))?.let { return root to it }
        if (depth >= 3) return null
        return root.listFiles().filter { it.isDirectory }.firstNotNullOfOrNull { findManifest(it, depth + 1) }
    }
    private fun copy(source: DocumentFile, target: File, depth: Int) {
        require(depth <= 64) { "固件目录层级过深" }
        check(target.mkdirs() || target.isDirectory)
        for (child in source.listFiles()) {
            val name = child.name ?: error("固件文件名不可用")
            require(name.isNotEmpty() && name != "." && name != ".." && '/' !in name)
            val file = File(target, name)
            if (child.isDirectory) copy(child, file, depth + 1) else {
                require(target.usableSpace > child.length() + 64L * 1024 * 1024) { "存储空间不足" }
                context.contentResolver.openInputStream(child.uri)!!.use { input -> file.outputStream().use { input.copyTo(it) } }
                file.setExecutable(true, true)
            }
        }
    }
}
