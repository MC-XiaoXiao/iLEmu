package com.xxiao.ilemu

import android.content.Intent
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.widget.LinearLayout
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import com.xxiao.ilemu.containers.ContainerRepository
import com.xxiao.ilemu.ui.ScreenUi
import com.xxiao.ilemu.ui.ContainerCard
import org.json.JSONArray

/** Read-only container summaries with independent icon actions. */
class MainActivity : AppCompatActivity() {
    private lateinit var repository: ContainerRepository
    private lateinit var launcher: SessionLauncher
    private lateinit var actions: ContainerActions
    private lateinit var cards: LinearLayout
    private lateinit var message: TextView
    private lateinit var count: TextView
    private lateinit var ui: ScreenUi
    private val items = linkedMapOf<String, ContainerCard>()
    private val handler = Handler(Looper.getMainLooper())
    private val refresh = object : Runnable {
        override fun run() { render(); handler.postDelayed(this, 1000) }
    }
    override fun onCreate(savedInstanceState: Bundle?) {
        AppSettings(this).applyAppearance()
        super.onCreate(savedInstanceState)
        launcher = SessionLauncher(this).also { it.restore(savedInstanceState) }
        repository = ContainerRepository(this)
        actions = ContainerActions(this, launcher)
        ui = ScreenUi(this)
        val body = ui.screen(this, getString(R.string.app_name), "版本 ${BuildConfig.VERSION_NAME}", false)
        val actions = ui.row()
        actions.addView(ui.button("＋  添加容器", true) { startActivity(Intent(this, ContainerActivity::class.java)) }, LinearLayout.LayoutParams(0, -2, 1f))
        actions.addView(ui.button("App 设置") { startActivity(Intent(this, SettingsActivity::class.java)) }, LinearLayout.LayoutParams(-2, -2).apply { leftMargin = ui.dp(10) })
        body.addView(actions)
        count = ui.text("", ScreenUi.Type.BODY, true)
        body.addView(count, LinearLayout.LayoutParams(-1, -2).apply { topMargin = ui.dp(24); bottomMargin = ui.dp(12) })
        message = ui.notice(body)
        cards = ui.column(); body.addView(cards)
        runCatching { repository.migrateLegacy() }.onFailure { message.text = "已有固件识别失败：${it.message}" }
        if (savedInstanceState == null) handleDebugCommand(intent)
    }
    private fun render() {
        runCatching {
            val entries = repository.list()
            val total = "${entries.size} 个容器"
            if (count.text.toString() != total) count.text = total
            if (items.keys.toList() == entries.map { it.id } && cards.childCount > 0) {
                entries.forEach { items[it.id]?.update(it, repository) }
                return
            }
            cards.removeAllViews(); items.clear()
            if (entries.isEmpty()) {
                val empty = ui.card(cards)
                ui.label(empty, "从一部 iPhone 开始", "添加容器，选择固件下载，或导入已准备的固件目录。每个容器拥有独立的数据与设置。")
                empty.addView(ui.button("添加第一个容器", true) { startActivity(Intent(this, ContainerActivity::class.java)) })
            }
            entries.forEach { entry ->
                items[entry.id] = ContainerCard(ui, cards, actions).also { it.update(entry, repository) }
            }
        }.onFailure { message.text = it.message }
    }
    override fun onNewIntent(intent: Intent) { super.onNewIntent(intent); setIntent(intent); handleDebugCommand(intent) }
    private fun handleDebugCommand(intent: Intent) {
        if (BuildConfig.DEBUG) intent.getStringExtra("arguments")?.let { encoded ->
            runCatching {
                require(encoded.length <= 65536)
                val args = JSONArray(encoded); require(args.length() in 1..128)
                launcher.launch(Array(args.length()) { args.getString(it) })
            }.onFailure { message.text = it.message }
        }
    }
    override fun onResume() { super.onResume(); handler.post(refresh) }
    override fun onPause() { handler.removeCallbacks(refresh); super.onPause() }
    override fun onSaveInstanceState(outState: Bundle) { launcher.save(outState); super.onSaveInstanceState(outState) }
}
