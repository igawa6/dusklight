#pragma once

// Internal shared surface of the companion dashboard modules. Everything
// here is private to src/dusk/companion*.cpp — the public API lives in
// include/dusk/companion.h.
//
// Layout of the modules:
//   companion_gfx.cpp     low-level drawing primitives + pane compositing
//   companion_icons.cpp   item/collect icon caches (all static buffers)
//   companion_touch.cpp   touch input, drag & drop, equip actions
//   companion_pages.cpp   MAP / ITEMS page content
//   companion_collect.cpp COLLECT page content
//   companion_dmap.cpp    live dungeon-map renderer (menu-map reuse)
//   companion.cpp         dashboard composition, public API, and the
//                         definitions of all shared state below

#include <atomic>
#include <cstdint>

#include "dolphin/types.h"
#include "dolphin/gx/GXStruct.h"
#include "d/actor/d_a_player.h"
#include "d/d_com_inf_game.h"
#include "d/d_meter2_info.h"
#include "d/d_save.h"
#include "dusk/dualscreen.h"

class J2DPicture;
class J2DPane;
struct ResTIMG;
// Defined in JUTFont.h (PC builds only); forward-declared here so the header
// does not have to pull the whole font stack in.
struct FontDrawContext;
class dMeter2Draw_c;

namespace dusk::companion {

// ---------------------------------------------------------------------------
// Types

enum Page {
    PAGE_MAP,
    PAGE_INVENTORY,
    PAGE_COLLECTION,
    PAGE_GUIDE,
    PAGE_COUNT,
};

// Per-cell ITEMS grid layout: slot + top-left position, built each frame the
// page draws and consumed by the touch module for hit tests.
struct InvCell {
    int slot;
    f32 x, y;
};

// ---------------------------------------------------------------------------
// Icon cache slots (companion_icons.cpp buffers are indexed by these).

constexpr u32 ICON_BUF_SIZE = 0x2800;  // fits 64x64 RGB5A3 + header (bottle etc.)
// Compact numbering — every slot is referenced by name only, so retired
// slots are removed outright instead of leaving gaps.
constexpr int ICON_SLOT_BITEM = MAX_ITEM_SLOTS;
constexpr int ICON_SLOT_XITEM = MAX_ITEM_SLOTS + 1;
constexpr int ICON_SLOT_YITEM = MAX_ITEM_SLOTS + 2;
constexpr int ICON_SLOT_KEY = MAX_ITEM_SLOTS + 3;  // top-bar small-key counter
constexpr int ICON_SLOT_DMAP = MAX_ITEM_SLOTS + 4;
constexpr int ICON_SLOT_COMPASS = MAX_ITEM_SLOTS + 5;
constexpr int ICON_SLOT_BOSSKEY = MAX_ITEM_SLOTS + 6;
constexpr int ICON_SLOT_LANTERN = MAX_ITEM_SLOTS + 7;
// Collection page slots (poe, rod, 24 golden bugs, gear, letter).
constexpr int ICON_SLOT_POE = MAX_ITEM_SLOTS + 8;
constexpr int ICON_SLOT_ROD = MAX_ITEM_SLOTS + 9;
constexpr int ICON_SLOT_BUG0 = MAX_ITEM_SLOTS + 10;
constexpr int ICON_SLOT_EQUIP0 = ICON_SLOT_BUG0 + 24;  // 9 equipment slots
constexpr int ICON_SLOT_LETTER = ICON_SLOT_EQUIP0 + 9;
constexpr int ICON_SLOT_RUPEE = ICON_SLOT_LETTER + 1;  // Functional left column
constexpr int ICON_SLOT_BUGPROG = ICON_SLOT_RUPEE + 1;  // left-column progress page
constexpr int ICON_SLOT_FN1 = ICON_SLOT_BUGPROG + 1;  // I / II corner slot items
constexpr int ICON_SLOT_FN2 = ICON_SLOT_FN1 + 1;
constexpr int ICON_SLOT_COUNT = ICON_SLOT_FN2 + 1;

// Collect-archive icon slots (clctres BTIs): 0 fish journal, 1 skills
// scroll, 2 letter, 3-7 scents (medicine/children/fish/iria/poe), 8
// heart-piece base, 9-12 the four cumulative wedge overlays.
constexpr int CLCT_ICON_COUNT = 13;
constexpr int CLCT_SLOT_HEART_BASE = 8;
constexpr int CLCT_SLOT_HEART_PARTS1 = 9;

// Raw ItemIcon-archive resource indices (items whose dItem_data texture
// entry is wrong/missing).
constexpr int RAWICON_YADUTU1 = 0x4A;  // quiver small/big/giant: +0/+1/+2

// Pause-menu decoration art (clctres, always mounted): intensity textures
// the game tints via TEV — draw them through drawTimgTinted.
// (identified by matching the live collection screen's panes to archive
// resources — see the pane dump in the session notes)
constexpr int DECO_COUNT = 6;
constexpr int DECO_BLOCKS = 0;     // TT_BLOCK128: stone-block menu backdrop
constexpr int DECO_TAB_PLATE = 1;  // the Save/Options button plate
constexpr int DECO_LINE = 2;       // TT_LINE2: soft separator line
constexpr int DECO_YAKUSHIMA = 3;  // TT_YAKUSHIMA: the Link-box mottled background
// Ornament pieces matched to the game's own collection screen (verified by
// instantiating zelda_collect_soubi_screen.blo and resolving pane textures
// by pointer): the plate every item/gear cell uses, and the small corner
// flourish the window border reproduces.
constexpr int DECO_SLOT_PLATE = 4;  // TT_SPOT_SQUARE3: item/gear cell plate
constexpr int DECO_KAZARI = 5;      // TT_KAZARI_2ND_OKAN_64: corner flourish

// ---------------------------------------------------------------------------
// Palette, matched to the pause menu's smoky stone-and-parchment look:
// warm dark panels, parchment text, gold accents.

constexpr GXColor COL_BG = {17, 17, 16, 255};
constexpr GXColor COL_TAB = {38, 36, 32, 215};
constexpr GXColor COL_TAB_ACTIVE = {214, 206, 186, 255};
// Content-window chrome, shared by every surface that must read as part of
// the window (the window itself, the Functional tab bed and context-tab
// bridge, the floor overlay's rule).
constexpr GXColor COL_WINDOW = {24, 24, 22, 248};
constexpr GXColor COL_FRAME = {108, 102, 90, 255};
// The window's side rule — the tab strip's bed uses the SAME colour and
// insets so the two side borders read as one continuous line.
constexpr GXColor COL_SIDE_RULE = {74, 68, 58, 235};
constexpr u32 TEXT_MAIN = 0xF0E8D0FF;   // parchment
constexpr u32 TEXT_DIM = 0xA69C82FF;
constexpr u32 TEXT_ACCENT = 0xE9CE8EFF;  // gold
constexpr u32 TEXT_TAB_ACTIVE = 0x2E2415FF;

// ---------------------------------------------------------------------------
// Companion-internal API (the cross-module hooks live in dusk/companion.h).

// Haptic weight paired with an interaction sound. Effective on Android only:
// aurora's device rumble is a no-op stub off-device, and the companion has no
// touch input there anyway.
enum Haptic {
    HAPTIC_NONE = 0,
    HAPTIC_LIGHT,    // selection ticks, tab switches
    HAPTIC_PRESS,    // button faces: X/Y, slots, transform, Z — a crisp click
    HAPTIC_CONFIRM,  // equips, combos armed
    HAPTIC_DENY,     // rejected actions
};

void queueSound(unsigned sfxId, int haptic = HAPTIC_NONE);
// Haptic-only cue (no menu SE): button-face presses, where the game's own
// item/action sounds carry the audio side.
void queueHaptic(int haptic);

// Whether Midna's portal warp can be started right now — mirrors the game's
// own dimming of the warp button on its map screen.
bool warpUnlocked();
bool warpAllowed();

// Whether Midna is with Link and can actually be called. Before she joins,
// her button has nothing behind it, so the companion leaves the slot EMPTY
// rather than showing a dead portrait — the same treatment the transform
// button gets before the shadow crystal.
bool midnaAvailable();

// Set by the touch pass; promoted by beginFrameCompanionInput().
void requestWarpToggle();
// True while the field map is already showing its portals.
bool warpPortalsShown();

// A map render texture whose descriptor has been freed or reused reads back
// with garbage dimensions rather than zero, and drawing it makes the GX layer
// size its read from those dimensions — which faulted in the wild asking for
// 1.45 GB. The live map textures are legitimately large (internal-res scale
// times the dual-screen boost), so the bound only has to be tight enough to
// reject nonsense: 8192 is the usual GPU maximum and well above anything the
// map renderer produces.
inline bool isSaneTimg(const ResTIMG* timg) {
    if (timg == NULL) {
        return false;
    }
    const u16 w = (u16)timg->width;
    const u16 h = (u16)timg->height;
    if (w == 0 || h == 0 || w > 8192 || h > 8192) {
        return false;
    }
    // Freed memory can still read back with plausible dimensions, so check the
    // format as well — a stale descriptor showed up as format 255, which the
    // GX layer treats as fatal.
    switch (timg->format) {
    case GX_TF_I4:
    case GX_TF_I8:
    case GX_TF_IA4:
    case GX_TF_IA8:
    case GX_TF_RGB565:
    case GX_TF_RGB5A3:
    case GX_TF_RGBA8:
    case GX_TF_C4:
    case GX_TF_C8:
    case GX_TF_C14X2:
    case GX_TF_CMPR:
        return true;
    // PC-only linear formats. Guide images are decoded from PNG at import and
    // wrapped as GX_TF_RGBA8_PC (guide/image.cpp) because it takes linear data
    // straight to the GPU — the GameCube RGBA8 layout is 4x4-tiled and this
    // tree has no linear->GX tiler. Rejecting them here would make every
    // fabricated texture invisible.
    case GX_TF_R8_PC:
    case GX_TF_RG8_PC:
    case GX_TF_RGBA8_PC:
        return true;
    default:
        return false;
    }
}

// ---------------------------------------------------------------------------
// Shared predicates. The window-status values are the game's own
// dMeter2Info window status (see d_meter2_info.h).

constexpr int WINDOW_STATUS_NORMAL = 0;
constexpr int WINDOW_STATUS_FIELD_MAP = 4;
constexpr int WINDOW_STATUS_DUNGEON_MAP = 5;

// Any game menu owns the screen — pause, item wheel, either map, submenus.
inline bool anyMenuOpen() {
    return dComIfGp_isPauseFlag() || dMeter2Info_getWindowStatus() != WINDOW_STATUS_NORMAL;
}

// Wolf form, null-safe. daPy_py_c::checkNowWolf() dereferences the player
// with no check, and the companion paints during scene teardown where the
// meter can outlive the player by a frame — every companion wolf gate goes
// through here instead.
inline bool companionWolf() {
    daPy_py_c* player = daPy_getPlayerActorClass();
    return player != NULL && player->checkWolf();
}

// The FIELD map specifically. Only it has portals, and only it owns
// dMw_c::mpMenuFmap — reaching for that object under any other status finds a
// pointer belonging to a different screen.
inline bool isFieldMapScreen() {
    return dMeter2Info_getWindowStatus() == WINDOW_STATUS_FIELD_MAP;
}

constexpr f32 HEARTS_H = 26.0f;
constexpr f32 TABS_H = 44.0f;

// --- Functional layout ------------------------------------------------------
// Majora's Mask 3DS-style control surface used when the mode moves the
// gameplay-critical HUD back to the main screen: content window centred,
// item buttons down the right, utility buttons down the left.
// Status strip: FPS left, then the oxygen bar and lantern gauge, battery
// right. Tall enough for the lantern icon, which sets the height.
constexpr f32 FN_TOPBAR_H = 28.0f;
constexpr f32 FN_BTN = 76.0f;       // X / Y circular button diameter
// The four utility boxes pinned to the screen corners (transform, I, Z, II).
// One size for all four, chamfered, hard against the edges.
constexpr f32 FN_CORNER = 84.0f;
// One gap value everywhere: screen edge -> corner box -> content window all
// use it, so the padding reads evenly across the whole surface.
constexpr f32 FN_GAP = 6.0f;
constexpr f32 FN_CORNER_MARGIN = FN_GAP;
constexpr f32 FN_CORNER_CHAMFER = 14.0f;
// Corner-box label: small, inset from the box's top-left corner rather than
// jammed against it.
constexpr f32 FN_CORNER_LABEL = 12.0f;
constexpr f32 FN_LABEL_DX = 8.0f;
constexpr f32 FN_LABEL_DY = 7.0f;
// Side column width — derived from the corner boxes so the counters between
// them can never overhang into the content window.
constexpr f32 FN_COL_W = FN_CORNER + FN_CORNER_MARGIN * 2.0f;
// X sits slightly right of Y, mirroring the controller's diagonal pair.
// Bounded so Y's left edge lands exactly on the corner boxes' left edge:
// any further and the right side would reach past the left, forcing the
// content window off-centre or the gaps unequal.
constexpr f32 FN_XY_STAGGER = FN_CORNER - FN_BTN;
// Left column, three zones between the transform and Z corner boxes:
// top rupee box, middle context tab, bottom dungeon-info box. Fixed heights
// (the tab and rupee box) with the dungeon box taking the remainder, so the
// zones never shift as context changes.
constexpr f32 FN_LEFT_GAP = 8.0f;      // between the three zones
constexpr f32 FN_RUPEE_H = 44.0f;      // rupee row: gem then its counter
constexpr f32 FN_CTXTAB_H = 44.0f;     // middle context-tab height
constexpr f32 FN_DUNGEON_H = 104.0f;   // dungeon 2x2 box, bottom-anchored
// COLLECT page sub-tabs (All/Bugs/Fish/Skills/Mail).
constexpr int COLLECT_TAB_COUNT = 5;
// Gap between the COLLECT sub-tab plates (shared by draw and touch).
constexpr f32 CTAB_GAP = 10.0f;
constexpr f32 CTAB_H = 40.0f;  // sub-tab strip height (plate = CTAB_H - 4)

// ---------------------------------------------------------------------------
// Shared state (defined in companion.cpp — see the "Shared state" block).

extern std::atomic<int> s_page;
extern std::atomic<int> s_collectTab;

// Gear tap-to-equip boxes on the COLLECT overview: 0-1 swords, 2-3 shields,
// 4-6 clothes. Geometry refreshed each frame the overview draws.
extern f32 s_gearBoxX[7];
extern f32 s_gearBoxY[7];
extern f32 s_gearBoxS;

// Touch stream (see companion.h touchAt/touchEvent for the packing).
extern std::atomic<uint32_t> s_pendingTouch;
extern std::atomic<uint32_t> s_touchPos;
extern std::atomic<int> s_touchPhase;

// Drag & drop equip state (game thread only).
extern int s_dragSlot;
extern bool s_dragging;
extern f32 s_dragX, s_dragY;
extern int s_selSlot;

// Equip feedback message ("can't equip now"), frames remaining. Decremented
// by whichever page draws it — pages are mutually exclusive.
extern int s_equipMsgFrames;
extern char s_equipMsg[64];

// The dashboard canvas in logical units, published each frame by
// drawDashboard. Its aspect follows the panel the companion is shown on —
// 8:7 on a bottom screen, the main screen's shape when the screens are
// swapped — so layouts that reflow by width read it from here.
extern f32 s_canvasW;
extern f32 s_canvasH;

// ITEMS page geometry, written by the page draw, read by touch hit tests.
extern bool s_invGeomValid;
extern f32 s_invCell;
extern InvCell s_invCells[24];
extern int s_invCellCount;

// Equip drop rectangles, valid only while an item is selected or dragged on
// the ITEMS page: [0]/[1] the real X/Y buttons, [2]/[3] the Functional I/II
// slot bindings. Entries a layout does not publish keep zero width.
constexpr int DROP_TARGET_COUNT = 4;
constexpr int DROP_TARGET_SLOT1 = 2;
extern bool s_dropRectValid;
extern f32 s_dropRect[DROP_TARGET_COUNT][4];  // x0, y0, x1, y1

// Wolf/human quick-transform button above the d-pad. Rect published by the
// dashboard draw (w == 0 when hidden); a tap sets the request, consumed on
// the game frame loop (f_ap_game) where starting the transform is safe.
extern f32 s_transformBtnRect[4];
extern std::atomic<bool> s_transformReq;

// Functional layout: Z button (bottom of the left column) and the two I/II
// item slots. Rects are published by the draw each frame (x1 == x0 when the
// button is not on screen) and hit-tested by the touch pass.
extern f32 s_zBtnRect[4];
extern std::atomic<bool> s_zPressReq;
extern f32 s_slotBtnRect[2][4];  // [0] = I, [1] = II
// Round X/Y buttons' touch rects (tap-to-use; Functional only).
extern f32 s_fnXYRect[2][4];

// --- Touch item buttons (Functional) ---------------------------------------
// The four item buttons: 0/1 are the real X/Y (held via PAD_BUTTON_X/Y in
// s_padHoldMaskState, consumed by mDoCPd_c::read), 2/3 are the slot buttons
// I/II (held via s_slotHoldMaskState bits 0/1, latched into Link's item
// masks by beginFrameCompanionInput + setStickData). Which logical button
// owns the current hold: -1 none, 0..3. The finger sliding off the button
// does NOT cancel the hold — real-button semantics; release/cancel does.
extern std::atomic<uint32_t> s_padHoldMaskState;
extern std::atomic<uint32_t> s_slotHoldMaskState;
extern int s_holdBtn;
// Minimum-hold plumbing. A tap must land as a DELIBERATE press: several
// items cancel on early release (ball & chain needs the button held through
// its ~19-frame wind-up), and a same-frame tap would otherwise clear the
// mask before the pad ever read it. Releases before the mask has been held
// TAP_HOLD_FRAMES game frames are deferred (s_holdReleaseReq) and applied by
// beginFrameCompanionInput, which ticks s_holdFrames once per GAME frame —
// the render rate the dashboard runs at is not the pad's rate.
constexpr int TAP_HOLD_FRAMES = 24;
extern int s_holdFrames;
extern bool s_holdReleaseReq;
// Rejected-tap flash, per item button (0/1 = X/Y, 2/3 = slots I/II). A tap
// the handlers refuse (menu open, item unusable, empty button) sets the
// counter; the button draws a fading red ring/frame while it runs down, so
// the screen the finger is ON acknowledges the refusal — the deny sound
// alone is easy to miss in gameplay noise. Ticked with the hold plumbing in
// beginFrameCompanionInput (game frames).
constexpr int DENY_FLASH_FRAMES = 14;
extern u8 s_denyFlash[4];
// Press-depress animation, 0..1 per touch button face: [0..3] = the item
// buttons (X, Y, I, II — same indexing as s_denyFlash/s_holdBtn) which
// follow their LIVE hold state (the 24-frame synthetic tail included, so
// the visual mirrors exactly what the game receives); [4] = transform and
// [5] = Z, one-shot taps that pulse and decay. Ticked with the hold
// plumbing in beginFrameCompanionInput; the draws scale/veil by it.
extern f32 s_pressAnim[6];
// Equip landing pop, per item button: set to 1 when a button's SELECT index
// changes to a real item (wheel or companion equip) — the face does a brief
// reverse-pinch so the eye finds where the item went. Decays in
// beginFrameCompanionInput.
extern f32 s_popAnim[4];
extern f32 s_readerZoomT;
extern bool s_readerZoomClosing;
extern f32 s_readerZoomFrom[4];
bool readerZoomStep(f32* io_x0, f32* io_y0, f32* io_x1, f32* io_y1);
f32 readerZoomProgress();
bool readerZoomActive();
void readerZoomOpenFrom(f32 x0, f32 y0, f32 x1, f32 y1);
// Drag-ghost feel. s_ghostPop: pickup pop (ghost starts 15% large, eases
// down; set to 1 when a drag engages). s_ghostFly*: after release, the
// ghost flies and shrinks into its destination — the drop target on a
// consumed drop, its home grid cell on a miss (t 0->1, active while
// item != 0xFF).
// COLLECT section grow/shrink: t eases 0->1 opening a section out of its
// tapped library cell (s_collectZoomFrom, window coords) and back down when
// the context tab closes it (s_collectZoomClosing; the dispatch flips the
// tab to overview when it lands).
extern f32 s_collectZoomT;
extern bool s_collectZoomClosing;
extern f32 s_collectZoomFrom[4];
// Left-box carousel: live finger offset while a swipe rides the box, eased
// back to 0 by the draw after release (the flip itself stays on release).
extern f32 s_leftBoxDragY;
extern bool s_leftBoxTracking;
// Wolf morph: eases 0->1 on human->wolf (and back), driving the right
// column's content crossfade-by-pinch. companionWolf() stays the logic
// truth; this is the VISUAL state.
extern f32 s_wolfBlend;
extern f32 s_ghostPop;
extern f32 s_ghostFlyT;
extern f32 s_ghostFlyFromX, s_ghostFlyFromY;
extern f32 s_ghostFlyToX, s_ghostFlyToY;
extern u8 s_ghostFlyItem;
extern int s_ghostFlySlot;
// Companion cutscene dim: 0..1, ramped in beginFrameCompanionInput while
// dComIfGp_event_runCheck() holds. drawDashboard paints a black overlay
// scaled by it and handleTouch swallows input above 0.5 — during events the
// companion has nothing actionable, and a glowing dashboard next to a
// cutscene is a distraction.

// I/II slot binding = the REAL savedata select-item indices 2/3 (the Wii
// version already reserved them), so bindings persist with the game save and
// the buttons are first-class everywhere in Link's code. Returns a validated
// inventory slot index or -1.
int slotBinding(int i_which);
void setSlotBinding(int i_which, int i_slot);
// Whether ITEMS-page equip mode is live (selection or drag in flight) — the
// X/Y ring and the drop targets key off this; touch-to-use is suppressed.
bool inEquipMode();

// Size of the button drawn at the equip drop targets — the diamond and the
// Functional column use different button sizes, so the draw publishes it
// rather than drawEquipTargets assuming one.
extern f32 s_dropBtnSize;

// Main tab strip geometry, published by the draw so the touch pass never
// recomputes it. The visible tab set differs per layout (Functional drops
// GUIDE), so page ids are published alongside the rects.
constexpr int TAB_RECT_MAX = PAGE_COUNT;
extern f32 s_tabRects[TAB_RECT_MAX][4];
extern int s_tabRectPage[TAB_RECT_MAX];
extern int s_tabRectCount;

// Pages the current layout shows, in tab order. Returns the count and fills
// o_pages. Functional drops GUIDE and centres MAP, per the reference layout —
// it reaches the guide from the left pane instead.
int visiblePages(int* o_pages);

// Content window rect {x0, y0, x1, y1}, published by drawContentWindow. The
// two layouts place it differently, so page-level touch geometry must be
// derived from this rather than from layout constants — the page draws are
// handed insets off these same edges.
extern f32 s_contentRect[4];

// Warp: opens the game's field map already armed in portal-warp mode. The
// tap sets the request, consumed on the game frame loop (f_ap_game) where the
// menu status write is picked up the same frame. In Functional the trigger is
// the left column's context tab (s_ctxTabRect); the old in-map button rect is
// retired.
extern std::atomic<bool> s_warpReq;

// Functional left-column context tab: one permanent slot whose action depends
// on the current page/state (Warp / Floor / Info / Read). Rect published by
// the draw; the touch pass resolves the action itself so the two can't drift.
enum ContextTabAction {
    CTX_NONE,   // no action here — tab drawn in the unselected (dim) style
    CTX_WARP,   // MAP, overworld: open the portal-warp map
    CTX_FLOOR,  // MAP, dungeon: toggle the floor-select overlay
    CTX_INFO,   // ITEMS: read the selected item's description
    CTX_HOME,   // COLLECT: jump straight back to the library overview
    CTX_BACK,   // a detail view is open — close it (replaces its Back button)
};
// Resolves the tab's action from the current page + world state. o_clickable
// is set when the action is actually available (selected-plate style); an
// unavailable action still returns its id so the tab can label itself, but
// draws dim and does nothing on tap.
int contextTabAction(bool* o_clickable);
extern f32 s_ctxTabRect[4];
// Draws the context tab into a rect and publishes s_ctxTabRect. Called from
// the Functional left column and from the Cinematic content window.
void drawContextTab(f32 x0, f32 y0, f32 x1, f32 y1);
// Cinematic-only: draws the context tab (and floor overlay) as an in-window
// button. No-op in Functional. Called after the page content, within the
// content-window scissor.
void drawCinematicContextTab(f32 x0, f32 y0, f32 x1, f32 y1);

// Functional bottom-left zone: a three-page carousel, swiped horizontally.
// Page 0 is context-sensitive (dungeon items, or the Vessel of Light during a
// tears quest); 1 and 2 are always available.
constexpr int LEFT_BOX_PAGES = 4;  // upper bound; the live list can be shorter
constexpr int LEFT_BOX_CONTEXT = 0;
constexpr int LEFT_BOX_PROGRESS = 1;
constexpr int LEFT_BOX_PLACE = 2;
constexpr int LEFT_BOX_GUIDE = 3;
// s_leftBoxPage holds a page ID, not an index — the available set changes as
// you walk in and out of dungeons, so an index would silently mean something
// different from one room to the next.
extern std::atomic<int> s_leftBoxPage;
extern f32 s_leftBoxRect[4];
// Set at touch-down: whether the gesture started over the content window.
// Gates the map pan and pinch so a swipe elsewhere can't drag the map.
// Atomic: written at touch ingress (Android UI thread), read by the game
// thread's drag pass and by pinchZoom.
extern std::atomic<bool> s_downOnContent;
// Fills o_pages (>= LEFT_BOX_PAGES entries) with the available page IDs in
// display order and returns the count. Shared by the draw and the swipe so
// they can't disagree about what is on screen.
void drawBattery(f32 x, f32 y);
bool drawMeterBar(f32 x0, f32 x1, f32 cy, f32 barH, bool allowOil);
constexpr f32 CLUSTER_BTN = 38.0f;
void publishEquipDropRect(int dropIdx, f32 x, f32 y, f32 btn = CLUSTER_BTN);
bool drawFpsReadout(f32 x, f32 baselineY);

extern f32 s_dropBtnPos[2][2];
extern std::atomic<int> s_batteryPct;
extern std::atomic<bool> s_batteryCharging;


// --- Guide reader overlay, companion_guide.cpp ---
// An overlay over the content window rather than a Page: see the note at the
// top of companion_guide.cpp for why a Page is not viable here.
void drawGuideOverlay(f32 x0, f32 y0, f32 x1, f32 y1);
bool handleGuideTouch(f32 tx, f32 ty);
bool guideIsOpen();
void guideOpen();
void guideClose();
void guideBack();
void guideRowTap(int row);
// True once at least one guide is saved — gates the left-column Guide page.
bool guideAvailable();
void drawLeftGuideBox(f32 x1, f32 y0, f32 y1);
extern f32 s_scrollGuide;

// --- Shared + Cinematic widgets, companion_hud.cpp ---
const char* tabName(int page);
f32 drawOilGauge(f32 rightX, f32 cy);
void drawHeartsRow(f32 x0, f32 x1);
void drawDungeonIcons(f32 x, f32 y);
void drawEquipTargets();
u8 resolveBItem(bool menuOpen);
void drawButtonAmmoChip(int ammo, f32 x, f32 y);
void fetchMenuPromptWords();
void mapPromptWords(int winStatus, const char** o_a, const char** o_b);
void drawMidnaButton(dMeter2Draw_c* md, f32 zx, f32 zy);
void drawItemCluster(f32 x1, f32 y0);
f32 tabRaise(int i_page, bool i_active);
void drawBottomFlourish(f32 x0, f32 y1);
void drawTabs(f32 x0, f32 x1, f32 h);
void drawBackdrop(f32 w, f32 h);
void drawTopBar(dMeter2Draw_c* md, f32 w, f32 x1);
void drawVesselOfLight(dMeter2Draw_c* md, f32 x1, f32 h);
void drawWindowOrnaments(f32 x0, f32 y0, f32 x1, f32 y1);
void drawContentWindow(f32 wx0, f32 wx1, f32 cy0, f32 cy1);
f32 drawDpadGlyph(dMeter2Draw_c* md, f32 x1, f32 bottomY);
bool drawTransformPlate(f32 bx, f32 by, f32 bw, f32 bh);
f32 drawTransformButton(f32 x1, f32 bottomY);
f32 drawStatusCorner(f32 x1, f32 bottomY);
void drawComboChoice();
void drawDragGhost();

// --- Functional ("3DS style") layout, companion_functional.cpp ---
void drawFunctionalTopBar(f32 w);
void drawFunctionalItemButtons(dMeter2Draw_c* md, f32 colX, f32 y0, f32 y1);
void drawFunctionalLeftColumn(dMeter2Draw_c* md, f32 colX, f32 y0, f32 y1);
void drawFunctionalCorners(dMeter2Draw_c* md, f32 w, f32 h);
bool leftVesselAvailable();
bool leftDungeonAvailable();
int leftBoxPages(int* o_pages);
// Current region name ("Hyrule Field" / "Lakebed Temple") — the map screen's
// own spot name, cached per stage + room. Shared by the map name plate and
// the left column's place page.
const char* mapStageName();

// COLLECT Skills/Mail: selected row (-1 = none), mirroring ITEMS' s_selSlot.
// A row tap selects (tapping the selection again deselects); the context
// tab's Read action opens the selection.
extern int s_collectSel;

// COLLECT library icon row (overview only): Bugs/Fish/Skills/Mail plates
// that open their section as a detail view (s_collectTab 1-4). Rects
// published by the draw; zero width while a detail view is open. Back is
// the context tab in both modes.
extern f32 s_collectIconRects[4][4];

// COLLECT Skills/Mail reader: selected entry (-1 = list view) plus the tap
// rects the draw publishes for the touch pass (ids: >= 0 open that entry —
// deferred to touch-up so drags scroll instead, -2 back to list —
// immediate). The scrollable views each keep a pixel offset; drags
// accumulate into them (touch side), the draws clamp them.
extern int s_readerSel;
extern int s_readerTapCand;
extern int s_readerRectCount;
extern f32 s_readerRects[12][4];
extern int s_readerRectIds[12];
extern f32 s_scrollSkills;
extern f32 s_scrollMail;
extern f32 s_scrollBody;

// ITEMS item-info view: slot being read (-1 = grid), plus the Info/Back
// button rect (w == 0 when hidden). The reader body helpers below render
// the description text.
extern int s_itemInfoSlot;
extern f32 s_itemInfoBtnRect[4];
extern f32 s_scrollItemInfo;

// Reader detail views: bottom of the scrollable text region. Cinematic
// reserves a 44px footer for the in-window Back plate; Functional retires
// that button (the context tab is Back), so the body reclaims the space.
inline f32 readerTextBottom(f32 by1) {
    return by1 - (dusk::dualscreen::mainHudRestored() ? 6.0f : 44.0f);
}

// companion_collect.cpp reader body helpers (shared wrapped-line buffer).
void readerWrapBody(const char* body, f32 width, f32 ts);
int readerBodyLineCount();
void readerDrawBodyLine(int idx, f32 x, f32 y, f32 ts, u32 rgba);
void readerInvalidate();
// Bumped whenever the shared body buffer is rewrapped — callers caching
// their own fetch must refetch when it moves on without them.
u32 readerBodyGen();

// Combo-or-replace chooser. GC vanilla splits this on the wheel: an X/Y
// equip REPLACES, only the explicit R press combines — touch has no second
// button, so dropping a combo partner (bombs/hawkeye) onto a bow-holding
// button opens a two-plate popup instead of assuming. btn/slot hold the
// pending drop (-1 = closed); rects [0] = combine, [1] = replace; any tap
// outside cancels.
extern int s_comboChoiceBtn;
extern int s_comboChoiceSlot;
extern f32 s_comboChoiceRects[2][4];

// Drag-scroll support shared by every scrolling list/body: clamp the
// offset to the content (returns the max), and draw the position hint bar.
f32 clampListScroll(f32* io_scroll, f32 contentH, f32 viewH);
void drawListScrollHint(f32 x1, f32 y0, f32 y1, f32 scroll, f32 maxScroll, f32 viewH,
    f32 contentH);

// MAP page view: gesture zoom/pan. The pinch delta accumulates from the
// Android UI thread in milli-units ((factor-1)*1000, additive); the map
// draw consumes it. s_mapPanX/Y accumulate raw canvas-pixel drag deltas
// from the touch code; the map draw converts them into the world-space
// view offset s_mapViewOffX/Z (cm) that steers the live map render (see
// dMap_c::_draw via mapViewWorldOffset), then zeroes them. All f32 state
// is game-thread only.
// Zoom > 1 magnifies the texture; zoom < 1 is a real render zoom-out:
// s_mapRenderScale (= 1/zoom) widens the renderer's window via
// mapViewAdjust so the texture covers more world.
extern std::atomic<int> s_mapPinchDeltaMilli;
extern f32 s_mapZoom;
extern f32 s_mapRenderScale;
extern f32 s_mapPanX;
extern f32 s_mapPanY;
extern f32 s_mapViewOffX;
extern f32 s_mapViewOffZ;
// Reset-view glide: armed by the Reset button, ticked on the MAP page in
// drawDashboard — pan (and in field mode zoom/steer) ease home instead of
// snapping, with a settle tick when they land. Any manual pan/pinch
// cancels it.
extern bool s_mapResetGlide;
// True while the page-slide transition runs (drawContentWindow): page
// CONTENT touch is suppressed until the slide lands — both pages publish
// rects mid-slide. Tabs and the permanent chrome stay live.
extern bool s_pageSliding;
// Active content-window clip in NATIVE pixels (w == 0 when none). Page
// content that scissors internally must restore via applyWinClip(), NOT to
// full screen — during the page-slide transition pages draw at an offset
// and a full-screen restore lets them paint over the dashboard chrome.
extern u32 s_winClip[4];
void applyWinClip();
// Set a scissor from LOGICAL canvas coords, intersected with the active
// window clip. Inner clips must go through this: during the page-slide the
// page rects extend past the window, and a raw GXSetScissorRender computed
// from them REPLACES the window clip and bleeds (negative coords also wrap
// the u32 casts). Empty intersections draw nothing.
void setWinScissor(f32 x0, f32 y0, f32 x1, f32 y1);
// Reset-view button rect (x0,y0,x1,y1), published by the map draw for the
// touch hit test; w == 0 when hidden.
extern f32 s_mapResetRect[4];

// Dungeon-map view (companion_dmap.cpp): live full-floor render of the
// pause-menu dungeon map during gameplay (it fully replaces the minimap in
// dungeons). s_dmapAvailable = in a dungeon with map-path data;
// s_dmapReady = the renderer is alive and its texture holds the view
// published in s_dmapView*.
// dmapTimg() returns the render target's ResTIMG (NULL when not created).
// The render target is DMAP_TEX_SIZE logical texels square — all world <->
// canvas math uses this, NOT the timg's (resolution-boosted) pixel size.
constexpr int DMAP_TEX_SIZE = 448;
const ResTIMG* dmapTimg();
// Bumped whenever the dungeon renderer (and its ResTIMG) is destroyed, so
// the page's blit cache can't key on a stale/reallocated texture pointer.
extern u32 s_dmapGen;
// Floor-list marker icons from the disc's GC layout archive: Link's face
// (wolf form aware; the bool overload picks a form explicitly) and the
// floor-list boss icon; NULL if unavailable.
const ResTIMG* dmapFloorFaceTimg();
const ResTIMG* dmapFloorFaceTimg(bool i_wolf);
// Point-sampled copy for large draws (the transform button) — the 44x45
// source goes soft under the BTI's own bilinear filter when scaled up.
const ResTIMG* dmapFloorFaceTimgSharp(bool i_wolf);
const ResTIMG* dmapFloorBossMarkTimg();
extern bool s_dmapAvailable;
extern bool s_dmapReady;
extern f32 s_dmapViewCx;
extern f32 s_dmapViewCz;
extern f32 s_dmapCmPerTexel;
extern int s_dmapViewFloor;

// Dungeon-view gestures/state (the floor map fully replaces the minimap in
// dungeons). Floor DMAP_FLOOR_FOLLOW tracks the player. Zoom/pan are REAL
// render adjustments (cm-per-texel divide + center offset), consumed by
// the page draw and applied by dmapUpdate next frame.
constexpr int DMAP_FLOOR_FOLLOW = -128;
extern int s_dmapFloorSel;
extern f32 s_dmapZoom;
extern f32 s_dmapOffX;
extern f32 s_dmapOffZ;
// Room-follow: the center slides to the stay room (minimap-style) until a
// manual drag breaks it; Reset re-fits the room zoom and re-follows.
extern bool s_dmapFollow;
extern bool s_dmapResetReq;
// Floor selector, top right inside the map window: one compact button
// showing the viewed floor; tapping opens a vertical pop-up list.
// Unavailable floors (nothing to draw: no map item and no visited room
// there) are dimmed and publish no rect.
// s_dmapFloorAvail: bit (floorNo + 5) set when the floor has content.
extern bool s_dmapFloorPickOpen;
extern f32 s_dmapFloorPickT;
// The game's dungeon floors span B5..7F, so every floor-indexed array and
// every row-count clamp is bounded by this — NOT by the unrelated 13 above.
// --- Animation curves ---
//
// Every companion animation is one of two shapes: an APPROACH toward a target
// (x += (target - x) * rate) or a DECAY toward zero (x *= rate), stepped once
// per render frame. These used to be eight ad-hoc literals scattered across
// six files, so widgets that should have matched — the reader zoom and the
// collect zoom, the floor picker and the page transition — drifted apart by a
// few hundredths for no reason. The values below are the medians of what they
// replaced, so the feel is close, but now two widgets that open the same way
// open at the same speed.
//
// Pick by INTENT, not by number: a pop-up opens FAST, a dismissal decays FAST
// (gone before the eye comes back), a press tail decays SOFT (felt rather than
// seen), a continuous follow GLIDEs.
constexpr f32 ANIM_RATE_SNAP = 0.50f;     // button press attack: must land under the finger
constexpr f32 ANIM_RATE_FAST = 0.30f;     // pop-ups opening: reader/collect zoom, floor picker, ghost fly, map reset
constexpr f32 ANIM_RATE_SETTLED = 0.20f;  // long travel that should read as motion: the page transition
constexpr f32 ANIM_RATE_GLIDE = 0.12f;    // continuous follow rather than a discrete open: tab raise, dmap follow

constexpr f32 ANIM_DECAY_FAST = 0.70f;    // dismissals: zoom close, picker close, pan settle
constexpr f32 ANIM_DECAY_SOFT = 0.80f;    // tails meant to be felt: press pop, ghost pop, floor fade

// Snap thresholds for values normalised to 0..1 — past these the animation is
// finished and the value is pinned. (Do NOT use them on quantities in other
// units; tabRaise's 0..8 raise has its own.)
constexpr f32 ANIM_DONE = 0.97f;
constexpr f32 ANIM_ZERO = 0.03f;

constexpr int DMAP_FLOOR_COUNT = 13;
extern f32 s_dmapFloorRects[DMAP_FLOOR_COUNT][4];
extern int s_dmapFloorVals[DMAP_FLOOR_COUNT];
extern int s_dmapFloorRectCount;
extern u16 s_dmapFloorAvail;
// Boss floor for the floor-plate marker (DMAP_FLOOR_FOLLOW when hidden:
// needs compass, boss switch condition, boss alive — the menu's gating).
extern int s_dmapBossFloor;

// Overlay icons for the viewed floor (world XZ + a d_menu_map_common
// ICON_*_e id + Y rotation for directional icons), gathered by dmapUpdate
// with the pause map's visibility policy; drawn by the page.
struct DmapIcon {
    f32 x;
    f32 z;
    s16 rot;
    u8 icon;
};
extern DmapIcon s_dmapIcons[96];
extern int s_dmapIconCount;

// Native offscreen canvas: physical texture size + canvas-unit -> pixel
// scale for raw scissor coords.
extern f32 s_pixelScale;
extern u32 s_nativeW;
extern u32 s_nativeH;

// ---------------------------------------------------------------------------
// companion_gfx.cpp — drawing primitives.

J2DPicture* createPicture(const ResTIMG* timg);
// Drop the dungeon-map blit picture (its ResTIMG is about to die).
void invalidateDmapPicture();
void fillRect(f32 x, f32 y, f32 x2, f32 y2, GXColor color);

// Fade a whole block of drawing without covering it: every primitive
// multiplies its alpha by this. Used for the page transition and the
// mail/skill pop-ups, so the window's backdrop, border and ornaments show
// through unchanged instead of being painted over. ALWAYS restore to 1.0f.
extern f32 s_drawAlpha;
// Scale an alpha by s_drawAlpha. Needed by the draw paths that own their own
// J2DPicture (the live map render, the item icons) instead of going through
// the primitives in companion_gfx — without this they stay fully opaque and
// pop rather than fade.
u8 mulDrawAlpha(u8 a);
GXColor mulDrawAlpha(GXColor c);
// Chamfered rectangle: 45-degree corner cuts of size ch on the corners
// selected by cornerMask (1 = TL, 2 = TR, 4 = BR, 8 = BL; 0xF = all).
void fillChamferVGrad(f32 x0, f32 y0, f32 x1, f32 y1, f32 ch, GXColor top, GXColor bot,
    int cornerMask);
void fillChamferRect(f32 x0, f32 y0, f32 x1, f32 y1, f32 ch, GXColor color, int cornerMask);
// Flat-color annulus (equip drop-target highlight on the round X/Y buttons).
void drawRing(f32 cx, f32 cy, f32 radius, f32 thickness, GXColor color);
// Flat-color convex polygon.
void fillPolyPublic(const f32* xy, int count, GXColor color);
// Flat-color filled circle.
void fillDisc(f32 cx, f32 cy, f32 r, GXColor color);
// Tab plate texture drawn into a chamfered silhouette (Functional corner
// boxes) — leaves the cut corners empty so the backdrop shows through.
// cornerMask picks which corners are cut (1 TL, 2 TR, 4 BR, 8 BL) — the
// corner boxes cut all four, the context tab only its left pair.
void drawChamferPlate(f32 x0, f32 y0, f32 x1, f32 y1, f32 ch, bool selected,
    int cornerMask = 1 | 2 | 4 | 8);
// Matching outline, thickness t, drawn just inside the same bounds.
void drawChamferFrame(f32 x0, f32 y0, f32 x1, f32 y1, f32 ch, f32 t, GXColor color,
    int cornerMask);
void drawText(f32 x, f32 y, f32 size, u32 rgba, const char* fmt, ...);
f32 measureText(f32 size, const char* text);
void drawTextCentered(f32 cx, f32 y, f32 size, u32 rgba, const char* text);
void toUpperLatin1(char* s);
const char* localizedWord(u32 msgId, const char* english, bool upper = false);
const char* archiveText(u32 msgId, const char* fallback, bool upper = false);
// UI label: English keeps the dashboard's wording, others take the archive.
const char* archiveLabel(u32 msgId, const char* english, bool upper = false);
f32 fittedTextSize(f32 size, f32 minSize, f32 maxW, const char* text);
void drawTextEllipsized(f32 x, f32 y, f32 size, f32 maxW, u32 rgba, const char* text);
int fitPrefix(f32 size, f32 maxW, const char* text);
void drawTextFittedCentered(f32 cx, f32 y, f32 size, f32 minSize, f32 maxW, u32 rgba,
    const char* text);
void drawTimg(const ResTIMG* timg, f32 x, f32 y, f32 w, f32 h, u8 alpha);
void drawTimgRotated(const ResTIMG* timg, f32 cx, f32 cy, f32 size, f32 angleDeg, u8 alpha);
// Rotated draw with an explicit w x h box (for art stored sideways).
void drawTimgRotatedRect(const ResTIMG* timg, f32 cx, f32 cy, f32 w, f32 h, f32 angleDeg,
    u8 alpha);
// Tinted variant for the menu's intensity textures: black/white RGBA pairs
// feed the picture's TEV colors (what the game's own layouts do).
void drawTimgTintedMirror(const ResTIMG* timg, f32 x, f32 y, f32 w, f32 h, u8 alpha,
    u32 blackRgba, u32 whiteRgba, bool mirrorX, bool mirrorY);
void drawTimgTinted(const ResTIMG* timg, f32 x, f32 y, f32 w, f32 h, u8 alpha, u32 blackRgba,
    u32 whiteRgba);
// Menu box: chamfered rectangle with a subtle warm outline, tinted to
// fillRgba.
void drawMenuBox(f32 x0, f32 y0, f32 x1, f32 y1, u32 fillRgba);
// The menu's button plate (Save/Options look): silvery cream when
// selected, dark stone otherwise.
void drawTabPlate(f32 x, f32 y, f32 w, f32 h, bool selected);
void drawDetailBox(f32 x0, f32 y0, f32 x1, f32 y1);

// Composite a pane subtree aspect-fit into a box. o_map (optional, 5
// floats) reports the root-relative -> box affine used for the layers:
// {dstX, dstY, minX, minY, scale}; scale stays 0 when nothing drew.
void drawPaneComposite(J2DPane* root, f32 x, f32 y, f32 boxW, f32 boxH, u8 minAlpha = 0,
    bool alignRight = false, bool skipLargest = false, f32* o_map = NULL,
    bool dropPlate = false);
void drawButtonCircleBase(dMeter2Draw_c* md, int xyIdx, f32 x, f32 y, f32 box);
void drawBeveledCornerButton(f32 x0, f32 y0, f32 x1, f32 y1, bool enabled, int cornerMask = 0xF);
void drawBeveledBorder(f32 x0, f32 y0, f32 x1, f32 y1, bool enabled, int cornerMask = 0xF);
// HUD digit textures; y is the glyph TOP (not a baseline). Draws nothing
// when the Main2D archive is unavailable.
void drawHudNumber(int value, f32 x, f32 y, f32 digitH);
dMeter2Draw_c* meterDraw();

// ---------------------------------------------------------------------------
// companion_icons.cpp — icon caches.

void drawItemIcon(int slot, u8 itemNo, f32 x, f32 y, f32 size, u8 alpha = 0xFF);
// Flat-color silhouette of an item icon (border/glow backing).
void drawItemIconSilhouette(int slot, u8 itemNo, f32 x, f32 y, f32 size, u32 rgba);
// Read the 13 collect-archive BTIs once (archive is mounted from boot).
void loadCollectIcons();
// Cached collect-archive texture by CLCT slot, NULL when unavailable.
const ResTIMG* collectIconTimg(int slot);
// Scent display name + collect-icon slot for a scent item number (-1 icon /
// "-" name when none held). Used by the COLLECT overview row and the wolf
// form's slot I corner readout.
const char* resolveScent(u8 scent, int* o_iconSlot);
const ResTIMG* rawArchiveIcon(int slot, int resIdx);
// Pause-menu decoration texture by DECO_* slot, NULL when unavailable.
const ResTIMG* decoTimg(int slot);
// The embedded Dusklight hero icon (always available, no archive needed).
const ResTIMG* dusklightLogoTimg();
// Ammo readout for an item, or -1 when it has none. i_xy: X/Y button index
// for equipped items (bombs read their own bag); i_invSlot: inventory slot
// for the ITEMS grid (bomb bags live in SLOT_15..SLOT_17).
int ammoForItem(u8 itemNo, int i_xy = -1, int i_invSlot = -1);

// ---------------------------------------------------------------------------
// companion_touch.cpp — input + equip.

void handleTouch(f32 w, f32 h);
void processDragTouch(f32 w, f32 h);
// Gear box index -> item shown/equipped (mirrors the pause collection
// screen's two-boxes-per-type layout).
u8 gearItemFor(int idx);

// ---------------------------------------------------------------------------
// Page content (companion_pages.cpp / companion_collect.cpp).

void drawMapContent(f32 x0, f32 y0, f32 x1, f32 y1);
// Dungeon-map helpers: floor name lookup (message resources) and the pause
// map's link-arrow texture.
const char* dmapFloorName(int floorNo);

// Functional context-tab content helpers (plate drawn by the caller). Warp
// and Floor live in companion_pages.cpp beside the map draw; drawFloorOverlay
// draws Floor's rightward pop-up list at (px, py).
void drawWarpTab(f32 x0, f32 y0, f32 x1, f32 y1, bool active);
void drawFloorTab(f32 x0, f32 y0, f32 x1, f32 y1, bool active);
// (tx0,ty0,tx1,ty1) is the CONTEXT TAB's rect; the list drops from its
// bottom edge, clamped into [clampY0, clampY1].
int dmapFloorCount();
void drawFloorColumn(f32 tx0, f32 ty0, f32 tx1, f32 ty1, f32 clampY0, f32 clampY1,
    f32 canvasH);
void drawFloorOverlay(f32 tx0, f32 ty0, f32 tx1, f32 ty1, f32 clampY0 = 0.0f,
    f32 clampY1 = 0.0f);
const ResTIMG* dmapLinkIconTimg();
void drawInventoryContent(f32 x0, f32 y0, f32 x1, f32 y1);
void drawCollectionContent(f32 x0, f32 y0, f32 x1, f32 y1);

}  // namespace dusk::companion
