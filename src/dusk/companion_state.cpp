#include "dusk/companion_state.h"

#include "dusk/dualscreen.h"

#if DUSK_PHONE_SPIKE_STATE

#include "dusk/companion.h"

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

}  // namespace dusk::companion

#endif  // DUSK_PHONE_SPIKE_STATE
