package com.xxiao.ilemu

import android.content.Intent
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import com.xxiao.ilemu.containers.*
import com.xxiao.ilemu.firmware.FirmwareDownloadService
import java.io.File

/** Container commands shared by the card and its action rail. */
internal class ContainerActions(private val activity: AppCompatActivity, private val launcher: SessionLauncher) {
    private val repository = ContainerRepository(activity)
    private fun attempt(action: () -> Unit) {
        runCatching(action).onFailure { message(it.message ?: "操作未完成") }
    }
    private fun message(text: String) { Toast.makeText(activity, text, Toast.LENGTH_LONG).show() }
    fun start(entry: Container) = attempt {
        val current = repository.get(entry.id)
        check(current.ready && !ContainerTasks.isBusy(current.id)) { "请等待固件准备或数据同步完成" }
        launcher.launch(SessionSettings(current).bootArguments(activity), current.id)
    }
    fun primary(entry: Container) = attempt {
        val current = repository.get(entry.id)
        val busy = ContainerTasks.isBusy(current.id)
        when {
            !current.ready && busy -> FirmwareDownloadService.pause(activity)
            !current.ready -> FirmwareDownloadService.start(activity, current.id)
            !busy -> start(current)
            NativeBridge.ownsRunningSession(current.id) -> AlertDialog.Builder(activity).setTitle("关闭“${current.name}”？")
                .setMessage("结束当前运行并保存容器数据。")
                .setNegativeButton("继续运行", null).setPositiveButton("关闭") { _, _ ->
                    NativeBridge.command(File(activity.filesDir, "control.fifo").path, "quit", true)
                }.show()
            else -> message("正在同步数据，请稍候")
        }
    }
    fun settings(entry: Container) = attempt {
        check(!ContainerTasks.isBusy(entry.id)) { "请等待当前操作完成" }
        activity.startActivity(Intent(activity, ContainerActivity::class.java).putExtra("id", entry.id))
    }
    fun logs(entry: Container) {
        activity.startActivity(Intent(activity, LogsActivity::class.java).putExtra("id", entry.id))
    }
    fun delete(entry: Container) = attempt {
        check(!ContainerTasks.isBusy(entry.id)) { "请等待当前操作完成" }
        AlertDialog.Builder(activity).setTitle("删除“${entry.name}”？")
            .setMessage(if (entry.legacy) "移除列表项，保留原有固件目录。" else "此容器的固件、下载文件和数据将被删除。")
            .setNegativeButton("取消", null).setPositiveButton("删除") { _, _ -> attempt {
                ContainerTasks.run(entry.id, { ContainerStorage(activity).delete(entry); repository.removeOwned(entry.id) }, { error -> activity.runOnUiThread {
                    message(error?.let { "删除失败：${it.message}" } ?: "已删除 ${entry.name}")
                } })
            } }.show()
    }
}
