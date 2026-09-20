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

namespace {

// Quantize a raw gauge counter to 0..100. Rounds to nearest so a full
// gauge reads 100 and a just-barely-nonempty one doesn't read 0 while the
// game still shows a sliver of fill.
u8 gaugePercent(int now, int max) {
    if (max <= 0 || now <= 0) {
        return 0;
    }
    if (now >= max) {
        return 100;
    }
    return (u8)((now * 100 + max / 2) / max);
}

// The itemNo to report for an equip slot. Normally just what the slot
// holds, with one substitution: the lantern's icon swaps to dedicated
// "empty" art once the oil runs out, which the dashboard's own icon cache
// already accounts for by rewriting its cache key
// (companion_icons.cpp's updateItemPics()). The phone fetches icon art BY
// ITEM NUMBER through the generic icon_request path, so doing the same
// substitution here means the phone asks for, caches and draws the empty-
// lantern art automatically — no new message type, no oil-awareness on the
// phone side at all, and the icon updates the instant the oil runs dry
// because the substitution changes the reported itemNo, which the existing
// whole-struct diff already notices.
u8 equippedItemForPhone(int button) {
    // Buttons 2/3 are slots I/II, which the Ooccoo quick-use path can
    // temporarily BORROW. The dashboard deliberately keeps showing the
    // player's own item throughout that borrow (slotDisplayBinding(),
    // companion.cpp) rather than the borrowed one, so reading the raw
    // selection here made the phone display something the local second
    // screen intentionally hides — the two disagreed for the duration of
    // every borrow.
    const u8 itemNo = button >= 2 ? (u8)slotDisplayBinding(button - 2)
                                  : dComIfGp_getSelectItem(button);
    if (itemNo == dItemNo_KANTERA_e && dComIfGs_getOil() == 0) {
        return dItemNo_KANTERA2_e;
    }
    return itemNo;
}

// Mirrors drawMeterBar()'s gates (companion.cpp) rather than the game's own
// alphaAnimeKantera/alphaAnimeOxygen, so the phone's native bar tracks what
// the companion dashboard shows — including the companion's deliberate
// relaxation of the oil gate (vanilla hides the oil disc unless the lantern
// is actively LIT, which is too strict for a persistent fuel readout; the
// companion keeps only the wolf-form half of that gate).
void gatherGauges(HudState& out) {
    dMeter2Draw_c* md = meterDraw();
    const bool oxygen =
        md != NULL && md->isOxygenActive() && dComIfGp_getMaxOxygen() > 0;

    // The lantern counts as equipped on ANY of the four item buttons.
    bool lanternEquipped = false;
    for (int b = 0; b < 4; b++) {
        if (dComIfGp_getSelectItem(b) == dItemNo_KANTERA_e) {
            lanternEquipped = true;
            break;
        }
    }
    const bool oil = !oxygen && !companionWolf() && lanternEquipped &&
        dComIfGs_getMaxOil() > 0;

    out.oxygenVisible = oxygen;
    out.oilVisible = oil;
    // NowOxygen, not Oxygen: that's the meter-animation value the game's own
    // bar renders, so the fill depletes smoothly instead of stepping — same
    // choice drawMeterBar() makes.
    out.oxygenPct = oxygen
        ? gaugePercent(dComIfGp_getNowOxygen(), dComIfGp_getMaxOxygen())
        : 0;
    out.oilPct =
        oil ? gaugePercent(dComIfGs_getOil(), dComIfGs_getMaxOil()) : 0;
}

}  // namespace

bool gatherHudState(HudState& out) {
    if (!hudReady()) {
        return false;
    }
    out.life = dComIfGs_getLife();
    out.maxLife = dComIfGs_getMaxLife();
    out.rupees = dComIfGs_getRupee();
    out.maxRupees = dComIfGs_getRupeeMax();
    out.keys = dComIfGs_getKeyNum();
    out.equipX = equippedItemForPhone(0);
    out.equipY = equippedItemForPhone(1);
    out.equipSlot1 = equippedItemForPhone(2);
    out.equipSlot2 = equippedItemForPhone(3);
    gatherGauges(out);
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
    // (A second, unrelated cause of oversized captures — every heart and the
    // key item returning a byte-identical copy of the whole dashboard — was
    // a capture/presentation race, since fixed; see
    // s_iconCaptureDrawnFrames in dualscreen.cpp.)
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
