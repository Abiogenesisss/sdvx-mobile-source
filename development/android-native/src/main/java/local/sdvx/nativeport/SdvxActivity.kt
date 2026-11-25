package local.sdvx.nativeport

import android.content.Context
import android.content.pm.ActivityInfo
import android.os.Build
import android.os.Bundle
import android.graphics.Rect
import android.hardware.input.InputManager
import android.media.MediaScannerConnection
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.util.Base64
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.Surface
import android.view.SurfaceHolder
import android.view.WindowInsets
import org.libsdl.app.SDLActivity
import org.libsdl.app.SDLSurface
import java.security.MessageDigest
import java.security.SecureRandom
import java.text.Normalizer
import java.util.Locale
import java.time.LocalDate
import java.util.concurrent.CountDownLatch
import javax.crypto.SecretKeyFactory
import javax.crypto.spec.PBEKeySpec

private external fun nativeRotary(x: Float, y: Float, ageMs: Long)
private external fun nativeControllerInput(key: Int, value: Int, ageMs: Long)

class SdvxActivity : SDLActivity(), InputManager.InputDeviceListener {
    @Volatile private var keyboardPixels = 0
    private val inputManager by lazy { getSystemService(InputManager::class.java) }
    private var controllerDevice = -1
    @Volatile private var controllerMode = 0
    private var dialScaleX = 0f
    private var dialScaleY = 0f
    private var previousX = 0f
    private var previousY = 0f
    private var rotaryBaseline = false
    private val preparedResourceImages = HashSet<String>()

    @Synchronized
    fun prepareExternalImage(path: String): Boolean {
        if (path in preparedResourceImages) return true
        // Native asset reads run off the Android UI thread. Wait for the scan
        // callback so the first fopen sees the .nomedia/ownership entry too.
        val complete = CountDownLatch(1)
        var ready = false
        MediaScannerConnection.scanFile(applicationContext, arrayOf(path), null) { _, uri ->
            ready = uri != null
            complete.countDown()
        }
        complete.await()
        if (ready) preparedResourceImages.add(path)
        return ready
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        if (mBrokenLibraries) return
        inputManager.registerInputDeviceListener(this, Handler(Looper.getMainLooper()))
        refreshController()
        window.decorView.setOnApplyWindowInsetsListener { _, insets ->
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                keyboardPixels = insets.getInsets(WindowInsets.Type.ime()).bottom
            }
            insets
        }
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) {
            val visible = Rect()
            window.decorView.viewTreeObserver.addOnGlobalLayoutListener {
                window.decorView.getWindowVisibleDisplayFrame(visible)
                val hidden = window.decorView.height - visible.bottom
                keyboardPixels = if (hidden > window.decorView.height / 4) hidden else 0
            }
        }
    }

    override fun onDestroy() {
        if (!mBrokenLibraries) inputManager.unregisterInputDeviceListener(this)
        super.onDestroy()
    }

    fun controllerInputMode(): Int = controllerMode

    override fun onInputDeviceAdded(deviceId: Int) = refreshController()
    override fun onInputDeviceRemoved(deviceId: Int) = refreshController()
    override fun onInputDeviceChanged(deviceId: Int) = refreshController(force = deviceId == controllerDevice)

    private fun refreshController(force: Boolean = false) {
        val devices = inputManager.inputDeviceIds.asSequence().mapNotNull(inputManager::getInputDevice)
            .filter { !it.isVirtual && (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q || it.isExternal) }.toList()
        // A keyboard/mouse controller can expose the two interfaces under different IDs.
        val mouse = devices.firstOrNull {
            it.supportsSource(InputDevice.SOURCE_MOUSE) || it.supportsSource(InputDevice.SOURCE_MOUSE_RELATIVE)
        }
            ?.takeIf { devices.any { it.supportsSource(InputDevice.SOURCE_KEYBOARD) } }
        val joystick = if (mouse != null) null else devices.firstOrNull {
            it.supportsSource(InputDevice.SOURCE_JOYSTICK) &&
                ((it.vendorId == 0x1ccf && it.productId == 0x101c) ||
                    it.name.contains("SOUND VOLTEX", ignoreCase = true) ||
                    it.name.contains("NEMSYS", ignoreCase = true))
        }
        val device = joystick ?: mouse
        val id = device?.id ?: -1
        val mode = when {
            mouse != null -> 1
            joystick == null -> 0
            id == controllerDevice && controllerMode == 1 -> 1
            else -> 2
        }
        if (!force && id == controllerDevice && mode == controllerMode) return
        controllerDevice = id
        controllerMode = mode
        dialScaleX = joystick?.getMotionRange(MotionEvent.AXIS_X, InputDevice.SOURCE_JOYSTICK)?.let { 255f / it.range } ?: 0f
        dialScaleY = joystick?.getMotionRange(MotionEvent.AXIS_Y, InputDevice.SOURCE_JOYSTICK)?.let { 255f / it.range } ?: 0f
        rotaryBaseline = false
        nativeControllerInput(0, mode, 0)
    }

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (controllerMode == 2 && event.deviceId == controllerDevice) {
            val key = when (event.keyCode) {
                in KeyEvent.KEYCODE_BUTTON_1..KeyEvent.KEYCODE_BUTTON_16 -> event.keyCode - KeyEvent.KEYCODE_BUTTON_1 + 1
                in KeyEvent.KEYCODE_BUTTON_A..KeyEvent.KEYCODE_BUTTON_Z -> event.keyCode - KeyEvent.KEYCODE_BUTTON_A + 1
                KeyEvent.KEYCODE_BUTTON_START -> 9
                KeyEvent.KEYCODE_BUTTON_SELECT -> 17
                else -> {
                    // Some composite controllers advertise a joystick but send real keyboard keys.
                    if (event.action == KeyEvent.ACTION_DOWN && event.scanCode != 0 &&
                        event.isFromSource(InputDevice.SOURCE_KEYBOARD) &&
                        event.keyCode != KeyEvent.KEYCODE_BACK && !KeyEvent.isGamepadButton(event.keyCode)) {
                        controllerMode = 1
                        rotaryBaseline = false
                        nativeControllerInput(0, controllerMode, 0)
                    }
                    return super.dispatchKeyEvent(event)
                }
            }
            if (event.repeatCount == 0 && (event.action == KeyEvent.ACTION_DOWN || event.action == KeyEvent.ACTION_UP))
                nativeControllerInput(key, if (event.action == KeyEvent.ACTION_DOWN) 1 else 0,
                    SystemClock.uptimeMillis() - event.eventTime)
            return true
        }
        return super.dispatchKeyEvent(event)
    }

    override fun dispatchGenericMotionEvent(event: MotionEvent): Boolean {
        if (controllerMode != 2 || event.deviceId != controllerDevice ||
            !event.isFromSource(InputDevice.SOURCE_JOYSTICK) || event.actionMasked != MotionEvent.ACTION_MOVE)
            return super.dispatchGenericMotionEvent(event)
        val now = SystemClock.uptimeMillis()
        // NEMSYS X/Y are cyclic 8-bit positions. Convert wraparound to signed movement.
        for (i in 0..event.historySize) {
            val current = i == event.historySize
            val x = if (current) event.getAxisValue(MotionEvent.AXIS_X) else event.getHistoricalAxisValue(MotionEvent.AXIS_X, i)
            val y = if (current) event.getAxisValue(MotionEvent.AXIS_Y) else event.getHistoricalAxisValue(MotionEvent.AXIS_Y, i)
            if (rotaryBaseline) {
                val dx = (x - previousX) * dialScaleX
                val dy = (y - previousY) * dialScaleY
                if (dx != 0f || dy != 0f)
                    nativeRotary(
                        dx - 256f * kotlin.math.round(dx / 256f),
                        dy - 256f * kotlin.math.round(dy / 256f),
                        now - if (current) event.eventTime else event.getHistoricalEventTime(i)
                    )
            }
            previousX = x
            previousY = y
            rotaryBaseline = true
        }
        return true
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        rotaryBaseline = false
        super.onWindowFocusChanged(hasFocus)
    }

    fun keyboardHeight(): Int = keyboardPixels

    @Suppress("DEPRECATION") // Keep the extracted files reachable from a file manager and native code.
    fun resourceDirectory(): String = externalMediaDirs.firstOrNull()?.resolve("resources")?.let {
        it.mkdirs()
        it.resolve(".nomedia").createNewFile()
        it.absolutePath
    }.orEmpty()

    override fun onPause() {
        super.onPause()
        // SDL defers pause until onStop on multi-window-capable Android. Pause
        // gameplay as soon as this Activity is no longer the active screen.
        pauseNativeThread()
    }

    override fun onResume() {
        super.onResume()
        resumeNativeThread()
    }

    override fun createSDLSurface(context: Context): SDLSurface = RotarySurface(context)

    fun showSubscreen(landscape: Boolean) = runOnUiThread {
        requestedOrientation = if (landscape) ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
            else ActivityInfo.SCREEN_ORIENTATION_PORTRAIT
    }

    fun normalizeSearch(value: ByteArray): ByteArray = Normalizer.normalize(value.toString(Charsets.UTF_8), Normalizer.Form.NFKC)
        .lowercase(Locale.ROOT).map { if (it in '\u3041'..'\u3096') it + 0x60 else it }.joinToString("").toByteArray(Charsets.UTF_8)

    fun localDay(): Long = LocalDate.now().toEpochDay()

    private val localCard by lazy { getSharedPreferences("local_card", Context.MODE_PRIVATE) }
    private val cardRandom by lazy { SecureRandom() }

    // The native entry screen receives the card ID and a PIN-enrolment flag.
    fun localCardState(): String {
        val id = localCard.getString("id", null) ?: run {
            val generated = ByteArray(8).also(cardRandom::nextBytes)
                .joinToString("") { "%02X".format(it.toInt() and 0xff) }
            if (!localCard.edit().putString("id", generated).commit()) return ""
            generated
        }
        return id + if (localCard.contains("pin_verifier")) "1" else "0"
    }

    fun submitLocalPin(pin: String): Boolean {
        val stored = localCard.getString("pin_verifier", null)
        val salt = if (stored == null) ByteArray(16).also(cardRandom::nextBytes)
            else Base64.decode(localCard.getString("pin_salt", null) ?: return false, Base64.NO_WRAP)
        val spec = PBEKeySpec(pin.toCharArray(), salt, 120_000, 256)
        val verifier = try {
            SecretKeyFactory.getInstance("PBKDF2WithHmacSHA256").generateSecret(spec).encoded
        } finally {
            spec.clearPassword()
        }
        if (stored != null) return MessageDigest.isEqual(verifier, Base64.decode(stored, Base64.NO_WRAP))
        return localCard.edit()
            .putString("pin_salt", Base64.encodeToString(salt, Base64.NO_WRAP))
            .putString("pin_verifier", Base64.encodeToString(verifier, Base64.NO_WRAP))
            .commit()
    }
}

private class RotarySurface(context: Context) : SDLSurface(context) {
    override fun surfaceCreated(holder: SurfaceHolder) {
        super.surfaceCreated(holder)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            holder.surface.setFrameRate(120f, Surface.FRAME_RATE_COMPATIBILITY_DEFAULT)
        }
    }

    override fun onCapturedPointerEvent(event: MotionEvent): Boolean {
        val action = event.actionMasked
        if (action != MotionEvent.ACTION_MOVE && action != MotionEvent.ACTION_HOVER_MOVE) {
            return super.onCapturedPointerEvent(event)
        }
        val now = SystemClock.uptimeMillis()
        // Captured mouse events may batch several physical samples with distinct times.
        for (index in 0 until event.historySize) {
            val dx = event.getHistoricalAxisValue(MotionEvent.AXIS_RELATIVE_X, index)
            val dy = event.getHistoricalAxisValue(MotionEvent.AXIS_RELATIVE_Y, index)
            val relativeAxes = dx != 0f || dy != 0f
            nativeRotary(
                if (relativeAxes) dx else event.getHistoricalX(index),
                if (relativeAxes) dy else event.getHistoricalY(index),
                now - event.getHistoricalEventTime(index)
            )
        }
        val dx = event.getAxisValue(MotionEvent.AXIS_RELATIVE_X)
        val dy = event.getAxisValue(MotionEvent.AXIS_RELATIVE_Y)
        val relativeAxes = dx != 0f || dy != 0f
        nativeRotary(
            if (relativeAxes) dx else event.x,
            if (relativeAxes) dy else event.y,
            now - event.eventTime
        )
        return true
    }
}
