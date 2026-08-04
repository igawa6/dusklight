package dev.twilitrealm.dusk;

import android.app.Activity;
import android.app.ActivityOptions;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.hardware.display.DisplayManager;
import android.os.Build;
import android.os.Bundle;
import android.util.Log;
import android.view.Display;

/**
 * The app's entry point, and nothing else: it decides which physical panel the
 * game's window opens on, starts {@link DuskActivity} there, and finishes.
 *
 * A window cannot be moved between displays after it is showing — the display
 * is fixed at launch — so "Swap Screens" can only be honoured by an activity
 * that has not started yet. An earlier attempt had DuskActivity relaunch
 * itself with FLAG_ACTIVITY_CLEAR_TASK; that restarts the task from its root
 * launcher intent, which threw away the extra saying "you are the swapped
 * launch" and looped forever. Splitting the decision into a separate activity
 * is what /root/zelda3-android does (SetupActivity), and it works on this
 * hardware.
 *
 * Invisible in use (translucent theme, finishes inside onCreate), so the only
 * thing the player sees is the game coming up on the screen they chose.
 */
public class DuskLauncherActivity extends Activity {
    private static final String TAG = "DuskLauncher";

    private static final String PREFS = "dusk_launch";
    private static final String KEY_SWAP = "swap_screens";

    /**
     * Mirror of the game.dualScreenSwap setting, written by native through
     * DuskActivity.publishSwapPreference. Native owns the value; this is only
     * somewhere the launcher can read it before the native library exists.
     */
    static void setSwapPreference(Context context, boolean swap) {
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .edit()
            .putBoolean(KEY_SWAP, swap)
            .apply();
    }

    static boolean getSwapPreference(Context context) {
        SharedPreferences prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        return prefs.getBoolean(KEY_SWAP, false);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        // The game is already running: get out of the way.
        //
        // Starting it again puts a SECOND task in front of the first, and
        // SDLActivity cannot be re-created in a live process — it exits, taking
        // the running game with it. On the AYN Thor this was not hypothetical:
        // the system shell re-launches the app by itself, so every re-launch
        // killed the session, and because the shell then launched it again the
        // app looked like it simply would not open.
        //
        // Doing nothing leaves the running game exactly where it is, which is
        // what bringing a running game to the front should do anyway.
        if (DuskActivity.instance != null) {
            Log.i(TAG, "Game already running; leaving it alone");
            finish();
            return;
        }

        Intent intent = new Intent(this, DuskActivity.class);
        // Command-line extras (dusk_args / dusk_argv) reach DuskActivity through
        // here now that it is no longer the launcher entry point.
        Bundle extras = getIntent() != null ? getIntent().getExtras() : null;
        if (extras != null) {
            intent.putExtras(extras);
        }

        final int display = swapDisplayId();
        if (display != -1) {
            ActivityOptions options = ActivityOptions.makeBasic();
            options.setLaunchDisplayId(display);
            try {
                startActivity(intent, options.toBundle());
                Log.i(TAG, "Game launched on display " + display + " (swapped)");
                finish();
                return;
            } catch (RuntimeException e) {
                // Some firmwares refuse app launches on a secondary display.
                // Coming up unswapped on the main screen is a far better
                // outcome than not coming up at all.
                Log.w(TAG, "Launch on display " + display + " refused", e);
            }
        }
        startActivity(intent);
        finish();
    }

    /**
     * The display the game should open on when swapped, or -1 to launch
     * normally — which covers both "swap is off" and "there is no second
     * screen to swap with".
     *
     * Deliberately DisplayManager.getDisplays() and not the PRESENTATION
     * category: that category exists to list displays a Presentation may
     * attach to, and never contains the default display. Here we are choosing
     * where an Activity opens, and the default display is a legitimate answer
     * for one of the two windows.
     */
    private int swapDisplayId() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) {
            return -1;  // setLaunchDisplayId did not exist before API 26
        }
        if (!getSwapPreference(this)) {
            return -1;
        }
        DisplayManager dm = (DisplayManager)getSystemService(Context.DISPLAY_SERVICE);
        if (dm == null) {
            return -1;
        }
        // Lowest non-default id, matching DuskActivity.pickAuxDisplay(): the
        // built-in panels are enumerated before hot-plugged external ones, so a
        // connected TV cannot claim the game while the bottom screen sits idle.
        int chosen = -1;
        for (Display d : dm.getDisplays()) {
            final int id = d.getDisplayId();
            if (id == Display.DEFAULT_DISPLAY) {
                continue;
            }
            if (chosen == -1 || id < chosen) {
                chosen = id;
            }
        }
        return chosen;
    }
}
