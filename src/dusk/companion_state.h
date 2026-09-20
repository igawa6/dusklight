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
// mechanism" section — no CPU-side texture decoder exists anywhere in this
// codebase, so getting RGBA8 pixels for any icon means drawing it and
// reading the GPU surface back, same as every other capture in this
// feature). Uses the dedicated ICON_SLOT_PHONE_REQUEST cache slot
// (companion_internal.h) so it can never corrupt a live dashboard icon
// slot's cache, no matter what itemNo is requested. Caller (dualscreen.cpp)
// is responsible for the surrounding render-pass/ortho-context setup —
// this function only issues the draw call itself, same division of
// responsibility drawDashboard() already has with its own caller.
void drawPhoneRequestedIcon(u8 itemNo, f32 canvasW, f32 canvasH);

}  // namespace dusk::companion
