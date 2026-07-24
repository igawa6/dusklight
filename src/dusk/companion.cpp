#include "dusk/action_bindings.h"
#include "dusk/companion.h"
#include "dusk/companion_internal.h"
#include "dusk/dualscreen.h"
#include "dusk/settings.h"
#include <aurora/gfx.h>

#include "imgui.h"

#include "JSystem/J2DGraph/J2DPicture.h"
#include "d/d_com_inf_game.h"
#include "d/d_kankyo.h"
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
#include "m_Do/m_Do_audio.h"
#include "m_Do/m_Do_graphic.h"
#include "f_op/f_op_overlap_mng.h"
#include "aurora/lib/device.hpp"

#include <atomic>
#include <cmath>
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
f32 s_dropRect[DROP_TARGET_COUNT][4];

f32 s_transformBtnRect[4];
std::atomic<bool> s_transformReq{false};

// Functional layout's Z button (bottom-left) and the two I/II item slots.
f32 s_zBtnRect[4];
std::atomic<bool> s_zPressReq{false};
f32 s_slotBtnRect[2][4];
f32 s_fnXYRect[2][4];
f32 s_dropBtnSize = 38.0f;

// Touch item buttons: held pad mask + ghost-use state (see the internal
// header for the model).
std::atomic<uint32_t> s_padHoldMaskState{0};
std::atomic<uint32_t> s_slotHoldMaskState{0};
int s_holdBtn = -1;
int s_holdFrames = 0;
bool s_holdReleaseReq = false;
u8 s_denyFlash[4] = {0, 0, 0, 0};
f32 s_pressAnim[6] = {};
f32 s_popAnim[4] = {};
// Detail pop: skill/mail readers and the ITEMS info reader grow out of the
// row (or item cell) they were opened from, and shrink back into it on
// close. Shared — only one detail can be open at a time.
// True for the WHOLE span of a scene change — armed on any transition
// signal, released only once the world is genuinely live again. Gates both
// the dashboard dim and the I/II/X/Y equip pop.
bool s_inSceneChange = false;
f32 s_readerZoomT = 1.0f;
bool s_readerZoomClosing = false;
f32 s_readerZoomFrom[4] = {};
f32 s_collectZoomT = 1.0f;
bool s_collectZoomClosing = false;
f32 s_collectZoomFrom[4] = {};
f32 s_leftBoxDragY = 0.0f;
bool s_leftBoxTracking = false;
f32 s_wolfBlend = 0.0f;
f32 s_ghostPop = 0.0f;
f32 s_ghostFlyT = 0.0f;
f32 s_ghostFlyFromX = 0.0f, s_ghostFlyFromY = 0.0f;
f32 s_ghostFlyToX = 0.0f, s_ghostFlyToY = 0.0f;
u8 s_ghostFlyItem = 0xFF;
int s_ghostFlySlot = -1;
f32 s_eventDim = 0.0f;
// The transition/cover dim: follows the main screen's fade alpha on the
// fade edges and holds full through the black/loading middle (see the dim
// block in beginFrameCompanionInput). Combined with s_eventDim at draw.
f32 s_dimHold = 0.0f;
// Slot-button bits latched once per game frame (game thread only):
// setStickData reads these later in the same frame.
u32 s_slotTrig = 0;
u32 s_slotHold = 0;

f32 s_tabRects[TAB_RECT_MAX][4];
int s_tabRectPage[TAB_RECT_MAX];
int s_tabRectCount = 0;
f32 s_contentRect[4];

std::atomic<bool> s_warpReq{false};
std::atomic<bool> s_warpToggleReq{false};
bool s_warpToggleLive = false;

// Functional left-column context tab + COLLECT reader selection.
f32 s_ctxTabRect[4];
std::atomic<int> s_leftBoxPage{LEFT_BOX_CONTEXT};
f32 s_leftBoxRect[4];
f32 s_canvasW = 0.0f;
f32 s_canvasH = 0.0f;
std::atomic<bool> s_downOnContent{false};
int s_collectSel = -1;
f32 s_collectIconRects[4][4];
int s_comboChoiceBtn = -1;
int s_comboChoiceSlot = -1;
f32 s_comboChoiceRects[2][4];

// Queued interaction cues. Plain (non-atomic) storage on purpose: the
// producer is the touch pass at the end of drawDashboard and the consumer is
// duskExecute, both on the game thread — the queue defers by phase, not
// across threads. A handful of cues per frame is already generous; a tap
// resolves to one.
struct SoundCue {
    unsigned sfx;
    int haptic;
};
constexpr int SOUND_QUEUE_MAX = 8;
SoundCue s_soundQueue[SOUND_QUEUE_MAX];
int s_soundQueueCount = 0;

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
bool s_mapResetGlide = false;
bool s_pageSliding = false;
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
// The 56px left lane exists for Cinematic's equipped-item rectangle only.
// Functional's round buttons ARE the drop targets (drawEquipTargets is
// skipped), so there the accept zone matches the circle — an invisible lane
// reaching into the content window equipped items with no ring feedback.
void publishEquipDropRect(int dropIdx, f32 x, f32 y, f32 btn = CLUSTER_BTN) {
    const f32 lane = dusk::dualscreen::mainHudRestored() ? 0.0f : 56.0f;
    s_dropRect[dropIdx][0] = x - lane;
    s_dropRect[dropIdx][1] = y - 1.0f;
    s_dropRect[dropIdx][2] = x + btn + 4.0f;
    s_dropRect[dropIdx][3] = y + btn + 1.0f;
    s_dropBtnPos[dropIdx][0] = x;
    s_dropBtnPos[dropIdx][1] = y;
    s_dropBtnSize = btn;
    s_dropRectValid = true;
}

// Ammo count inside the button box, bottom-left, on a dark chip.
// Ammo count centred on (cx, bottomY) — the Functional buttons are large and
// round, so the count reads better centred under the icon than chipped into a
// corner. drawHudNumber is TOP-anchored and its digits are digitH * 0.72 wide
// with 0.9 advance.
void drawAmmoCountCentered(int ammo, f32 cx, f32 bottomY) {
    constexpr GXColor COL_CHIP = {10, 9, 7, 210};
    constexpr f32 digitH = 12.0f;
    const int digits = ammo >= 100 ? 3 : ammo >= 10 ? 2 : 1;
    const f32 w = digitH * 0.72f * (0.9f * (f32)(digits - 1) + 1.0f);
    const f32 x0 = cx - w * 0.5f;
    const f32 y0 = bottomY - digitH;
    fillRect(x0 - 4.0f, y0 - 2.0f, x0 + w + 4.0f, bottomY + 2.0f, COL_CHIP);
    drawHudNumber(ammo, x0, y0, digitH);
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
    s_raise[i_page] += (target - s_raise[i_page]) * 0.12f;
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
    for (int i = 0; i < count; i++) {
        const f32 x = x0 + i * (tabW + GAP);
        const bool active = pages[i] == page;
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
        drawTextCentered(x + tabW * 0.5f, labelY, 15.0f,
            active ? TEXT_TAB_ACTIVE : TEXT_DIM, l_tabNames[pages[i]]);
        s_tabRects[s_tabRectCount][0] = x;
        s_tabRects[s_tabRectCount][1] = functional && ty0 < top ? ty0 : top;
        s_tabRects[s_tabRectCount][2] = x + tabW;
        s_tabRects[s_tabRectCount][3] = h;
        s_tabRectPage[s_tabRectCount] = pages[i];
        s_tabRectCount++;
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
    // Border, built from the collection screen's own ornament set (verified
    // against zelda_collect_soubi_screen.blo): the base panel's soft edge
    // strip along the sides, LINE2 rules top and bottom, a corner flourish
    // in each corner, and gold swirl accents on the top rule.
    drawWindowOrnaments(wx0, cy0, wx1, cy1);
    // Clip the page content to the window; published as the active window
    // clip so in-page scissor restores come back HERE, not to full screen
    // (see applyWinClip — required for the page-slide transition).
    s_winClip[0] = (u32)(wx0 * s_pixelScale);
    s_winClip[1] = (u32)(cy0 * s_pixelScale);
    s_winClip[2] = (u32)((wx1 - wx0) * s_pixelScale);
    s_winClip[3] = (u32)((cy1 - cy0) * s_pixelScale);
    applyWinClip();
    // Page grow: on a page change the incoming page GROWS out of its own
    // tab's rectangle up into the window (the COLLECT-cell grow, page
    // scale), over the outgoing page. Pages draw from live state, so
    // drawing two per frame is safe; content touch is suppressed until the
    // grow lands (s_pageSliding). A second change mid-grow snaps.
    static int sShownPage = -1;
    static int sGrowOldPage = -1;
    static f32 sGrowT = 1.0f;
    static f32 sGrowFrom[4];
    static f32 sShrinkTo[4];
    static bool sGrowing = false;
    static bool sShrinkValid = false;
    const int page = s_page.load();
    if (sShownPage < 0) {
        sShownPage = page;
    }
    if (page != sShownPage) {
        if (!sGrowing) {
            // The incoming page's OWN tab rect (previous frame's strip
            // geometry — stable). Missing rect (layout change) = snap.
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
            // The outgoing page shrinks back into ITS tab; without a rect
            // it simply isn't drawn while the new page grows.
            sShrinkValid = false;
            if (sGrowing) {
                for (int i = 0; i < s_tabRectCount; i++) {
                    if (s_tabRectPage[i] == sGrowOldPage &&
                        s_tabRects[i][2] > s_tabRects[i][0]) {
                        for (int r = 0; r < 4; r++) {
                            sShrinkTo[r] = s_tabRects[i][r];
                        }
                        sShrinkValid = true;
                        break;
                    }
                }
            }
        } else {
            sGrowing = false;
            sGrowT = 1.0f;
        }
        sShownPage = page;
    }
    if (sGrowing) {
        sGrowT += (1.0f - sGrowT) * 0.20f;
        if (sGrowT > 0.97f) {
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
        case PAGE_QUEST:
            drawQuestContent(x0 + 4.0f, y0 + 8.0f, x1 - 2.0f, y1 - 8.0f);
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
        // Both pages animate over the window background — NO bed box. The old
        // page shrinks back into its own tab while the new grows out of its
        // tab on the same eased t, so only the page CONTENT reads the motion
        // (the map, the grid) rather than a panel sliding around.
        const f32 t = sGrowT;
        if (sShrinkValid && sGrowOldPage >= 0 && t < 0.7f) {
            const f32 sx0 = wx0 + (sShrinkTo[0] - wx0) * t;
            const f32 sy0 = cy0 + (sShrinkTo[1] - cy0) * t;
            const f32 sx1 = wx1 + (sShrinkTo[2] - wx1) * t;
            const f32 sy1 = cy1 + (sShrinkTo[3] - cy1) * t;
            drawPage(sGrowOldPage, sx0, sy0, sx1, sy1);
        }
        const f32 ax0 = sGrowFrom[0] + (wx0 - sGrowFrom[0]) * t;
        const f32 ay0 = sGrowFrom[1] + (cy0 - sGrowFrom[1]) * t;
        const f32 ax1 = sGrowFrom[2] + (wx1 - sGrowFrom[2]) * t;
        const f32 ay1 = sGrowFrom[3] + (cy1 - sGrowFrom[3]) * t;
        if (t > 0.2f) {
            drawPage(page, ax0, ay0, ax1, ay1);
        }
    } else {
        drawPage(page, wx0, cy0, wx1, cy1);
    }
    // Cinematic draws its context action here, over the page content but
    // inside the window scissor (no-op in Functional, which uses the left
    // column). QUEST self-skips (no context action).
    drawCinematicContextTab(wx0 + 4.0f, cy0 + 4.0f, wx1 - 4.0f, cy1 - 4.0f);
    s_winClip[2] = 0;
    GXSetScissorRender(0, 0, s_nativeW, s_nativeH);
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
void drawCornerBox(f32 x0, f32 y0, f32 x1, f32 y1, const char* label, bool enabled);

// Transform as one of the Functional corner boxes: persistent, with the two
// portraits stacked under the label. Dims (rather than vanishing) whenever
// the transform is unavailable — before the shadow crystal, in menus, or
// while the game refuses it — so the corner never moves.
void drawTransformCorner(f32 x0, f32 y0, f32 x1, f32 y1) {
    // Tap pulse: the whole box (plate + portraits, which all derive from
    // these coords) pinches in and eases back, like the X/Y circles. The
    // touch rect publish at the end keeps the resting geometry.
    const f32 rx0 = x0, ry0 = y0, rx1 = x1, ry1 = y1;
    const f32 pin = (x1 - x0) * 0.07f * s_pressAnim[4];
    x0 += pin * 0.5f;
    y0 += pin * 0.5f;
    x1 -= pin * 0.5f;
    y1 -= pin * 0.5f;
    daAlink_c* alink = daAlink_getAlinkActorClass();
    const bool unlocked = dComIfGs_isEventBit(dSv_event_flag_c::M_077) != 0;
    const bool wolf = s_wolfBlend > 0.5f;
    const bool morphing = s_wolfBlend > 0.02f && s_wolfBlend < 0.98f;
    // Point-sampled copies: drawn far above their 44x45 native size here, so
    // the BTI's own bilinear filter would blur them.
    const ResTIMG* faceCur = dmapFloorFaceTimgSharp(wolf);
    const ResTIMG* faceTgt = dmapFloorFaceTimgSharp(!wolf);
    const bool enabled = unlocked && !anyMenuOpen() && alink != NULL &&
        alink->checkQuickTransformOK();
    // No label: the two portraits say what the box does.
    drawCornerBox(x0, y0, x1, y1, NULL, enabled);
    if (unlocked && morphing && faceCur != NULL) {
        // Mid-transform: one full portrait doing a half-spin reads better
        // than the split face (whose scissor halves can't rotate).
        const f32 icon = FN_BTN * 0.64f;
        drawTimgRotated(faceCur, (x0 + x1) * 0.5f, (y0 + y1) * 0.5f, icon,
            s_wolfBlend * 180.0f, 0xFF);
    } else if (unlocked && faceCur != NULL && faceTgt != NULL) {
        // ONE split portrait, not two small ones side by side: both faces are
        // drawn at full size over the same rect, each scissored to its own
        // half, so the result reads as a single face that is half Link and
        // half wolf. Sized to match the X/Y buttons' equipped-item icon.
        const f32 icon = FN_BTN * 0.64f;
        const f32 iconH = icon * 41.0f / 40.0f;
        const f32 ix = x0 + (x1 - x0 - icon) * 0.5f;
        const f32 iy = y0 + (y1 - y0 - iconH) * 0.5f;
        // Dimmed alongside the darker plate when the transform is blocked.
        const u8 alpha = enabled ? 0xFF : 0x78;
        const f32 midX = ix + icon * 0.5f;
        // Left half = current form.
        GXSetScissorRender((u32)(ix * s_pixelScale), (u32)(iy * s_pixelScale),
            (u32)((midX - ix) * s_pixelScale), (u32)(iconH * s_pixelScale));
        drawTimg(faceCur, ix, iy, icon, iconH, alpha);
        // Right half = the form the button switches to.
        GXSetScissorRender((u32)(midX * s_pixelScale), (u32)(iy * s_pixelScale),
            (u32)((ix + icon - midX) * s_pixelScale), (u32)(iconH * s_pixelScale));
        drawTimg(faceTgt, ix, iy, icon, iconH, alpha);
        GXSetScissorRender(0, 0, s_nativeW, s_nativeH);
    }
    // Published even when dimmed: the game answers a blocked transform with
    // its own error cue, matching the Cinematic button. Resting coords —
    // the pinch above must not shrink the hit target.
    s_transformBtnRect[0] = rx0;
    s_transformBtnRect[1] = ry0;
    s_transformBtnRect[2] = rx1;
    s_transformBtnRect[3] = ry1;
}

f32 drawTransformButton(f32 x1, f32 bottomY) {
    // Content-derived width, matching the original layout exactly: 5px pad
    // each side around the two portraits and the arrow.
    constexpr f32 bw = 5.0f * 2.0f + 24.0f * 2.0f + 3.0f * 2.0f + 12.0f;
    constexpr f32 bh = 34.0f;
    return drawTransformPlate(x1 - 142.0f, bottomY - bh, bw, bh) ? bh : 0.0f;
}

// Bottom-right row: [FPS] [battery glyph] [pct] — FPS only when the video
// setting "Show FPS Counter" is set (dual-screen auto-selects Companion),
// battery only on devices that report one.
//
// Returns the height the *d-pad column* has to clear, which is not the same
// as "was anything drawn". The battery lives in the far corner (x1-66 and
// right) while the d-pad column sits at x1-130; only the FPS text shares
// that column. So a battery alone reserves nothing and the d-pad drops
// alongside it rather than floating above an apparently empty row.
// Colour-coded FPS readout at (x, baselineY). Draws only when the corner
// setting actually points at the companion — otherwise the main screen keeps
// it, so all five options mean something. Returns whether it drew. Shared by
// both layouts' status rows so the gate and thresholds can't drift.
bool drawFpsReadout(f32 x, f32 baselineY) {
    if (!getSettings().video.enableFpsOverlay.getValue() ||
        getSettings().video.fpsOverlayCorner.getValue() != kFpsCornerCompanion)
    {
        return false;
    }
    const int fps = (int)(aurora_get_fps() + 0.5f);
    GXColor fpsCol = {150, 214, 120, 255};
    if (fps < 30) fpsCol = {224, 80, 64, 255};
    else if (fps < 55) fpsCol = {244, 186, 84, 255};
    const u32 fpsRgba = (fpsCol.r << 24) | (fpsCol.g << 16) | (fpsCol.b << 8) | fpsCol.a;
    drawText(x, baselineY, 13.0f, fpsRgba, "%d FPS", fps);
    return true;
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
    const bool partner = itemNo == dItemNo_NORMAL_BOMB_e || itemNo == dItemNo_WATER_BOMB_e ||
        itemNo == dItemNo_POKE_BOMB_e || itemNo == dItemNo_HAWK_EYE_e;
    const bool btnHasBow = dComIfGs_getSelectItemIndex(s_comboChoiceBtn) == SLOT_4 ||
        dComIfGs_getMixItemIndex(s_comboChoiceBtn) == SLOT_4;
    if (!partner || !btnHasBow || anyMenuOpen()) {
        s_comboChoiceBtn = -1;
        return;
    }
    const f32 bw = 300.0f;
    const f32 bh = 104.0f;
    const f32 x0 = (s_contentRect[0] + s_contentRect[2]) * 0.5f - bw * 0.5f;
    const f32 y0 = (s_contentRect[1] + s_contentRect[3]) * 0.5f - bh * 0.5f;
    drawMenuBox(x0, y0, x0 + bw, y0 + bh, 0x2A2722FFu);
    drawChamferFrame(x0, y0, x0 + bw, y0 + bh, 10.0f, 1.5f, COL_FRAME, 1 | 2 | 4 | 8);
    drawTextCentered(x0 + bw * 0.5f, y0 + 24.0f, 15.0f, TEXT_ACCENT, "Combine with Bow?");
    const char* comboLabel =
        itemNo == dItemNo_HAWK_EYE_e ? "Hawkeye" : "Bomb Arrows";
    static const char* l_labels[2] = {NULL, "Replace"};
    l_labels[0] = comboLabel;
    const f32 pw = 130.0f;
    const f32 ph = 40.0f;
    const f32 py = y0 + bh - ph - 12.0f;
    for (int i = 0; i < 2; i++) {
        const f32 px = x0 + 14.0f + (f32)i * (pw + 12.0f);
        drawTabPlate(px, py, pw, ph, i == 0);
        drawTextCentered(px + pw * 0.5f, py + ph * 0.5f + 5.0f, 14.0f,
            i == 0 ? TEXT_TAB_ACTIVE : TEXT_MAIN, l_labels[i]);
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
        s_ghostFlyT += (1.0f - s_ghostFlyT) * 0.30f;
        if (s_ghostFlyT > 0.92f || s_page.load() != PAGE_INVENTORY) {
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
    s_ghostPop *= 0.78f;
    const f32 g = 52.0f * (1.0f + 0.15f * s_ghostPop);
    const f32 gx = s_dragX - g * 0.5f;
    const f32 gy = s_dragY - g * 0.5f;
    drawItemIconSilhouette(s_dragSlot, dragItem, gx - 4.0f, gy - 4.0f, g + 8.0f, 0xECD054FFu);
    drawItemIcon(s_dragSlot, dragItem, gx, gy, g);
}

// --- Functional layout -----------------------------------------------------

// Horizontal meter bar with a visible track, spanning x0..x1: oxygen
// (drowning) first, else lantern oil when allowed. Drawn rather than
// composited from the game's meter panes: the pane track/frame alphas are
// nearly transparent, so the composite read as a floating sliver that
// shrank with the reading instead of a gauge with a fixed extent.
// Returns true when a meter was drawn.
bool drawMeterBar(f32 x0, f32 x1, f32 cy, f32 barH, bool allowOil) {
    dMeter2Draw_c* md = meterDraw();
    const bool oxygen = md != NULL && md->isOxygenActive() && dComIfGp_getMaxOxygen() > 0;
    // The lantern counts as equipped on ANY of the four item buttons —
    // slots I/II included.
    int lanternSlot = -1;
    for (int b = 0; b < 4; b++) {
        if (dComIfGp_getSelectItem(b) == dItemNo_KANTERA_e) {
            lanternSlot = b;
            break;
        }
    }
    // No oil gauge in wolf form — the wolf can't touch the lantern, and
    // vanilla hides its own disc there (alphaAnimeKantera's checkUseKandelaar
    // gate; that predicate is lit-only, too strict for this persistent fuel
    // readout, so the companion checks just the wolf half).
    const bool oil = !oxygen && allowOil && !companionWolf() &&
        lanternSlot >= 0 && dComIfGs_getMaxOil() > 0;
    if (!oxygen && !oil) {
        return false;
    }
    const f32 by0 = cy - barH * 0.5f;
    const f32 by1 = cy + barH * 0.5f;
    f32 barX0 = x0;
    if (oil) {
        // Lantern icon labels the bar.
        const f32 icon = barH + 10.0f;
        drawItemIcon(ICON_SLOT_LANTERN, dItemNo_KANTERA_e, barX0, cy - icon * 0.5f, icon);
        barX0 += icon + 8.0f;
    }
    f32 frac;
    GXColor fillCol;
    if (oxygen) {
        // NowOxygen is the meter-anim value the game's own bar shows, so the
        // fill depletes smoothly instead of stepping.
        frac = (f32)dComIfGp_getNowOxygen() / (f32)dComIfGp_getMaxOxygen();
        // The game's oxygen palette: water blue, danger red under a quarter.
        fillCol = frac <= 0.25f ? GXColor{255, 70, 60, 255} : GXColor{80, 180, 255, 255};
    } else {
        frac = (f32)dComIfGs_getOil() / (f32)dComIfGs_getMaxOil();
        // The lantern meter's amber.
        fillCol = {230, 170, 40, 255};
    }
    if (frac < 0.0f) {
        frac = 0.0f;
    } else if (frac > 1.0f) {
        frac = 1.0f;
    }
    // Danger pulse: a low gauge breathes, so the glance-down catches it
    // without reading the exact level. Render-rate phase; visual only (the
    // crossing haptic lives in beginFrameCompanionInput).
    if (oxygen ? frac <= 0.25f : frac <= 0.15f) {
        static f32 sPulse = 0.0f;
        sPulse += 0.18f;
        if (sPulse > 6.2831853f) {
            sPulse -= 6.2831853f;
        }
        fillCol.a = (u8)(255.0f * (0.72f + 0.28f * (0.5f + 0.5f * sinf(sPulse))));
    }
    constexpr GXColor COL_TRACK = {12, 11, 9, 230};
    fillRect(barX0, by0, x1, by1, COL_TRACK);
    if (frac > 0.0f) {
        fillRect(barX0 + 1.5f, by0 + 1.5f, barX0 + 1.5f + (x1 - barX0 - 3.0f) * frac,
            by1 - 1.5f, fillCol);
    }
    // Hairline frame in the window chrome's rule colour, so the track's full
    // extent always reads even when the fill is low.
    fillRect(barX0, by0, x1, by0 + 1.0f, COL_FRAME);
    fillRect(barX0, by1 - 1.0f, x1, by1, COL_FRAME);
    fillRect(barX0, by0, barX0 + 1.0f, by1, COL_FRAME);
    fillRect(x1 - 1.0f, by0, x1, by1, COL_FRAME);
    return true;
}

// Status strip: FPS on the left, battery on the right, and between them the
// two timed gauges — oxygen and lantern oil. Both are situational, so they
// belong on a status row rather than taking permanent space in the layout;
// each self-hides when it has nothing to report.
void drawFunctionalTopBar(f32 w) {
    constexpr GXColor COL_SCRIM = {11, 10, 8, 170};
    fillRect(0.0f, 0.0f, w, FN_TOPBAR_H, COL_SCRIM);
    if (const ResTIMG* line = decoTimg(DECO_LINE)) {
        drawTimgTinted(line, 0.0f, FN_TOPBAR_H - 3.0f, w, 5.0f, 150, 0x00000000u, 0xA89C74FFu);
    }
    const f32 baseline = FN_TOPBAR_H - 7.0f;
    // The gauges span the CONTENT WINDOW's width — left edge to right edge —
    // so they line up with the panel below them instead of drifting with
    // whatever the FPS and battery readouts happen to occupy.
    const f32 left = FN_CORNER_MARGIN + FN_CORNER + FN_GAP;
    const f32 right = w - left;
    drawFpsReadout(8.0f, baseline);
    // drawBattery self-gates on a reported percentage, so desktop shows
    // nothing here rather than an empty glyph.
    drawBattery(w - 62.0f, (FN_TOPBAR_H - 12.0f) * 0.5f);

    // The meter lane spans the content window's width — corner to corner —
    // so the gauge's extent is fixed and the fill alone carries the reading.
    drawMeterBar(left, right, FN_TOPBAR_H * 0.5f, FN_TOPBAR_H - 10.0f, true);
}

// Chamfered plate for the four corner boxes, with its label small and inset
// in the top-left. Persistent by design: an unavailable action dims rather
// than vanishing, so the four corners never move.
void drawCornerBox(f32 x0, f32 y0, f32 x1, f32 y1, const char* label, bool enabled) {
    // A drawn beveled button — raised warm face, gold rim, top-lit edge — so
    // the corner controls read as physical buttons distinct from the item
    // slots. The BOX never changes (persistent chrome, the layout can't
    // shift); only its contents dim when the action is unavailable. The
    // press pinch is applied to the rect by the caller.
    drawBeveledCornerButton(x0, y0, x1, y1, enabled);
    if (label != NULL && label[0] != 0) {
        drawText(x0 + FN_LABEL_DX, y0 + FN_LABEL_DY + FN_CORNER_LABEL, FN_CORNER_LABEL,
            enabled ? TEXT_MAIN : TEXT_DIM, "%s", label);
    }
}

// X and Y, stacked in the middle of the right edge. Larger than the Cinematic
// diamond, with the letter moved outside the circle (top-left) so the item
// icon owns the face. X sits slightly right of Y, mirroring the controller.
void drawFunctionalItemButtons(dMeter2Draw_c* md, f32 colX, f32 y0, f32 y1) {
    if (md == NULL || !md->isButtonClusterVisible()) {
        return;
    }
    // VISUAL wolf state: the blend crossfades the faces through a pinch on
    // a transform instead of popping the frame the form flips.
    const bool wolf = s_wolfBlend > 0.5f;
    const f32 morphPinch = FN_BTN * (1.0f - fabsf(2.0f * s_wolfBlend - 1.0f)) * 0.45f;
    const int winStatus = dMeter2Info_getWindowStatus();
    const bool menuOpen = dComIfGp_isPauseFlag() || winStatus != 0;
    // The game hides X/Y whenever a menu window is up — except the item
    // wheel, where X/Y are the equip targets and stay visible.
    const bool xyHidden = menuOpen && winStatus != 2;
    // Functional treats X/Y as permanent controls rather than a HUD readout,
    // so the circle and its letter stay put and only the equipped item
    // follows the game's hide rule — a control that vanishes mid-menu reads
    // as broken when it is the thing you press.
    const bool equipMode = inEquipMode();

    // Even vertical rhythm across the whole right edge: I, X, Y, II are four
    // equally spaced slots. y0/y1 are the I and II box CENTRES (passed in
    // from the same geometry drawFunctionalCorners uses) — deriving them here
    // from already-inset bounds is what previously collapsed the step until
    // the two circles overlapped.
    const f32 step = (y1 - y0) / 3.0f;
    const f32 xTop = y0 + step - FN_BTN * 0.5f;
    const f32 yTop = y0 + step * 2.0f - FN_BTN * 0.5f;
    // Right edge shared with the corner boxes, so the whole right column
    // lines up; X sits flush there and Y is staggered left by the amount
    // that lands it exactly on the boxes' left edge.
    const f32 baseX = colX + FN_COL_W - FN_CORNER_MARGIN - FN_BTN;

    struct Entry {
        int paneIdx;
        int xy;
        const char* label;
        f32 bx, by;
    };
    const Entry entries[] = {
        {2, 0, "X", baseX, xTop},
        {3, 1, "Y", baseX - FN_XY_STAGGER, yTop},
    };
    for (const Entry& e : entries) {
        if (equipMode) {
            // The round buttons ARE the drop targets here — no rectangle.
            publishEquipDropRect(e.xy, e.bx, e.by, FN_BTN);
        }
        // Depress animation: the face shrinks toward its centre while the
        // button is held (touch or synthetic tail), easing back on release.
        // A fresh equip briefly bulges the face instead (negative pinch);
        // an active press outweighs it. Touch rect, labels and rings keep
        // the resting geometry.
        const f32 pinch =
            FN_BTN * (0.07f * s_pressAnim[e.xy] - 0.05f * s_popAnim[e.xy]) + morphPinch;
        const f32 pbx = e.bx + pinch * 0.5f;
        const f32 pby = e.by + pinch * 0.5f;
        const f32 psz = FN_BTN - pinch;
        // Circle base only — no pane composite. The composite would stamp the
        // button's own letter in the middle of the face, and the letter now
        // lives outside the circle so the item icon owns the face.
        drawButtonCircleBase(md, e.paneIdx, pbx, pby, psz);
        // Touch rect for tap-to-use (the circle's bounding box).
        s_fnXYRect[e.xy][0] = e.bx;
        s_fnXYRect[e.xy][1] = e.by;
        s_fnXYRect[e.xy][2] = e.bx + FN_BTN;
        s_fnXYRect[e.xy][3] = e.by + FN_BTN;
        const u8 itemNo =
            (wolf || xyHidden) ? (u8)dItemNo_NONE_e : dComIfGp_getSelectItem(e.xy);
        if (itemNo != dItemNo_NONE_e) {
            // Dim like the main HUD when the item can't be used right now.
            // Icon rides the depressed face (psz), staying centred.
            const f32 picon = psz * 0.74f;
            drawItemIcon(ICON_SLOT_XITEM + e.xy, itemNo, pbx + (psz - picon) * 0.5f,
                pby + (psz - picon) * 0.5f, picon,
                md->isItemUsable(e.xy) ? (u8)0xFF : (u8)128);
            const int ammo = ammoForItem(itemNo, e.xy);
            if (ammo >= 0) {
                // Bomb/arrow/seed counts read best centred under the icon
                // rather than tucked in a corner of the round face.
                drawAmmoCountCentered(ammo, e.bx + FN_BTN * 0.5f, pby + psz - 8.0f);
            }
        }
        // "Sense" / "Dig" belong with their buttons, so they read inside the
        // face. WOLF ONLY: these panes exist just for wolf form, and because
        // dual-screen hides them on the main screen their own visibility flag
        // can no longer say whether the game wants them — the string alone
        // goes stale and would show in human form too.
        const char* action = (!menuOpen && wolf) ? md->getActionTextXY(e.xy) : NULL;
        if (action != NULL && action[0] != 0) {
            // Main-screen styling: white with a dark outline, so it stays
            // legible over the button face.
            const f32 tcx = e.bx + FN_BTN * 0.5f;
            const f32 tcy = e.by + FN_BTN * 0.5f + 5.0f;
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    if (dx != 0 || dy != 0) {
                        drawTextCentered(tcx + (f32)dx, tcy + (f32)dy, 13.0f, 0x000000C0u,
                            action);
                    }
                }
            }
            drawTextCentered(tcx, tcy, 13.0f, 0xFFFFFFFFu, action);
        }
        // Letter tucked at the button's top-left, clear of the circle so the
        // item icon owns the face. Small, matching the corner-box labels.
        drawText(e.bx + FN_LABEL_DX - 6.0f, e.by + FN_LABEL_DY + FN_CORNER_LABEL,
            FN_CORNER_LABEL, TEXT_MAIN, "%s", e.label);
        // Equip ring LAST so it replaces the button's own border instead of
        // being painted over by the face.
        if (equipMode) {
            constexpr GXColor COL_RING[2] = {{110, 145, 210, 255}, {105, 190, 130, 255}};
            constexpr GXColor COL_RING_HOT = {235, 200, 90, 255};
            const bool hot = s_dragging && s_dragX >= e.bx - 8.0f &&
                s_dragX <= e.bx + FN_BTN + 8.0f && s_dragY >= e.by - 8.0f &&
                s_dragY <= e.by + FN_BTN + 8.0f;
            // Sits inside the button's edge so it reads as the border
            // glowing rather than a separate ring around it.
            drawRing(e.bx + FN_BTN * 0.5f, e.by + FN_BTN * 0.5f, FN_BTN * 0.5f - 3.0f, 5.0f,
                hot ? COL_RING_HOT : COL_RING[e.xy]);
        } else if (s_denyFlash[e.xy] > 0) {
            // Rejected tap: the border flashes red and fades, so the refusal
            // is visible on the screen the finger is on.
            const u8 a = (u8)(255 * s_denyFlash[e.xy] / DENY_FLASH_FRAMES);
            drawRing(e.bx + FN_BTN * 0.5f, e.by + FN_BTN * 0.5f, FN_BTN * 0.5f - 3.0f, 5.0f,
                {225, 62, 50, a});
        }
    }
}

// Left column: transform button on top, rupee and small-key counters, the
// dungeon item icons, and the Z button at the bottom. Mirrors the reference
// layout's camera/ocarina bookends.
// Top zone: a dark box with the game's own rupee composite, drawn larger.
// Panel that bleeds in from the screen's left edge: flush left (no margin, no
// left chamfer) with only its right corners cut, so it reads as part of the
// screen rather than a floating box.
void drawLeftBleedPanel(f32 x1, f32 y0, f32 y1) {
    constexpr GXColor FILL = {17, 16, 14, 224};
    constexpr GXColor EDGE = {74, 70, 62, 235};
    // Corner mask 2|4 = top-right | bottom-right.
    fillChamferRect(0.0f, y0, x1, y1, 10.0f, FILL, 2 | 4);
    drawChamferFrame(0.0f, y0, x1, y1, 10.0f, 1.5f, EDGE, 2 | 4);
}

// Rupee readout: gem then count, left to right. The zone panels bleed in
// from the screen's left edge, so they take only their right edge and
// vertical span.
void drawLeftRupeeBox(f32 x1, f32 y0, f32 y1) {
    drawLeftBleedPanel(x1, y0, y1);
    constexpr f32 GAP = 4.0f;
    // drawHudNumber is TOP-anchored and its digits advance at 0.9 * width.
    // The shown value chases the real one, rolling like the main HUD's
    // counter instead of jumping.
    static f32 sShownRupee = -1.0f;
    const int actualRupee = dComIfGs_getRupee();
    if (sShownRupee < 0.0f) {
        sShownRupee = (f32)actualRupee;
    } else if (sShownRupee != (f32)actualRupee) {
        f32 step = ((f32)actualRupee - sShownRupee) / 12.0f;
        if (step > 0.0f && step < 1.0f) {
            step = 1.0f;
        } else if (step < 0.0f && step > -1.0f) {
            step = -1.0f;
        }
        sShownRupee += step;
        if ((step > 0.0f && sShownRupee > (f32)actualRupee) ||
            (step < 0.0f && sShownRupee < (f32)actualRupee))
        {
            sShownRupee = (f32)actualRupee;
        }
    }
    const int rupee = (int)sShownRupee;
    int digits = 1;
    for (int v = rupee; v >= 10; v /= 10) {
        digits++;
    }
    // A four-digit wallet is wider than the column: shrink the whole group
    // to fit rather than letting it spill out of the panel.
    f32 iconS = 32.0f;
    f32 digitH = 17.0f;
    const f32 avail = x1 - 10.0f;
    f32 numW = digitH * 0.72f * (0.9f * (f32)(digits - 1) + 1.0f);
    if (iconS + GAP + numW > avail) {
        const f32 k = avail / (iconS + GAP + numW);
        iconS *= k;
        digitH *= k;
        numW = digitH * 0.72f * (0.9f * (f32)(digits - 1) + 1.0f);
    }
    const f32 gx = (x1 - (iconS + GAP + numW)) * 0.5f;
    const f32 cy = (y0 + y1) * 0.5f;
    drawItemIcon(ICON_SLOT_RUPEE, dItemNo_GREEN_RUPEE_e, gx, cy - iconS * 0.5f, iconS);
    drawHudNumber(rupee, gx + iconS + GAP, cy - digitH * 0.5f, digitH);
}

// Bottom zone, tears-quest variant: the Vessel of Light with its count under
// it. The vessel art fills as tears go in, so the icon doubles as a progress
// bar and the counter gives the exact figure. Returns false when no tears
// quest is running, so the caller can fall through.
// Same gate as the Cinematic vessel: 0 = quest not started here, 0xFF =
// already finished.
bool leftVesselAvailable() {
    const s8 darkArea = dComIfGp_getStartStageDarkArea();
    if (darkArea < 0) {
        return false;
    }
    const u8 dropFlag = dMeter2Info_getLightDropGetFlag((u8)darkArea);
    return dropFlag != 0 && dropFlag != 0xFF;
}

bool drawLeftVesselBox(dMeter2Draw_c* md, f32 x1, f32 y0, f32 y1) {
    if (md == NULL || !leftVesselAvailable()) {
        return false;
    }
    const s8 darkArea = dComIfGp_getStartStageDarkArea();
    // The vessel is LIVE on the main screen in Functional, so the composite
    // brackets an alpha-only push/pop: alphas are raised to the always-
    // visible state just for this draw and the live (possibly faded) rates
    // restored immediately after — the box shows the vessel for the whole
    // quest without pinning the main screen's fade opaque, which is what
    // the old per-frame refresh did. (Cinematic's drawVesselOfLight keeps
    // the plain refresh — its panes are hidden on main, so pinning is free.)
    constexpr f32 LABEL_H = 22.0f;
    // Full state push: the composite must see the CANONICAL vessel layout —
    // while the main screen animates the vessel (tear collected, drops
    // flying), the live pane positions scatter and the composite's fitted
    // bounds ballooned, leaving this box visually empty exactly when the
    // quest was most active.
    f32 savedAlpha[dMeter2Draw_c::VESSEL_ALPHA_SAVE_COUNT];
    f32 savedX[dMeter2Draw_c::VESSEL_ALPHA_SAVE_COUNT];
    f32 savedY[dMeter2Draw_c::VESSEL_ALPHA_SAVE_COUNT];
    f32 savedScale[2];
    md->pushVesselStateForCompanion(savedAlpha, savedX, savedY, savedScale);
    drawPaneComposite(md->getLightDropPane(), 4.0f, y0 + 4.0f, x1 - 8.0f,
        y1 - y0 - LABEL_H - 8.0f);
    md->popVesselStateForCompanion(savedAlpha, savedX, savedY, savedScale);
    dComIfGp_getCurrentGrafPort()->setup2D();
    char text[16];
    snprintf(text, sizeof(text), "%d / %d", dComIfGs_getLightDropNum(darkArea),
        dComIfGp_getNeedLightDropNum());
    drawTextCentered(x1 * 0.5f, y1 - 7.0f, 15.0f, TEXT_MAIN, text);
    return true;
}

// Bottom zone: dark box with the dungeon items in a 2x2 grid — small key
// (counter), map, compass, boss key. Returns false outside dungeons so the
// caller can offer the zone to the tears quest instead.

// Small-key display rule, shared with the Cinematic top bar (the game's own
// dMeter2_c::isKeyVisible): stages flagged for key display show the counter
// — in dungeons even at zero, fields only with keys in hand. The key pane is
// hidden on the main screen in BOTH dual-screen modes, so on flagged
// non-dungeon stages this box is the only place Functional can show it.
bool leftKeyVisible(stage_stag_info_class* stagInfo) {
    return stagInfo != NULL && dStage_stagInfo_ChkKeyDisp(stagInfo) &&
        (dStage_stagInfo_GetSTType(stagInfo) != ST_FIELD || dComIfGs_getKeyNum() != 0);
}

bool leftDungeonAvailable() {
    stage_stag_info_class* stagInfo = dComIfGp_getStage()->getStagInfo();
    if (stagInfo == NULL) {
        return false;
    }
    return dStage_stagInfo_GetSTType(stagInfo) == ST_DUNGEON || leftKeyVisible(stagInfo);
}

bool drawLeftDungeonBox(f32 x1, f32 y0, f32 y1) {
    stage_stag_info_class* stagInfo = dComIfGp_getStage()->getStagInfo();
    if (!leftDungeonAvailable()) {
        return false;
    }
    const bool dungeon = dStage_stagInfo_GetSTType(stagInfo) == ST_DUNGEON;
    const f32 cellW = x1 * 0.5f;
    const f32 icon = 31.0f;
    // Rows sit a fixed distance apart around the box's centre rather than
    // filling half its height each — halving left a gap the icons couldn't
    // justify. SHIFT_X nudges the pair off the panel's bleeding left edge.
    constexpr f32 ROW_GAP = 40.0f;
    constexpr f32 SHIFT_X = 5.0f;
    const f32 cyC = (y0 + y1) * 0.5f;
    const f32 cxL = cellW * 0.5f + SHIFT_X;
    const f32 cxR = cellW * 1.5f + SHIFT_X;
    const f32 cyT = cyC - ROW_GAP * 0.5f;
    const f32 cyB = cyC + ROW_GAP * 0.5f;

    // Reading order left-to-right, top-to-bottom: map, compass, small key,
    // boss key. Owned draws at full strength, missing as a faint ghost — no
    // highlight ring here, the plain icon is enough in this layout.
    // Dungeon-only: on flagged non-dungeon key stages the box carries just
    // the key counter (three ghost icons for items that can't exist there
    // would only mislead).
    if (dungeon) {
        const struct {
            int slot;
            u8 itemNo;
            bool owned;
            f32 cx, cy;
        } items[3] = {
            {ICON_SLOT_DMAP, dItemNo_MAP_e, dComIfGs_isDungeonItemMap() != 0, cxL, cyT},
            {ICON_SLOT_COMPASS, dItemNo_COMPUS_e, dComIfGs_isDungeonItemCompass() != 0, cxR, cyT},
            {ICON_SLOT_BOSSKEY, dItemNo_BOSS_KEY_e, dComIfGs_isDungeonItemBossKey() != 0, cxR,
                cyB},
        };
        for (const auto& it : items) {
            drawItemIcon(it.slot, it.itemNo, it.cx - icon * 0.5f, it.cy - icon * 0.5f, icon,
                it.owned ? 0xFF : 55);
        }
    }

    // Small keys — count then icon, left to right. Bottom-left cell in
    // dungeons; centred when the counter is all the box shows.
    if (leftKeyVisible(stagInfo)) {
        const s16 keyNum = dComIfGs_getKeyNum();
        constexpr f32 DIGIT_H = 15.0f;
        const f32 kIcon = 24.0f;
        const f32 digitW = DIGIT_H * 0.72f;
        const f32 numW = keyNum >= 10 ? digitW * 1.9f : digitW;
        const f32 kcx = dungeon ? cxL : cellW + SHIFT_X;
        const f32 kcy = dungeon ? cyB : cyC;
        const f32 gx = kcx - (numW + 3.0f + kIcon) * 0.5f;
        drawHudNumber(keyNum, gx, kcy - DIGIT_H * 0.5f, DIGIT_H);
        drawItemIcon(ICON_SLOT_KEY, dItemNo_SMALL_KEY_e, gx + numW + 3.0f, kcy - kIcon * 0.5f,
            kIcon);
    }
    return true;
}

// Page 1: the three long-tail collectathons, which the game otherwise buries
// in the pause menu — icon left, count right, one row each.
void drawLeftProgressBox(f32 y0, f32 y1) {
    int bugs = 0;
    for (u8 itemNo = dItemNo_M_BEETLE_e; itemNo <= dItemNo_F_MAYFLY_e; itemNo++) {
        if (dComIfGs_isItemFirstBit(itemNo)) {
            bugs++;
        }
    }
    const int poes = dComIfGs_getPohSpiritNum();
    // Whole containers out of the 20 the game can reach, with the loose
    // pieces toward the next one carried by the icon rather than a number.
    const int hearts = dComIfGs_getMaxLife() / 5;
    const int pieces = dComIfGs_getMaxLife() % 5;

    constexpr f32 ICON = 22.0f;
    const f32 rowH = (y1 - y0) / 3.0f;
    const f32 ix = 8.0f;
    const f32 tx = ix + ICON + 5.0f;
    char text[16];
    for (int row = 0; row < 3; row++) {
        const f32 cy = y0 + rowH * ((f32)row + 0.5f);
        const f32 iy = cy - ICON * 0.5f;
        switch (row) {
        case 0:
            drawItemIcon(ICON_SLOT_POE, dItemNo_POU_SPIRIT_e, ix, iy, ICON, poes > 0 ? 0xFF : 55);
            snprintf(text, sizeof(text), "%d/60", poes);
            break;
        case 1:
            drawItemIcon(ICON_SLOT_BUGPROG, dItemNo_M_BEETLE_e, ix, iy, ICON,
                bugs > 0 ? 0xFF : 55);
            snprintf(text, sizeof(text), "%d/24", bugs);
            break;
        default:
            // The pause menu's heart-PIECE art: an empty base plus one
            // cumulative wedge per piece held. The wedges divide the heart
            // visually, so the icon carries the progress toward the next
            // container while the number counts whole ones.
            if (const ResTIMG* base = collectIconTimg(CLCT_SLOT_HEART_BASE)) {
                drawTimg(base, ix, iy, ICON, ICON, 0xFF);
                for (int i = 0; i < pieces && i < 4; i++) {
                    if (const ResTIMG* wedge = collectIconTimg(CLCT_SLOT_HEART_PARTS1 + i)) {
                        drawTimg(wedge, ix, iy, ICON, ICON, 0xFF);
                    }
                }
            }
            snprintf(text, sizeof(text), "%d/20", hearts);
            break;
        }
        drawText(tx, cy + 4.0f, 12.0f, TEXT_MAIN, "%s", text);
    }
}

// Sun or crescent moon, drawn rather than sourced: the game has no HUD art
// for either. The crescent is a disc with a second disc bitten out of it in
// the panel's own fill colour.
void drawDayNightGlyph(f32 cx, f32 cy, f32 r, bool night) {
    constexpr GXColor SUN = {236, 206, 122, 255};
    constexpr GXColor MOON = {206, 214, 230, 255};
    if (night) {
        // Scanline lune rather than a disc with a second disc painted over
        // it: the panel fill is semi-transparent, so re-painting its colour
        // can't reproduce the blend behind it and the "bite" showed as a grey
        // circle. Each row is the outer circle's span clipped at the inner
        // circle's left edge.
        constexpr int STEPS = 24;
        const f32 icx = cx + r * 0.52f;
        const f32 icy = cy - r * 0.22f;
        const f32 ir = r * 0.86f;
        const f32 half = r / (f32)STEPS;
        for (int i = 0; i < STEPS; i++) {
            const f32 y = cy - r + 2.0f * r * ((f32)i + 0.5f) / (f32)STEPS;
            const f32 dy = y - cy;
            const f32 hoSq = r * r - dy * dy;
            if (hoSq <= 0.0f) {
                continue;
            }
            const f32 ho = sqrtf(hoSq);
            f32 xR = cx + ho;
            const f32 diy = y - icy;
            const f32 hiSq = ir * ir - diy * diy;
            if (hiSq > 0.0f) {
                const f32 bite = icx - sqrtf(hiSq);
                if (bite < xR) {
                    xR = bite;
                }
            }
            const f32 xL = cx - ho;
            if (xR > xL) {
                fillRect(xL, y - half, xR, y + half, MOON);
            }
        }
        return;
    }
    // Eight tapered rays around the disc.
    for (int i = 0; i < 8; i++) {
        const f32 a = 0.7853982f * (f32)i;
        const f32 ca = cosf(a);
        const f32 sa = sinf(a);
        const f32 inner = r * 1.15f;
        const f32 outer = r * 1.7f;
        const f32 wx = -sa * r * 0.16f;
        const f32 wy = ca * r * 0.16f;
        const f32 xy[8] = {cx + ca * inner + wx, cy + sa * inner + wy, cx + ca * outer,
            cy + sa * outer, cx + ca * inner - wx, cy + sa * inner - wy, cx + ca * inner,
            cy + sa * inner};
        fillPolyPublic(xy, 4, SUN);
    }
    fillDisc(cx, cy, r, SUN);
}

// Page 2: where and when. The region name is the same string the map plate
// shows, wrapped to the column width.
void drawLeftPlaceBox(f32 x1, f32 y0, f32 y1) {
    const int hour = dKy_getdaytime_hour();
    // Quantised to 5 game minutes. Not for cost — these are plain getters and
    // the panel redraws every frame regardless — but because a game minute is
    // roughly a real second here, so an exact clock churns its last digit
    // continuously for a readout you only ever glance at.
    const int minute = (dKy_getdaytime_minute() / 5) * 5;
    // TP runs its night between 18:00 and 06:00 (the same window that gates
    // the day/night NPC schedules).
    const bool night = hour < 6 || hour >= 18;

    // Nudged right off the panel's bleeding left edge, to match the other
    // pages in this zone.
    const f32 cx = x1 * 0.5f + 4.0f;
    // The sun's rays reach 1.7x the disc radius, so the glyph's real extent
    // is 1.7 * R, not R — every offset below works off that, with explicit
    // padding above and below it rather than a hand-tuned centre.
    constexpr f32 GLYPH_R = 9.0f;
    constexpr f32 GLYPH_EXT = GLYPH_R * 1.7f;
    constexpr f32 GLYPH_PAD_TOP = 13.0f;
    constexpr f32 GLYPH_PAD_BOTTOM = 8.0f;
    const f32 glyphCY = y0 + GLYPH_PAD_TOP + GLYPH_EXT;
    drawDayNightGlyph(cx, glyphCY, GLYPH_R, night);
    char clock[8];
    snprintf(clock, sizeof(clock), "%02d:%02d", hour, minute);
    // Baseline sits a cap-height below the glyph's padded bottom edge.
    const f32 clockY = glyphCY + GLYPH_EXT + GLYPH_PAD_BOTTOM + 12.0f;
    drawTextCentered(cx, clockY, 16.0f, TEXT_MAIN, clock);

    // Greedy word wrap. Names run to "Lakebed Temple"; anything longer than
    // two lines is clipped rather than overrunning the panel.
    const char* name = mapStageName();
    if (name[0] == '\0') {
        return;
    }
    constexpr f32 TS = 11.0f;
    const f32 maxW = x1 - 8.0f;
    char line[2][32];
    int lines = 0;
    int cur = 0;
    line[0][0] = '\0';
    for (const char* p = name; lines < 2;) {
        while (*p == ' ') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        const char* end = p;
        while (*end != '\0' && *end != ' ') {
            end++;
        }
        char word[32];
        const size_t wlen = (size_t)(end - p) < sizeof(word) - 1 ? (size_t)(end - p)
                                                                 : sizeof(word) - 1;
        memcpy(word, p, wlen);
        word[wlen] = '\0';
        char probe[32];
        if (line[cur][0] == '\0') {
            snprintf(probe, sizeof(probe), "%s", word);
        } else {
            snprintf(probe, sizeof(probe), "%s %s", line[cur], word);
        }
        if (measureText(TS, probe) <= maxW || line[cur][0] == '\0') {
            snprintf(line[cur], sizeof(line[0]), "%s", probe);
            lines = cur + 1;
        } else if (cur == 0) {
            cur = 1;
            snprintf(line[1], sizeof(line[1]), "%s", word);
            lines = 2;
        } else {
            break;
        }
        p = end;
    }
    const f32 ty = clockY + (lines > 1 ? 12.0f : 18.0f);
    for (int i = 0; i < lines; i++) {
        drawTextCentered(cx, ty + (f32)i * 13.0f, TS, TEXT_DIM, line[i]);
    }
}

// Page dots stacked down the zone's right edge: one per AVAILABLE page, so
// the count itself tells you whether the context page is in play. Vertical
// rather than along the bottom because the swipe that drives them is
// vertical — the indicator should read as the axis you flick along.
void drawLeftBoxDots(f32 x0, f32 x1, f32 y0, f32 y1, int idx, int count) {
    constexpr f32 R = 2.5f;
    constexpr f32 STEP = 11.0f;
    const f32 cx = (x0 + x1) * 0.5f;
    const f32 cy0 = (y0 + y1) * 0.5f - STEP * (f32)(count - 1) * 0.5f;
    for (int i = 0; i < count; i++) {
        const GXColor c = i == idx ? GXColor{230, 222, 200, 255} : GXColor{110, 104, 92, 200};
        fillDisc(cx, cy0 + STEP * (f32)i, R, c);
    }
}

// Bottom zone dispatcher: one persistent panel, swipeable pages.
void drawLeftInfoBox(dMeter2Draw_c* md, f32 x1, f32 y0, f32 y1) {
    drawLeftBleedPanel(x1, y0, y1);
    s_leftBoxRect[0] = 0.0f;
    s_leftBoxRect[1] = y0;
    s_leftBoxRect[2] = x1;
    s_leftBoxRect[3] = y1;
    // The dot column eats width now, not height.
    constexpr f32 DOTS_W = 14.0f;
    const f32 cx1 = x1 - DOTS_W;
    int pages[LEFT_BOX_PAGES];
    const int count = leftBoxPages(pages);
    // Entering a dungeon or starting a tears quest is exactly when its page
    // becomes worth looking at, so jump to it on the transition. Only on the
    // edge — after that the player's own swipe stands. The edge detector
    // freezes while stagInfo is NULL (stage loads): availability reads false
    // then, and tracking that would re-fire the jump after every in-dungeon
    // room load, overriding the player's chosen page.
    static bool s_hadContext = false;
    const bool hasContext = pages[0] == LEFT_BOX_CONTEXT;
    if (dComIfGp_getStage()->getStagInfo() != NULL) {
        if (hasContext && !s_hadContext) {
            s_leftBoxPage.store(LEFT_BOX_CONTEXT);
        }
        s_hadContext = hasContext;
    }
    // The context page drops out of the list entirely when it has nothing to
    // show, so a stored page id can go stale — fall back to the first
    // available one rather than drawing an empty panel.
    int page = s_leftBoxPage.load();
    int idx = 0;
    for (int i = 0; i < count; i++) {
        if (pages[i] == page) {
            idx = i;
            break;
        }
    }
    page = pages[idx];
    // Carousel physics: the content rides the finger during a swipe and
    // settles back after release (the page flip itself happens on release,
    // so the new page slides home from the carried offset). Clipped to the
    // panel so the shift can't spill into the zones above/below.
    if (s_leftBoxDragY != 0.0f) {
        const f32 lim = (y1 - y0) * 0.45f;
        if (s_leftBoxDragY > lim) {
            s_leftBoxDragY = lim;
        } else if (s_leftBoxDragY < -lim) {
            s_leftBoxDragY = -lim;
        }
    }
    const f32 boxOff = s_leftBoxDragY;
    if (!s_leftBoxTracking) {
        s_leftBoxDragY *= 0.68f;
        if (s_leftBoxDragY > -0.5f && s_leftBoxDragY < 0.5f) {
            s_leftBoxDragY = 0.0f;
        }
    }
    const bool boxShift = boxOff != 0.0f;
    if (boxShift) {
        GXSetScissorRender((u32)(0.0f * s_pixelScale), (u32)(y0 * s_pixelScale),
            (u32)(x1 * s_pixelScale), (u32)((y1 - y0) * s_pixelScale));
    }
    switch (page) {
    case LEFT_BOX_PROGRESS:
        drawLeftProgressBox(y0 + boxOff, y1 + boxOff);
        break;
    case LEFT_BOX_PLACE:
        drawLeftPlaceBox(cx1, y0 + boxOff, y1 + boxOff);
        break;
    default:
        // Dungeon items and the tears quest can't both be running — the tears
        // areas are fields, not dungeons — so the order is a preference, not
        // a conflict.
        if (!drawLeftDungeonBox(cx1, y0 + boxOff, y1 + boxOff)) {
            drawLeftVesselBox(md, cx1, y0 + boxOff, y1 + boxOff);
        }
        break;
    }
    if (boxShift) {
        GXSetScissorRender(0, 0, s_nativeW, s_nativeH);
    }
    drawLeftBoxDots(cx1, x1, y0, y1, idx, count);
}

void drawFunctionalLeftColumn(dMeter2Draw_c* md, f32 colX, f32 y0, f32 y1) {
    const f32 x0 = colX + FN_CORNER_MARGIN;
    const f32 x1 = colX + FN_COL_W - FN_CORNER_MARGIN;
    // Rupee box pinned to the top, dungeon box pinned to the bottom at a fixed
    // height, and the tab centred in the space BETWEEN them — so the gap above
    // the tab equals the gap below it. Anchoring the dungeon box (rather than
    // letting it fill from the tab) is what makes the two gaps independent of
    // each other; it also keeps the tab in the same spot outside dungeons,
    // where that box draws nothing.
    const f32 rupeeY1 = y0 + FN_RUPEE_H;
    const f32 boxY0 = y1 - FN_DUNGEON_H;
    const f32 tabY0 = (rupeeY1 + boxY0 - FN_CTXTAB_H) * 0.5f;
    const f32 tabY1 = tabY0 + FN_CTXTAB_H;

    drawLeftRupeeBox(x1, y0, rupeeY1);
    // Dark bridge from the tab across to the content window's left edge, so
    // the tab reads as part of the window rather than a floating plate. Same
    // fill as the window interior; drawn before the tab so the plate sits on
    // top of it. Matches drawDashboardFunctional's own inset maths.
    // Backing under the WHOLE tab, 6px proud top and bottom, running from the
    // screen's left edge across to the content window — so the tab sits on a
    // dark bed that bleeds in from the left like the panels above and below
    // it, and flows into the window on the right. Left corners chamfered to
    // echo the tab's own cut.
    // Same fill and 1.5px frame as the content window (COL_WINDOW/COL_FRAME),
    // so the bed reads as an alcove of the window rather than a separate
    // panel. Its right edge is scissored off — a closed border there would
    // wall the bed away from the window it is supposed to open into — so the
    // top and bottom rules run straight into the window's own left rule.
    constexpr f32 BLEED_OVER = 6.0f;
    constexpr f32 BLEED_CH = 14.0f;
    const f32 contentX0 = FN_CORNER_MARGIN + FN_CORNER + FN_GAP;
    if (contentX0 > x1) {
        const f32 bedY0 = tabY0 - BLEED_OVER;
        const f32 bedY1 = tabY1 + BLEED_OVER;
        fillChamferRect(0.0f, bedY0, contentX0 + 2.0f, bedY1, BLEED_CH, COL_WINDOW, 1 | 8);
        GXSetScissorRender(0, 0, (u32)(contentX0 * s_pixelScale), s_nativeH);
        drawChamferFrame(0.0f, bedY0, contentX0 + 12.0f, bedY1, BLEED_CH, 1.5f, COL_FRAME, 1 | 8);
        GXSetScissorRender(0, 0, s_nativeW, s_nativeH);
    }
    // The tab runs all the way to the content window's left edge — it is the
    // one zone that bridges into the window, so stopping short of it (where
    // the rupee and dungeon panels stop) left a notch in the join.
    drawContextTab(x0, tabY0, contentX0, tabY1);
    // The bridge bed's fill erased the content window's own left rule across
    // the tab's span, so the border went dark exactly at this one junction.
    // Redraw that rule segment (same colour/inset as drawWindowOrnaments'
    // side rule) over the seam so the window's left border reads as one
    // clean, continuous line past the plate.
    if (contentX0 > x1) {
        fillRect(contentX0 + 0.5f, tabY0 - BLEED_OVER, contentX0 + 2.5f, tabY1 + BLEED_OVER,
            COL_SIDE_RULE);
    }
    drawLeftInfoBox(md, x1, boxY0, y1);
}

// The four persistent corner boxes: transform top-left, I top-right,
// Z bottom-left, II bottom-right. Same size, chamfered, hard against the
// screen corners. Each stays put whether or not its action is available —
// unavailable ones dim rather than disappearing.
// Wolf form: the slot buttons' items are all unusable, so the I/II corners
// become wolf readouts instead of dead item cells — I shows the scent the
// wolf currently carries, II counts Poe souls (poe hunting being the
// signature senses activity). Pure readouts: the plate stays dark like an
// unequipped slot so they never read as tappable, and handleSlotTap
// swallows taps inert.
void drawWolfCornerCell(int which, f32 x0, f32 y0, f32 x1, f32 y1) {
    const f32 side = x1 - x0;
    const f32 icon = side * 0.55f;
    const f32 ix = x0 + (side - icon) * 0.5f;
    const f32 iy = y0 + (side - icon) * 0.45f;
    const f32 cx = (x0 + x1) * 0.5f;
    if (which == 0) {
        int scentSlot;
        const char* name = resolveScent(dComIfGs_getCollectSmell(), &scentSlot);
        const bool has = scentSlot >= 0;
        drawCornerBox(x0, y0, x1, y1, "Scent", false);
        const ResTIMG* t = collectIconTimg(has ? scentSlot : 3);
        if (t != NULL) {
            drawTimg(t, ix, iy, icon, icon, has ? (u8)0xFF : (u8)55);
        }
        // Name hugs the icon rather than the box's bottom edge.
        drawTextCentered(cx, iy + icon + 8.0f, 10.0f, has ? TEXT_MAIN : TEXT_DIM, name);
    } else {
        // Rolls toward the real count like the rupee readout.
        static f32 sShownPoes = -1.0f;
        const int actualPoes = dComIfGs_getPohSpiritNum();
        if (sShownPoes < 0.0f) {
            sShownPoes = (f32)actualPoes;
        } else if (sShownPoes != (f32)actualPoes) {
            sShownPoes += sShownPoes < (f32)actualPoes ? 0.34f : -0.34f;
            if (sShownPoes < 0.0f) {
                sShownPoes = 0.0f;
            }
        }
        const int poes = (int)(sShownPoes + 0.5f);
        drawCornerBox(x0, y0, x1, y1, "Poe", false);
        // Icon + count in the rupee readout's exact style — same HUD digit
        // textures and sizes (drawLeftRupeeBox) — so the two counters read
        // as one family.
        constexpr f32 GAP = 4.0f;
        int digits = 1;
        for (int v = poes; v >= 10; v /= 10) {
            digits++;
        }
        const f32 iconS = 32.0f;
        const f32 digitH = 17.0f;
        const f32 numW = digitH * 0.72f * (0.9f * (f32)(digits - 1) + 1.0f);
        const f32 gx = x0 + (side - (iconS + GAP + numW)) * 0.5f;
        // Nudged below centre so the pair clears the corner label.
        const f32 pcy = (y0 + y1) * 0.5f + 4.0f;
        drawItemIcon(ICON_SLOT_POE, dItemNo_POU_SPIRIT_e, gx, pcy - iconS * 0.5f, iconS);
        drawHudNumber(poes, gx + iconS + GAP, pcy - digitH * 0.5f, digitH);
    }
}

void drawFunctionalCorners(dMeter2Draw_c* md, f32 w, f32 h) {
    const f32 lx0 = FN_CORNER_MARGIN;
    const f32 lx1 = lx0 + FN_CORNER;
    const f32 rx1 = w - FN_CORNER_MARGIN;
    const f32 rx0 = rx1 - FN_CORNER;
    const f32 ty0 = FN_TOPBAR_H + FN_CORNER_MARGIN;
    const f32 ty1 = ty0 + FN_CORNER;
    const f32 by1 = h - FN_CORNER_MARGIN;
    const f32 by0 = by1 - FN_CORNER;

    // Top-left: wolf/human transform. Drawn as a corner box in every state;
    // the plate itself dims when the transform is blocked or unavailable.
    drawTransformCorner(lx0, ty0, lx1, ty1);

    // Top-right / bottom-right: the I and II item slots — persistent
    // bindings that fire through the host button on tap ("extra buttons").
    // Boxes stay put in every state; empty or menu-hidden slots draw dim.
    // Rects are published even when empty so taps are swallowed rather than
    // falling through to the page beneath.
    {
        const int winStatus = dMeter2Info_getWindowStatus();
        const bool xyHidden =
            (dComIfGp_isPauseFlag() || winStatus != 0) && winStatus != 2;
        const bool equipMode = inEquipMode();
        const bool wolf = s_wolfBlend > 0.5f;
        const f32 morphPin =
            FN_CORNER * (1.0f - fabsf(2.0f * s_wolfBlend - 1.0f)) * 0.45f;
        constexpr GXColor COL_SLOT_HOT = {235, 200, 90, 255};
        constexpr GXColor COL_SLOT_DROP = {110, 145, 210, 235};
        const struct {
            f32 y0, y1;
            const char* label;
        } slots[2] = {{ty0, ty1, "I"}, {by0, by1, "II"}};
        for (int i = 0; i < 2; i++) {
            if (wolf) {
                // Readout cells replace the item slots; the rect still
                // publishes so taps are swallowed instead of falling
                // through to the page (handleSlotTap keeps them inert).
                // No equip targets — equips are denied in wolf form
                // anyway.
                drawWolfCornerCell(i, rx0 + morphPin * 0.5f, slots[i].y0 + morphPin * 0.5f,
                    rx1 - morphPin * 0.5f, slots[i].y1 - morphPin * 0.5f);
                s_slotBtnRect[i][0] = rx0;
                s_slotBtnRect[i][1] = slots[i].y0;
                s_slotBtnRect[i][2] = rx1;
                s_slotBtnRect[i][3] = slots[i].y1;
                continue;
            }
            const int bound = slotBinding(i);
            // Resolved through the play mirror so a combo on the slot shows
            // as the combined item (bomb/hawk arrows), exactly like X/Y.
            const u8 item =
                bound >= 0 ? dComIfGp_getSelectItem(2 + i) : (u8)dItemNo_NONE_e;
            const bool showItem = item != dItemNo_NONE_e && !xyHidden;
            // Depress: the whole box pinches toward its centre while the
            // button is held (touch or a physical "Use Slot" bind), like
            // the X/Y circles; a fresh equip bulges it instead (negative
            // pinch). Touch rects and equip frames keep the resting
            // geometry.
            const f32 pin =
                FN_CORNER * (0.07f * s_pressAnim[2 + i] - 0.05f * s_popAnim[2 + i]) +
                morphPin;
            const f32 px0 = rx0 + pin * 0.5f;
            const f32 py0 = slots[i].y0 + pin * 0.5f;
            const f32 px1 = rx1 - pin * 0.5f;
            const f32 py1 = slots[i].y1 - pin * 0.5f;
            const f32 pside = px1 - px0;
            drawCornerBox(px0, py0, px1, py1, slots[i].label, showItem);
            if (showItem) {
                // Real per-button usability: the slots are item buttons 2/3
                // now, so Link's own polling dims them exactly like X/Y.
                const f32 icon = pside * 0.62f;
                drawItemIcon(ICON_SLOT_FN1 + i, item, px0 + (pside - icon) * 0.5f,
                    py0 + (pside - icon) * 0.55f, icon,
                    md != NULL && md->isItemUsable(2 + i) ? (u8)0xFF : (u8)128);
                const int ammo = ammoForItem(item, 2 + i);
                if (ammo >= 0) {
                    drawAmmoCountCentered(ammo, px0 + pside * 0.5f, py1 - 10.0f);
                }
            }
            s_slotBtnRect[i][0] = rx0;
            s_slotBtnRect[i][1] = slots[i].y0;
            s_slotBtnRect[i][2] = rx1;
            s_slotBtnRect[i][3] = slots[i].y1;
            if (equipMode) {
                // The whole box is a drop/tap equip target, ringed like the
                // X/Y buttons so it reads as one.
                s_dropRect[DROP_TARGET_SLOT1 + i][0] = rx0;
                s_dropRect[DROP_TARGET_SLOT1 + i][1] = slots[i].y0;
                s_dropRect[DROP_TARGET_SLOT1 + i][2] = rx1;
                s_dropRect[DROP_TARGET_SLOT1 + i][3] = slots[i].y1;
                s_dropRectValid = true;
                const bool hot = s_dragging && s_dragX >= rx0 - 8.0f &&
                    s_dragX <= rx1 + 8.0f && s_dragY >= slots[i].y0 - 8.0f &&
                    s_dragY <= slots[i].y1 + 8.0f;
                drawChamferFrame(rx0, slots[i].y0, rx1, slots[i].y1, FN_CORNER_CHAMFER,
                    3.0f, hot ? COL_SLOT_HOT : COL_SLOT_DROP, 1 | 2 | 4 | 8);
            } else if (s_denyFlash[2 + i] > 0) {
                // Rejected tap: red border flash, mirroring the X/Y ring.
                const u8 a = (u8)(255 * s_denyFlash[2 + i] / DENY_FLASH_FRAMES);
                drawChamferFrame(rx0, slots[i].y0, rx1, slots[i].y1, FN_CORNER_CHAMFER,
                    3.0f, {225, 62, 50, a}, 1 | 2 | 4 | 8);
            }
        }
    }

    // Bottom-left: Z — a real Z press (talking to Midna, camera, menu Z
    // actions), injected at the pad by mDoCPd_c::read.
    J2DPane* midna = md != NULL ? md->getMidnaButtonPaneRaw() : NULL;
    const bool midnaActive = midna != NULL && midna->getAlpha() != 0;
    // Tap pulse: the box pinches in and eases back, like the X/Y circles.
    const f32 zPin = FN_CORNER * 0.07f * s_pressAnim[5];
    const f32 zx0 = lx0 + zPin * 0.5f;
    const f32 zy0 = by0 + zPin * 0.5f;
    const f32 zx1 = lx1 - zPin * 0.5f;
    const f32 zy1 = by1 - zPin * 0.5f;
    drawCornerBox(zx0, zy0, zx1, zy1, "Z", midnaActive);
    if (midna != NULL) {
        // Midna's own HUD portrait, so the box reads as "talk to Midna"
        // rather than a bare letter — full alpha when she wants attention,
        // dimmed otherwise, exactly like the Cinematic Z button. Sized to
        // match the X/Y buttons' equipped-item icon (FN_BTN * 0.74).
        // Square box: drawPaneComposite fits the subtree inside it preserving
        // aspect, so a short box was capping the portrait well under the
        // intended size. Sized to fill the corner box like the X/Y item icons
        // fill theirs.
        const f32 pw = (zx1 - zx0) * 0.92f;
        drawPaneComposite(midna, zx0 + ((zx1 - zx0) - pw) * 0.5f,
            zy0 + ((zy1 - zy0) - pw) * 0.5f, pw, pw, midnaActive ? (u8)0 : (u8)100);
    }
    s_zBtnRect[0] = lx0;
    s_zBtnRect[1] = by0;
    s_zBtnRect[2] = lx1;
    s_zBtnRect[3] = by1;
}

}  // namespace

// Pages available right now, in display order. The context page is only in
// the list when it has something to show, so the carousel is two pages in a
// plain field and three in a dungeon or a twilight region.
int leftBoxPages(int* o_pages) {
    int n = 0;
    if (leftDungeonAvailable() || leftVesselAvailable()) {
        o_pages[n++] = LEFT_BOX_CONTEXT;
    }
    o_pages[n++] = LEFT_BOX_PLACE;
    o_pages[n++] = LEFT_BOX_PROGRESS;
    return n;
}


// --- Public API ----------------------------------------------------------

// Advances the detail pop animation and, while it runs, narrows the caller's
// rect to the animated box (drawing the box itself). Returns false exactly
// once, on the frame a CLOSE finishes — the caller then clears its own
// "detail is open" state. Content is suppressed until the box has room.
bool readerZoomStep(f32* io_x0, f32* io_y0, f32* io_x1, f32* io_y1) {
    if (s_readerZoomClosing) {
        s_readerZoomT *= 0.70f;
        if (s_readerZoomT < 0.06f) {
            s_readerZoomClosing = false;
            s_readerZoomT = 1.0f;
            return false;
        }
    } else if (s_readerZoomT < 1.0f) {
        s_readerZoomT += (1.0f - s_readerZoomT) * 0.30f;
        if (s_readerZoomT > 0.97f) {
            s_readerZoomT = 1.0f;
        }
    }
    if (s_readerZoomT >= 1.0f || s_readerZoomFrom[2] <= s_readerZoomFrom[0]) {
        return true;
    }
    const f32 t = s_readerZoomT;
    const f32 ax0 = s_readerZoomFrom[0] + (*io_x0 - s_readerZoomFrom[0]) * t;
    const f32 ay0 = s_readerZoomFrom[1] + (*io_y0 - s_readerZoomFrom[1]) * t;
    const f32 ax1 = s_readerZoomFrom[2] + (*io_x1 - s_readerZoomFrom[2]) * t;
    const f32 ay1 = s_readerZoomFrom[3] + (*io_y1 - s_readerZoomFrom[3]) * t;
    drawDetailBox(ax0, ay0, ax1, ay1);
    *io_x0 = ax0;
    *io_y0 = ay0;
    *io_x1 = ax1;
    *io_y1 = ay1;
    return true;
}

// Records where a detail view should grow from (the tapped row / item cell)
// and arms the grow.
void readerZoomOpenFrom(f32 x0, f32 y0, f32 x1, f32 y1) {
    s_readerZoomFrom[0] = x0;
    s_readerZoomFrom[1] = y0;
    s_readerZoomFrom[2] = x1;
    s_readerZoomFrom[3] = y1;
    s_readerZoomT = 0.0f;
    s_readerZoomClosing = false;
}

// The permanent context tab: selected-plate style when its action is
// available (clickable), unselected when not; content is the resolved action.
// Publishes s_ctxTabRect. Functional draws it in the left column; Cinematic
// draws it at the old in-window button positions. The Floor overlay is drawn
// separately by the caller (Functional: dashboard; Cinematic: map content).
void drawContextTab(f32 x0, f32 y0, f32 x1, f32 y1) {
    bool clickable = false;
    const int action = contextTabAction(&clickable);
    // 3DS: left corners cut to echo the screen edge, right square so the
    // plate runs flush into the bridge across to the window. Wii U draws
    // this same plate as a FLOATING in-window button, where that left
    // chamfer has nothing to hang off — so it stays a plain rectangle there.
    const int mask = dualscreen::mainHudRestored() ? (1 | 8) : 0;
    drawChamferPlate(x0, y0, x1, y1, 12.0f, clickable, mask);
    switch (action) {
    case CTX_WARP:
        drawWarpTab(x0, y0, x1, y1, clickable);
        break;
    case CTX_FLOOR:
        drawFloorTab(x0, y0, x1, y1, clickable);
        break;
    case CTX_INFO:
        drawTextCentered((x0 + x1) * 0.5f, (y0 + y1) * 0.5f + 5.0f, 13.0f,
            clickable ? TEXT_TAB_ACTIVE : TEXT_DIM, "Info");
        break;
    case CTX_HOME:
        drawTextCentered((x0 + x1) * 0.5f, (y0 + y1) * 0.5f + 5.0f, 13.0f,
            clickable ? TEXT_TAB_ACTIVE : TEXT_DIM, "< Collect");
        break;
    case CTX_BACK:
        drawTextCentered((x0 + x1) * 0.5f, (y0 + y1) * 0.5f + 5.0f, 13.0f, TEXT_TAB_ACTIVE,
            "Back");
        break;
    default:
        break;
    }
    s_ctxTabRect[0] = x0;
    s_ctxTabRect[1] = y0;
    s_ctxTabRect[2] = x1;
    s_ctxTabRect[3] = y1;
}

void drawSplash(f32 w, f32 h) {
    // No dashboard, no touch pass: drop queued taps so one from the title
    // screen can't land on stale rects the first frame gameplay resumes.
    s_pendingTouch.exchange(~0u);
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

int visiblePages(int* o_pages) {
    // Functional mirrors the reference layout: three tabs with the map in the
    // middle. QUEST has no tab there — achievements stay a Cinematic feature.
    if (dualscreen::mainHudRestored()) {
        o_pages[0] = PAGE_COLLECTION;
        o_pages[1] = PAGE_MAP;
        o_pages[2] = PAGE_INVENTORY;
        return 3;
    }
    for (int i = 0; i < PAGE_COUNT; i++) {
        o_pages[i] = i;
    }
    return PAGE_COUNT;
}

void nextPage() {
    int pages[TAB_RECT_MAX];
    const int count = visiblePages(pages);
    // CAS loop: called from both the Android UI thread and the game thread.
    // The successor is recomputed per attempt, since a lost race means the
    // page moved under us.
    int page = s_page.load();
    for (;;) {
        int idx = 0;
        for (int i = 0; i < count; i++) {
            if (pages[i] == page) {
                idx = i;
                break;
            }
        }
        if (s_page.compare_exchange_weak(page, pages[(idx + 1) % count])) {
            break;
        }
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
        // Decided at the ingress, not in the per-frame drag pass: a pinch can
        // cancel the gesture before that pass ever runs, and pinchZoom needs
        // the same answer.
        const f32 lx = u * s_canvasW;
        const f32 ly = v * s_canvasH;
        s_downOnContent = s_contentRect[2] > s_contentRect[0] && lx >= s_contentRect[0] &&
            lx <= s_contentRect[2] && ly >= s_contentRect[1] && ly <= s_contentRect[3];
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
    // Same rule as the pan: the gesture has to have started over the map.
    if (s_downOnContent && factor > 0.2f && factor < 5.0f) {
        s_mapPinchDeltaMilli.fetch_add((int)((factor - 1.0f) * 1000.0f));
        // A manual pinch takes the view back from the reset glide (benign
        // cross-thread bool clear, like the other gesture flags).
        s_mapResetGlide = false;
    }
}

bool consumeTransformRequest() {
    return s_transformReq.exchange(false);
}

bool consumeZPress() {
    return s_zPressReq.exchange(false);
}

unsigned padHoldMask() {
    return s_padHoldMaskState.load();
}

unsigned slotTriggerBits() {
    return s_slotTrig;
}

unsigned slotHoldBits() {
    return s_slotHold;
}

// I/II slot bindings live in the REAL savedata select-item indices 2/3, so
// they persist with the game save and Link's code sees the slots as
// ordinary item buttons.
int slotBinding(int i_which) {
    const u8 idx = dComIfGs_getSelectItemIndex(2 + i_which);
    return idx < MAX_ITEM_SLOTS ? (int)idx : -1;
}

void setSlotBinding(int i_which, int i_slot) {
    // Slots never carry a combo mix.
    dComIfGs_setMixItemIndex(2 + i_which, dItemNo_NONE_e);
    dComIfGs_setSelectItemIndex(2 + i_which,
        i_slot < 0 ? (u8)dItemNo_NONE_e : (u8)i_slot);
}

bool inEquipMode() {
    const bool wolf = companionWolf();
    return s_page.load() == PAGE_INVENTORY && s_itemInfoSlot < 0 &&
        (s_dragging || s_selSlot >= 0) && !wolf && !anyMenuOpen();
}



bool consumeWarpRequest() {
    return s_warpReq.exchange(false);
}

void requestWarpToggle() {
    s_warpToggleReq.store(true);
}

void beginFrameCompanionInput() {
    s_warpToggleLive = s_warpToggleReq.exchange(false);

    // Touch item buttons, ticked at the GAME frame rate (the pad's rate).
    // A menu opening (full gate: the wheel and the item-explain window
    // included — item-button presses mean equip/close there) cancels any
    // held button. The X/Y buttons only exist in Functional; the slot
    // buttons are global (action binds work in any mode).
    const bool injectBlocked =
        anyMenuOpen() || dMeter2Info_getItemExplainWindowStatus() != 0;
    // Events cancel holds too: a finger resting on a button when a cutscene
    // starts must not keep the pad bit held through it (new input is
    // already swallowed by the dim gate).
    if (s_holdBtn >= 0 &&
        (injectBlocked || dComIfGp_event_runCheck() ||
            (s_holdBtn < 2 && !dualscreen::mainHudRestored())))
    {
        s_holdBtn = -1;
        s_holdReleaseReq = false;
        s_padHoldMaskState.store(0);
        s_slotHoldMaskState.store(0);
    }
    if (s_holdBtn >= 0) {
        s_holdFrames++;
        // Deferred short-tap release: the press lands as a deliberate
        // TAP_HOLD_FRAMES hold (ball & chain cancels if released during its
        // wind-up), then releases — which is what fires bow-class items.
        if (s_holdReleaseReq && s_holdFrames >= TAP_HOLD_FRAMES) {
            s_holdBtn = -1;
            s_holdReleaseReq = false;
            s_padHoldMaskState.store(0);
            s_slotHoldMaskState.store(0);
        }
    }

    // Latch the slot buttons' hold mask (touch + "Use Slot" action binds)
    // and edge-compute the trigger bits; daAlink_c::setStickData ORs them
    // into Link's item masks later this same frame.
    static u32 sPrevSlotHold = 0;
    u32 hold = injectBlocked ? 0 : s_slotHoldMaskState.load();
    // Wolf form matches the touch path: the slots are readouts there, so the
    // physical "Use Slot" binds go inert too rather than injecting item
    // presses Link's wolf code never expects.
    if (!injectBlocked && !companionWolf()) {
        if (dusk::getActionBindHold(dusk::ActionBinds::USE_SLOT_ITEM_1, 0)) {
            hold |= 1;
        }
        if (dusk::getActionBindHold(dusk::ActionBinds::USE_SLOT_ITEM_2, 0)) {
            hold |= 2;
        }
    }
    s_slotTrig = hold & ~sPrevSlotHold;
    s_slotHold = hold;
    sPrevSlotHold = hold;

    // Rejected-tap flashes fade on the same clock as the hold plumbing.
    for (int i = 0; i < 4; i++) {
        if (s_denyFlash[i] > 0) {
            s_denyFlash[i]--;
        }
    }

    // Press-depress animation. Item buttons track their LIVE hold (touch
    // hold incl. the deferred-tap tail, or a physical "Use Slot" bind for
    // the slots) — fast attack so the press lands instantly, softer release
    // so the pop-back reads. Transform/Z are tap pulses set by handleTouch.
    for (int i = 0; i < 4; i++) {
        const bool held =
            s_holdBtn == i || (i >= 2 && (s_slotHold & (1u << (i - 2))) != 0);
        if (held) {
            s_pressAnim[i] += (1.0f - s_pressAnim[i]) * 0.5f;
            if (s_pressAnim[i] > 0.97f) {
                s_pressAnim[i] = 1.0f;
            }
        } else {
            s_pressAnim[i] *= 0.65f;
            if (s_pressAnim[i] < 0.03f) {
                s_pressAnim[i] = 0.0f;
            }
        }
    }
    for (int i = 4; i < 6; i++) {
        s_pressAnim[i] *= 0.82f;
        if (s_pressAnim[i] < 0.03f) {
            s_pressAnim[i] = 0.0f;
        }
    }

    // ARM: a scene change has begun or is still running.
    //
    // fopOvlpM_IsDoingReq (NOT IsPeek) is the load-spanning bit. The overlap
    // request runs seven phases; IsPeek only reports phases 3-4, and the
    // snapshot wipes the overworld uses signal their Done ~24 frames into a
    // 26-frame arrival fade — so the tail of every load sat outside IsPeek
    // and the dashboard woke onto the loading map. IsDoingReq is set inside
    // fopScnM_ChangeReq itself and cleared only when the overlap process is
    // finally deleted, so it brackets the entire change with no interior
    // holes. (Never call fopOvlpM_IsDone here — it CONSUMES the completion
    // token and would hang the scene change.)
    //
    // The others cover the head of the change (next stage armed before the
    // request exists) and the torn-down middle. leftGameplay only fires for
    // quit-to-title / game-over / file-select — a location change requests
    // PLAY_SCENE, so it stays false throughout one.
    const bool armedNow = dComIfGp_isEnableNextStage() || fopOvlpM_IsDoingReq() != 0 ||
        dComIfGp_getStage()->getStagInfo() == NULL || !hudReady() ||
        dualscreen::leftGameplay();

    // Equip landing pop: a button whose SELECT changed to a real item
    // (wheel or companion equip) pops briefly so the eye finds where the
    // item went. Only during live gameplay — save/stage loads rewrite the
    // indices wholesale and must not fire it, and a map change must leave
    // the I/II/X/Y buttons perfectly still.
    {
        static u8 sPrevSel[4] = {0xFF, 0xFF, 0xFF, 0xFF};
        const bool live = meterDraw() != NULL && !s_inSceneChange;
        for (int b = 0; b < 4; b++) {
            const u8 sel = dComIfGs_getSelectItemIndex(b);
            if (live && sel != sPrevSel[b] && sel < MAX_ITEM_SLOTS &&
                sPrevSel[b] != 0xFE)
            {
                s_popAnim[b] = 1.0f;
            }
            sPrevSel[b] = live ? sel : (u8)0xFE;  // 0xFE = "was not live"
        }
        for (int b = 0; b < 4; b++) {
            s_popAnim[b] *= 0.8f;
            if (s_popAnim[b] < 0.03f) {
                s_popAnim[b] = 0.0f;
            }
        }
    }

    // Second-screen dim, built to match the MAIN screen's fade DURATION on
    // the way down and stay held (no flash) until real gameplay is back.
    //   - `cover` = the main screen's live coverage this frame (JUTFader wipe
    //     alpha, mDoGph event-fade alpha gated on isFade, iris-wipe rate). It
    //     ramps at the game's own fade speed.
    //   - `held` = a load/transition owns the display: HUD torn down, next
    //     stage armed, stage not resident, loading overlap active, OR the
    //     most-recent scene isn't the play scene (leftGameplay — this is what
    //     keeps the hold up across the intermediate loading-MAP screen, whose
    //     own fade-in was brightening the dashboard early: bright→dim→bright).
    // While held the dim RATCHETS UP to `cover` — it tracks the fade-out at
    // the main screen's exact pace (so it isn't "too fast") but never falls,
    // so the wipe dips between load phases can't flash it bright. Once the
    // play scene is truly back (held clears), it follows `cover` DOWN, so the
    // final fade-in brightens the dashboard in sync with the main screen.
    const u8 faderA = mDoGph_gInf_c::getFader() != NULL
        ? mDoGph_gInf_c::getFader()->mColor.a : 0;
    f32 cover = (f32)faderA / 255.0f;
    if (mDoGph_gInf_c::isFade() != 0) {
        const f32 ev = (f32)mDoGph_gInf_c::getFadeColor().a / 255.0f;
        cover = cover > ev ? cover : ev;
    }
    if (dDlst_list_c::mWipe != 0) {
        const f32 wipe = dDlst_list_c::getWipeRate();
        cover = cover > wipe ? cover : wipe;
    }
    cover = cover < 0.0f ? 0.0f : (cover > 1.0f ? 1.0f : cover);
    // "The picture is not fully back yet." JUTFader parks in Wait — the sole
    // terminal state of a completed fade-IN — so anything else means a fade
    // is still on screen. This is what closes the tail the overlap request
    // no longer covers (the snapshot wipe's last ~24 frames).
    const JUTFader* fader = mDoGph_gInf_c::getFader();
    const bool covered =
        (fader != NULL && fader->getStatus() != JUTFader::Wait) || cover > 0.0f;
    // LATCH: rises the instant anything says "changing", and releases only
    // when the change is fully retired AND the picture is back AND the player
    // actor exists again (it is created by dStage_playerInit, well after the
    // stage loads). A brief gap in any single signal can no longer wake the
    // dashboard — that was the whole bug.
    if (armedNow) {
        s_inSceneChange = true;
    } else if (s_inSceneChange && !covered && dComIfGp_getLinkPlayer() != NULL) {
        s_inSceneChange = false;
    }
    const bool held = s_inSceneChange;
    // Rise is deliberately capped at roughly HALF the game's own fade rate,
    // so the dashboard takes about twice as long to go dark as the main
    // screen does. It still ratchets (never falls while held), so the wipe
    // dipping between load phases cannot brighten it.
    constexpr f32 RISE_STEP = 0.0125f;
    constexpr f32 WAKE_STEP = 0.028f;
    static bool sReleasing = false;
    static bool sPrevHeld = false;
    if (held) {
        sReleasing = false;
        // Creep up on its own slow clock, but NEVER sit brighter than the
        // main screen already is. That second half matters more than it
        // looks: once the meter is torn down the companion stops redrawing
        // entirely (endHudCapture bails on !hudReady) and the aux screen
        // FREEZES on its last frame. A slow ramp alone was still only part
        // way down at that moment, so the loading map appeared beside a
        // frozen half-bright dashboard — with whatever button pop happened
        // to be mid-flight frozen into it. Tracking `cover` guarantees full
        // dark by the time the screen is covered, which is strictly before
        // the meter dies.
        f32 rise = s_dimHold + RISE_STEP;
        if (rise > 1.0f) {
            rise = 1.0f;
        }
        const f32 target = cover > rise ? cover : rise;
        if (target > s_dimHold) {
            s_dimHold = target;
        }
        // Belt and braces: the frame we freeze on must be fully dark.
        if (!hudReady()) {
            s_dimHold = 1.0f;
        }
    } else {
        if (sPrevHeld && s_dimHold > 0.0f) {
            sReleasing = true;  // the load just ended — start the one brighten
        }
        if (sReleasing) {
            // The ONE wake, on its own clock. The latch only lets go once the
            // main screen is already fully back, so there is no live fade
            // left to follow down — chasing `cover` here just snapped the
            // dashboard bright in a single frame. Ease it up instead
            // (~0.6s), monotonically, so nothing can re-dim mid-wake.
            s_dimHold -= WAKE_STEP;
            if (s_dimHold <= 0.001f) {
                s_dimHold = 0.0f;
                sReleasing = false;
            }
        } else {
            // Idle: mirror cutscene/event fades exactly.
            s_dimHold = cover;
        }
    }
    sPrevHeld = held;
    if (dComIfGp_event_runCheck()) {
        s_eventDim = s_eventDim > 0.95f ? 1.0f : s_eventDim + 0.05f;
    } else {
        s_eventDim = s_eventDim < 0.05f ? 0.0f : s_eventDim - 0.05f;
    }

    // Wolf morph blend: the right column crossfades through a pinch as the
    // form changes; a confirm buzz marks the transform taking hold.
    {
        static bool sPrevWolfForm = false;
        const bool wolfNow = companionWolf();
        if (wolfNow != sPrevWolfForm) {
            sPrevWolfForm = wolfNow;
            queueHaptic(HAPTIC_CONFIRM);
        }
        const f32 target = wolfNow ? 1.0f : 0.0f;
        if (s_wolfBlend < target) {
            s_wolfBlend += 0.12f;
            if (s_wolfBlend > target) {
                s_wolfBlend = target;
            }
        } else if (s_wolfBlend > target) {
            s_wolfBlend -= 0.12f;
            if (s_wolfBlend < target) {
                s_wolfBlend = target;
            }
        }
    }

    // Low-gauge warning: one double-tick when oil or oxygen first crosses
    // its danger line (matching the bar's pulse thresholds); re-arms after
    // recovering a margin above it.
    {
        dMeter2Draw_c* mdw = meterDraw();
        static bool sOxLow = false, sOilLow = false;
        static int sWarnEcho = 0;
        const bool oxActive =
            mdw != NULL && mdw->isOxygenActive() && dComIfGp_getMaxOxygen() > 0;
        const f32 oxF =
            oxActive ? (f32)dComIfGp_getNowOxygen() / (f32)dComIfGp_getMaxOxygen() : 1.0f;
        bool lanternEquipped = false;
        for (int b = 0; b < 4; b++) {
            if (dComIfGp_getSelectItem(b) == dItemNo_KANTERA_e) {
                lanternEquipped = true;
                break;
            }
        }
        const bool oilShown = mdw != NULL && lanternEquipped && !companionWolf() &&
            dComIfGs_getMaxOil() > 0;
        const f32 oilF =
            oilShown ? (f32)dComIfGs_getOil() / (f32)dComIfGs_getMaxOil() : 1.0f;
        bool warn = false;
        if (oxF <= 0.25f && !sOxLow) {
            sOxLow = true;
            warn = true;
        } else if (oxF > 0.30f) {
            sOxLow = false;
        }
        if (oilF <= 0.15f && !sOilLow) {
            sOilLow = true;
            warn = true;
        } else if (oilF > 0.20f) {
            sOilLow = false;
        }
        if (warn) {
            queueHaptic(HAPTIC_LIGHT);
            sWarnEcho = 8;
        }
        if (sWarnEcho > 0 && --sWarnEcho == 0) {
            queueHaptic(HAPTIC_LIGHT);
        }
    }

    // Taking a hit yanks the COLLECT page out of any reader/detail view:
    // reading mail is not worth dying over, and the player's next glance
    // down should meet the overview, not a letter. The game's own damage
    // audio is the cue — no extra sound.
    {
        static u16 sPrevLife = 0xFFFF;
        const u16 lifeNow = meterDraw() != NULL ? dComIfGs_getLife() : (u16)0xFFFF;
        const bool tookHit = lifeNow != 0xFFFF && sPrevLife != 0xFFFF && lifeNow < sPrevLife;
        if (tookHit && s_page.load() == PAGE_COLLECTION &&
            (s_collectTab.load() != 0 || s_readerSel >= 0))
        {
            s_readerSel = -1;
            s_readerTapCand = -1;
            s_scrollBody = 0.0f;
            s_collectTab.store(0);
            s_collectSel = -1;
            s_scrollSkills = 0.0f;
            s_scrollMail = 0.0f;
            s_collectZoomT = 1.0f;
            s_collectZoomClosing = false;
        }
        // The ITEMS page's item-info reader is the same "reading while being
        // attacked" hazard — close it too.
        if (tookHit && s_page.load() == PAGE_INVENTORY && s_itemInfoSlot >= 0) {
            s_itemInfoSlot = -1;
            s_scrollItemInfo = 0.0f;
        }
        sPrevLife = lifeNow;
    }

    // Save hygiene: retail (and pre-guard PC builds) park the learned
    // scent's ITEM NUMBER in select index 2 — on PC that field is slot I's
    // binding, which must be an inventory SLOT index. Clear out-of-range
    // residue so a stale scent from an old save can't read as a binding.
    for (int s = 2; s < 4; s++) {
        const u8 sel = dComIfGs_getSelectItemIndex(s);
        if (sel != dItemNo_NONE_e && sel >= MAX_ITEM_SLOTS) {
            dComIfGs_setMixItemIndex(s, dItemNo_NONE_e);
            dComIfGs_setSelectItemIndex(s, dItemNo_NONE_e);
        }
    }

    // The item wheel knows nothing about the slot buttons: if it just
    // equipped (or comboed) an item that a slot also holds, the slot yields
    // — an item lives in exactly one place. A slot whose combo PARTNER moved
    // to X/Y dissolves just the combo and keeps its own item.
    for (int s = 2; s < 4; s++) {
        const u8 xySel[4] = {dComIfGs_getSelectItemIndex(0), dComIfGs_getSelectItemIndex(1),
            dComIfGs_getMixItemIndex(0), dComIfGs_getMixItemIndex(1)};
        auto onXY = [&xySel](u8 idx) {
            return idx < MAX_ITEM_SLOTS &&
                (idx == xySel[0] || idx == xySel[1] || idx == xySel[2] || idx == xySel[3]);
        };
        if (onXY(dComIfGs_getSelectItemIndex(s))) {
            dComIfGs_setMixItemIndex(s, dItemNo_NONE_e);
            dComIfGs_setSelectItemIndex(s, dItemNo_NONE_e);
        } else if (onXY(dComIfGs_getMixItemIndex(s))) {
            dComIfGs_setMixItemIndex(s, dItemNo_NONE_e);
            dComIfGp_setSelectItem(s);
        }
    }
}

bool warpTogglePressed() {
    return s_warpToggleLive;
}

// True while the game's field map is already showing the portals (its Z
// toggle is "on"). Drives the companion button's active-vs-idle styling.
bool warpPortalsShown() {
    if (!isFieldMapScreen()) {
        return false;
    }
    dMw_c* mw = dMeter2Info_getMenuWindowClass();
    if (mw == NULL || mw->getMenuFmap() == NULL) {
        return false;
    }
    return mw->getMenuFmap()->isWarpMapMode();
}

// M_021 = "first portal warp": the game uses this same bit to decide whether
// its own warp button exists at all (dMenu_Fmap2DTop_c::isWarpAccept).
bool warpUnlocked() {
    return dComIfGs_isEventBit(dSv_event_flag_c::M_021) != 0;
}

bool warpAllowed() {
    if (!warpUnlocked() || dComIfGp_event_runCheck()) {
        return false;
    }
    // Resolve the stage ourselves before going near checkAcceptWarp. That
    // predicate reaches checkField()/checkCastleTown(), which dereference
    // getStagInfo() with no NULL check — and the pointer is NULL during loads
    // and room transitions. The game only ever calls it from the map screen,
    // where that cannot happen; we call it every frame from the paint pass,
    // including the frame dMw_c::key_wait_init clears the window status and
    // pause flag but has not yet finished tearing the menu down.
    // Exiting here also makes dungeons free — they can never warp anyway.
    stage_stag_info_class* stagInfo = dComIfGp_getStage()->getStagInfo();
    if (stagInfo == NULL) {
        return false;
    }
    const u32 stageType = dStage_stagInfo_GetSTType(stagInfo);
    if (stageType != ST_FIELD && stageType != ST_CASTLE_TOWN) {
        return false;
    }
    daAlink_c* alink = daAlink_getAlinkActorClass();
    // checkAcceptWarp dereferences the Midna actor unconditionally. The game
    // only ever reaches it from the map screen, where Midna is guaranteed
    // loaded; a per-frame companion caller has no such guarantee.
    if (alink == NULL || daPy_py_c::getMidnaActor() == NULL) {
        return false;
    }
    return alink->checkAcceptWarp();
}

int contextTabAction(bool* o_clickable) {
    bool clickable = false;
    int action = CTX_NONE;
    switch (s_page.load()) {
    case PAGE_MAP:
        if (s_dmapAvailable) {
            // In a dungeon: floor select. Navigation, never gated.
            action = CTX_FLOOR;
            clickable = true;
        } else {
            // Overworld/town: warp. Shown even when locked so the slot reads
            // as "Warp" — just dim and inert until it can actually start. The
            // field-map Z toggle (isFieldMapScreen) counts as available too.
            // Any OTHER menu (start screen, item wheel, submenus) makes it
            // inert: posting setMapStatus(3) under an open menu is a state
            // the game never reaches on its own — the retired in-map button
            // hid itself for the same reason.
            action = CTX_WARP;
            clickable = isFieldMapScreen() || (warpAllowed() && !anyMenuOpen());
        }
        break;
    case PAGE_INVENTORY:
        // Opening the detail turns the tab into its own way out, so the
        // reader needs no Back button of its own.
        if (s_itemInfoSlot >= 0) {
            action = CTX_BACK;
            clickable = true;
        } else {
            action = CTX_INFO;
            clickable = s_selSlot >= 0 && dComIfGs_getItem(s_selSlot, false) != dItemNo_NONE_e;
        }
        break;
    case PAGE_COLLECTION: {
        const int view = s_collectTab.load();
        if (view == 0) {
            // Overview: the library icon row does the navigating.
            break;
        }
        // Any section, list or detail: the tab is the way HOME to the
        // library overview. Stepping one level back (detail -> list) is the
        // header's own Back button; entering a Skills/Mail entry is done by
        // tapping its row.
        action = CTX_HOME;
        clickable = true;
        break;
    }
    default:
        break;
    }
    if (o_clickable != NULL) {
        *o_clickable = clickable;
    }
    return action;
}

void queueSound(unsigned sfxId, int haptic) {
    if (s_soundQueueCount < SOUND_QUEUE_MAX) {
        s_soundQueue[s_soundQueueCount].sfx = sfxId;
        s_soundQueue[s_soundQueueCount].haptic = haptic;
        s_soundQueueCount++;
    }
}

void queueHaptic(int haptic) {
    queueSound(0, haptic);
}

void flushQueuedSounds() {
    // seStartMenu self-gates on the audio system being up and on the
    // audio.menuSounds setting, so no extra guard is needed here.
    int haptic = HAPTIC_NONE;
    for (int i = 0; i < s_soundQueueCount; i++) {
        // sfx 0 = haptic-only entry (queueHaptic).
        if (s_soundQueue[i].sfx != 0) {
            mDoAud_seStartMenu(s_soundQueue[i].sfx);
        }
        if (s_soundQueue[i].haptic > haptic) {
            haptic = s_soundQueue[i].haptic;
        }
    }
    s_soundQueueCount = 0;

    // At most one pulse per frame, the heaviest cue winning: PlayHapticRumble
    // restarts the effect on every call, so firing several back to back would
    // leave only the last one felt regardless.
    if (haptic == HAPTIC_NONE || !getSettings().game.dualScreenHaptics.getValue()) {
        return;
    }
    switch (haptic) {
    case HAPTIC_LIGHT:
        aurora::device::rumble(0x2000, 0x3800, 12);
        break;
    case HAPTIC_PRESS:
        // Strong but short: reads as a physical click under the finger,
        // clearly firmer than the LIGHT tick without CONFIRM's length.
        aurora::device::rumble(0x7800, 0x9800, 18);
        break;
    case HAPTIC_CONFIRM:
        aurora::device::rumble(0x5800, 0x8000, 28);
        break;
    case HAPTIC_DENY:
        // Heavier and longer, low-frequency biased — reads as a thud rather
        // than a tick, so a rejection is distinguishable without looking.
        aurora::device::rumble(0xB000, 0x5000, 70);
        break;
    default:
        break;
    }
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

// Cinematic: the whole status HUD lives here — hearts strip across the top,
// controller diamond and vessel down the right, content window filling the
// rest.
void drawDashboardCinematic(dMeter2Draw_c* md, f32 w, f32 h) {
    const f32 x1 = w - 6.0f;
    drawTopBar(md, w, x1);

    // Controller cluster, top-right (also publishes the equip drop rects —
    // drawEquipTargets and the touch pass read them later this frame).
    drawItemCluster(x1, HEARTS_H + 42.0f);
    drawSpecialPanel(w, h);

    // Below the controller cluster: tears of light OR dungeon icons — they
    // never appear at the same time.
    drawDungeonIcons(x1 - 44.0f, h - TABS_H - 124.0f);
    drawVesselOfLight(md, x1, h);

    const f32 cy0 = HEARTS_H + 8.0f;
    const f32 cy1 = h - TABS_H - 6.0f;
    drawContentWindow(12.0f, w - 158.0f, cy0, cy1);

    // Oxygen (drowning) bar, overlaid along the window's top edge while
    // underwater. The main-screen draw is suppressed under dual-screen, so
    // without this Cinematic had no visible oxygen meter on either screen —
    // only the warning SFX. Oxygen only: the top bar already carries the
    // compact oil gauge in this layout.
    drawMeterBar(16.0f, w - 158.0f - 16.0f, cy0 + 11.0f, 14.0f, false);

    // Tab bar below the content window.
    constexpr GXColor COL_SCRIM = {11, 10, 8, 170};
    fillRect(12.0f, h - TABS_H, w - 158.0f, h, COL_SCRIM);
    drawTabs(12.0f, w - 158.0f, h);

    // Equip drop targets on top of everything in the cluster margin.
    drawEquipTargets();

    // Right band, stacked bottom-up: status row, d-pad, transform button.
    // Each reports the height it used, so anything hidden (FPS off, no
    // battery, transform still locked) collapses and the rest settles
    // downward instead of leaving a hole. With everything present the
    // positions match the previous fixed layout.
    constexpr f32 BAND_GAP = 8.0f;
    // Wider gap above the status row: it reads as part of the tab strip.
    constexpr f32 BAND_GAP_STATUS = 23.0f;
    s_transformBtnRect[0] = 0.0f;
    s_transformBtnRect[2] = 0.0f;
    f32 bandY = h - 17.0f;
    const f32 statusH = drawStatusCorner(x1, bandY);
    if (statusH > 0.0f) {
        bandY -= statusH + BAND_GAP_STATUS;
    }
    const f32 dpadH = drawDpadGlyph(md, x1, bandY);
    if (dpadH > 0.0f) {
        bandY -= dpadH + BAND_GAP;
    }
    drawTransformButton(x1, bandY);
}

// Functional: the gameplay HUD is on the main screen, so this becomes a
// control surface — slim status strip, utility column left, item buttons
// right, content window centred with its tabs beneath.
void drawDashboardFunctional(dMeter2Draw_c* md, f32 w, f32 h) {
    drawFunctionalTopBar(w);

    const f32 cy0 = FN_TOPBAR_H + 4.0f;
    const f32 cy1 = h - TABS_H - 4.0f;
    // Columns are exactly as wide as the corner boxes, so the content window
    // takes everything between them and nothing can overhang.
    const f32 leftX = 0.0f;
    const f32 rightX = w - FN_COL_W;

    // Equal padding everywhere: the corner boxes sit FN_GAP from the screen
    // edge, and the content window sits FN_GAP from the boxes. The stagger
    // is capped so Y never reaches past the boxes, which keeps both sides
    // the same width and the window genuinely centred.
    const f32 sideInset = FN_CORNER_MARGIN + FN_CORNER + FN_GAP;
    const f32 wx0 = sideInset;
    const f32 wx1 = w - sideInset;
    // Content window FIRST: the buttons and their labels sit slightly over
    // its edges, so drawing it afterwards would paint over them.
    drawContentWindow(wx0, wx1, cy0, cy1);

    drawFunctionalCorners(md, w, h);
    // Left column strictly between the two corner boxes (their exact top/
    // bottom edges, inset by the gap) so it can't overlap either corner.
    const f32 topCornerBottom = FN_TOPBAR_H + FN_CORNER_MARGIN + FN_CORNER;
    const f32 botCornerTop = h - FN_CORNER_MARGIN - FN_CORNER;
    drawFunctionalLeftColumn(md, leftX, topCornerBottom + FN_LEFT_GAP,
        botCornerTop - FN_LEFT_GAP);
    // X/Y take the I and II box CENTRES, so all four slots share one rhythm.
    // Must match drawFunctionalCorners' own geometry exactly.
    const f32 iCenterY = FN_TOPBAR_H + FN_CORNER_MARGIN + FN_CORNER * 0.5f;
    const f32 iiCenterY = h - FN_CORNER_MARGIN - FN_CORNER * 0.5f;
    drawFunctionalItemButtons(md, rightX, iCenterY, iiCenterY);

    // No scrim here: drawTabs lays its own bed for this mode, matching the
    // content window's fill and frame.
    drawTabs(wx0, wx1, h);
    // No drawEquipTargets() here: the round X/Y buttons are the drop targets
    // themselves and ring in blue/green, so the Cinematic rectangles would
    // just be clutter over the layout.

    // Floor-select overlay LAST so it sits above the content window. Anchored
    // to the right edge of the context tab (published as s_ctxTabRect).
    if (s_page.load() == PAGE_MAP && s_dmapAvailable && s_dmapFloorPickOpen) {
        // Inset from the window's own left edge, not from the tab: the tab
        // ends just short of the window, so anchoring to it left the plates
        // straddling the frame.
        drawFloorOverlay(wx0 + 6.0f, (s_ctxTabRect[1] + s_ctxTabRect[3]) * 0.5f, true,
            cy0 + 4.0f, cy1 - 4.0f);
    } else {
        s_dmapFloorPickOpen = false;
        s_dmapFloorRectCount = 0;
    }
}

void drawDashboard(float w, float h) {
    // Published for touchEvent, which arrives with normalized coordinates and
    // needs the canvas size to test them against the logical-unit rects.
    s_canvasW = w;
    s_canvasH = h;
    // Undo any idle fade the game applied this frame for whatever this screen
    // still owns. In Functional most of the HUD is back on the main screen and
    // must keep its own fades, so the pin set narrows there.
    if (dMeter2Draw_c* mdAlpha = meterDraw()) {
        mdAlpha->forceCompanionAlpha();
    }
    loadCollectIcons();
    // The equip feedback line counts down here (once per frame) so a page
    // switch can't freeze a stale warning for a later replay.
    if (s_equipMsgFrames > 0) {
        s_equipMsgFrames--;
    }
    // The visible tab set is layout-dependent: entering Functional while
    // sitting on QUEST would otherwise leave a page showing with no tab and
    // no way back to it.
    int pages[TAB_RECT_MAX];
    const int pageCount = visiblePages(pages);
    const int page = s_page.load();
    bool pageVisible = false;
    for (int i = 0; i < pageCount; i++) {
        pageVisible = pageVisible || pages[i] == page;
    }
    if (!pageVisible) {
        s_page.store(PAGE_MAP);
    }

    dMeter2Draw_c* md = meterDraw();

    s_dropRectValid = false;
    // All four entries: the layouts publish different subsets (Cinematic
    // never publishes the I/II targets), and a stale rect from the other
    // layout must not stay hit-testable.
    for (int i = 0; i < DROP_TARGET_COUNT; i++) {
        s_dropRect[i][0] = 0.0f;
        s_dropRect[i][2] = 0.0f;
    }
    // Functional-only buttons: cleared here rather than in their draw so a
    // switch to Cinematic can't leave a live rect nothing draws any more.
    s_ctxTabRect[0] = 0.0f;
    s_ctxTabRect[2] = 0.0f;
    s_zBtnRect[0] = 0.0f;
    s_zBtnRect[2] = 0.0f;
    for (int i = 0; i < 2; i++) {
        s_slotBtnRect[i][0] = 0.0f;
        s_slotBtnRect[i][2] = 0.0f;
        s_fnXYRect[i][0] = 0.0f;
        s_fnXYRect[i][2] = 0.0f;
    }

    drawBackdrop(w, h);
    if (dualscreen::mainHudRestored()) {
        drawDashboardFunctional(md, w, h);
    } else {
        drawDashboardCinematic(md, w, h);
    }

    // (The touch-button hold/ghost lifecycle ticks in
    // beginFrameCompanionInput — the GAME frame loop — not here: the
    // dashboard renders at the display rate, which is not the pad's rate.)

    // Combo-or-replace chooser floats above either layout.
    s_comboChoiceRects[0][0] = 0.0f;
    s_comboChoiceRects[0][2] = 0.0f;
    s_comboChoiceRects[1][0] = 0.0f;
    s_comboChoiceRects[1][2] = 0.0f;
    drawComboChoice();

    // Cutscene dim: near-black over everything while an event runs (ramp
    // ticked in beginFrameCompanionInput). The dashboard has nothing
    // actionable during events, and a lit second screen next to a cutscene
    // pulls the eye for no reason.
    {
        const f32 dim = s_dimHold > s_eventDim ? s_dimHold : s_eventDim;
        if (dim > 0.01f) {
            fillRect(0.0f, 0.0f, w, h, {0, 0, 0, (u8)(216.0f * dim)});
        }
    }

    // Touch runs LAST so it hit-tests against the geometry this frame's
    // draw just published.
    handleTouch(w, h);
    processDragTouch(w, h);
    drawDragGhost();

    // Cutscene: an in-flight drag dies with the lights — its release is
    // ignored (processDragTouch) and the ghost must not float above the
    // dim overlay, which paints before the drag pass. Live event state,
    // matching the touch gates.
    if (dComIfGp_event_runCheck()) {
        s_dragging = false;
        s_dragSlot = -1;
    }
    // Geometry statics go stale when the page changes; an in-flight item
    // drag must not survive onto another page either.
    if (s_page.load() != PAGE_INVENTORY) {
        s_invGeomValid = false;
        s_selSlot = -1;
        s_dragging = false;
        s_dragSlot = -1;
    }
    // Reset-view glide: ease the view home instead of snapping. Pan homes
    // in both modes; zoom and renderer steering only in field mode — the
    // dungeon's zoom/center ride its own room-follow glide. Render-rate
    // ease (visual only); a settle tick marks the landing.
    if (s_mapResetGlide && s_page.load() == PAGE_MAP) {
        s_mapPanX *= 0.72f;
        s_mapPanY *= 0.72f;
        if (!s_dmapAvailable) {
            s_mapZoom += (1.0f - s_mapZoom) * 0.28f;
            s_mapViewOffX *= 0.72f;
            s_mapViewOffZ *= 0.72f;
        }
        const bool panHome = s_mapPanX > -0.5f && s_mapPanX < 0.5f && s_mapPanY > -0.5f &&
            s_mapPanY < 0.5f;
        const bool fieldHome = s_dmapAvailable ||
            (s_mapZoom > 0.99f && s_mapZoom < 1.01f && s_mapViewOffX > -0.5f &&
                s_mapViewOffX < 0.5f && s_mapViewOffZ > -0.5f && s_mapViewOffZ < 0.5f);
        if (panHome && fieldHome) {
            s_mapPanX = 0.0f;
            s_mapPanY = 0.0f;
            if (!s_dmapAvailable) {
                s_mapZoom = 1.0f;
                s_mapViewOffX = 0.0f;
                s_mapViewOffZ = 0.0f;
            }
            s_mapResetGlide = false;
            queueHaptic(HAPTIC_LIGHT);
        }
    }
    // Pinches only mean something on the MAP page — drop them elsewhere so
    // they don't burst into the map view later.
    if (s_page.load() != PAGE_MAP) {
        s_mapPinchDeltaMilli.exchange(0);
        // The floor picker is MAP furniture. A tabless page change (F9 /
        // nextPage) skips the tap path that would close it, and Cinematic
        // then stops drawing (and refreshing the rects of) the overlay —
        // stale rects would swallow taps on the new page.
        s_dmapFloorPickOpen = false;
        s_dmapFloorRectCount = 0;
    }

}

}  // namespace dusk::companion
