package com.xxiao.ilemu

/** Shared physical switch state for the bezel and the session menu. */
internal class SessionControls(private val send: (String) -> Boolean) {
    var ringerSilent = false
        private set
    var onRingerChanged: ((Boolean) -> Unit)? = null
    fun command(value: String) = send(value)
    fun setRinger(silent: Boolean): Boolean {
        if (!send(if (silent) "ringer silent" else "ringer ring")) return false
        ringerSilent = silent
        onRingerChanged?.invoke(silent)
        return true
    }
    fun toggleRinger() = setRinger(!ringerSilent)
}
