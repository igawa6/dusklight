#include "dusk/phone_spike_pad.h"

#include "dusk/dualscreen.h"

#if DUSK_PHONE_SPIKE

#include "dusk/phone_spike_ws.h"

#include <dolphin/pad.h>

#include <algorithm>
#include <cmath>
#include <mutex>

namespace dusk::phone_spike {
namespace {

std::mutex g_stateMutex;
GamepadState g_state;

s8 stick_value(float value) noexcept {
    return static_cast<s8>(std::clamp(std::lround(value * 127.f), -127l, 127l));
}

}  // namespace

void set_gamepad_state(const GamepadState& state) {
    std::lock_guard lock{g_stateMutex};
    g_state = state;
}

void applyGamepadPassthrough() {
    if (!has_client()) {
        // No phone connected — nothing to hold a button/stick stuck on, but
        // clear anyway in case a connection just dropped mid-hold.
        PADClearVirtualStatus(PAD_CHAN0);
        return;
    }

    GamepadState state;
    {
        std::lock_guard lock{g_stateMutex};
        state = g_state;
    }

    PADStatus status{};
    status.err = PAD_ERR_NONE;
    if (state.up) {
        status.button |= PAD_BUTTON_UP;
    }
    if (state.down) {
        status.button |= PAD_BUTTON_DOWN;
    }
    if (state.left) {
        status.button |= PAD_BUTTON_LEFT;
    }
    if (state.right) {
        status.button |= PAD_BUTTON_RIGHT;
    }
    if (state.a) {
        status.button |= PAD_BUTTON_A;
    }
    if (state.b) {
        status.button |= PAD_BUTTON_B;
    }
    if (state.x) {
        status.button |= PAD_BUTTON_X;
    }
    if (state.y) {
        status.button |= PAD_BUTTON_Y;
    }
    if (state.start) {
        status.button |= PAD_BUTTON_START;
    }
    if (state.z) {
        status.button |= PAD_TRIGGER_Z;
    }
    if (state.l) {
        status.button |= PAD_TRIGGER_L;
    }
    if (state.r) {
        status.button |= PAD_TRIGGER_R;
    }

    status.stickX = stick_value(state.leftStickX);
    status.stickY = stick_value(-state.leftStickY);  // browser Y+ = down, GC Y+ = up
    status.substickX = stick_value(state.rightStickX);
    status.substickY = stick_value(-state.rightStickY);
    status.triggerLeft = static_cast<u8>(std::clamp(state.leftTrigger, 0.0f, 1.0f) * 255.0f);
    status.triggerRight = static_cast<u8>(std::clamp(state.rightTrigger, 0.0f, 1.0f) * 255.0f);

    const bool active = status.button != 0 || status.stickX != 0 || status.stickY != 0 ||
        status.substickX != 0 || status.substickY != 0 || status.triggerLeft != 0 ||
        status.triggerRight != 0;
    if (active) {
        PADSetVirtualStatus(PAD_CHAN0, &status);
    } else {
        PADClearVirtualStatus(PAD_CHAN0);
    }
}

}  // namespace dusk::phone_spike

#endif  // DUSK_PHONE_SPIKE
