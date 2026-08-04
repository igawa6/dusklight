package dev.twilitrealm.dusk;

import android.app.Activity;
import android.graphics.Color;
import android.graphics.Point;
import android.os.Bundle;
import android.util.Log;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.Display;
import android.view.View;
import android.view.WindowManager;

import org.libsdl.app.SDLControllerManager;

/**
 * Hosts the companion screen on the device's DEFAULT display, for when the game
 * has been launched onto the secondary one ("Swap Screens").
 *
 * A Presentation cannot do this job. The framework refuses to attach
 * presentation windows to the default display, so the swapped layout needs a
 * real activity; unswapped, {@link DuskActivity.AuxPresentation} is still what
 * drives the second screen.
 */
public class DuskCompanionActivity extends Activity {
    private static final String TAG = "DuskActivity";

    /**
     * DuskActivity starts and finishes us, and both run in the same process, so
     * a static handle is enough to find the live instance. Volatile because the
     * finish comes from the game thread's pause path, not this activity's.
     */
    static volatile DuskCompanionActivity instance;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        try {
            getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            // Focusable, deliberately, even though AuxPresentation is not.
            //
            // FLAG_NOT_FOCUSABLE was tried here and does keep the controller
            // with the game — but it also means this window cannot hide the
            // status bar or the gesture pill: applyImmersive() drives
            // WindowInsetsController, and the system only honours that from a
            // window that can take focus. On the default display, where the
            // real system bars live, that left them sitting on top of the
            // companion.
            //
            // So this window takes focus and hides the bars, and the gamepad
            // problem is solved at the other end instead — see
            // dispatchKeyEvent/onGenericMotionEvent below, which hand controller
            // input back to the game window.
            DuskActivity.applyImmersive(getWindow());
            getWindow().getDecorView().setBackgroundColor(Color.BLACK);

            final Point buffer = companionBufferSize(getDisplayCompat());
            setContentView(DuskActivity.createCompanionSurfaceView(this, buffer.x, buffer.y));
            // Edge to edge, for the reason the Presentation consumes insets:
            // hiding the bars stops them being drawn but the content frame is
            // still inset for them, leaving a band the companion never fills.
            final View decor = getWindow().getDecorView();
            decor.setOnApplyWindowInsetsListener(
                (v, insets) -> android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.R
                    ? android.view.WindowInsets.CONSUMED
                    : insets.consumeSystemWindowInsets());
            decor.requestApplyInsets();
        } catch (Throwable e) {
            // We share the game's process: throwing out of here would take the
            // game down with the companion. Drop the second screen instead.
            Log.e(TAG, "Companion activity failed", e);
            finish();
            // instance was never set, so onDestroy will not report for us —
            // and the game would otherwise wait forever on a launch that is
            // already dead, never falling back to a single-screen HUD.
            DuskActivity.onCompanionActivityChanged();
            return;
        }
        instance = this;
        Log.i(TAG, "Companion activity up on display " + getDisplayCompat().getDisplayId());
        // Only now is there really a second screen to draw on; the game
        // activity could not have known at startActivity() time.
        DuskActivity.onCompanionActivityChanged();
    }

    @SuppressWarnings("deprecation")  // getDefaultDisplay(), API 26-29 fallback
    private Display getDisplayCompat() {
        if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.R) {
            Display d = getDisplay();
            if (d != null) {
                return d;
            }
        }
        return getWindowManager().getDefaultDisplay();
    }

    /**
     * The companion's render buffer, shaped like the panel it is being shown
     * on. Swapped, that panel is the main screen — 16:9 on an AYN Thor rather
     * than the bottom screen's 8:7 — and the dashboard follows: side columns
     * keep their widths and the content window and its tabs take the extra
     * room. Letterboxing an 8:7 buffer here instead would waste a third of the
     * screen on black bars.
     */
    @SuppressWarnings("deprecation")  // getRealSize(), no API 26 equivalent
    private static Point companionBufferSize(Display display) {
        final Point fallback =
            new Point(DuskActivity.COMPANION_PANEL_W, DuskActivity.COMPANION_PANEL_H);
        if (display == null) {
            return fallback;
        }
        Point size = new Point();
        try {
            display.getRealSize(size);
        } catch (Throwable t) {
            return fallback;
        }
        if (size.x <= 0 || size.y <= 0) {
            return fallback;
        }
        // computeAuxCanvas() renders 1:1 into this buffer only while both
        // dimensions are within 320..2048; outside that it drops to a 2x
        // supersample of the logical canvas, which is blurrier than the panel.
        // Scale the panel's aspect into range rather than handing over a size
        // that quietly costs sharpness.
        double scale = 1.0;
        scale = Math.min(scale, 2048.0 / Math.max(size.x, size.y));
        scale = Math.max(scale, 320.0 / Math.min(size.x, size.y));
        final int w = (int)Math.round(size.x * scale);
        final int h = (int)Math.round(size.y * scale);
        if (w < 320 || h < 320 || w > 2048 || h > 2048) {
            return fallback;  // an aspect too extreme to satisfy both bounds
        }
        return new Point(w, h);
    }

    /**
     * Controller input that lands here belongs to the game.
     *
     * Key events go to whichever window holds focus, and touching the companion
     * moves focus to this one — so without this, picking something on the
     * companion left the gamepad dead until the player touched the game screen
     * again. Forwarding into the game activity's own dispatch puts the event
     * back on SDL's path; its window keeps its internal view focus whether or
     * not the system considers it the focused window.
     */
    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (isFromController(event.getDevice())) {
            DuskActivity game = DuskActivity.instance;
            if (game != null && game.dispatchKeyEvent(event)) {
                return true;
            }
        }
        return super.dispatchKeyEvent(event);
    }

    /** Sticks and triggers, same reasoning as dispatchKeyEvent. */
    @Override
    public boolean onGenericMotionEvent(MotionEvent event) {
        if (SDLControllerManager.isDeviceSDLJoystick(event.getDeviceId())
            && SDLControllerManager.handleJoystickMotionEvent(event)) {
            return true;
        }
        return super.onGenericMotionEvent(event);
    }

    /**
     * Only real controllers are forwarded. Back/volume and the like must keep
     * working on this screen, and handing them to the game would swallow them.
     */
    private static boolean isFromController(InputDevice device) {
        if (device == null) {
            return false;
        }
        final int sources = device.getSources();
        return (sources & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
            || (sources & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
    }

    @Override
    protected void onDestroy() {
        if (instance == this) {
            instance = null;
            // Native must stop hiding the main-screen HUD for a companion that
            // no longer exists. Also covers the user swiping this task away
            // independently of the game, which dismissAux() never sees.
            DuskActivity.onCompanionActivityChanged();
        }
        super.onDestroy();
    }
}
