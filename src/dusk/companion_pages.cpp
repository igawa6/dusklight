// Companion dashboard: MAP / ITEMS page content. The COLLECT page
// lives in companion_collect.cpp.

#include "dusk/companion.h"
#include "dusk/companion_internal.h"
#include "dusk/companion_strings.h"
#include "dusk/achievements.h"
#include "dusk/dualscreen.h"
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
// Frames the stage has been fully up with no minimap texture. Past the
// grace period the stage is treated as having no map at all (boss arenas)
// rather than one still loading. ~1.5-3s depending on frame rate.
constexpr int NO_MAP_GRACE_FRAMES = 90;
int s_mapAbsentFrames;

// Dungeon-map blit cache (separate: the dmap target outlives page switches
// but is destroyed on stage change / dungeon exit). s_dmapSeenGen tracks
// s_dmapGen: on renderer teardown the ResTIMG pointer may be reused by the
// next dungeon, so pointer-keyed caching alone would blit a stale texture.
J2DPicture* s_dmapPic;
const ResTIMG* s_lastDmapTimg;
u32 s_dmapSeenGen = 0;

// Floor-name cache: the menu's floor message ids by (floorNo + 5); 0 means
// no string — fall back to a generated B#/#F label.
constexpr u16 l_floorMsg[DMAP_FLOOR_COUNT] = {
    0, 0, 0x03DB, 0x03DA, 0x036B, 0x036C, 0x036D, 0x036E, 0x036F, 0x03DC, 0x03DD,
    0x03D9, 0x03D8,
};
char s_floorName[DMAP_FLOOR_COUNT][28];
bool s_floorNameOk[DMAP_FLOOR_COUNT];
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
    if (idx < 0 || idx >= DMAP_FLOOR_COUNT) {
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

// Copy a BTI out of a boot-resident archive into companion-owned storage.
// NEVER cache a raw pointer into these archives' loaded-resource memory:
// the game evicts it wholesale — ~dMenu_DmapBg_c runs
// dComIfGp_getDmapResArchive()->removeResourceAll() every time the dungeon
// map screen closes (d_menu_dmap.cpp:674), and game-over/file-select do the
// same to Main2D — so a pointer cached across frames dangles and the next
// companion draw uploads freed memory. That was the Android open/close-
// dungeon-map crash (XXH64/memcpy SIGSEGVs, "unknown texture format 255",
// vkAllocateMemory device-lost: one stale descriptor, four symptoms).
// Returns NULL and never retries if the file is missing or over the cap
// (the icon is simply not drawn); retries while the archive isn't mounted.
constexpr u32 DMAP_ICON_BUF_BYTES = 0x1800;
static const ResTIMG* copyTimgOwned(JKRArchive* arc, const char* name, u8* buf, u8* io_state) {
    constexpr u8 STATE_FAILED = 2;
    if (*io_state == STATE_FAILED || arc == NULL) {
        return NULL;
    }
    const void* res = arc->getResource('TIMG', name);
    if (res == NULL) {
        *io_state = STATE_FAILED;
        return NULL;
    }
    const u32 size = arc->getExpandedResSize(res);
    if (size == 0 || size > DMAP_ICON_BUF_BYTES) {
        *io_state = STATE_FAILED;
        return NULL;
    }
    memcpy(buf, res, size);
    return (const ResTIMG*)buf;
}

// The pause map's link-arrow icon from the boot-resident dmap layout archive.
const ResTIMG* dmapLinkIconTimg() {
    static const ResTIMG* timg;
    static u8 state;
    if (timg == NULL) {
        alignas(32) static u8 buf[DMAP_ICON_BUF_BYTES];
        timg = copyTimgOwned(dComIfGp_getDmapResArchive(),
            "tt_map_icon_link_ci8_32_00.bti", buf, &state);
    }
    return timg;
}

namespace {

// Overlay icon textures (the menu's full-size variants), same resident
// archive, each copied into owned storage (see copyTimgOwned).
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
    constexpr int TEX_COUNT = (int)(sizeof(l_tex) / sizeof(l_tex[0]));
    static const ResTIMG* cache[ICON_MAX_e];
    static u8 state[ICON_MAX_e];
    alignas(32) static u8 bufs[TEX_COUNT][DMAP_ICON_BUF_BYTES];
    if (icon >= ICON_MAX_e) {
        return NULL;
    }
    if (cache[icon] == NULL) {
        JKRArchive* arc = dComIfGp_getDmapResArchive();
        for (int i = 0; i < TEX_COUNT; i++) {
            if (l_tex[i].icon == icon) {
                cache[icon] = copyTimgOwned(arc, l_tex[i].name, bufs[i], &state[icon]);
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
// i_size scales both markers: the pop-up rows keep the compact default, the
// (taller) context tab passes a larger one.
void drawFloorPlateMarks(f32 tx, f32 ty, f32 tw, f32 th, int floorNo, int playerFloor,
    f32 i_size = 16.0f) {
    if (playerFloor != DMAP_FLOOR_FOLLOW && floorNo == playerFloor) {
        drawTimg(dmapFloorFaceTimg(), tx + 2.0f, ty + th * 0.5f - i_size * 0.5f, i_size, i_size,
            0xFF);
    }
    if (s_dmapBossFloor != DMAP_FLOOR_FOLLOW && floorNo == s_dmapBossFloor) {
        const ResTIMG* boss = dmapFloorBossMarkTimg();
        if (boss == NULL) {
            boss = dmapIconTimg(ICON_BOSS_e);
        }
        const f32 bs = i_size * 0.875f;
        drawTimg(boss, tx + tw - bs - 2.0f, ty + th * 0.5f - bs * 0.5f, bs, bs, 0xFF);
    }
}


// Hollow version of the player arrow, for showing Link's position on a floor
// he is not standing on.
//
// Stroked by hand rather than drawn from the icon: dmapLinkIconTimg is a
// filled texture, and there is no way to punch a hole in it — an alpha or
// tint pass only makes the whole triangle fainter, which reads as "far away"
// instead of "different floor". Three quads along the edges of the same
// isoceles triangle the icon uses, rotated to match.
void strokeArrow(f32 cx, f32 cy, f32 size, f32 angleDeg, f32 thickness, GXColor color) {
    const f32 h = size * 0.5f;
    // Tip, back-left, back-right — the icon's own proportions.
    //
    // The tip is at +h (DOWN in canvas coords), not -h: dmapLinkIconTimg is
    // authored pointing down at rotation 0, so a triangle built pointing up
    // came out exactly 180 degrees from the filled icon. Measured, not
    // guessed — drawing both at one centre and one angle put their tips
    // 177-179 degrees apart at every angle tried, which is the signature of a
    // constant flip rather than a rotation-sign error (that would scale with
    // the angle).
    const f32 local[3][2] = {{0.0f, h}, {-h * 0.62f, -h * 0.72f}, {h * 0.62f, -h * 0.72f}};
    // NEGATED: J2DPane::makeMatrix builds MTXRotRad(..., -mRotateZ), so the
    // solid icon path rotates by R(-angle). Both branches are handed the same
    // cM_sht2d(rotY), so matching that sign is what keeps the hollow arrow and
    // the filled one pointing the same way — it was mirrored, exact at 0/180
    // and backwards at +/-90.
    const f32 rad = -angleDeg * 3.14159265f / 180.0f;
    const f32 cs = cosf(rad);
    const f32 sn = sinf(rad);
    f32 pts[3][2];
    for (int i = 0; i < 3; i++) {
        pts[i][0] = cx + local[i][0] * cs - local[i][1] * sn;
        pts[i][1] = cy + local[i][0] * sn + local[i][1] * cs;
    }
    for (int i = 0; i < 3; i++) {
        const f32 ax = pts[i][0], ay = pts[i][1];
        const f32 bx2 = pts[(i + 1) % 3][0], by2 = pts[(i + 1) % 3][1];
        f32 dx = bx2 - ax, dy = by2 - ay;
        const f32 len = sqrtf(dx * dx + dy * dy);
        if (len < 0.001f) {
            continue;
        }
        // Unit normal, scaled to half the stroke width.
        const f32 nx = -dy / len * thickness * 0.5f;
        const f32 ny = dx / len * thickness * 0.5f;
        const f32 quad[8] = {ax + nx, ay + ny, bx2 + nx, by2 + ny,
                             bx2 - nx, by2 - ny, ax - nx, ay - ny};
        fillPolyPublic(quad, 4, color);
    }
}

// Whole-floor dungeon view: contain-fit blit of the companion's live
// dungeon-map render (companion_dmap.cpp) plus gestures, overlay icons and
// the realtime player arrow. Returns false when the view is not available
// so the caller falls through to the minimap.
// Reset-view button, bottom right inside the map window. Shared by both map
// modes, which also share the tap rect it publishes.
void drawMapResetButton(f32 x1, f32 y1, bool viewMoved) {
    drawTabPlate(x1 - 70.0f, y1 - 30.0f, 66.0f, 26.0f, viewMoved);
    drawTextFittedCentered(x1 - 37.0f, y1 - 12.0f, 13.0f, 8.0f, 60.0f,
        viewMoved ? TEXT_TAB_ACTIVE : TEXT_DIM, txt(STR_RESET));
    s_mapResetRect[0] = x1 - 70.0f;
    s_mapResetRect[1] = y1 - 30.0f;
    s_mapResetRect[2] = x1 - 4.0f;
    s_mapResetRect[3] = y1 - 4.0f;
}

// Steer the live map render with the finger drag, then keep the shifted
// render-window center inside the stage's map-path bounds.
void miniMapApplyPan(dMap_c* map, f32 drawW, f32 drawH) {
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
}

// Mirror mode flips the map's X axis along with the world.
f32 mapXSign() {
    return getSettings().game.enableMirrorMode ? -1.0f : 1.0f;
}

// Apply this frame's pinch/drag to the dungeon render view. Consumes the
// pending gesture deltas; dmapUpdate picks the result up next frame.
void dmapApplyGestures(f32 drawW) {
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
    if ((s_mapPanX != 0.0f || s_mapPanY != 0.0f) && s_dmapCmPerTexel > 0.0f && drawW > 0.0f) {
        const f32 worldPerPx = s_dmapCmPerTexel * (f32)DMAP_TEX_SIZE / drawW;
        s_dmapOffX -= s_mapPanX * worldPerPx * mapXSign();
        s_dmapOffZ -= s_mapPanY * worldPerPx;
        // Manual drag = free look: stop following the stay room.
        s_dmapFollow = false;
    }
    s_mapPanX = 0.0f;
    s_mapPanY = 0.0f;
}


// Floor change: a quick fade-through-dark over the map instead of the
// texture hard-swapping under the viewer.
void dmapFloorFadeOverlay(f32 bx, f32 by, f32 drawW, f32 drawH, int playerFloor) {
    // Floor change: a quick fade-through-dark over the map instead of the
    // texture hard-swapping under the viewer (double-texture crossfades
    // aren't worth a second render target here).
    {
        static int sPrevShownFloor = -99;
        static f32 sFloorFade = 0.0f;
        const int shown = s_dmapFloorSel == DMAP_FLOOR_FOLLOW ? playerFloor : s_dmapFloorSel;
        if (shown != sPrevShownFloor) {
            if (sPrevShownFloor != -99) {
                sFloorFade = 1.0f;
            }
            sPrevShownFloor = shown;
        }
        if (sFloorFade > 0.0f) {
            sFloorFade *= ANIM_DECAY_SOFT;
            if (sFloorFade < ANIM_ZERO) {
                sFloorFade = 0.0f;
            } else {
                fillRect(bx, by, bx + drawW, by + drawH,
                    {0, 0, 0, (u8)(230.0f * sFloorFade)});
            }
        }
    }
}


// Floor icons and the player arrow, drawn over the rendered floor in the
// same affine the render used.
void dmapDrawOverlays(f32 bx, f32 by, f32 drawW, f32 drawH, int playerFloor) {
    // World -> canvas mapping for the overlays (same affine the render used).
    const f32 pxPerTexel = drawW / (f32)DMAP_TEX_SIZE;
    const f32 mapCx = bx + drawW * 0.5f;
    const f32 mapCy = by + drawH * 0.5f;
    // A marker whose position falls outside the rendered floor is simply not
    // drawn. (The alternative, edge-clamping it to the border the way the
    // pause map does, was tried and rejected: pinned markers claim positions
    // that are not theirs.) Partially-overlapping icons are left to the
    // scissor above, which crops them where the map genuinely ends.
    const auto onMap = [&](f32 px, f32 py) {
        return px >= bx && px <= bx + drawW && py >= by && py <= by + drawH;
    };
    if (s_dmapCmPerTexel > 0.0f) {
        // Floor icons (chests, boss, entrance, warps, ...), gathered with
        // the pause map's visibility policy.
        for (int i = 0; i < s_dmapIconCount; i++) {
            const DmapIcon& ic = s_dmapIcons[i];
            const f32 ix = mapCx + (ic.x - s_dmapViewCx) * mapXSign() / s_dmapCmPerTexel * pxPerTexel;
            const f32 iy = mapCy + (ic.z - s_dmapViewCz) / s_dmapCmPerTexel * pxPerTexel;
            if (!onMap(ix, iy)) {
                continue;
            }
            const ResTIMG* it = dmapIconTimg(ic.icon);
            const f32 sz = 24.0f;
            if (ic.icon == ICON_LIGHT_DROP_e) {
                // Tear of light: intensity texture, the menu's green tint.
                drawTimgTinted(it, ix - sz * 0.5f, iy - sz * 0.5f, sz, sz, 0xFF,
                    0x00F0AA00u, 0xFFFFE6FFu);
            } else if (ic.rot != 0) {
                const s16 rot = mapXSign() < 0.0f ? (s16)-ic.rot : ic.rot;
                drawTimgRotated(it, ix, iy, sz, cM_sht2d((f32)rot), 0xFF);
            } else {
                drawTimg(it, ix - sz * 0.5f, iy - sz * 0.5f, sz, sz, 0xFF);
            }
        }
        // Realtime player arrow (the render itself carries no cursor — the
        // pause map draws it as an overlay too).
        //
        // Drawn on EVERY floor, not just the one Link is on: previewing 3F
        // while standing on 1F, his X/Z still says which part of the dungeon
        // you are looking at relative to yourself. On another floor it is an
        // OUTLINE rather than the solid icon, so it reads as "he is here, but
        // not on this floor" and can never be mistaken for his real position
        // on the floor being shown.
        if (playerFloor != DMAP_FLOOR_FOLLOW) {
            const Vec pos = dMapInfo_n::getMapPlayerPos();
            s16 rotY = dMapInfo_n::getMapPlayerAngleY();
            if (mapXSign() < 0.0f) {
                rotY = -rotY;
            }
            const f32 ax = mapCx + (pos.x - s_dmapViewCx) * mapXSign() / s_dmapCmPerTexel * pxPerTexel;
            const f32 ay = mapCy + (pos.z - s_dmapViewCz) / s_dmapCmPerTexel * pxPerTexel;
            // Panned off Link, it disappears rather than sitting on the border
            // pretending he is there.
            if (onMap(ax, ay)) {
                if (playerFloor == s_dmapViewFloor) {
                    drawTimgRotated(dmapLinkIconTimg(), ax, ay, 30.0f, cM_sht2d((f32)rotY), 0xFF);
                } else {
                    constexpr GXColor COL_ARROW_GHOST = {236, 214, 96, 235};
                    strokeArrow(ax, ay, 30.0f, cM_sht2d((f32)rotY), 2.0f, COL_ARROW_GHOST);
                }
            }
        }
    }
}


bool drawDungeonMapContent(f32 x0, f32 y0, f32 x1, f32 y1) {
    if (s_dmapSeenGen != s_dmapGen) {
        s_dmapSeenGen = s_dmapGen;
        s_lastDmapTimg = NULL;
    }
    const ResTIMG* timg = dmapTimg();
    if (!s_dmapReady || !isSaneTimg(timg)) {
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
    // Functional: top-aligned, not centred — with the name plate gone the map
    // is the only thing in the window, and centring a wide floor left dead
    // space along the top edge. Cinematic still draws the name plate at the
    // window's top-left, so it keeps the centred layout the plate was
    // designed over.
    const f32 by = dusk::dualscreen::mainHudRestored()
        ? y0 : y0 + (availH - drawH) * 0.5f;

    dmapApplyGestures(drawW);

    // Scissor the MAP RECT, not the whole available area. The texture is
    // square and contain-fitted, so it occupies a square inset inside a
    // non-square window: clipping to the full area let the overlays below
    // draw in the empty band beside/under the map, which looked like markers
    // floating outside it. Both the texture and its overlays now share one
    // clip, so nothing can render off the map.
    setWinScissor(bx, by, bx + drawW, by + drawH);
    s_dmapPic->setAlpha(mulDrawAlpha(0xFF));
    s_dmapPic->draw(bx, by, drawW, drawH, false, false, false);
    s_dmapPic->setAlpha(0xFF);
    dmapFloorFadeOverlay(bx, by, drawW, drawH, playerFloor);

    dmapDrawOverlays(bx, by, drawW, drawH, playerFloor);
    applyWinClip();
    dComIfGp_getCurrentGrafPort()->setup2D();

    // Reset-view button, bottom right (shared rect with the minimap mode).
    const bool viewMoved = !s_dmapFollow || s_dmapFloorSel != DMAP_FLOOR_FOLLOW;
    drawMapResetButton(x1, y1, viewMoved);
    return true;
}

// Warp button, bottom-left inside the map window — mirrors the Reset button
// opposite it. Hidden until portal warping is unlocked; dimmed (but still
// tappable) when the player state forbids it, exactly like the game's own
// warp button on its map screen. A tap while dimmed is refused by the press
// handler, which answers with the error cue.
// The message system's portal glyph (MSGTAG_WARP_ICON), copied out of the
// Main2D archive (game-over/file-select evict its loaded resources — same
// dangling-pointer hazard as the dmap icons, see copyTimgOwned). Falls back
// to the dungeon map's warp marker.
const ResTIMG* warpIconTimg() {
    static const ResTIMG* timg;
    static u8 state;
    if (timg == NULL) {
        alignas(32) static u8 buf[DMAP_ICON_BUF_BYTES];
        timg = copyTimgOwned(dComIfGp_getMain2DArchive(),
            "im_map_icon_portal_4ia_40_05.bti", buf, &state);
        if (timg == NULL) {
            timg = dmapIconTimg(ICON_LV8_WARP_e);
        }
    }
    return timg;
}

// ITEMS grid: two-column box grouping —
//   left  : Tools 5x2, then Bombs 3x1 + Rod/Slingshot 2x1
//   right : Bottles 2x2 above Quest 2x2
// gx 0-4 = left block, gx 5-6 = right block (column gap applied).
struct CellDef {
    int slot;
    u8 gx, gy, group;
};
// 23, not 24: the table was declared [24] with 23 initializers, so the last
// entry was value-initialized to {slot 0, gx 0, gy 0} and redrew the first
// cell on top of itself every frame — invisible, but it also published a
// duplicate hit rect for slot 0 into s_invCells.
constexpr int INV_CELLS = 23;

constexpr CellDef l_invCells[INV_CELLS] = {
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

// 16:9 and wider: 6 columns x 4 rows. Every group stays a solid rectangle —
// the extra column is paid for by the bottles, which leave their 2x2 block
// and become the full-height right-hand strip. Losing a row is what makes the
// cells bigger; six narrower columns alone would have made them smaller.
//   gx 0-4, gy 0-1 : tools        gx 5, gy 0-3 : bottles
//   gx 0-2, gy 2   : bombs        gx 3-4, gy 2 : rod / slingshot
//   gx 0-3, gy 3   : quest        gx 4,  gy 3  : empty
constexpr CellDef l_invCellsWide[INV_CELLS] = {
    {0, 0, 0, 0}, {1, 1, 0, 0}, {2, 2, 0, 0}, {3, 3, 0, 0}, {4, 4, 0, 0},
    {5, 0, 1, 0}, {6, 1, 1, 0}, {8, 2, 1, 0}, {9, 3, 1, 0}, {10, 4, 1, 0},
    {15, 0, 2, 2}, {16, 1, 2, 2}, {17, 2, 2, 2},
    {20, 3, 2, 4}, {23, 4, 2, 4},
    {18, 0, 3, 3}, {19, 1, 3, 3}, {21, 2, 3, 3}, {22, 3, 3, 3},
    {11, 5, 0, 1}, {12, 5, 1, 1}, {13, 5, 2, 1}, {14, 5, 3, 1},
};
// The two layouts must stay interchangeable. They are separate tables, so a
// later edit to one could quietly drop an item, duplicate a cell or run off the
// grid in that layout only — and it would look like a missing item in the game,
// not a wrong number in a table. Checked here instead, at compile time.
constexpr bool inv_tables_hold_same_slots() {
    for (const CellDef& a : l_invCells) {
        bool found = false;
        for (const CellDef& b : l_invCellsWide) {
            if (a.slot == b.slot) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

constexpr bool inv_table_is_sane(const CellDef (&cells)[INV_CELLS], u8 cols, u8 rows) {
    for (std::size_t i = 0; i < INV_CELLS; i++) {
        if (cells[i].gx >= cols || cells[i].gy >= rows) {
            return false;  // outside the grid it is drawn into
        }
        for (std::size_t j = i + 1; j < INV_CELLS; j++) {
            if (cells[i].gx == cells[j].gx && cells[i].gy == cells[j].gy) {
                return false;  // two items stacked in one cell
            }
            if (cells[i].slot == cells[j].slot) {
                return false;  // one item drawn twice
            }
        }
    }
    return true;
}

static_assert(inv_tables_hold_same_slots(), "ITEMS layouts disagree on which slots exist");
static_assert(inv_table_is_sane(l_invCells, 5, 5), "5x5 ITEMS table is malformed");
static_assert(inv_table_is_sane(l_invCellsWide, 6, 4), "6x4 ITEMS table is malformed");

const GXColor l_groupTint[5] = {
    {45, 41, 33, 255},   // tools (stone brown)
    {38, 44, 40, 255},   // bottles (moss green)
    {54, 36, 30, 255},   // bombs (rust red)
    {52, 44, 28, 255},   // quest (gold-brown)
    {42, 42, 34, 255},   // rod/slingshot (olive grey)
};

constexpr f32 INV_CAPTION_H = 26.0f;
constexpr f32 INV_GROUP_GAP = 8.0f;

// Grid metrics for the ITEMS page, fit to the content box. `wide` is carried
// here rather than re-tested per cell so the geometry and the cell table can
// never disagree if the canvas is resized mid-frame.
struct InvGrid {
    f32 cell, originX, originY, pad, icon;
    bool wide;
};

// The companion is 8:7 on a bottom panel but takes the main screen's shape
// when the screens are swapped. On a 16:9 canvas the 5x5 grid is limited by
// HEIGHT, so it sat small and centred with a third of the width unused; 6x4
// spends that width on a shorter grid with bigger cells.
bool invWideLayout() {
    return s_canvasH > 0.0f && s_canvasW / s_canvasH >= 1.7f;
}

InvGrid computeInvGrid(f32 x0, f32 y0, f32 x1, f32 y1) {
    const bool wide = invWideLayout();
    const f32 cols = wide ? 6.0f : 5.0f;
    const f32 rows = wide ? 4.0f : 5.0f;
    const f32 availW = x1 - x0;
    const f32 availH = y1 - y0 - INV_CAPTION_H;
    f32 cell = availW / cols;
    if (cell * rows + INV_GROUP_GAP * 2.0f > availH) {
        cell = (availH - INV_GROUP_GAP * 2.0f) / rows;
    }
    const f32 gridW = cell * cols;
    const f32 gridH = cell * rows + INV_GROUP_GAP * 2.0f;
    InvGrid g;
    g.wide = wide;
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
    const CellDef& d = (g.wide ? l_invCellsWide : l_invCells)[i];
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
// (The s_equipMsgFrames countdown lives in drawDashboard, once per frame.)
void drawInventoryCaption(f32 x0, f32 x1, f32 y1) {
    u8 nameItem = dItemNo_NONE_e;
    if (s_dragging && s_dragSlot >= 0) {
        nameItem = dComIfGs_getItem(s_dragSlot, false);
    } else if (s_selSlot >= 0) {
        nameItem = dComIfGs_getItem(s_selSlot, false);
    }
    // All caption variants share one 13px baseline, sitting low in the
    // window's bottom margin so they read clear of the grid.
    const f32 base = y1 - 5.0f;
    if (s_equipMsgFrames > 0) {
        constexpr u32 TEXT_WARN = 0xF0A050FF;
        drawText(x0 + 18.0f, base, 13.0f, TEXT_WARN, "%s", s_equipMsg);
    } else if (nameItem != dItemNo_NONE_e) {
        static u8 s_nameItemNo = dItemNo_NONE_e;
        static char s_nameBuf[96];
        if (s_nameItemNo != nameItem) {
            s_nameItemNo = nameItem;
            s_nameBuf[0] = 0;
            dMeter2Info_getString(0x165 + nameItem, s_nameBuf, NULL);
        }
        if (s_nameBuf[0] != 0) {
            drawText(x0 + 18.0f, base, 13.0f, TEXT_MAIN, "%s", s_nameBuf);
        }
    } else {
        // maxW keeps the German caption inside the window rather than
        // running under the slot boxes on the right.
        drawTextEllipsized(x0 + 18.0f, base, 13.0f, (x1 - 24.0f) - (x0 + 18.0f), TEXT_DIM,
            txt(STR_DRAG_EQUIP));
    }
}






}  // namespace

// Warp content (portal glyph + "Warp") for the Functional context tab; the
// plate is drawn by the caller. While the game's field map is up the tab acts
// as its Z toggle, so the icon dims to show the portals are already on.
void drawWarpTab(f32 x0, f32 y0, f32 x1, f32 y1, bool active) {
    // Nothing to offer until Midna grants warping: leave the plate EMPTY
    // instead of advertising a control that cannot do anything yet. The
    // caller has already drawn the plate, so the slot keeps its place and the
    // layout never shifts — the transform button behaves the same way before
    // the shadow crystal.
    if (!warpUnlocked()) {
        return;
    }
    const f32 tw = x1 - x0;
    const f32 th = y1 - y0;
    const bool portalsShown = isFieldMapScreen() && warpPortalsShown();
    const u8 iconA = active && !portalsShown ? 0xFF : 130;
    // Icon left, label right, the group centred on one baseline — larger than
    // the old stacked layout.
    constexpr f32 ICON = 28.0f;
    constexpr f32 TS = 15.0f;
    constexpr f32 GAP = 5.0f;
    // The plate is a fixed width, so a longer localized word (ES) has to
    // shrink rather than overflow the group.
    // NOT taken from the archive: 0x529 is a verb phrase in several languages
    // ("Mostrar portales"), far too long for this compact plate.
    const char* const label = txt(STR_WARP);
    const f32 labelMax = tw - ICON - GAP - 16.0f;
    const f32 labelTS = fittedTextSize(TS, 8.0f, labelMax, label);
    const f32 groupW = ICON + GAP + measureText(labelTS, label);
    const f32 gx = x0 + (tw - groupW) * 0.5f;
    const f32 cy = y0 + th * 0.5f;
    drawTimg(warpIconTimg(), gx, cy - ICON * 0.5f, ICON, ICON, iconA);
    drawText(gx + ICON + GAP, cy + 5.0f, labelTS, active ? TEXT_TAB_ACTIVE : TEXT_DIM, "%s",
        label);
}

// Floor button content (viewed-floor name + the pause map's link/boss
// markers) for the Functional context tab; the tab plate itself is drawn by
// the caller so its selected/unselected styling stays uniform with the other
// context actions.
void drawFloorTab(f32 x0, f32 y0, f32 x1, f32 y1, bool active) {
    const int playerFloor = dMapInfo_c::getNowStayFloorNoDecisionFlg()
        ? dMapInfo_c::getNowStayFloorNo() : DMAP_FLOOR_FOLLOW;
    const f32 tw = x1 - x0;
    const f32 th = y1 - y0;
    drawTextCentered(x0 + tw * 0.5f, y0 + th * 0.5f + 6.0f, 15.0f,
        active ? TEXT_TAB_ACTIVE : TEXT_DIM, dmapFloorName(s_dmapViewFloor));
    drawFloorPlateMarks(x0, y0, tw, th, s_dmapViewFloor, playerFloor, 26.0f);
}

// Floor-select pop-up as a rightward overlay panel: a vertical list of floor
// plates whose top-left starts at (px, py) — the right edge of the context
// tab. Drawn after the content window so it sits on top. Empty floors are
// dimmed and not tappable; the rest publish their tap rects.
// Number of selectable floors in this dungeon (capped at the rect budget).
// One floor means there is nothing to pick, so the picker never opens.
int dmapFloorCount() {
    s8 top = 0;
    s8 bottom = 0;
    dMpath_c::getTopBottomFloorNo(&top, &bottom);
    int count = top - bottom + 1;
    if (count < 1) {
        count = 1;
    } else if (count > DMAP_FLOOR_COUNT) {
        count = DMAP_FLOOR_COUNT;
    }
    return count;
}

// One floor row, shared by the Functional column and the Cinematic pop-up.
// The two differ only in geometry and trim, so the row's LOGIC — which floor
// is being viewed, which floors have been mapped, where Link is, what is
// tappable — lives here once. It was written out in both places before, and
// the copies had already drifted apart on chamfer, text size and baseline.
struct FloorRowStyle {
    f32 chamfer;
    int cornerMask;
    f32 textSize;
    f32 textBaseline;  // from the row's vertical centre
    f32 markSize;
    bool frameViewed;  // outline the viewed row, or leave it bare like the tab
};

void drawFloorRow(f32 x0, f32 y0, f32 x1, f32 y1, int floorNo, int playerFloor,
    const FloorRowStyle& st, bool publishTaps) {
    constexpr GXColor COL_SCRIM = {18, 17, 14, 122};
    constexpr GXColor COL_HERE = {233, 206, 142, 255};
    constexpr u32 TEXT_LOCKED = 0x6E685AFFu;
    const bool viewed = floorNo == s_dmapViewFloor;
    const int bit = floorNo + 5;
    const bool hasContent = bit >= 0 && bit < DMAP_FLOOR_COUNT &&
        (s_dmapFloorAvail & (1u << bit)) != 0;
    const bool here = playerFloor != DMAP_FLOOR_FOLLOW && floorNo == playerFloor;

    drawChamferPlate(x0, y0, x1, y1, st.chamfer, viewed, st.cornerMask);
    // Gold outline marks the floor Link is ON, a different question from which
    // floor is being SHOWN (that is the bright plate).
    if (!viewed || st.frameViewed) {
        drawChamferFrame(x0, y0, x1, y1, st.chamfer, 1.5f, here ? COL_HERE : COL_FRAME,
            st.cornerMask);
    }
    drawTextCentered((x0 + x1) * 0.5f, (y0 + y1) * 0.5f + st.textBaseline, st.textSize,
        viewed ? TEXT_TAB_ACTIVE : (hasContent ? TEXT_DIM : TEXT_LOCKED),
        dmapFloorName(floorNo));
    drawFloorPlateMarks(x0, y0, x1 - x0, y1 - y0, floorNo, playerFloor, st.markSize);
    if (!hasContent && !viewed) {
        fillRect(x0, y0, x1, y1, COL_SCRIM);
        return;  // unmapped floors are shown but not selectable
    }
    if (publishTaps && s_dmapFloorRectCount < DMAP_FLOOR_COUNT) {
        s_dmapFloorRects[s_dmapFloorRectCount][0] = x0;
        s_dmapFloorRects[s_dmapFloorRectCount][1] = y0;
        s_dmapFloorRects[s_dmapFloorRectCount][2] = x1;
        s_dmapFloorRects[s_dmapFloorRectCount][3] = y1;
        s_dmapFloorVals[s_dmapFloorRectCount] = floorNo;
        s_dmapFloorRectCount++;
    }
}

// Floor picker for the Functional layout: the context tab EXPANDS IN PLACE in
// the left column, growing up and down from the current-floor button, over one
// continuous bed. Previously a separate panel slid out to the right, over the
// map — which covered the thing you were choosing a floor for.
//
// Rows are the tab's own height and width, so the button the player tapped
// stays exactly where it was and simply gains neighbours.
void drawFloorColumn(f32 tx0, f32 ty0, f32 tx1, f32 ty1, f32 clampY0, f32 clampY1,
    f32 canvasH) {
    s_dmapFloorRectCount = 0;
    const int count = dmapFloorCount();
    if (count <= 1) {
        // Nothing to choose between; never leave it stuck open.
        s_dmapFloorPickOpen = false;
        s_dmapFloorPickT = 0.0f;
        return;
    }
    // Open/close ease. Opening is a touch snappier than closing so the list
    // feels like it springs out and settles back.
    if (s_dmapFloorPickOpen) {
        s_dmapFloorPickT += (1.0f - s_dmapFloorPickT) * ANIM_RATE_FAST;
        if (s_dmapFloorPickT > ANIM_DONE) {
            s_dmapFloorPickT = 1.0f;
        }
    } else {
        s_dmapFloorPickT *= ANIM_DECAY_FAST;
        if (s_dmapFloorPickT < ANIM_ZERO) {
            s_dmapFloorPickT = 0.0f;
        }
    }
    if (s_dmapFloorPickT <= 0.0f) {
        return;
    }
    const f32 openT = s_dmapFloorPickT;
    const int playerFloor = dMapInfo_c::getNowStayFloorNoDecisionFlg()
        ? dMapInfo_c::getNowStayFloorNo() : DMAP_FLOOR_FOLLOW;
    s8 top = 0;
    s8 bottom = 0;
    dMpath_c::getTopBottomFloorNo(&top, &bottom);

    const f32 rowH = ty1 - ty0;
    const f32 gap = 2.0f;
    const f32 pad = 4.0f;
    const f32 listH = (f32)count * rowH + (f32)(count - 1) * gap;
    // Anchor the VIEWED floor on the tab so the list opens symmetrically
    // around the button that was tapped.
    int sel = 0;
    for (int i = 0; i < count; i++) {
        if (top - i == s_dmapViewFloor) {
            sel = i;
            break;
        }
    }
    f32 listTop = ty0 - (f32)sel * (rowH + gap);
    if (clampY1 > clampY0) {
        if (listTop + listH + pad > clampY1) {
            listTop = clampY1 - listH - pad;
        }
        if (listTop - pad < clampY0) {
            listTop = clampY0 + pad;
        }
    }

    constexpr GXColor COL_TAB_SCRIM = {18, 17, 14, 122};
    constexpr GXColor COL_BED = {24, 24, 22, 248};
    constexpr GXColor COL_FRAME = {108, 102, 90, 255};
    constexpr GXColor COL_HERE = {233, 206, 142, 255};
    constexpr u32 TEXT_LOCKED = 0x6E685AFFu;
    // The bed grows with the list — this is the "bleed" behind the column,
    // chamfered on the left like the panels it sits among.
    // Cover the ENTIRE left column while the picker is up: the panels behind
    // it (Link doll, rupees, clock, dungeon keys) are neither readable nor
    // tappable during a pick, so showing them half-buried under the bed just
    // looked broken. Same vertical gradient as a corner button's face, so the
    // column reads as one inert slab rather than a scrim over live chrome.
    // Fades in with the list; touch is blocked for the same span.
    {
        constexpr GXColor COVER_TOP = {52, 50, 46, 255};
        constexpr GXColor COVER_BOT = {31, 30, 27, 255};
        const f32 prevA = s_drawAlpha;
        s_drawAlpha = prevA * openT;
        // Starts below the top bar: that strip carries the clock/battery and
        // stays legible and live while a floor is being picked.
        fillChamferVGrad(0.0f, FN_TOPBAR_H, tx1, canvasH, 0.0f, COVER_TOP, COVER_BOT, 0);
        s_drawAlpha = prevA;
    }

    // GROW: everything is interpolated from the tab's own rect (collapsed) out
    // to the full list, so the bed and the rows expand up and down together
    // from the button that was tapped.
    const f32 bedY0 = ty0 - pad + ((listTop - pad) - (ty0 - pad)) * openT;
    const f32 bedY1 = ty0 + rowH + pad +
        ((listTop + listH + pad) - (ty0 + rowH + pad)) * openT;
    fillChamferRect(tx0, bedY0, tx1, bedY1, 10.0f, COL_BED, 1 | 8);
    drawChamferFrame(tx0, bedY0, tx1, bedY1, 10.0f, 1.5f, COL_FRAME, 1 | 8);

    for (int i = 0; i < count; i++) {
        const int floorNo = top - i;
        const bool viewed = floorNo == s_dmapViewFloor;
        // Each row slides out of the tab's slot to its own place.
        const f32 ryFull = listTop + (f32)i * (rowH + gap);
        const f32 ry = ty0 + (ryFull - ty0) * openT;
        // The viewed row IS the tab, so it stays solid; the others fade up as
        // they travel, or they would all be piled on the tab at t = 0.
        const f32 rowPrevA = s_drawAlpha;
        s_drawAlpha = rowPrevA * (viewed ? 1.0f : openT);
        // Same trim as the context tab it grew out of: left corners cut to
        // echo the screen edge, right square so each row runs flush into the
        // content window's border, and the viewed row left unframed so the
        // button does not visibly gain an edge the moment it opens.
        static const FloorRowStyle kColumnRow = {12.0f, 1 | 8, 15.0f, 6.0f, 26.0f, false};
        drawFloorRow(tx0, ry, tx1, ry + rowH, floorNo, playerFloor, kColumnRow,
            s_dmapFloorPickOpen);
        s_drawAlpha = rowPrevA;
    }
}


// Floor-select pop-up for the Cinematic layout: the list DROPS STRAIGHT DOWN
// from the context tab, taking the tab's own x-span and row height so the
// two read as one column. (It used to be offset a row-width to the left,
// which left it hanging beside the button it belongs to instead of under
// it.) Takes the tab's RECT for that reason — deriving the geometry beats
// passing a pre-offset point and hoping the two stay in step, and it mirrors
// drawFloorColumn. Drawn after the content window so it sits on top. Empty
// floors are dimmed and not tappable; the rest publish their tap rects.
void drawFloorOverlay(f32 tx0, f32 ty0, f32 tx1, f32 ty1, f32 clampY0, f32 clampY1) {
    s_dmapFloorRectCount = 0;
    if (!s_dmapFloorPickOpen) {
        return;
    }
    const int playerFloor = dMapInfo_c::getNowStayFloorNoDecisionFlg()
        ? dMapInfo_c::getNowStayFloorNo() : DMAP_FLOOR_FOLLOW;
    const f32 tw = tx1 - tx0;
    const f32 th = ty1 - ty0;
    const f32 gap = 3.0f;
    s8 top = 0;
    s8 bottom = 0;
    dMpath_c::getTopBottomFloorNo(&top, &bottom);
    int count = top - bottom + 1;
    if (count > DMAP_FLOOR_COUNT) {
        count = DMAP_FLOOR_COUNT;
    }
    // Hangs off the tab's bottom edge, then clamps so the list still fits
    // the content area (left top-anchored if it is taller than that).
    f32 listTop = ty1 + 2.0f;
    const f32 listH = (f32)count * th + (f32)(count - 1) * gap;
    // Geometric clamp whenever bounds are given: the plate renderer's scissor
    // strips override any enclosing clip, so a tall list would otherwise paint
    // straight past the content window's edge.
    if (clampY1 > clampY0) {
        if (listTop + listH > clampY1) {
            listTop = clampY1 - listH;
        }
        if (listTop < clampY0) {
            listTop = clampY0;
        }
    }
    for (int i = 0; i < count; i++) {
        const f32 ty = listTop + (f32)i * (th + gap);
        // The tab's own trim, for the same reason the Functional column uses
        // it: the list hangs directly off that button, so a different
        // chamfer or text size makes the button appear to change the moment
        // it opens. Cinematic draws the context tab as a plain rectangle
        // (mask 0), and drawChamferPlate carries no border, so the viewed
        // row is left unframed to match.
        static const FloorRowStyle kOverlayRow = {12.0f, 0, 15.0f, 6.0f, 26.0f, false};
        drawFloorRow(tx0, ty, tx0 + tw, ty + th, top - i, playerFloor, kOverlayRow, true);
    }
}

namespace {

// Logo placeholder for the map window when there is nothing to draw —
// shared by the minimap and floor-map paths so the two states look alike.
void drawMapPlaceholder(f32 x0, f32 y0, f32 x1, f32 y1, const char* caption) {
    const f32 mcx = (x0 + x1) * 0.5f;
    const f32 mcy = (y0 + y1) * 0.5f;
    if (const ResTIMG* logo = dusklightLogoTimg()) {
        const f32 s = 96.0f;
        drawTimg(logo, mcx - s * 0.5f, mcy - s * 0.5f, s, s, 0xFF);
    }
    drawTextCentered(mcx, mcy - 60.0f, 17.0f, TEXT_DIM, caption);
}

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
        s_mapAbsentFrames = 0;
        // The world offset is in the old stage's coordinates.
        s_mapViewOffX = 0.0f;
        s_mapViewOffZ = 0.0f;
    }
    // A live, valid map texture is required every frame: the render-texture
    // is freed on stage/room transitions and a cached pointer would show
    // garbage from the reused heap.
    if (!isSaneTimg(timg)) {
        s_lastMapTimg = NULL;  // force re-upload when it comes back
        // Nothing consumes the gestures this frame — drop them so they
        // don't burst into the view once the map appears.
        s_mapPanX = 0.0f;
        s_mapPanY = 0.0f;
        s_mapPinchDeltaMilli.exchange(0);
        // Some stages simply have no minimap — boss arenas most visibly —
        // and the game reports that by never drawing one. Once the stage is
        // fully loaded and the meter is up, a map that still has not appeared
        // after a grace period is not coming: say so instead of pretending to
        // load forever. During real loads stagInfo is NULL, which also resets
        // the grace counter for the next stage.
        const bool stageUp =
            meterMap != NULL && dComIfGp_getStage()->getStagInfo() != NULL;
        if (!stageUp) {
            s_mapAbsentFrames = 0;
        } else if (s_mapAbsentFrames <= NO_MAP_GRACE_FRAMES) {
            s_mapAbsentFrames++;
        }
        drawMapPlaceholder(x0, y0, x1, y1,
            s_mapAbsentFrames > NO_MAP_GRACE_FRAMES ? txt(STR_NO_MAP)
                                                    : txt(STR_LOADING_MAP));
        return;
    }
    s_mapAbsentFrames = 0;
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
    // Contain-fit, matching the dungeon map above. This used to take the LARGER
    // ratio — filling the window and cropping whatever overflowed. On the 8:7
    // bottom panel that cost almost nothing, because the map texture is roughly
    // square and so is the window. On a 16:9 companion (screens swapped) the
    // width ratio is far larger, so the map was scaled to fill the width and
    // the top and bottom of the area were cut off.
    const f32 fitScale = availW / texW < availH / texH ? availW / texW : availH / texH;
    const f32 scale = fitScale * zoomIn;
    const f32 drawW = texW * scale;
    const f32 drawH = texH * scale;

    miniMapApplyPan(map, drawW, drawH);

    setWinScissor(x0, y0, x0 + availW, y0 + availH);
    s_mapPic->setAlpha(mulDrawAlpha(0xFF));
    s_mapPic->draw(x0 + (availW - drawW) * 0.5f,
        y0 + (availH - drawH) * 0.5f, drawW, drawH, false, false, false);
    applyWinClip();
    dComIfGp_getCurrentGrafPort()->setup2D();

    // Reset-view button, bottom right inside the window.
    const bool viewMoved = s_mapZoom > 1.01f || s_mapZoom < 0.99f ||
        s_mapViewOffX != 0.0f || s_mapViewOffZ != 0.0f;
    drawMapResetButton(x1, y1, viewMoved);
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
    // Functional shows the region name in the left column's place page, so a
    // plate over the map here would just say it twice.
    if (dusk::dualscreen::mainHudRestored()) {
        return;
    }
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

// Cinematic has no left column, so the context action lives as an in-window
// button: top-right on the map (with a leftward floor overlay), bottom-right
// on items/collect. Functional draws it in the left column instead, so this
// is a no-op there.
void drawCinematicContextTab(f32 x0, f32 y0, f32 x1, f32 y1) {
    if (dusk::dualscreen::mainHudRestored()) {
        return;
    }
    bool clickable = false;
    const int action = contextTabAction(&clickable);
    // The ITEMS reader keeps its own in-window Back button, so its CTX_BACK
    // is suppressed here; COLLECT's detail views navigate through the tab in
    // both modes (the library sections have no other way back).
    if (action == CTX_NONE ||
        (action == CTX_BACK && s_page.load() == PAGE_INVENTORY))
    {
        s_ctxTabRect[0] = 0.0f;
        s_ctxTabRect[2] = 0.0f;
        return;
    }
    const f32 tw = 70.0f;
    const f32 th = 28.0f;
    const bool onMap = s_page.load() == PAGE_MAP;
    const f32 tx = x1 - 4.0f - tw;
    const f32 ty = onMap ? y0 + 4.0f : y1 - 4.0f - th;
    drawContextTab(tx, ty, tx + tw, ty + th);
    if (onMap && s_dmapAvailable && s_dmapFloorPickOpen) {
        // Clamped to the window's vertical span because the plate strips
        // ignore the enclosing scissor.
        drawFloorOverlay(tx, ty, tx + tw, ty + th, y0, y1);
    }
}

namespace {

// Feedback line for the MAP page ("Can't warp from here"): the context tab
// sets it, but no map draw rendered it — only the error sound reached the
// user. Bottom-left, clear of the Reset button opposite; the ITEMS caption's
// metrics.
void drawMapCaption(f32 x0, f32 y1) {
    if (s_equipMsgFrames > 0) {
        constexpr u32 TEXT_WARN = 0xF0A050FF;
        drawText(x0 + 14.0f, y1 - 10.0f, 13.0f, TEXT_WARN, "%s", s_equipMsg);
    }
}

}  // namespace

void drawMapContent(f32 x0, f32 y0, f32 x1, f32 y1) {

    // In dungeons the live floor map replaces the minimap crop.
    if (s_dmapAvailable) {
        if (drawDungeonMapContent(x0, y0, x1, y1)) {
            drawMapNamePlate(x0, y0);
        } else {
            // Either the companion yielded because the game's own dungeon
            // map screen is up (point the player there), or the first render
            // (palette mount / renderer create) is still pending.
            const bool mapOnMain =
                dMeter2Info_getWindowStatus() == WINDOW_STATUS_DUNGEON_MAP;
            drawMapPlaceholder(x0, y0, x1, y1,
                mapOnMain ? txt(STR_MAP_ON_MAIN) : txt(STR_LOADING_FLOOR));
            // The floor overlay must not linger over a map that isn't there.
            s_dmapFloorRectCount = 0;
            s_dmapFloorPickOpen = false;
            s_mapResetRect[2] = s_mapResetRect[0];          // hidden
            // Nothing consumed the gestures this frame — drop them so they
            // don't burst into the view once it appears.
            s_mapPanX = 0.0f;
            s_mapPanY = 0.0f;
            s_mapPinchDeltaMilli.exchange(0);
        }
        drawMapCaption(x0, y1);
        return;
    }
    drawMiniMapContent(x0, y0, x1, y1);
    drawMapNamePlate(x0, y0);
    drawMapCaption(x0, y1);
    // Overworld minimap has no floors: the context tab shows Warp, not Floor.
    s_dmapFloorRectCount = 0;
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
        // Full column: the icon sits in the header row, not beside the text,
        // so nothing narrows the wrap. Reserving icon width here squeezed the
        // description into a sliver on narrow companion canvases.
        readerWrapBody(body, x1 - x0 - 24.0f, 14.0f);
        fetchedGen = readerBodyGen();
        readerInvalidate();
        s_scrollItemInfo = 0.0f;
    }
    // Pop out of / back into the item cell this was opened from.
    if (!readerZoomStep(&x0, &y0, &x1, &y1)) {
        s_itemInfoSlot = -1;
        s_scrollItemInfo = 0.0f;
        return;
    }
    // Header band: the icon keeps its old size but sits above the text
    // instead of inside it, so the description still gets the full column.
    constexpr f32 HDR_ICON = 48.0f;
    constexpr f32 HDR_H = HDR_ICON + 8.0f;
    drawItemIcon(s_itemInfoSlot, itemNo, x1 - HDR_ICON - 4.0f, y0 + 2.0f, HDR_ICON);
    // Name right-aligned, ending just before the icon (title-before-icon,
    // matching the collect headers), vertically centred against it.
    const f32 nw = measureText(16.0f, name);
    drawText(x1 - HDR_ICON - 12.0f - nw, y0 + HDR_H * 0.5f + 6.0f, 16.0f, TEXT_ACCENT, "%s",
        name);
    const f32 by0 = y0 + HDR_H;
    const f32 by1 = y1 - 2.0f;
    drawDetailBox(x0, by0, x1, by1);
    const f32 textBottom = readerTextBottom(by1);
    const f32 lineH = 21.0f;
    const f32 viewH = textBottom - by0 - 12.0f;
    const int lineCount = readerBodyLineCount();
    const f32 contentH = (f32)lineCount * lineH;
    const f32 maxScroll = clampListScroll(&s_scrollItemInfo, contentH, viewH);
    if (s_nativeW != 0) {
        setWinScissor(x0, by0 + 6.0f, x1, textBottom);
    }
    for (int i = 0; i < lineCount; i++) {
        const f32 ly = by0 + 22.0f + (f32)i * lineH - s_scrollItemInfo;
        if (ly < by0 - lineH || ly > textBottom + lineH) {
            continue;
        }
        readerDrawBodyLine(i, x0 + 16.0f, ly, 14.0f, TEXT_MAIN);
    }
    if (s_nativeW != 0) {
        applyWinClip();
    }
    drawListScrollHint(x1 - 4.0f, by0 + 4.0f, textBottom - 4.0f, s_scrollItemInfo, maxScroll,
        viewH, contentH);
    // Functional turns the context tab into Back while this is open, so the
    // in-window button would be a second way to do the same thing.
    if (dusk::dualscreen::mainHudRestored()) {
        s_itemInfoBtnRect[0] = 0.0f;
        s_itemInfoBtnRect[2] = 0.0f;
        return;
    }
    const f32 fy = by1 - 38.0f;
    drawTabPlate(x1 - 100.0f, fy, 90.0f, 30.0f, false);
    drawTextFittedCentered(x1 - 55.0f, fy + 20.0f, 14.0f, 9.0f, 84.0f, TEXT_MAIN,
        localizedWord(0x0054, "Back"));
    s_itemInfoBtnRect[0] = x1 - 100.0f;
    s_itemInfoBtnRect[1] = fy;
    s_itemInfoBtnRect[2] = x1 - 10.0f;
    s_itemInfoBtnRect[3] = fy + 30.0f;
}

void drawInventoryContent(f32 x0, f32 y0, f32 x1, f32 y1) {
    s_itemInfoBtnRect[0] = 0.0f;
    s_itemInfoBtnRect[2] = 0.0f;
    if (s_itemInfoSlot >= 0) {
        // The grid stays underneath and POPS DOWN while the info panel
        // travels, matching the mail/skill readers. Without it the panel grew
        // over a bare window backdrop: the detail box that used to fill it was
        // removed, but only the two collect readers got the compensating
        // underlay, so ITEMS was left animating over nothing.
        // Geometry is still marked invalid — the cells must not be tappable
        // while they are a backdrop (pushReaderRect guards the readers the
        // same way).
        if (readerZoomActive()) {
            const f32 t = readerZoomProgress();
            const f32 shrink = 0.06f * t;
            const f32 ox = (x1 - x0) * shrink * 0.5f;
            const f32 oy = (y1 - y0) * shrink * 0.5f;
            const f32 prevA = s_drawAlpha;
            s_drawAlpha = prevA * (1.0f - t);
            const InvGrid under = computeInvGrid(x0 + ox, y0 + oy, x1 - ox, y1 - oy);
            for (int i = 0; i < INV_CELLS; i++) {
                drawInvCell(under, i);
            }
            s_drawAlpha = prevA;
        }
        s_invGeomValid = false;
        drawItemInfo(x0, y0, x1, y1);
        return;
    }
    const InvGrid grid = computeInvGrid(x0, y0, x1, y1);
    s_invGeomValid = true;
    s_invCell = grid.cell;
    s_invCellCount = INV_CELLS;
    for (int i = 0; i < INV_CELLS; i++) {
        drawInvCell(grid, i);
    }
    drawInventoryCaption(x0, x1, y1);
    // The "Info" trigger now lives in the left column's context tab; the
    // reader view (drawItemInfo) is unchanged.
}


// Destroy the dungeon-map picture outright. Called when the renderer that
// owns its ResTIMG is torn down: re-pointing it later is not enough, because
// the J2DPicture keeps the old descriptor (and the buffer behind it) until
// then, and anything that draws it in the meantime uploads from freed memory.
void invalidateDmapPicture() {
    if (s_dmapPic != NULL) {
        JKR_DELETE(s_dmapPic);
        s_dmapPic = NULL;
    }
    s_lastDmapTimg = NULL;
}

}  // namespace dusk::companion
