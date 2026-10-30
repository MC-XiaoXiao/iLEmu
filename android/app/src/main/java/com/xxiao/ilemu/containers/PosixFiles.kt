package com.xxiao.ilemu.containers

import java.io.File
import java.io.InputStream
import java.io.ByteArrayOutputStream

internal object PosixFiles {
    fun delete(file: File) = FirmwareFiles.remove(file.path)
    fun copy(source: File, target: File) = FirmwareFiles.copy(source.path, target.path)
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
