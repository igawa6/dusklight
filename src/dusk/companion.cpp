#include "dusk/action_bindings.h"
#include "dusk/companion.h"
#include "dusk/companion_internal.h"
#include "dusk/companion_strings.h"
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
// ===== second-screen dim =====
//
// Two independent things want to darken the companion, and they must not
// fight: a SCENE CHANGE (load) and an EVENT (cutscene). Everything here was
// derived from frame traces of real stage changes; the numbers are the
// evidence, so they are kept in the comments.
//
// Phases. "Held" in the old notes is simply DOWN saturated at 1.0 — it needs
// no state of its own, since nothing behaves differently once the ramp tops
// out.
//   IDLE  - no load: mirror the main screen exactly. This is the path menus
//           and cutscenes take, and it is the one that always looked right.
//   DOWN  - a load owns the panel: rate-limited mirror, ratcheted.
//   WAKE  - the load is retired: ride the game's arrival fade back up.
enum DimPhase {
    DIM_IDLE,
    DIM_DOWN,
    DIM_WAKE,
};

struct DimState {
    DimPhase phase;
    // Transition dim, 0..1.
    f32 level;
    // Cutscene dim, 0..1. Ramps 4x faster than `level`, so it is FROZEN for
    // the duration of a transition rather than allowed to race it.
    f32 eventLevel;
    // level/cover ratio latched when a load retires; lets the wake trace the
    // game's own fade curve, scaled, with no jump at the handover.
    f32 wakeScale;
    // Consecutive frames with no transition signal (release debounce).
    int clearFrames;
    // Ring for the de-spike minimum filter over `cover`.
    f32 coverHist[3];
    int coverIdx;
};

DimState s_dim = {DIM_IDLE, 0.0f, 0.0f, 0.0f, 0, {0.0f, 0.0f, 0.0f}, 0};

// True for the WHOLE span of a scene change — armed on any transition
// signal, released only once the world is genuinely live again. Gates the
// I/II/X/Y equip pop as well as the dim.
bool transitionOwnsPanel() {
    return s_dim.phase != DIM_IDLE;
}
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
// Floor-picker open/close progress: 0 = collapsed onto the tab, 1 = full
// list. Separate from the open flag so the CLOSE can play out instead of the
// list vanishing the frame it is dismissed.
f32 s_dmapFloorPickT = 0.0f;
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

// Equip mode: the drop targets are rendered later by drawEquipTargets as
// two stacked rectangles at the right edge (no overlap) — publish the rect
// anchored at (and centered on) the diamond button. The X/Y separation
// keeps the lanes disjoint.
// The 56px left lane exists for Cinematic's equipped-item rectangle only.
// Functional's round buttons ARE the drop targets (drawEquipTargets is
// skipped), so there the accept zone matches the circle — an invisible lane
// reaching into the content window equipped items with no ring feedback.
void publishEquipDropRect(int dropIdx, f32 x, f32 y, f32 btn) {
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

// Shared by both HUD layouts (companion_functional.cpp draws them in the
// 3DS top bar, the Cinematic status corner below). At namespace scope so
// the other translation unit can reach them.
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
// Zoom progress for the callers that draw an underlay: 1.0 = settled.
f32 readerZoomProgress() {
    return s_readerZoomT;
}

bool readerZoomActive() {
    return s_readerZoomT < 1.0f && s_readerZoomFrom[2] > s_readerZoomFrom[0];
}

bool readerZoomStep(f32* io_x0, f32* io_y0, f32* io_x1, f32* io_y1) {
    if (s_readerZoomClosing) {
        s_readerZoomT *= ANIM_DECAY_FAST;
        if (s_readerZoomT < ANIM_ZERO) {
            s_readerZoomClosing = false;
            s_readerZoomT = 1.0f;
            return false;
        }
    } else if (s_readerZoomT < 1.0f) {
        s_readerZoomT += (1.0f - s_readerZoomT) * ANIM_RATE_FAST;
        if (s_readerZoomT > ANIM_DONE) {
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
    // No panel fill while it travels: a box growing across the window read as
    // an extra background sliding around. The detail FADES in over the list
    // instead (its own backdrop is drawn once it lands).
    //
    // SIDE EFFECT: this lowers s_drawAlpha for the caller's subsequent draws.
    // It multiplies rather than assigns, so a page transition already fading
    // this page keeps its fade. The caller's choke point restores it.
    s_drawAlpha *= t;
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
    case CTX_INFO: {
        drawTextFittedCentered((x0 + x1) * 0.5f, (y0 + y1) * 0.5f + 5.0f, 13.0f, 8.0f,
            (x1 - x0) - 10.0f, clickable ? TEXT_TAB_ACTIVE : TEXT_DIM,
            localizedWord(0x005D, "Info"));
        break;
    }
    case CTX_HOME: {
        // Same word as the COLLECTION tab, so the two never drift apart.
        char home[48];
        snprintf(home, sizeof(home), "< %s", tabName(PAGE_COLLECTION));
        drawTextFittedCentered((x0 + x1) * 0.5f, (y0 + y1) * 0.5f + 5.0f, 13.0f, 7.5f,
            (x1 - x0) - 10.0f, clickable ? TEXT_TAB_ACTIVE : TEXT_DIM, home);
        break;
    }
    case CTX_BACK: {
        drawTextFittedCentered((x0 + x1) * 0.5f, (y0 + y1) * 0.5f + 5.0f, 13.0f, 8.0f,
            (x1 - x0) - 10.0f, TEXT_TAB_ACTIVE,
            localizedWord(0x0054, "Back"));
        break;
    }
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

float currentDim() {
    // The ONE place the two dims are combined. A transition owns the panel
    // while it runs, but never brightens it: the event ramp is frozen at
    // whatever it had reached, so a cutscene interrupted by a load stays as
    // dark as it was instead of flashing.
    const f32 t = s_dim.level > s_dim.eventLevel ? s_dim.level : s_dim.eventLevel;
    if (t <= 0.01f) {
        return 0.0f;
    }
    // 216/255 was this overlay's alpha when it was painted into the picture.
    // Keep it: full dim is deep charcoal, not pure black, so the panel reads
    // as "asleep" rather than "switched off".
    return t * (216.0f / 255.0f);
}

// Drop the dim to lit with no ramp. The boot/quit-to-title splash owns the
// panel outright and has no fade of its own; without this the dashboard's
// ratcheted level was still sitting at full when the splash handed back,
// snapping a lit logo to black in one frame.
void resetDim() {
    s_dim.phase = DIM_IDLE;
    s_dim.level = 0.0f;
    s_dim.eventLevel = 0.0f;
    s_dim.wakeScale = 0.0f;
    s_dim.clearFrames = 0;
}


void update() {
    if (ImGui::IsKeyPressed(ImGuiKey_F9, false)) {
        nextPage();
    }
#if DUSK_COMPANION_CAPTURE
    // Desktop counterpart of the Android DUMP broadcast: write what the
    // second screen is presenting to companion-screenshot.png in the data
    // folder. (F10/F11 belong to the ImGui debug views.)
    if (ImGui::IsKeyPressed(ImGuiKey_F12, false)) {
        dualscreen::requestScreenshot(NULL);
    }
#endif
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

// The main screen's live coverage this frame: JUTFader wipe alpha, the
// mDoGph event-fade alpha (gated on isFade), and the iris-wipe rate, whichever
// is deepest.
//
// DE-SPIKED. Measured across a real stage change, the raw signal alternates
// between 1.0 and the true ramp on consecutive frames of a fade-OUT:
//
//   f=601 1.000 | f=602 0.039 | f=603 1.000 | f=604 0.078 | f=605 1.000
//
// The three sources disagree for one frame of each pair (the wipe reports a
// full cover while the fader is still ramping). Used raw it wrecks both
// directions: the ratchet slams to black in ONE frame, and the wake flickers
// bright on every low frame. Taking the minimum over a short window fixes it —
// a genuine full-black hold reads 1.0 on EVERY frame so it survives untouched,
// while an alternating spike never does. Fade-INs are already a clean 30Hz
// staircase and pass through unchanged, which is why menus always looked right.
f32 computeCover() {
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

    s_dim.coverHist[s_dim.coverIdx] = cover;
    s_dim.coverIdx = (s_dim.coverIdx + 1) % (int)ARRAY_SIZE(s_dim.coverHist);
    f32 lo = s_dim.coverHist[0];
    for (int i = 1; i < (int)ARRAY_SIZE(s_dim.coverHist); i++) {
        if (s_dim.coverHist[i] < lo) {
            lo = s_dim.coverHist[i];
        }
    }
    return lo;
}

// Advance the dim one game frame. `armed` is true while anything says a scene
// change is in flight (see its computation in beginFrameCompanionInput).
void tickDim(bool armed) {
    // Dim-down rate: ~1.3s end to end, roughly half the game's own fade, so
    // the panel settles rather than snaps.
    constexpr f32 RISE_STEP = 0.0125f;
    // Wake floor. Applied BEFORE the smoothing below, so the true worst-case
    // rate is WAKE_STEP * WAKE_SMOOTH = 0.022/frame, i.e. ~45 frames from full.
    // It only governs when the arrival fade is already spent; while a fade is
    // live the ratio follow is slower and wins.
    constexpr f32 WAKE_STEP = 0.10f;
    // Per-frame approach toward the wake target. Deliberately SLOWER than the
    // game's 30Hz fade cadence: a filter as fast as the staircase it smooths
    // just reproduces it as a 2-frame ripple in the rate.
    constexpr f32 WAKE_SMOOTH = 0.22f;
    // Consecutive clear frames before a load is considered retired. A
    // multi-phase change lets `armed` blink false between phases; releasing on
    // the first clear frame started the wake, and the next phase re-armed and
    // ratcheted straight back up (flicker-bright / fast-dim at area edges).
    constexpr int RELEASE_DEBOUNCE = 6;

    const f32 cover = computeCover();

    s_dim.clearFrames = armed ? 0 : s_dim.clearFrames + 1;

    // ---- phase transitions ----
    if (armed) {
        s_dim.phase = DIM_DOWN;
    } else if (s_dim.phase == DIM_DOWN && s_dim.clearFrames >= RELEASE_DEBOUNCE &&
               dComIfGp_getLinkPlayer() != NULL)
    {
        if (s_dim.level > 0.0f) {
            // Ride the main screen's own fade-in. The load retires partway
            // through it, so we sit at `level` while the game is at `cover`.
            // Latch the ratio: the dim then traces the game's exact curve,
            // scaled, and lands on the same frame. ratio * cover == level at
            // this instant, so the handover has no step.
            s_dim.wakeScale = cover > 0.01f ? s_dim.level / cover : 0.0f;
            s_dim.phase = DIM_WAKE;
        } else {
            s_dim.phase = DIM_IDLE;
        }
    } else if (s_dim.phase == DIM_WAKE && cover > s_dim.level) {
        // A NEW fade started while we were waking (e.g. the arrival runs
        // straight into a cutscene). The ratio follow is monotonic and clamped
        // to `level`, so it would sit pinned and never reach the exit — the
        // wake would hang at a weak dim for the whole cutscene. The main screen
        // is darker than us now, so hand back to the plain mirror.
        s_dim.phase = DIM_IDLE;
        s_dim.wakeScale = 0.0f;
    }

    // ---- level ----
    switch (s_dim.phase) {
    case DIM_DOWN: {
        // MIRROR the main screen, RATE LIMITED.
        //
        // Measured across five wipe types: `cover` during a stage change is
        // not a fade curve, it is whatever wipe the game picked. Wipes 5/13/21
        // ramp; wipes 0/8 are HARD CUTS — 0.000 to 1.000 in a single frame.
        // A plain mirror would slam the panel black on those. So follow
        // `cover` while it moves slower than RISE_STEP (a real fade -> we track
        // the main screen and are never darker than it) and cap the rate
        // otherwise. The cap also makes this immune to a spiky signal.
        // Ratcheted: never brightens while a load owns the panel.
        f32 step = s_dim.level + RISE_STEP;
        if (step > 1.0f) {
            step = 1.0f;
        }
        const f32 target = cover < step ? cover : step;
        if (target > s_dim.level) {
            s_dim.level = target;
        }
        break;
    }
    case DIM_WAKE: {
        f32 wake = s_dim.wakeScale > 0.0f ? cover * s_dim.wakeScale : 0.0f;
        // The floor applies ONLY once the fade is spent. `cover` is a 30Hz
        // staircase, so an always-on floor would drag the level down between
        // steps and turn the descent into a sawtooth.
        if (cover <= 0.001f) {
            const f32 floorV = s_dim.level - WAKE_STEP;
            if (floorV > wake) {
                wake = floorV;
            }
        }
        if (wake > s_dim.level) {
            wake = s_dim.level;  // monotonic: a wipe blip cannot re-dim mid-wake
        }
        // Ease toward the target so the game's 30Hz steps do not read as
        // shuttering on what is, here, a flat overlay.
        s_dim.level += (wake - s_dim.level) * WAKE_SMOOTH;
        if (s_dim.level <= 0.004f) {
            s_dim.level = 0.0f;
            s_dim.wakeScale = 0.0f;
            s_dim.phase = DIM_IDLE;
        }
        break;
    }
    case DIM_IDLE:
    default:
        // Mirror cutscene/event fades exactly. Nothing is frozen and there is
        // only ever one fade on screen, so the plain mirror is exact.
        s_dim.level = cover;
        break;
    }

    // ---- event (cutscene) dim ----
    // FROZEN while a transition owns the panel. It ramps at 0.05/frame — four
    // times the transition rate — and stage changes run a brief event of their
    // own, so letting it run during a load produced a fast-dim/flicker-bright
    // pair either side of the correct curve. Freezing (rather than clamping it
    // down, which destroyed the ramp and flashed the panel bright at the START
    // of a transition that interrupted a cutscene) keeps a cutscene already in
    // progress exactly as dark as it was.
    if (!transitionOwnsPanel()) {
        if (dComIfGp_event_runCheck()) {
            s_dim.eventLevel = s_dim.eventLevel > 0.95f ? 1.0f : s_dim.eventLevel + 0.05f;
        } else {
            s_dim.eventLevel = s_dim.eventLevel < 0.05f ? 0.0f : s_dim.eventLevel - 0.05f;
        }
    }
}

// Per-frame ramps that ride the GAME clock (the pad's rate) rather than the
// render clock: rejected-tap flashes, the button press-depress, the equip
// landing pop and the wolf morph crossfade. None of them read the dim, so
// they run before it — split out of beginFrameCompanionInput, which is an
// input latch and should read as one.
void tickAnimations() {
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
            s_pressAnim[i] += (1.0f - s_pressAnim[i]) * ANIM_RATE_SNAP;
            if (s_pressAnim[i] > ANIM_DONE) {
                s_pressAnim[i] = 1.0f;
            }
        } else {
            s_pressAnim[i] *= ANIM_DECAY_FAST;
            if (s_pressAnim[i] < ANIM_ZERO) {
                s_pressAnim[i] = 0.0f;
            }
        }
    }
    for (int i = 4; i < 6; i++) {
        s_pressAnim[i] *= ANIM_DECAY_SOFT;
        if (s_pressAnim[i] < ANIM_ZERO) {
            s_pressAnim[i] = 0.0f;
        }
    }

    // Equip landing pop: a button whose SELECT changed to a real item
    // (wheel or companion equip) pops briefly so the eye finds where the
    // item went. Only during live gameplay — save/stage loads rewrite the
    // indices wholesale and must not fire it, and a map change must leave
    // the I/II/X/Y buttons perfectly still.
    {
        static u8 sPrevSel[4] = {0xFF, 0xFF, 0xFF, 0xFF};
        const bool live = meterDraw() != NULL && !transitionOwnsPanel();
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
            s_popAnim[b] *= ANIM_DECAY_SOFT;
            if (s_popAnim[b] < ANIM_ZERO) {
                s_popAnim[b] = 0.0f;
            }
        }
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
}

// Haptic-only feedback on the lantern/oxygen gauges. No visual state.
void tickGaugeWarning() {
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
}

// Damage cancels any open reader/detail view.
void cancelReadersOnDamage() {
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
}

// Reconcile the slot bindings with the save and the item wheel.
void sanitizeSlotBindings() {
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

    tickAnimations();

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

    tickDim(armedNow);

    tickGaugeWarning();
    cancelReadersOnDamage();
    sanitizeSlotBindings();
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

bool midnaAvailable() {
    // Mirrors the game's own gate on the HUD Z button (d_meter2_draw.cpp):
    //   M_067  - Midna riding / not riding; OFF for the whole Ordon prologue,
    //            and the same bit daPy_py_c::checkFirstMidnaDemo() tests.
    //   F_0800 - after returning to Ordon Woods, until she comes out of the
    //            shadows; while it is ON she cannot be called at all.
    return daPy_py_c::checkFirstMidnaDemo() &&
           dComIfGs_isEventBit(dSv_event_flag_c::F_0800) == 0;
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
            // Overworld/town: warp. Before Midna grants it the plate is drawn
            // EMPTY (drawWarpTab bails on !warpUnlocked), so it must be inert
            // too — isFieldMapScreen alone used to make a blank plate render in
            // SELECTED style and still fire requestWarpToggle on tap.
            // The field-map Z toggle (isFieldMapScreen) counts as available.
            // Any OTHER menu (start screen, item wheel, submenus) makes it
            // inert: posting setMapStatus(3) under an open menu is a state
            // the game never reaches on its own — the retired in-map button
            // hid itself for the same reason.
            action = CTX_WARP;
            clickable = warpUnlocked() &&
                (isFieldMapScreen() || (warpAllowed() && !anyMenuOpen()));
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

    // Floor picker LAST so it sits above everything. It expands the context
    // tab IN PLACE, up and down in the left column, rather than sliding a
    // panel out over the map the player is reading. Clamped to the screen
    // rather than the content window — the column is outside it.
    if (s_page.load() == PAGE_MAP && s_dmapAvailable) {
        // Top capped to the content window's top border, so a tall dungeon's
        // list lines up with the window rather than running to the screen
        // edge. drawFloorColumn also drives the open/close animation, so it is
        // called while closing too — not only while open.
        drawFloorColumn(s_ctxTabRect[0], s_ctxTabRect[1], s_ctxTabRect[2], s_ctxTabRect[3],
            cy0, h - 6.0f, h);
    } else {
        s_dmapFloorPickOpen = false;
        s_dmapFloorPickT = 0.0f;
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
        // s_dmapFloorPickT belongs to the Functional in-place floor column,
        // and drawFloorColumn is the ONLY code that decays it — Cinematic
        // draws the pop-up list instead (drawFloorOverlay), which keys on
        // s_dmapFloorPickOpen alone and never touches the ramp.
        //
        // So a live HUD-mode switch taken with the picker open (or still
        // closing) strands the ramp above zero with nothing left running to
        // bring it down. That matters because handleTouch's picker gate is
        // `open || T > 0`, and it sits ahead of the corner buttons, the
        // tabs and every page: a stranded ramp swallows EVERY companion tap
        // with a cancel beep, for the rest of the session, recoverable only
        // by switching back to Functional on a multi-floor dungeon map.
        s_dmapFloorPickT = 0.0f;
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

    // (The transition/load hold and the cutscene dim are NOT painted here.
    // They are published via currentDim() and applied by the aux window to
    // the present-time blit — see dualscreen::endHudCapture. Baking them into
    // the picture froze the fade whenever the HUD tore down and the capture
    // stopped refreshing.)

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
        s_mapPanX *= ANIM_DECAY_FAST;
        s_mapPanY *= ANIM_DECAY_FAST;
        if (!s_dmapAvailable) {
            s_mapZoom += (1.0f - s_mapZoom) * ANIM_RATE_FAST;
            s_mapViewOffX *= ANIM_DECAY_FAST;
            s_mapViewOffZ *= ANIM_DECAY_FAST;
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
