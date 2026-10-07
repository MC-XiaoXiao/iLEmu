package com.xxiao.ilemu.firmware

import android.content.Context
import com.xxiao.ilemu.containers.RootfsArchive
import java.io.File
import java.util.concurrent.TimeUnit

internal class FirmwareTools(context: Context, private val work: File, private val canceled: () -> Boolean) {
    private val libraries = File(context.applicationInfo.nativeLibraryDir)
    private fun launch(tool: String, arguments: List<String>): Process {
        val executable = File(libraries, "lib$tool.so")
        check(executable.canExecute()) { "固件工具未安装：$tool" }
        return ProcessBuilder(listOf(executable.path) + arguments).directory(work)
            .redirectError(File(work, "tool-error.log")).start()
    }
    fun run(tool: String, arguments: List<String>): String {
        val process = launch(tool, arguments)
        val output = File(work, "tool-output.log")
        val reader = Thread { process.inputStream.use { input -> output.outputStream().use { input.copyTo(it) } } }
        reader.start()
        try {
            await(process)
            reader.join()
            // dmg2img reports its partition listing on stderr even on success.
            return (output.readText() + File(work, "tool-error.log").readText()).takeLast(65536)
        }
        finally { process.destroyForcibly(); reader.join() }
    }
    fun extract(volume: File, target: File) {
        val process = launch("firmware_hfstar", listOf("-e", "-t", "-s", "--rsrc-ext", ".ilemu-rsrc",
            "--format", "pax", "--options", "xattrheader=LIBARCHIVE", volume.path, "-"))
        try {
            RootfsArchive.extract(object : java.io.FilterInputStream(process.inputStream) {
                override fun read(b: ByteArray, off: Int, len: Int): Int {
                    check(!canceled()) { "已暂停固件准备" }
                    return super.read(b, off, len)
                }
            }, target)
            await(process)
        } finally { process.destroyForcibly() }
    }
    private fun await(process: Process) {
        while (!process.waitFor(200, TimeUnit.MILLISECONDS)) {
            check(!canceled()) { "已暂停固件准备" }
            check(work.usableSpace > 64L * 1024 * 1024) { "存储空间不足" }
        }
        check(process.exitValue() == 0) { "固件工具失败 (${process.exitValue()})：" + File(work, "tool-error.log").readText().takeLast(4096) }
    }
}
