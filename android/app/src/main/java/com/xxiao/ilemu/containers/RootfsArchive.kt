package com.xxiao.ilemu.containers

import android.system.Os
import android.system.OsConstants
import android.system.ErrnoException
import org.apache.commons.compress.archivers.tar.TarArchiveEntry
import org.apache.commons.compress.archivers.tar.TarArchiveInputStream
import org.apache.commons.compress.archivers.tar.TarArchiveOutputStream
import org.apache.commons.compress.archivers.tar.TarConstants
import java.io.File
import java.io.InputStream
import java.io.OutputStream

// Reuse the tar implementation; preserve guest links without following them on the host.
internal object RootfsArchive {
    fun write(root: File, output: OutputStream) {
        TarArchiveOutputStream(output.buffered()).use { tar ->
            tar.setLongFileMode(TarArchiveOutputStream.LONGFILE_POSIX)
            tar.setBigNumberMode(TarArchiveOutputStream.BIGNUMBER_POSIX)
            fun append(file: File, name: String) {
                val stat = Os.lstat(file.path)
                val type = when {
                    OsConstants.S_ISDIR(stat.st_mode) -> TarConstants.LF_DIR
                    OsConstants.S_ISLNK(stat.st_mode) -> TarConstants.LF_SYMLINK
                    OsConstants.S_ISREG(stat.st_mode) -> TarConstants.LF_NORMAL
                    else -> return // Runtime sockets and FIFOs are recreated by the firmware.
                }
                val entry = TarArchiveEntry(name, type).apply {
                    mode = stat.st_mode and 511
                    size = if (type == TarConstants.LF_NORMAL) stat.st_size else 0
                    modTime = java.util.Date(stat.st_mtime * 1000)
                    if (type == TarConstants.LF_SYMLINK) linkName = Os.readlink(file.path)
                }
                if (type != TarConstants.LF_SYMLINK) GuestFileMetadata.archive(file, entry)
                tar.putArchiveEntry(entry)
                if (type == TarConstants.LF_NORMAL) file.inputStream().use { it.copyTo(tar) }
                tar.closeArchiveEntry()
                if (type == TarConstants.LF_DIR) file.listFiles()?.forEach { append(it, "$name/${it.name}") }
                    ?: error("无法读取固件目录")
            }
            root.listFiles()?.forEach { append(it, it.name) } ?: error("无法读取固件目录")
        }
    }

    fun extract(input: InputStream, root: File) {
        check(root.mkdirs() || root.isDirectory)
        var count = 0
        val hardLinks = mutableListOf<Pair<File, String>>()
        val modes = mutableListOf<Pair<File, Int>>()
        val directoryTimes = mutableListOf<Pair<File, Long>>()
        TarArchiveInputStream(input.buffered()).use { tar ->
            while (true) {
                val entry = tar.nextEntry ?: break
                require(++count <= 250000) { "固件归档文件过多" }
                val parts = entry.name.trimEnd('/').split('/').filter { it != "." }
                require(!entry.name.startsWith('/') && parts.isNotEmpty() && parts.none { it == ".." || it.isEmpty() }) { "不安全的归档路径" }
                var parent = root
                parts.dropLast(1).forEach { part ->
                    parent = File(parent, part)
                    if (java.nio.file.Files.isSymbolicLink(parent.toPath())) error("归档路径经过符号链接")
                    check(parent.mkdirs() || parent.isDirectory)
                }
                val file = File(parent, parts.last())
                require(!java.nio.file.Files.isSymbolicLink(file.toPath())) { "归档目标为符号链接" }
                when {
                    entry.isDirectory -> check(file.mkdirs() || file.isDirectory)
                    entry.isSymbolicLink -> { require(!file.exists()); Os.symlink(entry.linkName, file.path) }
                    entry.isLink -> hardLinks.add(file to entry.linkName)
                    entry.isFile -> {
                        require(entry.size >= 0 && root.usableSpace > entry.size + 64L * 1024 * 1024) { "存储空间不足" }
                        file.outputStream().use { tar.copyTo(it) }
                        require(file.length() == entry.size) { "固件归档截断" }
                    }
                    else -> error("不支持的归档文件类型")
                }
                if (!entry.isSymbolicLink && !entry.isLink) {
                    GuestFileMetadata.restore(file, entry)
                    check(file.setLastModified(entry.lastModifiedDate.time)) { "无法保存固件时间戳" }
                    if (entry.isDirectory) directoryTimes.add(file to entry.lastModifiedDate.time)
                    modes.add(file to (entry.mode and 511))
                }
            }
        }
        // POSIX archives may reference files that appear later. Resolve links
        // after all regular files exist, without traversing archive symlinks.
        hardLinks.forEach { (file, name) ->
            require(!name.startsWith('/') && name.split('/').none { it == ".." || it.isEmpty() }) { "不安全的硬链接路径" }
            var source = root
            name.split('/').filter { it != "." }.forEach { part ->
                source = File(source, part)
                require(!java.nio.file.Files.isSymbolicLink(source.toPath())) { "硬链接经过符号链接" }
            }
            require(source.isFile && !file.exists()) { "固件硬链接目标不可用" }
            try { Os.link(source.path, file.path) }
            catch (error: ErrnoException) {
                if (error.errno != OsConstants.EACCES && error.errno != OsConstants.EPERM &&
                    error.errno != OsConstants.EOPNOTSUPP) throw error
                // Android SELinux can deny hard links even within app data. Materialize
                // their bytes and guest HFS metadata without requiring root privileges.
                PosixFiles.copy(source, file)
            }
        }
        directoryTimes.asReversed().forEach { (file, time) ->
            check(file.setLastModified(time)) { "无法保存固件目录时间戳" }
        }
        modes.asReversed().forEach { (file, mode) -> Os.chmod(file.path, mode) }
    }
}
