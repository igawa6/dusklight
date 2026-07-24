#pragma once

namespace dusk::companion {

// Handle page-cycling input. Call once per frame on the game thread while the
// dual-screen capture is active (an ImGui frame must be live).
void update();
bool hudReady();
void setNativeCanvas(unsigned width, unsigned height, float scale);
void applyNativeViewport();

// Draw the integrated second-screen dashboard (status strips + current page)
// into the active capture pass. Canvas coordinates are pixels.
void drawDashboard(float width, float height);

// Boot/loading splash: companion backdrop with the hero icon centered.
// Safe before the game launches (needs no archives or fonts).
void drawSplash(float width, float height);

// Advance to the next page. Thread-safe.
void nextPage();
// action: 0 = down, 1 = move, 2 = up, 3 = cancel (pinch took over).
void touchEvent(int action, float u, float v);

// Pinch gesture on the second screen (MAP page zoom). Thread-safe.
void pinchZoom(float factor);

// True once per tap on the companion's wolf/human transform button; the
// game frame loop consumes it and runs the quick transform there (starting
// a player proc from the draw pass is unsafe).
bool consumeTransformRequest();

// True once per tap on the MAP page's Warp context tab; the game frame loop
// consumes it and opens the field map in portal-warp mode there (menu
// status writes belong on the game thread, not the painter pass).
bool consumeWarpRequest();

// True once per tap on the Functional layout's Z button. Consumed by
// mDoCPd_c::read(), which ORs a real Z into pad 0 for that frame — injecting
// at the pad rather than at Link's item masks means every consumer (talking
// to Midna, the camera, menu Z actions) sees an ordinary Z press.
bool consumeZPress();

// Pad buttons the companion's touch layer currently HOLDS down (PAD_BUTTON_X
// or _Y) for the Functional X/Y item buttons. Read by mDoCPd_c::read() every
// frame; the pad computes the trigger edges itself. Held (not one-shot)
// because bow-class items aim on hold and fire on release, so the mask
// follows the finger.
unsigned padHoldMask();

// Slot buttons I/II — first-class item buttons 2/3. Hold/trigger bits
// (bit 0 = slot I, bit 1 = slot II) latched once per game frame by
// beginFrameCompanionInput from the touch layer and the "Use Slot" action
// binds; daAlink_c::setStickData ORs them into Link's item masks later the
// same frame.
unsigned slotTriggerBits();
unsigned slotHoldBits();

// While the game's own map screen is up, the companion's warp button acts as
// its Z button (portal-warp mode on/off). The tap is latched for exactly one
// game frame: beginFrameCompanionInput() promotes it at the top of the frame
// loop, and dMenu_Fmap_c reads it alongside dMw_Z_TRIGGER() later that same
// frame. Non-consuming, so both of the menu's Z call sites see one press.
void beginFrameCompanionInput();
bool warpTogglePressed();

// Sound + haptic feedback for companion-screen interactions. The touch
// handlers run inside the painter pass, where nothing else in the codebase
// starts a sound, so they only queue Z2SE_* ids here; the game frame loop
// drains the queue (f_ap_game) and fires them from the same phase as every
// other seStart. Both ends run on the game thread. Overflow drops extra cues.
void flushQueuedSounds();

// View adjustment for the live minimap render while the MAP page is
// panned or zoomed out: world-space center offset (cm) plus a cm-per-texel
// multiplier (> 1 widens the render window = zoom out). Returns true only
// when the dual-screen HUD is active and the view has been moved; the map
// renderer (dMap_c::_draw) applies it to the render window.
bool mapViewAdjust(float* o_x, float* o_z, float* o_texelScale);

// Drive the companion's live dungeon-map render (game thread, once per
// frame from the meter draw pass). Self-gated: no-op outside dungeons or
// when the dual-screen HUD is off.
void dmapUpdate();

// Report the device battery level (0-100; negative = unknown/no battery).
// Thread-safe (called from the Android UI thread on battery broadcasts).
void setBatteryStatus(int percent, bool charging);

}  // namespace dusk::companion
