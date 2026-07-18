#include "dusk/companion.h"
#include "dusk/companion_internal.h"
#include "dusk/dualscreen.h"
#include "dusk/settings.h"
#include <aurora/gfx.h>

#include "imgui.h"

#include "JSystem/J2DGraph/J2DPicture.h"
#include "d/d_com_inf_game.h"
#include "d/d_stage.h"
#include "d/d_menu_window.h"
#include "d/d_menu_fmap.h"
#include "d/d_menu_fmap2D.h"
#include "d/d_menu_dmap.h"
#include "d/d_item_data.h"
#include "d/d_kantera_icon_meter.h"
#include "JSystem/J2DGraph/J2DOrthoGraph.h"
#include "d/d_meter2.h"
#include "d/d_meter2_draw.h"
#include "d/d_meter2_info.h"
#include "d/actor/d_a_player.h"
#include "d/actor/d_a_alink.h"
#include "d/d_meter_string.h"
#include "dolphin/gx/GXAurora.h"

#include <atomic>
#include <cstdio>
#include <cstring>

namespace dusk::companion {

// ---------------------------------------------------------------------------
// Shared state — the single definition site for the externs declared in
// companion_internal.h.

std::atomic<int> s_page{PAGE_MAP};
std::atomic<int> s_collectTab{0};
std::atomic<int> s_questTab{0};

f32 s_gearBoxX[7];
f32 s_gearBoxY[7];
f32 s_gearBoxS = 0.0f;

std::atomic<uint32_t> s_pendingTouch{~0u};
std::atomic<uint32_t> s_touchPos{~0u};
std::atomic<int> s_touchPhase{0};

int s_dragSlot = -1;
bool s_dragging = false;
f32 s_dragX, s_dragY;
int s_selSlot = -1;

int s_equipMsgFrames = 0;
char s_equipMsg[64];

bool s_invGeomValid = false;
f32 s_invCell;
InvCell s_invCells[24];
int s_invCellCount = 0;

bool s_dropRectValid = false;
f32 s_dropRect[2][4];

f32 s_transformBtnRect[4];
std::atomic<bool> s_transformReq{false};

int s_readerSel = -1;
int s_readerTapCand = -1;
int s_readerRectCount = 0;
f32 s_readerRects[12][4];
int s_readerRectIds[12];
f32 s_scrollQuest = 0.0f;
f32 s_scrollSkills = 0.0f;
f32 s_scrollMail = 0.0f;
f32 s_scrollBody = 0.0f;
int s_itemInfoSlot = -1;
f32 s_itemInfoBtnRect[4];
f32 s_scrollItemInfo = 0.0f;

std::atomic<int> s_mapPinchDeltaMilli{0};
f32 s_mapZoom = 1.0f;
f32 s_mapRenderScale = 1.0f;
f32 s_mapPanX = 0.0f;
f32 s_mapPanY = 0.0f;
f32 s_mapViewOffX = 0.0f;
f32 s_mapViewOffZ = 0.0f;
f32 s_mapResetRect[4];

bool s_dmapAvailable = false;
bool s_dmapReady = false;
u32 s_dmapGen = 0;
f32 s_dmapViewCx = 0.0f;
f32 s_dmapViewCz = 0.0f;
f32 s_dmapCmPerTexel = 0.0f;
int s_dmapViewFloor = 0;

int s_dmapFloorSel = DMAP_FLOOR_FOLLOW;
f32 s_dmapZoom = 1.0f;
f32 s_dmapOffX = 0.0f;
f32 s_dmapOffZ = 0.0f;
bool s_dmapFollow = true;
bool s_dmapResetReq = true;
bool s_dmapFloorPickOpen = false;
f32 s_dmapFloorBtnRect[4];
f32 s_dmapFloorRects[13][4];
int s_dmapFloorVals[13];
int s_dmapFloorRectCount = 0;
u16 s_dmapFloorAvail = 0;
int s_dmapBossFloor = DMAP_FLOOR_FOLLOW;
DmapIcon s_dmapIcons[96];
int s_dmapIconCount = 0;

f32 s_pixelScale = 1.0f;
u32 s_nativeW = 0;
u32 s_nativeH = 0;

// ---------------------------------------------------------------------------
// Dashboard-private helpers.

namespace {

const char* l_tabNames[PAGE_COUNT] = {"MAP", "ITEMS", "COLLECT", "QUEST"};

// X/Y drop-target button centers, published alongside s_dropRect.
f32 s_dropBtnPos[2][2];
std::atomic<int> s_batteryPct{-1};
std::atomic<bool> s_batteryCharging{false};

// --- Hearts (pixel-drawn; the heart item icon resolves to a wrong texture) --

// Lantern oil: the game's own kantera meter (icon + radial gauge, live fill
// state), repositioned to the right end of the hearts strip. Hidden on the
// main screen in dual-screen mode.
// Lantern icon + oil meter, centered vertically on cy, tight spacing, right
// edge at rightX. Both hidden unless the lantern is equipped to X or Y
// (matching the game HUD). Returns the left extent of what was drawn, or
// rightX unchanged when hidden.
f32 drawOilGauge(f32 rightX, f32 cy) {
    dMeter2Draw_c* md = meterDraw();
    if (md == NULL) {
        return rightX;
    }
    int slot = -1;
    if (dComIfGp_getSelectItem(0) == dItemNo_KANTERA_e) {
        slot = 0;
    } else if (dComIfGp_getSelectItem(1) == dItemNo_KANTERA_e) {
        slot = 1;
    }
    if (slot < 0) {
        return rightX;
    }
    // Order: oil meter, then lantern icon (left to right).
    const f32 icon = 24.0f;
    const f32 iconX = rightX - icon;
    const f32 meterCX = iconX - 18.0f;
    dKantera_icon_c* meter = md->getKanteraMeter(slot);
    if (meter != NULL) {
        meter->setScale(1.2f, 1.2f);
        meter->setPos(meterCX, cy);
        meter->setNowGauge(dComIfGs_getMaxOil(), dComIfGs_getOil());
        // Pin fully opaque: the game's fade-in after stage transitions left
        // it invisible for a while.
        meter->setAlphaRate(1.0f);
        meter->drawSelf();
    }
    drawItemIcon(ICON_SLOT_LANTERN, dItemNo_KANTERA_e, iconX, cy - icon * 0.5f, icon);
    dComIfGp_getCurrentGrafPort()->setup2D();
    return meterCX - 26.0f;
}

void drawHeartsRow(f32 x0, f32 x1) {
    const int life = dComIfGs_getLife();          // quarter hearts
    const int maxHearts = dComIfGs_getMaxLife() / 5;  // max life counts in 5s
    if (maxHearts <= 0) {
        return;
    }
    // Two rows of up to 10 hearts, like the game HUD.
    const int perRow = maxHearts > 10 ? 10 : maxHearts;
    const int rows = maxHearts > 10 ? 2 : 1;
    f32 size = rows == 2 ? 13.0f : 17.0f;
    f32 gap = rows == 2 ? 2.0f : 4.0f;
    const f32 availW = x1 - x0 - 8.0f;
    if (perRow * (size + gap) > availW) {
        size = availW / perRow - gap;
    }
    dMeter2Draw_c* md = meterDraw();
    bool usedOriginalAsset = false;
    for (int i = 0; i < maxHearts && i < 20; i++) {
        const int row = i / 10;
        const int col = i % 10;
        const f32 x = x0 + 4.0f + col * (size + gap);
        const f32 y = rows == 2 ? 2.0f + row * (size + 2.0f)
                                : (HEARTS_H - size) * 0.5f + 3.0f;
        // Prefer the game's own heart panes: base + fill composite reflecting
        // the live state, quarter hearts included.
        J2DPicture* heartPics[2];
        const int picCount = md != NULL ? md->getHeartPictures(i, heartPics) : 0;
        for (int j = 0; j < picCount; j++) {
            heartPics[j]->draw(x, y, size, size, false, false, false);
        }
        if (picCount > 0) {
            usedOriginalAsset = true;
        }
    }
    if (usedOriginalAsset) {
        dComIfGp_getCurrentGrafPort()->setup2D();
    }
}

// --- Frame ---------------------------------------------------------------

// Dungeon items (map, compass, boss key) between tabs and battery.
void drawDungeonIcons(f32 x, f32 y) {
    // Only meaningful inside dungeons — same visibility rule as the game HUD.
    stage_stag_info_class* stagInfo = dComIfGp_getStage()->getStagInfo();
    if (stagInfo == NULL || dStage_stagInfo_GetSTType(stagInfo) != ST_DUNGEON) {
        return;
    }
    // Top to bottom: boss key, compass, map. Obtained items get a yellow
    // silhouette border; missing ones are nearly black ghosts.
    const f32 icon = 33.0f;
    const f32 step = icon + 5.0f;
    const struct {
        int slot;
        u8 itemNo;
        bool owned;
    } items[3] = {
        {ICON_SLOT_BOSSKEY, dItemNo_BOSS_KEY_e, dComIfGs_isDungeonItemBossKey() != 0},
        {ICON_SLOT_COMPASS, dItemNo_COMPUS_e, dComIfGs_isDungeonItemCompass() != 0},
        {ICON_SLOT_DMAP, dItemNo_MAP_e, dComIfGs_isDungeonItemMap() != 0},
    };
    for (int i = 0; i < 3; i++) {
        const f32 iy = y + step * (f32)i;
        if (items[i].owned) {
            drawItemIconSilhouette(items[i].slot, items[i].itemNo, x - 3.0f, iy - 3.0f,
                icon + 6.0f, 0xF0D060FFu);
            drawItemIcon(items[i].slot, items[i].itemNo, x, iy, icon, 0xFF);
        } else {
            drawItemIcon(items[i].slot, items[i].itemNo, x, iy, icon, 55);
        }
    }
}

// Equip drop targets, drawn late so they sit on top of the content window:
// [currently equipped item] on the left, the X/Y button (letter visible, no
// item overlay) on the right. X is blue-tinted, Y green-tinted.
void drawEquipTargets() {
    if (!s_dropRectValid) {
        return;
    }
    dMeter2Draw_c* md = meterDraw();
    if (md == NULL) {
        return;
    }
    static const GXColor l_dropBg[2] = {{34, 44, 66, 240}, {32, 56, 42, 240}};
    static const GXColor l_dropBorder[2] = {{110, 145, 210, 255}, {105, 190, 130, 255}};
    constexpr GXColor COL_DROP_HOT = {235, 200, 90, 255};
    for (int i = 0; i < 2; i++) {
        const f32 rx0 = s_dropRect[i][0];
        const f32 ry0 = s_dropRect[i][1];
        const f32 rx1 = s_dropRect[i][2];
        const f32 ry1 = s_dropRect[i][3];
        const f32 rh = ry1 - ry0;
        const bool hot = s_dragging && s_dragX >= rx0 - 6.0f && s_dragX <= rx1 + 6.0f &&
            s_dragY >= ry0 - 6.0f && s_dragY <= ry1 + 6.0f;
        fillRect(rx0 - 3.0f, ry0 - 3.0f, rx1 + 3.0f, ry1 + 3.0f,
            hot ? COL_DROP_HOT : l_dropBorder[i]);
        fillRect(rx0, ry0, rx1, ry1, l_dropBg[i]);
        const u8 equipped = dComIfGp_getSelectItem(i);
        if (equipped != dItemNo_NONE_e) {
            const f32 eq = rh - 8.0f;
            drawItemIcon(ICON_SLOT_XITEM + i, equipped, rx0 + 6.0f,
                ry0 + (rh - eq) * 0.5f, eq);
        }
        // The button itself at its real diamond position, letter
        // unobstructed (no item overlay).
        constexpr f32 btn = 38.0f;
        drawButtonCircleBase(md, 2 + i, s_dropBtnPos[i][0], s_dropBtnPos[i][1], btn);
        drawPaneComposite(md->getButtonPane(2 + i), s_dropBtnPos[i][0], s_dropBtnPos[i][1],
            btn, btn, 0, false, true);
    }
}

// Equipped items in the game HUD's diagonal cluster arrangement, drawn over
// the top-right of the content area (no boxes).

constexpr f32 CLUSTER_BTN = 38.0f;

// B shows the equipped sword when no special B item overrides it (fishing
// rod etc.) — dynamic across wooden/Ordon/Master/Light. Hidden in menus
// (start screen etc.), like the game HUD.
u8 resolveBItem(bool menuOpen) {
    if (menuOpen) {
        return dItemNo_NONE_e;
    }
    u8 bItem = dComIfGs_getBButtonItemKey();
    if (bItem == dItemNo_NONE_e || bItem == 0xFF) {
        bItem = dComIfGs_getSelectEquipSword();
    }
    return bItem;
}

// Equip mode: the drop targets are rendered later by drawEquipTargets as
// two stacked rectangles at the right edge (no overlap) — publish the rect
// anchored at (and centered on) the diamond button. The X/Y separation
// keeps the lanes disjoint.
void publishEquipDropRect(int dropIdx, f32 x, f32 y) {
    s_dropRect[dropIdx][0] = x - 56.0f;
    s_dropRect[dropIdx][1] = y - 1.0f;
    s_dropRect[dropIdx][2] = x + CLUSTER_BTN + 4.0f;
    s_dropRect[dropIdx][3] = y + 39.0f;
    s_dropBtnPos[dropIdx][0] = x;
    s_dropBtnPos[dropIdx][1] = y;
    s_dropRectValid = true;
}

// Ammo count inside the button box, bottom-left, on a dark chip.
void drawButtonAmmoChip(int ammo, f32 x, f32 y) {
    constexpr GXColor COL_CHIP = {10, 9, 7, 210};
    const f32 chipW = (ammo >= 100 ? 3.0f : ammo >= 10 ? 2.0f : 1.0f) * 8.0f + 5.0f;
    fillRect(x + 1.0f, y + CLUSTER_BTN - 15.0f, x + 1.0f + chipW, y + CLUSTER_BTN - 1.0f,
        COL_CHIP);
    drawHudNumber(ammo, x + 3.0f, y + CLUSTER_BTN - 13.0f, 10.0f);
}

// Menu prompt words. In menus the HUD words go stale ("Put away"); A/B swap
// to the localized menu prompts (Confirm / Back), loaded once and cached.
// Map screens instead mirror the LIVE prompt strings from the map's own
// layout (NULL/empty when the main screen hides them).
char s_menuAWord[64];
char s_menuBWord[64];
char s_mapZoomWord[64];

void fetchMenuPromptWords() {
    if (s_menuAWord[0] != 0) {
        return;
    }
    dMeter2Info_getString(0x40C, s_menuAWord, NULL);
    dMeter2Info_getString(0x3F9, s_menuBWord, NULL);
    dMeter2Info_getString(0x524, s_mapZoomWord, NULL);
}

void mapPromptWords(int winStatus, const char** o_a, const char** o_b) {
    *o_a = NULL;
    *o_b = NULL;
    dMw_c* mw = dMeter2Info_getMenuWindowClass();
    if (mw == NULL) {
        return;
    }
    if (winStatus == 4 && mw->getMenuFmap() != NULL &&
        mw->getMenuFmap()->getDraw2DTop() != NULL)
    {
        *o_a = mw->getMenuFmap()->getDraw2DTop()->getButtonLabel(0);
        *o_b = mw->getMenuFmap()->getDraw2DTop()->getButtonLabel(1);
    } else if (winStatus == 5 && mw->getMenuDmap() != NULL &&
               mw->getMenuDmap()->getDrawBg() != NULL)
    {
        *o_a = mw->getMenuDmap()->getDrawBg()->getButtonLabel(0);
        *o_b = mw->getMenuDmap()->getDrawBg()->getButtonLabel(1);
    }
}

// Z button beside X: circular grey base (like X/Y) with the Midna prompt
// over it — full alpha when active, dimmed otherwise — plus the prompt's
// pulse light at this position.
void drawMidnaButton(dMeter2Draw_c* md, f32 zx, f32 zy) {
    drawPaneComposite(md->getButtonPane(4), zx, zy, CLUSTER_BTN, CLUSTER_BTN);
    J2DPane* midnaPane = md->getMidnaButtonPaneRaw();
    if (midnaPane == NULL) {
        return;
    }
    const bool midnaActive = midnaPane->getAlpha() != 0;
    drawPaneComposite(midnaPane, zx, zy, CLUSTER_BTN, CLUSTER_BTN,
        midnaActive ? (u8)0 : (u8)100);
    if (midnaActive) {
        md->drawMidnaPikariAt(zx + CLUSTER_BTN * 0.5f, zy + CLUSTER_BTN * 0.5f);
        dComIfGp_getCurrentGrafPort()->setup2D();
    }
}

void drawItemCluster(f32 x1, f32 y0) {
    // Button diamond matching the device — X top, Y left, A right, B bottom,
    // Z (Midna) beside X — each drawn by compositing the real HUD button
    // subtree (base, ring, letter) with the equipped item overlaid.
    dMeter2Draw_c* md = meterDraw();
    if (md == NULL || !md->isButtonClusterVisible()) {
        return;
    }

    constexpr f32 BTN = CLUSTER_BTN;
    constexpr f32 ICON = 44.0f;
    struct Entry {
        int paneIdx;  // getButtonPane index
        int slot;
        u8 itemNo;
        f32 dx, dy;
    };
    // Wolf form: items are unusable, so X/Y show nothing — matching the
    // game HUD, which swaps to the wolf button set.
    daPy_py_c* player = daPy_getPlayerActorClass();
    const bool wolf = player != NULL && player->checkWolf();

    // Tight diamond: X top, Y left, B bottom, A right — side buttons pulled in.
    // Menu state first: several elements below hide while a menu is up.
    const int winStatus = dMeter2Info_getWindowStatus();
    const bool menuOpen = dComIfGp_isPauseFlag() || winStatus != 0;
    const u8 bItem = resolveBItem(menuOpen);
    // X raised and Y lowered so each sits vertically centered in its equip
    // lane; B (and A) pushed down to keep the diamond proportional.
    const Entry entries[] = {
        {2, ICON_SLOT_XITEM, wolf ? (u8)dItemNo_NONE_e : dComIfGp_getSelectItem(0), -92.0f, -4.0f},
        {3, ICON_SLOT_YITEM, wolf ? (u8)dItemNo_NONE_e : dComIfGp_getSelectItem(1), -126.0f, 42.0f},
        {1, ICON_SLOT_BITEM, wolf ? (u8)dItemNo_NONE_e : bItem, -92.0f, 86.0f},
    };
    // The game hides X/Y whenever a menu window is up — except the item
    // wheel, where X/Y are the equip targets and stay visible.
    const bool xyHidden = menuOpen && winStatus != 2;
    for (const Entry& e : entries) {
        const f32 x = x1 + e.dx;
        const f32 y = y0 + e.dy;
        if (e.paneIdx == 2 || e.paneIdx == 3) {
            if (xyHidden) {
                continue;
            }
            // Equip mode: skip the diamond button, publish the drop rect for
            // drawEquipTargets and the touch hit tests instead.
            const bool equipMode = s_page.load() == PAGE_INVENTORY && s_itemInfoSlot < 0 &&
                (s_dragging || s_selSlot >= 0) && !wolf && !menuOpen;
            if (equipMode) {
                publishEquipDropRect(e.paneIdx - 2, x, y);
                continue;
            }
            // X/Y: circular base like A/B, keeping their grey gradient; the
            // pill plate in the subtree is skipped, letters/overlays kept.
            drawButtonCircleBase(md, e.paneIdx, x, y, BTN);
            drawPaneComposite(md->getButtonPane(e.paneIdx), x, y, BTN, BTN, 0, false, true);
        } else {
            drawPaneComposite(md->getButtonPane(e.paneIdx), x, y, BTN, BTN);
        }
        if (e.itemNo != dItemNo_NONE_e) {
            drawItemIcon(e.slot, e.itemNo, x + (BTN - ICON) * 0.5f, y + (BTN - ICON) * 0.5f,
                ICON);
            const int xy = e.paneIdx == 2 ? 0 : e.paneIdx == 3 ? 1 : -1;
            const int ammo = ammoForItem(e.itemNo, xy);
            if (ammo >= 0) {
                drawButtonAmmoChip(ammo, x, y);
            }
        }
    }

    // Contextual action words under their buttons: B ("Attack") plus the
    // wolf-form X ("Sense") and Y ("Dig") texts. Hidden, like the buttons,
    // while any menu window is up.
    struct ActionText {
        const char* text;
        f32 bx, by;
    };
    if (menuOpen) {
        fetchMenuPromptWords();
    }
    const bool mapMenu = winStatus == 4 || winStatus == 5;
    const char* mapAWord = NULL;
    const char* mapBWord = NULL;
    if (mapMenu) {
        mapPromptWords(winStatus, &mapAWord, &mapBWord);
    }
    const ActionText actionTexts[] = {
        {menuOpen ? (mapMenu ? mapBWord : s_menuBWord) : md->getActionTextB(),
            x1 - 92.0f, y0 + 86.0f + BTN + 10.0f},
        {menuOpen ? NULL : md->getActionTextXY(0), x1 - 92.0f, y0 - 10.0f},
        {menuOpen ? NULL : md->getActionTextXY(1), x1 - 126.0f, y0 + 42.0f + BTN + 10.0f},
    };
    for (const ActionText& at : actionTexts) {
        if (at.text == NULL || strlen(at.text) == 0) {
            continue;
        }
        drawTextCentered(at.bx + BTN * 0.5f, at.by, 12.0f, TEXT_ACCENT, at.text);
    }

    drawMidnaButton(md, x1 - 46.0f, y0 - 12.0f);

    // A button, right of center (same row as Y), pulled 20% of the Y-A
    // span toward Y, with action word below.
    const f32 ax = x1 - BTN - 18.0f;
    const f32 ay = y0 + 42.0f;
    drawPaneComposite(md->getButtonPane(0), ax, ay, BTN, BTN);

    // Action word ("Speak", "Blow", ...) centered under the A button.
    const char* action = menuOpen ? (mapMenu ? mapAWord : s_menuAWord) : md->getActionTextA();
    if (action != NULL && strlen(action) > 0) {
        drawTextCentered(ax + BTN * 0.5f, ay + BTN + 10.0f, 12.0f, TEXT_ACCENT, action);
    }
    if (mapMenu && s_mapZoomWord[0] != 0) {
        // Map screens: zoom prompt (stick) under the cluster.
        drawTextCentered(x1 - 76.0f, y0 + 86.0f + BTN + 34.0f, 12.0f, TEXT_DIM, s_mapZoomWord);
    }
}

void drawTabs(f32 x0, f32 x1, f32 h) {
    constexpr f32 GAP = 3.0f;
    const f32 tabW = (x1 - x0 - GAP * (PAGE_COUNT - 1)) / PAGE_COUNT;
    const f32 top = h - TABS_H;
    const int page = s_page.load();
    for (int i = 0; i < PAGE_COUNT; i++) {
        const f32 x = x0 + i * (tabW + GAP);
        // The collection screen's Save/Options button plate.
        drawTabPlate(x, top + 4.0f, tabW, h - 6.0f - (top + 4.0f), i == page);
        drawTextCentered(x + tabW * 0.5f, h - 18.0f, 15.0f,
            i == page ? TEXT_TAB_ACTIVE : TEXT_DIM, l_tabNames[i]);
    }
}

// Device battery, right corner of the tab strip.
// Battery glyph at (x, y) with the percentage to its RIGHT on the same row.
void drawBattery(f32 x, f32 y) {
    const int pct = s_batteryPct.load();
    if (pct < 0) {
        return;
    }
    const bool charging = s_batteryCharging.load();
    const f32 bw = 26.0f;
    const f32 bh = 12.0f;
    constexpr GXColor COL_SHELL = {150, 156, 172, 255};
    constexpr GXColor COL_WELL = {18, 21, 30, 255};
    fillRect(x - 1.0f, y - 1.0f, x + bw + 1.0f, y + bh + 1.0f, COL_SHELL);
    fillRect(x, y, x + bw, y + bh, COL_WELL);
    fillRect(x + bw + 1.0f, y + 3.0f, x + bw + 4.0f, y + bh - 3.0f, COL_SHELL);
    GXColor fillColor = {150, 214, 120, 255};
    if (charging) {
        fillColor = {120, 190, 240, 255};
    } else if (pct <= 20) {
        fillColor = {224, 80, 64, 255};
    }
    const f32 fillW = (bw - 4.0f) * (f32)pct / 100.0f;
    if (fillW >= 1.0f) {
        fillRect(x + 2.0f, y + 2.0f, x + 2.0f + fillW, y + bh - 2.0f, fillColor);
    }
    drawText(x + bw + 10.0f, y + bh * 0.5f + 5.0f, 13.0f, TEXT_MAIN, "%d%%", pct);
}

// The pause menu's stone-block backdrop, tiled and tinted down to the same
// smoky dark warm grey the game fades it to.
void drawBackdrop(f32 w, f32 h) {
    fillRect(0.0f, 0.0f, w, h, COL_BG);
    const ResTIMG* blocks = decoTimg(DECO_BLOCKS);
    if (blocks == NULL) {
        return;
    }
    const f32 tile = 172.0f;
    for (f32 ty = 0.0f; ty < h; ty += tile) {
        for (f32 tx = 0.0f; tx < w; tx += tile) {
            drawTimgTinted(blocks, tx, ty, tile, tile, 0xFF, 0x1B1B1AFFu, 0x605D57FFu);
        }
    }
}

// Top bar: hearts left; [oil meter + lantern] [keys] [rupees] on the right.
// The right side flows right-to-left from the rupee art's true left edge, so
// absent groups leave no dead space.
void drawTopBar(dMeter2Draw_c* md, f32 w, f32 x1) {
    constexpr GXColor COL_SCRIM = {11, 10, 8, 170};
    constexpr f32 GAP = 12.0f;
    fillRect(0.0f, 0.0f, w, HEARTS_H + 6.0f, COL_SCRIM);
    // Soft gold separator under the bar, from the menu's own line art.
    if (const ResTIMG* line = decoTimg(DECO_LINE)) {
        drawTimgTinted(line, 0.0f, HEARTS_H + 2.0f, w, 5.0f, 150, 0x00000000u, 0xA89C74FFu);
    }
    f32 cursor = x1;
    if (md != NULL) {
        // dropPlate: the counter's backdrop blob extends well past the gem and
        // would otherwise hold the visible art away from the right edge.
        f32 map[5] = {};
        drawPaneComposite(md->getCounterPane(0), x1 - 80.0f, 4.0f, 78.0f, 24.0f, 0, true, false,
            map, true);
        cursor = map[4] > 0.0f ? map[0] : x1 - 80.0f;
        // Small keys, by the game's own display rule (dMeter2_c::isKeyVisible):
        // stages flagged for key display show the counter — in dungeons even at
        // zero — fields only with keys in hand. Drawn as icon + HUD digits
        // because every key_n pane is hidden at zero, so the pane composite can
        // never render a "0". (The pane's J2D visible flag is useless as a gate:
        // the game fades the counter via alphaRate and never toggles it.)
        stage_stag_info_class* stagInfo = dComIfGp_getStage()->getStagInfo();
        const s16 keyNum = dComIfGs_getKeyNum();
        if (stagInfo != NULL && dStage_stagInfo_ChkKeyDisp(stagInfo) &&
            (dStage_stagInfo_GetSTType(stagInfo) != ST_FIELD || keyNum != 0))
        {
            // Count on the left of the key icon, matching the rupee counter's
            // digits-then-gem order.
            const f32 icon = 24.0f;
            const f32 digitH = 16.0f;
            const f32 digitW = digitH * 0.72f;
            const f32 numW = keyNum >= 10 ? digitW * 1.9f : digitW;
            const f32 bx = cursor - GAP - (icon + 3.0f + numW);
            drawHudNumber(keyNum, bx, (HEARTS_H + 6.0f - digitH) * 0.5f, digitH);
            drawItemIcon(ICON_SLOT_KEY, dItemNo_SMALL_KEY_e, bx + numW + 3.0f,
                (HEARTS_H + 6.0f - icon) * 0.5f, icon);
            cursor = bx;
        }
        if (dComIfGs_getMaxOil() > 0) {
            const f32 oilRight = cursor - GAP;
            const f32 oilLeft = drawOilGauge(oilRight, HEARTS_H * 0.5f + 2.0f);
            if (oilLeft < oilRight) {
                cursor = oilLeft;
            }
        }
    }
    drawHeartsRow(8.0f, cursor - 4.0f);
}

// Map each tear pane's center through the vessel composite's affine
// (root-relative units, same accumulation as collectPaneLayers — root's own
// origin included). Unmappable tears stay below -9000. Returns whether any
// tear mapped.
bool computeTearPositions(dMeter2Draw_c* md, J2DPane* vesselRoot, const f32 vmap[5],
    f32* tearX, f32* tearY) {
    bool any = false;
    for (int i = 0; i < 16; i++) {
        tearX[i] = -10000.0f;
        tearY[i] = -10000.0f;
        J2DPane* tear = md->getVesselTearPane(i);
        if (tear == NULL) {
            continue;
        }
        f32 rx = 0.0f;
        f32 ry = 0.0f;
        J2DPane* p = tear;
        while (p != NULL) {
            const JGeometry::TBox2<f32>& b = p->getBounds();
            rx += b.i.x;
            ry += b.i.y;
            if (p == vesselRoot) {
                break;
            }
            p = p->getParentPane();
        }
        if (p == NULL) {
            continue;
        }
        const JGeometry::TBox2<f32>& tb = tear->getBounds();
        rx += (tb.f.x - tb.i.x) * 0.5f;
        ry += (tb.f.y - tb.i.y) * 0.5f;
        tearX[i] = vmap[0] + (rx - vmap[2]) * vmap[4];
        tearY[i] = vmap[1] + (ry - vmap[3]) * vmap[4];
        any = true;
    }
    return any;
}

// Vessel of Light: full main-screen-style vessel composite, shown for the
// entire tears quest (any active flag state, plus the fade alpha as a
// fallback so it never misses). Right-aligned, below the B button/action
// text, filling down to the battery row — the lit tears show the progress.
// The tear glow sparkles are direct overlays in the game, so the composite
// doesn't carry them; they are re-drawn at the mapped tear positions.
void drawVesselOfLight(dMeter2Draw_c* md, f32 x1, f32 h) {
    const s8 darkArea = dComIfGp_getStartStageDarkArea();
    const u8 dropFlag = darkArea >= 0 ? dMeter2Info_getLightDropGetFlag((u8)darkArea) : 0;
    const bool tearsQuest = dropFlag != 0 && dropFlag != 0xFF;
    if (md == NULL || darkArea < 0 || (!tearsQuest && md->getLightDropAlpha() <= 0.0f)) {
        return;
    }
    md->refreshVesselForCompanion();
    const f32 tY = HEARTS_H + 186.0f;
    const f32 tB = h - TABS_H - 6.0f;
    f32 vmap[5] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    drawPaneComposite(md->getLightDropPane(), x1 - 150.0f, tY, 150.0f, tB - tY, 0, true,
        false, vmap);
    J2DPane* vesselRoot = md->getLightDropPane();
    if (vmap[4] > 0.0f && vesselRoot != NULL) {
        f32 tearX[16];
        f32 tearY[16];
        if (computeTearPositions(md, vesselRoot, vmap, tearX, tearY)) {
            md->drawVesselPikariForCompanion(tearX, tearY, vmap[4]);
        }
    }
}

// Windowed content between the top bar and tabs: page dispatch. The panel
// uses the Link-display box's own mottled background (TT_YAKUSHIMA) with a
// thin frame line, like the collection screen.
void drawContentWindow(f32 w, f32 cy0, f32 cy1) {
    constexpr GXColor COL_WINDOW = {24, 24, 22, 248};
    constexpr GXColor COL_FRAME = {108, 102, 90, 255};
    const f32 wx0 = 12.0f;
    const f32 wx1 = w - 158.0f;
    fillRect(wx0, cy0, wx1, cy1, COL_WINDOW);
    if (const ResTIMG* bg = decoTimg(DECO_YAKUSHIMA)) {
        drawTimgTinted(bg, wx0 + 1.5f, cy0 + 1.5f, wx1 - wx0 - 3.0f, cy1 - cy0 - 3.0f, 160,
            0x0C0B0AFFu, 0x1F1D19FFu);
    }
    fillRect(wx0, cy0, wx1, cy0 + 1.5f, COL_FRAME);
    fillRect(wx0, cy1 - 1.5f, wx1, cy1, COL_FRAME);
    fillRect(wx0, cy0, wx0 + 1.5f, cy1, COL_FRAME);
    fillRect(wx1 - 1.5f, cy0, wx1, cy1, COL_FRAME);
    // Clip the page content to the window.
    GXSetScissorRender((u32)(wx0 * s_pixelScale), (u32)(cy0 * s_pixelScale),
        (u32)((wx1 - wx0) * s_pixelScale), (u32)((cy1 - cy0) * s_pixelScale));
    switch (s_page.load()) {
    case PAGE_INVENTORY:
        drawInventoryContent(16.0f, cy0 + 8.0f, w - 160.0f, cy1 - 8.0f);
        break;
    case PAGE_QUEST:
        drawQuestContent(16.0f, cy0 + 8.0f, w - 160.0f, cy1 - 8.0f);
        break;
    case PAGE_COLLECTION:
        drawCollectionContent(16.0f, cy0 + 8.0f, w - 160.0f, cy1 - 8.0f);
        break;
    case PAGE_MAP:
    default:
        drawMapContent(16.0f, cy0 + 4.0f, w - 162.0f, cy1 - 4.0f);
        break;
    }
    GXSetScissorRender(0, 0, s_nativeW, s_nativeH);
}

// D-pad cross HUD above the FPS/battery corner: the game's pad is assembled
// from rotated corner pieces the compositor can't reproduce (they render as
// loose hearts/corners), so draw a clean glyph plus the game's own
// localized labels (up = items, right = map). Hidden, like the buttons,
// while any menu window is up.
void drawDpadGlyph(dMeter2Draw_c* md, f32 x1, f32 h) {
    const bool menuOpen = dComIfGp_isPauseFlag() || dMeter2Info_getWindowStatus() != 0;
    if (menuOpen || md == NULL || md->getButtonCrossPane() == NULL) {
        return;
    }
    const f32 dbox = 23.0f;
    const f32 dx = x1 - 130.0f;
    const f32 dy = h - TABS_H - dbox - 8.0f;
    constexpr GXColor COL_DPAD = {126, 116, 96, 255};
    constexpr GXColor COL_DPAD_HI = {198, 184, 152, 255};
    const f32 arm = dbox / 3.0f;
    fillRect(dx + arm, dy, dx + 2.0f * arm, dy + dbox, COL_DPAD);
    fillRect(dx, dy + arm, dx + dbox, dy + 2.0f * arm, COL_DPAD);
    fillRect(dx + arm + 2.0f, dy + 2.0f, dx + 2.0f * arm - 2.0f, dy + arm, COL_DPAD_HI);
    fillRect(dx + 2.0f * arm, dy + arm + 2.0f, dx + dbox - 2.0f, dy + 2.0f * arm - 2.0f,
        COL_DPAD_HI);
    const char* upLabel = md->getDpadLabel(0);
    const char* rightLabel = md->getDpadLabel(1);
    if (upLabel != NULL && upLabel[0] != 0) {
        drawTextCentered(dx + dbox * 0.5f, dy - 4.0f, 10.0f, TEXT_MAIN, upLabel);
    }
    if (rightLabel != NULL && rightLabel[0] != 0) {
        drawText(dx + dbox + 5.0f, dy + dbox * 0.5f + 4.0f, 10.0f, TEXT_MAIN, "%s",
            rightLabel);
    }
}

// Wolf/human quick-transform button above the d-pad: a menu tab plate
// reading [current form] > [target form] in small pause-map portraits.
// Hidden until the shadow crystal is obtained (M_077) and while a menu
// window is up, like the d-pad; dimmed when the transform is currently
// blocked (airborne, cutscene, NPCs nearby...). A tap while dimmed still
// sends the request — the game answers with its error beep.
void drawTransformButton(f32 x1, f32 h) {
    const bool menuOpen = dComIfGp_isPauseFlag() || dMeter2Info_getWindowStatus() != 0;
    daAlink_c* alink = daAlink_getAlinkActorClass();
    if (menuOpen || alink == NULL || !dComIfGs_isEventBit(dSv_event_flag_c::M_077)) {
        return;
    }
    const bool wolf = daPy_py_c::checkNowWolf();
    const ResTIMG* faceCur = dmapFloorFaceTimg(wolf);
    const ResTIMG* faceTgt = dmapFloorFaceTimg(!wolf);
    if (faceCur == NULL || faceTgt == NULL) {
        return;
    }
    // Portraits keep the source 40x41 aspect.
    const f32 icon = 24.0f;
    const f32 iconH = 24.6f;
    const f32 arrowW = 12.0f;
    const f32 pad = 5.0f;
    const f32 gap = 3.0f;
    const f32 bw = pad * 2.0f + icon * 2.0f + gap * 2.0f + arrowW;
    const f32 bh = 34.0f;
    // Bottom edge clears the d-pad glyph plus its up-label.
    const f32 bx = x1 - 142.0f;
    const f32 by = h - TABS_H - 87.0f;
    drawTabPlate(bx, by, bw, bh, true);
    const f32 iy = by + (bh - iconH) * 0.5f;
    drawTimg(faceCur, bx + pad, iy, icon, iconH, 0xFF);
    drawTextCentered(bx + pad + icon + gap + arrowW * 0.5f, by + bh * 0.5f + 5.0f, 14.0f,
        TEXT_TAB_ACTIVE, ">");
    drawTimg(faceTgt, bx + pad + icon + gap + arrowW + gap, iy, icon, iconH, 0xFF);
    if (!alink->checkQuickTransformOK()) {
        constexpr GXColor COL_DIM = {0, 0, 0, 150};
        fillRect(bx, by, bx + bw, by + bh, COL_DIM);
    }
    s_transformBtnRect[0] = bx;
    s_transformBtnRect[1] = by;
    s_transformBtnRect[2] = bx + bw;
    s_transformBtnRect[3] = by + bh;
}

// Bottom-right row: [FPS] [battery glyph] [pct] — FPS only when the video
// setting "Show FPS Counter" is set (dual-screen auto-selects Companion).
void drawStatusCorner(f32 x1, f32 h) {
    // Vertically centered on the tab bar's label line (baseline h - 18):
    // the battery glyph is 12 tall, its pct text baseline sits at y + 11.
    const f32 batY = h - 29.0f;
    const f32 batX = x1 - 66.0f;
    drawBattery(batX, batY);
    if (getSettings().video.enableFpsOverlay.getValue()) {
        const int fps = (int)(aurora_get_fps() + 0.5f);
        GXColor fpsCol = {150, 214, 120, 255};
        if (fps < 30) fpsCol = {224, 80, 64, 255};
        else if (fps < 55) fpsCol = {244, 186, 84, 255};
        const u32 fpsRgba =
            (fpsCol.r << 24) | (fpsCol.g << 16) | (fpsCol.b << 8) | fpsCol.a;
        drawText(batX - 64.0f, batY + 11.0f, 13.0f, fpsRgba, "%d FPS", fps);
    }
}

// Drag ghost: the item follows the finger with a thick orange border.
void drawDragGhost() {
    if (!s_dragging || s_dragSlot < 0) {
        return;
    }
    const u8 dragItem = dComIfGs_getItem(s_dragSlot, false);
    if (dragItem == dItemNo_NONE_e) {
        return;
    }
    const f32 g = 52.0f;
    const f32 gx = s_dragX - g * 0.5f;
    const f32 gy = s_dragY - g * 0.5f;
    drawItemIconSilhouette(s_dragSlot, dragItem, gx - 4.0f, gy - 4.0f, g + 8.0f, 0xECD054FFu);
    drawItemIcon(s_dragSlot, dragItem, gx, gy, g);
}

}  // namespace

// --- Public API ----------------------------------------------------------

void drawSplash(f32 w, f32 h) {
    drawBackdrop(w, h);
    const ResTIMG* logo = dusklightLogoTimg();
    if (logo != NULL) {
        const f32 s = 128.0f;
        drawTimg(logo, (w - s) * 0.5f, (h - s) * 0.5f, s, s, 0xFF);
    }
}


void setNativeCanvas(unsigned width, unsigned height, float scale) {
    s_nativeW = width;
    s_nativeH = height;
    s_pixelScale = scale;
}

// (Re)apply the full-texture viewport/scissor via the aurora passthrough
// commands — J2D setPort only knows logical units.
void applyNativeViewport() {
    if (s_nativeW != 0 && s_nativeH != 0) {
        GXSetViewportRender(0.0f, 0.0f, (f32)s_nativeW, (f32)s_nativeH, 0.0f, 1.0f);
        GXSetScissorRender(0, 0, s_nativeW, s_nativeH);
    }
}

bool hudReady() {
    return meterDraw() != NULL;
}

void update() {
    if (ImGui::IsKeyPressed(ImGuiKey_F9, false)) {
        nextPage();
    }
}

void nextPage() {
    // CAS loop: called from both the Android UI thread and the game thread.
    int page = s_page.load();
    while (!s_page.compare_exchange_weak(page, (page + 1) % PAGE_COUNT)) {
    }
}

void setBatteryStatus(int percent, bool charging) {
    s_batteryPct.store(percent > 100 ? 100 : percent);
    s_batteryCharging.store(charging);
}

void touchEvent(int action, float u, float v) {
    if (u < 0.0f) {
        u = 0.0f;
    }
    if (u > 1.0f) {
        u = 1.0f;
    }
    if (v < 0.0f) {
        v = 0.0f;
    }
    if (v > 1.0f) {
        v = 1.0f;
    }
    const uint32_t packed = ((uint32_t)(u * 65535.0f) << 16) | (uint32_t)(v * 65535.0f);
    s_touchPos.store(packed);
    if (action == 0) {
        s_touchPhase.store(1);
        // Taps still feed the legacy single-point path (tabs etc.).
        s_pendingTouch.store(packed);
    } else if (action == 2) {
        s_touchPhase.store(2);
    } else if (action == 3) {
        // Cancel (a pinch took over): drop the gesture without a tap.
        s_touchPhase.store(3);
        s_pendingTouch.store(~0u);
    }
}

void pinchZoom(float factor) {
    if (factor > 0.2f && factor < 5.0f) {
        s_mapPinchDeltaMilli.fetch_add((int)((factor - 1.0f) * 1000.0f));
    }
}

bool consumeTransformRequest() {
    return s_transformReq.exchange(false);
}

bool mapViewAdjust(float* o_x, float* o_z, float* o_texelScale) {
    if (!dualscreen::hudOnCompanion() ||
        (s_mapViewOffX == 0.0f && s_mapViewOffZ == 0.0f && s_mapRenderScale == 1.0f)) {
        return false;
    }
    *o_x = s_mapViewOffX;
    *o_z = s_mapViewOffZ;
    *o_texelScale = s_mapRenderScale;
    return true;
}

namespace {

// The game's contextual control panel (grass melody, sumo, fishing controls)
// replaces the button cluster during special actions. It lays itself out in
// main-screen units, so draw it under a 608-wide ortho mapped to our canvas.
void drawSpecialPanel(f32 w, f32 h) {
    dMeter2_c* meter = dMeter2Info_getMeterClass();
    if (meter == NULL) {
        return;
    }
    // Horse spur/stamina meter (type 1), crawling arrows (type 2) and the
    // scope overlay (type 4) draw on the main screen instead.
    if (meter->getSubContents() == 1 || meter->getSubContents() == 2 ||
        meter->getSubContents() == 4)
    {
        return;
    }
    dMeterSub_c* sub = meter->getSubContentsDlst();
    dMeterString_c* subSub = meter->getSubSubContentsDlst();
    if (sub == NULL && subSub == NULL) {
        return;
    }
    J2DGrafContext* prevPort = dComIfGp_getCurrentGrafPort();
    J2DOrthoGraph ortho(0.0f, 0.0f, 608.0f, 608.0f * h / w, -1.0f, 1.0f);
    dComIfGp_setCurrentGrafPort(&ortho);
    ortho.setPort();
    if (sub != NULL) {
        sub->draw();
    }
    if (subSub != NULL &&
        (meter->getSubContents() != 5 || meter->getSubContentsStringType() != 0))
    {
        subSub->draw();
    }
    dComIfGp_setCurrentGrafPort((J2DOrthoGraph*)prevPort);
    if (prevPort != NULL) {
        prevPort->setPort();
    }
    applyNativeViewport();
}

}  // namespace

void drawDashboard(float w, float h) {
    // Undo any idle fade the game applied this frame: hearts, rupee counter
    // and buttons live only on this screen and must stay visible.
    if (dMeter2Draw_c* mdAlpha = meterDraw()) {
        mdAlpha->forceCompanionAlpha();
    }
    loadCollectIcons();
    // The equip feedback line counts down here (once per frame) so a page
    // switch can't freeze a stale warning for a later replay.
    if (s_equipMsgFrames > 0) {
        s_equipMsgFrames--;
    }
    dMeter2Draw_c* md = meterDraw();
    const f32 x1 = w - 6.0f;

    drawBackdrop(w, h);
    drawTopBar(md, w, x1);

    // Controller cluster, top-right (also publishes the equip drop rects —
    // drawEquipTargets and the touch pass read them later this frame).
    s_dropRectValid = false;
    drawItemCluster(x1, HEARTS_H + 42.0f);
    drawSpecialPanel(w, h);

    // Below the controller cluster: tears of light OR dungeon icons — they
    // never appear at the same time.
    drawDungeonIcons(x1 - 44.0f, h - TABS_H - 124.0f);
    drawVesselOfLight(md, x1, h);

    const f32 cy0 = HEARTS_H + 8.0f;
    const f32 cy1 = h - TABS_H - 6.0f;
    drawContentWindow(w, cy0, cy1);

    // Tab bar below the content window.
    constexpr GXColor COL_SCRIM = {11, 10, 8, 170};
    fillRect(12.0f, h - TABS_H, w - 158.0f, h, COL_SCRIM);
    drawTabs(12.0f, w - 158.0f, h);

    // Equip drop targets on top of everything in the cluster margin.
    drawEquipTargets();

    drawDpadGlyph(md, x1, h);
    s_transformBtnRect[0] = 0.0f;
    s_transformBtnRect[2] = 0.0f;
    drawTransformButton(x1, h);
    drawStatusCorner(x1, h);

    // Touch runs LAST so it hit-tests against the geometry this frame's
    // draw just published.
    handleTouch(w, h, 12.0f, w - 158.0f);
    processDragTouch(w, h);
    drawDragGhost();

    // Geometry statics go stale when the page changes; an in-flight item
    // drag must not survive onto another page either.
    if (s_page.load() != PAGE_INVENTORY) {
        s_invGeomValid = false;
        s_selSlot = -1;
        s_dragging = false;
        s_dragSlot = -1;
    }
    // Pinches only mean something on the MAP page — drop them elsewhere so
    // they don't burst into the map view later.
    if (s_page.load() != PAGE_MAP) {
        s_mapPinchDeltaMilli.exchange(0);
    }
}

}  // namespace dusk::companion
