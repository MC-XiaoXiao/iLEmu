package com.xxiao.ilemu.ui

import android.view.View
import android.view.ViewGroup
import android.widget.FrameLayout
import android.widget.LinearLayout
import androidx.appcompat.app.AppCompatActivity
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import com.google.android.material.appbar.CollapsingToolbarLayout
import com.google.android.material.bottomnavigation.BottomNavigationView
import com.google.android.material.floatingactionbutton.FloatingActionButton
import com.xxiao.ilemu.BuildConfig
import com.xxiao.ilemu.R

/** Top-level destinations retain their views and scroll position while switching. */
internal class MainNavigation(activity: AppCompatActivity, addContainer: () -> Unit) {
    private data class Destination(val root: View, val content: LinearLayout)
    private val ui = ScreenUi(activity)
    private val root = activity.layoutInflater.inflate(R.layout.activity_main, activity.findViewById<ViewGroup>(android.R.id.content), false)
    private val library = destination(activity, activity.getString(R.string.app_name), R.id.library_app_bar, R.id.library_scroll, ui.dp(88))
    private val settings = destination(activity, activity.getString(R.string.navigation_settings), R.id.settings_app_bar, R.id.settings_scroll)
    private val navigation = root.findViewById<BottomNavigationView>(R.id.main_navigation)
    private var selected = R.id.navigation_emulators
    val libraryContent get() = library.content
    val settingsContent get() = settings.content
    val selectedTab get() = selected
    val isLibraryVisible get() = selected == R.id.navigation_emulators
    var onTabChanged: (() -> Unit)? = null
    init {
        val pages = root.findViewById<FrameLayout>(R.id.main_pages)
        pages.addView(library.root)
        pages.addView(settings.root)
        settings.root.visibility = View.GONE
        libraryContent.addView(ui.text("版本 ${BuildConfig.VERSION_NAME}", ScreenUi.Type.BODY, true))
        val fab = activity.layoutInflater.inflate(R.layout.add_container_fab, library.root as ViewGroup, false) as FloatingActionButton
        fab.setOnClickListener { addContainer() }
        library.root.addView(fab)
        navigation.setOnItemSelectedListener { item ->
            selected = item.itemId
            library.root.visibility = if (isLibraryVisible) View.VISIBLE else View.GONE
            settings.root.visibility = if (isLibraryVisible) View.GONE else View.VISIBLE
            onTabChanged?.invoke()
            true
        }
        activity.setContentView(root)
        ui.systemBars(activity)
        ViewCompat.setOnApplyWindowInsetsListener(root) { view, insets ->
            val bars = insets.getInsets(WindowInsetsCompat.Type.systemBars() or WindowInsetsCompat.Type.displayCutout())
            // BottomNavigationView applies its own navigation-bar inset.
            // AppBarLayout owns the top inset so its background extends behind the status bar.
            view.setPadding(bars.left, 0, bars.right, 0)
            insets
        }
    }
    private fun destination(activity: AppCompatActivity, title: String, appBarId: Int, scrollId: Int, bottomClearance: Int = 0): Destination {
        val view = activity.layoutInflater.inflate(R.layout.main_destination, root.findViewById<FrameLayout>(R.id.main_pages), false)
        view.findViewById<CollapsingToolbarLayout>(R.id.destination_title).apply {
            this.title = title
            // Let Material animate the status-bar scrim together with the toolbar scrim.
            statusBarScrim = contentScrim?.constantState?.newDrawable()
            id = View.NO_ID
        }
        view.findViewById<View>(R.id.destination_app_bar).id = appBarId
        view.findViewById<View>(R.id.destination_scroll).id = scrollId
        val content = view.findViewById<LinearLayout>(R.id.destination_content).apply {
            // Keep the last card reachable above the floating action, without reserving a viewport strip.
            setPadding(paddingLeft, paddingTop, paddingRight, paddingBottom + bottomClearance)
            id = View.NO_ID
        }
        return Destination(view, content)
    }
    fun select(tab: Int) {
        navigation.selectedItemId = if (tab == R.id.navigation_settings) tab else R.id.navigation_emulators
    }
}
