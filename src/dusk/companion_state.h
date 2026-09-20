#pragma once

// Phase-1 addition to the phone-companion-display spike (see
// docs/phone-companion-design.md): a pure, read-only snapshot of the small
// HUD "chrome" values (hearts, rupees, keys, equipped items) independent of
// drawing — the companion dashboard's existing draw code
// (companion_hud.cpp/companion_functional.cpp) re-reads these same
// underlying accessors every frame with no such struct to reuse; this
// centralizes them for the new state-streaming path
// (dualscreen.cpp's pollAndPushSpikeState()) without touching any draw
// call. Precedented by discord_presence.cpp's existing "read state without
// drawing" pattern, generalized.
//
// Gated by DUSK_PHONE_SPIKE_STATE (dualscreen.h) and off by default.

namespace dusk::companion {

struct HudState {
    u16 life = 0;
    u16 maxLife = 0;
    u16 rupees = 0;
    u16 maxRupees = 0;
    u8 keys = 0;
    // dItemNo_NONE_e (0xFF) means the slot is empty — callers of
    // gatherHudState() never need to know that sentinel value themselves,
    // it's already dItemNo_NONE_e's actual value (d_item_data.h), kept as
    // raw u8 here rather than translated to avoid a dependency on that
    // header from this small file.
    u8 equipX = 0xFF;
    u8 equipY = 0xFF;
    u8 equipSlot1 = 0xFF;
    u8 equipSlot2 = 0xFF;

    // Lantern oil and underwater oxygen, as PERCENT (0..100) rather than
    // the raw values, and deliberately so.
    //
    // Both drain one unit per frame while active — oxygen across 0..600
    // (ten seconds) and oil across 0..21600 (six minutes) — so putting the
    // raw counters in this struct would make the whole-struct diff in
    // pollAndPushSpikeState() fire EVERY frame for the entire time the
    // player is underwater or carrying a lit lantern. That would quietly
    // invalidate the premise its "send directly on the game thread" comment
    // rests on (tiny messages, only on actual change, rare relative to the
    // frame rate) and turn a rare-diff path into a per-frame one. This is
    // the same trap that got dim/fade deferred out of Phase 1 in the design
    // plan for exactly this reason.
    //
    // A percent is also all the phone can use: these render as a bar a
    // hundred-odd pixels wide, so sub-percent precision isn't visible. At
    // this granularity one step is 0.1s of oxygen or 3.6s of oil.
    //
    // Unlike hearts, neither needs any art fetched: the game draws both by
    // scaling a single flat-colored pane to the fraction
    // (dMeter2Draw_c::drawKantera/drawOxygen — a J2D pane resize(), no
    // per-step textures at all), and the companion dashboard already
    // reimplements both natively with fillRect in drawMeterBar()
    // (companion.cpp), having found the game's own panes unusable. The
    // phone just draws a rectangle.
    u8 oilPct = 0;
    u8 oxygenPct = 0;
    // Whether each gauge should be shown at all, mirroring drawMeterBar()'s
    // own gates so the phone's native bar appears and disappears in step
    // with the streamed dashboard's rather than on its own rules. Oxygen
    // takes priority over oil: the game shares ONE set of panes between
    // them, so they are mutually exclusive on the real HUD too.
    bool oilVisible = false;
    bool oxygenVisible = false;

    bool operator==(const HudState&) const = default;
};

// Fills `out` and returns true if the companion HUD is ready to report state
// (mirrors the hudReady() gate the rest of the companion module already
// uses before touching game state); returns false and leaves `out`
// unmodified otherwise. Pure read: no drawing, no side effects, safe to
// call every frame.
bool gatherHudState(HudState& out);

// Phase-2 addition: draws ONE item's icon (any itemNo a phone requested,
// see phone_spike_ws.h's request_icon()) into the CURRENT 2D render
// context, sized to fill most of the given canvas — dualscreen.cpp's
// endHudCapture() calls this INSTEAD OF drawDashboard() for one substituted
// frame per icon-fetch request, borrowing the exact same
// capture/encode/send pipeline the whole-dashboard frame stream already
// uses (see docs and the state-streaming design plan's "icon rendering
// mechanism" section — this draws and reads the GPU surface back
// rather than decoding the texture on the CPU. NOTE: an earlier version of
// this comment claimed no CPU-side texture decoder existed anywhere in the
// codebase, and used that to justify this approach. That was wrong —
// aurora::convert_texture()/convert_texture_palette()
// (extern/aurora/lib/gfx/texture_convert.hpp) handle every GX format
// including the CI8 these icons use, and their source bytes are already in
// CPU memory. Decoding directly would avoid stealing a capture slot from
// the frame stream, which is why fetching art visibly drops the frame rate.
// Left as-is for now because it is working and proven, but it is the
// obvious thing to replace). Uses the dedicated ICON_SLOT_PHONE_REQUEST cache slot
// (companion_internal.h) so it can never corrupt a live dashboard icon
// slot's cache, no matter what itemNo is requested. Caller (dualscreen.cpp)
// is responsible for the surrounding render-pass/ortho-context setup —
// this function only issues the draw call itself, same division of
// responsibility drawDashboard() already has with its own caller.
void drawPhoneRequestedIcon(u8 itemNo, f32 canvasW, f32 canvasH);

// Phase-3 addition: hearts. Unlike items, heart container art isn't a
// static archive texture loadable by identity — it's live, currently-
// animating J2DPicture objects owned by the game's own HUD meter
// (dMeter2Draw_c), so there's no "load state N" call to make. Instead this
// opportunistically probes the 20 live heart slots (dMeter2Draw_c::
// getHeartState(), a new small read-only accessor added alongside the
// existing getHeartPictures() in include/d/d_meter2_draw.h) for one
// CURRENTLY showing `wantedState` (0=empty, 1/2/3=quarter/half/three-
// quarter, 4=full — see getHeartState()'s own doc comment for exactly how
// these map to the game's internal quarter-texture tags), and if found,
// draws that slot's live picture(s) into the CURRENT 2D render context
// (same flat-clear + small-fixed-size treatment as
// drawPhoneRequestedIcon(), see its doc comment for why) — never mutates
// the live panes' visibility/texture, only reads their current state and
// draws (with the pane's transform matrix saved/restored around the draw,
// same as companion_hud.cpp's drawHeartsRow() already does for the exact
// same reason: draw() clobbers the pane's own position matrix, and this is
// the GAME's shared pane, not something owned by this feature).
//
// Returns false (draws nothing, sends nothing) if no live slot currently
// shows the wanted state — a player who hasn't taken graduated damage may
// not have a heart showing e.g. state 2 (half) available yet. The caller
// (dualscreen.cpp) keeps the request pending and retries on a later frame
// rather than treating this as a failure.
bool drawWantedHeartIcon(u8 wantedState, f32 canvasW, f32 canvasH);

}  // namespace dusk::companion
