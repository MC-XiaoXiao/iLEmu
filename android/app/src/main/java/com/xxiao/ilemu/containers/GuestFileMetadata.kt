package com.xxiao.ilemu.containers

import android.net.Uri
import android.system.Os
import android.util.Base64
import org.apache.commons.compress.archivers.tar.TarArchiveEntry
import java.io.File

// Preserve guest metadata as user xattrs without applying guest ownership or
// privileged flags to host files. This is the existing HFS metadata contract.
internal object GuestFileMetadata {
    private const val PREFIX = "LIBARCHIVE.xattr."
    fun archive(file: File, entry: TarArchiveEntry) {
        Os.listxattr(file.path).filter { it.startsWith("user.") }.forEach { name ->
            entry.addPaxHeader(PREFIX + Uri.encode(name), Base64.encodeToString(Os.getxattr(file.path, name), Base64.NO_WRAP))
        }
    }
    fun restore(file: File, entry: TarArchiveEntry) {
        entry.extraPaxHeaders.filterKeys { it.startsWith(PREFIX) }.forEach { (key, value) ->
            val name = Uri.decode(key.removePrefix(PREFIX))
            require(name.startsWith("user.") && '\u0000' !in name) { "固件扩展属性名称无效" }
            Os.setxattr(file.path, name, Base64.decode(value, Base64.DEFAULT), 0)
        }
    }
    fun copy(source: File, destination: File) {
        Os.listxattr(source.path).filter { it.startsWith("user.") }.forEach { name ->
            Os.setxattr(destination.path, name, Os.getxattr(source.path, name), 0)
        }
        check(destination.setLastModified(source.lastModified())) { "无法保存固件时间戳" }
    }
}
