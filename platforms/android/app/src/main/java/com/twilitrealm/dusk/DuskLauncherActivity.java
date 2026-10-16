package dev.twilitrealm.dusk;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.os.Bundle;
import android.util.Log;

/**
 * The app's entry point: starts {@link DuskActivity} on the main screen and
 * finishes, and guards against a second launch killing a running game.
 *
 * It used to honour "Swap Screens" by opening the game's window on the other
 * display. Android refuses that for an untrusted display (a screen-casting
 * app's VirtualDisplay) and it needed a restart, so the swap is now done live
 * inside DuskActivity by trading surfaces between its window and the
 * Presentation. The swap preference stays here so the game can come up the
 * right way round before native has published it.
 *
 * Invisible in use (translucent theme, finishes inside onCreate).
 */
public class DuskLauncherActivity extends Activity {
    private static final String TAG = "DuskLauncher";

    private static final String PREFS = "dusk_launch";
    private static final String KEY_SWAP = "swap_screens";

    /**
     * Mirror of the game.dualScreenSwap setting, written by native through
     * DuskActivity.publishSwapPreference. Native owns the value; this is only
     * somewhere DuskActivity can read it before the native library exists.
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

        startActivity(intent);
        finish();
    }
}
