// Companion dashboard: second-screen touch input. Tab/page taps arrive as
// one-shot normalized points (s_pendingTouch); drag & drop equipping
// consumes the live DOWN/MOVE/UP stream (s_touchPos/s_touchPhase). Both
// run at the END of drawDashboard so they hit-test against the geometry
// this frame's draw just published (s_invCells, s_gearBox*, s_dropRect).

#include "dusk/companion.h"
#include "dusk/companion_internal.h"
#include "dusk/companion_strings.h"
#include "dusk/dualscreen.h"

#include "Z2AudioLib/Z2SeMgr.h"
#include "d/d_com_inf_game.h"
#include "d/d_item_data.h"
#include "d/d_meter2_draw.h"
#include "d/d_meter2_info.h"
#include "d/actor/d_a_player.h"
#include "d/actor/d_a_alink.h"
#include <dolphin/pad.h>

#include <cstdarg>

namespace dusk::companion {
namespace {

// Drag gesture tracking.
bool s_touchTracking = false;
f32 s_readerTapRect[4] = {};
f32 s_downX, s_downY;
// This gesture started inside the Functional left column's bottom box, so a
// vertical release there pages its carousel.
bool s_leftBoxSwipe = false;

// Post the transient equip feedback line (drawn by the active page).
void setEquipMsg(int frames, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(s_equipMsg, sizeof(s_equipMsg), fmt, args);
    va_end(args);
    s_equipMsgFrames = frames;
}

// Bow combos, on any of the four item buttons. Dropping a bomb bag /
// hawkeye onto a button that holds the bow (plain or already comboed) arms
// bomb/hawk arrows — the same state the item wheel writes (select = bomb
// slot, mix = bow slot). Dropping the bow onto its own combo button turns
// the combo off. Returns true when the drop was consumed as a combo action.
bool tryBowCombo(int btn, int slot, u8 itemNo) {
    const bool comboPartner = itemNo == dItemNo_NORMAL_BOMB_e ||
        itemNo == dItemNo_WATER_BOMB_e || itemNo == dItemNo_POKE_BOMB_e ||
        itemNo == dItemNo_HAWK_EYE_e;
    const bool btnHasBow = dComIfGs_getSelectItemIndex(btn) == SLOT_4 ||
        dComIfGs_getMixItemIndex(btn) == SLOT_4;
    if (comboPartner && btnHasBow) {
        for (int other = 0; other < 4; other++) {
            if (other == btn) {
                continue;
            }
            // The bow can only ever mix on one button.
            if (dComIfGs_getMixItemIndex(other) == SLOT_4) {
                dComIfGs_setMixItemIndex(other, dItemNo_NONE_e);
                dComIfGs_setSelectItemIndex(other, SLOT_4);
            }
            // If the combo partner sits on another button, clear it there.
            if (dComIfGs_getSelectItemIndex(other) == slot) {
                dComIfGs_setMixItemIndex(other, dItemNo_NONE_e);
                dComIfGs_setSelectItemIndex(other, dItemNo_NONE_e);
            }
        }
        dComIfGs_setMixItemIndex(btn, SLOT_4);
        dComIfGs_setSelectItemIndex(btn, (u8)slot);
        // The item name comes from the archive; the sentence around it from our
        // table (the game has no equivalent phrasing). "Bomb Arrows" is not an
        // item name — dItemNo_BOMB_ARROW_e resolves to "Hero's Bow" — so that
        // one comes from the table too.
        setEquipMsg(120, txt(STR_COMBO_ON),
            itemNo == dItemNo_HAWK_EYE_e
                ? archiveText(0x165 + dItemNo_HAWK_EYE_e, "Hawkeye")
                : txt(STR_BOMB_ARROWS));
        // Marked apart from a plain equip's ITEM_SET_X/_Y: arming a combo is
        // the special outcome, so it gets the confirm cue.
        queueSound(Z2SE_SY_CURSOR_OK, HAPTIC_CONFIRM);
        return true;
    }
    if (itemNo == dItemNo_BOW_e && dComIfGs_getMixItemIndex(btn) == SLOT_4) {
        dComIfGs_setMixItemIndex(btn, dItemNo_NONE_e);
        dComIfGs_setSelectItemIndex(btn, SLOT_4);
        // archiveText, not the 5-arg localizedWord: that overload latched its
        // English fallback permanently if the archive happened to be
        // unavailable on the one frame it fetched (e.g. mid room transition).
        setEquipMsg(120, "%s", archiveText(0x04D3, "Combo off"));
        queueSound(Z2SE_SY_CURSOR_CANCEL, HAPTIC_LIGHT);
        return true;
    }
    return false;
}

void plainEquip(int btn, int slot);

bool equipFromCompanion(int btn, int slot) {
    if (anyMenuOpen()) {
        setEquipMsg(150, "%s", txt(STR_NO_EQUIP_MENU));
        queueSound(Z2SE_SYS_ERROR, HAPTIC_DENY);
        return false;
    }
    if (companionWolf()) {
        setEquipMsg(150, "%s", txt(STR_NO_EQUIP_WOLF));
        queueSound(Z2SE_SYS_ERROR, HAPTIC_DENY);
        return false;
    }
    if (slot < 0 || slot >= MAX_ITEM_SLOTS ||
        dComIfGs_getItem(slot, false) == dItemNo_NONE_e)
    {
        return false;
    }
    const u8 itemNo = dComIfGs_getItem(slot, false);
    if (btn >= DROP_TARGET_SLOT1) {
        // Talk/trade items need the per-button talk-event plumbing that only
        // exists for X/Y.
        if (daPy_py_c::checkTradeItem(itemNo)) {
            setEquipMsg(150, "%s", txt(STR_NO_SLOT));
            queueSound(Z2SE_SYS_ERROR, HAPTIC_DENY);
            return false;
        }
        // Dropping the slot's own PLAIN item back unbinds it (toggle) —
        // unless it forms a combo (bow onto its combo = combo off, below).
        if (slotBinding(btn - DROP_TARGET_SLOT1) == slot &&
            dComIfGs_getMixItemIndex(btn) == 0xFF)
        {
            setSlotBinding(btn - DROP_TARGET_SLOT1, -1);
            setEquipMsg(120, txt(STR_SLOT_CLEARED), btn == DROP_TARGET_SLOT1 ? "I" : "II");
            queueSound(Z2SE_SY_CURSOR_CANCEL, HAPTIC_LIGHT);
            return true;
        }
    }
    // Ambiguous drop: a combo partner onto a bow-holding button. GC vanilla
    // splits the outcomes across inputs (X/Y equip replaces, only the
    // explicit R press combines) — touch has one gesture, so ask instead of
    // assuming. The chooser (drawComboChoice) resolves to tryBowCombo or
    // plainEquip in handleTouch.
    {
        const bool comboPartner = itemNo == dItemNo_NORMAL_BOMB_e ||
            itemNo == dItemNo_WATER_BOMB_e || itemNo == dItemNo_POKE_BOMB_e ||
            itemNo == dItemNo_HAWK_EYE_e;
        const bool btnHasBow = dComIfGs_getSelectItemIndex(btn) == SLOT_4 ||
            dComIfGs_getMixItemIndex(btn) == SLOT_4;
        if (comboPartner && btnHasBow) {
            s_comboChoiceBtn = btn;
            s_comboChoiceSlot = slot;
            queueSound(Z2SE_SY_CURSOR_ITEM, HAPTIC_LIGHT);
            return true;
        }
    }
    // Combo-off still works directly (bow dropped onto its own combo).
    if (tryBowCombo(btn, slot, itemNo)) {
        return true;
    }
    plainEquip(btn, slot);
    return true;
}

// Plain (no-combo) equip executor — the wheel's setItem semantics; also the
// chooser's "Replace" outcome.
void plainEquip(int btn, int slot) {
    // Plain equip onto any of the four buttons (btn IS the select-item
    // index; the slots are indices 2/3), with the wheel's own semantics
    // (dMenu_Ring_c::setItem): the equipped item always arrives PLAIN; a
    // button whose SELECT is taken receives this button's previous
    // select-and-mix pair whole (a swapped-aside combo survives); equipping
    // a button's MIX partner (the bow) dissolves that combo and its bag
    // drops off the buttons. Trade items never land on a slot.
    const u8 prevSel = dComIfGs_getSelectItemIndex(btn);
    const u8 prevMix = dComIfGs_getMixItemIndex(btn);
    dComIfGs_setMixItemIndex(btn, dItemNo_NONE_e);
    for (int b = 0; b < 4; b++) {
        if (b == btn) {
            continue;
        }
        const bool selMatch = dComIfGs_getSelectItemIndex(b) == (u8)slot;
        const bool mixMatch = dComIfGs_getMixItemIndex(b) == (u8)slot;
        if (!selMatch && !mixMatch) {
            continue;
        }
        u8 give = prevSel;
        u8 giveMix = selMatch ? prevMix : (u8)dItemNo_NONE_e;
        if (b >= DROP_TARGET_SLOT1 && give < MAX_ITEM_SLOTS &&
            daPy_py_c::checkTradeItem(dComIfGs_getItem(give, false)))
        {
            give = dItemNo_NONE_e;
        }
        if (give == dItemNo_NONE_e) {
            giveMix = dItemNo_NONE_e;
        }
        dComIfGs_setMixItemIndex(b, giveMix);
        dComIfGs_setSelectItemIndex(b, give);
    }
    dComIfGs_setSelectItemIndex(btn, (u8)slot);
    if (btn >= DROP_TARGET_SLOT1) {
        setEquipMsg(120, txt(STR_SLOT_BOUND), btn == DROP_TARGET_SLOT1 ? "I" : "II");
        queueSound(Z2SE_SY_CURSOR_OK, HAPTIC_CONFIRM);
    } else {
        // Distinct per-button cues, exactly as the vanilla menus equip.
        queueSound(btn == 0 ? Z2SE_SY_ITEM_SET_X : Z2SE_SY_ITEM_SET_Y, HAPTIC_CONFIRM);
        s_equipMsgFrames = 0;
    }
}

// Gear changes are blocked in menus, as a wolf, and while a previous model
// swap is still reloading. Posts the reason when it is user-visible.
bool gearChangeBlocked(daPy_py_c* player) {
    if (anyMenuOpen()) {
        setEquipMsg(150, "%s", txt(STR_NO_GEAR_MENU));
        queueSound(Z2SE_SYS_ERROR, HAPTIC_DENY);
        return true;
    }
    if (player == NULL) {
        return true;
    }
    if (player->checkWolf()) {
        setEquipMsg(150, "%s", txt(STR_NO_GEAR_WOLF));
        queueSound(Z2SE_SYS_ERROR, HAPTIC_DENY);
        return true;
    }
    return player->getSwordChangeWaitTimer() != 0 || player->getShieldChangeWaitTimer() != 0 ||
           player->getClothesChangeWaitTimer() != 0;
}

void equipGear(int idx) {
    if (idx < 0 || idx >= 7) {
        return;
    }
    const u8 itemNo = gearItemFor(idx);
    if (!dComIfGs_isItemFirstBit(itemNo)) {
        return;
    }
    daPy_py_c* player = daPy_getPlayerActorClass();
    if (gearChangeBlocked(player)) {
        return;
    }
    if (idx < 2) {
        // Swords: savedata write only — the actor pointer-swaps the loaded
        // models every frame.
        dMeter2Info_setSword(itemNo, false);
    } else if (idx < 4) {
        // Shields: per-shield archive; trigger the change, the actor's
        // execute loop pumps the reload.
        dMeter2Info_setShield(itemNo, false);
        daAlink_c* alink = daAlink_getAlinkActorClass();
        if (alink != NULL) {
            alink->setShieldChange();
        }
    } else {
        // Clothes: full Link-archive reload, pumped by the actor's own
        // execute loop.
        dMeter2Info_setCloth(itemNo, false);
        player->setClothesChange(0);
    }
    static char s_gearName[64];
    s_gearName[0] = 0;
    dMeter2Info_getString(0x165 + itemNo, s_gearName, NULL);
    setEquipMsg(120, txt(STR_EQUIPPED),
        s_gearName[0] != 0 ? s_gearName : txt(STR_GEAR));
    // Same cue the vanilla collection screen uses for its own gear equips.
    queueSound(Z2SE_SY_ITEM_SET_X, HAPTIC_CONFIRM);
}

// --- Touch item buttons (Functional) ----------------------------------------

// Menus own the X/Y buttons (the wheel equips with them, the item-explain
// window closes with them) — never inject item presses there.
bool touchUseBlocked() {
    return anyMenuOpen() || dMeter2Info_getItemExplainWindowStatus() != 0;
}

// Start a touch hold on an item button (0 = X, 1 = Y, 2/3 = slots I/II).
// The button reads held until the finger lifts — bow-class items aim on
// hold and fire on release, so a one-shot press would only raise them. No
// cue: the game's own item sounds are the feedback, like a physical press.
void beginHold(int btn) {
    s_holdBtn = btn;
    s_holdFrames = 0;
    s_holdReleaseReq = false;
    if (btn < 2) {
        s_padHoldMaskState.store(btn == 0 ? PAD_BUTTON_X : PAD_BUTTON_Y);
    } else {
        s_slotHoldMaskState.store(1u << (btn - 2));
    }
}

// Finger lifted (or the gesture was cancelled): release the held button.
// Short taps are deferred until the press has lasted TAP_HOLD_FRAMES game
// frames (beginFrameCompanionInput applies them), so a tap reads as a
// deliberate press instead of a wind-up-cancelling blip.
void releaseHold() {
    if (s_holdBtn < 0) {
        return;
    }
    // Up-click at the FINGER's lift (not the deferred synthetic release):
    // lighter than the down-click, completing the press/release pair. For
    // short taps both land in the same frame and the flush coalesces them
    // into the single firmer click, so taps don't double-buzz.
    queueHaptic(HAPTIC_LIGHT);
    if (s_holdFrames < TAP_HOLD_FRAMES) {
        s_holdReleaseReq = true;
        return;
    }
    s_holdBtn = -1;
    s_holdReleaseReq = false;
    s_padHoldMaskState.store(0);
    s_slotHoldMaskState.store(0);
}

// Tap on a round X/Y button: use the equipped item. In equip mode the tap
// keeps its equip meaning (handled on release via dropTargetAt).
void handleXYTap(int xy) {
    if (inEquipMode()) {
        return;
    }
    // A selection with a menu open means the player is TRYING to equip but
    // the mode couldn't engage — say why instead of silently beeping (same
    // message the gear boxes post).
    if (s_selSlot >= 0 && anyMenuOpen()) {
        setEquipMsg(150, "%s", txt(STR_NO_EQUIP_MENU));
        queueSound(Z2SE_SYS_ERROR, HAPTIC_DENY);
        s_denyFlash[xy] = DENY_FLASH_FRAMES;
        return;
    }
    dMeter2Draw_c* md = meterDraw();
    // Wolf form: items are never usable, but X/Y ARE the wolf's action
    // buttons — the circles draw the game's own "Sense"/"Dig" words, and a
    // tap presses through whenever a word is showing. Without this the
    // usability gate below would deny every wolf tap.
    if (companionWolf()) {
        const char* action =
            (md != NULL && !touchUseBlocked()) ? md->getActionTextXY(xy) : NULL;
        if (action == NULL || action[0] == 0) {
            s_denyFlash[xy] = DENY_FLASH_FRAMES;
            return;
        }
        queueHaptic(HAPTIC_PRESS);
        beginHold(xy);
        return;
    }
    if (md == NULL || dComIfGp_getSelectItem(xy) == dItemNo_NONE_e) {
        // Empty button: swallow, but still flash — a tap that dies with no
        // reaction at all reads as a broken button, not an empty one.
        s_denyFlash[xy] = DENY_FLASH_FRAMES;
        return;
    }
    if (touchUseBlocked() || !md->isItemUsable(xy)) {
        queueSound(Z2SE_SYS_ERROR, HAPTIC_DENY);
        s_denyFlash[xy] = DENY_FLASH_FRAMES;
        return;
    }
    queueHaptic(HAPTIC_PRESS);
    beginHold(xy);
}

// Tap on the I / II slot: press item button 2/3 — the slots ARE first-class
// buttons now, so this is exactly a tap on X or Y.
void handleSlotTap(int which) {
    if (inEquipMode()) {
        return;  // release path binds the selection to this slot
    }
    // Wolf form: the corners are pure readouts (scent / Poe souls) on dark
    // unequipped-style plates — a tap is swallowed inert (no sound, no
    // flash) so nothing suggests they were ever buttons. The rect still
    // publishes so the tap can't fall through to the page beneath.
    if (companionWolf()) {
        return;
    }
    // See handleXYTap: a blocked equip attempt explains itself.
    if (s_selSlot >= 0 && anyMenuOpen()) {
        setEquipMsg(150, "%s", txt(STR_NO_EQUIP_MENU));
        queueSound(Z2SE_SYS_ERROR, HAPTIC_DENY);
        s_denyFlash[2 + which] = DENY_FLASH_FRAMES;
        return;
    }
    const int bound = slotBinding(which);
    const u8 itemNo = bound >= 0 ? dComIfGs_getItem(bound, false) : (u8)dItemNo_NONE_e;
    if (itemNo == dItemNo_NONE_e) {
        // Empty slot: swallow with a flash (see handleXYTap).
        s_denyFlash[2 + which] = DENY_FLASH_FRAMES;
        return;
    }
    dMeter2Draw_c* md = meterDraw();
    if (md == NULL || touchUseBlocked() || !md->isItemUsable(2 + which)) {
        queueSound(Z2SE_SYS_ERROR, HAPTIC_DENY);
        s_denyFlash[2 + which] = DENY_FLASH_FRAMES;
        return;
    }
    queueHaptic(HAPTIC_PRESS);
    beginHold(2 + which);
}

// ITEMS grid cell index under (tx, ty), -1 when none.
int invCellIndexAt(f32 tx, f32 ty) {
    for (int i = 0; i < s_invCellCount; i++) {
        if (tx >= s_invCells[i].x && tx < s_invCells[i].x + s_invCell &&
            ty >= s_invCells[i].y && ty < s_invCells[i].y + s_invCell)
        {
            return i;
        }
    }
    return -1;
}

// Drop-target index under (tx, ty) with an 8px grab margin, -1 when none:
// 0/1 the real X/Y buttons, 2/3 the I/II slot bindings. Entries the current
// layout didn't publish this frame have zero width.
int dropTargetAt(f32 tx, f32 ty) {
    for (int i = 0; s_dropRectValid && i < DROP_TARGET_COUNT; i++) {
        if (s_dropRect[i][2] > s_dropRect[i][0] &&
            tx >= s_dropRect[i][0] - 8.0f && tx <= s_dropRect[i][2] + 8.0f &&
            ty >= s_dropRect[i][1] - 8.0f && ty <= s_dropRect[i][3] + 8.0f)
        {
            return i;
        }
    }
    return -1;
}


// COLLECT page: sub-tab strip, then overview gear-box tap-to-equip.
// Returns true when the tap was consumed.
bool handleCollectTouch(f32 tx, f32 ty) {
    // Library icon row (published by the overview draw only): opens that
    // section as a detail view. Back out is the context tab.
    for (int i = 0; i < 4; i++) {
        if (s_collectIconRects[i][2] > s_collectIconRects[i][0] &&
            tx >= s_collectIconRects[i][0] && tx <= s_collectIconRects[i][2] &&
            ty >= s_collectIconRects[i][1] && ty <= s_collectIconRects[i][3])
        {
            s_collectTab.store(i + 1);
            // Grow the section out of the tapped cell.
            for (int r = 0; r < 4; r++) {
                s_collectZoomFrom[r] = s_collectIconRects[i][r];
            }
            s_collectZoomT = 0.0f;
            s_collectZoomClosing = false;
            // Fresh section: list view, scrolled to the top, selection
            // cleared so the Read tab starts as Back.
            s_readerSel = -1;
            s_readerTapCand = -1;
            s_collectSel = -1;
            s_scrollSkills = 0.0f;
            s_scrollMail = 0.0f;
            s_scrollBody = 0.0f;
            queueSound(Z2SE_SY_MENU_CHANGE_WINDOW, HAPTIC_LIGHT);
            return true;
        }
    }
    // Overview: taps on the gear boxes equip that gear.
    if (s_collectTab.load() == 0 && s_gearBoxS > 0.0f) {
        for (int i = 0; i < 7; i++) {
            if (tx >= s_gearBoxX[i] - 2.0f && tx <= s_gearBoxX[i] + s_gearBoxS + 2.0f &&
                ty >= s_gearBoxY[i] - 2.0f && ty <= s_gearBoxY[i] + s_gearBoxS + 2.0f)
            {
                equipGear(i);
                return true;
            }
        }
    }
    // Section/reader rects. Entry rows (ids >= 0) defer to touch-up so a
    // drag scrolls instead of opening; the reader header's "< Back" (-4)
    // fires immediately, stepping out to the entry list.
    const int ctab = s_collectTab.load();
    if (ctab >= 1 && ctab <= 4) {
        for (int i = 0; i < s_readerRectCount; i++) {
            if (tx >= s_readerRects[i][0] && tx <= s_readerRects[i][2] &&
                ty >= s_readerRects[i][1] && ty <= s_readerRects[i][3])
            {
                const int id = s_readerRectIds[i];
                if (id >= 0) {
                    s_readerTapCand = id;
                    // Remember the row so the detail can pop out of it.
                    for (int r = 0; r < 4; r++) {
                        s_readerTapRect[r] = s_readerRects[i][r];
                    }
                } else if (id == -4) {
                    // Shrink back into its row; drawReaderDetail clears
                    // s_readerSel when the animation lands.
                    s_readerTapCand = -1;
                    s_readerZoomClosing = true;
                    queueSound(Z2SE_SY_CURSOR_CANCEL, HAPTIC_LIGHT);
                }
                return true;
            }
        }
    }
    return false;
}

}  // namespace

// Gear boxes mirror the pause menu's collection screen: two sword boxes
// (Ordon, Master-that-upgrades-to-Light), two shield boxes (Ordon-or-Wooden,
// Hylian), three clothes boxes.
u8 gearItemFor(int idx) {
    switch (idx) {
    case 0:
        return dItemNo_SWORD_e;
    case 1:
        return dComIfGs_isItemFirstBit(dItemNo_LIGHT_SWORD_e) ? dItemNo_LIGHT_SWORD_e
                                                              : dItemNo_MASTER_SWORD_e;
    case 2:
        return dComIfGs_isItemFirstBit(dItemNo_WOOD_SHIELD_e) &&
                       !dComIfGs_isItemFirstBit(dItemNo_SHIELD_e)
                   ? dItemNo_WOOD_SHIELD_e
                   : dItemNo_SHIELD_e;
    case 3:
        return dItemNo_HYLIA_SHIELD_e;
    case 4:
        return dItemNo_WEAR_KOKIRI_e;
    case 5:
        return dItemNo_WEAR_ZORA_e;
    default:
        return dItemNo_ARMOR_e;
    }
}

void handleTouch(f32 w, f32 h) {
    const uint32_t packed = s_pendingTouch.exchange(~0u);
    if (packed == ~0u) {
        return;
    }
    // Cutscene: the tap is consumed (not queued for later) and ignored —
    // nothing on the companion is operable during an event. Gated on the
    // LIVE event state, not the dim level: the dim takes ~10 frames to
    // decay after an event ends, and a tap in that window (tapping
    // Transform right after a Midna dialogue is the classic case) must
    // work, not die against a fading overlay.
    if (dComIfGp_event_runCheck()) {
        return;
    }
    const f32 tx = (f32)(packed >> 16) / 65535.0f * w;
    const f32 ty = (f32)(packed & 0xFFFF) / 65535.0f * h;
    // The combo-or-replace chooser is modal: one of its plates, or cancel.
    if (s_comboChoiceBtn >= 0) {
        const int btn = s_comboChoiceBtn;
        const int slot = s_comboChoiceSlot;
        s_comboChoiceBtn = -1;
        s_comboChoiceSlot = -1;
        if (s_comboChoiceRects[0][2] > s_comboChoiceRects[0][0] &&
            tx >= s_comboChoiceRects[0][0] && tx <= s_comboChoiceRects[0][2] &&
            ty >= s_comboChoiceRects[0][1] && ty <= s_comboChoiceRects[0][3])
        {
            tryBowCombo(btn, slot, dComIfGs_getItem(slot, false));
        } else if (s_comboChoiceRects[1][2] > s_comboChoiceRects[1][0] &&
            tx >= s_comboChoiceRects[1][0] && tx <= s_comboChoiceRects[1][2] &&
            ty >= s_comboChoiceRects[1][1] && ty <= s_comboChoiceRects[1][3])
        {
            plainEquip(btn, slot);
        } else {
            queueSound(Z2SE_SY_CURSOR_CANCEL, HAPTIC_LIGHT);
        }
        return;
    }
    // Corner buttons are tested BEFORE the tab-strip split: the bottom two
    // deliberately sit level with the tab bar, so gating them on
    // "ty < h - TABS_H" would leave only their upper halves tappable.
    // They never overlap the tabs horizontally (the tabs live between the
    // two side columns), so testing them first steals nothing.
    //
    // Wolf/human transform button, and the Functional layout's Z button —
    // the Z press is injected at the pad next frame, so every consumer
    // (Midna, camera, menu Z actions) sees an ordinary Z.
    // Both left corners get a grace margin: they hug the screen edge, where
    // devices shave touchable area (gesture zones, rounded corners), and
    // nothing else lives near them to steal from.
    // FLOOR PICKER FIRST — ahead of the corner buttons below.
    //
    // It is modal: while it is up the left column is hidden behind an opaque
    // slab, so nothing under that slab may be reachable. The corner buttons
    // are hit-tested before everything else (see the note above) and so used
    // to fire straight THROUGH the cover — a button the player cannot see
    // responding to a tap. Testing the picker first closes that hole.
    // A tap on a row picks it; anything else closes the picker and is
    // swallowed rather than falling through.
    // Gated on the ANIMATION, not the open flag: the cover keeps drawing for
    // ~12 frames after the flag clears, and during those frames taps used to
    // reach the corner buttons underneath it — pick a floor, tap top-left 50ms
    // later, and Link transformed into a wolf through an opaque slab.
    if (s_dmapFloorPickOpen || s_dmapFloorPickT > 0.0f) {
        // BACKWARDS: rows overlap while the list is still expanding, and they
        // are painted in ascending order, so the last drawn is on top. A
        // forward scan returned the row *underneath* the one being touched.
        for (int i = s_dmapFloorRectCount - 1; i >= 0; i--) {
            if (tx >= s_dmapFloorRects[i][0] && tx <= s_dmapFloorRects[i][2] &&
                ty >= s_dmapFloorRects[i][1] && ty <= s_dmapFloorRects[i][3])
            {
                s_dmapFloorSel = s_dmapFloorVals[i];
                s_dmapFloorPickOpen = false;
                queueSound(Z2SE_SY_CURSOR_OK, HAPTIC_LIGHT);
                return;
            }
        }
        s_dmapFloorPickOpen = false;
        queueSound(Z2SE_SY_CURSOR_CANCEL, HAPTIC_LIGHT);
        return;
    }

    // The left column's Guide page opens the reader — but NOT from here.
    // Opening on press stole every vertical swipe, so the column could no
    // longer be paged. The open is deferred to release, next to the swipe
    // test, so a drag pages the column and only a still press opens.
    if (!guideIsOpen() && guideAvailable() &&
        s_leftBoxPage.load() == LEFT_BOX_GUIDE && s_leftBoxRect[2] > s_leftBoxRect[0] &&
        tx >= s_leftBoxRect[0] && tx <= s_leftBoxRect[2] && ty >= s_leftBoxRect[1] &&
        ty <= s_leftBoxRect[3])
    {
        return;  // consumed; the release decides swipe vs open
    }
    // The reader is modal only over the CONTENT WINDOW: corner buttons, slots,
    // X/Y and the tab strip are all tested above this point and stay live, so
    // the HUD keeps working while a guide is open.
    if (guideIsOpen() && s_contentRect[2] > s_contentRect[0] &&
        tx >= s_contentRect[0] && tx <= s_contentRect[2] &&
        ty >= s_contentRect[1] && ty <= s_contentRect[3])
    {
        handleGuideTouch(tx, ty);
        return;
    }

    constexpr f32 CORNER_GRACE = 12.0f;
    if (s_transformBtnRect[2] > s_transformBtnRect[0] &&
        tx >= s_transformBtnRect[0] - CORNER_GRACE &&
        tx <= s_transformBtnRect[2] + CORNER_GRACE &&
        ty >= s_transformBtnRect[1] - CORNER_GRACE &&
        ty <= s_transformBtnRect[3] + CORNER_GRACE)
    {
        s_pressAnim[4] = 1.0f;
        queueHaptic(HAPTIC_PRESS);
        s_transformReq.store(true);
        return;
    }
    if (s_zBtnRect[2] > s_zBtnRect[0] && tx >= s_zBtnRect[0] - CORNER_GRACE &&
        tx <= s_zBtnRect[2] + CORNER_GRACE && ty >= s_zBtnRect[1] - CORNER_GRACE &&
        ty <= s_zBtnRect[3] + CORNER_GRACE)
    {
        s_pressAnim[5] = 1.0f;
        s_zPressReq.store(true);
        queueSound(Z2SE_SY_CURSOR_OK, HAPTIC_PRESS);
        return;
    }
    // The I and II item slots: tap fires the bound item through the host
    // button. Equip-mode taps fall through to the release path, which binds
    // the selection instead. Empty slots still swallow the tap.
    for (int i = 0; i < 2; i++) {
        if (s_slotBtnRect[i][2] > s_slotBtnRect[i][0] && tx >= s_slotBtnRect[i][0] &&
            tx <= s_slotBtnRect[i][2] && ty >= s_slotBtnRect[i][1] &&
            ty <= s_slotBtnRect[i][3])
        {
            handleSlotTap(i);
            return;
        }
    }
    // Round X/Y buttons: tap-to-use (hold semantics; equip-mode taps keep
    // their equip meaning via the release path).
    for (int i = 0; i < 2; i++) {
        if (s_fnXYRect[i][2] > s_fnXYRect[i][0] && tx >= s_fnXYRect[i][0] &&
            tx <= s_fnXYRect[i][2] && ty >= s_fnXYRect[i][1] && ty <= s_fnXYRect[i][3])
        {
            handleXYTap(i);
            return;
        }
    }
    // Functional left-column context tab (page-independent, like the corners).
    // The draw and this handler both call contextTabAction, so they agree on
    // what the tab does and whether it is live.
    if (s_ctxTabRect[2] > s_ctxTabRect[0] && tx >= s_ctxTabRect[0] && tx <= s_ctxTabRect[2] &&
        ty >= s_ctxTabRect[1] && ty <= s_ctxTabRect[3])
    {
        bool clickable = false;
        const int action = contextTabAction(&clickable);
        if (!clickable) {
            queueSound(Z2SE_SYS_ERROR, HAPTIC_DENY);
            return;
        }
        switch (action) {
        case CTX_WARP:
            if (isFieldMapScreen()) {
                // Map screen up: act as its Z key; the menu owns the outcome.
                requestWarpToggle();
                queueSound(Z2SE_SY_CURSOR_OK, HAPTIC_LIGHT);
            } else if (warpAllowed()) {
                s_warpReq.store(true);
                queueSound(Z2SE_WARP_MAP_ON, HAPTIC_CONFIRM);
            } else {
                setEquipMsg(150, "%s", txt(STR_NO_WARP_HERE));
                queueSound(Z2SE_SYS_ERROR, HAPTIC_DENY);
            }
            break;
        case CTX_FLOOR:
            // One floor = nothing to choose; leave the button inert rather
            // than opening a list with a single entry in it.
            if (dmapFloorCount() > 1) {
                s_dmapFloorPickOpen = !s_dmapFloorPickOpen;
                queueSound(Z2SE_SY_CURSOR_OK, HAPTIC_LIGHT);
            }
            break;
        case CTX_INFO:
            s_itemInfoSlot = s_selSlot;
            s_scrollItemInfo = 0.0f;
            // Grow out of that item's own cell.
            for (int c = 0; c < s_invCellCount; c++) {
                if (s_invCells[c].slot == s_selSlot) {
                    readerZoomOpenFrom(s_invCells[c].x, s_invCells[c].y,
                        s_invCells[c].x + s_invCell, s_invCells[c].y + s_invCell);
                    break;
                }
            }
            queueSound(Z2SE_SY_CURSOR_OK, HAPTIC_LIGHT);
            break;
        case CTX_HOME:
            // Straight home to the library overview from ANY depth — a open
            // reader closes with its section. Shrink back into the library
            // cell; the dispatch flips the tab to overview when the
            // animation lands. Straight out if there is no recorded cell
            // (page opened by other means, e.g. a wolf slot tap).
            s_readerSel = -1;
            s_readerTapCand = -1;
            s_scrollBody = 0.0f;
            s_readerZoomT = 1.0f;
            s_readerZoomClosing = false;
            s_collectSel = -1;
            s_scrollSkills = 0.0f;
            s_scrollMail = 0.0f;
            if (s_collectZoomFrom[2] > s_collectZoomFrom[0]) {
                s_collectZoomClosing = true;
            } else {
                s_collectTab.store(0);
            }
            queueSound(Z2SE_SY_CURSOR_CANCEL, HAPTIC_LIGHT);
            break;
        case CTX_BACK:
            if (guideIsOpen()) {
                // The reader owns the window, so its Back steps out of a
                // section first and closes the reader second.
                guideBack();
                queueSound(Z2SE_SY_CURSOR_CANCEL, HAPTIC_LIGHT);
                break;
            }
            // ITEMS info reader — COLLECT navigates via CTX_HOME plus the
            // reader header's own Back. Shrinks back into its cell;
            // drawItemInfo clears the slot when the animation lands.
            s_readerZoomClosing = true;
            queueSound(Z2SE_SY_CURSOR_CANCEL, HAPTIC_LIGHT);
            break;
        default:
            break;
        }
        return;
    }
    // Tab strip: hit-test the rects the draw published rather than
    // recomputing the layout, and BEFORE the fixed-height page/strip split —
    // the Functional MAP plate stands 4px taller than TABS_H, and a band test
    // would drop taps on exactly that crown.
    for (int i = 0; i < s_tabRectCount; i++) {
        if (tx >= s_tabRects[i][0] && tx <= s_tabRects[i][2] && ty >= s_tabRects[i][1] &&
            ty <= s_tabRects[i][3])
        {
            // Silent when the tap lands on the page already showing.
            // Picking a page means "show me that", so the reader gets out of
            // the way — otherwise the tab lights up behind a covered window.
            // The GUIDE tab is the exception: there the reader IS the page.
            if (s_tabRectPage[i] == PAGE_GUIDE) {
                guideOpen();
            } else if (guideIsOpen()) {
                guideClose();
            }
            if (s_page.exchange(s_tabRectPage[i]) != s_tabRectPage[i]) {
                queueSound(Z2SE_SY_MENU_CHANGE_WINDOW, HAPTIC_LIGHT);
            }
            return;
        }
    }
    if (ty < h - TABS_H) {
        // ITEMS info reader Back button (only published while reading; the
        // Info trigger itself is the left-column context tab now).
        if (s_page.load() == PAGE_INVENTORY && s_itemInfoSlot >= 0 &&
            s_itemInfoBtnRect[2] > s_itemInfoBtnRect[0] &&
            tx >= s_itemInfoBtnRect[0] && tx <= s_itemInfoBtnRect[2] &&
            ty >= s_itemInfoBtnRect[1] && ty <= s_itemInfoBtnRect[3])
        {
            s_readerZoomClosing = true;  // shrinks back into its cell
            return;
        }
        if (s_pageSliding) {
            // Mid-slide both pages published rects; swallow content taps
            // until the transition lands.
            return;
        }
        if (s_page.load() == PAGE_COLLECTION) {
            handleCollectTouch(tx, ty);
        } else if (s_page.load() == PAGE_MAP) {
            // Warp and Floor are the left-column context tab now (handled
            // above, page-independent). Only the in-window Reset button
            // remains here.
            // Reset-view button (mode-specific view state). Nothing snaps:
            // the glide (ticked on the MAP page in drawDashboard) eases the
            // view home; the dungeon's zoom/center ride its existing
            // room-follow glide, re-engaged here WITHOUT s_dmapResetReq —
            // that flag's snap is for stage init, not for this button.
            if (s_mapResetRect[2] > s_mapResetRect[0] && tx >= s_mapResetRect[0] &&
                tx <= s_mapResetRect[2] && ty >= s_mapResetRect[1] && ty <= s_mapResetRect[3])
            {
                s_mapResetGlide = true;
                if (s_dmapAvailable) {
                    // The room-fit request IS the "supposed" view — the
                    // round-154 glide dropped it and the dungeon stopped
                    // returning to the room-fit zoom. The center still
                    // glides via room-follow; only the zoom snaps.
                    s_dmapResetReq = true;
                    s_dmapFollow = true;
                    s_dmapFloorSel = DMAP_FLOOR_FOLLOW;
                }
            }
        }
        return;
    }
}

// Per-frame drag/tap processing for the ITEMS page (game thread, after the
// page has refreshed its geometry).
void processDragTouch(f32 w, f32 h) {
    const int phase = s_touchPhase.load();
    if (phase == 0) {
        return;
    }
    if (phase == 3) {
        // Pinch took over: drop the single-finger gesture without a tap
        // (including a pending deferred row tap — it must not fire on the
        // next unrelated touch-up, and a left-box swipe must not phantom-flip
        // the carousel on a stray UP after the cancel).
        s_touchPhase.store(0);
        s_touchTracking = false;
        s_dragging = false;
        s_dragSlot = -1;
        s_readerTapCand = -1;
        s_leftBoxSwipe = false;
        s_leftBoxTracking = false;
        // A held touch button releases like a lifted finger.
        releaseHold();
        return;
    }
    const uint32_t packed = s_touchPos.load();
    const f32 tx = (f32)(packed >> 16) / 65535.0f * w;
    const f32 ty = (f32)(packed & 0xFFFF) / 65535.0f * h;
    const bool onItemsPage = s_page.load() == PAGE_INVENTORY && s_invGeomValid &&
        !guideIsOpen();  // the reader covers the grid: a scroll must not equip
    // Panning the map requires the gesture to have STARTED over the map
    // itself. Without that, a swipe anywhere on the companion — the dungeon
    // icon box, the side columns — dragged the map with it.
    // The guide owns the content window while it is up, so the map must not
    // pan, drag or pinch underneath it — that was the reader "sliding the map
    // around" as you scrolled.
    const bool onMapPage = s_page.load() == PAGE_MAP && s_downOnContent &&
        !s_pageSliding && !guideIsOpen();
    // Scrollable list under the finger: the guide reader, skills/mail lists and
    // the open reader body. Vertical drags feed its scroll offset; the
    // draws clamp it.
    f32* scrollVar = NULL;
    // Only when the drag STARTED inside the content window. Swiping the left
    // column or the tab strip is not a scroll of the reader.
    if (guideIsOpen() && s_downOnContent) {
        scrollVar = &s_scrollGuide;
    } else
    if (s_page.load() == PAGE_COLLECTION) {
        const int ctab = s_collectTab.load();
        if (ctab == 3 || ctab == 4) {
            scrollVar = s_readerSel >= 0 ? &s_scrollBody
                                         : (ctab == 3 ? &s_scrollSkills : &s_scrollMail);
        }
    } else if (s_page.load() == PAGE_INVENTORY && s_itemInfoSlot >= 0) {
        scrollVar = &s_scrollItemInfo;
    }

    if (phase == 1) {
        const bool firstFrame = !s_touchTracking;
        if (firstFrame) {
            s_touchTracking = true;
            s_downX = tx;
            s_downY = ty;
            s_dragSlot = -1;
            s_leftBoxSwipe = dusk::dualscreen::mainHudRestored() &&
                s_leftBoxRect[2] > s_leftBoxRect[0] && tx >= s_leftBoxRect[0] &&
                tx <= s_leftBoxRect[2] && ty >= s_leftBoxRect[1] && ty <= s_leftBoxRect[3];
            if (onItemsPage) {
                const int cell = invCellIndexAt(tx, ty);
                if (cell >= 0) {
                    const int slot = s_invCells[cell].slot;
                    if (dComIfGs_getItem(slot, false) != dItemNo_NONE_e) {
                        s_dragSlot = slot;
                    }
                }
            }
        }
        // MAP page: one-finger drag pans the view (finger-follow). Skip the
        // gesture's first frame: s_dragX/Y still hold the previous gesture's
        // last position and would produce a jump.
        if (onMapPage && !firstFrame) {
            // While the reset glide runs, a REAL drag (past the tap slop)
            // takes the view back — but the Reset tap's own 1-3px finger
            // wobble must not: on device it cancelled the glide one frame
            // in, so Reset only ever moved a fraction per tap.
            const f32 gdx = tx - s_downX;
            const f32 gdy = ty - s_downY;
            if (!s_mapResetGlide || gdx * gdx + gdy * gdy > 100.0f) {
                s_mapPanX += tx - s_dragX;
                s_mapPanY += ty - s_dragY;
                s_mapResetGlide = false;
            }
        }
        if (scrollVar != NULL && !firstFrame) {
            *scrollVar -= ty - s_dragY;
        }
        if (s_leftBoxSwipe) {
            // Carousel follow: the box content rides the finger.
            s_leftBoxDragY = ty - s_downY;
            s_leftBoxTracking = true;
        }
        // Floor-picker detents: a light tick whenever the finger crosses
        // into another row of the open floor list, like a selector wheel.
        if (s_dmapFloorPickOpen) {
            static int sLastFloorRow = -1;
            int row = -1;
            for (int i = 0; i < s_dmapFloorRectCount; i++) {
                if (tx >= s_dmapFloorRects[i][0] && tx <= s_dmapFloorRects[i][2] &&
                    ty >= s_dmapFloorRects[i][1] && ty <= s_dmapFloorRects[i][3])
                {
                    row = i;
                    break;
                }
            }
            if (row >= 0 && row != sLastFloorRow) {
                queueHaptic(HAPTIC_LIGHT);
            }
            sLastFloorRow = row;
        }
        s_dragX = tx;
        s_dragY = ty;
        const f32 dx = tx - s_downX;
        const f32 dy = ty - s_downY;
        if (dx * dx + dy * dy > 100.0f) {
            if (s_dragSlot >= 0 && !s_dragging) {
                s_dragging = true;
                // Pluck: the ghost pops slightly large and a light tick
                // marks the item leaving the grid.
                s_ghostPop = 1.0f;
                queueHaptic(HAPTIC_LIGHT);
            }
            // Moved past the slop: this is a scroll, not a row tap.
            s_readerTapCand = -1;
        }
        return;
    }

    // phase == 2: released.
    s_touchPhase.store(0);
    s_touchTracking = false;
    // Release the touch-held button: this is what fires bow-class items.
    // The ghost's host binding is restored a few frames later (drawDashboard)
    // so the release press is consumed with the ghost item still equipped.
    releaseHold();
    // Left-column carousel: a mostly-VERTICAL drag that started in the box
    // pages it — the box is taller than it is wide, so an up/down flick has
    // more room to register than a sideways one. Nothing else lives in that
    // column, so this can consume the gesture outright.
    if (s_leftBoxSwipe) {
        s_leftBoxSwipe = false;
        s_leftBoxTracking = false;
        const f32 sdx = tx - s_downX;
        const f32 sdy = ty - s_downY;
        if (sdy * sdy > 400.0f && sdy * sdy > sdx * sdx) {
            int pages[LEFT_BOX_PAGES];
            const int count = leftBoxPages(pages);
            const int page = s_leftBoxPage.load();
            int idx = 0;
            for (int i = 0; i < count; i++) {
                if (pages[i] == page) {
                    idx = i;
                    break;
                }
            }
            // Swipe up walks forward, matching a scrolling list.
            const int step = sdy < 0.0f ? 1 : count - 1;
            s_leftBoxPage.store(pages[(idx + step) % count]);
            queueSound(Z2SE_SY_CURSOR_FLOOR, HAPTIC_LIGHT);
        } else if (!guideIsOpen() && guideAvailable() &&
            s_leftBoxPage.load() == LEFT_BOX_GUIDE &&
            s_leftBoxRect[2] > s_leftBoxRect[0] && tx >= s_leftBoxRect[0] &&
            tx <= s_leftBoxRect[2] && ty >= s_leftBoxRect[1] && ty <= s_leftBoxRect[3])
        {
            // Not a swipe and it ended on the Guide page: open the reader.
            guideOpen();
            queueSound(Z2SE_SY_CURSOR_OK, HAPTIC_LIGHT);
        }
        s_dragging = false;
        s_dragSlot = -1;
        s_readerTapCand = -1;
        return;
    }
    // Deferred reader-row tap: fires only if the finger stayed within the
    // slop (otherwise the gesture was a scroll). Tapping a row OPENS that
    // entry outright — the context tab is the way home, not the way in, so
    // there is no select-then-confirm step here.
    if (s_readerTapCand >= 0 && guideIsOpen()) {
        // Same deferred path the collect rows use: the tap only lands if the
        // finger stayed inside the slop, so dragging the list scrolls it
        // instead of opening whatever was under the finger when it went down.
        guideRowTap(s_readerTapCand);
        s_readerTapCand = -1;
        s_dragging = false;
        return;
    }
    if (s_readerTapCand >= 0) {
        s_readerSel = s_readerTapCand;
        s_scrollBody = 0.0f;
        readerZoomOpenFrom(s_readerTapRect[0], s_readerTapRect[1], s_readerTapRect[2],
            s_readerTapRect[3]);
        s_readerTapCand = -1;
        s_dragging = false;
        s_dragSlot = -1;
        queueSound(Z2SE_SY_CURSOR_OK, HAPTIC_LIGHT);
        return;
    }
    if (s_dragging) {
        // Cutscene: the tap path swallows input while an event runs — a
        // drag released during one drops without equipping, or the two
        // gestures would disagree about whether events block equips.
        // Live event state, matching handleTouch (not the decaying dim).
        const int dragSlot = s_dragSlot;
        int target = -1;
        bool consumed = false;
        if (onItemsPage && !dComIfGp_event_runCheck()) {
            target = dropTargetAt(tx, ty);
            if (target >= 0) {
                consumed = equipFromCompanion(target, dragSlot);
            }
        }
        // Fly-out: the ghost shrinks into the consumed target, or flies
        // home to its grid cell on a miss/deny — it never just vanishes.
        const u8 flyItem =
            dragSlot >= 0 ? dComIfGs_getItem(dragSlot, false) : (u8)dItemNo_NONE_e;
        if (flyItem != dItemNo_NONE_e) {
            f32 toX = 0.0f, toY = 0.0f;
            bool haveTo = false;
            if (consumed && target >= 0 && s_dropRect[target][2] > s_dropRect[target][0]) {
                toX = (s_dropRect[target][0] + s_dropRect[target][2]) * 0.5f;
                toY = (s_dropRect[target][1] + s_dropRect[target][3]) * 0.5f;
                haveTo = true;
            } else {
                for (int c = 0; c < s_invCellCount; c++) {
                    if (s_invCells[c].slot == dragSlot) {
                        toX = s_invCells[c].x + s_invCell * 0.5f;
                        toY = s_invCells[c].y + s_invCell * 0.5f;
                        haveTo = true;
                        break;
                    }
                }
            }
            if (haveTo) {
                s_ghostFlyItem = flyItem;
                s_ghostFlySlot = dragSlot;
                s_ghostFlyFromX = tx;
                s_ghostFlyFromY = ty;
                s_ghostFlyToX = toX;
                s_ghostFlyToY = toY;
                s_ghostFlyT = 0.0f;
            }
        }
        s_dragging = false;
        s_dragSlot = -1;
        return;
    }
    // Tap (no drag): select an item, or equip the selection via a target tap.
    if (onItemsPage) {
        const int cell = invCellIndexAt(tx, ty);
        if (cell >= 0) {
            const int slot = s_invCells[cell].slot;
            if (dComIfGs_getItem(slot, false) != dItemNo_NONE_e) {
                const bool deselect = slot == s_selSlot;
                s_selSlot = deselect ? -1 : slot;
                queueSound(deselect ? Z2SE_SY_CURSOR_CANCEL : Z2SE_SY_CURSOR_ITEM, HAPTIC_LIGHT);
            }
            s_dragSlot = -1;
            return;
        }
        if (s_selSlot >= 0) {
            const int target = dropTargetAt(tx, ty);
            if (target >= 0) {
                equipFromCompanion(target, s_selSlot);
                s_selSlot = -1;
                return;
            }
        }
    }
    s_dragSlot = -1;
}

}  // namespace dusk::companion
