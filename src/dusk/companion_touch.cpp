// Companion dashboard: second-screen touch input. Tab/page taps arrive as
// one-shot normalized points (s_pendingTouch); drag & drop equipping
// consumes the live DOWN/MOVE/UP stream (s_touchPos/s_touchPhase). Both
// run at the END of drawDashboard so they hit-test against the geometry
// this frame's draw just published (s_invCells, s_gearBox*, s_dropRect).

#include "dusk/companion.h"
#include "dusk/companion_internal.h"

#include "d/d_com_inf_game.h"
#include "d/d_item_data.h"
#include "d/d_meter2_info.h"
#include "d/actor/d_a_player.h"
#include "d/actor/d_a_alink.h"

#include <cstdarg>
#include <cstdio>

namespace dusk::companion {
namespace {

// Drag gesture tracking.
bool s_touchTracking = false;
f32 s_downX, s_downY;

// Post the transient equip feedback line (drawn by the active page).
void setEquipMsg(int frames, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(s_equipMsg, sizeof(s_equipMsg), fmt, args);
    va_end(args);
    s_equipMsgFrames = frames;
}

// Bow combos. Dropping a bomb bag / hawkeye onto a button that holds the
// bow (plain or already comboed) arms bomb/hawk arrows — the same state the
// item wheel writes (select = bomb slot, mix = bow slot). Dropping the bow
// onto its own combo button turns the combo off. Returns true when the drop
// was consumed as a combo action.
bool tryBowCombo(int btn, int slot, u8 itemNo) {
    const bool comboPartner = itemNo == dItemNo_NORMAL_BOMB_e ||
        itemNo == dItemNo_WATER_BOMB_e || itemNo == dItemNo_POKE_BOMB_e ||
        itemNo == dItemNo_HAWK_EYE_e;
    const bool btnHasBow = dComIfGs_getSelectItemIndex(btn) == SLOT_4 ||
        dComIfGs_getMixItemIndex(btn) == SLOT_4;
    if (comboPartner && btnHasBow) {
        // The bow can only ever mix on one button.
        const int other = 1 - btn;
        if (dComIfGs_getMixItemIndex(other) == SLOT_4) {
            dComIfGs_setMixItemIndex(other, dItemNo_NONE_e);
            dComIfGs_setSelectItemIndex(other, SLOT_4);
        }
        // If the combo partner sits on the other button, clear it there.
        if (dComIfGs_getSelectItemIndex(other) == slot) {
            dComIfGs_setMixItemIndex(other, dItemNo_NONE_e);
            dComIfGs_setSelectItemIndex(other, dItemNo_NONE_e);
        }
        dComIfGs_setMixItemIndex(btn, SLOT_4);
        dComIfGs_setSelectItemIndex(btn, (u8)slot);
        setEquipMsg(120, "%s",
            itemNo == dItemNo_HAWK_EYE_e ? "Hawkeye + Bow combo!" : "Bomb Arrow combo!");
        return true;
    }
    if (itemNo == dItemNo_BOW_e && dComIfGs_getMixItemIndex(btn) == SLOT_4) {
        dComIfGs_setMixItemIndex(btn, dItemNo_NONE_e);
        dComIfGs_setSelectItemIndex(btn, SLOT_4);
        setEquipMsg(120, "Combo off");
        return true;
    }
    return false;
}

void equipFromCompanion(int btn, int slot) {
    if (dComIfGp_isPauseFlag() || dMeter2Info_getWindowStatus() != 0) {
        setEquipMsg(150, "Can't equip while a menu is open");
        return;
    }
    if (daPy_getPlayerActorClass() != NULL && daPy_getPlayerActorClass()->checkWolf()) {
        setEquipMsg(150, "Can't equip items as a wolf");
        return;
    }
    if (slot < 0 || slot >= MAX_ITEM_SLOTS ||
        dComIfGs_getItem(slot, false) == dItemNo_NONE_e)
    {
        return;
    }
    const u8 itemNo = dComIfGs_getItem(slot, false);
    if (tryBowCombo(btn, slot, itemNo)) {
        return;
    }

    // Plain equip. Clear any stale mix on this button first: leaving
    // mix = bow behind would make it keep resolving to the bow.
    dComIfGs_setMixItemIndex(btn, dItemNo_NONE_e);
    // If the item is on the other button, swap the two assignments
    // (clearing that button's mix too — its combo can't survive a swap).
    const int other = 1 - btn;
    if (dComIfGs_getSelectItemIndex(other) == slot) {
        dComIfGs_setMixItemIndex(other, dItemNo_NONE_e);
        dComIfGs_setSelectItemIndex(other, dComIfGs_getSelectItemIndex(btn));
    }
    dComIfGs_setSelectItemIndex(btn, (u8)slot);
    s_equipMsgFrames = 0;
}

// Gear changes are blocked in menus, as a wolf, and while a previous model
// swap is still reloading. Posts the reason when it is user-visible.
bool gearChangeBlocked(daPy_py_c* player) {
    if (dComIfGp_isPauseFlag() || dMeter2Info_getWindowStatus() != 0) {
        setEquipMsg(150, "Can't change gear while a menu is open");
        return true;
    }
    if (player == NULL) {
        return true;
    }
    if (player->checkWolf()) {
        setEquipMsg(150, "Can't change gear as a wolf");
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
    setEquipMsg(120, "Equipped %s", s_gearName[0] != 0 ? s_gearName : "gear");
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

// X/Y drop-target index under (tx, ty) with an 8px grab margin, -1 when none.
int dropTargetAt(f32 tx, f32 ty) {
    for (int i = 0; s_dropRectValid && i < 2; i++) {
        if (tx >= s_dropRect[i][0] - 8.0f && tx <= s_dropRect[i][2] + 8.0f &&
            ty >= s_dropRect[i][1] - 8.0f && ty <= s_dropRect[i][3] + 8.0f)
        {
            return i;
        }
    }
    return -1;
}

// QUEST page: category sub-tab taps (the list itself drag-scrolls).
// Returns true when the tap was consumed.
bool handleQuestTouch(f32 tx, f32 ty, f32 x1) {
    const f32 cy0 = HEARTS_H + 8.0f;
    const f32 sx0 = 16.0f;
    const f32 sx1 = x1 - 2.0f;
    if (tx < sx0 || tx > sx1) {
        return false;
    }
    if (ty >= cy0 + 8.0f && ty <= cy0 + 48.0f) {
        const f32 qx0 = sx0 + 12.0f;
        const f32 qx1 = sx1 - 12.0f;
        const f32 qtabW = (qx1 - qx0 - 8.0f) / 5.0f;
        for (int i = 0; i < 5; i++) {
            const f32 x = qx0 + i * (qtabW + 2.0f);
            if (tx >= x && tx <= x + qtabW) {
                s_questTab.store(i);
                s_scrollQuest = 0.0f;
                return true;
            }
        }
        return false;
    }
    return false;
}

// COLLECT page: sub-tab strip, then overview gear-box tap-to-equip.
// Returns true when the tap was consumed.
bool handleCollectTouch(f32 tx, f32 ty, f32 x1) {
    const f32 cy0 = HEARTS_H + 8.0f;
    const f32 sx0 = 16.0f;
    const f32 sx1 = x1 - 2.0f;
    if (ty >= cy0 + 8.0f && ty <= cy0 + 8.0f + CTAB_H && tx >= sx0 && tx <= sx1) {
        const f32 cx0 = sx0 + 12.0f;
        const f32 cx1 = sx1 - 12.0f;
        const f32 ctabW = (cx1 - cx0 - CTAB_GAP * (COLLECT_TAB_COUNT - 1)) / COLLECT_TAB_COUNT;
        for (int i = 0; i < COLLECT_TAB_COUNT; i++) {
            const f32 x = cx0 + i * (ctabW + CTAB_GAP);
            if (tx >= x && tx <= x + ctabW) {
                s_collectTab.store(i);
                // Fresh tab: back to its list view, scrolled to the top.
                s_readerSel = -1;
                s_readerTapCand = -1;
                s_scrollSkills = 0.0f;
                s_scrollMail = 0.0f;
                s_scrollBody = 0.0f;
                return true;
            }
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
    // Skills / Mail reader rects. Rows (ids >= 0) defer to touch-up so a
    // drag scrolls instead of opening; Back (-2) fires immediately.
    const int ctab = s_collectTab.load();
    if (ctab == 3 || ctab == 4) {
        for (int i = 0; i < s_readerRectCount; i++) {
            if (tx >= s_readerRects[i][0] && tx <= s_readerRects[i][2] &&
                ty >= s_readerRects[i][1] && ty <= s_readerRects[i][3])
            {
                const int id = s_readerRectIds[i];
                if (id >= 0) {
                    s_readerTapCand = id;
                } else if (id == -2) {
                    s_readerSel = -1;
                    s_readerTapCand = -1;
                    s_scrollBody = 0.0f;
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

void handleTouch(f32 w, f32 h, f32 x0, f32 x1) {
    const uint32_t packed = s_pendingTouch.exchange(~0u);
    if (packed == ~0u) {
        return;
    }
    const f32 tx = (f32)(packed >> 16) / 65535.0f * w;
    const f32 ty = (f32)(packed & 0xFFFF) / 65535.0f * h;
    if (ty < h - TABS_H) {
        // Wolf/human transform button (right panel, above the d-pad). Sits
        // outside the content window, so it can't shadow any page rect.
        if (s_transformBtnRect[2] > s_transformBtnRect[0] &&
            tx >= s_transformBtnRect[0] && tx <= s_transformBtnRect[2] &&
            ty >= s_transformBtnRect[1] && ty <= s_transformBtnRect[3])
        {
            s_transformReq.store(true);
            return;
        }
        // ITEMS: the Info button (grid view, item selected) / Back (info
        // view) share one rect.
        if (s_page.load() == PAGE_INVENTORY &&
            s_itemInfoBtnRect[2] > s_itemInfoBtnRect[0] &&
            tx >= s_itemInfoBtnRect[0] && tx <= s_itemInfoBtnRect[2] &&
            ty >= s_itemInfoBtnRect[1] && ty <= s_itemInfoBtnRect[3])
        {
            if (s_itemInfoSlot >= 0) {
                s_itemInfoSlot = -1;
            } else if (s_selSlot >= 0) {
                s_itemInfoSlot = s_selSlot;
            }
            s_scrollItemInfo = 0.0f;
            return;
        }
        if (s_page.load() == PAGE_QUEST) {
            handleQuestTouch(tx, ty, x1);
        } else if (s_page.load() == PAGE_COLLECTION) {
            handleCollectTouch(tx, ty, x1);
        } else if (s_page.load() == PAGE_MAP) {
            // Floor button toggles the pop-up list (dungeons only).
            if (s_dmapFloorBtnRect[2] > s_dmapFloorBtnRect[0] &&
                tx >= s_dmapFloorBtnRect[0] && tx <= s_dmapFloorBtnRect[2] &&
                ty >= s_dmapFloorBtnRect[1] && ty <= s_dmapFloorBtnRect[3])
            {
                s_dmapFloorPickOpen = !s_dmapFloorPickOpen;
                return;
            }
            for (int i = 0; i < s_dmapFloorRectCount; i++) {
                if (tx >= s_dmapFloorRects[i][0] && tx <= s_dmapFloorRects[i][2] &&
                    ty >= s_dmapFloorRects[i][1] && ty <= s_dmapFloorRects[i][3])
                {
                    s_dmapFloorSel = s_dmapFloorVals[i];
                    s_dmapFloorPickOpen = false;
                    return;
                }
            }
            // Any other tap closes the pop-up.
            s_dmapFloorPickOpen = false;
            // Reset-view button (mode-specific view state).
            if (s_mapResetRect[2] > s_mapResetRect[0] && tx >= s_mapResetRect[0] &&
                tx <= s_mapResetRect[2] && ty >= s_mapResetRect[1] && ty <= s_mapResetRect[3])
            {
                s_mapPanX = 0.0f;
                s_mapPanY = 0.0f;
                if (s_dmapAvailable) {
                    // Back to the room-view default; the center slides there.
                    s_dmapResetReq = true;
                    s_dmapFloorSel = DMAP_FLOOR_FOLLOW;
                } else {
                    s_mapZoom = 1.0f;
                    s_mapViewOffX = 0.0f;
                    s_mapViewOffZ = 0.0f;
                }
            }
        }
        return;
    }
    constexpr f32 GAP = 3.0f;
    const f32 tabW = (x1 - x0 - GAP * (PAGE_COUNT - 1)) / PAGE_COUNT;
    for (int i = 0; i < PAGE_COUNT; i++) {
        const f32 x = x0 + i * (tabW + GAP);
        if (tx >= x && tx <= x + tabW) {
            s_page.store(i);
            return;
        }
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
        // next unrelated touch-up).
        s_touchPhase.store(0);
        s_touchTracking = false;
        s_dragging = false;
        s_dragSlot = -1;
        s_readerTapCand = -1;
        return;
    }
    const uint32_t packed = s_touchPos.load();
    const f32 tx = (f32)(packed >> 16) / 65535.0f * w;
    const f32 ty = (f32)(packed & 0xFFFF) / 65535.0f * h;
    const bool onItemsPage = s_page.load() == PAGE_INVENTORY && s_invGeomValid;
    const bool onMapPage = s_page.load() == PAGE_MAP;
    // Scrollable list under the finger: quest table, skills/mail lists and
    // the open reader body. Vertical drags feed its scroll offset; the
    // draws clamp it.
    f32* scrollVar = NULL;
    if (s_page.load() == PAGE_QUEST) {
        scrollVar = &s_scrollQuest;
    } else if (s_page.load() == PAGE_COLLECTION) {
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
            s_mapPanX += tx - s_dragX;
            s_mapPanY += ty - s_dragY;
        }
        if (scrollVar != NULL && !firstFrame) {
            *scrollVar -= ty - s_dragY;
        }
        s_dragX = tx;
        s_dragY = ty;
        const f32 dx = tx - s_downX;
        const f32 dy = ty - s_downY;
        if (dx * dx + dy * dy > 100.0f) {
            if (s_dragSlot >= 0) {
                s_dragging = true;
            }
            // Moved past the slop: this is a scroll, not a row tap.
            s_readerTapCand = -1;
        }
        return;
    }

    // phase == 2: released.
    s_touchPhase.store(0);
    s_touchTracking = false;
    // Deferred reader-row tap: fires only if the finger stayed within the
    // slop (otherwise the gesture was a scroll).
    if (s_readerTapCand >= 0) {
        s_readerSel = s_readerTapCand;
        s_scrollBody = 0.0f;
        s_readerTapCand = -1;
        s_dragging = false;
        s_dragSlot = -1;
        return;
    }
    if (s_dragging) {
        if (onItemsPage) {
            const int target = dropTargetAt(tx, ty);
            if (target >= 0) {
                equipFromCompanion(target, s_dragSlot);
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
                s_selSlot = slot == s_selSlot ? -1 : slot;
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
