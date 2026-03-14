package com.droidspaces.app.ui.terminal.virtualkeys

import android.view.View
import android.widget.Button
import com.termux.terminal.TerminalSession

class VirtualKeysListener(val session: TerminalSession) : VirtualKeysView.IVirtualKeysView {

    override fun onVirtualKeyButtonClick(
        view: View?,
        buttonInfo: VirtualKeyButton?,
        button: Button?,
    ) {
        val key = buttonInfo?.key ?: return
        val writeable: String =
            when (key) {
                "UP"    -> "\u001B[A"  // Up Arrow
                "DOWN"  -> "\u001B[B"  // Down Arrow
                "LEFT"  -> "\u001B[D"  // Left Arrow
                "RIGHT" -> "\u001B[C"  // Right Arrow
                "ENTER" -> "\u000D"    // Carriage Return
                "PGUP"  -> "\u001B[5~" // Page Up
                "PGDN"  -> "\u001B[6~" // Page Down
                "TAB"   -> "\u0009"    // Tab
                "HOME"  -> "\u001B[H"  // Home
                "END"   -> "\u001B[F"  // End
                "ESC"   -> "\u001B"    // Escape
                else    -> key
            }
        session.write(writeable)
    }

    override fun performVirtualKeyButtonHapticFeedback(
        view: View?,
        buttonInfo: VirtualKeyButton?,
        button: Button?,
    ): Boolean = false
}
