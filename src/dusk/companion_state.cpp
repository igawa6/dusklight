#include "dusk/companion_state.h"

#include "dusk/dualscreen.h"

#if DUSK_PHONE_SPIKE_STATE

#include "dusk/companion.h"
#include "dusk/companion_internal.h"

#include "JSystem/J2DGraph/J2DPicture.h"
#include "d/d_com_inf_game.h"
#include "d/d_meter2_draw.h"
#include "m_Do/m_Do_mtx.h"

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
    // Explicit flat-color clear FIRST — found missing via live testing:
    // without it, this draws into whatever the aux render target's
    // pooled/reused texture already held (the last real dashboard frame's
    // detailed content), and the small icon ends up composited over that
    // stale, highly-detailed background instead of a blank one. Every
    // normal dashboard draw path (drawDashboard()/drawSplash()) starts
    // with its own full-canvas fillRect() for exactly this reason
    // (companion_hud.cpp:566's `fillRect(0.0f, 0.0f, w, h, COL_BG)`) — this
    // path was missing that step. Fixed the rupee gem capture from ~650KB
    // down to ~130KB (confirmed: both captures are the same real
    // resolution — the phone's negotiated aux surface size, e.g. 800x1200,
    // not the clamped logical canvasW/canvasH — so the size difference
    // really is compression efficiency, not resolution).
    //
    // KNOWN REMAINING ISSUE, not yet root-caused: the small key icon
    // (dItemNo_SMALL_KEY_e) still captures at ~865KB despite this fix and
    // the small fixed draw size below — confirmed via live testing that
    // both this fillRect() and the icon draw genuinely execute (logged),
    // at the same real capture resolution as the rupee case, so this isn't
    // a resolution or sequencing bug. The remaining difference traces into
    // dMeter2Info::readItemTexture()/J2DPicture's shared, non-trivial
    // texture-loading path (src/d/d_meter2_info.cpp) — something itemNo-
    // specific there is producing more visual entropy for this one item
    // than the flat background + small icon should account for. Not
    // pursued further for v1: it's a one-time, per-identity cost (not a
    // per-frame one), and digging further means modifying complex, already
    // shared/proven decompiled rendering code rather than this feature's
    // own additive path — a real follow-up, not a blocker.
    fillRect(0.0f, 0.0f, canvasW, canvasH, COL_BG);

    // Small FIXED size, not a fraction of the canvas: the capture still
    // reads back the whole aux surface (whatever resolution the phone's
    // own hello negotiated, e.g. ~1270x2416) — this just keeps the drawn
    // region itself small too, on top of the flat-fill fix above.
    constexpr f32 kIconSize = 128.0f;
    const f32 size = kIconSize < canvasW && kIconSize < canvasH
        ? kIconSize
        : (canvasW < canvasH ? canvasW : canvasH) * 0.9f;
    const f32 x = (canvasW - size) * 0.5f;
    const f32 y = (canvasH - size) * 0.5f;
    drawItemIcon(ICON_SLOT_PHONE_REQUEST, itemNo, x, y, size);
}

bool drawWantedHeartIcon(u8 wantedState, f32 canvasW, f32 canvasH) {
    dMeter2Draw_c* md = meterDraw();
    if (md == NULL) {
        return false;
    }
    int slot = -1;
    for (int i = 0; i < 20; i++) {
        if (md->getHeartState(i) == wantedState) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return false;  // nothing live currently shows this state — caller retries later
    }
    J2DPicture* heartPics[2];
    const int picCount = md->getHeartPictures(slot, heartPics);
    if (picCount <= 0) {
        return false;  // raced with the game's own state changing between the two calls above
    }

    // Same flat clear + small fixed size as drawPhoneRequestedIcon() — see
    // its doc comment for why (uncleared stale background, wrong-resolution
    // sizing).
    fillRect(0.0f, 0.0f, canvasW, canvasH, COL_BG);
    constexpr f32 kIconSize = 128.0f;
    const f32 size = kIconSize < canvasW && kIconSize < canvasH
        ? kIconSize
        : (canvasW < canvasH ? canvasW : canvasH) * 0.9f;
    const f32 x = (canvasW - size) * 0.5f;
    const f32 y = (canvasH - size) * 0.5f;

    for (int j = 0; j < picCount; j++) {
        // These are the GAME's live panes, shared with the main HUD —
        // draw() overwrites the pane's own position matrix
        // (J2DPane::draw: MTXConcat(parent->mGlobalMtx, mPositionMtx, ...)
        // is what the main screen's own hierarchical draw later consumes),
        // so it must be restored after borrowing it here. Exactly the same
        // save/restore companion_hud.cpp's drawHeartsRow() already does
        // for the identical reason — skipping it showed up there as
        // displaced duplicate hearts the moment the HUD moved back to the
        // main screen; same risk here since this shares the same panes.
        Mtx saved;
        MTXCopy(*heartPics[j]->getMtx(), saved);
        heartPics[j]->draw(x, y, size, size, false, false, false);
        heartPics[j]->setMtx(saved);
    }
    dComIfGp_getCurrentGrafPort()->setup2D();
    return true;
}

}  // namespace dusk::companion

#endif  // DUSK_PHONE_SPIKE_STATE
