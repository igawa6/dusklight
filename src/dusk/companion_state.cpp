#include "dusk/companion_state.h"

#include "dusk/dualscreen.h"

#if DUSK_PHONE_SPIKE_STATE

#include "dusk/companion.h"
#include "dusk/companion_internal.h"

#include "d/d_com_inf_game.h"

namespace dusk::companion {

bool gatherHudState(HudState& out) {
    if (!hudReady()) {
        return false;
    }
    out.life = dComIfGs_getLife();
    out.maxLife = dComIfGs_getMaxLife();
    out.rupees = dComIfGs_getRupee();
    out.maxRupees = dComIfGs_getRupeeMax();
    out.keys = dComIfGs_getKeyNum();
    out.equipX = dComIfGp_getSelectItem(0);
    out.equipY = dComIfGp_getSelectItem(1);
    out.equipSlot1 = dComIfGp_getSelectItem(2);
    out.equipSlot2 = dComIfGp_getSelectItem(3);
    return true;
}

void drawPhoneRequestedIcon(u8 itemNo, f32 canvasW, f32 canvasH) {
    // Filled square, centered, leaving a small margin — matches how the
    // dashboard's own item icons are drawn (drawItemIcon's own aspect-fit
    // sizing logic keeps non-square art from stretching). Deliberately
    // simple: this is a one-off substitution frame, not dashboard layout.
    const f32 size = (canvasW < canvasH ? canvasW : canvasH) * 0.9f;
    const f32 x = (canvasW - size) * 0.5f;
    const f32 y = (canvasH - size) * 0.5f;
    drawItemIcon(ICON_SLOT_PHONE_REQUEST, itemNo, x, y, size);
}

}  // namespace dusk::companion

#endif  // DUSK_PHONE_SPIKE_STATE
