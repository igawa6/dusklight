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
