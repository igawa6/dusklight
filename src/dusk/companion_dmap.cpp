// Companion dungeon map: a live, full-floor render of the pause menu's
// dungeon map (renderingDmap_c pair owned by dMenu_DmapMap_c), driven during
// gameplay for the second screen. The pause menu only renders this while the
// world is frozen; here it re-renders every frame, so room states, doors and
// (later) the player arrow stay live. The renderer registers itself on the
// frame's copy-2D drawlist, the same pipeline stage the minimap uses.

#include "dusk/companion.h"
#include "dusk/companion_internal.h"
#include "dusk/dualscreen.h"

#include "JSystem/JKernel/JKRExpHeap.h"  // mDoExt heaps are JKRExpHeap*
#include "JSystem/JKernel/JKRMemArchive.h"
#include "d/actor/d_a_player.h"
#include "d/d_com_inf_game.h"
#include "d/d_map.h"
#include "d/d_menu_dmap_map.h"
#include "d/d_meter2_info.h"
#include "d/d_meter_map.h"
#include "m_Do/m_Do_dvd_thread.h"
#include "m_Do/m_Do_ext.h"

#include <cstring>

namespace dusk::companion {
namespace {

#include "companion_dmap_icons.inc"

// Fraction of the target left as margin around the whole-floor fit. The
// logical target size is DMAP_TEX_SIZE (the PC render-size boost scales the
// real pixel resolution on top, exactly as for the minimap texture).
constexpr f32 DMAP_FIT = 0.92f;

dMenu_DmapMap_c* s_inst;
mDoDvdThd_mountArchive_c* s_mount;
// CI palette for the render target: dat/data.dat from res-d.arc (120 RGB5A3
// entries, 240 bytes). makeResTIMG stores palette - timg into ResTIMG's
// s32 paletteOffset, so this copy MUST live on a JKR heap (same arena as
// the ResTIMG allocation) — a static buffer is out of s32 offset range.
u8* s_palette;
bool s_paletteReady;
constexpr u32 DMAP_PALETTE_BYTES = 240;
char s_dmapStage[12];

// Fetch the menu-map palette once: mount the menu's archive, copy the
// palette out, unmount immediately (no lifetime coupling with the pause
// menu's own mount of the same archive). Returns false while the async
// mount is still in flight.
bool ensurePalette() {
    if (s_paletteReady) {
        return true;
    }
    if (s_mount == NULL) {
        s_mount = mDoDvdThd_mountArchive_c::create("/res/FieldMap/res-d.arc", 2,
            mDoExt_getJ2dHeap());
        if (s_mount == NULL) {
            return false;
        }
    }
    if (!s_mount->sync()) {
        return false;
    }
    JKRMemArchive* arc = s_mount->getArchive();
    if (arc != NULL) {
        const void* dat = arc->getResource("dat/data.dat");
        if (dat != NULL) {
            if (s_palette == NULL) {
                JKRHeap* prevHeap = mDoExt_setCurrentHeap(mDoExt_getZeldaHeap());
                s_palette = JKR_NEW_ARRAY_ARGS(u8, DMAP_PALETTE_BYTES, 0x20);
                mDoExt_setCurrentHeap(prevHeap);
            }
            if (s_palette != NULL) {
                memcpy(s_palette, dat, DMAP_PALETTE_BYTES);
                s_paletteReady = true;
            }
        }
        JKRUnmountArchive(arc);
    }
    s_mount->destroy();
    s_mount = NULL;
    return s_paletteReady;
}

void releaseRenderer() {
    if (s_inst != NULL) {
        // Drop the page's picture BEFORE the descriptor it points at dies:
        // the generation counter only makes the page re-point later, which
        // leaves a window where the picture still holds freed memory.
        invalidateDmapPicture();
        s_inst->_delete();
        JKR_DELETE(s_inst);
        s_inst = NULL;
        s_dmapGen++;  // invalidate the page's blit cache (pointer may be reused)
    }
    s_dmapReady = false;
    s_dmapIconCount = 0;
}

// Collect the viewed floor's overlay icons with the pause map's exact
// enumeration and visibility policy (dMenu_Dmap_c::getIconPos): dTres
// type-groups mapped to menu icon ids, filtered per floor/item ownership by
// the renderer's isDrawIconSingle.
void gatherIcons(int i_floor) {
    s_dmapIconCount = 0;
    struct GroupMap {
        u8 group;
        u8 icon;
        bool firstOnly;
    };
    static const GroupMap l_groups[] = {
        {1, ICON_DUNGEON_ENTER_e, false},
        {8, ICON_LV8_WARP_e, false},
        {3, ICON_BOSS_e, true},
        {0, ICON_TREASURE_CHEST_e, false},
        {5, ICON_DESTINATION_e, false},
        {4, ICON_LIGHT_DROP_e, false},
        {12, ICON_LIGHT_BALL_e, false},
        {11, ICON_CANNON_BALL_e, false},
        {2, ICON_KEY_e, false},
        {9, ICON_MONKEY_e, false},
        {15, ICON_COPY_STATUE_e, false},
        {16, ICON_OOCCOO_e, false},
    };
    const s8 stayNo = (s8)dComIfGp_roomControl_getStayNo();
    constexpr int MAX_ICONS = (int)(sizeof(s_dmapIcons) / sizeof(s_dmapIcons[0]));
    for (const GroupMap& g : l_groups) {
        if (g.group == 4 && !dComIfGp_isLightDropMapVisible()) {
            continue;
        }
        dTres_c::typeGroupData_c* data = dTres_c::getFirstData(g.group);
        int n = dTres_c::getTypeGroupNumber(g.group);
        while (data != NULL && n-- > 0 && s_dmapIconCount < MAX_ICONS) {
            BE(Vec) pos = *data->getPos();
            if (s_inst->mRend[0].isDrawIconSingle(data->getDataPointer(), stayNo, i_floor, 1,
                    true, &pos))
            {
                u8 icon = g.icon;
                if (g.group == 16) {
                    const u8 sw = data->getSwBit();
                    icon = (sw == 0xFF || dComIfGs_isSwitch(sw, data->getRoomNo()))
                        ? ICON_OOCCOO_e : ICON_OOCCOO_JR_e;
                }
                s_dmapIcons[s_dmapIconCount].x = pos.x;
                s_dmapIcons[s_dmapIconCount].z = pos.z;
                s_dmapIcons[s_dmapIconCount].rot = 0;
                s_dmapIcons[s_dmapIconCount].icon = icon;
                s_dmapIconCount++;
                if (g.firstOnly) {
                    break;
                }
            }
            data = dTres_c::getNextData(data);
        }
    }
    // Boss floor for the floor-plate marker (compass/switch/alive gating
    // lives inside getBossIconFloorNo, same as the pause menu).
    int bossFloor = 0;
    s_dmapBossFloor =
        dTres_c::getBossIconFloorNo(&bossFloor) ? bossFloor : DMAP_FLOOR_FOLLOW;
    // Restart (save entry) marker, on its own floor only.
    if (s_dmapIconCount < MAX_ICONS) {
        const Vec restart = dMapInfo_n::getMapRestartPos();
        if (dMapInfo_c::calcNowStayFloorNo(restart.y, true) == i_floor) {
            s_dmapIcons[s_dmapIconCount].x = restart.x;
            s_dmapIcons[s_dmapIconCount].z = restart.z;
            s_dmapIcons[s_dmapIconCount].rot = dMapInfo_n::getMapRestartAngleY();
            s_dmapIcons[s_dmapIconCount].icon = ICON_LINK_ENTER_e;
            s_dmapIconCount++;
        }
    }
}

}  // namespace

const ResTIMG* dmapTimg() {
    return s_inst != NULL ? s_inst->getResTIMGPointer(0) : NULL;
}

const ResTIMG* dmapFloorFaceTimg(bool i_wolf) {
    return (const ResTIMG*)(i_wolf ? l_dmapFaceWolfBti : l_dmapFaceRinkBti);
}

const ResTIMG* dmapFloorFaceTimg() {
    return dmapFloorFaceTimg(daPy_py_c::checkNowWolf());
}

const ResTIMG* dmapFloorBossMarkTimg() {
    return (const ResTIMG*)l_dmapFloorBossBti;
}

// Stay-room fit zoom (the minimap-style close view): room span * 1.6 mapped
// into the dungeon window, clamped to the gesture zoom range. 0 when the
// room bounds are unknown.
static f32 roomFitZoom(int i_stayNo, f32 i_size) {
    f32 rx0 = 0.0f;
    f32 rz0 = 0.0f;
    f32 rx1 = 0.0f;
    f32 rz1 = 0.0f;
    dMapInfo_n::getRoomMinMaxXZ(i_stayNo, &rx0, &rz0, &rx1, &rz1);
    const f32 rw = rx1 > rx0 ? rx1 - rx0 : rx0 - rx1;
    const f32 rh = rz1 > rz0 ? rz1 - rz0 : rz0 - rz1;
    const f32 roomSpan = (rw > rh ? rw : rh) * 1.6f;
    if (roomSpan <= 0.0f) {
        return 0.0f;
    }
    f32 zoom = i_size / (DMAP_FIT * roomSpan);
    if (zoom < 1.0f) {
        zoom = 1.0f;
    } else if (zoom > 4.0f) {
        zoom = 4.0f;
    }
    return zoom;
}

void dmapUpdate() {
    // Drain an in-flight palette mount even outside dungeons so it can't
    // stay pending (and its archive mounted) for the rest of the session.
    if (s_mount != NULL) {
        ensurePalette();
    }
    // Stage change rebuilds the map-path data: drop the renderer and
    // re-latch the defaults (dungeon view on, follow the player's floor).
    const char* stage = dComIfGp_getStartStageName();
    if (strncmp(stage, s_dmapStage, sizeof(s_dmapStage) - 1) != 0) {
        strncpy(s_dmapStage, stage, sizeof(s_dmapStage) - 1);
        s_dmapStage[sizeof(s_dmapStage) - 1] = '\0';
        releaseRenderer();
        s_dmapFloorSel = DMAP_FLOOR_FOLLOW;
        s_dmapZoom = 1.0f;
        s_dmapOffX = 0.0f;
        s_dmapOffZ = 0.0f;
        s_dmapFollow = true;
        s_dmapResetReq = true;
    }

    dMeterMap_c* meterMap = dMeter2Info_getMeterMapClass();
    dMap_c* map = meterMap != NULL ? meterMap->getDMap() : NULL;
    const bool inDungeon =
        map != NULL && map->getStayType() == 1 && dMpath_c::isExistMapPathData();
    // On dungeon entry, clear the overworld minimap steering: the floor map
    // replaces the minimap here, and stale offsets would keep shifting the
    // (hidden) minimap render via mapViewAdjust for the whole dungeon.
    static bool s_wasDungeon = false;
    if (inDungeon && !s_wasDungeon) {
        s_mapViewOffX = 0.0f;
        s_mapViewOffZ = 0.0f;
        s_mapRenderScale = 1.0f;
        s_mapZoom = 1.0f;
        s_mapPanX = 0.0f;
        s_mapPanY = 0.0f;
    }
    s_wasDungeon = inDungeon;
    s_dmapAvailable = inDungeon;
    if (!inDungeon) {
        releaseRenderer();
        return;
    }
    if (!dualscreen::hudOnCompanion() || !ensurePalette()) {
        return;
    }
    if (s_inst == NULL) {
        JKRHeap* prevHeap = mDoExt_setCurrentHeap(mDoExt_getZeldaHeap());
        s_inst = JKR_NEW dMenu_DmapMap_c;
        if (s_inst != NULL) {
            s_inst->_create(DMAP_TEX_SIZE, DMAP_TEX_SIZE, DMAP_TEX_SIZE, DMAP_TEX_SIZE, s_palette);
        }
        mDoExt_setCurrentHeap(prevHeap);
        if (s_inst == NULL) {
            return;
        }
    }
    if (s_page.load() != PAGE_MAP) {
        // Keep the instance (and its last texture) but skip the render pass
        // while the map page isn't showing.
        return;
    }
    // While the game's own dungeon map screen is up, yield: the copy-2D
    // drawlist holds four entries and the minimap plus the menu's renderers
    // sit at that bound already (dDlst_list_c::set drops overflow SILENTLY —
    // a dropped renderer never produces its copy texture and whoever samples
    // it reads a 32-byte stub as a full-size image). There is also no point
    // re-rendering the same dungeon map behind the menu's full-screen one.
    if (dMeter2Info_getWindowStatus() == WINDOW_STATUS_DUNGEON_MAP) {
        // No render entry this frame means no copy texture, so the page must
        // not draw the picture either.
        s_dmapReady = false;
        return;
    }

    s8 top = 0;
    s8 bottom = 0;
    dMpath_c::getTopBottomFloorNo(&top, &bottom);
    const bool playerFloorKnown = dMapInfo_c::getNowStayFloorNoDecisionFlg();
    int floor = s_dmapFloorSel;
    if (floor == DMAP_FLOOR_FOLLOW) {
        floor = playerFloorKnown ? dMapInfo_c::getNowStayFloorNo() : bottom;
    }
    if (floor < bottom) {
        floor = bottom;
    } else if (floor > top) {
        floor = top;
    }

    // Whole-dungeon window baseline at zoom 1 so floor switches don't jump;
    // zoom divides the window (real render zoom, always sharp).
    const f32 sizeX = dMpath_c::getSizeX();
    const f32 sizeZ = dMpath_c::getSizeZ();
    const f32 size = sizeX > sizeZ ? sizeX : sizeZ;
    if (size <= 0.0f) {
        return;
    }
    const s8 stayNo = (s8)dComIfGp_roomControl_getStayNo();
    const bool stayRoomValid =
        stayNo >= 0 && dStage_roomControl_c::getFileList2(stayNo) != NULL;

    // Floor availability, bit (floorNo + 5): with the dungeon map item every
    // floor has content; without it only floors holding the stay room or a
    // visited room draw anything.
    if (dMapInfo_n::chkGetMap()) {
        s_dmapFloorAvail = 0x1FFF;
    } else {
        u16 avail = 0;
        for (int layer = 0; layer < 2; layer++) {
            for (int room = 0; room < 64; room++) {
                dDrawPath_c::room_class* rc = dMpath_c::getRoomPointer(layer, room);
                if (rc == NULL ||
                    (room != stayNo && !dMapInfo_n::isVisitedRoom(room))) {
                    continue;
                }
                dDrawPath_c::floor_class* fl = rc->mpFloor;
                for (int i = 0; fl != NULL && i < rc->mFloorNum; i++, fl++) {
                    const int bit = fl->mFloorNo + 5;
                    if (bit >= 0 && bit < 13) {
                        avail |= (u16)(1u << bit);
                    }
                }
            }
        }
        s_dmapFloorAvail = avail;
    }

    // Reset: back to the minimap-style default — the stay room fit at a
    // close zoom, room-follow re-engaged (the center then slides there).
    if (s_dmapResetReq) {
        s_dmapResetReq = false;
        s_dmapFollow = true;
        if (stayRoomValid) {
            const f32 fit = roomFitZoom(stayNo, size);
            if (fit > 0.0f) {
                s_dmapZoom = fit;
            }
        }
    }
    if (s_dmapZoom < 1.0f) {
        s_dmapZoom = 1.0f;
    } else if (s_dmapZoom > 4.0f) {
        s_dmapZoom = 4.0f;
    }
    const f32 cmPerTexel = size / ((f32)DMAP_TEX_SIZE * DMAP_FIT) / s_dmapZoom;
    const f32 baseCx = dMpath_c::getCenterX();
    const f32 baseCz = dMpath_c::getCenterZ();

    // Room-follow: glide the center toward the stay room; when browsing
    // another floor, glide to that floor's overview (whole floor, zoom 1).
    // Manual gestures (drag/pinch) clear the flag.
    if (s_dmapFollow) {
        f32 tx = baseCx;
        f32 tz = baseCz;
        const bool onPlayerFloor =
            !playerFloorKnown || floor == dMapInfo_c::getNowStayFloorNo();
        if (onPlayerFloor && stayRoomValid) {
            dMapInfo_n::getRoomCenter(stayNo, &tx, &tz);
            // Re-fit the zoom to the room being followed, gliding alongside
            // the pan: crossing a door into a differently-sized room used to
            // keep the old room's zoom until a manual Reset.
            const f32 fit = roomFitZoom(stayNo, size);
            if (fit > 0.0f) {
                s_dmapZoom += (fit - s_dmapZoom) * 0.12f;
            }
        } else {
            s_dmapZoom += (1.0f - s_dmapZoom) * 0.12f;
        }
        s_dmapOffX += ((tx - baseCx) - s_dmapOffX) * 0.12f;
        s_dmapOffZ += ((tz - baseCz) - s_dmapOffZ) * 0.12f;
    }

    // Keep the render window on the dungeon: the clamp converges to the
    // dungeon center as the window grows (whole floor at zoom 1).
    const f32 span = size / (DMAP_FIT * s_dmapZoom);
    const f32 maxOffX = sizeX > span ? (sizeX - span) * 0.5f : 0.0f;
    const f32 maxOffZ = sizeZ > span ? (sizeZ - span) * 0.5f : 0.0f;
    if (s_dmapOffX < -maxOffX) {
        s_dmapOffX = -maxOffX;
    } else if (s_dmapOffX > maxOffX) {
        s_dmapOffX = maxOffX;
    }
    if (s_dmapOffZ < -maxOffZ) {
        s_dmapOffZ = -maxOffZ;
    } else if (s_dmapOffZ > maxOffZ) {
        s_dmapOffZ = maxOffZ;
    }
    const f32 cx = baseCx + s_dmapOffX;
    const f32 cz = baseCz + s_dmapOffZ;
    s_inst->mRend[0].entry(cx, cz, cmPerTexel, (s8)dComIfGp_roomControl_getStayNo(),
        (s8)floor, 1.0f);
    gatherIcons(floor);
    s_dmapViewCx = cx;
    s_dmapViewCz = cz;
    s_dmapCmPerTexel = cmPerTexel;
    s_dmapViewFloor = floor;
    s_dmapReady = true;
}

}  // namespace dusk::companion
