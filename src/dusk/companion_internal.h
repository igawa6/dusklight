#pragma once

// Internal shared surface of the companion dashboard modules. Everything
// here is private to src/dusk/companion*.cpp — the public API lives in
// include/dusk/companion.h.
//
// Layout of the modules:
//   companion_gfx.cpp     low-level drawing primitives + pane compositing
//   companion_icons.cpp   item/collect icon caches (all static buffers)
//   companion_touch.cpp   touch input, drag & drop, equip actions
//   companion_pages.cpp   MAP / ITEMS / QUEST page content
//   companion_collect.cpp COLLECT page content
//   companion_dmap.cpp    live dungeon-map renderer (menu-map reuse)
//   companion.cpp         dashboard composition, public API, and the
//                         definitions of all shared state below

#include <atomic>
#include <cstdint>

#include "dolphin/types.h"
#include "dolphin/gx/GXStruct.h"
#include "d/d_save.h"

class J2DPicture;
class J2DPane;
struct ResTIMG;
class dMeter2Draw_c;

namespace dusk::companion {

// ---------------------------------------------------------------------------
// Types

enum Page {
    PAGE_MAP,
    PAGE_INVENTORY,
    PAGE_COLLECTION,
    PAGE_QUEST,
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
constexpr int ICON_SLOT_COUNT = ICON_SLOT_LETTER + 1;

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
constexpr int DECO_COUNT = 4;
constexpr int DECO_BLOCKS = 0;     // TT_BLOCK128: stone-block menu backdrop
constexpr int DECO_TAB_PLATE = 1;  // the Save/Options button plate
constexpr int DECO_LINE = 2;       // TT_LINE2: soft separator line
constexpr int DECO_YAKUSHIMA = 3;  // TT_YAKUSHIMA: the Link-box mottled background

// ---------------------------------------------------------------------------
// Palette, matched to the pause menu's smoky stone-and-parchment look:
// warm dark panels, parchment text, gold accents.

constexpr GXColor COL_BG = {17, 17, 16, 255};
constexpr GXColor COL_TAB = {38, 36, 32, 215};
constexpr GXColor COL_TAB_ACTIVE = {214, 206, 186, 255};
constexpr u32 TEXT_MAIN = 0xF0E8D0FF;   // parchment
constexpr u32 TEXT_DIM = 0xA69C82FF;
constexpr u32 TEXT_ACCENT = 0xE9CE8EFF;  // gold
constexpr u32 TEXT_TAB_ACTIVE = 0x2E2415FF;

constexpr f32 HEARTS_H = 26.0f;
constexpr f32 TABS_H = 44.0f;
// COLLECT page sub-tabs (All/Bugs/Fish/Skills/Mail).
constexpr int COLLECT_TAB_COUNT = 5;
// Gap between the COLLECT sub-tab plates (shared by draw and touch).
constexpr f32 CTAB_GAP = 10.0f;
constexpr f32 CTAB_H = 40.0f;  // sub-tab strip height (plate = CTAB_H - 4)

// ---------------------------------------------------------------------------
// Shared state (defined in companion.cpp — see the "Shared state" block).

extern std::atomic<int> s_page;
extern std::atomic<int> s_collectTab;
extern std::atomic<int> s_questTab;

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

// ITEMS page geometry, written by the page draw, read by touch hit tests.
extern bool s_invGeomValid;
extern f32 s_invCell;
extern InvCell s_invCells[24];
extern int s_invCellCount;

// Equip drop rectangles anchored at the cluster X/Y buttons; valid only
// while an item is selected or dragged on the ITEMS page.
extern bool s_dropRectValid;
extern f32 s_dropRect[2][4];  // x0, y0, x1, y1

// Wolf/human quick-transform button above the d-pad. Rect published by the
// dashboard draw (w == 0 when hidden); a tap sets the request, consumed on
// the game frame loop (f_ap_game) where starting the transform is safe.
extern f32 s_transformBtnRect[4];
extern std::atomic<bool> s_transformReq;

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
extern f32 s_scrollQuest;
extern f32 s_scrollSkills;
extern f32 s_scrollMail;
extern f32 s_scrollBody;

// ITEMS item-info view: slot being read (-1 = grid), plus the Info/Back
// button rect (w == 0 when hidden). The reader body helpers below render
// the description text.
extern int s_itemInfoSlot;
extern f32 s_itemInfoBtnRect[4];
extern f32 s_scrollItemInfo;

// companion_collect.cpp reader body helpers (shared wrapped-line buffer).
void readerWrapBody(const char* body, f32 width, f32 ts);
int readerBodyLineCount();
void readerDrawBodyLine(int idx, f32 x, f32 y, f32 ts, u32 rgba);
void readerInvalidate();
// Bumped whenever the shared body buffer is rewrapped — callers caching
// their own fetch must refetch when it moves on without them.
u32 readerBodyGen();

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
extern f32 s_dmapFloorBtnRect[4];
extern f32 s_dmapFloorRects[13][4];  // full floor range (-5..7)
extern int s_dmapFloorVals[13];
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
void fillRect(f32 x, f32 y, f32 x2, f32 y2, GXColor color);
// Chamfered rectangle: 45-degree corner cuts of size ch on the corners
// selected by cornerMask (1 = TL, 2 = TR, 4 = BR, 8 = BL; 0xF = all).
void fillChamferRect(f32 x0, f32 y0, f32 x1, f32 y1, f32 ch, GXColor color, int cornerMask);
// Matching outline, thickness t, drawn just inside the same bounds.
void drawChamferFrame(f32 x0, f32 y0, f32 x1, f32 y1, f32 ch, f32 t, GXColor color,
    int cornerMask);
void drawText(f32 x, f32 y, f32 size, u32 rgba, const char* fmt, ...);
f32 measureText(f32 size, const char* text);
void drawTextCentered(f32 cx, f32 y, f32 size, u32 rgba, const char* text);
void drawTimg(const ResTIMG* timg, f32 x, f32 y, f32 w, f32 h, u8 alpha);
void drawTimgRotated(const ResTIMG* timg, f32 cx, f32 cy, f32 size, f32 angleDeg, u8 alpha);
// Rotated draw with an explicit w x h box (for art stored sideways).
void drawTimgRotatedRect(const ResTIMG* timg, f32 cx, f32 cy, f32 w, f32 h, f32 angleDeg,
    u8 alpha);
// Tinted variant for the menu's intensity textures: black/white RGBA pairs
// feed the picture's TEV colors (what the game's own layouts do).
void drawTimgTinted(const ResTIMG* timg, f32 x, f32 y, f32 w, f32 h, u8 alpha, u32 blackRgba,
    u32 whiteRgba);
// Menu box: chamfered rectangle with a subtle warm outline, tinted to
// fillRgba.
void drawMenuBox(f32 x0, f32 y0, f32 x1, f32 y1, u32 fillRgba);
// The menu's button plate (Save/Options look): silvery cream when
// selected, dark stone otherwise.
void drawTabPlate(f32 x, f32 y, f32 w, f32 h, bool selected);

// Composite a pane subtree aspect-fit into a box. o_map (optional, 5
// floats) reports the root-relative -> box affine used for the layers:
// {dstX, dstY, minX, minY, scale}; scale stays 0 when nothing drew.
void drawPaneComposite(J2DPane* root, f32 x, f32 y, f32 boxW, f32 boxH, u8 minAlpha = 0,
    bool alignRight = false, bool skipLargest = false, f32* o_map = NULL,
    bool dropPlate = false);
void drawButtonCircleBase(dMeter2Draw_c* md, int xyIdx, f32 x, f32 y, f32 box);
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

void handleTouch(f32 w, f32 h, f32 x0, f32 x1);
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
const ResTIMG* dmapLinkIconTimg();
void drawInventoryContent(f32 x0, f32 y0, f32 x1, f32 y1);
void drawQuestContent(f32 x0, f32 y0, f32 x1, f32 y1);
void drawCollectionContent(f32 x0, f32 y0, f32 x1, f32 y1);

}  // namespace dusk::companion
