// Companion dashboard: MAP / ITEMS / QUEST page content. The COLLECT page
// lives in companion_collect.cpp.

#include "dusk/companion.h"
#include "dusk/companion_internal.h"
#include "dusk/achievements.h"
#include "dusk/settings.h"

#include "JSystem/J2DGraph/J2DPicture.h"
#include "JSystem/JKernel/JKRAramArchive.h"
#include "JSystem/JKernel/JKRArchive.h"
#include "JSystem/JKernel/JKRExpHeap.h"
#include "SSystem/SComponent/c_math.h"
#include "d/actor/d_a_player.h"
#include "d/d_com_inf_game.h"
#include "d/d_item_data.h"
#include "d/d_lib.h"
#include "d/d_map.h"
#include "d/d_menu_fmap.h"
#include "d/d_menu_map_common.h"
#include "d/d_meter2.h"
#include "d/d_meter2_draw.h"
#include "d/d_meter2_info.h"
#include "d/d_meter_map.h"
#include "d/d_stage.h"
#include "dolphin/gx/GXAurora.h"

#include <cstdio>
#include <vector>
#include <cstring>

namespace dusk::companion {
namespace {

// The map page's own texture cache (independent of the gfx icon cache):
// the live map render texture is re-uploaded only when the game swaps it,
// and invalidated across stage transitions where the pointer is reused.
J2DPicture* s_mapPic;
const ResTIMG* s_lastMapTimg;
f32 s_lastMapW = 448.0f;
f32 s_lastMapH = 448.0f;
char s_mapStage[12];
bool s_mapStale = false;

// Dungeon-map blit cache (separate: the dmap target outlives page switches
// but is destroyed on stage change / dungeon exit). s_dmapSeenGen tracks
// s_dmapGen: on renderer teardown the ResTIMG pointer may be reused by the
// next dungeon, so pointer-keyed caching alone would blit a stale texture.
J2DPicture* s_dmapPic;
const ResTIMG* s_lastDmapTimg;
u32 s_dmapSeenGen = 0;

// Floor-name cache: the menu's floor message ids by (floorNo + 5); 0 means
// no string — fall back to a generated B#/#F label.
constexpr u16 l_floorMsg[13] = {
    0, 0, 0x03DB, 0x03DA, 0x036B, 0x036C, 0x036D, 0x036E, 0x036F, 0x03DC, 0x03DD,
    0x03D9, 0x03D8,
};
char s_floorName[13][28];
bool s_floorNameOk[13];
char s_floorNameStage[12];

}  // namespace

const char* dmapFloorName(int floorNo) {
    const char* stage = dComIfGp_getStartStageName();
    if (strncmp(stage, s_floorNameStage, sizeof(s_floorNameStage) - 1) != 0) {
        strncpy(s_floorNameStage, stage, sizeof(s_floorNameStage) - 1);
        s_floorNameStage[sizeof(s_floorNameStage) - 1] = '\0';
        memset(s_floorNameOk, 0, sizeof(s_floorNameOk));
    }
    const int idx = floorNo + 5;
    if (idx < 0 || idx >= 13) {
        return "?";
    }
    if (!s_floorNameOk[idx]) {
        s_floorName[idx][0] = '\0';
        if (l_floorMsg[idx] != 0) {
            dMeter2Info_getString(l_floorMsg[idx], s_floorName[idx], NULL);
        }
        if (s_floorName[idx][0] == '\0') {
            if (floorNo < 0) {
                snprintf(s_floorName[idx], sizeof(s_floorName[idx]), "B%d", -floorNo);
            } else {
                snprintf(s_floorName[idx], sizeof(s_floorName[idx]), "%dF", floorNo + 1);
            }
        }
        s_floorNameOk[idx] = true;
    }
    return s_floorName[idx];
}

// The pause map's link-arrow icon from the boot-resident dmap layout archive.
const ResTIMG* dmapLinkIconTimg() {
    static const ResTIMG* timg;
    if (timg == NULL) {
        JKRArchive* arc = dComIfGp_getDmapResArchive();
        if (arc != NULL) {
            timg = (const ResTIMG*)arc->getResource('TIMG', "tt_map_icon_link_ci8_32_00.bti");
        }
    }
    return timg;
}

namespace {

// Overlay icon textures (the menu's full-size variants), same resident
// archive, cached per icon id.
const ResTIMG* dmapIconTimg(u8 icon) {
    struct Entry {
        u8 icon;
        const char* name;
    };
    static const Entry l_tex[] = {
        {ICON_BOSS_e, "tt_map_icon_boss_ci8_32_00.bti"},
        {ICON_DUNGEON_ENTER_e, "im_map_icon_enter_ci8_02.bti"},
        {ICON_LINK_ENTER_e, "tt_map_icon_enter_ci8_32_00.bti"},
        {ICON_LV8_WARP_e, "im_map_icon_warp_32_ci8_00.bti"},
        {ICON_TREASURE_CHEST_e, "tt_map_icon_box_ci8_32_00.bti"},
        {ICON_KEY_e, "tt_map_icon_key_ci8_32_00.bti"},
        {ICON_MONKEY_e, "tt_map_icon_monkey_ci8_32_00.bti"},
        {ICON_OOCCOO_e, "ni_obacyan.bti"},
        {ICON_OOCCOO_JR_e, "ni_obacyan.bti"},
        {ICON_COPY_STATUE_e, "im_zelda_map_icon_copy_stone_statue_snup_try_00_04.bti"},
        {ICON_LIGHT_BALL_e, "im_zelda_map_icon_hikari_ball_03.bti"},
        {ICON_CANNON_BALL_e, "im_map_icon_iron_ball_ci8_32_00.bti"},
        {ICON_LIGHT_DROP_e, "im_hikari_no_shizuku_try_10_00_24x24.bti"},
        {ICON_DESTINATION_e, "im_nijumaru_40x40_ind_01.bti"},
    };
    static const ResTIMG* cache[ICON_MAX_e];
    if (icon >= ICON_MAX_e) {
        return NULL;
    }
    if (cache[icon] == NULL) {
        JKRArchive* arc = dComIfGp_getDmapResArchive();
        if (arc == NULL) {
            return NULL;
        }
        for (const Entry& e : l_tex) {
            if (e.icon == icon) {
                cache[icon] = (const ResTIMG*)arc->getResource('TIMG', e.name);
                break;
            }
        }
    }
    return cache[icon];
}

// Overlay the plate markers with the pause map floor list's own icons:
// Link's face (or wolf form) on the player's floor at the left edge, the
// boss icon on the boss floor at the right edge (compass-gated upstream).
// Falls back to the skull emblem if the disc's GC layout archive was
// unavailable.
void drawFloorPlateMarks(f32 tx, f32 ty, f32 tw, f32 th, int floorNo, int playerFloor) {
    if (playerFloor != DMAP_FLOOR_FOLLOW && floorNo == playerFloor) {
        drawTimg(dmapFloorFaceTimg(), tx + 2.0f, ty + th * 0.5f - 8.0f, 16.0f, 16.0f, 0xFF);
    }
    if (s_dmapBossFloor != DMAP_FLOOR_FOLLOW && floorNo == s_dmapBossFloor) {
        const ResTIMG* boss = dmapFloorBossMarkTimg();
        if (boss == NULL) {
            boss = dmapIconTimg(ICON_BOSS_e);
        }
        drawTimg(boss, tx + tw - 16.0f, ty + th * 0.5f - 7.0f, 14.0f, 14.0f, 0xFF);
    }
}

// Floor selector, top right inside the map window: a compact button with
// the viewed floor; tapping opens a vertical pop-up list. Viewed floor
// uses the active-tab style, other floors the idle style; floors with
// nothing to draw (no map item and nothing visited there) are dimmed and
// not tappable. Plates carry the pause map's link/boss markers.
void drawDmapFloorTabs(f32 x1, f32 y0) {
    s_dmapFloorRectCount = 0;
    const f32 tw = 52.0f;
    const f32 th = 24.0f;
    const f32 gap = 2.0f;
    const f32 tx = x1 - 4.0f - tw;
    const f32 ty = y0 + 4.0f;
    const int playerFloor = dMapInfo_c::getNowStayFloorNoDecisionFlg()
        ? dMapInfo_c::getNowStayFloorNo() : DMAP_FLOOR_FOLLOW;
    drawTabPlate(tx, ty, tw, th, true);
    drawTextCentered(tx + tw * 0.5f, ty + 17.0f, 12.0f, TEXT_TAB_ACTIVE,
        dmapFloorName(s_dmapViewFloor));
    drawFloorPlateMarks(tx, ty, tw, th, s_dmapViewFloor, playerFloor);
    s_dmapFloorBtnRect[0] = tx;
    s_dmapFloorBtnRect[1] = ty;
    s_dmapFloorBtnRect[2] = tx + tw;
    s_dmapFloorBtnRect[3] = ty + th;
    if (!s_dmapFloorPickOpen) {
        return;
    }
    s8 top = 0;
    s8 bottom = 0;
    dMpath_c::getTopBottomFloorNo(&top, &bottom);
    int count = top - bottom + 1;
    if (count > 13) {
        count = 13;
    }
    constexpr GXColor COL_TAB_SCRIM = {0, 0, 0, 150};
    f32 py = ty + th + 4.0f;
    for (int i = 0; i < count; i++) {
        const int floorNo = top - i;
        const bool viewed = floorNo == s_dmapViewFloor;
        const int bit = floorNo + 5;
        const bool hasContent =
            bit >= 0 && bit < 13 && (s_dmapFloorAvail & (1u << bit)) != 0;
        drawTabPlate(tx, py, tw, th, viewed);
        drawTextCentered(tx + tw * 0.5f, py + 17.0f, 12.0f,
            viewed ? TEXT_TAB_ACTIVE : TEXT_DIM, dmapFloorName(floorNo));
        drawFloorPlateMarks(tx, py, tw, th, floorNo, playerFloor);
        if (!hasContent && !viewed) {
            fillRect(tx, py, tx + tw, py + th, COL_TAB_SCRIM);
        } else if (s_dmapFloorRectCount < 13) {
            s_dmapFloorRects[s_dmapFloorRectCount][0] = tx;
            s_dmapFloorRects[s_dmapFloorRectCount][1] = py;
            s_dmapFloorRects[s_dmapFloorRectCount][2] = tx + tw;
            s_dmapFloorRects[s_dmapFloorRectCount][3] = py + th;
            s_dmapFloorVals[s_dmapFloorRectCount] = floorNo;
            s_dmapFloorRectCount++;
        }
        py += th + gap;
    }
}

// Whole-floor dungeon view: contain-fit blit of the companion's live
// dungeon-map render (companion_dmap.cpp) plus gestures, overlay icons and
// the realtime player arrow. Returns false when the view is not available
// so the caller falls through to the minimap.
bool drawDungeonMapContent(f32 x0, f32 y0, f32 x1, f32 y1) {
    if (s_dmapSeenGen != s_dmapGen) {
        s_dmapSeenGen = s_dmapGen;
        s_lastDmapTimg = NULL;
    }
    const ResTIMG* timg = dmapTimg();
    if (!s_dmapReady || timg == NULL || timg->width == 0 || timg->height == 0) {
        s_lastDmapTimg = NULL;
        return false;
    }
    if (s_dmapPic == NULL) {
        s_dmapPic = createPicture(timg);
        s_lastDmapTimg = timg;
    } else if (timg != s_lastDmapTimg) {
        s_dmapPic->changeTexture(timg, 0);
        s_lastDmapTimg = timg;
    }
    if (s_dmapPic == NULL || s_dmapPic->getTexture(0) == NULL) {
        return false;
    }
    const int playerFloor = dMapInfo_c::getNowStayFloorNoDecisionFlg()
        ? dMapInfo_c::getNowStayFloorNo() : DMAP_FLOOR_FOLLOW;

    const f32 bx0 = x0 + 2.0f;
    const f32 availW = x1 - bx0;
    const f32 availH = y1 - y0;
    const f32 texW = (f32)(u16)timg->width;
    const f32 texH = (f32)(u16)timg->height;
    // Contain-fit: the render window (whole floor at zoom 1) stays visible.
    const f32 fit = availW / texW < availH / texH ? availW / texW : availH / texH;
    const f32 drawW = texW * fit;
    const f32 drawH = texH * fit;
    const f32 bx = bx0 + (availW - drawW) * 0.5f;
    const f32 by = y0 + (availH - drawH) * 0.5f;

    // Gestures: pinch scales the real render zoom; drag pans the render
    // center (content follows the finger). Applied by dmapUpdate next frame.
    // All world <-> canvas math uses the LOGICAL texel size — the timg's
    // pixel size carries the PC resolution boost.
    const int pinchMilli = s_mapPinchDeltaMilli.exchange(0);
    if (pinchMilli != 0) {
        s_dmapZoom *= 1.0f + (f32)pinchMilli / 1000.0f;
        if (s_dmapZoom < 1.0f) {
            s_dmapZoom = 1.0f;
        } else if (s_dmapZoom > 4.0f) {
            s_dmapZoom = 4.0f;
        }
        // Manual zoom = free look, like a drag.
        s_dmapFollow = false;
    }
    const f32 xSign = getSettings().game.enableMirrorMode ? -1.0f : 1.0f;
    if ((s_mapPanX != 0.0f || s_mapPanY != 0.0f) && s_dmapCmPerTexel > 0.0f && drawW > 0.0f) {
        const f32 worldPerPx = s_dmapCmPerTexel * (f32)DMAP_TEX_SIZE / drawW;
        s_dmapOffX -= s_mapPanX * worldPerPx * xSign;
        s_dmapOffZ -= s_mapPanY * worldPerPx;
        // Manual drag = free look: stop following the stay room.
        s_dmapFollow = false;
    }
    s_mapPanX = 0.0f;
    s_mapPanY = 0.0f;

    GXSetScissorRender((u32)(bx0 * s_pixelScale), (u32)(y0 * s_pixelScale),
        (u32)(availW * s_pixelScale), (u32)(availH * s_pixelScale));
    s_dmapPic->draw(bx, by, drawW, drawH, false, false, false);

    // World -> canvas mapping for the overlays (same affine the render used).
    const f32 pxPerTexel = drawW / (f32)DMAP_TEX_SIZE;
    const f32 mapCx = bx + drawW * 0.5f;
    const f32 mapCy = by + drawH * 0.5f;
    if (s_dmapCmPerTexel > 0.0f) {
        // Floor icons (chests, boss, entrance, warps, ...), gathered with
        // the pause map's visibility policy.
        for (int i = 0; i < s_dmapIconCount; i++) {
            const DmapIcon& ic = s_dmapIcons[i];
            const f32 ix = mapCx + (ic.x - s_dmapViewCx) * xSign / s_dmapCmPerTexel * pxPerTexel;
            const f32 iy = mapCy + (ic.z - s_dmapViewCz) / s_dmapCmPerTexel * pxPerTexel;
            const ResTIMG* it = dmapIconTimg(ic.icon);
            const f32 sz = 24.0f;
            if (ic.icon == ICON_LIGHT_DROP_e) {
                // Tear of light: intensity texture, the menu's green tint.
                drawTimgTinted(it, ix - sz * 0.5f, iy - sz * 0.5f, sz, sz, 0xFF,
                    0x00F0AA00u, 0xFFFFE6FFu);
            } else if (ic.rot != 0) {
                const s16 rot = xSign < 0.0f ? (s16)-ic.rot : ic.rot;
                drawTimgRotated(it, ix, iy, sz, cM_sht2d((f32)rot), 0xFF);
            } else {
                drawTimg(it, ix - sz * 0.5f, iy - sz * 0.5f, sz, sz, 0xFF);
            }
        }
        // Realtime player arrow, only on the viewed floor (the render itself
        // carries no cursor — the pause map draws it as an overlay too).
        if (playerFloor != DMAP_FLOOR_FOLLOW && playerFloor == s_dmapViewFloor) {
            const Vec pos = dMapInfo_n::getMapPlayerPos();
            s16 rotY = dMapInfo_n::getMapPlayerAngleY();
            if (xSign < 0.0f) {
                rotY = -rotY;
            }
            const f32 ax = mapCx + (pos.x - s_dmapViewCx) * xSign / s_dmapCmPerTexel * pxPerTexel;
            const f32 ay = mapCy + (pos.z - s_dmapViewCz) / s_dmapCmPerTexel * pxPerTexel;
            drawTimgRotated(dmapLinkIconTimg(), ax, ay, 30.0f, cM_sht2d((f32)rotY), 0xFF);
        }
    }
    GXSetScissorRender(0, 0, s_nativeW, s_nativeH);
    dComIfGp_getCurrentGrafPort()->setup2D();

    // Reset-view button, bottom right (shared rect with the minimap mode).
    const bool viewMoved = !s_dmapFollow || s_dmapFloorSel != DMAP_FLOOR_FOLLOW;
    drawTabPlate(x1 - 70.0f, y1 - 30.0f, 66.0f, 26.0f, viewMoved);
    drawTextCentered(x1 - 37.0f, y1 - 12.0f, 13.0f,
        viewMoved ? TEXT_TAB_ACTIVE : TEXT_DIM, "Reset");
    s_mapResetRect[0] = x1 - 70.0f;
    s_mapResetRect[1] = y1 - 30.0f;
    s_mapResetRect[2] = x1 - 4.0f;
    s_mapResetRect[3] = y1 - 4.0f;
    return true;
}

// ITEMS grid: two-column box grouping —
//   left  : Tools 5x2, then Bombs 3x1 + Rod/Slingshot 2x1
//   right : Bottles 2x2 above Quest 2x2
// gx 0-4 = left block, gx 5-6 = right block (column gap applied).
struct CellDef {
    int slot;
    u8 gx, gy, group;
};
const CellDef l_invCells[24] = {
    // Tools 5x2 (group 0)
    {0, 0, 0, 0}, {1, 1, 0, 0}, {2, 2, 0, 0}, {3, 3, 0, 0}, {4, 4, 0, 0},
    {5, 0, 1, 0}, {6, 1, 1, 0}, {8, 2, 1, 0}, {9, 3, 1, 0}, {10, 4, 1, 0},
    // Bombs 3x1 (group 2) + Misc rod/slingshot 2x1 (group 4)
    {15, 0, 2, 2}, {16, 1, 2, 2}, {17, 2, 2, 2},
    {20, 3, 2, 4}, {23, 4, 2, 4},
    // Quest 2x2 (group 3) left, Bottles 2x2 (group 1) beside them — the
    // empty column sits at the far right.
    {18, 0, 3, 3}, {19, 1, 3, 3}, {21, 0, 4, 3}, {22, 1, 4, 3},
    {11, 2, 3, 1}, {12, 3, 3, 1}, {13, 2, 4, 1}, {14, 3, 4, 1},
};
const GXColor l_groupTint[5] = {
    {45, 41, 33, 255},   // tools (stone brown)
    {38, 44, 40, 255},   // bottles (moss green)
    {54, 36, 30, 255},   // bombs (rust red)
    {52, 44, 28, 255},   // quest (gold-brown)
    {42, 42, 34, 255},   // rod/slingshot (olive grey)
};

constexpr f32 INV_CAPTION_H = 26.0f;
constexpr f32 INV_GROUP_GAP = 8.0f;

// Grid metrics for the ITEMS page, fit to the content box.
struct InvGrid {
    f32 cell, originX, originY, pad, icon;
};

InvGrid computeInvGrid(f32 x0, f32 y0, f32 x1, f32 y1) {
    const f32 availW = x1 - x0;
    const f32 availH = y1 - y0 - INV_CAPTION_H;
    f32 cell = availW / 5.0f;
    if (cell * 5.0f + INV_GROUP_GAP * 2.0f > availH) {
        cell = (availH - INV_GROUP_GAP * 2.0f) / 5.0f;
    }
    const f32 gridW = cell * 5.0f;
    const f32 gridH = cell * 5.0f + INV_GROUP_GAP * 2.0f;
    InvGrid g;
    g.cell = cell;
    g.originX = x0 + (availW - gridW) * 0.5f;
    g.originY = y0 + (availH - gridH) * 0.5f;
    g.pad = cell * 0.08f;
    g.icon = cell - g.pad * 2.0f - 4.0f;
    return g;
}

// One grid cell: group-tinted box, item icon, ammo chip, drag border.
// Publishes the cell rect into s_invCells[i] for the touch hit tests.
void drawInvCell(const InvGrid& g, int i) {
    constexpr GXColor COL_CELL_SEL = {236, 208, 84, 255};
    const CellDef& d = l_invCells[i];
    const f32 cx = g.originX + d.gx * g.cell;
    const f32 cy = g.originY + d.gy * g.cell + (d.gy >= 2 ? INV_GROUP_GAP : 0.0f) +
        (d.gy >= 3 ? INV_GROUP_GAP : 0.0f);
    s_invCells[i].slot = d.slot;
    s_invCells[i].x = cx;
    s_invCells[i].y = cy;
    const u8 itemNo = dComIfGs_getItem(d.slot, false);
    // The collection screen's item box plate, tinted per group.
    const bool dragSource = s_dragging && d.slot == s_dragSlot;
    const GXColor tint =
        (d.slot == s_selSlot || dragSource) ? COL_CELL_SEL : l_groupTint[d.group];
    const u32 tintRgba = ((u32)tint.r << 24) | ((u32)tint.g << 16) | ((u32)tint.b << 8) | 0xFF;
    drawMenuBox(cx + 2.0f, cy + 2.0f, cx + g.cell - 2.0f, cy + g.cell - 2.0f, tintRgba);
    if (itemNo == dItemNo_NONE_e) {
        return;
    }
    drawItemIcon(d.slot, itemNo, cx + g.pad + 2.0f, cy + g.pad + 2.0f, g.icon);
    // Quantity: bottom-left, on a small dark chip so it reads over any
    // icon art.
    const int ammo = ammoForItem(itemNo, -1, d.slot);
    if (ammo >= 0) {
        constexpr GXColor COL_CHIP = {10, 9, 7, 210};
        const f32 chipW = (ammo >= 100 ? 3.0f : ammo >= 10 ? 2.0f : 1.0f) * 9.0f + 6.0f;
        fillRect(cx + 3.0f, cy + g.cell - 19.0f, cx + 3.0f + chipW, cy + g.cell - 3.0f,
            COL_CHIP);
        drawHudNumber(ammo, cx + 6.0f, cy + g.cell - 17.0f, 11.0f);
    }
}

// Caption line under the grid: equip notice > selected item name > hint.
// Owns the s_equipMsgFrames decrement while this page is active.
void drawInventoryCaption(f32 x0, f32 y1) {
    u8 nameItem = dItemNo_NONE_e;
    if (s_dragging && s_dragSlot >= 0) {
        nameItem = dComIfGs_getItem(s_dragSlot, false);
    } else if (s_selSlot >= 0) {
        nameItem = dComIfGs_getItem(s_selSlot, false);
    }
    // All caption variants use the Info button label's metrics (13px on
    // the y1 - 12 baseline) so the bottom row reads as one line.
    if (s_equipMsgFrames > 0) {
        constexpr u32 TEXT_WARN = 0xF0A050FF;
        drawText(x0 + 18.0f, y1 - 12.0f, 13.0f, TEXT_WARN, "%s", s_equipMsg);
    } else if (nameItem != dItemNo_NONE_e) {
        static u8 s_nameItemNo = dItemNo_NONE_e;
        static char s_nameBuf[96];
        if (s_nameItemNo != nameItem) {
            s_nameItemNo = nameItem;
            s_nameBuf[0] = 0;
            dMeter2Info_getString(0x165 + nameItem, s_nameBuf, NULL);
        }
        if (s_nameBuf[0] != 0) {
            drawText(x0 + 18.0f, y1 - 12.0f, 13.0f, TEXT_MAIN, "%s", s_nameBuf);
        }
    } else {
        drawText(x0 + 18.0f, y1 - 12.0f, 13.0f, TEXT_DIM,
            "Drag an item onto X or Y to equip");
    }
}

// QUEST page: Dusklight achievements. Snapshot cached and refreshed twice
// a second.
const std::vector<dusk::Achievement>& achievementSnapshot() {
    static std::vector<dusk::Achievement> s_ach;
    static int refresh = 0;
    if (refresh-- <= 0) {
        refresh = 30;
        s_ach = dusk::AchievementSystem::get().getAchievements();
    }
    return s_ach;
}

const char* l_catNames[5] = {"Challenge", "Collect", "Minigame", "Misc", "Glitched"};

void countByCategory(const std::vector<dusk::Achievement>& ach, int unlocked[5], int total[5]) {
    for (const dusk::Achievement& a : ach) {
        const int c = (int)a.category;
        if (c >= 0 && c < 5) {
            total[c]++;
            if (a.unlocked) {
                unlocked[c]++;
            }
        }
    }
}

// Category sub-tab strip with per-category unlocked counts.
void drawQuestTabs(f32 x0, f32 y0, f32 tabW, int active, const int unlocked[5],
    const int total[5]) {
    for (int i = 0; i < 5; i++) {
        const f32 x = x0 + i * (tabW + 2.0f);
        drawTabPlate(x, y0, tabW, 40.0f, i == active);
        drawTextCentered(x + tabW * 0.5f, y0 + 16.0f, 12.0f,
            i == active ? TEXT_TAB_ACTIVE : TEXT_DIM, l_catNames[i]);
        char cnt[16];
        snprintf(cnt, sizeof(cnt), "%d/%d", unlocked[i], total[i]);
        drawTextCentered(x + tabW * 0.5f, y0 + 34.0f, 12.0f,
            i == active ? TEXT_TAB_ACTIVE : TEXT_DIM, cnt);
    }
}

// Word-wrap a description onto up to two 11px lines within wrapW.
void drawWrappedDescription(const char* text, f32 x, f32 ry, f32 wrapW) {
    const char* p = text != NULL ? text : "";
    char line[112];
    for (int ln = 0; ln < 2 && *p != 0; ln++) {
        int len = 0;
        int lastSpace = -1;
        const char* q = p;
        while (*q != 0 && len < (int)sizeof(line) - 1) {
            line[len] = *q;
            if (*q == ' ') {
                lastSpace = len;
            }
            len++;
            line[len] = 0;
            if (measureText(11.0f, line) > wrapW) {
                if (lastSpace > 0) {
                    len = lastSpace;
                } else {
                    len--;
                }
                line[len] = 0;
                break;
            }
            q++;
        }
        drawText(x, ry + ln * 14.0f, 11.0f, TEXT_DIM, "%s", line);
        p += len;
        while (*p == ' ') {
            p++;
        }
    }
}

}  // namespace

namespace {

// Player-centered minimap view (non-dungeon stages; dungeons always use
// the floor map instead).
void drawMiniMapContent(f32 x0, f32 y0, f32 x1, f32 y1) {
    dMeterMap_c* meterMap = dMeter2Info_getMeterMapClass();
    dMap_c* map = meterMap != NULL ? meterMap->getDMap() : NULL;
    ResTIMG* timg = (map != NULL && map->isDraw()) ? map->getResTIMGPointer() : NULL;
    // On a stage change the old map render-texture is freed (and the new one
    // may reuse the same address): invalidate the cache so we neither draw
    // garbage nor skip the re-upload.
    const char* stage = dComIfGp_getStartStageName();
    if (strncmp(stage, s_mapStage, sizeof(s_mapStage) - 1) != 0) {
        strncpy(s_mapStage, stage, sizeof(s_mapStage) - 1);
        s_mapStage[sizeof(s_mapStage) - 1] = '\0';
        s_lastMapTimg = NULL;
        s_mapStale = true;
        // The world offset is in the old stage's coordinates.
        s_mapViewOffX = 0.0f;
        s_mapViewOffZ = 0.0f;
    }
    // A live, valid map texture is required every frame: the render-texture
    // is freed on stage/room transitions and a cached pointer would show
    // garbage from the reused heap.
    if (timg == NULL || timg->width == 0 || timg->height == 0) {
        s_lastMapTimg = NULL;  // force re-upload when it comes back
        // Nothing consumes the gestures this frame — drop them so they
        // don't burst into the view once the map appears.
        s_mapPanX = 0.0f;
        s_mapPanY = 0.0f;
        s_mapPinchDeltaMilli.exchange(0);
        // Hero icon centered in the map area with the status line above it.
        const f32 mcx = (x0 + x1) * 0.5f;
        const f32 mcy = (y0 + y1) * 0.5f;
        if (const ResTIMG* logo = dusklightLogoTimg()) {
            const f32 s = 96.0f;
            drawTimg(logo, mcx - s * 0.5f, mcy - s * 0.5f, s, s, 0xFF);
        }
        drawTextCentered(mcx, mcy - 60.0f, 17.0f, TEXT_DIM,
            s_mapStale ? "Loading map..." : "Map not available here.");
        return;
    }
    s_mapStale = false;
    s_lastMapW = (f32)(u16)timg->width;
    s_lastMapH = (f32)(u16)timg->height;
    if (s_mapPic == NULL) {
        s_mapPic = createPicture(timg);
    } else if (timg != s_lastMapTimg) {
        s_mapPic->changeTexture(timg, 0);
        s_lastMapTimg = timg;
    }
    if (s_mapPic == NULL || s_mapPic->getTexture(0) == NULL) {
        return;
    }
    const f32 availW = x1 - x0;
    const f32 availH = y1 - y0;
    // Use cached dimensions when the live timg is unavailable (stage transition).
    const f32 texW = (timg != NULL && timg->width != 0) ? (f32)(u16)timg->width : s_lastMapW;
    const f32 texH = (timg != NULL && timg->height != 0) ? (f32)(u16)timg->height : s_lastMapH;

    // Gesture view: consume the accumulated pinch delta, clamp the zoom,
    // and clamp the pan so the map can't be dragged out of the window.
    const int pinchMilli = s_mapPinchDeltaMilli.exchange(0);
    if (pinchMilli != 0) {
        s_mapZoom *= 1.0f + (f32)pinchMilli / 1000.0f;
        if (s_mapZoom < 0.34f) {
            s_mapZoom = 0.34f;
        } else if (s_mapZoom > 3.0f) {
            s_mapZoom = 3.0f;
        }
    }
    // Zoom-in magnifies the texture; zoom-out widens the renderer's world
    // window instead (mapViewAdjust multiplies its cm-per-texel), so the
    // texture keeps filling the map area while covering up to ~3x the world.
    const f32 zoomIn = s_mapZoom > 1.0f ? s_mapZoom : 1.0f;
    s_mapRenderScale = s_mapZoom < 1.0f ? 1.0f / s_mapZoom : 1.0f;
    const f32 fitScale = availW / texW > availH / texH ? availW / texW : availH / texH;
    const f32 scale = fitScale * zoomIn;
    const f32 drawW = texW * scale;
    const f32 drawH = texH * scale;

    // Convert the finger drag (canvas px) into a world-space view offset
    // that steers the live map render (dMap_c::_draw adds it to the render
    // window center via mapViewWorldOffset). The full texture spans
    // baseTexSize / texelPerCm world-cm, so one canvas pixel is
    // worldExtent / draw size. Content follows the finger, so the render
    // center moves the opposite way; mirror mode flips the render's X
    // projection, so the X sign flips with it.
    if ((s_mapPanX != 0.0f || s_mapPanY != 0.0f) && map->getTexelPerCm() > 0.0f) {
        const f32 worldW = (f32)map->getTexSizeX() / map->getTexelPerCm() * s_mapRenderScale;
        const f32 worldH = (f32)map->getTexSizeY() / map->getTexelPerCm() * s_mapRenderScale;
        const f32 xSign = getSettings().game.enableMirrorMode ? -1.0f : 1.0f;
        s_mapViewOffX -= s_mapPanX * (worldW / drawW) * xSign;
        s_mapViewOffZ -= s_mapPanY * (worldH / drawH);
    }
    s_mapPanX = 0.0f;
    s_mapPanY = 0.0f;
    // Keep the shifted render-window center inside the stage's map-path
    // bounds so the view can't wander into empty space forever. The range
    // is widened to always include zero: in some stages (Zora's Domain)
    // the live map center sits OUTSIDE the map-path box, and clamping
    // toward it would inject a phantom pan every frame — Reset stuck lit
    // and the view jittering against the game's own center.
    f32 loX = dMpath_c::getMinX() - map->getCenterX();
    f32 hiX = dMpath_c::getMaxX() - map->getCenterX();
    f32 loZ = dMpath_c::getMinZ() - map->getCenterZ();
    f32 hiZ = dMpath_c::getMaxZ() - map->getCenterZ();
    if (loX > 0.0f) {
        loX = 0.0f;
    }
    if (hiX < 0.0f) {
        hiX = 0.0f;
    }
    if (loZ > 0.0f) {
        loZ = 0.0f;
    }
    if (hiZ < 0.0f) {
        hiZ = 0.0f;
    }
    if (s_mapViewOffX < loX) {
        s_mapViewOffX = loX;
    } else if (s_mapViewOffX > hiX) {
        s_mapViewOffX = hiX;
    }
    if (s_mapViewOffZ < loZ) {
        s_mapViewOffZ = loZ;
    } else if (s_mapViewOffZ > hiZ) {
        s_mapViewOffZ = hiZ;
    }

    GXSetScissorRender((u32)(x0 * s_pixelScale), (u32)(y0 * s_pixelScale),
        (u32)(availW * s_pixelScale), (u32)(availH * s_pixelScale));
    s_mapPic->draw(x0 + (availW - drawW) * 0.5f,
        y0 + (availH - drawH) * 0.5f, drawW, drawH, false, false, false);
    GXSetScissorRender(0, 0, s_nativeW, s_nativeH);
    dComIfGp_getCurrentGrafPort()->setup2D();

    // Reset-view button, bottom right inside the window.
    const bool viewMoved = s_mapZoom > 1.01f || s_mapZoom < 0.99f ||
        s_mapViewOffX != 0.0f || s_mapViewOffZ != 0.0f;
    drawTabPlate(x1 - 70.0f, y1 - 30.0f, 66.0f, 26.0f, viewMoved);
    drawTextCentered(x1 - 37.0f, y1 - 12.0f, 13.0f,
        viewMoved ? TEXT_TAB_ACTIVE : TEXT_DIM, "Reset");
    s_mapResetRect[0] = x1 - 70.0f;
    s_mapResetRect[1] = y1 - 30.0f;
    s_mapResetRect[2] = x1 - 4.0f;
    s_mapResetRect[3] = y1 - 4.0f;
}

}  // namespace

// One-session copy of the pause world map's spot database (dat/field.dat
// from the boot-resident field-map archive): the spot names ARE the map
// screen's names ("Ordon Village", "Hyrule Field"). Read once, kept on the
// Zelda heap.
dMenu_Fmap_field_data_c* fieldMapDat() {
    static dMenu_Fmap_field_data_c* s_dat;
    static bool s_failed;
    if (s_dat != NULL || s_failed) {
        return s_dat;
    }
    JKRAramArchive* arc = dComIfGp_getFieldMapArchive2();
    if (arc == NULL) {
        return NULL;  // not mounted yet — retry next call
    }
    const u32 size = dLib_getExpandSizeFromAramArchive(arc, "dat/field.dat");
    if (size == 0) {
        s_failed = true;
        return NULL;
    }
    JKRHeap* prevHeap = mDoExt_setCurrentHeap(mDoExt_getZeldaHeap());
    u8* buf = JKR_NEW_ARRAY_ARGS(u8, size, 0x20);
    mDoExt_setCurrentHeap(prevHeap);
    if (buf == NULL || arc->readResource(buf, size, "dat/field.dat") == 0) {
        s_failed = true;
        return NULL;
    }
    s_dat = (dMenu_Fmap_field_data_c*)buf;
    return s_dat;
}

// Map-screen area name (msg id) for the current stage + room: the special
// rooms with their own name first (dMenu_Fmap_c::checkStRoomData's table),
// then the stage's spot entries — exact room, then the 0xff any-room entry.
// 0xffff when the stage is not on the world map (dungeons, interiors).
u16 fmapAreaNameMsg(int stayNo) {
    dMenu_Fmap_field_data_c* dat = fieldMapDat();
    if (dat == NULL) {
        return 0xffff;
    }
    const char* stage = dMenuFmap_getStartStageName(dat);
    dMenu_Fmap_field_room_data_c* roomData =
        (dMenu_Fmap_field_room_data_c*)((intptr_t)dat + dat->mRoomDataOffset);
    dMenu_Fmap_field_room_data_c::data* rd = roomData->mData;
    for (int i = 0; i < roomData->mCount; i++) {
        int offset = rd->mCount + sizeof(dMenu_Fmap_field_room_data_c::data) - 1;
        if (rd->mCount % 2 == 0) {
            offset += 1;
        }
        if (!strcmp(stage, rd->mStageName)) {
            for (int j = 0; j < rd->mCount; j++) {
                if (stayNo == rd->mRoomNos[j]) {
                    return rd->mAreaName;
                }
            }
        }
        rd = (dMenu_Fmap_field_room_data_c::data*)((intptr_t)rd + offset);
    }
    dMenuMapCommon_c::Stage_c* stageData =
        (dMenuMapCommon_c::Stage_c*)((intptr_t)dat + dat->mStageDataOffset);
    u16 anyRoom = 0xffff;
    for (int i = 0; i < stageData->mCount; i++) {
        const dMenuMapCommon_c::Stage_c::data& sd = stageData->mData[i];
        if (strcmp(stage, sd.mName) != 0) {
            continue;
        }
        if (sd.mRoomNo == stayNo) {
            return sd.mAreaName;
        }
        if (sd.mRoomNo == 0xff && anyRoom == 0xffff) {
            anyRoom = sd.mAreaName;
        }
    }
    return anyRoom;
}

// Current map name ("Hyrule Field"): the map screen's own spot name for the
// stage + room, falling back to the stage title message (dungeons — the same
// id the pause dungeon map shows). Cached per stage + room; the lookup
// retries while stagInfo is NULL during loads.
const char* mapStageName() {
    static char s_name[64];
    static char s_nameStage[8];
    static int s_nameRoom = -100;
    stage_stag_info_class* stagInfo = dComIfGp_getStage()->getStagInfo();
    if (stagInfo == NULL) {
        return s_name;
    }
    const char* stage = dComIfGp_getStartStageName();
    const int stayNo = dComIfGp_roomControl_getStayNo();
    if (strncmp(stage, s_nameStage, sizeof(s_nameStage) - 1) == 0 && stayNo == s_nameRoom) {
        return s_name;
    }
    strncpy(s_nameStage, stage, sizeof(s_nameStage) - 1);
    s_nameStage[sizeof(s_nameStage) - 1] = '\0';
    s_nameRoom = stayNo;
    s_name[0] = '\0';
    u16 msgNo = fmapAreaNameMsg(stayNo);
    if (msgNo == 0xffff) {
        const u16 titleNo = dStage_stagInfo_GetStageTitleNo(stagInfo);
        msgNo = titleNo != 0 ? titleNo : 0xffff;
    }
    if (msgNo != 0xffff) {
        dMeter2Info_getString(msgNo, s_name, NULL);
    }
    return s_name;
}

// Map name plate flush in the map window's top-left corner: dark banner
// with the bottom corners chamfered.
void drawMapNamePlate(f32 x0, f32 y0) {
    const char* name = mapStageName();
    if (name[0] == '\0') {
        return;
    }
    const f32 ts = 12.0f;
    const f32 tw = measureText(ts, name);
    const f32 ph = 22.0f;
    const f32 pad = 9.0f;
    const f32 pw = pad * 2.0f + tw;
    constexpr GXColor plate = {20, 18, 15, 150};
    fillChamferRect(x0, y0, x0 + pw, y0 + ph, 7.0f, plate, 4 | 8);
    drawText(x0 + pad, y0 + ph * 0.5f + 4.5f, ts, TEXT_MAIN, "%s", name);
}

void drawMapContent(f32 x0, f32 y0, f32 x1, f32 y1) {
    // In dungeons the live floor map replaces the minimap crop.
    if (s_dmapAvailable) {
        if (drawDungeonMapContent(x0, y0, x1, y1)) {
            drawDmapFloorTabs(x1, y0);
            drawMapNamePlate(x0, y0);
        } else {
            // First render (palette mount / renderer create) still pending.
            drawTextCentered((x0 + x1) * 0.5f, (y0 + y1) * 0.5f, 15.0f, TEXT_DIM,
                "Loading floor map...");
            s_dmapFloorRectCount = 0;
            s_dmapFloorBtnRect[2] = s_dmapFloorBtnRect[0];  // hidden
            s_mapResetRect[2] = s_mapResetRect[0];          // hidden
            // Nothing consumed the gestures this frame — drop them so they
            // don't burst into the view once it appears.
            s_mapPanX = 0.0f;
            s_mapPanY = 0.0f;
            s_mapPinchDeltaMilli.exchange(0);
        }
        return;
    }
    drawMiniMapContent(x0, y0, x1, y1);
    drawMapNamePlate(x0, y0);
    s_dmapFloorRectCount = 0;
    s_dmapFloorBtnRect[2] = s_dmapFloorBtnRect[0];  // hidden
    s_dmapFloorPickOpen = false;
}


// Item-info reader: the ring menu's own explain text (name = itemNo +
// 0x165, description = itemNo + 0x265 — dMenu_ItemExplain_c's mapping)
// with inline button icons and live capacities resolved. Icon top-right
// inside the box, body drag-scrolls, Back inside bottom-right.
void drawItemInfo(f32 x0, f32 y0, f32 x1, f32 y1) {
    const u8 itemNo = dComIfGs_getItem(s_itemInfoSlot, false);
    if (itemNo == dItemNo_NONE_e) {
        s_itemInfoSlot = -1;
        return;
    }
    static char name[64];
    static int fetchedSlot = -1;
    static u8 fetchedItem = 0xFF;
    static u32 fetchedGen = 0;
    // Refetch when the selection changes OR when someone else (the collect
    // reader) rewrapped the shared body buffer since our fetch.
    if (fetchedSlot != s_itemInfoSlot || fetchedItem != itemNo ||
        fetchedGen != readerBodyGen())
    {
        fetchedSlot = s_itemInfoSlot;
        fetchedItem = itemNo;
        name[0] = 0;
        dMeter2Info_getStringFull(0x165 + itemNo, name, sizeof(name));
        static char body[2048];
        body[0] = 0;
        // X-or-Y tags in the text follow where the item is equipped.
        const int xyBtn = dComIfGp_getSelectItem(1) == itemNo ? 1 : 0;
        dMeter2Info_getStringFull(0x265 + itemNo, body, sizeof(body), xyBtn);
        readerWrapBody(body, x1 - x0 - 100.0f, 14.0f);
        fetchedGen = readerBodyGen();
        readerInvalidate();
        s_scrollItemInfo = 0.0f;
    }
    drawText(x0 + 4.0f, y0 + 16.0f, 16.0f, TEXT_ACCENT, "%s", name);
    const f32 by0 = y0 + 26.0f;
    const f32 by1 = y1 - 2.0f;
    drawMenuBox(x0, by0, x1, by1, 0x22201DFFu);
    drawItemIcon(s_itemInfoSlot, itemNo, x1 - 68.0f, by0 + 12.0f, 52.0f);
    const f32 textBottom = by1 - 44.0f;
    const f32 lineH = 21.0f;
    const f32 viewH = textBottom - by0 - 12.0f;
    const int lineCount = readerBodyLineCount();
    const f32 contentH = (f32)lineCount * lineH;
    const f32 maxScroll = clampListScroll(&s_scrollItemInfo, contentH, viewH);
    if (s_nativeW != 0) {
        GXSetScissorRender((u32)(x0 * s_pixelScale), (u32)((by0 + 6.0f) * s_pixelScale),
            (u32)((x1 - x0) * s_pixelScale) + 1,
            (u32)((textBottom - by0 - 6.0f) * s_pixelScale) + 1);
    }
    for (int i = 0; i < lineCount; i++) {
        const f32 ly = by0 + 22.0f + (f32)i * lineH - s_scrollItemInfo;
        if (ly < by0 - lineH || ly > textBottom + lineH) {
            continue;
        }
        readerDrawBodyLine(i, x0 + 16.0f, ly, 14.0f, TEXT_MAIN);
    }
    if (s_nativeW != 0) {
        GXSetScissorRender(0, 0, s_nativeW, s_nativeH);
    }
    drawListScrollHint(x1 - 4.0f, by0 + 4.0f, textBottom - 4.0f, s_scrollItemInfo, maxScroll,
        viewH, contentH);
    const f32 fy = by1 - 38.0f;
    drawTabPlate(x1 - 100.0f, fy, 90.0f, 30.0f, false);
    drawTextCentered(x1 - 55.0f, fy + 20.0f, 14.0f, TEXT_MAIN, "Back");
    s_itemInfoBtnRect[0] = x1 - 100.0f;
    s_itemInfoBtnRect[1] = fy;
    s_itemInfoBtnRect[2] = x1 - 10.0f;
    s_itemInfoBtnRect[3] = fy + 30.0f;
}

void drawInventoryContent(f32 x0, f32 y0, f32 x1, f32 y1) {
    s_itemInfoBtnRect[0] = 0.0f;
    s_itemInfoBtnRect[2] = 0.0f;
    if (s_itemInfoSlot >= 0) {
        s_invGeomValid = false;
        drawItemInfo(x0, y0, x1, y1);
        return;
    }
    const InvGrid grid = computeInvGrid(x0, y0, x1, y1);
    s_invGeomValid = true;
    s_invCell = grid.cell;
    s_invCellCount = 24;
    for (int i = 0; i < 24; i++) {
        drawInvCell(grid, i);
    }
    drawInventoryCaption(x0, y1);
    // Info button (map-Reset style) once an item is selected.
    if (s_selSlot >= 0 && dComIfGs_getItem(s_selSlot, false) != dItemNo_NONE_e) {
        drawTabPlate(x1 - 70.0f, y1 - 30.0f, 66.0f, 26.0f, true);
        drawTextCentered(x1 - 37.0f, y1 - 12.0f, 13.0f, TEXT_TAB_ACTIVE, "Info");
        s_itemInfoBtnRect[0] = x1 - 70.0f;
        s_itemInfoBtnRect[1] = y1 - 30.0f;
        s_itemInfoBtnRect[2] = x1 - 4.0f;
        s_itemInfoBtnRect[3] = y1 - 4.0f;
    }
}

// QUEST page: Dusklight's achievements, 5 category sub-tabs, drag-scrolled.
void drawQuestContent(f32 x0, f32 y0, f32 x1, f32 y1) {
    const std::vector<dusk::Achievement>& ach = achievementSnapshot();
    const int active = s_questTab.load();
    int unlocked[5] = {};
    int total[5] = {};
    countByCategory(ach, unlocked, total);
    // Tab strip inset from the window edges like the COLLECT sub-tabs.
    const f32 qx0 = x0 + 12.0f;
    const f32 qx1 = x1 - 12.0f;
    const f32 tabW = (qx1 - qx0 - 2.0f * 4.0f) / 5.0f;
    drawQuestTabs(qx0, y0, tabW, active, unlocked, total);

    // Entry table for the active category. Columns: name | progress/status,
    // description word-wrapped beneath. Drag-scrolls (clamped here).
    const f32 listTop = y0 + 48.0f;
    const f32 rowH = 58.0f;
    const f32 colStatus = x1 - 62.0f;
    drawText(x0 + 4.0f, listTop + 12.0f, 12.0f, TEXT_DIM, "Achievement");
    drawText(colStatus, listTop + 12.0f, 12.0f, TEXT_DIM, "Status");
    const f32 tableTop = listTop + 20.0f;
    const f32 listBottom = y1 - 6.0f;
    const f32 viewH = listBottom - tableTop;
    const int catCount = total[active];
    const f32 contentH = (f32)catCount * rowH;
    const f32 maxScroll = clampListScroll(&s_scrollQuest, contentH, viewH);
    constexpr u32 TEXT_DONE = 0xB4D890FF;
    // Rows stop short of the right edge so the scroll hint has its own lane.
    const f32 rx1 = x1 - 12.0f;
    const f32 wrapW = colStatus - x0 - 30.0f;
    if (s_nativeW != 0) {
        GXSetScissorRender((u32)(x0 * s_pixelScale), (u32)(tableTop * s_pixelScale),
            (u32)((x1 - x0) * s_pixelScale) + 1, (u32)(viewH * s_pixelScale) + 1);
    }
    int idx = 0;
    for (const dusk::Achievement& a : ach) {
        if ((int)a.category != active) {
            continue;
        }
        const f32 ry = tableTop + (f32)idx * rowH - s_scrollQuest;
        idx++;
        if (ry < tableTop - rowH || ry > listBottom) {
            continue;
        }
        // Boxed rows like the skills/mail lists.
        drawMenuBox(x0, ry + 2.0f, rx1, ry + rowH - 4.0f, 0x22201DFFu);
        drawText(x0 + 14.0f, ry + 19.0f, 14.0f, a.unlocked ? TEXT_DONE : TEXT_MAIN,
            "%s", a.name);
        if (a.unlocked) {
            drawText(colStatus, ry + 19.0f, 13.0f, TEXT_DONE, "Done");
        } else if (a.isCounter) {
            drawText(colStatus, ry + 19.0f, 13.0f, TEXT_DIM, "%d/%d", a.progress, a.goal);
        } else {
            drawText(colStatus, ry + 19.0f, 13.0f, TEXT_DIM, "-");
        }
        drawWrappedDescription(a.description, x0 + 20.0f, ry + 34.0f, wrapW);
    }
    if (s_nativeW != 0) {
        GXSetScissorRender(0, 0, s_nativeW, s_nativeH);
    }
    drawListScrollHint(x1, tableTop, listBottom, s_scrollQuest, maxScroll, viewH, contentH);
}

}  // namespace dusk::companion
