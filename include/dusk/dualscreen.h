#pragma once

namespace dusk::dualscreen {

// Renders the companion dashboard (HUD, map, inventory, ...) into an offscreen
// texture for the second screen when game.dualScreen is enabled. Call from
// mDoGph_Painter, bracketing the 2D draw-list flush; the dashboard is drawn
// during endHudCapture, after the main screen's own 2D pass.
void beginHudCapture();
void endHudCapture();

// Report whether a physical second display exists (Android reports this
// from its DisplayManager; defaults to available on desktop). When
// unavailable, the dual-screen setting is inert and the HUD stays on the
// main screen.
void setDisplayAvailable(bool available);

// True when the HUD actually lives on the second screen this frame: the
// setting is on AND a physical second display exists. Game-side gates must
// use this (never the raw setting) so single-screen devices keep their HUD.
bool hudOnCompanion();

}  // namespace dusk::dualscreen
