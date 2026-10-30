package com.xxiao.ilemu.containers

import java.io.File
import java.io.InputStream
import java.io.OutputStream

internal object RootfsArchive {
    fun write(root: File, output: OutputStream) = output.buffered().use {
        FirmwareFiles.pack(root.path, it)
    }

    fun extract(input: InputStream, root: File) = input.buffered().use {
        FirmwareFiles.unpack(it, root.path)
    }
}
