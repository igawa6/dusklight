// Companion dashboard: the Functional ("3DS style") layout — the left column
// of status boxes and the four corner controls.
//
// Split out of companion.cpp, which had grown past 3700 lines carrying both
// HUD layouts at once. The two are selected by dualscreen::mainHudRestored()
// and share nothing but the primitives in companion_gfx.cpp and the state in
// companion_internal.h, so they are genuinely separate surfaces.

#include "dusk/companion.h"
#include "dusk/companion_internal.h"
#include "dusk/companion_strings.h"
#include "dusk/dualscreen.h"
#include "dusk/settings.h"

#include "JSystem/J2DGraph/J2DPicture.h"
#include "JSystem/J2DGraph/J2DOrthoGraph.h"
#include "d/d_com_inf_game.h"
#include "d/d_kankyo.h"
#include "d/d_stage.h"
#include "d/d_menu_window.h"
#include "d/d_item_data.h"
#include "d/d_kantera_icon_meter.h"
#include "d/d_meter2.h"
#include "d/d_meter2_draw.h"
#include "d/d_meter2_info.h"
#include "d/actor/d_a_player.h"
#include "d/actor/d_a_alink.h"
#include "dolphin/gx/GXAurora.h"
#include "m_Do/m_Do_audio.h"

#include <cstring>

#include "d/d_menu_fmap.h"
#include "d/d_menu_dmap_map.h"
#include "d/d_menu_fmap2D.h"

#include <cstdio>

namespace dusk::companion {

// English fallbacks; tabName() prefers the game's own localized screen
// titles (verified on the PAL disc: 0x3E0 "Field Map", 0x3E8 "Item
// Selection", 0x3E1 "Collection"). GUIDE is a Dusklight page with no game
// equivalent, so it stays English until a translation table exists.
const char* l_tabNames[PAGE_COUNT] = {"MAP", "ITEMS", "COLLECTION", "GUIDE"};

const char* tabName(int page) {
    // 0x0062/0x0061 are the game's OWN short uppercase "MAP"/"ITEMS";
    // 0x03E1 is "Collection" (mixed case in the archive, uppercased here so
    // every language matches the tab strip's uppercase house style).
    static const u16 l_msg[PAGE_COUNT] = {0x0062, 0x0061, 0x03E1, 0};
    if (page < 0 || page >= PAGE_COUNT) {
        return "";
    }
    // English keeps the dashboard's own uppercase tab words. The archive's
    // 0x0062/0x0061 are literally "MAP"/"ITEMS" already, so those two would
    // match anyway; 0x03E1 is mixed-case "Collection", and we want the
    // uppercase house style. Other languages take the game's wording.
    return localizedWord(l_msg[page], l_tabNames[page], true);
}

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
    // Equipped on any of the four item buttons (slots I/II included). The
    // kantera icon meters only exist for X/Y, so slot equips borrow meter 0.
    int slot = -1;
    for (int b = 0; b < 4; b++) {
        if (dComIfGp_getSelectItem(b) == dItemNo_KANTERA_e) {
            slot = b < 2 ? b : 0;
            break;
        }
    }
    if (slot < 0) {
        return rightX;
    }
    // No oil gauge in wolf form — the wolf can't touch the lantern (see
    // drawMeterBar's matching gate).
    if (companionWolf()) {
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
            // These are the GAME's live panes. The explicit-rect draw runs
            // makeMatrix(), which overwrites the pane's own mPositionMtx with
            // our companion coordinates — and the main screen's hierarchical
            // draw CONSUMES that matrix rather than recomputing it
            // (J2DPane::draw: MTXConcat(parent->mGlobalMtx, mPositionMtx,
            // ...)). Leaving it clobbered showed up as displaced duplicate
            // hearts the moment the HUD moved back to the main screen. Save
            // and restore it around the draw.
            Mtx saved;
            MTXCopy(*heartPics[j]->getMtx(), saved);
            // No mulDrawAlpha here: drawHeartsRow is only reached from the
            // top bar, which is chrome outside the faded content window, so
            // it would be the identity — and these are the GAME's own live
            // panes, so writing alpha back onto them is not free.
            heartPics[j]->draw(x, y, size, size, false, false, false);
            heartPics[j]->setMtx(saved);
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
        const bool hot = dragOverDropRect(i);
        fillRect(rx0 - 3.0f, ry0 - 3.0f, rx1 + 3.0f, ry1 + 3.0f,
            hot ? COL_DROP_HOT : l_dropBorder[i]);
        fillRect(rx0, ry0, rx1, ry1, l_dropBg[i]);
        const u8 equipped = dComIfGp_getSelectItem(i);
        if (equipped != dItemNo_NONE_e) {
            const f32 eq = rh - 8.0f;
            drawItemIcon(ICON_SLOT_XITEM + i, equipped, rx0 + 6.0f,
                ry0 + (rh - eq) * 0.5f, eq);
        }
        // The button itself at its real position, letter unobstructed (no
        // item overlay). Size comes from whichever layout published it.
        const f32 btn = s_dropBtnSize;
        drawButtonCircleBase(md, 2 + i, s_dropBtnPos[i][0], s_dropBtnPos[i][1], btn);
        drawPaneComposite(md->getButtonPane(2 + i), s_dropBtnPos[i][0], s_dropBtnPos[i][1],
            btn, btn, 0, false, true);
    }
}

// Equipped items in the game HUD's diagonal cluster arrangement, drawn over
// the top-right of the content area (no boxes).


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
    // Empty base button until she is actually with Link (see midnaAvailable).
    if (midnaPane == NULL || !midnaAvailable()) {
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
    const bool wolf = companionWolf();

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

// Animated per-page tab raise (Functional): the SELECTED tab stands 8px
// taller than its neighbours; the raise eases in and out at the render rate
// so a page change reads as the plates trading height, not snapping.
f32 tabRaise(int i_page, bool i_active) {
    static f32 s_raise[PAGE_COUNT];
    if (i_page < 0 || i_page >= PAGE_COUNT) {
        return 0.0f;
    }
    const f32 target = i_active ? 8.0f : 0.0f;
    s_raise[i_page] += (target - s_raise[i_page]) * ANIM_RATE_GLIDE;
    if (s_raise[i_page] < 0.05f) {
        s_raise[i_page] = 0.0f;
    } else if (s_raise[i_page] > 7.95f) {
        s_raise[i_page] = 8.0f;
    }
    return s_raise[i_page];
}

// The window's bottom-left flourish, drawn by the tab strip so it sits over
// the strip's bed but under its plates (Functional only — see
// drawWindowOrnaments).
void drawBottomFlourish(f32 x0, f32 y1) {
    const ResTIMG* kaz = decoTimg(DECO_KAZARI);
    if (kaz == NULL) {
        return;
    }
    constexpr u32 INK = 0x00000000u;
    constexpr u32 ORN = 0xB6A886FFu;
    const f32 k = 40.0f;
    const f32 kh = k * (f32)(u16)kaz->height / (f32)(u16)kaz->width;
    drawTimgTintedMirror(kaz, x0 + 3.0f, y1 - kh - 3.0f + kh * 0.75f, k, kh, 210, INK, ORN,
        false, true);
}

// Draws the main tab strip between x0 and x1, with its bottom edge at h, and
// publishes each plate's rect for the touch pass. Nothing else may recompute
// this geometry: the two layouts show different tab counts, and a second copy
// of the maths would drift out of sync with the drawn plates.
void drawTabs(f32 x0, f32 x1, f32 h) {
    constexpr f32 GAP = 3.0f;
    int pages[TAB_RECT_MAX];
    const int count = visiblePages(pages);
    const f32 tabW = (x1 - x0 - GAP * (count - 1)) / count;
    const f32 top = h - TABS_H;
    const int page = s_page.load();
    // Functional dresses the strip as part of the content window: a dark bed
    // carrying the window's own fill and frame, with the tabs cut into its top
    // edge. Cinematic keeps the plain plates on a scrim.
    const bool functional = dualscreen::mainHudRestored();
    if (functional) {
        // Starts above the window's bottom rule and paints over it, so the
        // window opens downward into the strip instead of closing off.
        const f32 bedY0 = top - 8.0f;
        fillRect(x0, bedY0, x1, h, COL_WINDOW);
        // Same inset, width and colour as the content window's side rules
        // (drawWindowOrnaments) so the strip continues that line.
        fillRect(x0 + 0.5f, bedY0, x0 + 2.5f, h, COL_SIDE_RULE);
        fillRect(x1 - 2.5f, bedY0, x1 - 0.5f, h, COL_SIDE_RULE);
        // Over the bed, under the plates below.
        if (s_contentRect[3] > s_contentRect[1]) {
            drawBottomFlourish(x0, s_contentRect[3]);
        }
        // No rule along the bottom: the strip runs off the screen edge so the
        // tabs read as part of it rather than as a closed panel.
    }
    s_tabRectCount = 0;
    // One size for the whole strip, driven by the longest label. Fitting each
    // tab independently left a long localized name (ES "COLECCION") noticeably
    // smaller than its neighbours in the same row.
    f32 labelTS = 15.0f;
    for (int i = 0; i < count; i++) {
        const f32 ts = fittedTextSize(15.0f, 10.0f, tabW - 12.0f, tabName(pages[i]));
        if (ts < labelTS) {
            labelTS = ts;
        }
    }
    for (int i = 0; i < count; i++) {
        const f32 x = x0 + i * (tabW + GAP);
        // The guide is its own context window: while it is up none of the
        // page tabs is the thing being shown, so none of them is selected.
        const bool active = pages[i] == page && !guideIsOpen();
        f32 ty0 = top + 4.0f;
        // Functional runs the plates to the screen edge (see the bed above);
        // Cinematic keeps its floating look.
        const f32 ty1 = functional ? h : h - 6.0f;
        if (functional) {
            // The SELECTED page's tab stands taller than its neighbours,
            // trading height with an eased animation when the selection
            // moves. Only the top edge moves — all bottoms stay on one line.
            ty0 -= tabRaise(pages[i], active);
            // The game's own plate texture as the FILL, wrapped in the corner
            // buttons' beveled gold BORDER — chamfered on the TOP corners only
            // (1|2) and run past the screen edge so the bottom rim falls
            // off-canvas. Selected reads as the "enabled" (gold) rim.
            const f32 ty1e = ty1 + 8.0f;
            drawChamferPlate(x, ty0, x + tabW, ty1e, 10.0f, active, 1 | 2);
            drawBeveledBorder(x, ty0, x + tabW, ty1e, active, 1 | 2);
        } else {
            // The collection screen's Save/Options button plate.
            drawTabPlate(x, ty0, tabW, ty1 - ty0, active);
        }
        // Centred on the plate's VISIBLE span, not its full height: the
        // Functional plates run past the screen edge to blend into it, and
        // centring on that would push the label off the bottom.
        const f32 labelY = functional ? (ty0 + h - 6.0f) * 0.5f + 5.0f : h - 18.0f;
        // The active plate is the game's light parchment texture, so the
        // active label is dark ink; inactive is dim on the darker plate.
        drawTextFittedCentered(x + tabW * 0.5f, labelY, labelTS, labelTS, tabW - 12.0f,
            active ? TEXT_TAB_ACTIVE : TEXT_DIM, tabName(pages[i]));
        s_tabRects[s_tabRectCount][0] = x;
        s_tabRects[s_tabRectCount][1] = functional && ty0 < top ? ty0 : top;
        s_tabRects[s_tabRectCount][2] = x + tabW;
        s_tabRects[s_tabRectCount][3] = h;
        s_tabRectPage[s_tabRectCount] = pages[i];
        s_tabRectCount++;
    }
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

// Window chrome from the game's own menu art. Frame recipe (converged over
// many visual iterations with the user): DOUBLE LINE2 rule along the top, a
// SINGLE rule at the bottom (the tab strip sits right under it — a second
// line there just crowds the context tab), one dark drawn rule down each
// side (continued by the tab strip's bed so the two read as one line), and
// the collection screen's small flourish (TT_KAZARI_2ND_OKAN) in the
// top-left corner plus a vertically-mirrored copy at the bottom-left.
void drawWindowOrnaments(f32 x0, f32 y0, f32 x1, f32 y1) {
    constexpr u32 INK = 0x00000000u;
    constexpr u32 LIT = 0x9A8F79FFu;
    constexpr u32 LIT_IN = 0x6E6555FFu;
    constexpr u32 ORN = 0xB6A886FFu;
    const ResTIMG* line = decoTimg(DECO_LINE);
    if (line == NULL) {
        fillRect(x0, y0, x1, y0 + 1.5f, COL_FRAME);
        fillRect(x0, y1 - 1.5f, x1, y1, COL_FRAME);
        fillRect(x0, y0, x0 + 1.5f, y1, COL_FRAME);
        fillRect(x1 - 1.5f, y0, x1, y1, COL_FRAME);
        return;
    }
    const f32 w = x1 - x0;
    // Top: outer rule on the edge, inner rule just inside it.
    drawTimgTinted(line, x0, y0 - 1.0f, w, 5.0f, 235, INK, LIT);
    drawTimgTinted(line, x0, y0 + 3.0f, w, 4.0f, 185, INK, LIT_IN);
    // Bottom: one rule only.
    drawTimgTinted(line, x0, y1 - 4.0f, w, 5.0f, 235, INK, LIT);
    // Sides: drawn, not textured. The LINE2 art is a horizontal rule —
    // squeezed into a tall thin strip its gradient faded out before the
    // bottom, leaving the side rule visible only down the top half.
    fillRect(x0 + 0.5f, y0, x0 + 2.5f, y1, COL_SIDE_RULE);
    fillRect(x1 - 2.5f, y0, x1 - 0.5f, y1, COL_SIDE_RULE);
    if (const ResTIMG* kaz = decoTimg(DECO_KAZARI)) {
        const f32 k = 40.0f;
        const f32 kh = k * (f32)(u16)kaz->height / (f32)(u16)kaz->width;
        drawTimgTinted(kaz, x0 + 3.0f, y0 + 3.0f - kh * 0.25f, k, kh, 210, INK, ORN);
        // Functional defers the bottom flourish to drawTabs — it has to
        // land ON the tab strip's bed but UNDER its plates, and the strip
        // draws after this window.
        if (!dualscreen::mainHudRestored()) {
            drawTimgTintedMirror(kaz, x0 + 3.0f, y1 - kh - 3.0f + kh * 0.75f, k, kh, 210, INK,
                ORN, false, true);
        }
    }
}

// uses the Link-display box's own mottled background (TT_YAKUSHIMA) with a
// thin frame line, like the collection screen.
void drawContentWindow(f32 wx0, f32 wx1, f32 cy0, f32 cy1) {




    // Published for the touch pass: page hit-tests inset off these edges the
    // same way the page draws below do.
    s_contentRect[0] = wx0;
    s_contentRect[1] = cy0;
    s_contentRect[2] = wx1;
    s_contentRect[3] = cy1;
    fillRect(wx0, cy0, wx1, cy1, COL_WINDOW);
    if (const ResTIMG* bg = decoTimg(DECO_YAKUSHIMA)) {
        drawTimgTinted(bg, wx0 + 1.5f, cy0 + 1.5f, wx1 - wx0 - 3.0f, cy1 - cy0 - 3.0f, 160,
            0x0C0B0AFFu, 0x1F1D19FFu);
    }
    // (The border/ornament frame is drawn LAST, on top of the page — see the
    // end of this function.)
    // Clip the page content to the window; published as the active window
    // clip so in-page scissor restores come back HERE, not to full screen
    // (see applyWinClip — required for the page-slide transition).
    s_winClip[0] = (u32)(wx0 * s_pixelScale);
    s_winClip[1] = (u32)(cy0 * s_pixelScale);
    s_winClip[2] = (u32)((wx1 - wx0) * s_pixelScale);
    s_winClip[3] = (u32)((cy1 - cy0) * s_pixelScale);
    applyWinClip();
    // Page change transition: the incoming page grows out of its own tab
    // while the outgoing one fades out in place. Content touch is suppressed
    // until it lands (s_pageSliding); a second change mid-transition snaps.
    static int sShownPage = -1;
    static int sGrowOldPage = -1;
    static f32 sGrowT = 1.0f;
    static bool sGrowing = false;
    // The incoming page's own tab rect, captured when the change starts.
    static f32 sGrowFrom[4];
    const int page = s_page.load();
    if (sShownPage < 0) {
        sShownPage = page;
    }
    if (page != sShownPage) {
        if (!sGrowing) {
            // Grow out of the incoming page's OWN tab (previous frame's strip
            // geometry — stable). No rect (layout change) = snap.
            sGrowing = false;
            for (int i = 0; i < s_tabRectCount; i++) {
                if (s_tabRectPage[i] == page && s_tabRects[i][2] > s_tabRects[i][0]) {
                    for (int r = 0; r < 4; r++) {
                        sGrowFrom[r] = s_tabRects[i][r];
                    }
                    sGrowT = 0.0f;
                    sGrowing = true;
                    sGrowOldPage = sShownPage;
                    break;
                }
            }
        } else {
            sGrowing = false;
            sGrowT = 1.0f;
        }
        sShownPage = page;
    }
    if (sGrowing) {
        sGrowT += (1.0f - sGrowT) * ANIM_RATE_SETTLED;
        if (sGrowT > ANIM_DONE) {
            sGrowT = 1.0f;
            sGrowing = false;
        }
    }
    s_pageSliding = sGrowing;
    auto drawPage = [&](int p, f32 x0, f32 y0, f32 x1, f32 y1) {
        switch (p) {
        case PAGE_INVENTORY:
            drawInventoryContent(x0 + 4.0f, y0 + 8.0f, x1 - 2.0f, y1 - 8.0f);
            break;
        case PAGE_GUIDE:
            // No content of its own: the reader is drawn as an overlay over
            // this window further down (drawGuideOverlay), so the page only
            // has to guarantee it is open. Gated on the slide because
            // drawPage runs for BOTH pages mid-transition, and reopening the
            // one being slid away from would fight the tab that just closed
            // it.
            if (!s_pageSliding && s_page.load() == PAGE_GUIDE) {
                guideOpen();
            }
            break;
        case PAGE_COLLECTION:
            drawCollectionContent(x0 + 4.0f, y0 + 8.0f, x1 - 2.0f, y1 - 8.0f);
            break;
        case PAGE_MAP:
        default:
            drawMapContent(x0 + 4.0f, y0 + 4.0f, x1 - 4.0f, y1 - 4.0f);
            break;
        }
    };
    if (sGrowing) {
        // The incoming page POPS UP out of its own tab; the outgoing one stays
        // where it is and FADES OUT underneath it.
        //
        // The outgoing page is not shrunk back into its tab (two things moving
        // in opposite directions read as busy) and not left opaque either —
        // fading it means the window is still covered while the new page is
        // small, so the backdrop never flashes through.
        const f32 t = sGrowT < 0.0f ? 0.0f : (sGrowT > 1.0f ? 1.0f : sGrowT);
        if (sGrowOldPage >= 0 && sGrowOldPage != page) {
            s_drawAlpha = 1.0f - t;
            drawPage(sGrowOldPage, wx0, cy0, wx1, cy1);
            s_drawAlpha = 1.0f;
        }
        const f32 ax0 = sGrowFrom[0] + (wx0 - sGrowFrom[0]) * t;
        const f32 ay0 = sGrowFrom[1] + (cy0 - sGrowFrom[1]) * t;
        const f32 ax1 = sGrowFrom[2] + (wx1 - sGrowFrom[2]) * t;
        const f32 ay1 = sGrowFrom[3] + (cy1 - sGrowFrom[3]) * t;
        // Below this the box is too small for the page to lay out sensibly.
        if (t > 0.2f) {
            drawPage(page, ax0, ay0, ax1, ay1);
        }
    } else {
            drawPage(page, wx0, cy0, wx1, cy1);
        s_drawAlpha = 1.0f;
    }
    // Single choke point for the content fade: every page, reader and pop-up
    // is drawn above this line, and several of them have early returns, so the
    // restore lives here rather than being repeated on each path. Nothing
    // below (context tab, tab strip, chrome) is ever faded.
    s_drawAlpha = 1.0f;
    // Cinematic draws its context action here, over the page content but
    // inside the window scissor (no-op in Functional, which uses the left
    // column). GUIDE self-skips (no context action).
    drawCinematicContextTab(wx0 + 4.0f, cy0 + 4.0f, wx1 - 4.0f, cy1 - 4.0f);
    s_winClip[2] = 0;
    GXSetScissorRender(0, 0, s_nativeW, s_nativeH);
    // Border, built from the collection screen's own ornament set (verified
    // against zelda_collect_soubi_screen.blo): the base panel's soft edge
    // strip along the sides, LINE2 rules top and bottom, a corner flourish in
    // each corner, and gold swirl accents on the top rule.
    //
    // Drawn AFTER the page, and outside the clip, so it is never touched by
    // the content fade. The page rect is the full window and the content only
    // insets a few pixels, so it overlapped the frame: when the content faded,
    // the frame underneath was revealed and the whole border appeared to
    // animate. On top it simply stays put.
    drawWindowOrnaments(wx0, cy0, wx1, cy1);
    // Guide LAST, so it covers the ornaments rather than being covered by
    // them: its buttons and text sit in the window's corners, exactly where
    // the flourishes are, and a flourish painted over the < > buttons looked
    // like damage. The frame is deliberately behind the reader.
    drawGuideOverlay(wx0 + 4.0f, cy0 + 4.0f, wx1 - 4.0f, cy1 - 4.0f);
}

// D-pad cross HUD above the FPS/battery corner: the game's pad is assembled
// from rotated corner pieces the compositor can't reproduce (they render as
// loose hearts/corners), so draw a clean glyph plus the game's own
// localized labels (up = items, right = map). Hidden, like the buttons,
// while any menu window is up.
// Returns the height consumed (0 when hidden) so the band can stack.
f32 drawDpadGlyph(dMeter2Draw_c* md, f32 x1, f32 bottomY) {
    const bool menuOpen = anyMenuOpen();
    if (menuOpen || md == NULL || md->getButtonCrossPane() == NULL) {
        return 0.0f;
    }
    const f32 dbox = 23.0f;
    // The up-label sits above the glyph and is part of the slot.
    const f32 labelH = 14.0f;
    const f32 dx = x1 - 130.0f;
    const f32 dy = bottomY - dbox;
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
    return dbox + labelH;
}

// Wolf/human quick-transform button above the d-pad: a menu tab plate
// reading [current form] > [target form] in small pause-map portraits.
// Hidden until the shadow crystal is obtained (M_077) and while a menu
// window is up, like the d-pad; dimmed when the transform is currently
// blocked (airborne, cutscene, NPCs nearby...). A tap while dimmed still
// sends the request — the game answers with its error beep.
// Shared body: draws the plate into an explicit box, centering the portraits
// within it, and publishes the touch rect. False when the button should not
// be on screen at all. Both layouts place the box differently.
bool drawTransformPlate(f32 bx, f32 by, f32 bw, f32 bh) {
    const bool menuOpen = anyMenuOpen();
    daAlink_c* alink = daAlink_getAlinkActorClass();
    if (menuOpen || alink == NULL || !dComIfGs_isEventBit(dSv_event_flag_c::M_077)) {
        return false;
    }
    const bool wolf = companionWolf();
    const ResTIMG* faceCur = dmapFloorFaceTimg(wolf);
    const ResTIMG* faceTgt = dmapFloorFaceTimg(!wolf);
    if (faceCur == NULL || faceTgt == NULL) {
        return false;
    }
    // Portraits keep the source 40x41 aspect.
    const f32 icon = 24.0f;
    const f32 iconH = 24.6f;
    const f32 arrowW = 12.0f;
    const f32 gap = 3.0f;
    const f32 contentW = icon * 2.0f + gap * 2.0f + arrowW;
    const f32 cx0 = bx + (bw - contentW) * 0.5f;
    drawTabPlate(bx, by, bw, bh, true);
    const f32 iy = by + (bh - iconH) * 0.5f;
    drawTimg(faceCur, cx0, iy, icon, iconH, 0xFF);
    drawTextCentered(cx0 + icon + gap + arrowW * 0.5f, by + bh * 0.5f + 5.0f, 14.0f,
        TEXT_TAB_ACTIVE, ">");
    drawTimg(faceTgt, cx0 + icon + gap + arrowW + gap, iy, icon, iconH, 0xFF);
    if (!alink->checkQuickTransformOK()) {
        constexpr GXColor COL_DIM = {0, 0, 0, 150};
        fillRect(bx, by, bx + bw, by + bh, COL_DIM);
    }
    s_transformBtnRect[0] = bx;
    s_transformBtnRect[1] = by;
    s_transformBtnRect[2] = bx + bw;
    s_transformBtnRect[3] = by + bh;
    return true;
}

// Defined below with the other Functional chrome. File-local: the corner
// boxes exist only in this layout.


f32 drawTransformButton(f32 x1, f32 bottomY) {
    // Content-derived width, matching the original layout exactly: 5px pad
    // each side around the two portraits and the arrow.
    constexpr f32 bw = 5.0f * 2.0f + 24.0f * 2.0f + 3.0f * 2.0f + 12.0f;
    constexpr f32 bh = 34.0f;
    return drawTransformPlate(x1 - 142.0f, bottomY - bh, bw, bh) ? bh : 0.0f;
}


f32 drawStatusCorner(f32 x1, f32 bottomY) {
    // The battery glyph is 12 tall; its pct text baseline sits at y + 11.
    constexpr f32 rowH = 12.0f;
    const f32 batY = bottomY - rowH;
    const f32 batX = x1 - 66.0f;
    // Self-gates on a reported percentage; always anchored to the corner.
    drawBattery(batX, batY);
    return drawFpsReadout(batX - 64.0f, batY + 11.0f) ? rowH : 0.0f;
}

// Combo-or-replace chooser, centered over the content window: two plates,
// [Bomb Arrows / Hawkeye] and [Replace]. Anything else tapped cancels
// (handled in the touch pass). Cancels itself if the pending drop went
// stale (bow moved, item gone).
void drawComboChoice() {
    if (s_comboChoiceBtn < 0) {
        return;
    }
    const u8 itemNo =
        s_comboChoiceSlot >= 0 ? dComIfGs_getItem(s_comboChoiceSlot, false) : (u8)dItemNo_NONE_e;
    if (!bowComboAmbiguous(s_comboChoiceBtn, itemNo) || anyMenuOpen()) {
        s_comboChoiceBtn = -1;
        return;
    }
    const f32 bw = 300.0f;
    const f32 bh = 104.0f;
    const f32 x0 = (s_contentRect[0] + s_contentRect[2]) * 0.5f - bw * 0.5f;
    const f32 y0 = (s_contentRect[1] + s_contentRect[3]) * 0.5f - bh * 0.5f;
    drawMenuBox(x0, y0, x0 + bw, y0 + bh, 0x2A2722FFu);
    drawChamferFrame(x0, y0, x0 + bw, y0 + bh, 10.0f, 1.5f, COL_FRAME, 1 | 2 | 4 | 8);
    drawTextFittedCentered(x0 + bw * 0.5f, y0 + 24.0f, 15.0f, 10.0f, bw - 24.0f,
        TEXT_ACCENT, txt(STR_COMBINE_BOW));
    // Hawkeye is a real item, so its name is in the archive. "Bomb Arrows"
    // is not an item name (dItemNo_BOMB_ARROW_e resolves to "Hero's Bow"),
    // so that one comes from the translation table.
    const char* l_labels[2];
    l_labels[0] = itemNo == dItemNo_HAWK_EYE_e
        ? archiveText(0x165 + dItemNo_HAWK_EYE_e, "Hawkeye")
        : txt(STR_BOMB_ARROWS);
    l_labels[1] = txt(STR_REPLACE);
    const f32 pw = 130.0f;
    const f32 ph = 40.0f;
    const f32 py = y0 + bh - ph - 12.0f;
    for (int i = 0; i < 2; i++) {
        const f32 px = x0 + 14.0f + (f32)i * (pw + 12.0f);
        drawTabPlate(px, py, pw, ph, i == 0);
        drawTextFittedCentered(px + pw * 0.5f, py + ph * 0.5f + 5.0f, 14.0f, 9.0f,
            pw - 12.0f, i == 0 ? TEXT_TAB_ACTIVE : TEXT_MAIN, l_labels[i]);
        s_comboChoiceRects[i][0] = px;
        s_comboChoiceRects[i][1] = py;
        s_comboChoiceRects[i][2] = px + pw;
        s_comboChoiceRects[i][3] = py + ph;
    }
}

// Drag ghost: the item follows the finger with a thick orange border.
void drawDragGhost() {
    // Fly-out after release: the ghost shrinks into its destination (drop
    // target on a consumed drop, its home cell on a miss). Runs after
    // s_dragging cleared; a page change cancels it (epilogue clears slot).
    if (s_ghostFlyItem != 0xFF) {
        s_ghostFlyT += (1.0f - s_ghostFlyT) * ANIM_RATE_FAST;
        if (s_ghostFlyT > ANIM_DONE || s_page.load() != PAGE_INVENTORY) {
            s_ghostFlyItem = 0xFF;
        } else {
            const f32 t = s_ghostFlyT;
            const f32 cx = s_ghostFlyFromX + (s_ghostFlyToX - s_ghostFlyFromX) * t;
            const f32 cy = s_ghostFlyFromY + (s_ghostFlyToY - s_ghostFlyFromY) * t;
            const f32 g = 52.0f * (1.0f - 0.75f * t);
            drawItemIcon(s_ghostFlySlot, s_ghostFlyItem, cx - g * 0.5f, cy - g * 0.5f, g);
        }
    }
    if (!s_dragging || s_dragSlot < 0) {
        return;
    }
    const u8 dragItem = dComIfGs_getItem(s_dragSlot, false);
    if (dragItem == dItemNo_NONE_e) {
        return;
    }
    // Pickup pop: starts 15% large and eases down, so lifting an item off
    // the grid reads as plucking it.
    s_ghostPop *= ANIM_DECAY_SOFT;
    const f32 g = 52.0f * (1.0f + 0.15f * s_ghostPop);
    const f32 gx = s_dragX - g * 0.5f;
    const f32 gy = s_dragY - g * 0.5f;
    drawItemIconSilhouette(s_dragSlot, dragItem, gx - 4.0f, gy - 4.0f, g + 8.0f, 0xECD054FFu);
    drawItemIcon(s_dragSlot, dragItem, gx, gy, g);
}

// --- Functional layout -----------------------------------------------------





// Left column: transform button on top, rupee and small-key counters, the
// dungeon item icons, and the Z button at the bottom. Mirrors the reference
// layout's camera/ocarina bookends.
// Top zone: a dark box with the game's own rupee composite, drawn larger.
// Panel that bleeds in from the screen's left edge: flush left (no margin, no
// left chamfer) with only its right corners cut, so it reads as part of the
// screen rather than a floating box.

}  // namespace dusk::companion
