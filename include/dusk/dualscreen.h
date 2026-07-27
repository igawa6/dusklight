#pragma once

// Companion frame capture (the on-device screenshot aid). OFF by default: it
// is a development tool, and its Android trigger is a broadcast receiver that
// has to be exported to be reachable from `adb`, so it must not ship.
//
// Flip to 1 (here or with -DDUSK_COMPANION_CAPTURE=1) to build it in; that is
// the intended way to use it, since build-dusklight-apk.sh produces a RELEASE
// apk and there is no separate debug build of these files.
//
// NOTE: do NOT write `DEBUG` here. Every dusk translation unit compiles with
// NDEBUG=1 and no -DDEBUG, and include/global.h:31 then defines DEBUG as 0 —
// so `#define DUSK_COMPANION_CAPTURE DEBUG` was silently 0 in *every* build
// and compiled the whole feature away, including the JNI entry point the
// Android receiver calls (it failed with UnsatisfiedLinkError and a swallowed
// log line, which is exactly why it went unnoticed).
#ifndef DUSK_COMPANION_CAPTURE
#define DUSK_COMPANION_CAPTURE 0
#endif

namespace dusk::dualscreen {

// Renders the companion dashboard (HUD, map, inventory, ...) into an offscreen
// texture for the second screen when game.dualScreen is enabled. Call from
// mDoGph_Painter, bracketing the 2D draw-list flush; the dashboard is drawn
// during endHudCapture, after the main screen's own 2D pass.
void beginHudCapture();
void endHudCapture();

#if DUSK_COMPANION_CAPTURE
// Ask for the next presented companion frame to be written to `path` as a
// PNG (exactly what the panel shows: letterbox, dim and all). One-shot and
// asynchronous — the file appears a frame or two later. Verification aid:
// on Android it is wired to the DUMP broadcast, on desktop to F12.
// Passing an empty path uses the default location.
void requestScreenshot(const char* path);
#endif

// Report whether a physical second display exists (Android reports this
// from its DisplayManager; defaults to available on desktop). When
// unavailable, the dual-screen setting is inert and the HUD stays on the
// main screen.
void setDisplayAvailable(bool available);

// True when the HUD actually lives on the second screen this frame: the
// setting is on AND a physical second display exists. Game-side gates must
// use this (never the raw setting) so single-screen devices keep their HUD.
// Stays the gate for everything that is on the companion in BOTH modes
// (rupees/keys, minimap, X/Y buttons).
bool hudOnCompanion();

// True when the HUD is on the companion AND the mode moves the
// gameplay-critical subset back to the main screen (Functional): hearts,
// vessel of light, A/B/Z + Midna, d-pad and the special-action panels.
// (The lantern oil gauge and oxygen bar stay on the companion's status strip
// in both modes.) Those elements are then drawn ONLY on the main screen —
// the companion stops drawing them. Game-side gates must use this helper
// rather than reading game.dualScreenHudMode directly, for the same reason
// hudOnCompanion() exists.
bool mainHudRestored();

// True when the status cluster (hearts, A/B/Z, d-pad, contextual panels)
// renders on the MAIN screen this frame: dual-screen off/unavailable, or the
// Functional mode. Exactly `!hudOnCompanion() || mainHudRestored()` — use
// this instead of re-deriving that compound at each gate.
bool mainHudActive();

// Cinematic safety net: latched true while Link's health is low enough that
// the hearts pop back onto the MAIN screen (with hysteresis, so the boundary
// doesn't flicker). Always false in Functional / single-screen — hearts are
// already there.
bool lowLifePopIn();

// Scene-change hook (called from fopScnM_ChangeReq): any change to a
// non-play scene (quit to title, game over to menu, file select) sends the
// companion back to the boot splash once the HUD meter dies; play-to-play
// stage transitions keep the last dashboard frame instead.
void onSceneChangeReq(short procName);

// True while the most recently requested scene is NOT the play scene — quit
// to title, game over, file select. NOTE: a location change requests
// PLAY_SCENE again (d_s_play.cpp), so this stays FALSE throughout one; the
// dim's load latch keys on fopOvlpM_IsDoingReq for that.
bool leftGameplay();

}  // namespace dusk::dualscreen
