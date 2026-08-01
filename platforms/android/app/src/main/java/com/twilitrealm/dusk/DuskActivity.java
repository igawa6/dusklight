package dev.twilitrealm.dusk;

import android.app.ActionBar;
import android.app.Activity;
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

    private long folderDialogUserdata = 0;
    private boolean awaitingManageStoragePermission = false;

    private static native void nativeFolderDialogResult(long userdata, String path, String error);
    private static native void nativeAuxSurfaceChanged(Surface surface, int width, int height);
    private static native void nativeCompanionTouchEvent(int action, float u, float v);
    private static native void nativeCompanionPinch(float factor);
    private static native void nativeDualScreenAvailable(boolean available);
    private static native void nativeBatteryStatus(int percent, boolean charging);
    private static native void nativeCompanionScreenshot(String path);

    private DisplayManager auxDisplayManager;
    private DisplayManager.DisplayListener auxDisplayListener;
    private BroadcastReceiver auxBatteryReceiver;
    private BroadcastReceiver screenshotReceiver;
    private AuxPresentation auxPresentation;
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
        dismissAux();
        super.onPause();
    }

    @Override
    protected void onStop() {
        // System overlays (OLED burn-in protection, screen savers) may
        // trigger onStop without onPause — dismiss defensively. The
        // Presentation is re-shown in onResume via showAuxPresentation.
        dismissAux();
        super.onStop();
    }

    private void dismissAux() {
        if (auxPresentation != null) {
            auxPresentation.dismiss();
            auxPresentation = null;
            // Keep native in sync immediately — the game thread may render a
            // few more frames before the pause fully lands, and it must not
            // spend them hiding the main HUD for a companion that is gone.
            reportDualScreenAvailable();
        }
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
        super.onDestroy();
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
    private Display pickAuxDisplay() {
        if (auxDisplayManager == null) {
            return null;
        }
        final int selfId = getActivityDisplayId();
        Display[] displays =
            auxDisplayManager.getDisplays(DisplayManager.DISPLAY_CATEGORY_PRESENTATION);
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
        StringBuilder sb = new StringBuilder();
        sb.append("Display scan: activity on id=").append(selfId)
          .append(", presentation candidates=[");
        for (int i = 0; i < displays.length; i++) {
            if (i > 0) {
                sb.append(", ");
            }
            sb.append("id=").append(displays[i].getDisplayId())
              .append(" name=").append(displays[i].getName())
              .append(" flags=0x").append(Integer.toHexString(displays[i].getFlags()));
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
            nativeDualScreenAvailable(auxPresentation != null);
        } catch (UnsatisfiedLinkError e) {
            Log.w(TAG, "nativeDualScreenAvailable missing", e);
        }
    }

    // Always ends with reportDualScreenAvailable(): every exit path leaves
    // native agreeing with whether a Presentation is actually showing. An
    // early return that skipped the report could leave a stale "available"
    // from before a pause/dismiss — native then keeps the main-screen HUD
    // hidden for a companion that no longer exists.
    private void showAuxPresentation() {
        if (auxDisplayManager != null && auxPresentation == null) {
            Display target = pickAuxDisplay();
            if (target != null) {
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

    // The companion surface, hosted by the Presentation below.
    private static SurfaceView createCompanionSurfaceView(Context context) {
        SurfaceView surfaceView = new SurfaceView(context);
        // Render the bottom screen at 8:7 in 1080p (native landscape).
        surfaceView.getHolder().setFixedSize(1240, 1080);
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
            // was never told to hide its bars, so the gesture pill sat on top
            // of the companion. FLAG_NOT_FOCUSABLE above does not affect this.
            applyImmersive(getWindow());
            setContentView(createCompanionSurfaceView(getContext()));
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

    private static final class DuskSurface extends SDLSurface {
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
