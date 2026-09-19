# Dual-Screen Fork Notes

This fork (igawa6/dusklight, branch `dual-screen-v2`) adds a second-screen
companion display: a separate OS window on desktop, and the bottom panel on
dual-screen Android handhelds (AYN Thor). This file documents how the feature
is wired and how to keep the fork in sync with upstream.

## Architecture in short

- `game.dualScreen` CVar gates everything (Settings → Gameplay).
- `src/dusk/dualscreen.cpp` renders the companion dashboard into an offscreen
  pass each frame (hooked into `mDoGph_Painter` right after the 2D flush) and
  hands the resolved texture to `aurora::auxwin`.
- `src/dusk/companion.cpp` draws the dashboard: left status column, hearts row
  (original heart panes), TPHD-style controller cluster with the game's live
  action-label texture, and Map/Items/Quest/Collect pages with touch tabs.
- `extern/aurora` (fork igawa6/aurora, branch `aux-window`) provides the
  auxiliary output: an SDL window + wgpu surface on desktop, or an
  ANativeWindow surface fed from DuskActivity's Presentation on Android.
- `src/dusk/android_aux_display.cpp` is the JNI bridge (surface lifecycle,
  bottom-screen touch).

## Upstream-file touchpoints (the only places merges can conflict)

| File | Edit | Why |
|---|---|---|
| `.gitmodules` | aurora URL → igawa6/aurora | fork carries `aux-window` branch |
| `extern/aurora` (gitlink) | pinned to `aux-window` head | see aurora bump below |
| `src/m_Do/m_Do_graphic.cpp` | include + `beginHudCapture()`/`endHudCapture()` beside the `GFX_STAGE_FRAME_BEFORE/AFTER_HUD` hooks | frame hook for the dashboard pass |
| `src/d/d_meter2_draw.cpp` / `include/d/d_meter2_draw.h` | dual-screen pane-hide block in `draw()`; public getters (`getActionPicture`, `getHeartPictures`, `getLightDropAlpha`, `isButtonClusterVisible`) | hide status panes + controller cluster on main; expose live HUD assets to the companion |
| `src/d/d_meter2.cpp` | null `mpMeterDraw` at `_create` start; null the `dMeter2Info` meter global in `_delete` | the companion reads the meter global every frame; stale pointers crashed on stage transitions |
| `src/dusk/settings.h` (moved from `include/dusk/`), `src/dusk/settings.cpp`, `src/dusk/ui/settings.cpp` | `game.dualScreen` + placement cvars, UI toggle | additive |
| `files.cmake` | new source files | additive |
| `platforms/android/.../DuskActivity.java` | `extends BorealisActivity` (was `SDLActivity`, since upstream's Borealis migration); Presentation + SurfaceView + touch block, JNI natives, display selection, `DuskSurface`'s Android 13+ frame-rate hook (duplicates `BorealisSurface`'s ~20 lines rather than forking Borealis to expose it) | Android bottom-screen output |
| `platforms/android/.../AndroidManifest.xml` | `DuskLauncherActivity`/`DuskCompanionActivity` entries, `DuskActivity`'s `launchMode`/`preferMinimalPostProcessing` attributes, `USB_DEVICE_ATTACHED` intent-filter | supports the launcher/companion split and dual-screen launch selection |
| `platforms/android/.../DuskCompanionActivity.java` | "Swap Screens" companion activity (hosts the game's own output when swapped to the secondary panel); `extends plain Activity`, unaffected by the `BorealisActivity` reparenting | dual-screen swap-screens mode |
| `platforms/android/.../DuskLauncherActivity.java` | `MAIN`/`LAUNCHER` trampoline that picks which physical display to open `DuskActivity` on; `extends plain Activity` | dual-screen display selection at cold start |
| `extern/borealis` (gitlink) | pinned to upstream `encounter/borealis`, no fork patches needed (its Android piece already coexists cleanly with a second Presentation/SurfaceView) | required dependency since the Borealis migration; not itself a dual-screen touchpoint |
| `src/d/d_menu_fmap.cpp` / `include/d/d_menu_fmap.h` | four `dMw_Z_TRIGGER()` sites also accept `companion::warpTogglePressed()`; public `isWarpMapMode()` | companion warp button acts as the map screen's Z key |
| `src/d/d_meter2_info.cpp` / `include/d/d_meter2_info.h` | `getStringFull()` (full-fidelity text fetch), `msgTagOutfontIndex()` | companion reader needs tags and units the truncating `getString` drops |
| `include/d/d_msg_out_font.h` | `getBtiName()` made `static` | companion shares the icon table instead of copying it |
| `src/d/actor/d_a_alink.cpp` / `d_a_alink_dusk.cpp` / `d_a_alink_swindow.inc` / `include/d/actor/d_a_alink.h` | shield-reload pump in `execute()`, quick-transform helpers | companion gear equip and transform button |
| `src/d/d_menu_dmap.cpp` / `d_menu_fmap2D.cpp` / `d_menu_ring.cpp` / their headers | public getters, item-wheel centre offset | companion map + wheel |
| `src/d/d_map.cpp` / `d_map_path.cpp` / `include/d/d_map.h` | render-scale boost, view adjust | companion map detail |
| `src/d/d_meter_map.h` / `include/d/d_menu_window.h` / `d_menu_collect.h` | public accessors | companion reads live menu state |
| `src/f_ap/f_ap_game.cpp` | `duskExecute()` consumes companion requests (transform, warp, sound/haptic queue) | draw-pass work deferred to the game thread |
| `src/dusk/ui/overlay.cpp` | FPS counter corner anchoring | bug fix, additive |
| `src/dusk/ui/prelaunch.cpp` / `src/dusk/app_info.hpp` (moved from `include/dusk/`, now a `borealis::AppInfo` override) | update check points at the fork (`.githubOwner = "igawa6"`) | additive |
| `src/dusk/iso_validate.cpp` | multi-revision disc acceptance via `borealis::disc::AcceptedDisc{revision=N}` entries (was a hand-rolled multi-hash array); hash-mismatch diagnostic logs the near-matched `{gameId, revision}` since `borealis::disc::Result` doesn't expose the computed hash | supports multiple valid GZ2E01/GZ2P01 disc dumps + an actionable support-report diagnostic |
| `src/dusk/data.cpp` | `"guides"` added to the migration `.directories` list inside `manager()` | carries the guide-reader cache forward on a data-folder migration |
| `src/dusk/mods/svc/gfx.cpp` | `GfxDrawContext`'s `color_format`/`depth_format`/`sample_count`/`target_width`/`target_height` populated from `ctx.layout.*` (was flat `DrawContext` fields, moved into aurora's `RenderTargetLayout` refactor) | mod SDK ABI compat — these fields are deprecated but still populated for mods built against the older shape |
| `src/dusk/guide/fetch.cpp` | `borealis::http::start()` polled to completion from its own worker thread (was synchronous `dusk::http::get()`, module deleted upstream) | offline guide-reader image/page fetch |

Regenerate this list with:
`git diff --name-only $(git merge-base HEAD upstream/main) HEAD | grep -v '^src/dusk/companion'`

Everything else lives in new files and never conflicts: `src/dusk/dualscreen.cpp`,
the five `src/dusk/companion*.cpp` sources plus `companion_internal.h`,
`src/dusk/android_aux_display.cpp`, `include/dusk/dualscreen.h`,
`include/dusk/companion.h`, and in aurora `lib/aux_window.{cpp,hpp}` +
`include/aurora/aux_window.hpp`.

## Routine upstream sync

Run `scripts/sync-upstream.sh`. It fetches upstream, merges `upstream/main`,
rebuilds, and runs the headless smoke test. Conflicts, if any, will be in the
table above.

## Aurora bump procedure

When upstream changes its `extern/aurora` pointer (the sync script warns):

```bash
cd extern/aurora
git fetch upstream                    # encounter/aurora
git rebase <new-upstream-sha> aux-window
# resolve conflicts (only lib/aux_window.*, lib/aurora.cpp hooks,
# lib/window.cpp filter_event, cmake/aurora_core.cmake source list)
git push -f origin aux-window
cd ../..
git add extern/aurora && git commit -m "Bump aurora to upstream <sha> + aux-window"
```

## Verification

Desktop smoke test (needs Xvfb + llvmpipe + a TP USA .rvz):

```bash
Xvfb :99 -screen 0 2880x1080x24 &
cd build/linux-default-relwithdebinfo
DISPLAY=:99 VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json \
SDL_AUDIO_DRIVER=dummy ./dusklight --dvd <game.rvz> --backend vulkan \
  --cvar game.dualScreen=true --cvar game.dualScreenPosX=1600 \
  --cvar game.dualScreenPosY=92 --stage F_SP103
# expect: process alive, aux window shows the dashboard
DISPLAY=:99 import -window root check.png
```

**Always run the dual-screen-OFF case too** — it is the single-screen
device path, and it must be checked on desktop because most phones have no
second display:

```bash
DISPLAY=:99 VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json \
SDL_AUDIO_DRIVER=dummy ./dusklight --dvd <game.rvz> --backend vulkan \
  --cvar game.dualScreen=false --stage F_SP103
# expect: game renders on the MAIN window with its normal HUD, no aux window
DISPLAY=:99 import -window root check_nods.png
```

A black main window here — with the log still reaching `Starting main01`
and the process alive — means something in the dual-screen path has taken
over a step the main screen depends on. That is exactly how the
`present_with_encoder` regression shipped: the main frame's `Finish`/
`Submit` had been moved inside the aux present, behind an early return
taken whenever no aux surface existed, so every single-screen Android
device drew nothing while running perfectly. Because the recipe above only
ever exercised `dualScreen=true`, which always has an aux surface, it went
unnoticed until users reported it.

General rule for this fork: **the companion is optional, so no main-screen
step may live inside a companion code path.** Any change touching
`extern/aurora/lib/aurora.cpp`'s `end_frame`, `aux_window.cpp`, or the
`beginHudCapture`/`endHudCapture` bracket needs both cases run.

Android build: stage the JNI libs, `gradlew :app:assembleRelease`, then
zipalign + apksigner with your own keystore.
