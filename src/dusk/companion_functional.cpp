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

#include <cmath>
#include <cstdio>
#include <cstring>

namespace dusk::companion {

// Defined further down this file; the left column and the corners are written
// above them.
void drawCornerBox(f32 x0, f32 y0, f32 x1, f32 y1, const char* label, bool enabled);
void drawTransformCorner(f32 x0, f32 y0, f32 x1, f32 y1);
void drawAmmoCountCentered(int ammo, f32 cx, f32 bottomY);

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
    bool truncated = false;
    bool overWide = false;
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
            // The second clause force-accepts a word wider than the line —
            // there is nowhere else to put it — so the line still needs
            // fitting on the way out.
            if (measureText(TS, probe) > maxW) {
                overWide = true;
            }
            snprintf(line[cur], sizeof(line[0]), "%s", probe);
            lines = cur + 1;
        } else if (cur == 0) {
            cur = 1;
            snprintf(line[1], sizeof(line[1]), "%s", word);
            lines = 2;
        } else {
            // Ran out of lines with words left over — mark the second line so
            // the clip is visible instead of silent (localized place names run
            // longer than the English these two lines were sized for).
            truncated = true;
            break;
        }
        p = end;
    }
    const f32 ty = clockY + (lines > 1 ? 12.0f : 18.0f);
    for (int i = 0; i < lines; i++) {
        if (truncated && i == lines - 1) {
            char clipped[36];
            snprintf(clipped, sizeof(clipped), "%s...", line[i]);
            drawTextFittedCentered(cx, ty + (f32)i * 13.0f, TS, 8.0f, maxW, TEXT_DIM, clipped);
        } else if (overWide) {
            drawTextFittedCentered(cx, ty + (f32)i * 13.0f, TS, 8.0f, maxW, TEXT_DIM, line[i]);
        } else {
            drawTextCentered(cx, ty + (f32)i * 13.0f, TS, TEXT_DIM, line[i]);
        }
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
        s_leftBoxDragY *= ANIM_DECAY_FAST;
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
// Case-insensitive LATIN-1 byte compare over n bytes.
bool scentCiEqual(const char* a, const char* b, int n) {
    for (int i = 0; i < n; i++) {
        char x = a[i];
        char y = b[i];
        if (x >= 'A' && x <= 'Z') { x = (char)(x - 'A' + 'a'); }
        if (y >= 'A' && y <= 'Z') { y = (char)(y - 'A' + 'a'); }
        if (x != y) { return false; }
    }
    return true;
}

// The corner box is already headed with the scent word ("Scent" / "Geruch" /
// "Odeur" / "Olor" / "Odore"), so repeating it in the name below says it twice
// and eats width this FN_CORNER-wide box does not have. Strip it wherever it
// falls -- English puts it last ("Medicine Scent"), the other four put it
// first ("Geruch von Medizin") -- then drop the linking particle it leaves
// stranded, so "Scent of Ilia" reduces to "Ilia" and not "of Ilia".
//
// ONE flat particle list covers all five languages rather than keying off the
// current one: no scent name in any language begins with another language's
// particle, and a single list cannot drift out of sync with the language
// setting the way a per-language table would.
const char* trimScentWord(const char* name, char* buf, int bufLen) {
    const char* label = txt(STR_SCENT);
    if (name == NULL || label == NULL || *label == '\0') {
        return name;
    }
    const int nl = (int)strlen(name);
    const int ll = (int)strlen(label);
    if (ll == 0 || nl <= ll) {
        return name;
    }
    // Match the label as a WHOLE word, so it can never hit inside a longer one.
    int at = -1;
    for (int i = 0; i + ll <= nl; i++) {
        const bool leftOk = (i == 0) || name[i - 1] == ' ';
        const bool rightOk = (i + ll == nl) || name[i + ll] == ' ';
        if (leftOk && rightOk && scentCiEqual(name + i, label, ll)) {
            at = i;
            break;
        }
    }
    if (at < 0) {
        return name;
    }
    char tmp[80];
    int t = 0;
    for (int i = 0; i < nl && t < (int)sizeof(tmp) - 1; i++) {
        if (i >= at && i < at + ll) {
            continue;
        }
        tmp[t++] = name[i];
    }
    tmp[t] = '\0';
    int s = 0;
    while (tmp[s] == ' ') {
        s++;
    }
    while (t > s && tmp[t - 1] == ' ') {
        tmp[--t] = '\0';
    }
    // Elided article ("d'Iria"): cut through the apostrophe.
    for (int i = s; i < t && i < s + 5; i++) {
        if (tmp[i] == '\'') {
            static const char* const kElide[] = {"d", "l", "dell", "nell", "all"};
            const int len = i - s;
            for (unsigned k = 0; k < sizeof(kElide) / sizeof(kElide[0]); k++) {
                if ((int)strlen(kElide[k]) == len && scentCiEqual(tmp + s, kElide[k], len)) {
                    s = i + 1;
                    break;
                }
            }
            break;
        }
    }
    // Leading particle, but only when a real word follows it -- a one-word
    // name that happens to look like a particle must survive intact.
    {
        static const char* const kParticles[] = {"of", "the", "von", "vom", "der",
            "des", "dem", "die", "das", "de", "du", "da", "di", "dei", "del",
            "della", "delle", "a", "al", "el", "la", "le", "los", "las"};
        int e = s;
        while (e < t && tmp[e] != ' ') {
            e++;
        }
        if (e < t) {
            const int len = e - s;
            for (unsigned k = 0; k < sizeof(kParticles) / sizeof(kParticles[0]); k++) {
                if ((int)strlen(kParticles[k]) == len &&
                    scentCiEqual(tmp + s, kParticles[k], len))
                {
                    s = e;
                    while (s < t && tmp[s] == ' ') {
                        s++;
                    }
                    break;
                }
            }
        }
    }
    const int keep = t - s;
    if (keep <= 0 || keep >= bufLen) {
        return name;
    }
    memcpy(buf, tmp + s, (size_t)keep);
    buf[keep] = '\0';
    // Standing alone under the header the fragment is a LABEL, not a phrase,
    // so every word takes a capital even where the source had it mid-sentence
    // ("Olor a barbo oloroso" -> "Barbo Oloroso").
    //
    // LATIN-1: the accented letters carry the same 0x20 case offset as ASCII,
    // so one branch covers both. 0xF7 is the division sign sitting inside that
    // range and must be skipped; 0xFF has no uppercase form in LATIN-1 at all,
    // which is why the range stops at 0xFE.
    for (int i = 0; i < keep; i++) {
        if (i > 0 && buf[i - 1] != ' ') {
            continue;
        }
        const unsigned char c = (unsigned char)buf[i];
        if ((c >= 'a' && c <= 'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7)) {
            buf[i] = (char)(c - 0x20);
        }
    }
    return buf;
}



void drawWolfCornerCell(int which, f32 x0, f32 y0, f32 x1, f32 y1) {
    const f32 side = x1 - x0;
    const f32 icon = side * 0.55f;
    const f32 ix = x0 + (side - icon) * 0.5f;
    const f32 iy = y0 + (side - icon) * 0.45f;
    const f32 cx = (x0 + x1) * 0.5f;
    if (which == 0) {
        int scentSlot;
        const char* name = resolveScent(dComIfGs_getCollectSmell(), &scentSlot);
        char scentBuf[64];
        const bool has = scentSlot >= 0;
        drawCornerBox(x0, y0, x1, y1, txt(STR_SCENT), false);
        const ResTIMG* t = collectIconTimg(has ? scentSlot : 3);
        if (t != NULL) {
            drawTimg(t, ix, iy, icon, icon, has ? (u8)0xFF : (u8)55);
        }
        // Name hugs the icon rather than the box's bottom edge.
        // Localized scent names are far longer than the old short labels
        // ("Geruch von Medizin"), and this corner is only FN_CORNER wide.
        drawTextFittedCentered(cx, iy + icon + 8.0f, 10.0f, 6.5f, (x1 - x0) - 8.0f,
            has ? TEXT_MAIN : TEXT_DIM, trimScentWord(name, scentBuf,
                (int)sizeof(scentBuf)));
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
        drawCornerBox(x0, y0, x1, y1, localizedWord(0x0245, "Poe"),
            false);
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

    // Top-right / bottom-right: the I and II item slots — persistent
    // bindings that fire through the host button on tap ("extra buttons").
    // Boxes stay put in every state; empty or menu-hidden slots draw dim.
    // Rects are published even when empty so taps are swallowed rather than
    // falling through to the page beneath.
void drawFunctionalSlotCorners(dMeter2Draw_c* md, f32 rx0, f32 rx1, f32 ty0, f32 ty1,
    f32 by0, f32 by1) {
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

void drawFunctionalZCorner(dMeter2Draw_c* md, f32 lx0, f32 lx1, f32 by0, f32 by1) {
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
    // Before Midna joins the button is a plain empty plate — the letter still
    // names it (Z remains a real button for the camera and menu actions), but
    // there is no portrait to show, exactly as the transform box carries no
    // faces before the shadow crystal.
    if (midna != NULL && midnaAvailable()) {
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
    s_zBtnRect[3] = by1;}

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

    drawFunctionalSlotCorners(md, rx0, rx1, ty0, ty1, by0, by1);

    drawFunctionalZCorner(md, lx0, lx1, by0, by1);
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
}  // namespace dusk::companion
