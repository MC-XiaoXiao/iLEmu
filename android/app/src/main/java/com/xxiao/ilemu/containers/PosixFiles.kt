package com.xxiao.ilemu.containers

import android.system.Os
import android.system.OsConstants
import java.io.File
import java.io.InputStream
import java.io.ByteArrayOutputStream

internal object PosixFiles {
    fun delete(file: File) {
        val stat = try { Os.lstat(file.path) } catch (_: Exception) { return }
        if (OsConstants.S_ISDIR(stat.st_mode)) {
            file.listFiles()?.forEach(::delete) ?: error("无法读取目录：${file.path}")
        }
        check(file.delete()) { "无法删除：${file.path}" }
    }
    fun copy(source: File, target: File) {
        val stat = Os.lstat(source.path)
        when {
            OsConstants.S_ISLNK(stat.st_mode) -> Os.symlink(Os.readlink(source.path), target.path)
            OsConstants.S_ISDIR(stat.st_mode) -> {
                check(target.mkdirs() || target.isDirectory)
                source.listFiles()?.forEach { copy(it, File(target, it.name)) } ?: error("无法读取源目录")
            }
            OsConstants.S_ISREG(stat.st_mode) -> {
                check(target.parentFile!!.usableSpace > stat.st_size + 64L * 1024 * 1024) { "存储空间不足" }
                source.inputStream().use { input -> target.outputStream().use { input.copyTo(it) } }
            }
            else -> error("不支持的固件文件类型：${source.name}")
        }
        if (!OsConstants.S_ISLNK(stat.st_mode)) {
            GuestFileMetadata.copy(source, target)
            Os.chmod(target.path, stat.st_mode and 511)
        }
    }
    fun readLimited(input: InputStream, limit: Int): ByteArray = input.use {
        val output = ByteArrayOutputStream()
        val bytes = ByteArray(8192)
        while (true) {
            val count = it.read(bytes)
            if (count < 0) break
            require(output.size() + count <= limit) { "元数据过大" }
            output.write(bytes, 0, count)
        }
        output.toByteArray()
    }
}
