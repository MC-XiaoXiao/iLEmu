package com.xxiao.ilemu.firmware

import android.content.Context
import com.xxiao.ilemu.containers.*
import org.json.JSONObject
import java.io.File
import java.io.InputStream
import java.io.RandomAccessFile
import java.net.HttpURLConnection
import java.net.URL
import java.util.zip.ZipFile

// Orchestrates upstream tools. No DMG, encryption or filesystem codecs live in
// the Android client, and preparation never patches the guest system files.
internal class FirmwarePreparation(context: Context) {
    private val context = context.applicationContext
    private val repository = ContainerRepository(context)

    fun prepare(container: Container, archive: File, canceled: () -> Boolean, progress: (String) -> Unit) {
        ZipFile(archive).use { zip ->
            val entry = zip.getEntry("BuildManifest.plist") ?: zip.getEntry("Restore.plist") ?: error("IPSW 缺少固件 plist")
            val manifest = FirmwareMetadata.document(zip.getInputStream(entry))
            val path = rootImage(manifest, container.device)
            val image = zip.getEntry(path) ?: error("IPSW 缺少 plist 指定的系统镜像：$path")
            prepareImage(container, manifest, image.size, { zip.getInputStream(image) }, canceled, progress)
        }
    }

    fun rootImage(manifest: JSONObject, device: String): String {
        val board = FirmwareMetadata.models().first { it.identifier == device }.board
        val array = manifest.getJSONArray("RootFilesystems")
        val paths = (0 until array.length()).map { array.getJSONObject(it) }
            .filter { it.optString("board").isEmpty() || it.optString("board").equals(board, true) }
            .map { it.getString("path") }.distinct()
        require(paths.size == 1) { "plist 未能为所选设备确定唯一系统镜像" }
        return paths.single().also { path ->
            require(!path.startsWith('/') && path.split('/').none { it.isEmpty() || it == ".." || it == "." }) { "固件镜像路径无效" }
        }
    }

    fun prepareImage(container: Container, manifest: JSONObject, size: Long, open: () -> InputStream,
        canceled: () -> Boolean = { false }, progress: (String) -> Unit = {}) {
        val identity = FirmwareMetadata.identity(manifest)
        require(identity.accepts(container.device)) { "固件目标设备与容器不符" }
        val work = File(repository.home(container), "preparation").apply { mkdirs() }
        PosixFiles.delete(work)
        check(work.mkdirs())
        val tool = FirmwareTools(context, work, canceled)
        try {
            progress("提取系统镜像")
            val source = File(work, "system.dmg")
            require(size >= 0 && work.usableSpace > size + RESERVE) { "系统镜像所需空间不足" }
            open().use { input -> source.outputStream().use { output ->
                val buffer = ByteArray(256 * 1024)
                while (true) {
                    check(!canceled()) { "已暂停固件准备" }
                    val count = input.read(buffer)
                    if (count < 0) break
                    check(work.usableSpace > count + RESERVE) { "存储空间不足" }
                    output.write(buffer, 0, count)
                }
            } }
            var decoded = source
            if (signature(source, 0, 8) == "encrcdsa" || signature(source, (source.length() - 8).coerceAtLeast(0), 8) == "cdsaencr") {
                progress("获取系统镜像解密信息")
                val key = rootKey(container, rootImage(manifest, container.device))
                progress("解密系统镜像")
                decoded = File(work, "decrypted.dmg")
                tool.run("firmware_decrypt", listOf("-i", source.path, "-o", decoded.path, "-k", key))
                check(decoded.length() > 0) { "系统镜像解密失败" }
                source.delete()
            }
            var volume = decoded
            if (decoded.length() >= 512 && signature(decoded, decoded.length() - 512, 4) == "koly") {
                progress("展开 DMG 分区")
                val listing = tool.run("firmware_dmg", listOf("-l", decoded.path))
                val partitions = Regex("(?m)^\\s*partition (\\d+):.*Apple_HFSX?").findAll(listing).map { it.groupValues[1] }.toList()
                require(partitions.size == 1) { "系统镜像未包含唯一 HFS+ 系统分区" }
                volume = File(work, "system.hfs")
                tool.run("firmware_dmg", listOf("-s", "-p", partitions.single(), decoded.path, volume.path))
                decoded.delete()
            }
            val magic = signature(volume, 1024, 2)
            require(magic == "H+" || magic == "HX") { "系统镜像不是工具支持的 HFS+ 格式" }
            progress("解包运行固件")
            FirmwareImporter(context).installPrepared(container) { target ->
                tool.extract(volume, target)
                check(!canceled()) { "已暂停固件准备" }
            }
            progress("固件已就绪")
        } finally { PosixFiles.delete(work) }
    }

    private fun rootKey(container: Container, path: String): String {
        container.decryptionKey.takeIf { it.isNotEmpty() }?.let { return it }
        val url = URL("https://api.ipsw.me/v4/keys/ipsw/${container.device}/${container.build}")
        val connection = url.openConnection() as HttpURLConnection
        connection.connectTimeout = 15000
        connection.readTimeout = 30000
        try {
            check(connection.responseCode == 200) { "暂时无法获取解密信息；可在容器设置中填写公开的 RootFS Key 后重试" }
            val json = JSONObject(connection.inputStream.use { PosixFiles.readLimited(it, 4 * 1024 * 1024) }.toString(Charsets.UTF_8))
            val keys = json.getJSONArray("keys")
            return (0 until keys.length()).map { keys.getJSONObject(it) }
                .firstOrNull { it.optString("filename") == path }
                ?.getString("key")?.also { require(it.matches(Regex("[0-9a-fA-F]{72}"))) { "解密信息无效" } }
                ?: error("此固件暂无公开系统镜像密钥；可在设置中填写 RootFS Key")
        } finally { connection.disconnect() }
    }

    private fun signature(file: File, offset: Long, size: Int): String = RandomAccessFile(file, "r").use {
        if (offset < 0 || offset + size > it.length()) return@use ""
        it.seek(offset)
        val bytes = ByteArray(size)
        it.readFully(bytes)
        bytes.toString(Charsets.US_ASCII)
    }
    companion object { private const val RESERVE = 64L * 1024 * 1024 }
}
