package dev.twilitrealm.dusk;

import android.app.ActionBar;
import android.app.Activity;
import android.app.ActivityOptions;
import android.app.Presentation;
import android.content.BroadcastReceiver;
import android.content.IntentFilter;
import android.hardware.display.DisplayManager;
import android.os.BatteryManager;
import android.view.MotionEvent;
import android.view.SurfaceView;
import android.content.ActivityNotFoundException;
import android.content.ClipData;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.SystemClock;
import android.os.Environment;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.provider.Settings;
import android.util.Log;
import android.view.Display;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;

import dev.encounter.aurora.AuroraSurface;
import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLSurface;

import java.io.File;
import java.util.ArrayList;
import java.util.List;

public class DuskActivity extends SDLActivity {
    private static final String TAG = "DuskActivity";
    private static final float DEFAULT_SURFACE_FRAME_RATE = 60.0f;
    private static final int FOLDER_DIALOG_REQUEST_CODE = 0x4455;
    private static final int MANAGE_STORAGE_REQUEST_CODE = 0x4456;
    private static final String EXTERNAL_STORAGE_AUTHORITY =
        "com.android.externalstorage.documents";
    /**
     * Companion render buffer for a bottom panel: 8:7 at 1080p, the AYN Thor's
     * native landscape size. Used directly by the Presentation, and as the
     * fallback in DuskCompanionActivity when a panel will not report its size.
     */
    static final int COMPANION_PANEL_W = 1240;
    static final int COMPANION_PANEL_H = 1080;

    private long folderDialogUserdata = 0;
    private boolean awaitingManageStoragePermission = false;

    private static native void nativeFolderDialogResult(long userdata, String path, String error);
    private static native void nativeAuxSurfaceChanged(Surface surface, int width, int height);
    private static native void nativeCompanionTouchEvent(int action, float u, float v);
    private static native void nativeCompanionPinch(float factor);
    private static native void nativeDualScreenAvailable(boolean available);
    private static native void nativeBatteryStatus(int percent, boolean charging);
    private static native void nativeCompanionScreenshot(String path);

    // The live game activity, for the companion activity and the native swap
    // publisher to reach. Volatile: written on the UI thread, read from the
    // game thread's JNI callbacks.
    static volatile DuskActivity instance;

    private DisplayManager auxDisplayManager;
    private DisplayManager.DisplayListener auxDisplayListener;
    private BroadcastReceiver auxBatteryReceiver;
    private BroadcastReceiver screenshotReceiver;
    private AuxPresentation auxPresentation;
    // When startActivity() for the companion was issued, or 0 when none is
    // outstanding. A timestamp rather than a flag so a launch that never lands
    // can expire — see companionLaunchPending(). Written on the UI thread, read
    // there too: the companion's onCreate runs on the same main looper, as both
    // live in this process.
    private long companionLaunchPendingAt;
    private String lastDisplayScan;
    // Display the companion last presented on; preferred on every re-pick so an
    // attached TV cannot steal it across a pause/resume. -1 = none yet.
    private int lastAuxDisplayId = -1;

    private static String[] splitArgs(String raw) {
        List<String> out = new ArrayList<>();
        StringBuilder current = new StringBuilder();
        boolean inSingle = false;
        boolean inDouble = false;
        boolean escaped = false;

        for (int i = 0; i < raw.length(); ++i) {
            char c = raw.charAt(i);
            if (escaped) {
                current.append(c);
                escaped = false;
                continue;
            }
            if (c == '\\' && !inSingle) {
                escaped = true;
                continue;
            }
            if (c == '"' && !inSingle) {
                inDouble = !inDouble;
                continue;
            }
            if (c == '\'' && !inDouble) {
                inSingle = !inSingle;
                continue;
            }
            if (!inSingle && !inDouble && Character.isWhitespace(c)) {
                if (current.length() > 0) {
                    out.add(current.toString());
                    current.setLength(0);
                }
                continue;
            }
            current.append(c);
        }

        if (escaped) {
            current.append('\\');
        }
        if (current.length() > 0) {
            out.add(current.toString());
        }
        return out.toArray(new String[0]);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        instance = this;
        // Never let the device sleep mid-game: an idle screen-off pauses the
        // app and tears down both surfaces, which is our main crash source.
        getWindow().addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        hideSystemBars();
        installStickyImmersive();
        initAuxDisplay();
        initBatteryMonitor();
        initScreenshotReceiver();
    }

    // Verification aid: dump what the companion screen is presenting.
    //   adb shell am broadcast -a dev.twilitrealm.dusk.DUMP
    //   adb shell am broadcast -a dev.twilitrealm.dusk.DUMP --es name fr-map.png
    //   adb pull <externalFilesDir>/companion-screenshot.png
    // The path comes from Java because getExternalFilesDir() is the only
    // location adb can reach on an unrooted device.
    //
    // DEBUG BUILDS ONLY. The receiver has to be exported for `adb` to reach it,
    // which means any installed app could trigger it — and the extra below is a
    // filename that reaches a plain fopen() in native code. It is a development
    // tool, so it does not ship.
    private void initScreenshotReceiver() {
        // Debuggable APKs only. BuildConfig is not generated for this module,
        // and this is the more direct signal anyway: it asks whether THIS apk
        // was built debuggable rather than which gradle variant produced it.
        if ((getApplicationInfo().flags & ApplicationInfo.FLAG_DEBUGGABLE) == 0) {
            return;
        }
        screenshotReceiver = new BroadcastReceiver() {
            @Override
            public void onReceive(Context context, Intent intent) {
                java.io.File dir = getExternalFilesDir(null);
                if (dir == null) {
                    Log.w(TAG, "Screenshot: no external files dir");
                    return;
                }
                // Basename only. getAbsolutePath() does NOT normalise "..", so
                // an unchecked extra could walk out of the files dir and have
                // the native writer truncate an arbitrary file with PNG bytes.
                String name = intent.getStringExtra("name");
                if (name == null || name.isEmpty() || name.indexOf('/') >= 0
                    || name.indexOf('\\') >= 0 || name.contains("..")) {
                    name = "companion-screenshot.png";
                }
                String path = new java.io.File(dir, name).getAbsolutePath();
                try {
                    nativeCompanionScreenshot(path);
                    Log.i(TAG, "Screenshot requested -> " + path);
                } catch (UnsatisfiedLinkError e) {
                    Log.w(TAG, "nativeCompanionScreenshot missing", e);
                }
            }
        };
        IntentFilter filter = new IntentFilter("dev.twilitrealm.dusk.DUMP");
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            registerReceiver(screenshotReceiver, filter, Context.RECEIVER_EXPORTED);
        } else {
            registerReceiver(screenshotReceiver, filter);
        }
    }

    @Override
    protected void onPause() {
        // A Presentation must not outlive a paused activity (window leak
        // crash); dismissing here also detaches the aux surface in a
        // controlled order before Android kills it.
        //
        // The PRESENTATION only. The swapped companion is a separate activity
        // on another display, and on devices without multi-resume, bringing it
        // up is itself what pauses us — so finishing it here would tear down
        // the companion, resume, start it again, pause again, forever. It has
        // no window-leak problem to avoid either: that hazard is specific to a
        // Presentation attached to this activity's window.
        dismissAuxPresentation();
        super.onPause();
    }

    @Override
    protected void onStop() {
        // System overlays (OLED burn-in protection, screen savers) may
        // trigger onStop without onPause — dismiss defensively. The
        // Presentation is re-shown in onResume via showAuxPresentation.
        //
        // onStop, unlike onPause, means the user really has left: an activity
        // showing on another display is not stopped. So this is the right place
        // to take the companion activity down with us.
        dismissAux();
        super.onStop();
    }

    private void dismissAuxPresentation() {
        if (auxPresentation != null) {
            auxPresentation.dismiss();
            auxPresentation = null;
            // Keep native in sync immediately — the game thread may render a
            // few more frames before the pause fully lands, and it must not
            // spend them hiding the main HUD for a companion that is gone.
            reportDualScreenAvailable();
        }
    }

    /**
     * How long a companion launch may stay outstanding before we assume it is
     * never arriving and allow another attempt.
     *
     * Without an expiry, a launch the system accepts and then silently drops
     * leaves the pending flag set forever: showAuxPresentation() refuses to
     * retry, native has already been told there is no second screen, and the
     * player gets no companion for the rest of the session with nothing in the
     * log to say why. The Thor has already shown one firmware quirk in this
     * area, so this is not hypothetical.
     */
    private static final long COMPANION_LAUNCH_TIMEOUT_MS = 5000L;

    /** True while a companion launch is outstanding and still plausibly alive. */
    private boolean companionLaunchPending() {
        if (companionLaunchPendingAt == 0) {
            return false;
        }
        if (SystemClock.uptimeMillis() - companionLaunchPendingAt < COMPANION_LAUNCH_TIMEOUT_MS) {
            return true;
        }
        Log.w(TAG, "Companion launch never landed; allowing another attempt");
        companionLaunchPendingAt = 0;
        return false;
    }

    private void dismissAux() {
        dismissAuxPresentation();
        // Swapped, the companion is an activity in its own task, so Android
        // will not tear it down with ours — it has to be finished explicitly,
        // or it survives as a stray window on the main screen.
        companionLaunchPendingAt = 0;
        DuskCompanionActivity companion = DuskCompanionActivity.instance;
        if (companion != null) {
            DuskCompanionActivity.instance = null;
            companion.finish();
        }
        // One report, at the end: dismissAuxPresentation() already made one, and
        // this path's whole contract is that native agrees with reality when it
        // returns — easier to keep true with a single exit than two.
        reportDualScreenAvailable();
    }

    @Override
    protected void onDestroy() {
        // Static overlay state must not outlive us; see shutdown().
        DuskGuideBrowser.shutdown();
        if (auxBatteryReceiver != null) {
            unregisterReceiver(auxBatteryReceiver);
            auxBatteryReceiver = null;
        }
        if (screenshotReceiver != null) {
            unregisterReceiver(screenshotReceiver);
            screenshotReceiver = null;
        }
        if (auxDisplayManager != null && auxDisplayListener != null) {
            auxDisplayManager.unregisterDisplayListener(auxDisplayListener);
            auxDisplayListener = null;
        }
        dismissAux();
        if (instance == this) {
            instance = null;
        }
        super.onDestroy();
    }

    /**
     * Called from native whenever the Swap Screens setting changes, and once at
     * startup to reconcile. DuskLauncherActivity reads this on the next start to
     * decide which panel to open the game on — it runs before the native library
     * is loaded, so it cannot ask the config directly.
     *
     * Public and named for JNI: android_aux_display.cpp looks it up by name.
     */
    public void publishSwapPreference(boolean swap) {
        DuskLauncherActivity.setSwapPreference(this, swap);
    }

    // Dual-screen devices (e.g. AYN Thor) expose the second panel as a
    // presentation display. Mirror the dual-screen HUD/companion output there.
    // Battery level for the companion dashboard. ACTION_BATTERY_CHANGED is a
    // sticky broadcast, so registering delivers the current state immediately.
    private void initBatteryMonitor() {
        auxBatteryReceiver = new BroadcastReceiver() {
            @Override
            public void onReceive(Context context, Intent intent) {
                int level = intent.getIntExtra(BatteryManager.EXTRA_LEVEL, -1);
                int scale = intent.getIntExtra(BatteryManager.EXTRA_SCALE, 100);
                int status = intent.getIntExtra(BatteryManager.EXTRA_STATUS, -1);
                boolean charging = status == BatteryManager.BATTERY_STATUS_CHARGING ||
                    status == BatteryManager.BATTERY_STATUS_FULL;
                if (level >= 0 && scale > 0) {
                    nativeBatteryStatus(level * 100 / scale, charging);
                }
            }
        };
        registerReceiver(auxBatteryReceiver,
            new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
    }

    private void initAuxDisplay() {
        auxDisplayManager = (DisplayManager)getSystemService(Context.DISPLAY_SERVICE);
        if (auxDisplayManager == null) {
            return;
        }
        auxDisplayListener = new DisplayManager.DisplayListener() {
            @Override
            public void onDisplayAdded(int displayId) {
                showAuxPresentation();
            }

            @Override
            public void onDisplayRemoved(int displayId) {
                if (displayId == lastAuxDisplayId) {
                    // Forget it, or the preference would keep pointing at a
                    // panel that is gone and block a valid fallback.
                    lastAuxDisplayId = -1;
                }
                if (auxPresentation != null &&
                    auxPresentation.getDisplay().getDisplayId() == displayId)
                {
                    dismissAux();
                    // The bottom panel may be gone for good (dock mode) or
                    // just re-enumerating; re-pick so a surviving companion
                    // panel is used instead of dropping to single-screen.
                    //
                    // Safe while paused only because this whole branch is
                    // guarded by auxPresentation != null, and onPause already
                    // nulled it — so a paused activity can never be dragged
                    // into showing a Presentation (that is the window-leak
                    // crash onPause exists to avoid). Keep that guard.
                    showAuxPresentation();
                }
            }

            @Override
            public void onDisplayChanged(int displayId) {}
        };
        auxDisplayManager.registerDisplayListener(auxDisplayListener, null);
        showAuxPresentation();
    }

    // The display this activity is actually being shown on. NOT necessarily
    // display 0: on some dual-screen handhelds (AYANEO Pocket DS) the game can
    // be launched onto either panel, and the one it lands on may or may not be
    // the system default.
    @SuppressWarnings("deprecation")  // getDefaultDisplay(), API 26-29 fallback
    private int getActivityDisplayId() {
        Display display = null;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            display = getDisplay();
        }
        if (display == null && getWindowManager() != null) {
            display = getWindowManager().getDefaultDisplay();
        }
        return display != null ? display.getDisplayId() : Display.DEFAULT_DISPLAY;
    }

    // Pick a panel for the companion, never the one the game itself occupies.
    // DISPLAY_CATEGORY_PRESENTATION can include the activity's own display
    // when the game was launched onto a non-default panel; presenting there
    // covers the game window and leaves the game screen black.
    /** "1080x1920@60" — enough to tell a real panel from a virtual one. */
    private static String describeDisplay(Display d) {
        if (d == null) {
            return "?";
        }
        try {
            android.graphics.Point size = new android.graphics.Point();
            d.getRealSize(size);
            return size.x + "x" + size.y + "@" + Math.round(d.getRefreshRate());
        } catch (Throwable t) {
            return "?";
        }
    }

    private Display getActivityDisplay() {
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                return getDisplay();
            }
        } catch (Throwable t) {
            // fall through
        }
        return null;
    }

    /**
     * True when DuskLauncherActivity opened the game on a non-default panel
     * ("Swap Screens"), so the companion belongs on the main screen instead.
     *
     * Derived from where this window actually IS, not from the preference: if
     * the firmware refused the swapped launch, the launcher falls back to a
     * normal one, and the preference would then claim a swap that never
     * happened.
     */
    private boolean isSwappedLayout() {
        return getActivityDisplayId() != Display.DEFAULT_DISPLAY;
    }

    private Display pickAuxDisplay() {
        if (auxDisplayManager == null) {
            return null;
        }
        final int selfId = getActivityDisplayId();
        // DISPLAY_CATEGORY_PRESENTATION lists the displays a Presentation may
        // attach to, and the default display is never among them. Swapped, that
        // is exactly the display the companion has to go to — so ask for every
        // display instead, and host it in an activity (see showAuxPresentation).
        Display[] displays = isSwappedLayout()
            ? auxDisplayManager.getDisplays()
            : auxDisplayManager.getDisplays(DisplayManager.DISPLAY_CATEGORY_PRESENTATION);
        // Plugging in a TV/monitor adds a SECOND candidate, and dismissAux +
        // showAuxPresentation re-pick on every pause/resume — so "first match
        // wins" could migrate the dashboard onto the television mid-session,
        // leaving the built-in bottom panel showing the launcher.
        //   1. Stay on the panel we were already using, if it is still there.
        //   2. Otherwise take the LOWEST display id: built-in panels are
        //      enumerated before hot-plugged external ones.
        Display chosen = null;
        for (Display d : displays) {
            final int id = d.getDisplayId();
            if (id == selfId) {
                continue;
            }
            if (id == lastAuxDisplayId) {
                chosen = d;
                break;
            }
            if (chosen == null || id < chosen.getDisplayId()) {
                chosen = d;
            }
        }
        // One line listing every candidate — dual-screen handhelds differ
        // wildly in how they enumerate their panels, so a bug report needs it.
        // Size is logged per display because "the wrong screen was chosen" and
        // "a phantom display was chosen" look identical by id alone. A virtual
        // or overlay display shows up in DISPLAY_CATEGORY_PRESENTATION exactly
        // like the physical panel does, and taking the lowest id then sends the
        // companion somewhere nobody can see -- leaving the real bottom screen
        // showing the system's own app placeholder.
        StringBuilder sb = new StringBuilder();
        sb.append("Display scan: activity on id=").append(selfId)
          .append(" (").append(describeDisplay(getActivityDisplay())).append(")")
          .append(isSwappedLayout() ? " SWAPPED" : "")
          .append(", candidates=[");
        for (int i = 0; i < displays.length; i++) {
            if (i > 0) {
                sb.append(", ");
            }
            sb.append("id=").append(displays[i].getDisplayId())
              .append(" name=").append(displays[i].getName())
              .append(" flags=0x").append(Integer.toHexString(displays[i].getFlags()))
              .append(" ").append(describeDisplay(displays[i]));
        }
        sb.append("] -> chose ")
          .append(chosen != null ? Integer.toString(chosen.getDisplayId()) : "none");
        final String scan = sb.toString();
        if (!scan.equals(lastDisplayScan)) {
            lastDisplayScan = scan;
            Log.i(TAG, scan);
        }
        return chosen;
    }

    // Dual-screen only activates when a physical secondary display exists;
    // single-screen devices keep their HUD on the main screen.
    // Report what actually exists, never a second scan. Native hides the
    // main-screen HUD panes when this is true, so claiming a companion that
    // failed to appear leaves the HUD drawn nowhere at all.
    private void reportDualScreenAvailable() {
        try {
            // Either host counts: the Presentation on the second screen, or —
            // swapped — the companion activity on the main one.
            nativeDualScreenAvailable(
                auxPresentation != null || DuskCompanionActivity.instance != null);
        } catch (UnsatisfiedLinkError e) {
            Log.w(TAG, "nativeDualScreenAvailable missing", e);
        }
    }

    /**
     * The companion activity appears and disappears asynchronously — it is a
     * separate task on a separate display, so it does not exist yet when
     * startActivity() returns, and reporting availability there would always
     * report "none". It tells us instead, from its own onCreate/onDestroy.
     */
    static void onCompanionActivityChanged() {
        DuskActivity self = instance;
        if (self != null) {
            self.companionLaunchPendingAt = 0;
            self.reportDualScreenAvailable();
        }
    }

    // Always ends with reportDualScreenAvailable(): every exit path leaves
    // native agreeing with whether a Presentation is actually showing. An
    // early return that skipped the report could leave a stale "available"
    // from before a pause/dismiss — native then keeps the main-screen HUD
    // hidden for a companion that no longer exists.
    private void showAuxPresentation() {
        if (auxDisplayManager != null && auxPresentation == null &&
            DuskCompanionActivity.instance == null && !companionLaunchPending())
        {
            Display target = pickAuxDisplay();
            if (target != null && target.getDisplayId() == Display.DEFAULT_DISPLAY) {
                // Swapped. A Presentation cannot attach to the default display,
                // so the companion is hosted by an ordinary activity started
                // there instead — in its own task, since ours is pinned to the
                // panel the game is on.
                startCompanionActivity(target.getDisplayId());
                lastAuxDisplayId = target.getDisplayId();
            } else if (target != null) {
                try {
                    auxPresentation = new AuxPresentation(this, target);
                    auxPresentation.show();
                    lastAuxDisplayId = target.getDisplayId();
                    Log.i(TAG, "Aux presentation shown on display " + target.getDisplayId());
                } catch (Exception e) {
                    Log.w(TAG, "Failed to show aux presentation", e);
                    auxPresentation = null;
                }
            }
        }
        reportDualScreenAvailable();
    }

    private void startCompanionActivity(int displayId) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) {
            return;  // setLaunchDisplayId is API 26+
        }
        Intent intent = new Intent(this, DuskCompanionActivity.class);
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        ActivityOptions options = ActivityOptions.makeBasic();
        options.setLaunchDisplayId(displayId);
        try {
            startActivity(intent, options.toBundle());
            // instance is not set until the companion's own onCreate runs, so
            // without this the next showAuxPresentation() — onResume fires one
            // almost immediately — would see "no companion" and start a second.
            companionLaunchPendingAt = SystemClock.uptimeMillis();
            Log.i(TAG, "Companion activity started on display " + displayId);
        } catch (RuntimeException e) {
            // Same fallback as the launcher: no companion is survivable, a dead
            // game is not. reportDualScreenAvailable() (our caller) then tells
            // native there is no second screen, and the HUD stays on the game.
            Log.w(TAG, "Companion launch on display " + displayId + " refused", e);
        }
    }

    // The companion surface, hosted by the Presentation below — or, when the
    // game itself has been launched onto the second screen, by
    // DuskCompanionActivity on the main one.
    static SurfaceView createCompanionSurfaceView(Context context, int bufW, int bufH) {
        SurfaceView surfaceView = new SurfaceView(context);
        // The buffer's shape decides the companion's shape: computeAuxCanvas()
        // derives the dashboard's logical canvas from the surface aspect, and
        // the layout is width-parametric (side columns pinned to the edges, the
        // content window taking whatever is left). So this is where "the
        // companion is 8:7" is actually decided — pass the host panel's own
        // size and the dashboard lays itself out to fit it.
        surfaceView.getHolder().setFixedSize(bufW, bufH);
        surfaceView.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override
            public void surfaceCreated(SurfaceHolder holder) {}

            @Override
            public void surfaceChanged(SurfaceHolder holder, int format, int width,
                int height)
            {
                nativeAuxSurfaceChanged(holder.getSurface(), width, height);
            }

            @Override
            public void surfaceDestroyed(SurfaceHolder holder) {
                nativeAuxSurfaceChanged(null, 0, 0);
            }
        });
        // Pinch (two fingers) zooms the companion map; while a pinch is
        // active, the single-finger stream is cancelled so it can't
        // register taps or drags.
        final android.view.ScaleGestureDetector scaleDetector =
            new android.view.ScaleGestureDetector(context,
                new android.view.ScaleGestureDetector.SimpleOnScaleGestureListener() {
                    @Override
                    public boolean onScale(android.view.ScaleGestureDetector d) {
                        nativeCompanionPinch(d.getScaleFactor());
                        return true;
                    }
                });
        surfaceView.setOnTouchListener((view, event) -> {
            scaleDetector.onTouchEvent(event);
            if (event.getPointerCount() > 1 || scaleDetector.isInProgress()) {
                nativeCompanionTouchEvent(3, 0.0f, 0.0f);
                return true;
            }
            final int action = event.getActionMasked();
            if (view.getWidth() > 0 && view.getHeight() > 0 &&
                (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_MOVE ||
                 action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL))
            {
                // 0 = down, 1 = move, 2 = up (cancel maps to up).
                final int phase = action == MotionEvent.ACTION_DOWN ? 0
                    : action == MotionEvent.ACTION_MOVE ? 1 : 2;
                nativeCompanionTouchEvent(phase, event.getX() / view.getWidth(),
                    event.getY() / view.getHeight());
                return true;
            }
            return false;
        });
        return surfaceView;
    }

    private static final class AuxPresentation extends Presentation {
        AuxPresentation(Context context, Display display) {
            super(context, display);
        }

        @Override
        protected void onCreate(Bundle savedInstanceState) {
            super.onCreate(savedInstanceState);
            // The second screen gets no wake-resetting input during normal
            // play; keep it from sleeping (and tearing down our surface).
            getWindow().addFlags(
                android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            // Never take input focus: with the Thor's bottom-screen/auto
            // focus modes (especially after touching the companion), a
            // focusable Presentation captures the controller and the game
            // stops receiving it. Touches still arrive without focus.
            getWindow().addFlags(
                android.view.WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE);
            // The Presentation is a separate Window on the second display and
            // was never told to hide its bars, so the navigation bar sat on top
            // of the companion.
            applyImmersive(getWindow());
            setContentView(
                createCompanionSurfaceView(getContext(), COMPANION_PANEL_W, COMPANION_PANEL_H));
            // Take the whole panel, including the strip the navigation bar
            // would occupy. Hiding the bars stops them being DRAWN but the
            // content frame is still inset for them, so the companion was laid
            // out short by the bar's height and a transparent band sat where
            // the buttons would have been.
            //
            // Consumed here rather than on the Activity: SDL needs real insets
            // on the main window to place its own views, and this Presentation
            // has exactly one child that is meant to be edge-to-edge.
            final View auxDecor = getWindow().getDecorView();
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                auxDecor.setOnApplyWindowInsetsListener((v, insets) -> WindowInsets.CONSUMED);
            } else {
                auxDecor.setOnApplyWindowInsetsListener(
                    (v, insets) -> insets.consumeSystemWindowInsets());
            }
            auxDecor.requestApplyInsets();
        }
    }

    @Override
    protected SDLSurface createSDLSurface(Context context) {
        return new DuskSurface(context);
    }

    @Override
    protected void onResume() {
        super.onResume();
        hideSystemBars();
        showAuxPresentation();
        if (awaitingManageStoragePermission) {
            resumeFolderDialogAfterPermissionGrant();
        }
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            hideSystemBars();
            // Reshow the aux presentation after a focus-loss overlay
            // (e.g. OLED burn-in protection) disappears.
            showAuxPresentation();
        }
    }

    /**
     * Re-hides the bars a moment after anything brings them back.
     *
     * <p>SDLActivity already has this idea (rehideSystemUi +
     * onSystemUiVisibilityChange) but both halves are dead at targetSdk 30+:
     * setSystemUiVisibility is a no-op and the visibility callback no longer
     * fires. So nothing re-hid the bars once they appeared, and
     * hideSystemBars() only runs on create/resume/focus-change -- a transient
     * bar does not change focus. That is the gesture pill turning up mid-play.
     *
     * <p>Delayed by 2s, matching SDL's original, so it does not fight the
     * deliberate swipe-to-reveal.
     */
    private final Runnable reHideBars = () -> applyImmersive(getWindow());

    private void installStickyImmersive() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) {
            return;  // SDL's own path still works below R
        }
        final View decor = getWindow().getDecorView();
        decor.setOnApplyWindowInsetsListener((v, insets) -> {
            if (insets.isVisible(WindowInsets.Type.systemBars())) {
                v.removeCallbacks(reHideBars);
                v.postDelayed(reHideBars, 2000);
            }
            // Chain preserved: SDL lays its surface out from these insets.
            return v.onApplyWindowInsets(insets);
        });
    }

    private void hideSystemBars() {
        applyImmersive(getWindow());
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) {
            ActionBar actionBar = getActionBar();
            if (actionBar != null) {
                actionBar.hide();
            }
        }
    }

    /**
     * Hides the status and navigation bars on any window.
     *
     * <p>Static and Window-taking because there are three of them: this
     * Activity, the Presentation on the second screen, and the guide overlay's
     * restore path. The Presentation has its own Window and never hid its bars
     * at all, and the guide used only the pre-R {@code setSystemUiVisibility}
     * flags -- which do nothing at targetSdk 30+, so the gesture pill stayed up
     * after the overlay closed. Both routed here now.
     */
    static void applyImmersive(Window window) {
        if (window == null) {
            return;
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            window.setDecorFitsSystemWindows(false);
            WindowInsetsController ctrl = window.getDecorView().getWindowInsetsController();
            if (ctrl != null) {
                ctrl.setSystemBarsBehavior(
                    WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
                ctrl.hide(WindowInsets.Type.systemBars());
            }
        } else {
            View decorView = window.getDecorView();
            int uiOptions = View.SYSTEM_UI_FLAG_FULLSCREEN |
                View.SYSTEM_UI_FLAG_HIDE_NAVIGATION |
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY |
                View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN |
                View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION |
                View.SYSTEM_UI_FLAG_LAYOUT_STABLE;
            decorView.setSystemUiVisibility(uiOptions);
        }
    }

    @Override
    protected String[] getLibraries() {
        // SDL3 is statically linked into libmain.so in this build.
        return new String[] {
            "main"
        };
    }

    public void setPreferredSurfaceFrameRate(float frameRate) {
        runOnUiThread(() -> {
            if (mSurface instanceof DuskSurface) {
                ((DuskSurface)mSurface).setPreferredFrameRate(frameRate);
            }
        });
    }

    // Extends AuroraSurface, not SDLSurface directly: AuroraSurface overrides the
    // three surface callbacks to tell the native side when the surface is safe
    // to draw into (nativeSetSurfaceReady). That signalling used to be patched
    // straight into SDL's own SDLSurface.java, which meant carrying a modified
    // copy of an upstream SDL file plus a matching JNI export inside aurora.
    // Subclassing keeps SDL's Java stock and uses the hook aurora already ships.
    //
    // surfaceChanged below calls super FIRST, so AuroraSurface's ready/not-ready
    // bracketing runs before the frame-rate request, which needs mIsSurfaceReady
    // to already be settled.
    private static final class DuskSurface extends AuroraSurface {
        private float preferredFrameRate = DEFAULT_SURFACE_FRAME_RATE;

        DuskSurface(Context context) {
            super(context);
        }

        @Override
        public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
            super.surfaceChanged(holder, format, width, height);
            setTargetFrameRate(holder);
        }

        void setPreferredFrameRate(float frameRate) {
            preferredFrameRate = frameRate;
            setTargetFrameRate(getHolder());
        }

        private void setTargetFrameRate(SurfaceHolder holder) {
            if (!mIsSurfaceReady || Build.VERSION.SDK_INT < Build.VERSION_CODES.R) {
                return;
            }

            Surface surface = holder != null ? holder.getSurface() : getHolder().getSurface();
            if (surface == null || !surface.isValid()) {
                return;
            }

            float targetFrameRate = getMaxSupportedFrameRate();
            if (preferredFrameRate > 0.0f) {
                targetFrameRate = preferredFrameRate;
            }
            if (targetFrameRate <= 0.0f) {
                return;
            }

            try {
                surface.setFrameRate(
                    targetFrameRate, Surface.FRAME_RATE_COMPATIBILITY_DEFAULT);
                Log.v(TAG, "Requested surface frame rate " + targetFrameRate + " fps");
            } catch (RuntimeException e) {
                Log.w(TAG, "Failed to request surface frame rate", e);
            }
        }

        private float getMaxSupportedFrameRate() {
            if (mDisplay == null) {
                return 0.0f;
            }

            float maxFrameRate = mDisplay.getRefreshRate();
            Display.Mode[] modes = mDisplay.getSupportedModes();
            if (modes == null) {
                return maxFrameRate;
            }

            for (Display.Mode mode : modes) {
                maxFrameRate = Math.max(maxFrameRate, mode.getRefreshRate());
            }
            return maxFrameRate;
        }
    }

    @Override
    protected String[] getArguments() {
        Intent intent = getIntent();
        if (intent != null) {
            String[] argv = intent.getStringArrayExtra("dusk_argv");
            if (argv != null && argv.length > 0) {
                return argv;
            }

            String rawArgs = intent.getStringExtra("dusk_args");
            if (rawArgs != null) {
                String trimmed = rawArgs.trim();
                if (!trimmed.isEmpty()) {
                    return splitArgs(trimmed);
                }
            }
        }
        return new String[0];
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (resultCode == RESULT_OK) {
            persistUriPermissions(data);
        }
        if (requestCode == FOLDER_DIALOG_REQUEST_CODE) {
            finishFolderDialog(resultCode, data);
            return;
        }
        super.onActivityResult(requestCode, resultCode, data);
    }

    public boolean showFolderDialog(long userdata) {
        if (userdata == 0 || folderDialogUserdata != 0) {
            return false;
        }

        folderDialogUserdata = userdata;
        if (requiresManageStoragePermission() && !hasManageStoragePermission()) {
            if (!requestManageStoragePermission()) {
                finishFolderDialogWithError("Unable to request Android file access permission");
                return false;
            }
            return true;
        }

        openFolderDialog();
        return true;
    }

    private void openFolderDialog() {
        runOnUiThread(() -> {
            Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
            intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION |
                Intent.FLAG_GRANT_WRITE_URI_PERMISSION |
                Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION |
                Intent.FLAG_GRANT_PREFIX_URI_PERMISSION);

            try {
                startActivityForResult(intent, FOLDER_DIALOG_REQUEST_CODE);
            } catch (ActivityNotFoundException e) {
                Log.w(TAG, "Unable to open folder dialog.", e);
                finishFolderDialog(Activity.RESULT_CANCELED, null);
            }
        });
    }

    private boolean requiresManageStoragePermission() {
        return Build.VERSION.SDK_INT >= Build.VERSION_CODES.R;
    }

    private boolean hasManageStoragePermission() {
        return !requiresManageStoragePermission() || Environment.isExternalStorageManager();
    }

    private boolean requestManageStoragePermission() {
        if (!requiresManageStoragePermission()) {
            return true;
        }

        awaitingManageStoragePermission = true;
        runOnUiThread(() -> {
            if (tryStartManageStorageIntent(
                    new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION)
                        .setData(Uri.parse("package:" + getPackageName()))) ||
                tryStartManageStorageIntent(
                    new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION)))
            {
                return;
            }

            finishFolderDialogWithError("Unable to request Android file access permission");
        });
        return true;
    }

    private boolean tryStartManageStorageIntent(Intent intent) {
        try {
            startActivityForResult(intent, MANAGE_STORAGE_REQUEST_CODE);
            return true;
        } catch (ActivityNotFoundException e) {
            Log.w(TAG, "Unable to open all-files access settings.", e);
            return false;
        }
    }

    private void resumeFolderDialogAfterPermissionGrant() {
        awaitingManageStoragePermission = false;
        if (folderDialogUserdata == 0) {
            return;
        }

        if (hasManageStoragePermission()) {
            openFolderDialog();
            return;
        }

        finishFolderDialogWithError(
            "Allow \"All files access\" for Dusklight before choosing a custom data folder");
    }

    private void finishFolderDialogWithError(String error) {
        long userdata = folderDialogUserdata;
        folderDialogUserdata = 0;
        awaitingManageStoragePermission = false;
        if (userdata != 0) {
            nativeFolderDialogResult(userdata, null, error);
        }
    }

    private void finishFolderDialog(int resultCode, Intent data) {
        long userdata = folderDialogUserdata;
        folderDialogUserdata = 0;
        if (userdata == 0) {
            return;
        }

        if (resultCode == RESULT_OK && data != null && data.getData() != null) {
            String path = getRealPathForUri(data.getData());
            if (path != null && !path.isEmpty()) {
                nativeFolderDialogResult(userdata, path, null);
            } else {
                nativeFolderDialogResult(
                    userdata, null, "Selected folder is not available as a filesystem path");
            }
            return;
        }

        nativeFolderDialogResult(userdata, null, null);
    }

    private String getRealPathForUri(Uri uri) {
        if (uri == null) {
            return null;
        }

        String scheme = uri.getScheme();
        if ("file".equals(scheme)) {
            return uri.getPath();
        }

        if (!"content".equals(scheme) ||
            !EXTERNAL_STORAGE_AUTHORITY.equals(uri.getAuthority()) ||
            Build.VERSION.SDK_INT < Build.VERSION_CODES.KITKAT)
        {
            return null;
        }

        try {
            return getExternalStoragePathForDocumentId(getExternalStorageDocumentId(uri));
        } catch (IllegalArgumentException e) {
            Log.w(TAG, "Unable to resolve URI: " + uri, e);
            return null;
        }
    }

    private static String getExternalStorageDocumentId(Uri uri) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP && isTreeDocumentUri(uri)) {
            return DocumentsContract.getTreeDocumentId(uri);
        }

        return DocumentsContract.getDocumentId(uri);
    }

    private static boolean isTreeDocumentUri(Uri uri) {
        List<String> segments = uri.getPathSegments();
        return segments.size() >= 2 && "tree".equals(segments.get(0));
    }

    private String getExternalStoragePathForDocumentId(String documentId) {
        if (documentId == null || documentId.isEmpty()) {
            return null;
        }
        if (documentId.startsWith("raw:")) {
            return documentId.substring("raw:".length());
        }

        String[] parts = documentId.split(":", 2);
        String volumeId = parts[0];
        String relativePath = parts.length > 1 ? parts[1] : "";

        File root = getExternalStorageRoot(volumeId);
        if (root == null) {
            return null;
        }

        return relativePath.isEmpty()
            ? root.getAbsolutePath()
            : new File(root, relativePath).getAbsolutePath();
    }

    private File getExternalStorageRoot(String volumeId) {
        if ("primary".equalsIgnoreCase(volumeId)) {
            return Environment.getExternalStorageDirectory();
        }
        if ("home".equalsIgnoreCase(volumeId)) {
            return new File(
                Environment.getExternalStorageDirectory(), Environment.DIRECTORY_DOCUMENTS);
        }

        File[] externalFilesDirs = getExternalFilesDirs(null);
        if (externalFilesDirs != null) {
            for (File externalFilesDir : externalFilesDirs) {
                File root = getStorageRootForExternalFilesDir(externalFilesDir);
                if (root != null && volumeId.equalsIgnoreCase(root.getName())) {
                    return root;
                }
            }
        }

        File fallback = new File("/storage", volumeId);
        return fallback.exists() ? fallback : null;
    }

    private File getStorageRootForExternalFilesDir(File externalFilesDir) {
        if (externalFilesDir == null) {
            return null;
        }

        String path = externalFilesDir.getAbsolutePath();
        int androidDir = path.indexOf("/Android/");
        if (androidDir <= 0) {
            return null;
        }

        return new File(path.substring(0, androidDir));
    }

    private void persistUriPermissions(Intent data) {
        if (data == null) {
            return;
        }

        int permissionFlags =
            data.getFlags() & (Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        if (permissionFlags == 0) {
            return;
        }

        Uri uri = data.getData();
        if (uri != null) {
            persistUriPermission(uri, permissionFlags);
        }

        ClipData clipData = data.getClipData();
        if (clipData == null) {
            return;
        }
        for (int i = 0; i < clipData.getItemCount(); ++i) {
            Uri itemUri = clipData.getItemAt(i).getUri();
            if (itemUri != null) {
                persistUriPermission(itemUri, permissionFlags);
            }
        }
    }

    private void persistUriPermission(Uri uri, int permissionFlags) {
        if ((permissionFlags & Intent.FLAG_GRANT_READ_URI_PERMISSION) != 0) {
            persistUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION, "read");
        }
        if ((permissionFlags & Intent.FLAG_GRANT_WRITE_URI_PERMISSION) != 0) {
            persistUriPermission(uri, Intent.FLAG_GRANT_WRITE_URI_PERMISSION, "write");
        }
    }

    private void persistUriPermission(Uri uri, int permissionFlag, String permissionName) {
        try {
            getContentResolver().takePersistableUriPermission(uri, permissionFlag);
        } catch (SecurityException | IllegalArgumentException e) {
            Log.w(TAG, "Unable to persist " + permissionName + " URI permission for " + uri, e);
        }
    }

    public String getDisplayNameForUri(String uriString) {
        if (uriString == null || uriString.isEmpty()) {
            return "";
        }

        Uri uri = Uri.parse(uriString);
        if ("content".equals(uri.getScheme())) {
            try (Cursor cursor = getContentResolver().query(
                uri, new String[] { OpenableColumns.DISPLAY_NAME }, null, null, null))
            {
                if (cursor != null && cursor.moveToFirst()) {
                    int displayNameColumn = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                    if (displayNameColumn >= 0) {
                        String displayName = cursor.getString(displayNameColumn);
                        if (displayName != null && !displayName.isEmpty()) {
                            return displayName;
                        }
                    }
                }
            } catch (SecurityException | IllegalArgumentException e) {
                Log.w(TAG, "Unable to query display name for " + uri, e);
            }
        } else if ("file".equals(uri.getScheme())) {
            String path = uri.getPath();
            if (path != null && !path.isEmpty()) {
                String name = new File(path).getName();
                if (!name.isEmpty()) {
                    return name;
                }
            }
        }

        String lastSegment = uri.getLastPathSegment();
        return lastSegment != null ? lastSegment : "";
    }
}
