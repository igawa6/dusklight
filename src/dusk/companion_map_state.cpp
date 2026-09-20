#include "dusk/companion_map_state.h"

#include "dusk/dualscreen.h"

#if DUSK_PHONE_SPIKE_STATE

#include "dusk/companion.h"
#include "dusk/companion_internal.h"
#include "dusk/settings.h"

#include "SSystem/SComponent/c_math.h"
#include "d/d_com_inf_game.h"
#include "d/d_map_path_dmap.h"
#include "d/d_save.h"

namespace dusk::companion {

namespace {

// Mirror mode flips the map horizontally, and the dashboard's own overlay
// math accounts for it (companion_pages.cpp's mapXSign()). That function is
// file-local there, so the one-line rule is repeated rather than exported —
// duplicating a sign is cheaper than widening that file's interface, and if
// it ever grows past one line it should move to the header instead.
f32 mapXSignLocal() {
    return dusk::getSettings().game.enableMirrorMode ? -1.0f : 1.0f;
}

}  // namespace

bool canonicalMapView(f32& centerX, f32& centerZ, f32& span) {
    const f32 sizeX = dMpath_c::getSizeX();
    const f32 sizeZ = dMpath_c::getSizeZ();
    const f32 size = sizeX > sizeZ ? sizeX : sizeZ;
    if (!(size > 0.0f)) {
        return false;  // extents not known yet (also catches NaN)
    }
    centerX = dMpath_c::getCenterX();
    centerZ = dMpath_c::getCenterZ();
    // The canonical framing is the whole floor at zoom 1: the same
    // cmPerTexel * DMAP_TEX_SIZE the dashboard computes when s_dmapZoom is
    // 1, which reduces to size / DMAP_FIT. Deliberately independent of
    // s_dmapZoom and s_dmapOffX/Z — those follow the player, and a base
    // image that moves with the player is one that has to be resent every
    // frame (see the header comment).
    span = size / DMAP_FIT;
    return true;
}

u32 mapBaseGeneration() {
    // Two independent sources, mixed rather than compared: s_dmapGen bumps
    // when the renderer is torn down (stage change, save load), and the
    // visited-room counter bumps when a room's visited bit changes — which
    // alters room colours, line widths and which rooms are drawn at all. The
    // viewed floor is folded in because each floor is a different image.
    return s_dmapGen * 1000003u + dSv_visitedRoomGeneration() * 31u +
        (u32)(s_dmapViewFloor + 128);
}

bool mapBaseAvailable() {
    return s_dmapReady && dmapTimg() != NULL;
}

void setMapBaseCanonicalView(bool enabled) {
    s_dmapCanonicalView = enabled;
}

bool gatherMapState(MapState& out) {
    out = MapState{};
    if (!s_dmapReady) {
        return false;
    }
    f32 centerX = 0.0f;
    f32 centerZ = 0.0f;
    f32 span = 0.0f;
    if (!canonicalMapView(centerX, centerZ, span)) {
        return false;
    }
    const f32 xSign = mapXSignLocal();
    // Projects a world position into the canonical image's 0..1 space.
    auto project = [&](f32 worldX, f32 worldZ, f32& u, f32& v) {
        u = 0.5f + (worldX - centerX) * xSign / span;
        v = 0.5f + (worldZ - centerZ) / span;
    };

    out.active = true;
    out.floor = (s8)s_dmapViewFloor;
    out.playerFloorKnown = dMapInfo_c::getNowStayFloorNoDecisionFlg();
    out.playerFloor = out.playerFloorKnown ? (s8)dMapInfo_c::getNowStayFloorNo() : (s8)0;

    // getMapPlayerPos/AngleY, not the actor's own position: these apply the
    // stay room's origin/rotation correction from the stage file list, which
    // is per-room data the phone has no access to, and on PC they are also
    // already run through the frame interpolator so they are smoothed to
    // render rate rather than stepping at the 60Hz game tick.
    const Vec playerPos = dMapInfo_n::getMapPlayerPos();
    project(playerPos.x, playerPos.z, out.playerU, out.playerV);
    f32 heading = cM_sht2d((f32)dMapInfo_n::getMapPlayerAngleY());
    if (xSign < 0.0f) {
        heading = -heading;  // mirror mode flips the heading too
    }
    out.playerHeadingDeg = heading;

    // Content generation for the base image. Two independent sources, mixed
    // rather than compared: s_dmapGen bumps when the renderer is torn down
    // (stage change), and the visited-room counter bumps when a room's
    // visited bit changes — which alters room colours, line widths and which
    // rooms are drawn at all. The viewed floor is folded in because each
    // floor is a different image entirely.
    out.baseGen = mapBaseGeneration();

    const int count = s_dmapIconCount < kMaxMapIcons ? s_dmapIconCount : kMaxMapIcons;
    out.iconCount = count;
    for (int i = 0; i < count; i++) {
        const DmapIcon& src = s_dmapIcons[i];
        MapIconState& dst = out.icons[i];
        project(src.x, src.z, dst.u, dst.v);
        // rot is a raw s16 binary angle and is zero for every icon except
        // the restart/save marker (gatherIcons only sets a real angle there).
        dst.rotDeg = src.rot != 0 ? cM_sht2d((f32)src.rot) : 0.0f;
        dst.kind = src.icon;
    }
    return true;
}

bool drawMapBaseImage(f32 canvasW, f32 canvasH, f32& outFitU0, f32& outFitV0, f32& outFitU1,
    f32& outFitV1) {
    const ResTIMG* timg = dmapTimg();
    if (!s_dmapReady || timg == NULL || canvasW <= 0.0f || canvasH <= 0.0f) {
        return false;
    }
    // Same flat clear the other substitution draws start with — the aux
    // render target's colour texture is pooled by size and never cleared
    // between uses, so without this the map composites over the last
    // dashboard frame still sitting in it.
    fillRect(0.0f, 0.0f, canvasW, canvasH, COL_BG);

    // The map texture is square (DMAP_TEX_SIZE logical texels each way);
    // letterbox it into whatever aspect the phone negotiated.
    const f32 side = canvasW < canvasH ? canvasW : canvasH;
    const f32 x = (canvasW - side) * 0.5f;
    const f32 y = (canvasH - side) * 0.5f;
    drawTimg(timg, x, y, side, side, 0xFF);

    outFitU0 = x / canvasW;
    outFitV0 = y / canvasH;
    outFitU1 = (x + side) / canvasW;
    outFitV1 = (y + side) / canvasH;
    return true;
}

}  // namespace dusk::companion

#endif  // DUSK_PHONE_SPIKE_STATE
