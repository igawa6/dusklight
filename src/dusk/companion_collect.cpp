// Companion dashboard: COLLECT page — a lookalike of the pause menu's
// collection screen built from resident/collect-archive art. Five
// sub-tabs: the All overview (counters, gear boxes, heart-piece progress,
// Fused Shadows bar), Bugs, Fish, and the Skills / Mail readers (shared
// wrapped-body buffer, drag-scrolled).

#include "dusk/companion.h"
#include "dusk/companion_internal.h"
#include "dusk/companion_strings.h"
#include "dusk/dualscreen.h"

#include "d/d_com_inf_game.h"
#include "d/d_item_data.h"
#include "d/d_menu_collect.h"
#include "d/d_meter2_info.h"
#include "d/d_msg_out_font.h"
#include "dusk/version.hpp"

#include <cstdio>
#include <cstring>

namespace dusk::companion {
namespace {

// Box plate tint for the overview cells.
constexpr u32 CELL_RGBA = 0x22201DFFu;

int countBugs() {
    int bugs = 0;
    for (u8 itemNo = dItemNo_M_BEETLE_e; itemNo <= dItemNo_F_MAYFLY_e; itemNo++) {
        if (dComIfGs_isItemFirstBit(itemNo)) {
            bugs++;
        }
    }
    return bugs;
}

int countFishSpecies() {
    int n = 0;
    for (int i = 0; i < 6; i++) {
        if (dComIfGs_getFishNum(i) > 0) {
            n++;
        }
    }
    return n;
}

// The skills scroll art (ni_item_icon_makimono) is stored 32x80 — an
// upright scroll. The pause menu's pane shows it tilted diagonally
// (top-right to bottom-left); match it, at the art's own aspect, centered
// in a size x size slot.
void drawScrollIcon(f32 slotX, f32 slotY, f32 size, u8 alpha) {
    const ResTIMG* t = collectIconTimg(1);
    if (t == NULL) {
        return;
    }
    const f32 h = size;
    const f32 w = size * (f32)t->width / (f32)t->height;
    drawTimgRotatedRect(t, slotX + size * 0.5f, slotY + size * 0.5f, w, h, -40.0f, alpha);
}

int countSkills() {
    // F_0338..F_0344: one flag per learned technique — count set bits like
    // dMenu_Skill_c::getSkillNum (they are not cumulative markers).
    int n = 0;
    for (int i = 0; i < 7; i++) {
        if (dComIfGs_isEventBit(dSv_event_flag_c::saveBitLabels[338 + i])) {
            n++;
        }
    }
    return n;
}

// Rows 1-3: swords (2), shields (2), clothes (3) — tap to equip. The worn
// piece gets an accent underline. Publishes the box geometry (s_gearBox*)
// for the touch hit tests.
void drawGearBoxes(f32 x0, f32 y0, f32 gearS, f32 gap) {
    const u8 curSword = dComIfGs_getSelectEquipSword();
    const u8 curShield = dComIfGs_getSelectEquipShield();
    const u8 curClothes = dComIfGs_getSelectEquipClothes();
    s_gearBoxS = gearS;
    for (int i = 0; i < 7; i++) {
        int row, col;
        if (i < 2) {
            row = 0;
            col = i;
        } else if (i < 4) {
            row = 1;
            col = i - 2;
        } else {
            row = 2;
            col = i - 4;
        }
        const f32 bx = x0 + (f32)col * (gearS + gap);
        const f32 by = y0 + (f32)row * (gearS + gap);
        s_gearBoxX[i] = bx;
        s_gearBoxY[i] = by;
        const u8 itemNo = gearItemFor(i);
        const bool worn = itemNo == curSword || itemNo == curShield || itemNo == curClothes;
        // Worn gear gets the same bright-yellow box as a selected item.
        drawMenuBox(bx, by, bx + gearS, by + gearS, worn ? 0xECD054FFu : CELL_RGBA);
        drawItemIcon(ICON_SLOT_EQUIP0 + i, itemNo, bx + 3.0f, by + 3.0f, gearS - 6.0f,
            dComIfGs_isItemFirstBit(itemNo) ? 0xFF : 55);
    }
}

// Heart-piece progress beside the gear rows: the pause menu's own art —
// empty base plus one cumulative wedge overlay per collected piece, sized
// to fill the block.
void drawHeartPieceProgress(f32 leftEdge, f32 x1, f32 blockTop, f32 blockBottom, f32 hIcon) {
    const int pieces = dComIfGs_getMaxLife() % 5;
    // Nudged right of centre to sit past the gear column — but clamped, or
    // the offset crowds the window edge once the layout narrows (the
    // Functional content window is a good deal tighter than Cinematic's).
    constexpr f32 EDGE_PAD = 14.0f;
    f32 cx = (leftEdge + x1) * 0.5f + 25.0f;
    const f32 cxMax = x1 - EDGE_PAD - hIcon * 0.5f;
    if (cx > cxMax) {
        cx = cxMax;
    }
    const f32 hx = cx - hIcon * 0.5f;
    const f32 hy = (blockTop + blockBottom) * 0.5f - hIcon * 0.5f;
    // The game's own art (bilinear-filtered), so texture packs that replace
    // the heart pieces show here too.
    if (const ResTIMG* base = collectIconTimg(CLCT_SLOT_HEART_BASE)) {
        drawTimg(base, hx, hy, hIcon, hIcon, 0xFF);
        for (int i = 0; i < pieces && i < 4; i++) {
            if (const ResTIMG* wedge = collectIconTimg(CLCT_SLOT_HEART_PARTS1 + i)) {
                drawTimg(wedge, hx, hy, hIcon, hIcon, 0xFF);
            }
        }
    }
}

// Rows 4a/4b, three columns left-to-right: plain counters in the first
// (quiver over poe), then the four tappable library cells — bugs/fish in
// the middle, skills/mail on the right — which open their section as a
// detail view. The tappable cells publish s_collectIconRects
// ([0] bugs, [1] fish, [2] skills, [3] mail) for the touch pass.
void drawCounterGrid(f32 x0, f32 x1, f32 rowY, f32 rowH, f32 rowGap) {
    const f32 gap = 14.0f;
    const f32 cellW = (x1 - x0 - 2.0f * gap) / 3.0f;
    // Keep a real margin inside the cell: at 30 the taller art (quiver) sat
    // hard against the chamfered frame.
    const f32 icon = rowH - 18.0f;
    const int arrowMax = dComIfGs_getArrowMax();
    const int quiverIdx = RAWICON_YADUTU1 + (arrowMax >= 100 ? 2 : arrowMax >= 60 ? 1 : 0);
    for (int cell = 0; cell < 6; cell++) {
        const f32 cx = x0 + (f32)(cell % 3) * (cellW + gap);
        const f32 cy = rowY + (f32)(cell / 3) * (rowH + rowGap);
        drawMenuBox(cx, cy, cx + cellW, cy + rowH, CELL_RGBA);
        const f32 ix = cx + 8.0f;
        const f32 iy = cy + (rowH - icon) * 0.5f;
        f32 textPad = 8.0f;
        int rectIdx = -1;
        char text[16];
        text[0] = 0;
        switch (cell) {
        case 0: {  // quiver (counter)
            const ResTIMG* q = rawArchiveIcon(0, quiverIdx);
            const bool hasBow = dComIfGs_isItemFirstBit(dItemNo_BOW_e) != 0;
            if (q != NULL) {
                drawTimg(q, ix, iy, icon, icon, hasBow ? 0xFF : 55);
            }
            snprintf(text, sizeof(text), hasBow ? "%d" : "-", arrowMax);
            break;
        }
        case 1: {  // bugs (opens the grid)
            const int bugs = countBugs();
            drawItemIcon(ICON_SLOT_BUG0, dItemNo_M_BEETLE_e, ix, iy, icon,
                bugs > 0 ? 0xFF : 55);
            snprintf(text, sizeof(text), "%d/24", bugs);
            rectIdx = 0;
            break;
        }
        case 2: {  // skills (opens the reader)
            const int skills = countSkills();
            // Rotated 40 degrees, so its bounding box is taller than its
            // slot — shrink it or the corners clip the cell frame.
            const f32 scrollIcon = icon * 0.82f;
            drawScrollIcon(ix + (icon - scrollIcon) * 0.5f, iy + (icon - scrollIcon) * 0.5f,
                scrollIcon, skills > 0 ? 0xFF : 55);
            textPad = 12.0f;
            snprintf(text, sizeof(text), "%d/7", skills);
            rectIdx = 2;
            break;
        }
        case 3: {  // poe (counter)
            const int poes = dComIfGs_getPohSpiritNum();
            drawItemIcon(ICON_SLOT_POE, dItemNo_POU_SPIRIT_e, ix, iy, icon,
                poes > 0 ? 0xFF : 55);
            if (poes > 0) {
                snprintf(text, sizeof(text), "%d", poes);
            } else {
                snprintf(text, sizeof(text), "-");
            }
            break;
        }
        case 4: {  // fish (opens the journal)
            const int species = countFishSpecies();
            if (const ResTIMG* t = collectIconTimg(0)) {
                drawTimg(t, ix, iy, icon, icon, species > 0 ? 0xFF : 55);
            } else {
                drawItemIcon(ICON_SLOT_ROD, dItemNo_FISHING_ROD_1_e, ix, iy, icon,
                    species > 0 ? 0xFF : 55);
            }
            snprintf(text, sizeof(text), "%d/6", species);
            rectIdx = 1;
            break;
        }
        default: {  // mail (opens the reader)
            const int letters = (int)dMeter2Info_getRecieveLetterNum();
            if (const ResTIMG* t = collectIconTimg(2)) {
                drawTimg(t, ix, iy, icon, icon, letters > 0 ? 0xFF : 55);
            } else {
                drawItemIcon(ICON_SLOT_LETTER, dItemNo_TKS_LETTER_e, ix, iy, icon,
                    letters > 0 ? 0xFF : 55);
            }
            if (letters > 0) {
                snprintf(text, sizeof(text), "%d", letters);
            } else {
                snprintf(text, sizeof(text), "-");
            }
            rectIdx = 3;
            break;
        }
        }
        drawText(ix + icon + textPad, cy + rowH * 0.5f + 5.0f, 14.0f, TEXT_MAIN, "%s", text);
        // Not while the section zoom is travelling: cx/cy here are the
        // interpolated underlay geometry, so a second tap during the ~170ms
        // animation re-opened whichever cell had drifted under the finger.
        if (rectIdx >= 0 && !collectZoomActive()) {
            s_collectIconRects[rectIdx][0] = cx;
            s_collectIconRects[rectIdx][1] = cy;
            s_collectIconRects[rectIdx][2] = cx + cellW;
            s_collectIconRects[rectIdx][3] = cy + rowH;
        }
    }
}

// Row 5: half-width Scent panel and half-width Fused Shadows / Mirror
// Shards panel (name above its meter). The save tracks a 4th story
// crystal, so the count clamps to the collectible total.
void drawScentAndShadowRow(f32 x0, f32 x1, f32 rowY, f32 rowH) {
    const f32 gap = 14.0f;
    const f32 half = (x1 - x0 - gap) * 0.5f;

    // Scent panel.
    drawMenuBox(x0, rowY, x0 + half, rowY + rowH, CELL_RGBA);
    const u8 scent = dComIfGs_getCollectSmell();
    int scentSlot;
    const char* scentName = resolveScent(scent, &scentSlot);
    const f32 icon = 30.0f;
    const f32 iy = rowY + (rowH - icon) * 0.5f;
    if (const ResTIMG* t = scentSlot >= 0 ? collectIconTimg(scentSlot) : NULL) {
        drawTimg(t, x0 + 10.0f, iy, icon, icon, 0xFF);
    } else if (const ResTIMG* dim = collectIconTimg(3)) {
        drawTimg(dim, x0 + 10.0f, iy, icon, icon, 55);
    }
    const f32 scentX = x0 + 10.0f + icon + 6.0f;
    drawTextEllipsized(scentX, rowY + rowH * 0.5f + 5.0f, 14.0f,
        (x0 + half) - 6.0f - scentX, TEXT_MAIN, scentName);

    // Fused Shadows / Mirror Shards panel: name above the meter.
    const f32 fx0 = x0 + half + gap;
    drawMenuBox(fx0, rowY, x1, rowY + rowH, CELL_RGBA);
    const u8 maskMdl = dMenu_Collect3D_c::getMaskMdlVisible();
    const char* fsLabel = localizedWord(0x05AA, "Fused Shadows");
    int fsHave = 0;
    int fsTotal = 3;
    if (maskMdl == 2) {
        fsLabel = localizedWord(0x020A, "Mirror Shards");
        fsHave = dMenu_Collect3D_c::getMirrorNum();
        fsTotal = 4;
    } else if (maskMdl == 1) {
        fsHave = dMenu_Collect3D_c::getCrystalNum();
    }
    if (fsHave > fsTotal) {
        fsHave = fsTotal;
    }
    // Counter right-aligned by measurement, label shrunk if the two would
    // collide on a narrow window.
    char fsCount[8];
    if (maskMdl != 0) {
        snprintf(fsCount, sizeof(fsCount), "%d/%d", fsHave, fsTotal);
    } else {
        snprintf(fsCount, sizeof(fsCount), "-");
    }
    const f32 countX = x1 - 10.0f - measureText(13.0f, fsCount);
    const f32 labelMax = (countX - 8.0f) - (fx0 + 10.0f);
    const f32 labelTs = fittedTextSize(13.0f, 10.0f, labelMax, fsLabel);
    drawTextEllipsized(fx0 + 10.0f, rowY + 19.0f, labelTs, labelMax,
        maskMdl != 0 ? TEXT_MAIN : TEXT_DIM, fsLabel);
    drawText(countX, rowY + 19.0f, 13.0f, maskMdl != 0 ? TEXT_ACCENT : TEXT_DIM, "%s",
        fsCount);
    const f32 bx0 = fx0 + 10.0f;
    const f32 bx1 = x1 - 10.0f;
    constexpr GXColor COL_BAR_BG = {10, 9, 7, 210};
    fillRect(bx0, rowY + 26.0f, bx1, rowY + 38.0f, COL_BAR_BG);
    if (maskMdl != 0 && fsHave > 0) {
        constexpr GXColor COL_BAR_FILL = {235, 200, 90, 255};
        fillRect(bx0 + 1.0f, rowY + 27.0f,
            bx0 + 1.0f + (bx1 - bx0 - 2.0f) * (f32)fsHave / (f32)fsTotal, rowY + 37.0f,
            COL_BAR_FILL);
    }
}

void drawCollectOverview(f32 x0, f32 y0, f32 x1, f32 y1) {
    // Rows 1-3: swords / shields / clothes with the big heart beside them;
    // row 4: quiver, poe, skills, letters; row 5: Scent and Fused Shadows.
    // Compact enough that the equip feedback line below row 5 stays inside
    // the window.
    const f32 gearS = 50.0f;
    const f32 gap = 9.0f;
    const f32 gearX = x0 + 16.0f;  // breathing room off the window's left edge
    const f32 blockTop = y0 + 2.0f;
    const f32 blockBottom = blockTop + 3.0f * gearS + 2.0f * gap;
    drawGearBoxes(gearX, blockTop, gearS, gap);
    drawHeartPieceProgress(gearX + 2.0f * (gearS + gap), x1, blockTop, blockBottom, 132.0f);
    // Rows 4/5 sit inset from the window edges for breathing room.
    const f32 rowInset = 16.0f;
    const f32 row4 = blockBottom + 8.0f;
    drawCounterGrid(x0 + rowInset, x1 - rowInset, row4, 44.0f, 8.0f);
    const f32 row5 = row4 + 2.0f * 44.0f + 8.0f + 8.0f;
    drawScentAndShadowRow(x0 + rowInset, x1 - rowInset, row5, 44.0f);
    if (s_equipMsgFrames > 0) {
        constexpr u32 TEXT_WARN = 0xF0A050FF;
        // Centered between the scent row and the window bottom, starting at
        // the boxes' left edge.
        const f32 msgY = (row5 + 44.0f + y1) * 0.5f + 5.0f;
        drawText(x0 + rowInset, msgY, 14.0f, TEXT_WARN, "%s", s_equipMsg);
    }
}

f32 drawSectionHeader(f32 x0, f32 y0, f32 x1, f32 iconSize, f32 titleSize, const char* title,
    bool detail);
void sectionTitle(u32 msgId, const char* fallback, char* out, int cap, int have, int total);
const char* readerSectionName(int tab);

void drawCollectBugs(f32 x0, f32 y0, f32 x1) {
    const f32 tIcon = 26.0f;
    char title[64];
    sectionTitle(0x5BA, "Golden Bugs", title, sizeof(title), countBugs(), 24);
    const f32 iconX = drawSectionHeader(x0, y0, x1, tIcon, 15.0f, title, false);
    drawItemIcon(ICON_SLOT_BUG0, dItemNo_M_BEETLE_e, iconX, y0 + 2.0f, tIcon);
    y0 += tIcon + 14.0f;
    const int bugCols = 6;
    const f32 bugCell = (x1 - x0) / (f32)bugCols;
    f32 bugIcon = bugCell - 8.0f;
    if (bugIcon > 44.0f) {
        bugIcon = 44.0f;
    }
    for (int i = 0; i < 24; i++) {
        const u8 itemNo = (u8)(dItemNo_M_BEETLE_e + i);
        const f32 bx = x0 + (i % bugCols) * bugCell + (bugCell - bugIcon) * 0.5f;
        const f32 by = y0 + 4.0f + (i / bugCols) * (bugIcon + 6.0f);
        drawItemIcon(ICON_SLOT_BUG0 + i, itemNo, bx, by, bugIcon,
            dComIfGs_isItemFirstBit(itemNo) ? 0xFF : 55);
    }
}

void drawCollectFish(f32 x0, f32 y0, f32 x1) {
    const f32 rodIcon = 26.0f;
    char title[64];
    sectionTitle(0x5A1, "Fish Journal", title, sizeof(title), countFishSpecies(), 6);
    const f32 iconX = drawSectionHeader(x0, y0, x1, rodIcon, 15.0f, title, false);
    if (const ResTIMG* t = collectIconTimg(0)) {
        drawTimg(t, iconX, y0 + 2.0f, rodIcon, rodIcon, 0xFF);
    } else {
        drawItemIcon(ICON_SLOT_ROD, dItemNo_FISHING_ROD_1_e, iconX, y0 + 2.0f, rodIcon,
            dComIfGs_isItemFirstBit(dItemNo_FISHING_ROD_1_e) ? 0xFF : 55);
    }
    // Species names from the game's own message archive, so they follow the
    // language. Order is the SAVE-DATA index (the one getFishNum takes) and is
    // copied from d_menu_fishing.cpp's name_id[] — which pairs name_id[i] with
    // getFishNum(i). The old hardcoded table had a different order and so
    // mislabelled indices 0, 1 and 5.
    static const u16 l_fishMsg[6] = {0x59E, 0x59D, 0x59B, 0x599, 0x59A, 0x59C};
    // Table: Species | Caught | Record.
    const f32 availW = x1 - x0;
    const f32 colName = x0 + 10.0f;
    const f32 colCaught = x0 + availW * 0.58f;
    const f32 colRecord = x0 + availW * 0.80f;
    const f32 headY = y0 + rodIcon + 28.0f;
    drawText(colName, headY, 13.0f, TEXT_DIM, "%s", txt(STR_SPECIES));
    drawText(colCaught, headY, 13.0f, TEXT_DIM, "%s",
        archiveLabel(0x5A0, "Caught"));  // DE/FR/... = "No. Caught"
    drawText(colRecord, headY, 13.0f, TEXT_DIM, "%s",
        archiveLabel(0x59F, "Record"));  // DE/FR/... = "Largest"
    // The game reports the record in inches for English and centimetres for
    // every other language (d_menu_fishing.cpp:152); match it.
    const bool inches = dusk::version::isRegionPal() &&
        dComIfGs_getPalLanguage() == dSv_player_config_c::LANGUAGE_ENGLISH;
    for (int i = 0; i < 6; i++) {
        const f32 fy = headY + 26.0f + i * 26.0f;
        const u16 num = dComIfGs_getFishNum(i);
        const u32 col = num > 0 ? TEXT_MAIN : TEXT_DIM;
        // Clipped to the column: the localized names run much longer than the
        // English ones ("Ombre chevalier" vs "Greengill").
        drawTextEllipsized(colName, fy, 15.0f, colCaught - 8.0f - colName, col,
            archiveText(l_fishMsg[i], "-"));
        if (num > 0) {
            const s32 size = dComIfGs_getFishSize(i);
            drawText(colCaught, fy, 15.0f, col, "%d", num);
            drawText(colRecord, fy, 15.0f, col, "%d", inches ? (s32)(size / 2.54f) : size);
        } else {
            drawText(colCaught, fy, 15.0f, col, "-");
            drawText(colRecord, fy, 15.0f, col, "-");
        }
    }
}

// ---- Skills / Mail reader ------------------------------------------------

// Hidden-skill tables in the ougi menu's fixed display order
// (dMenu_Skill_c::read_open_init / getSkillNum).
constexpr u32 l_skillEvt[7] = {339, 338, 340, 341, 342, 343, 344};
constexpr u32 l_skillName[7] = {1709, 1708, 1710, 1711, 1712, 1713, 1714};
constexpr u32 l_skillText[7] = {1716, 1715, 1717, 1718, 1719, 1720, 1721};

bool skillLearned(int i) {
    return dComIfGs_isEventBit(dSv_event_flag_c::saveBitLabels[l_skillEvt[i]]) != 0;
}

void pushReaderRect(f32 x0, f32 y0, f32 x1, f32 y1, int id) {
    // Nothing is tappable while a detail is travelling. The list is still
    // DRAWN underneath (it pops down), so without this its rows publish live
    // rects: a tap where another row happens to sit mid-animation re-targeted
    // the reader and re-popped it from the shrunken underlay geometry. It also
    // stops the underlay's rows eating the 12-rect budget and silently
    // dropping the detail's own "< Back". The page transition guards the same
    // way via s_pageSliding.
    if (readerZoomActive() || collectZoomActive()) {
        return;
    }
    if (s_readerRectCount >= 12) {
        return;
    }
    s_readerRects[s_readerRectCount][0] = x0;
    s_readerRects[s_readerRectCount][1] = y0;
    s_readerRects[s_readerRectCount][2] = x1;
    s_readerRects[s_readerRectCount][3] = y1;
    s_readerRectIds[s_readerRectCount] = id;
    s_readerRectCount++;
}

// Shared header for every collect section and reader: the section identity
// on the RIGHT (title right-aligned immediately before its icon), and — in
// a DETAIL view only — a "< Back" plate on the LEFT that steps out to the
// entry list (id -4). List views carry no header button: the left-column
// context tab is the way home, and rows are opened by tapping them.
// Returns the icon's left edge; the caller draws its own icon art there.
// "<localized section name>  <have>/<total>". The count is appended rather
// than interpolated: the game's own title strings are bare nouns, and number
// placement varies by language, so a trailing count is the safe form.
// Falls back to the English literal if the archive has no entry (NTSC discs
// still return the English text, so this only guards a failed lookup).
void sectionTitle(u32 msgId, const char* fallback, char* out, int cap, int have, int total) {
    snprintf(out, cap, "%s  %d/%d", archiveText(msgId, fallback), have, total);
}

// Section identity shown in a reader's header (tab 3 = skills, 4 = mail).
const char* readerSectionName(int tab) {
    if (tab == 3) {
        return archiveText(0x6A4, "Hidden Skills");
    }
    // 0x4D6 is a "Letter <n>/<n>" reader caption driven by
    // setMessageCountNumber and does not fit a section label; 0x04C8
    // ("Letters") is the plain noun the game's own letter screen uses.
    return localizedWord(0x04C8, "Mail");
}

f32 drawSectionHeader(f32 x0, f32 y0, f32 x1, f32 iconSize, f32 titleSize, const char* title,
    bool detail) {
    // "< Back" plate geometry, shared by the plate itself and by the title's
    // left limit below.
    constexpr f32 BACK_PLATE_X = 2.0f;
    constexpr f32 BACK_PLATE_W = 78.0f;
    if (detail) {
        const f32 bh = iconSize >= 24.0f ? 26.0f : 22.0f;
        // Selected-plate style (light parchment + dark ink), matching the
        // active context tab — it is the primary action in a detail view.
        drawTabPlate(x0 + BACK_PLATE_X, y0 + 1.0f, BACK_PLATE_W, bh, true);

        char back[48];
        snprintf(back, sizeof(back), "< %s",
            localizedWord(0x0054, "Back"));
        drawTextFittedCentered(x0 + BACK_PLATE_X + BACK_PLATE_W * 0.5f,
            y0 + 1.0f + bh * 0.5f + 5.0f, 13.0f, 8.0f, BACK_PLATE_W - 8.0f, TEXT_TAB_ACTIVE, back);
        pushReaderRect(x0 + BACK_PLATE_X, y0 + 1.0f, x0 + BACK_PLATE_X + BACK_PLATE_W,
            y0 + 1.0f + bh, -4);
    }
    const f32 iconX = x1 - 6.0f - iconSize;
    // Right-aligned, growing leftward — so it has to stop before the "< Back"
    // plate in a detail view, and before the window edge otherwise. German
    // section names ("Verborgene F\xE4higkeiten  3/7") overrun both.
    // Derived from the Back plate, not hand-tuned to match it: the two were
    // 78 and 88 in separate scopes, so widening the plate would have slid the
    // right-aligned title silently underneath it.
    const f32 leftLimit = x0 + (detail ? BACK_PLATE_X + BACK_PLATE_W + 8.0f : 6.0f);
    const f32 titleMax = (iconX - 8.0f) - leftLimit;
    const f32 ts = fittedTextSize(titleSize, titleSize - 3.0f, titleMax, title);
    const f32 tw = measureText(ts, title);
    const f32 titleY = y0 + (iconSize >= 24.0f ? 20.0f : 15.0f);
    if (tw > titleMax) {
        drawTextEllipsized(leftLimit, titleY, ts, titleMax, TEXT_DIM, title);
    } else {
        drawText(iconX - 8.0f - tw, titleY, ts, TEXT_DIM, "%s", title);
    }
    return iconX;
}

// --- Inline message icons -------------------------------------------------
// getStringFull emits 0x02 followed by (outfont index + 1) for the
// controller-button tags. The textures are the message system's own
// font_XX.bti icons from the resident Main2D archive, tinted like
// COutFont_c::createPane.
struct MsgTint {
    u32 black;
    u32 white;
};

// Tints for the controller-button block, matching COutFont_c::createPane.
// Everything past it (targets, heart, quaver, bullets, ...) draws untinted,
// which is what createPane does for those indices too.
constexpr MsgTint l_msgTints[10] = {
    {0xFFFFFF00u, 0x62A32EFFu},  // A
    {0xFFFFFF00u, 0xC82727FFu},  // B
    {0x00000000u, 0xFFC832FFu},  // C-stick
    {0x00000000u, 0xC8C8C8FFu},  // L
    {0x00000000u, 0xC8C8C8FFu},  // R
    {0x00000000u, 0xC8C8C8FFu},  // X
    {0x00000000u, 0xC8C8C8FFu},  // Y
    {0xFFFFFF00u, 0x5046A5FFu},  // Z
    {0x00000000u, 0xC8C8C8FFu},  // D-pad
    {0x00000000u, 0xFFFFFFFFu},  // control stick
};
constexpr int MSG_ICON_MAX = 70;  // COutFont_c::getBtiName table size
constexpr int OUTFONT_IDX_BOMB_BAG = 41;  // lives in the item-icon archive
constexpr f32 MSG_ICON_W = 20.0f;

MsgTint msgIconTint(int idx) {
    if (idx >= 0 && idx < 10) {
        return l_msgTints[idx];
    }
    return {0x00000000u, 0xFFFFFFFFu};
}

// Names come straight from the game's own table so this can't drift: the
// hand-copied 20-entry version silently dropped every icon past the button
// block (lock-on reticles, hearts, bullets).
const ResTIMG* msgIconTimg(int idx) {
    if (idx < 0 || idx >= MSG_ICON_MAX) {
        return NULL;
    }
    const char* bti = COutFont_c::getBtiName(idx);
    if (bti == NULL || bti[0] == 0) {
        return NULL;
    }
    // createPane pulls index 41 (the bomb-bag icon) from the item-icon
    // archive; everything else, index 30 included, comes from Main2D.
    JKRArchive* arc = idx == OUTFONT_IDX_BOMB_BAG ? dComIfGp_getItemIconArchive()
                                                  : dComIfGp_getMain2DArchive();
    if (arc == NULL) {
        return NULL;
    }
    return (const ResTIMG*)arc->getResource('TIMG', bti);
}

// Width of a body segment: text runs measured, icon markers at fixed width.
f32 bodyTextWidth(const char* s, f32 ts) {
    f32 w = 0.0f;
    char run[128];
    int rl = 0;
    for (const char* p = s; ; p++) {
        if (*p == 0x02 && p[1] != 0) {
            if (rl > 0) {
                run[rl] = 0;
                w += measureText(ts, run);
                rl = 0;
            }
            w += MSG_ICON_W;
            p++;
            continue;
        }
        if (*p == 0) {
            break;
        }
        if (rl < 127) {
            run[rl++] = *p;
        }
    }
    if (rl > 0) {
        run[rl] = 0;
        w += measureText(ts, run);
    }
    return w;
}

// Draw a body line at (x, y-baseline): text runs plus inline icons.
void drawBodyText(f32 x, f32 y, f32 ts, u32 rgba, const char* s) {
    char run[128];
    int rl = 0;
    for (const char* p = s; ; p++) {
        if (*p == 0x02 && p[1] != 0) {
            if (rl > 0) {
                run[rl] = 0;
                drawText(x, y, ts, rgba, "%s", run);
                x += measureText(ts, run);
                rl = 0;
            }
            const int idx = (u8)p[1] - 1;
            const MsgTint tint = msgIconTint(idx);
            if (const ResTIMG* icon = msgIconTimg(idx)) {
                drawTimgTinted(icon, x + 1.0f, y - ts - 1.0f, MSG_ICON_W - 2.0f,
                    MSG_ICON_W - 2.0f, 0xFF, tint.black, tint.white);
            }
            x += MSG_ICON_W;
            p++;
            continue;
        }
        if (*p == 0) {
            break;
        }
        if (rl < 127) {
            run[rl++] = *p;
        }
    }
    if (rl > 0) {
        run[rl] = 0;
        drawText(x, y, ts, rgba, "%s", run);
    }
}

// Open entry's title/context + word-wrapped body, refilled when the
// selection changes (message walks are too heavy for per-frame).
char s_readerTitle[96];
char s_readerCorner[96];
// One bound for the whole wrap pipeline: the join buffer, the line being
// built, and the stored line. They must match or a long paragraph truncates
// silently instead of wrapping.
constexpr int BODY_LINE_MAX = 228;
char s_bodyLines[64][BODY_LINE_MAX];
// Left offset per line, so a bullet item's wrapped continuations hang under
// its text instead of under the bullet itself.
f32 s_bodyIndent[64];
int s_bodyLineCount = 0;
int s_fetchedTab = -1;
int s_fetchedSel = -2;

void pushBodyLine(const char* line, f32 indent) {
    if (s_bodyLineCount < 64) {
        snprintf(s_bodyLines[s_bodyLineCount], sizeof(s_bodyLines[0]), "%s", line);
        s_bodyIndent[s_bodyLineCount] = indent;
        s_bodyLineCount++;
    }
}

// Inline marker for MSGTAG_BULLET / BULLET_SPACE — outfont 42/43, stored as
// index + 1 by getStringFull.
bool isBulletMarker(const char* p) {
    return *p == 0x02 && ((u8)p[1] == 43 || (u8)p[1] == 44);
}

u32 s_bodyGen = 0;

void wrapBody(const char* body, f32 width, f32 ts) {
    s_bodyGen++;
    s_bodyLineCount = 0;
    char cur[BODY_LINE_MAX];
    cur[0] = 0;
    const char* p = body;
    bool lastBlank = false;
    // A bullet item's continuations hang under its text. curIndent applies to
    // the line being built; hangIndent is what the next wrapped line inherits.
    f32 curIndent = 0.0f;
    f32 hangIndent = 0.0f;
    bool newPara = true;
    while (*p != 0 && s_bodyLineCount < 64) {
        if (*p == '\n') {
            // A blank line is a deliberate paragraph break, and it has to be
            // read off the SOURCE. Inferring it from `cur` only worked while
            // every newline flushed the line; now that they are swallowed
            // `cur` is still populated, and the break was being eaten.
            const char* scan = p + 1;
            while (*scan == ' ') {
                scan++;
            }
            if (*scan == '\n') {
                // Finish the paragraph, then emit one separator (runs collapse).
                if (cur[0] != 0) {
                    pushBodyLine(cur, curIndent);
                    cur[0] = 0;
                    lastBlank = false;
                }
                if (!lastBlank) {
                    pushBodyLine("", 0.0f);
                    lastBlank = true;
                }
                curIndent = 0.0f;
                hangIndent = 0.0f;
                newPara = true;
                p++;
                continue;
            }
            // The source strings are hard-wrapped for the game's dialog box,
            // which is far narrower than this column, so obeying every break
            // leaves ragged half-lines mid-sentence. Only break where the
            // line actually ends a sentence; otherwise swallow the newline
            // and let the wrapper decide, which is what the space-joining
            // path below does for the next word.
            const char* tail = cur;
            char last = 0;
            while (*tail != 0) {
                if (*tail != ' ') {
                    last = *tail;
                }
                tail++;
            }
            const char* peek = p + 1;
            while (*peek == ' ') {
                peek++;
            }
            const bool sentenceEnd = last == '.' || last == '!' || last == '?';
            // Only a following bullet forces the break. Breaking on every
            // newline while inside an item was wrong: a bullet anywhere in a
            // description made the rest of it break at each source line, so
            // running text split mid-sentence ("almost" / "anything"). The
            // hanging indent already carries the item's shape; the text
            // itself should reflow.
            if (isBulletMarker(peek) || sentenceEnd) {
                pushBodyLine(cur, curIndent);
                cur[0] = 0;
                curIndent = 0.0f;
                hangIndent = 0.0f;
                newPara = true;
                lastBlank = false;
            }
            p++;
            continue;
        }
        lastBlank = false;
        char word[96];
        int wl = 0;
        while (*p != 0 && *p != ' ' && *p != '\n' && wl < 94) {
            if (*p == 0x02 && p[1] != 0) {
                // Icon marker: keep the pair intact inside the word.
                word[wl++] = *p++;
                word[wl++] = *p++;
                continue;
            }
            word[wl++] = (u8)*p < 0x20 ? ' ' : *p;
            p++;
        }
        word[wl] = 0;
        while (*p == ' ') {
            p++;
        }
        if (wl == 0) {
            // Nothing scanned, so the cursor was sitting on whitespace: the
            // source indents lines for the game's centred dialog box, and
            // since a swallowed newline leaves that indentation in place,
            // every one of those spaces would otherwise append an empty word
            // — i.e. a stray space each — to the line being built.
            continue;
        }
        if (newPara) {
            // First word of an item decides whether it hangs.
            hangIndent = isBulletMarker(word) ? MSG_ICON_W : 0.0f;
            curIndent = 0.0f;
            newPara = false;
        }
        char cand[BODY_LINE_MAX];
        if (cur[0] != 0) {
            snprintf(cand, sizeof(cand), "%s %s", cur, word);
        } else {
            snprintf(cand, sizeof(cand), "%s", word);
        }
        if (cur[0] != 0 && bodyTextWidth(cand, ts) > width - curIndent) {
            pushBodyLine(cur, curIndent);
            curIndent = hangIndent;
            snprintf(cur, sizeof(cur), "%s", word);
        } else {
            snprintf(cur, sizeof(cur), "%s", cand);
        }
    }
    if (cur[0] != 0) {
        pushBodyLine(cur, curIndent);
    }
}

// Ordinal labels for the skills rows and reader header.
const char* skillOrdinal(int i) {
    // The game's own ordinals ("Skill One"..."Last Skill"), msg 1701 + i — same
    // order as l_skillName, verified against d_menu_skill.cpp:634.
    static const char* const l_fallback[7] = {"Skill One", "Skill Two", "Skill Three",
        "Skill Four", "Skill Five", "Skill Six", "Last Skill"};
    if (i < 0 || i >= 7) {
        return "";
    }
    // The old version latched `loaded = true` before fetching and returned the
    // buffer raw, so a single draw before the archive was resident left all
    // seven labels blank for the rest of the session. archiveText falls back to
    // real text instead of an empty string.
    return archiveText(1701 + i, l_fallback[i]);
}

void ensureReaderContent(int tab, f32 width) {
    if (s_fetchedTab == tab && s_fetchedSel == s_readerSel) {
        return;
    }
    s_fetchedTab = tab;
    s_fetchedSel = s_readerSel;
    s_readerTitle[0] = 0;
    s_readerCorner[0] = 0;
    s_bodyLineCount = 0;
    if (s_readerSel < 0) {
        return;
    }
    static char body[4096];
    body[0] = 0;
    if (tab == 3) {
        // Header: "Skill One" left, the technique's name right.
        snprintf(s_readerTitle, sizeof(s_readerTitle), "%s", skillOrdinal(s_readerSel));
        dMeter2Info_getStringFull(l_skillName[s_readerSel], s_readerCorner,
            sizeof(s_readerCorner));
        dMeter2Info_getStringFull(l_skillText[s_readerSel], body, sizeof(body));
    } else {
        // Header: subject left, the sender right.
        dMeter2Info_getStringFull(dMenu_Letter::getLetterSubject(s_readerSel), s_readerTitle,
            sizeof(s_readerTitle));
        char sender[64];
        sender[0] = 0;
        dMeter2Info_getStringFull(dMenu_Letter::getLetterName(s_readerSel), sender,
            sizeof(sender));
        if (sender[0] != 0) {
            snprintf(s_readerCorner, sizeof(s_readerCorner), "%s", sender);
        }
        dMeter2Info_getStringFull(dMenu_Letter::getLetterText(s_readerSel), body, sizeof(body));
    }
    wrapBody(body, width, 14.0f);
}

// Detail view shared by both tabs: header line, boxed body that scrolls by
// drag (clamped here), Back plate.
void drawReaderDetail(int tab, f32 x0, f32 y0, f32 x1, f32 y1) {
    // Wrap against the FULL width before any zoom, so the animation never
    // rewraps the body (that would reflow the text every frame).
    ensureReaderContent(tab, x1 - x0 - 36.0f);
    // Pop out of / back into the row this entry lives on.
    if (!readerZoomStep(&x0, &y0, &x1, &y1)) {
        s_readerSel = -1;
        s_scrollBody = 0.0f;
        return;
    }
    // Header: "< Collect" jumps straight home even from the reader; the
    // section identity sits on the right so it never fights the shortcut.
    const f32 hIcon = 20.0f;
    const f32 iconX =
        drawSectionHeader(x0, y0, x1, hIcon, 12.0f, readerSectionName(tab), true);
    if (tab == 3) {
        drawScrollIcon(iconX, y0, hIcon, 0xFF);
    } else if (const ResTIMG* t = collectIconTimg(2)) {
        drawTimg(t, iconX, y0, hIcon, hIcon, 0xFF);
    }
    y0 += hIcon + 8.0f;
    drawText(x0 + 4.0f, y0 + 16.0f, 16.0f, TEXT_ACCENT, "%s", s_readerTitle);
    if (s_readerCorner[0] != 0) {
        const f32 cw = measureText(14.0f, s_readerCorner);
        drawText(x1 - cw - 6.0f, y0 + 16.0f, 14.0f, TEXT_MAIN, "%s", s_readerCorner);
    }
    // The box fills down to the window bottom; Back lives inside it,
    // bottom right, with the text region stopping above it.
    const f32 by0 = y0 + 26.0f;
    const f32 by1 = y1 - 2.0f;
    drawDetailBox(x0, by0, x1, by1);
    const f32 textBottom = readerTextBottom(by1);
    const f32 lineH = 21.0f;
    const f32 viewH = textBottom - by0 - 12.0f;
    const f32 contentH = (f32)s_bodyLineCount * lineH;
    const f32 maxScroll = clampListScroll(&s_scrollBody, contentH, viewH);
    if (s_nativeW != 0) {
        setWinScissor(x0, by0 + 6.0f, x1, textBottom);
    }
    for (int i = 0; i < s_bodyLineCount; i++) {
        const f32 ly = by0 + 22.0f + (f32)i * lineH - s_scrollBody;
        if (ly < by0 - lineH || ly > textBottom + lineH) {
            continue;
        }
        drawBodyText(x0 + 16.0f + s_bodyIndent[i], ly, 14.0f, TEXT_MAIN, s_bodyLines[i]);
    }
    if (s_nativeW != 0) {
        applyWinClip();
    }
    // Scroll hint bar just inside the box edge, clear of the Back row.
    drawListScrollHint(x1 - 4.0f, by0 + 4.0f, textBottom - 4.0f, s_scrollBody, maxScroll,
        viewH, contentH);
}

// Skills tab: all seven techniques, three columns — scroll icon, ordinal,
// technique name (??? until learned). Tap a learned row to read it.
// The list itself. Split from drawSkillsContent so it can be drawn UNDERNEATH a
// detail that is still travelling — see below.
void drawSkillsList(f32 x0, f32 y0, f32 x1, f32 y1);

// Composes list + detail. While the detail is travelling the list stays
// underneath and POPS DOWN — fading and easing back slightly — so the view
// being replaced animates out instead of vanishing the instant a row is
// tapped, and the window is never left empty mid-animation.
void drawSkillsContent(f32 x0, f32 y0, f32 x1, f32 y1) {
    if (s_readerSel >= 0) {
        if (readerZoomActive()) {
            const f32 t = readerZoomProgress();
            const f32 shrink = 0.06f * t;
            const f32 ox = (x1 - x0) * shrink * 0.5f;
            const f32 oy = (y1 - y0) * shrink * 0.5f;
            // Multiply, never assign: this can run INSIDE a page transition
            // that is already fading the whole page. Assigning stomped that
            // outer fade, so the outgoing page stayed opaque and then snapped.
            const f32 prevA = s_drawAlpha;
            s_drawAlpha = prevA * (1.0f - t);
            drawSkillsList(x0 + ox, y0 + oy, x1 - ox, y1 - oy);
            s_drawAlpha = prevA;
        }
        drawReaderDetail(3, x0, y0, x1, y1);
        return;
    }
    drawSkillsList(x0, y0, x1, y1);
}

void drawSkillsList(f32 x0, f32 y0, f32 x1, f32 y1) {
    // Section header, matching the bugs/fish pages.
    const f32 hIcon = 26.0f;
    char title[64];
    sectionTitle(0x6A4, "Hidden Skills", title, sizeof(title), countSkills(), 7);
    const f32 iconX = drawSectionHeader(x0, y0, x1, hIcon, 15.0f, title, false);
    drawScrollIcon(iconX, y0 + 2.0f, hIcon, 0xFF);
    y0 += hIcon + 14.0f;
    static char names[7][64];
    static bool namesLoaded = false;
    if (!namesLoaded) {
        // Latch only once every name actually came back. Setting the flag
        // BEFORE the fetch meant a single draw before the archive was resident
        // left all seven technique names blank for the rest of the session —
        // the same trap skillOrdinal above was rewritten to avoid.
        bool all = true;
        for (int i = 0; i < 7; i++) {
            names[i][0] = 0;
            dMeter2Info_getStringFull(l_skillName[i], names[i], sizeof(names[0]));
            if (names[i][0] == 0) {
                all = false;
            }
        }
        namesLoaded = all;
    }
    const f32 rowH = 44.0f;
    const f32 gap = 8.0f;
    const f32 viewH = y1 - y0;
    const f32 contentH = 7.0f * rowH + 6.0f * gap;
    const f32 maxScroll = clampListScroll(&s_scrollSkills, contentH, viewH);
    if (s_nativeW != 0) {
        setWinScissor(x0, y0, x1, y0 + viewH);
    }
    // Rows stop short of the right edge so the scroll hint has its own lane.
    const f32 rx1 = x1 - 12.0f;
    for (int i = 0; i < 7; i++) {
        const f32 ry = y0 + (f32)i * (rowH + gap) - s_scrollSkills;
        if (ry < y0 - rowH || ry > y1) {
            continue;
        }
        drawMenuBox(x0, ry, rx1, ry + rowH, i == s_collectSel ? 0x5A4A2AFFu : CELL_RGBA);
        const bool got = skillLearned(i);
        drawScrollIcon(x0 + 12.0f, ry + (rowH - 30.0f) * 0.5f, 30.0f, got ? 0xFF : 55);
        // Two fixed columns: the ordinal, then the technique name.
        constexpr f32 ORD_X = 54.0f;
        constexpr f32 NAME_X = 210.0f;
        drawTextEllipsized(x0 + ORD_X, ry + rowH * 0.5f + 5.0f, 15.0f,
            (NAME_X - ORD_X) - 6.0f, got ? TEXT_MAIN : TEXT_DIM, skillOrdinal(i));
        if (got) {
            drawTextEllipsized(x0 + NAME_X, ry + rowH * 0.5f + 5.0f, 15.0f,
                (rx1 - 34.0f) - (x0 + NAME_X), TEXT_ACCENT, names[i]);
            drawText(rx1 - 26.0f, ry + rowH * 0.5f + 5.0f, 15.0f, TEXT_DIM, ">");
            // Publish only the visible part: rows scrolled past the viewport
            // edge must not be tappable through the sub-tab strip / margins.
            const f32 vy0 = ry > y0 ? ry : y0;
            const f32 vy1 = ry + rowH < y1 ? ry + rowH : y1;
            if (vy1 - vy0 > 12.0f) {
                pushReaderRect(x0, vy0, rx1, vy1, i);
            }
        } else {
            drawText(x0 + NAME_X, ry + rowH * 0.5f + 5.0f, 15.0f, TEXT_DIM, "???");
        }
    }
    if (s_nativeW != 0) {
        applyWinClip();
    }
    drawListScrollHint(x1, y0, y1, s_scrollSkills, maxScroll, viewH, contentH);
}

// Received letters, newest first — the pause menu's order (it reads the
// save's receive-order table backwards).
int sortedLetters(int* o_idxs) {
    const int n = (int)dMeter2Info_getRecieveLetterNum();
    int count = 0;
    for (int i = 0; i < n && count < 64; i++) {
        const int v = (int)dComIfGs_getGetNumber(n - i - 1);
        // The RANGE matters, not just the count: v comes from save data and
        // every consumer uses the returned value to index 64-entry caches
        // (subj/from/cached/tries/wait in drawLettersContent). Bounding the
        // loop alone left a corrupt or unexpected order table writing past
        // them. The fallback below is index-generated and always in range.
        if (v > 0 && v - 1 < 64) {
            o_idxs[count++] = v - 1;
        }
    }
    if (count == 0) {
        // Older saves without the order table: raw flag order.
        for (int i = 0; i < 64 && count < 64; i++) {
            if (dComIfGs_isLetterGetFlag(i)) {
                o_idxs[count++] = i;
            }
        }
    }
    return count;
}

// Mail tab: three columns — letter icon, subject, sender. Newest first,
// scrolls by drag. Tap to read.
// The list itself. Split from drawLettersContent so it can be drawn UNDERNEATH a
// detail that is still travelling — see below.
void drawLettersList(f32 x0, f32 y0, f32 x1, f32 y1);

// Composes list + detail. While the detail is travelling the list stays
// underneath and POPS DOWN — fading and easing back slightly — so the view
// being replaced animates out instead of vanishing the instant a row is
// tapped, and the window is never left empty mid-animation.
void drawLettersContent(f32 x0, f32 y0, f32 x1, f32 y1) {
    if (s_readerSel >= 0) {
        if (readerZoomActive()) {
            const f32 t = readerZoomProgress();
            const f32 shrink = 0.06f * t;
            const f32 ox = (x1 - x0) * shrink * 0.5f;
            const f32 oy = (y1 - y0) * shrink * 0.5f;
            // Multiply, never assign: this can run INSIDE a page transition
            // that is already fading the whole page. Assigning stomped that
            // outer fade, so the outgoing page stayed opaque and then snapped.
            const f32 prevA = s_drawAlpha;
            s_drawAlpha = prevA * (1.0f - t);
            drawLettersList(x0 + ox, y0 + oy, x1 - ox, y1 - oy);
            s_drawAlpha = prevA;
        }
        drawReaderDetail(4, x0, y0, x1, y1);
        return;
    }
    drawLettersList(x0, y0, x1, y1);
}

void drawLettersList(f32 x0, f32 y0, f32 x1, f32 y1) {
    // Section header, matching the bugs/fish pages.
    const f32 hIcon = 26.0f;
    char title[64];
    {
        snprintf(title, sizeof(title), "%s  %d", readerSectionName(4),
            (int)dMeter2Info_getRecieveLetterNum());
    }
    const f32 iconX = drawSectionHeader(x0, y0, x1, hIcon, 15.0f, title, false);
    if (const ResTIMG* t = collectIconTimg(2)) {
        drawTimg(t, iconX, y0 + 2.0f, hIcon, hIcon, 0xFF);
    }
    y0 += hIcon + 14.0f;
    int idxs[64];
    const int n = sortedLetters(idxs);
    if (n == 0) {
        drawTextCentered((x0 + x1) * 0.5f, (y0 + y1) * 0.5f, 15.0f, TEXT_DIM,
            txt(STR_NO_LETTERS));
        return;
    }
    const f32 rowH = 40.0f;
    const f32 gap = 6.0f;
    const f32 viewH = y1 - y0;
    const f32 contentH = (f32)n * rowH + (f32)(n - 1) * gap;
    const f32 maxScroll = clampListScroll(&s_scrollMail, contentH, viewH);
    // Subject/sender caches keyed by letter index (message walks are heavy).
    static char subj[64][80];
    static char from[64][48];
    static bool cached[64];
    static u8 tries[64];
    static u8 wait[64];
    static int cachedCount = -1;
    static u32 cachedGen = 0;
    if (cachedCount != n || cachedGen != s_lettersCacheGen) {
        cachedCount = n;
        cachedGen = s_lettersCacheGen;
        memset(cached, 0, sizeof(cached));
        memset(tries, 0, sizeof(tries));
        memset(wait, 0, sizeof(wait));
    }
    if (s_nativeW != 0) {
        setWinScissor(x0, y0, x1, y0 + viewH);
    }
    const ResTIMG* mailIcon = collectIconTimg(2);
    // Rows stop short of the right edge so the scroll hint has its own lane.
    const f32 rx1 = x1 - 12.0f;
    for (int i = 0; i < n; i++) {
        const f32 ry = y0 + (f32)i * (rowH + gap) - s_scrollMail;
        if (ry < y0 - rowH || ry > y1) {
            continue;
        }
        const int li = idxs[i];
        // Latch on SUCCESS, not on attempt. dMeter2Info_c::getStringFull returns
        // an EMPTY buffer when the message resource is not resident yet — which
        // it routinely is not for the first frames after a stage load — and
        // marking the row cached before looking meant a row first drawn in that
        // window stayed blank for the rest of the session.
        //
        // Bounded, though: a legitimately empty subject would otherwise rescan
        // the whole .bmg every frame forever. Same shape as the interning in
        // companion_gfx.cpp, which exists because three sites had that bug.
        if (!cached[li]) {
            constexpr u8 FETCH_TRIES = 8;
            constexpr u8 RETRY_GAP = 20;  // draws between attempts
            if (wait[li] > 0) {
                wait[li]--;
            } else {
                subj[li][0] = 0;
                from[li][0] = 0;
                dMeter2Info_getStringFull(dMenu_Letter::getLetterSubject(li), subj[li],
                    sizeof(subj[0]));
                char sender[40];
                sender[0] = 0;
                dMeter2Info_getStringFull(dMenu_Letter::getLetterName(li), sender,
                    sizeof(sender));
                if (sender[0] != 0) {
                    snprintf(from[li], sizeof(from[0]), "%s", sender);
                }
                if (subj[li][0] != 0 || ++tries[li] >= FETCH_TRIES) {
                    cached[li] = true;  // got it, or gave up
                } else {
                    wait[li] = RETRY_GAP;
                }
            }
        }
        drawMenuBox(x0, ry, rx1, ry + rowH, li == s_collectSel ? 0x5A4A2AFFu : CELL_RGBA);
        if (mailIcon != NULL) {
            drawTimg(mailIcon, x0 + 12.0f, ry + (rowH - 28.0f) * 0.5f, 28.0f, 28.0f, 0xFF);
        }
        // Sender right-aligned, subject taking the rest. The sender is capped
        // at 40% of the text lane: unclamped, a long localized name drove the
        // subject's width negative and the subject disappeared from the row.
        const f32 textX = x0 + 52.0f;
        const f32 laneW = (rx1 - 30.0f) - textX;
        f32 senderW = 0.0f;
        if (from[li][0] != 0) {
            senderW = measureText(13.0f, from[li]);
            const f32 senderMax = laneW * 0.40f;
            if (senderW > senderMax) {
                senderW = senderMax;
            }
            drawTextEllipsized(rx1 - 30.0f - senderW, ry + rowH * 0.5f + 5.0f, 13.0f, senderW,
                TEXT_DIM, from[li]);
            senderW += 12.0f;
        }
        drawTextEllipsized(textX, ry + rowH * 0.5f + 5.0f, 14.0f, laneW - senderW, TEXT_MAIN,
            subj[li]);
        drawText(rx1 - 20.0f, ry + rowH * 0.5f + 5.0f, 15.0f, TEXT_DIM, ">");
        // Publish only the visible part (see the skills rows).
        const f32 vy0 = ry > y0 ? ry : y0;
        const f32 vy1 = ry + rowH < y1 ? ry + rowH : y1;
        if (vy1 - vy0 > 12.0f) {
            pushReaderRect(x0, vy0, rx1, vy1, li);
        }
    }
    if (s_nativeW != 0) {
        applyWinClip();
    }
    drawListScrollHint(x1, y0, y1, s_scrollMail, maxScroll, viewH, contentH);
}


}  // namespace

// Scent icon slot + display name. External: the wolf form's slot I corner
// readout uses it too.
const char* resolveScent(u8 scent, int* o_iconSlot) {
    *o_iconSlot = -1;
    // The scents ARE ordinary items, so their names live in the message
    // archive at 0x165 + itemNo like any other — verified on the PAL disc
    // ("Medicine Scent" / "Geruch von Medizin").
    int slot = -1;
    u8 nameItem = scent;
    switch (scent) {
    case dItemNo_SMELL_MEDICINE_e:
        slot = 3;
        break;
    case dItemNo_SMELL_CHILDREN_e:
        slot = 4;
        break;
    case dItemNo_SMELL_FISH_e:
        slot = 5;
        break;
    case dItemNo_SMELL_YELIA_POUCH_e:
    case dItemNo_SMELL_PUMPKIN_e:
        slot = 6;
        // Both pumpkin and pouch are Ilia's trail; the pouch carries the name.
        nameItem = dItemNo_SMELL_YELIA_POUCH_e;
        break;
    case dItemNo_SMELL_POH_e:
        slot = 7;
        break;
    default:
        return "-";
    }
    *o_iconSlot = slot;
    // Interned per message ID, so this is one archive scan per scent for the
    // whole session — it sits on the wolf HUD's per-frame path.
    return archiveText(0x165 + nameItem, "-");
}

// Reader body helpers shared with the ITEMS page's item-info view: fill the
// shared wrapped-line buffer, draw from it, and invalidate the collect
// reader's cache (the buffer is single-instance).
void readerWrapBody(const char* body, f32 width, f32 ts) {
    wrapBody(body, width, ts);
}

int readerBodyLineCount() {
    return s_bodyLineCount;
}

void readerDrawBodyLine(int idx, f32 x, f32 y, f32 ts, u32 rgba) {
    if (idx >= 0 && idx < s_bodyLineCount) {
        drawBodyText(x + s_bodyIndent[idx], y, ts, rgba, s_bodyLines[idx]);
    }
}

void readerInvalidate() {
    s_fetchedSel = -2;
}

// drawLettersContent's subject/sender caches are keyed on letter COUNT alone,
// so they survive a file change whenever the two files hold the same number of
// letters. This is the hook that drops them; the statics themselves live in
// that function, so the count sentinel is what gets poisoned.
void lettersInvalidate() {
    s_lettersCacheGen++;
}

u32 readerBodyGen() {
    return s_bodyGen;
}

void drawCollectionContent(f32 x0, f32 y0, f32 x1, f32 y1) {
    s_readerRectCount = 0;
    const int view = s_collectTab.load();
    if (view == 0) {
        // Overview owns the whole window; the counter grid's bugs/fish/
        // skills/mail cells open the sections as detail views (Back = the
        // context tab in both modes).
        drawCollectOverview(x0, y0 + 4.0f, x1, y1);
        return;
    }
    // Detail views own the full window; the icon row's rects go dead.
    for (int i = 0; i < 4; i++) {
        s_collectIconRects[i][0] = 0.0f;
        s_collectIconRects[i][2] = 0.0f;
    }
    // Grow/shrink animation between the tapped library cell and the full
    // window (render-rate ease). While shrinking, the overview waits under
    // it; the tab flips back to 0 when the box lands on the cell.
    if (s_collectZoomClosing) {
        s_collectZoomT *= ANIM_DECAY_FAST;
        if (s_collectZoomT < ANIM_ZERO) {
            s_collectZoomClosing = false;
            s_collectZoomT = 1.0f;
            s_collectTab.store(0);
            drawCollectOverview(x0, y0 + 4.0f, x1, y1);
            return;
        }
    } else if (s_collectZoomT < 1.0f) {
        s_collectZoomT += (1.0f - s_collectZoomT) * ANIM_RATE_FAST;
        if (s_collectZoomT > ANIM_DONE) {
            s_collectZoomT = 1.0f;
        }
    }
    const f32 outerA = s_drawAlpha;
    f32 ax0 = x0, ay0 = y0, ax1 = x1, ay1 = y1;
    const bool zooming = s_collectZoomT < 1.0f && s_collectZoomFrom[2] > s_collectZoomFrom[0];
    if (zooming) {
        // The overview sits underneath while the section travels, so the grow
        // visibly comes out of (and returns into) the tapped cell — and it
        // POPS DOWN as it goes: it shrinks slightly and fades, instead of
        // sitting there at full strength while something grows over it.
        const f32 prevA = s_drawAlpha;
        const f32 outT = 1.0f - s_collectZoomT;
        const f32 shrink = 0.06f * s_collectZoomT;  // 0 -> 6% in
        const f32 ow = (x1 - x0) * shrink * 0.5f;
        const f32 oh = (y1 - y0) * shrink * 0.5f;
        s_drawAlpha = prevA * (outT < 0.0f ? 0.0f : outT);
        drawCollectOverview(x0 + ow, y0 + 4.0f + oh, x1 - ow, y1 - oh);
        s_drawAlpha = prevA;
        const f32 t = s_collectZoomT;
        ax0 = s_collectZoomFrom[0] + (x0 - s_collectZoomFrom[0]) * t;
        ay0 = s_collectZoomFrom[1] + (y0 - s_collectZoomFrom[1]) * t;
        ax1 = s_collectZoomFrom[2] + (x1 - s_collectZoomFrom[2]) * t;
        ay1 = s_collectZoomFrom[3] + (y1 - s_collectZoomFrom[3]) * t;
        // No panel fill while it travels — that extra background sliding over
        // the overview is exactly what this transition should not add. The
        // section FADES in over the overview instead.
        s_drawAlpha = prevA * t;
    }
    switch (view) {
    case 1:
        drawCollectBugs(ax0, ay0 + 4.0f, ax1);
        break;
    case 2:
        drawCollectFish(ax0, ay0 + 4.0f, ax1);
        break;
    case 3:
        drawSkillsContent(ax0, ay0 + 4.0f, ax1, ay1);
        break;
    default:
        drawLettersContent(ax0, ay0 + 4.0f, ax1, ay1);
        break;
    }
    s_drawAlpha = outerA;
}

}  // namespace dusk::companion
