package com.droidspaces.app.wayland

import android.content.Context
import android.os.SystemClock
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.view.inputmethod.BaseInputConnection
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputConnection
import android.view.inputmethod.InputMethodManager
import android.widget.FrameLayout
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.viewinterop.AndroidView
import java.util.concurrent.Executors

/**
 * Wayland compositor display view.
 *
 * Architecture (mirrors trierarch's SoftKeyboardView pattern):
 *  - WaylandDisplayLayout is a FrameLayout that holds:
 *    a) The EGL SurfaceView (full-size, renders Wayland output)
 *    b) A hidden 1×1 SoftKeyboardSink that is the *sole* IME target.
 *       The sink uses TYPE_CLASS_TEXT so all IMEs show reliably, and
 *       translates commitText / sendKeyEvent → nativeOnKeyEvent.
 *  - Hardware key events go via onKeyDown/Up on the FrameLayout.
 *  - showKeyboard() / hideKeyboard() focus the sink and call showSoftInput on it.
 */


@Composable
fun WaylandDisplayView(
    resolutionPercent: Int = 100,
    scalePercent: Int = 200,
    modifier: Modifier = Modifier,
    onViewReady: ((WaylandDisplayLayout) -> Unit)? = null,
) {
    val rp = resolutionPercent.coerceIn(10, 100)
    val sp = scalePercent.coerceIn(100, 1000)
    AndroidView(
        factory  = { ctx ->
            WaylandDisplayLayout(ctx, rp, sp).also { onViewReady?.invoke(it) }
        },
        update   = { view -> (view as? WaylandDisplayLayout)?.updateParams(rp, sp) },
        modifier = modifier,
    )
}

// ---------------------------------------------------------------------------
// Hidden IME sink — 1×1 invisible view, sole target for showSoftInput()
// ---------------------------------------------------------------------------

internal class SoftKeyboardSink(context: Context) : View(context) {

    private val commitExecutor = Executors.newSingleThreadExecutor { r ->
        Thread(r, "WaylandCommitExecutor").apply { isDaemon = true }
    }

    private var activeInputConnection: InputConnection? = null

    init {
        isFocusable = true
        isFocusableInTouchMode = true
    }

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        setMeasuredDimension(1, 1)
    }

    override fun onCheckIsTextEditor() = true

    override fun onCreateInputConnection(outAttrs: EditorInfo): InputConnection {
        // TYPE_CLASS_TEXT: required so all IMEs (Samsung, Gboard, SwiftKey) show reliably.
        // The sink never displays text — it only forwards events to Wayland.
        outAttrs.inputType =
           EditorInfo.TYPE_CLASS_TEXT or
           EditorInfo.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD or
           EditorInfo.TYPE_TEXT_FLAG_NO_SUGGESTIONS

       outAttrs.imeOptions =
          EditorInfo.IME_FLAG_NO_FULLSCREEN or
          EditorInfo.IME_FLAG_NO_EXTRACT_UI
        val connection = object : BaseInputConnection(this, true) {
            override fun closeConnection() {
                if (activeInputConnection === this) {
                    activeInputConnection = null
                }

                super.closeConnection()
            }

            // Hardware-style key events from IME (Backspace, Enter, arrows)
            override fun sendKeyEvent(event: KeyEvent): Boolean {
                WaylandSurface.nativeOnKeyEvent(
                    event.keyCode, event.action == KeyEvent.ACTION_DOWN,
                    event.eventTime.toInt())
                return true
            }

            // Composing text: forward each update immediately so keystrokes aren't lost
            override fun setComposingText(text: CharSequence?, newCursorPosition: Int): Boolean {
                return true
            }

            // Flush residual composing buffer on IME commit/dismiss
            override fun finishComposingText(): Boolean {
                super.finishComposingText()
                getEditable()?.clear()
                return true
            }

            override fun deleteSurroundingText(beforeLength: Int, afterLength: Int): Boolean {
                val b = beforeLength.coerceIn(0, 256)
                val a = afterLength.coerceIn(0, 256)
                commitExecutor.execute {
                    var t = System.currentTimeMillis()
                    repeat(b) {
                        WaylandSurface.nativeOnKeyEvent(KeyEvent.KEYCODE_DEL, true,  t.toInt()); t++
                        WaylandSurface.nativeOnKeyEvent(KeyEvent.KEYCODE_DEL, false, t.toInt()); t++
                    }
                    repeat(a) {
                        WaylandSurface.nativeOnKeyEvent(KeyEvent.KEYCODE_FORWARD_DEL, true,  t.toInt()); t++
                        WaylandSurface.nativeOnKeyEvent(KeyEvent.KEYCODE_FORWARD_DEL, false, t.toInt()); t++
                    }
                }
                return super.deleteSurroundingText(beforeLength, afterLength)
            }

            override fun commitText(text: CharSequence?, newCursorPosition: Int): Boolean {
                if (text.isNullOrEmpty()) return true
                val str = text.toString()
                commitExecutor.execute {
                    var t = System.currentTimeMillis()
                    fun dn(kc: Int) { WaylandSurface.nativeOnKeyEvent(kc, true,  t.toInt()); t++ }
                    fun up(kc: Int) { WaylandSurface.nativeOnKeyEvent(kc, false, t.toInt()); t++ }
                    fun tap(kc: Int) { dn(kc); up(kc) }

                    val cps = str.codePoints().toArray()
                    for ((i, cp) in cps.withIndex()) {
                        when (cp) {
                            10 -> { tap(KeyEvent.KEYCODE_ENTER); continue }
                            9  -> { tap(KeyEvent.KEYCODE_TAB);   continue }
                        }
                        if (cp in 32..126) {
                            val (kc, shift) = asciiToKeyCode(cp.toChar())
                            if (kc != 0) {
                                if (shift) dn(KeyEvent.KEYCODE_SHIFT_LEFT)
                                tap(kc)
                                if (shift) up(KeyEvent.KEYCODE_SHIFT_LEFT)
                                continue
                            }
                        }
                        // Unicode fallback: Ctrl+Shift+U <hex> Space (GTK ISO-14755)
                        dn(KeyEvent.KEYCODE_CTRL_LEFT); dn(KeyEvent.KEYCODE_SHIFT_LEFT)
                        tap(KeyEvent.KEYCODE_U)
                        up(KeyEvent.KEYCODE_SHIFT_LEFT); up(KeyEvent.KEYCODE_CTRL_LEFT)
                        for (ch in cp.toString(16)) {
                            val (kc, sh) = asciiToKeyCode(ch)
                            if (kc != 0) {
                                if (sh) dn(KeyEvent.KEYCODE_SHIFT_LEFT)
                                tap(kc)
                                if (sh) up(KeyEvent.KEYCODE_SHIFT_LEFT)
                            }
                        }
                        tap(KeyEvent.KEYCODE_SPACE)
                        // Throttle long pastes to avoid flooding the key queue
                        if (cps.size >= 200 && (i + 1) % 8 == 0) {
                            try { Thread.sleep(2) } catch (_: InterruptedException) {}
                        } else if ((i + 1) % 64 == 0) {
                            try { Thread.sleep(1) } catch (_: InterruptedException) {}
                        }
                    }
                }
                return true
            }
        }
        activeInputConnection = connection
        return connection
    }

     fun pasteClipboard(): Boolean {
        val connection = activeInputConnection
            ?: return false

        val clipboard =
            context.getSystemService(
                Context.CLIPBOARD_SERVICE
            ) as? android.content.ClipboardManager
                ?: return false

        if (!clipboard.hasPrimaryClip()) {
            return false
        }

        val clip = clipboard.primaryClip
            ?: return false

        if (clip.itemCount <= 0) {
            return false
        }

        val text = clip
            .getItemAt(0)
            .coerceToText(context)
            ?.toString()
            ?: return false

        if (text.isEmpty()) {
            return false
        }

        /*
         * PENTING:
         * Jangan memanggil nativeOnText().
         *
         * commitText() ini masuk ke implementasi commitText()
         * yang sudah ada di class ini.
         */
        connection.commitText(text, 1)

        return true
    }   

    // ASCII → (Android KEYCODE, needsShift)
    private fun asciiToKeyCode(c: Char): Pair<Int, Boolean> = when (val code = c.code) {
        in 48..57  -> KeyEvent.KEYCODE_0 + (code - 48) to false
        in 97..122 -> KeyEvent.KEYCODE_A + (code - 97) to false
        in 65..90  -> KeyEvent.KEYCODE_A + (code - 65) to true
        32  -> KeyEvent.KEYCODE_SPACE         to false
        45  -> KeyEvent.KEYCODE_MINUS         to false
        61  -> KeyEvent.KEYCODE_EQUALS        to false
        91  -> KeyEvent.KEYCODE_LEFT_BRACKET  to false
        93  -> KeyEvent.KEYCODE_RIGHT_BRACKET to false
        59  -> KeyEvent.KEYCODE_SEMICOLON     to false
        39  -> KeyEvent.KEYCODE_APOSTROPHE    to false
        44  -> KeyEvent.KEYCODE_COMMA         to false
        46  -> KeyEvent.KEYCODE_PERIOD        to false
        47  -> KeyEvent.KEYCODE_SLASH         to false
        92  -> KeyEvent.KEYCODE_BACKSLASH     to false
        96  -> KeyEvent.KEYCODE_GRAVE         to false
        33  -> KeyEvent.KEYCODE_1             to true   // !
        64  -> KeyEvent.KEYCODE_2             to true   // @
        35  -> KeyEvent.KEYCODE_3             to true   // #
        36  -> KeyEvent.KEYCODE_4             to true   // $
        37  -> KeyEvent.KEYCODE_5             to true   // %
        94  -> KeyEvent.KEYCODE_6             to true   // ^
        38  -> KeyEvent.KEYCODE_7             to true   // &
        42  -> KeyEvent.KEYCODE_8             to true   // *
        40  -> KeyEvent.KEYCODE_9             to true   // (
        41  -> KeyEvent.KEYCODE_0             to true   // )
        95  -> KeyEvent.KEYCODE_MINUS         to true   // _
        43  -> KeyEvent.KEYCODE_EQUALS        to true   // +
        123 -> KeyEvent.KEYCODE_LEFT_BRACKET  to true   // {
        125 -> KeyEvent.KEYCODE_RIGHT_BRACKET to true   // }
        58  -> KeyEvent.KEYCODE_SEMICOLON     to true   // :
        34  -> KeyEvent.KEYCODE_APOSTROPHE    to true   // "
        60  -> KeyEvent.KEYCODE_COMMA         to true   // <
        62  -> KeyEvent.KEYCODE_PERIOD        to true   // >
        63  -> KeyEvent.KEYCODE_SLASH         to true   // ?
        124 -> KeyEvent.KEYCODE_BACKSLASH     to true   // |
        126 -> KeyEvent.KEYCODE_GRAVE         to true   // ~
        else -> 0 to false
    }
}

// ---------------------------------------------------------------------------
// Display layout — holds SurfaceView + hidden IME sink
// ---------------------------------------------------------------------------

class WaylandDisplayLayout(
    context: Context,
    private var resolutionPercent: Int,
    private var scalePercent: Int,
) : FrameLayout(context) {

    //pending ime animation-----
    private var imeAnimating = false

    private var pendingWidth = 0
    private var pendingHeight = 0

    private val resizeCommit = Runnable {
        WaylandSurface.nativeOutputSizeChanged(
            pendingWidth,
            pendingHeight,
            resolutionPercent,
            scalePercent
        )
    }

    fun setImeAnimating(animating: Boolean) {
        imeAnimating = animating

        if (!animating) {
            removeCallbacks(resizeCommit)

            if (pendingWidth > 0 && pendingHeight > 0) {
                WaylandSurface.nativeOutputSizeChanged(
                    pendingWidth,
                    pendingHeight,
                    resolutionPercent,
                    scalePercent
                )
            }
        }
    }
    //--------------------
    
    private val imeSink = SoftKeyboardSink(context)

    /* -----------------------------------------------------------------------
     * PATCH:
     * Trierarch-compatible input router.
     *
     * Semua routing touch/mouse nantinya dipindahkan ke WaylandTouchLayout
     * tanpa mengubah lifecycle SurfaceView maupun IME.
     * ----------------------------------------------------------------------*/
    private val coordMapper = WaylandCoordMapper()

    private val inputRouter by lazy {
        WaylandTouchLayout(
            context,
            coordMapper
        )
    }

    init {
        isFocusable = true
        isFocusableInTouchMode = true

        val surfaceView = SurfaceView(context).also { sv ->
            sv.holder.addCallback(object : SurfaceHolder.Callback {
                override fun surfaceCreated(h: SurfaceHolder) {
                    val s = h.surface ?: return
                    WaylandSurface.nativeSurfaceCreated(s, resolutionPercent, scalePercent)
                }
                override fun surfaceChanged(
                    holder: SurfaceHolder,
                    format: Int,
                    w: Int,
                    h: Int
                ) {
                    if (w <= 0 || h <= 0)
                        return

                    pendingWidth = w
                    pendingHeight = h

                    if (!imeAnimating) {

                        removeCallbacks(resizeCommit)

                        WaylandSurface.nativeOutputSizeChanged(
                            w,
                            h,
                            resolutionPercent,
                            scalePercent
                        )

                    } else {

                        removeCallbacks(resizeCommit)

                           postDelayed(
                           resizeCommit,
                           16L
                        )
                    }
                    /* PATCH:
                     * Sinkronkan ukuran Surface dengan mapper Trierarch.
                     */
                    
                    coordMapper.setSurfaceSize(w, h)
                }
                override fun surfaceDestroyed(h: SurfaceHolder) {
                    WaylandSurface.nativeSurfaceDestroyed()
                }
            })
        }

        addView(surfaceView, LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.MATCH_PARENT,
            Gravity.CENTER,
        ))

        // Add the 1×1 hidden IME sink (invisible, zero-alpha, non-interactive)
        addView(imeSink, LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT,
            ViewGroup.LayoutParams.WRAP_CONTENT,
            Gravity.TOP or Gravity.START,
        ))

        WaylandSurface.nativeSetCursorVisible(false)
        /* -----------------------------------------------------------------------
         * PATCH:
         * WaylandTouchLayout tidak menggambar apapun.
         * Ia hanya menerima seluruh MotionEvent kemudian meneruskannya
         * ke JNI sehingga DisplayLayout cukup menjadi host.
         * ----------------------------------------------------------------------*/

        addView(
            inputRouter,
            LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT
            )
        )
    }

    fun updateParams(rp: Int, sp: Int) {
        resolutionPercent = rp
        scalePercent = sp
    }

    /** Show soft keyboard — focus the IME sink and call showSoftInput on it. */
    fun showKeyboard() {
        imeSink.requestFocus()
        val imm = context.getSystemService(Context.INPUT_METHOD_SERVICE) as? InputMethodManager
        imm?.showSoftInput(imeSink, InputMethodManager.SHOW_FORCED)
    }

    /** Hide soft keyboard. */
    fun hideKeyboard() {
        val imm = context.getSystemService(Context.INPUT_METHOD_SERVICE) as? InputMethodManager
        imm?.hideSoftInputFromWindow(imeSink.windowToken, 0)
    }

    fun pasteClipboard(): Boolean {
        return imeSink.pasteClipboard()
    }

    // ---- Hardware keyboard ------------------------------------------------

    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        if (keyCode == KeyEvent.KEYCODE_BACK) return false
        WaylandSurface.nativeOnKeyEvent(keyCode, true, uptimeMs())
        return true
    }

    override fun onKeyUp(keyCode: Int, event: KeyEvent): Boolean {
        if (keyCode == KeyEvent.KEYCODE_BACK) return false
        WaylandSurface.nativeOnKeyEvent(keyCode, false, uptimeMs())
        return true
    }

    // ---- Touch input ------------------------------------------------------

    override fun onInterceptTouchEvent(ev: MotionEvent): Boolean {
        return inputRouter.onInterceptTouchEvent(ev)
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        return inputRouter.onTouchEvent(event)
    }

    override fun onGenericMotionEvent(event: MotionEvent): Boolean {

        return inputRouter.onGenericMotionEvent(event)
    }
    // ---- Helpers ----------------------------------------------------------

    private fun uptimeMs() = (SystemClock.uptimeMillis() and 0x7FFF_FFFFL).toInt()
}
