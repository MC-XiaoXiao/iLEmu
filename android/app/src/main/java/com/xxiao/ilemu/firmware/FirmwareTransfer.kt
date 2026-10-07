package com.xxiao.ilemu.firmware

import com.xxiao.ilemu.containers.PosixFiles
import java.io.File
import java.security.MessageDigest
import java.util.zip.ZipFile

internal object FirmwareTransfer {
    fun verify(file: File, release: FirmwareRelease, canceled: () -> Boolean) {
        require(file.length() == release.size) { "固件大小不匹配" }
        val digest = MessageDigest.getInstance(release.algorithm)
        file.inputStream().buffered().use { input ->
            val bytes = ByteArray(256 * 1024)
            while (true) {
                check(!canceled()) { "已暂停校验" }
                val count = input.read(bytes)
                if (count < 0) break
                digest.update(bytes, 0, count)
            }
        }
        val actual = digest.digest().joinToString("") { "%02x".format(it) }
        require(actual.equals(release.digest, ignoreCase = true)) { "固件 ${release.algorithm} 校验失败，请重新下载" }
        ZipFile(file).use { zip ->
            val manifest = zip.getEntry("BuildManifest.plist") ?: zip.getEntry("Restore.plist")
                ?: error("IPSW 中未找到固件 plist")
            val identity = FirmwareMetadata.inspect(zip.getInputStream(manifest))
            require(identity.version == release.version && identity.build == release.build && identity.accepts(release.device)) { "IPSW 版本或设备与所选固件不符" }
        }
    }
}
