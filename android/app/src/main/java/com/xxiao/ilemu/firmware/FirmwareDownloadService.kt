package com.xxiao.ilemu.firmware

import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import androidx.core.app.NotificationCompat
import androidx.core.content.ContextCompat
import com.liulishuo.okdownload.DownloadTask
import com.liulishuo.okdownload.OkDownload
import com.liulishuo.okdownload.core.breakpoint.BlockInfo
import com.liulishuo.okdownload.core.breakpoint.BreakpointInfo
import com.liulishuo.okdownload.core.cause.EndCause
import com.liulishuo.okdownload.core.file.FirmwareFileStrategy
import com.liulishuo.okdownload.core.listener.DownloadListener4
import com.liulishuo.okdownload.core.listener.assist.Listener4Assist
import com.xxiao.ilemu.MainActivity
import com.xxiao.ilemu.R
import com.xxiao.ilemu.containers.*
import java.io.File
import java.util.concurrent.Executors

class FirmwareDownloadService : Service() {
    private var task: DownloadTask? = null
    private var container: Container? = null
    private lateinit var repository: ContainerRepository
    @Volatile private var paused = false
    @Volatile private var state = DownloadState()
    private val verifier = Executors.newSingleThreadExecutor()

    override fun onCreate() {
        super.onCreate()
        initializeDownloader(this)
        repository = ContainerRepository(this)
        getSystemService(NotificationManager::class.java).createNotificationChannel(
            NotificationChannel(CHANNEL, "固件下载", NotificationManager.IMPORTANCE_LOW))
    }
    override fun onBind(intent: Intent?): IBinder? = null
    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == PAUSE) {
            paused = true
            task?.cancel()
            return START_NOT_STICKY
        }
        if (container != null) {
            intent?.getStringExtra("id")?.takeIf { it != container?.id }?.let { id ->
                val waiting = repository.get(id)
                DownloadState.write(repository, waiting, DownloadState.read(repository, waiting)
                    .copy(phase = "等待下载", error = "另一固件任务正在进行，请完成后继续"))
                android.widget.Toast.makeText(this, "另一固件任务正在进行", android.widget.Toast.LENGTH_SHORT).show()
            }
            return START_NOT_STICKY
        }
        val id = intent?.getStringExtra("id") ?: run { stopSelf(); return START_NOT_STICKY }
        try {
            val entry = repository.get(id)
            check(ContainerTasks.reserve(id)) { "容器正在使用中" }
            container = entry
            state = DownloadState.read(repository, entry).copy(phase = "连接中", error = "")
            foreground()
            val release = FirmwareRelease.fromJson(entry.firmware ?: error("未选择固件"))
            val home = repository.home(entry).apply { mkdirs() }
            val partial = File(home, "firmware.ipsw.part")
            check(File(home, "firmware.ipsw").exists() || home.usableSpace > (release.size - DownloadState.read(repository, entry).bytes).coerceAtLeast(0) + 64L * 1024 * 1024) { "存储空间不足" }
            if (File(home, "firmware.ipsw").exists()) {
                verifier.execute { finishDownload(entry, release, File(home, "firmware.ipsw")) }
            } else {
                task = DownloadTask.Builder(release.url, home.path, "firmware.ipsw.part")
                    .setConnectionCount(4).setPreAllocateLength(true).setAutoCallbackToUIThread(false)
                    .setMinIntervalMillisCallbackProcess(1000).setPassIfAlreadyCompleted(true).build()
                task!!.enqueue(listener(entry, release, partial))
            }
        } catch (error: Exception) { fail(error); finish() }
        return START_NOT_STICKY
    }

    private fun listener(entry: Container, release: FirmwareRelease, partial: File) = object : DownloadListener4() {
        override fun taskStart(task: DownloadTask) = Unit
        override fun connectStart(task: DownloadTask, blockIndex: Int, headers: MutableMap<String, MutableList<String>>) = Unit
        override fun connectEnd(task: DownloadTask, blockIndex: Int, code: Int, headers: MutableMap<String, MutableList<String>>) = Unit
        override fun infoReady(task: DownloadTask, info: BreakpointInfo, fromBreakpoint: Boolean, model: Listener4Assist.Listener4Model) {
            state = state.copy(phase = "下载中", bytes = info.totalOffset, total = info.totalLength,
                connections = info.blockCount, resumed = fromBreakpoint)
            publish()
        }
        override fun progress(task: DownloadTask, currentOffset: Long) { state = state.copy(bytes = currentOffset); publish() }
        override fun progressBlock(task: DownloadTask, blockIndex: Int, offset: Long) = Unit
        override fun blockEnd(task: DownloadTask, blockIndex: Int, info: BlockInfo) = Unit
        override fun taskEnd(task: DownloadTask, cause: EndCause, realCause: Exception?, model: Listener4Assist.Listener4Model) {
            if (cause == EndCause.COMPLETED && !paused) verifier.execute { finishDownload(entry, release, partial) }
            else {
                state = state.copy(phase = if (cause == EndCause.CANCELED) "已暂停" else "下载失败", error = realCause?.message ?: "")
                publish(); finish()
            }
        }
    }

    private fun finishDownload(entry: Container, release: FirmwareRelease, file: File) {
        try {
            state = state.copy(phase = "校验中", bytes = file.length(), total = release.size); publish()
            try { FirmwareTransfer.verify(file, release) { paused } } catch (error: Exception) {
                if (!paused) { file.delete(); task?.let { OkDownload.with().breakpointStore().remove(it.id) } }
                throw error
            }
            val verified = File(file.parentFile, "firmware.ipsw")
            if (file != verified) check(file.renameTo(verified)) { "无法保存校验后的固件" }
            state = state.copy(phase = "保存固件"); publish()
            ContainerStorage(this).exportFirmware(entry, verified)
            FirmwarePreparation(this).prepare(repository.get(entry.id), verified, { paused }) { phase ->
                state = state.copy(phase = phase, error = ""); publish()
            }
            state = state.copy(phase = "固件已就绪", error = ""); publish()
        } catch (error: Exception) {
            if (paused) { state = state.copy(phase = "已暂停"); publish() } else fail(error)
        } finally { finish() }
    }

    private fun fail(error: Exception) {
        val verified = container?.let { File(repository.home(it), "firmware.ipsw").isFile } == true
        state = state.copy(phase = if (verified) "固件准备失败" else "下载失败", error = error.message ?: error.toString())
        publish()
    }
    @Synchronized private fun publish() {
        container?.let { DownloadState.write(repository, it, state) }
        getSystemService(NotificationManager::class.java).notify(NOTIFICATION, notification())
    }
    private fun notification(): android.app.Notification {
        val open = PendingIntent.getActivity(this, 0, Intent(this, MainActivity::class.java), PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE)
        val pause = PendingIntent.getService(this, 0, Intent(this, FirmwareDownloadService::class.java).setAction(PAUSE), PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE)
        return NotificationCompat.Builder(this, CHANNEL).setSmallIcon(android.R.drawable.stat_sys_download)
            .setContentTitle(container?.name ?: "固件下载").setContentText(state.toString()).setContentIntent(open)
            .setOnlyAlertOnce(true).setOngoing(true).addAction(0, "暂停", pause)
            .setProgress(100, if (state.total > 0) (state.bytes * 100 / state.total).toInt() else 0, state.total == 0L).build()
    }
    private fun foreground() {
        if (Build.VERSION.SDK_INT >= 29) startForeground(NOTIFICATION, notification(), ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC)
        else startForeground(NOTIFICATION, notification())
    }
    private fun finish() {
        container?.let { ContainerTasks.release(it.id) }
        task = null
        stopForeground(STOP_FOREGROUND_REMOVE)
        stopSelf()
    }
    override fun onTimeout(startId: Int, fgsType: Int) { paused = true; task?.cancel(); stopForeground(STOP_FOREGROUND_REMOVE); stopSelf() }
    override fun onDestroy() {
        paused = true
        task?.cancel()
        verifier.shutdown()
        super.onDestroy()
    }
    companion object {
        private const val CHANNEL = "firmware-download"
        private const val NOTIFICATION = 1001
        private const val PAUSE = "com.xxiao.ilemu.PAUSE_DOWNLOAD"
        private var initialized = false
        @Synchronized private fun initializeDownloader(context: Context) {
            if (!initialized) {
                OkDownload.setSingletonInstance(OkDownload.Builder(context)
                    .processFileStrategy(FirmwareFileStrategy()).build())
                initialized = true
            }
        }
        fun start(context: Context, id: String) = ContextCompat.startForegroundService(context,
            Intent(context, FirmwareDownloadService::class.java).putExtra("id", id))
        fun pause(context: Context) = context.startService(Intent(context, FirmwareDownloadService::class.java).setAction(PAUSE))
    }
}
