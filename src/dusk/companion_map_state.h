#pragma once

// Phase-4 addition to the phone-companion state-streaming path: the dungeon
// floor map, decomposed by update frequency instead of streamed as pixels.
//
// The decomposition the design plan called for, and what research into the
// existing renderer actually found:
//
//   * base image  — the room/door texture. The companion's own renderer
//     (renderingDmap_c) derives from renderingPlusDoor_c, NOT
//     renderingPlusDoorAndCursor_c, and its getPlayerCursorSize() returns 0,
//     so the player cursor is architecturally never drawn into it. Icons
//     aren't either; they're a separate 2D pass. The seam already exists.
//   * icons       — a list of (kind, position), changing only when an icon's
//     status does.
//   * player      — position and heading, changing every frame, but three
//     numbers.
//
// TWO findings reshaped this from the original plan, both confirmed by
// reading the renderer rather than assumed:
//
// 1. dmapTimg() is NOT a byte source. On PC its image pointer is a 32-byte
//    stub (d_menu_dmap_map.cpp's setTexture) and the real pixels are a
//    GPU-side GX copy-texture; aurora exposes no generic texture readback.
//    So the base image has to go through the same draw-and-capture
//    substitution the item/heart icons already use.
//
// 2. The dashboard's map view is NOT static per floor — it is a following
//    minimap. dmapUpdate() glides both the pan offset and the zoom toward
//    the player's room every frame (companion_dmap.cpp's s_dmapFollow
//    block), so the rendered texture's content changes continuously while
//    the player walks. Sending that as "the base layer" would resend it
//    every frame, which is the exact cost this phase exists to remove.
//
// Hence the CANONICAL VIEW below: the phone's base image is rendered at a
// fixed whole-floor framing that depends only on the dungeon's own extents,
// never on where the player is standing. It genuinely changes only on floor
// change, stage change, or a room becoming newly visited. The phone then
// does its own following and zooming locally, against a cached image, at its
// own refresh rate and with no network traffic at all — which is both
// simpler to reason about and strictly better than gliding a streamed image.
//
// Positions are published NORMALIZED to that canonical image (u, v in 0..1,
// top-left origin) rather than as world centimetres, so the phone needs none
// of the world-to-texture math, the mirror-mode sign flip, or the logical-
// vs-boosted texture size distinction that trips up that conversion on the
// PC side.
//
// Gated by DUSK_PHONE_SPIKE_STATE (dualscreen.h) and off by default.

namespace dusk::companion {

// One map icon, already projected into canonical-image space.
struct MapIconState {
    f32 u = 0.0f;
    f32 v = 0.0f;
    f32 rotDeg = 0.0f;
    u8 kind = 0;  // ICON_*_e, d_menu_map_common.h

    bool operator==(const MapIconState&) const = default;
};

constexpr int kMaxMapIcons = 96;  // matches s_dmapIcons's own capacity

// Everything the phone needs to draw the map, minus the base image itself
// (which travels separately, as a captured PNG, because it has no CPU-side
// bytes — see the header comment).
struct MapState {
    // False whenever there is no dungeon map to show — not in a dungeon, the
    // companion isn't rendering the map page, the renderer was torn down, or
    // the dungeon's extents aren't known yet. The phone hides the whole map
    // layer rather than showing a stale one.
    bool active = false;

    s8 floor = 0;        // the floor being VIEWED (may differ from the player's)
    s8 playerFloor = 0;
    bool playerFloorKnown = false;

    // Player position/heading in canonical-image space. Sent every frame
    // these change, which is the whole point of the split.
    f32 playerU = 0.0f;
    f32 playerV = 0.0f;
    f32 playerHeadingDeg = 0.0f;

    // Bumps whenever the canonical base image's CONTENT changes, so the
    // phone knows to re-request it. Covers floor change, stage/renderer
    // teardown, and rooms becoming visited or un-visited.
    u32 baseGen = 0;

    int iconCount = 0;
    MapIconState icons[kMaxMapIcons];

    bool operator==(const MapState&) const = default;
};

// Fills `out` with the current map state. Pure read — no drawing, no
// mutation of any renderer or game state; safe to call every frame. Returns
// false (leaving `out` at its default, inactive value) when there is no
// dungeon map to report.
bool gatherMapState(MapState& out);

// The canonical view's world-space framing, for callers that need to project
// something themselves. `span` is the world-centimetre width AND height the
// canonical image covers (it is square). Returns false if the dungeon's
// extents aren't available yet.
bool canonicalMapView(f32& centerX, f32& centerZ, f32& span);

// The current base-image content generation — the same value gatherMapState()
// reports in MapState::baseGen. Exposed so the capture path can stamp the
// image it sends with the generation it actually corresponds to, using one
// definition rather than two that could drift.
u32 mapBaseGeneration();

// True while a dungeon map base image could actually be captured right now.
bool mapBaseAvailable();

// Turns the canonical whole-floor render override on or off (see
// s_dmapCanonicalView in companion_internal.h). Accessors rather than the
// variable itself so dualscreen.cpp doesn't need the companion module's
// internal header just to drive one flag. MUST be cleared on every path
// that ends a capture cycle, or the dashboard keeps rendering the
// whole-floor framing instead of following the player.
void setMapBaseCanonicalView(bool enabled);

// Draws the dungeon map's base image — room/door texture only, no icons and
// no player cursor — into the CURRENT 2D render context. Called from
// dualscreen.cpp's endHudCapture() in place of the dashboard for the few
// substituted frames of a base-image capture, exactly like
// drawWantedHeartIcon() does, because the pixels
// exist only on the GPU and the aux capture is the codebase's sole readback
// path.
//
// The image is square but the aux surface is the PHONE's aspect ratio, so it
// is letterboxed; `outFit*` receive the sub-rectangle it occupies,
// normalized 0..1 over the canvas, which the phone needs in order to place
// the normalized icon/player coordinates correctly inside the captured PNG.
//
// The caller must have set s_dmapCanonicalView at least one frame earlier,
// or this draws whatever follow-view framing the dashboard last rendered —
// the texture is produced by the game's own copy-2D pass, which runs before
// this point in the frame, so the override cannot take effect in the same
// frame it is set.
bool drawMapBaseImage(f32 canvasW, f32 canvasH, f32& outFitU0, f32& outFitV0, f32& outFitU1,
    f32& outFitV1);

}  // namespace dusk::companion
