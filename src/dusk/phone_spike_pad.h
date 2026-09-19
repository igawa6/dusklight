#pragma once

// Phase-5 spike for the phone-companion-display idea (see
// docs/phone-companion-design.md): a gamepad connected to the PHONE acts as
// a real second controller for the MAIN GAME — not the companion display,
// which touch alone continues to drive via dusk::companion::touchEvent()/
// pinchZoom(). Bridges the browser Standard Gamepad API state the test page
// reports into dolphin::PAD's existing virtual-controller injection point
// (PADSetVirtualStatus/PADClearVirtualStatus on PAD_CHAN0) — the exact
// mechanism the shipping on-screen touch controls already use (see
// src/dusk/ui/touch_controls.cpp's sync_virtual_input()), so the game's own
// input-reading code needs zero changes.
//
// Gated by DUSK_PHONE_SPIKE (dualscreen.h) and off by default.

namespace dusk::phone_spike {

struct GamepadState {
    bool up = false, down = false, left = false, right = false;
    bool a = false, b = false, x = false, y = false;
    bool start = false;
    bool l = false, r = false, z = false;
    // -1..1; Y+ is "down" in the browser Gamepad API's convention, flipped
    // to GameCube's Y+-is-up when this gets turned into a PADStatus.
    float leftStickX = 0.0f, leftStickY = 0.0f;
    float rightStickX = 0.0f, rightStickY = 0.0f;
    float leftTrigger = 0.0f, rightTrigger = 0.0f;  // 0..1
};

// Called from the WS receive thread with each parsed "pad" message.
// Thread-safe: mutex-guarded latest-value, not a queue — gamepad state is
// continuous (what matters is "what's held right now"), unlike the
// discrete one-shot events (touch taps, resize requests) elsewhere in this
// spike, so only ever the newest report matters.
void set_gamepad_state(const GamepadState& state);

// Builds a PADStatus from the latest reported state and applies it via
// PADSetVirtualStatus(PAD_CHAN0, ...) — or PADClearVirtualStatus(PAD_CHAN0)
// if nothing is currently held, or if there's no phone connected at all
// (so a button/stick held at the moment the connection drops can't get
// stuck on forever). Mirrors touch_controls.cpp's sync_virtual_input()
// exactly. GAME THREAD ONLY, once per frame — this codebase's only other
// caller of PADSetVirtualStatus/PADClearVirtualStatus only ever calls them
// from there, and they aren't documented thread-safe.
void applyGamepadPassthrough();

}  // namespace dusk::phone_spike
