package com.xxiao.ilemu.ui

import android.view.View
import android.widget.CheckBox
import androidx.appcompat.app.AppCompatActivity

/** The editor's views are separate from firmware, storage and import operations. */
internal class ContainerEditor(activity: AppCompatActivity, editing: Boolean) {
    private val ui = ScreenUi(activity)
    val form = ui.screen(activity, if (editing) "容器设置" else "添加容器", if (editing) "固件独立保存，运行参数单独调整" else "选择设备与固件，创建一份独立的运行环境")
    val title = ui.heading(if (editing) "基本信息" else "01  设备与固件", 18)
    private val basic = ui.card(form)
    val name = ui.edit("例如：我的 iPhone", 80).apply { id = View.generateViewId() }
    val device = ui.spinner()
    private val sourceCard = ui.section(form, "固件来源")
    val firmwareLabel = ui.text("在线固件（下载后自动校验与解包）", 13, true)
    val firmware = ui.spinner()
    val reload = ui.button("刷新版本列表") {}
    val useOnline = ui.button("改用在线固件") {}
    val importDirectory = ui.button("从文件夹导入固件") {}
    val source = ui.text("可导入 rootfs、解包的 IPSW 或容器快照，自动识别版本。", 13, true)
    private val storageCard = ui.section(form, "存储位置")
    val chooseStorage = ui.button("选择文件夹") {}
    val privateStorage = ui.button("使用 App 存储") {}
    val storage = ui.text("", 13, true)
    private val advanced = ui.section(form, "运行参数")
    val cores = ui.spinner()
    val jit = ui.spinner()
    val headless = CheckBox(activity).apply { text = "无画面诊断"; minimumHeight = ui.dp(48) }
    val decryptionKey = ui.edit("自动获取失败时填写", 72)
    val status = ui.notice(form)
    val save = ui.button(if (editing) "保存设置" else "创建并准备固件", true) {}
    val cancel = ui.button("取消") { activity.finish() }
    init {
        basic.addView(title)
        ui.field(basic, "容器名称", name)
        ui.field(basic, "设备型号", device)
        sourceCard.addView(firmwareLabel)
        sourceCard.addView(firmware)
        sourceCard.addView(reload)
        sourceCard.addView(importDirectory)
        sourceCard.addView(useOnline)
        useOnline.visibility = View.GONE
        sourceCard.addView(source)
        storageCard.addView(storage)
        storageCard.addView(chooseStorage)
        storageCard.addView(privateStorage)
        ui.field(advanced, "模拟 CPU 核数", cores)
        ui.field(advanced, "每执行器 JIT 缓存（MiB）", jit)
        val extra = ui.column()
        extra.visibility = View.GONE
        val disclosure = ui.button("高级选项  ＋") {}
        disclosure.setOnClickListener {
            val opening = extra.visibility != View.VISIBLE
            extra.visibility = if (opening) View.VISIBLE else View.GONE
            disclosure.text = if (opening) "高级选项  −" else "高级选项  ＋"
        }
        advanced.addView(disclosure)
        extra.addView(headless)
        ui.field(extra, "RootFS 解密密钥（可选）", decryptionKey)
        advanced.addView(extra)
        form.addView(save)
        form.addView(cancel)
    }
}
