package com.droidspaces.app.ui.terminal

import com.droidspaces.app.ui.terminal.virtualkeys.VirtualKeysView
import com.termux.view.TerminalView
import java.lang.ref.WeakReference

/**
 * Holds WeakReferences to the active TerminalView and VirtualKeysView so that
 * TerminalBackEnd can read special-button state without holding strong references
 * that would prevent GC.  Mirrors the top-level vars in LXC-Manager's TerminalScreen.kt.
 */
object TerminalScreenState {
    var terminalView: WeakReference<TerminalView?> = WeakReference(null)
    var virtualKeysView: WeakReference<VirtualKeysView?> = WeakReference(null)
}
