package com.xxiao.ilemu

import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.view.View
import android.widget.AdapterView
import android.widget.ArrayAdapter
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import androidx.lifecycle.ViewModelProvider
import androidx.documentfile.provider.DocumentFile
import com.xxiao.ilemu.containers.*
import com.xxiao.ilemu.databinding.ActivityContainerBinding
import com.xxiao.ilemu.firmware.*
import java.util.concurrent.Executors

class ContainerActivity : AppCompatActivity() {
    private lateinit var binding: ActivityContainerBinding
    private lateinit var repository: ContainerRepository
    private var original: Container? = null
    private var storageTree = ""
    private var sourceUri: Uri? = null
    private var identity: FirmwareIdentity? = null
    private var models = emptyList<DeviceOption>()
    private var releases = emptyList<FirmwareRelease>()
    private val executor = Executors.newSingleThreadExecutor()
    private var catalogGeneration = 0
    private var working = false
    private lateinit var operation: ContainerOperation
    private val jitSizes = listOf(8, 16, 32, 64, 128)

    private val sourcePicker = registerForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri ->
        if (uri != null) {
            try {
                contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
                sourceUri = uri
                inspectSource(uri)
            } catch (error: Exception) { binding.status.text = error.message }
        }
    }
    private val storagePicker = registerForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri ->
        if (uri != null) {
            try {
                contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION)
                storageTree = uri.toString()
                showStorage()
            } catch (error: Exception) { binding.status.text = error.message }
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityContainerBinding.inflate(layoutInflater)
        setContentView(binding.root)
        ViewCompat.setOnApplyWindowInsetsListener(binding.form) { view, insets ->
            val bars = insets.getInsets(WindowInsetsCompat.Type.systemBars() or WindowInsetsCompat.Type.displayCutout())
            val padding = (20 * resources.displayMetrics.density).toInt()
            view.setPadding(padding + bars.left, padding + bars.top, padding + bars.right, padding + bars.bottom)
            insets
        }
        repository = ContainerRepository(this)
        operation = ViewModelProvider(this)[ContainerOperation::class.java]
        try {
            original = intent.getStringExtra("id")?.let(repository::get)
            original?.let { check(!ContainerTasks.isBusy(it.id) || operation.state.value?.working == true) { "容器正在使用中" } }
            models = FirmwareMetadata.models()
        } catch (error: Exception) { binding.status.text = error.message; binding.save.isEnabled = false; return }
        storageTree = savedInstanceState?.getString("storage") ?: original?.storageTree ?: ""
        sourceUri = savedInstanceState?.getString("source")?.let(Uri::parse)
        binding.device.adapter = adapter(models)
        binding.cores.adapter = adapter((1..8).toList())
        binding.jit.adapter = adapter(jitSizes)
        val entry = original
        if (savedInstanceState == null) binding.name.setText(entry?.name ?: "")
        binding.device.setSelection(models.indexOfFirst { it.identifier == entry?.device }.coerceAtLeast(0))
        binding.cores.setSelection((entry?.cores ?: 1) - 1)
        binding.jit.setSelection(jitSizes.indexOf(entry?.jitCacheMiB ?: 16).coerceAtLeast(0))
        binding.decryptionKey.setText(entry?.decryptionKey ?: "")
        binding.headless.isChecked = entry?.headless ?: false
        binding.device.isEnabled = entry?.ready != true && entry?.firmware == null
        if (entry != null) {
            binding.title.text = "容器设置"
            binding.save.text = "保存设置"
            binding.source.text = "iOS ${entry.version} (${entry.build})" + if (entry.ready) " · 运行固件已就绪" else " · 等待导入运行固件"
        }
        if (entry?.ready == true || entry?.firmware != null) {
            binding.firmware.visibility = View.GONE
            binding.firmwareLabel.visibility = View.GONE
            binding.reload.visibility = View.GONE
        }
        binding.importDirectory.isEnabled = entry?.ready != true
        binding.device.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onNothingSelected(parent: AdapterView<*>?) = Unit
            override fun onItemSelected(parent: AdapterView<*>?, view: View?, position: Int, id: Long) {
                if (entry?.ready != true && entry?.firmware == null) loadCatalog()
            }
        }
        binding.reload.setOnClickListener { loadCatalog() }
        binding.importDirectory.setOnClickListener { sourcePicker.launch(sourceUri) }
        binding.chooseStorage.setOnClickListener { storagePicker.launch(storageTree.takeIf { it.isNotEmpty() }?.let(Uri::parse)) }
        binding.privateStorage.setOnClickListener { storageTree = ""; showStorage() }
        binding.save.setOnClickListener { save() }
        binding.cancel.setOnClickListener { finish() }
        showStorage()
        sourceUri?.let(::inspectSource)
        operation.state.observe(this) { state ->
            working = state.working
            binding.save.isEnabled = !working
            binding.cancel.isEnabled = !working
            if (working) binding.status.text = "正在保存固件与容器，请稍候…"
            state.error?.let { binding.status.text = "保存失败：${it.message}" }
            if (state.completed) finish()
        }
    }

    private fun <T> adapter(items: List<T>) = ArrayAdapter(this, android.R.layout.simple_spinner_item, items).apply {
        setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item)
    }
    private fun showStorage() {
        binding.storage.text = if (storageTree.isEmpty()) "运行目录：App 私有数据。卸载 App 会删除私有容器。"
        else "存储文件夹：${folderName(Uri.parse(storageTree))}\n所选目录持久保存固件与数据；App 内维护支持符号链接的运行副本，停止后自动同步。所选目录和运行副本都会占用空间。"
    }
    private fun folderName(uri: Uri): String = runCatching {
        DocumentFile.fromTreeUri(this, uri)?.name
    }.getOrNull() ?: "所选文件夹"
    private fun loadCatalog() {
        val generation = ++catalogGeneration
        val device = models[binding.device.selectedItemPosition].identifier
        releases = emptyList()
        binding.firmware.adapter = adapter(listOf("正在获取固件版本…"))
        executor.execute {
            val result = runCatching { FirmwareCatalog(this).releases(device) }
            runOnUiThread {
                if (isDestroyed || generation != catalogGeneration) return@runOnUiThread
                releases = result.getOrDefault(emptyList())
                binding.firmware.adapter = if (releases.isEmpty()) adapter(listOf("暂无可校验的固件")) else adapter(releases)
                if (result.isFailure && !working) binding.status.text = "获取固件失败：${result.exceptionOrNull()?.message}；仍可从目录导入。"
            }
        }
    }
    private fun inspectSource(uri: Uri) {
        identity = null
        binding.status.text = "读取固件 plist…"
        executor.execute {
            val result = runCatching { FirmwareImporter(this).identity(uri) }
            runOnUiThread {
                if (isDestroyed || sourceUri != uri) return@runOnUiThread
                identity = result.getOrNull()
                binding.source.text = identity?.let { "iOS ${it.version} (${it.build})\n文件夹：${folderName(uri)}" } ?: "目录识别失败"
                if (!working) binding.status.text = result.exceptionOrNull()?.message ?: "已识别固件版本，创建时导入完整目录。"
                identity?.devices?.firstOrNull()?.let { device ->
                    val index = models.indexOfFirst { it.identifier == device }
                    if (index >= 0 && binding.device.isEnabled) binding.device.setSelection(index)
                }
            }
        }
    }
    private fun save() {
        if (working) return
        try {
            val name = binding.name.text.toString().trim()
            require(name.length in 1..80) { "请输入容器名称（1–80 个字符）" }
            val device = models[binding.device.selectedItemPosition].identifier
            val key = binding.decryptionKey.text.toString().trim()
            require(key.isEmpty() || key.matches(Regex("[a-fA-F0-9]{72}"))) { "RootFS Key 应为 72 位十六进制字符串" }
            if (sourceUri != null) require(identity != null && identity!!.accepts(device)) { "请先完成目录识别，并选择匹配的设备" }
            val selected = if (sourceUri == null && original?.firmware == null && original?.ready != true)
                releases.getOrNull(binding.firmware.selectedItemPosition) else null
            require(original != null || sourceUri != null || selected != null) { "请选择固件版本或导入目录" }
            val entry = (original ?: Container(name = name, device = device)).copy(name = name, device = device,
                storageTree = storageTree, decryptionKey = key, headless = binding.headless.isChecked,
                cores = binding.cores.selectedItemPosition + 1, jitCacheMiB = jitSizes[binding.jit.selectedItemPosition],
                version = selected?.version ?: original?.version ?: "",
                build = selected?.build ?: original?.build ?: "",
                firmware = original?.firmware ?: selected?.json())
            val source = sourceUri
            working = true
            binding.save.isEnabled = false
            binding.cancel.isEnabled = false
            binding.status.text = if (source != null) "正在导入固件，请保持此页面打开…" else "正在保存容器…"
            val app = applicationContext
            val previousStorage = original?.storageTree
            val store = repository
            operation.start(entry.id, {
                if (source != null) FirmwareImporter(app).importTree(entry, source)
                else {
                    if (entry.ready && entry.storageTree.isNotEmpty() && entry.storageTree != previousStorage)
                        ContainerStorage(app).checkpoint(entry)
                    store.save(entry)
                }
            }, {
                if (source == null && !entry.ready && entry.firmware != null) FirmwareDownloadService.start(app, entry.id)
            })
        } catch (error: Exception) { working = false; binding.status.text = error.message }
    }
    override fun onSaveInstanceState(outState: Bundle) {
        outState.putString("storage", storageTree)
        sourceUri?.let { outState.putString("source", it.toString()) }
        super.onSaveInstanceState(outState)
    }
    override fun onDestroy() { executor.shutdown(); super.onDestroy() }
}
