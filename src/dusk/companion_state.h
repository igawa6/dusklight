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

}  // namespace dusk::companion
