// Companion dashboard: COLLECT page — a lookalike of the pause menu's
// collection screen built from resident/collect-archive art. Five
// sub-tabs: the All overview (counters, gear boxes, heart-piece progress,
// Fused Shadows bar), Bugs, Fish, and the Skills / Mail readers (shared
// wrapped-body buffer, drag-scrolled).

#include "dusk/companion.h"
#include "dusk/companion_internal.h"

#include "d/d_com_inf_game.h"
#include "d/d_item_data.h"
#include "d/d_menu_collect.h"
#include "d/d_meter2_info.h"

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

void drawCollectSubTabs(f32 x0, f32 y0, f32 x1) {
    static const char* l_tabNames[COLLECT_TAB_COUNT] = {"All", "Bugs", "Fish", "Skills", "Mail"};
    const int active = s_collectTab.load();
    const f32 tabW = (x1 - x0 - CTAB_GAP * (COLLECT_TAB_COUNT - 1)) / COLLECT_TAB_COUNT;
    for (int i = 0; i < COLLECT_TAB_COUNT; i++) {
        const f32 x = x0 + i * (tabW + CTAB_GAP);
        drawTabPlate(x, y0, tabW, CTAB_H - 4.0f, i == active);
        drawTextCentered(x + tabW * 0.5f, y0 + CTAB_H * 0.5f + 4.0f, 16.0f,
            i == active ? TEXT_TAB_ACTIVE : TEXT_DIM, l_tabNames[i]);
    }
}

// Scent icon slot + display name. Scent item numbers have no entry in
// zel_00.bmg (the 0x165+itemNo lookup lands on unrelated text), so the
// names are fixed strings like the other companion labels.
const char* resolveScent(u8 scent, int* o_iconSlot) {
    *o_iconSlot = -1;
    switch (scent) {
    case dItemNo_SMELL_MEDICINE_e:
        *o_iconSlot = 3;
        return "Medicine";
    case dItemNo_SMELL_CHILDREN_e:
        *o_iconSlot = 4;
        return "Youths'";
    case dItemNo_SMELL_FISH_e:
        *o_iconSlot = 5;
        return "Reekfish";
    case dItemNo_SMELL_YELIA_POUCH_e:
    case dItemNo_SMELL_PUMPKIN_e:
        *o_iconSlot = 6;
        return "Ilia's";
    case dItemNo_SMELL_POH_e:
        *o_iconSlot = 7;
        return "Poe";
    }
    return "-";
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
    const f32 cx = (leftEdge + x1) * 0.5f + 25.0f;
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

// Row 4: quiver, poe souls, skills, letters — icon with count beside it.
void drawCounterRow(f32 x0, f32 x1, f32 rowY, f32 rowH) {
    const f32 gap = 14.0f;
    const f32 cellW = (x1 - x0 - 3.0f * gap) / 4.0f;
    const f32 icon = 30.0f;
    const int arrowMax = dComIfGs_getArrowMax();
    const int quiverIdx = RAWICON_YADUTU1 + (arrowMax >= 100 ? 2 : arrowMax >= 60 ? 1 : 0);
    for (int i = 0; i < 4; i++) {
        const f32 cx = x0 + (f32)i * (cellW + gap);
        drawMenuBox(cx, rowY, cx + cellW, rowY + rowH, CELL_RGBA);
        const f32 ix = cx + 8.0f;
        const f32 iy = rowY + (rowH - icon) * 0.5f;
        // Counts sit close to their icon; the tilted scroll fills its
        // slot's full width, so that cell keeps a bigger gap.
        f32 textPad = 1.0f;
        char text[16];
        text[0] = 0;
        switch (i) {
        case 0: {
            const ResTIMG* q = rawArchiveIcon(0, quiverIdx);
            const bool hasBow = dComIfGs_isItemFirstBit(dItemNo_BOW_e) != 0;
            if (q != NULL) {
                drawTimg(q, ix, iy, icon, icon, hasBow ? 0xFF : 55);
            }
            if (hasBow) {
                snprintf(text, sizeof(text), "%d", arrowMax);
            } else {
                snprintf(text, sizeof(text), "-");
            }
            break;
        }
        case 1: {
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
        case 2: {
            const int skills = countSkills();
            drawScrollIcon(ix, iy, icon, skills > 0 ? 0xFF : 55);
            textPad = 4.0f;
            if (skills > 0) {
                snprintf(text, sizeof(text), "%d/7", skills);
            } else {
                snprintf(text, sizeof(text), "-");
            }
            break;
        }
        case 3: {
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
            break;
        }
        }
        drawText(ix + icon + textPad, rowY + rowH * 0.5f + 5.0f, 14.0f, TEXT_MAIN, "%s", text);
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
    drawText(x0 + 10.0f + icon + 6.0f, rowY + rowH * 0.5f + 5.0f, 14.0f, TEXT_MAIN, "%s",
        scentName);

    // Fused Shadows / Mirror Shards panel: name above the meter.
    const f32 fx0 = x0 + half + gap;
    drawMenuBox(fx0, rowY, x1, rowY + rowH, CELL_RGBA);
    const u8 maskMdl = dMenu_Collect3D_c::getMaskMdlVisible();
    const char* fsLabel = "Fused Shadows";
    int fsHave = 0;
    int fsTotal = 3;
    if (maskMdl == 2) {
        fsLabel = "Mirror Shards";
        fsHave = dMenu_Collect3D_c::getMirrorNum();
        fsTotal = 4;
    } else if (maskMdl == 1) {
        fsHave = dMenu_Collect3D_c::getCrystalNum();
    }
    if (fsHave > fsTotal) {
        fsHave = fsTotal;
    }
    drawText(fx0 + 10.0f, rowY + 19.0f, 13.0f, maskMdl != 0 ? TEXT_MAIN : TEXT_DIM, "%s",
        fsLabel);
    if (maskMdl != 0) {
        drawText(x1 - 42.0f, rowY + 19.0f, 13.0f, TEXT_ACCENT, "%d/%d", fsHave, fsTotal);
    } else {
        drawText(x1 - 24.0f, rowY + 19.0f, 13.0f, TEXT_DIM, "-");
    }
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
    drawCounterRow(x0 + rowInset, x1 - rowInset, row4, 44.0f);
    const f32 row5 = row4 + 44.0f + 8.0f;
    drawScentAndShadowRow(x0 + rowInset, x1 - rowInset, row5, 44.0f);
    if (s_equipMsgFrames > 0) {
        constexpr u32 TEXT_WARN = 0xF0A050FF;
        // Centered between the scent row and the window bottom, starting at
        // the boxes' left edge.
        const f32 msgY = (row5 + 44.0f + y1) * 0.5f + 5.0f;
        drawText(x0 + rowInset, msgY, 14.0f, TEXT_WARN, "%s", s_equipMsg);
    }
}

void drawCollectBugs(f32 x0, f32 y0, f32 x1) {
    const f32 tIcon = 26.0f;
    drawItemIcon(ICON_SLOT_BUG0, dItemNo_M_BEETLE_e, x0 + 6.0f, y0 + 2.0f, tIcon);
    drawText(x0 + 6.0f + tIcon + 10.0f, y0 + 20.0f, 15.0f, TEXT_DIM, "Golden Bugs  %d/24",
        countBugs());
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
    if (const ResTIMG* t = collectIconTimg(0)) {
        drawTimg(t, x0 + 6.0f, y0 + 2.0f, rodIcon, rodIcon, 0xFF);
    } else {
        drawItemIcon(ICON_SLOT_ROD, dItemNo_FISHING_ROD_1_e, x0 + 6.0f, y0 + 2.0f, rodIcon,
            dComIfGs_isItemFirstBit(dItemNo_FISHING_ROD_1_e) ? 0xFF : 55);
    }
    drawText(x0 + 6.0f + rodIcon + 10.0f, y0 + 20.0f, 15.0f, TEXT_DIM, "Fish Journal  %d/6",
        countFishSpecies());
    static const char* l_fishNames[] = {"Greengill", "Hyrule Bass", "Hylian Pike", "Ordon Catfish",
        "Reekfish", "Hylian Loach"};
    // Table: Species | Caught | Record.
    const f32 availW = x1 - x0;
    const f32 colName = x0 + 10.0f;
    const f32 colCaught = x0 + availW * 0.58f;
    const f32 colRecord = x0 + availW * 0.80f;
    const f32 headY = y0 + rodIcon + 28.0f;
    drawText(colName, headY, 13.0f, TEXT_DIM, "Species");
    drawText(colCaught, headY, 13.0f, TEXT_DIM, "Caught");
    drawText(colRecord, headY, 13.0f, TEXT_DIM, "Record");
    for (int i = 0; i < 6; i++) {
        const f32 fy = headY + 26.0f + i * 26.0f;
        const u16 num = dComIfGs_getFishNum(i);
        const u32 col = num > 0 ? TEXT_MAIN : TEXT_DIM;
        drawText(colName, fy, 15.0f, col, "%s", l_fishNames[i]);
        if (num > 0) {
            drawText(colCaught, fy, 15.0f, col, "%d", num);
            drawText(colRecord, fy, 15.0f, col, "%d", dComIfGs_getFishSize(i));
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

// --- Inline message icons -------------------------------------------------
// getStringFull emits 0x02 followed by (outfont index + 1) for the
// controller-button tags. The textures are the message system's own
// font_XX.bti icons from the resident Main2D archive, tinted like
// COutFont_c::createPane.
struct MsgIcon {
    const char* bti;
    u32 black;
    u32 white;
};
constexpr MsgIcon l_msgIcons[20] = {
    {"font_00.bti", 0xFFFFFF00u, 0x62A32EFFu},     // A
    {"font_01.bti", 0xFFFFFF00u, 0xC82727FFu},     // B
    {"font_09.bti", 0x00000000u, 0xFFC832FFu},     // C-stick
    {"font_04.bti", 0x00000000u, 0xC8C8C8FFu},     // L
    {"font_05.bti", 0x00000000u, 0xC8C8C8FFu},     // R
    {"font_02.bti", 0x00000000u, 0xC8C8C8FFu},     // X
    {"font_03.bti", 0x00000000u, 0xC8C8C8FFu},     // Y
    {"font_06.bti", 0xFFFFFF00u, 0x5046A5FFu},     // Z
    {"font_08.bti", 0x00000000u, 0xC8C8C8FFu},     // D-pad
    {"font_07_01.bti", 0x00000000u, 0xFFFFFFFFu},  // control stick
    {"font_10.bti", 0x00000000u, 0xFFFFFFFFu},     // arrows
    {"font_10.bti", 0x00000000u, 0xFFFFFFFFu},
    {"font_10.bti", 0x00000000u, 0xFFFFFFFFu},
    {"font_10.bti", 0x00000000u, 0xFFFFFFFFu},
    {"font_07_01.bti", 0x00000000u, 0xFFFFFFFFu},  // stick directions
    {"font_07_01.bti", 0x00000000u, 0xFFFFFFFFu},
    {"font_07_01.bti", 0x00000000u, 0xFFFFFFFFu},
    {"font_07_01.bti", 0x00000000u, 0xFFFFFFFFu},
    {"font_07_01.bti", 0x00000000u, 0xFFFFFFFFu},
    {"font_07_01.bti", 0x00000000u, 0xFFFFFFFFu},
};
constexpr f32 MSG_ICON_W = 20.0f;

const ResTIMG* msgIconTimg(int idx) {
    if (idx < 0 || idx >= 20 || dComIfGp_getMain2DArchive() == NULL) {
        return NULL;
    }
    return (const ResTIMG*)dComIfGp_getMain2DArchive()->getResource('TIMG', l_msgIcons[idx].bti);
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
            if (const ResTIMG* icon = msgIconTimg(idx)) {
                drawTimgTinted(icon, x + 1.0f, y - ts - 1.0f, MSG_ICON_W - 2.0f,
                    MSG_ICON_W - 2.0f, 0xFF, l_msgIcons[idx].black, l_msgIcons[idx].white);
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
char s_bodyLines[64][128];
int s_bodyLineCount = 0;
int s_fetchedTab = -1;
int s_fetchedSel = -2;

void pushBodyLine(const char* line) {
    if (s_bodyLineCount < 64) {
        snprintf(s_bodyLines[s_bodyLineCount], sizeof(s_bodyLines[0]), "%s", line);
        s_bodyLineCount++;
    }
}

u32 s_bodyGen = 0;

void wrapBody(const char* body, f32 width, f32 ts) {
    s_bodyGen++;
    s_bodyLineCount = 0;
    char cur[128];
    cur[0] = 0;
    const char* p = body;
    bool lastBlank = false;
    while (*p != 0 && s_bodyLineCount < 64) {
        if (*p == '\n') {
            // Collapse runs of blank lines into a single one.
            const bool blank = cur[0] == 0;
            if (!blank || !lastBlank) {
                pushBodyLine(cur);
            }
            lastBlank = blank;
            cur[0] = 0;
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
        char cand[228];
        if (cur[0] != 0) {
            snprintf(cand, sizeof(cand), "%s %s", cur, word);
        } else {
            snprintf(cand, sizeof(cand), "%s", word);
        }
        if (cur[0] != 0 && bodyTextWidth(cand, ts) > width) {
            pushBodyLine(cur);
            snprintf(cur, sizeof(cur), "%s", word);
        } else {
            snprintf(cur, sizeof(cur), "%s", cand);
        }
    }
    if (cur[0] != 0) {
        pushBodyLine(cur);
    }
}

// Ordinal labels for the skills rows and reader header.
const char* skillOrdinal(int i) {
    static const char* l_ord[7] = {"Skill One", "Skill Two", "Skill Three", "Skill Four",
        "Skill Five", "Skill Six", "Skill Seven"};
    return l_ord[i];
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
    ensureReaderContent(tab, x1 - x0 - 36.0f);
    drawText(x0 + 4.0f, y0 + 16.0f, 16.0f, TEXT_ACCENT, "%s", s_readerTitle);
    if (s_readerCorner[0] != 0) {
        const f32 cw = measureText(14.0f, s_readerCorner);
        drawText(x1 - cw - 6.0f, y0 + 16.0f, 14.0f, TEXT_MAIN, "%s", s_readerCorner);
    }
    // The box fills down to the window bottom; Back lives inside it,
    // bottom right, with the text region stopping above it.
    const f32 by0 = y0 + 26.0f;
    const f32 by1 = y1 - 2.0f;
    drawMenuBox(x0, by0, x1, by1, CELL_RGBA);
    const f32 textBottom = by1 - 44.0f;
    const f32 lineH = 21.0f;
    const f32 viewH = textBottom - by0 - 12.0f;
    const f32 contentH = (f32)s_bodyLineCount * lineH;
    const f32 maxScroll = clampListScroll(&s_scrollBody, contentH, viewH);
    if (s_nativeW != 0) {
        GXSetScissorRender((u32)(x0 * s_pixelScale), (u32)((by0 + 6.0f) * s_pixelScale),
            (u32)((x1 - x0) * s_pixelScale) + 1,
            (u32)((textBottom - by0 - 6.0f) * s_pixelScale) + 1);
    }
    for (int i = 0; i < s_bodyLineCount; i++) {
        const f32 ly = by0 + 22.0f + (f32)i * lineH - s_scrollBody;
        if (ly < by0 - lineH || ly > textBottom + lineH) {
            continue;
        }
        drawBodyText(x0 + 16.0f, ly, 14.0f, TEXT_MAIN, s_bodyLines[i]);
    }
    if (s_nativeW != 0) {
        GXSetScissorRender(0, 0, s_nativeW, s_nativeH);
    }
    // Scroll hint bar just inside the box edge, clear of the Back row.
    drawListScrollHint(x1 - 4.0f, by0 + 4.0f, textBottom - 4.0f, s_scrollBody, maxScroll,
        viewH, contentH);
    const f32 fy = by1 - 38.0f;
    drawTabPlate(x1 - 100.0f, fy, 90.0f, 30.0f, false);
    drawTextCentered(x1 - 55.0f, fy + 20.0f, 14.0f, TEXT_MAIN, "Back");
    pushReaderRect(x1 - 100.0f, fy, x1 - 10.0f, fy + 30.0f, -2);
}

// Skills tab: all seven techniques, three columns — scroll icon, ordinal,
// technique name (??? until learned). Tap a learned row to read it.
void drawSkillsContent(f32 x0, f32 y0, f32 x1, f32 y1) {
    if (s_readerSel >= 0) {
        drawReaderDetail(3, x0, y0, x1, y1);
        return;
    }
    static char names[7][64];
    static bool namesLoaded = false;
    if (!namesLoaded) {
        namesLoaded = true;
        for (int i = 0; i < 7; i++) {
            names[i][0] = 0;
            dMeter2Info_getStringFull(l_skillName[i], names[i], sizeof(names[0]));
        }
    }
    const f32 rowH = 44.0f;
    const f32 gap = 8.0f;
    const f32 viewH = y1 - y0;
    const f32 contentH = 7.0f * rowH + 6.0f * gap;
    const f32 maxScroll = clampListScroll(&s_scrollSkills, contentH, viewH);
    if (s_nativeW != 0) {
        GXSetScissorRender((u32)(x0 * s_pixelScale), (u32)(y0 * s_pixelScale),
            (u32)((x1 - x0) * s_pixelScale) + 1, (u32)(viewH * s_pixelScale) + 1);
    }
    // Rows stop short of the right edge so the scroll hint has its own lane.
    const f32 rx1 = x1 - 12.0f;
    for (int i = 0; i < 7; i++) {
        const f32 ry = y0 + (f32)i * (rowH + gap) - s_scrollSkills;
        if (ry < y0 - rowH || ry > y1) {
            continue;
        }
        drawMenuBox(x0, ry, rx1, ry + rowH, CELL_RGBA);
        const bool got = skillLearned(i);
        drawScrollIcon(x0 + 12.0f, ry + (rowH - 30.0f) * 0.5f, 30.0f, got ? 0xFF : 55);
        drawText(x0 + 54.0f, ry + rowH * 0.5f + 5.0f, 15.0f, got ? TEXT_MAIN : TEXT_DIM, "%s",
            skillOrdinal(i));
        if (got) {
            drawText(x0 + 210.0f, ry + rowH * 0.5f + 5.0f, 15.0f, TEXT_ACCENT, "%s", names[i]);
            drawText(rx1 - 26.0f, ry + rowH * 0.5f + 5.0f, 15.0f, TEXT_DIM, ">");
            // Publish only the visible part: rows scrolled past the viewport
            // edge must not be tappable through the sub-tab strip / margins.
            const f32 vy0 = ry > y0 ? ry : y0;
            const f32 vy1 = ry + rowH < y1 ? ry + rowH : y1;
            if (vy1 - vy0 > 12.0f) {
                pushReaderRect(x0, vy0, rx1, vy1, i);
            }
        } else {
            drawText(x0 + 210.0f, ry + rowH * 0.5f + 5.0f, 15.0f, TEXT_DIM, "???");
        }
    }
    if (s_nativeW != 0) {
        GXSetScissorRender(0, 0, s_nativeW, s_nativeH);
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
        if (v > 0) {
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
void drawLettersContent(f32 x0, f32 y0, f32 x1, f32 y1) {
    if (s_readerSel >= 0) {
        drawReaderDetail(4, x0, y0, x1, y1);
        return;
    }
    int idxs[64];
    const int n = sortedLetters(idxs);
    if (n == 0) {
        drawTextCentered((x0 + x1) * 0.5f, (y0 + y1) * 0.5f, 15.0f, TEXT_DIM,
            "No letters yet.");
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
    static int cachedCount = -1;
    if (cachedCount != n) {
        cachedCount = n;
        memset(cached, 0, sizeof(cached));
    }
    if (s_nativeW != 0) {
        GXSetScissorRender((u32)(x0 * s_pixelScale), (u32)(y0 * s_pixelScale),
            (u32)((x1 - x0) * s_pixelScale) + 1, (u32)(viewH * s_pixelScale) + 1);
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
        if (!cached[li]) {
            cached[li] = true;
            subj[li][0] = 0;
            from[li][0] = 0;
            dMeter2Info_getStringFull(dMenu_Letter::getLetterSubject(li), subj[li],
                sizeof(subj[0]));
            char sender[40];
            sender[0] = 0;
            dMeter2Info_getStringFull(dMenu_Letter::getLetterName(li), sender, sizeof(sender));
            if (sender[0] != 0) {
                snprintf(from[li], sizeof(from[0]), "%s", sender);
            }
        }
        drawMenuBox(x0, ry, rx1, ry + rowH, CELL_RGBA);
        if (mailIcon != NULL) {
            drawTimg(mailIcon, x0 + 12.0f, ry + (rowH - 28.0f) * 0.5f, 28.0f, 28.0f, 0xFF);
        }
        drawText(x0 + 52.0f, ry + rowH * 0.5f + 5.0f, 14.0f, TEXT_MAIN, "%s", subj[li]);
        if (from[li][0] != 0) {
            const f32 fw = measureText(13.0f, from[li]);
            drawText(rx1 - fw - 30.0f, ry + rowH * 0.5f + 5.0f, 13.0f, TEXT_DIM, "%s",
                from[li]);
        }
        drawText(rx1 - 20.0f, ry + rowH * 0.5f + 5.0f, 15.0f, TEXT_DIM, ">");
        // Publish only the visible part (see the skills rows).
        const f32 vy0 = ry > y0 ? ry : y0;
        const f32 vy1 = ry + rowH < y1 ? ry + rowH : y1;
        if (vy1 - vy0 > 12.0f) {
            pushReaderRect(x0, vy0, rx1, vy1, li);
        }
    }
    if (s_nativeW != 0) {
        GXSetScissorRender(0, 0, s_nativeW, s_nativeH);
    }
    drawListScrollHint(x1, y0, y1, s_scrollMail, maxScroll, viewH, contentH);
}


}  // namespace

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
        drawBodyText(x, y, ts, rgba, s_bodyLines[idx]);
    }
}

void readerInvalidate() {
    s_fetchedSel = -2;
}

u32 readerBodyGen() {
    return s_bodyGen;
}

void drawCollectionContent(f32 x0, f32 y0, f32 x1, f32 y1) {
    s_readerRectCount = 0;
    drawCollectSubTabs(x0 + 12.0f, y0, x1 - 12.0f);
    const f32 top = y0 + CTAB_H + 4.0f;
    switch (s_collectTab.load()) {
    case 1:
        drawCollectBugs(x0, top, x1);
        break;
    case 2:
        drawCollectFish(x0, top, x1);
        break;
    case 3:
        drawSkillsContent(x0, top, x1, y1);
        break;
    case 4:
        drawLettersContent(x0, top, x1, y1);
        break;
    case 0:
    default:
        drawCollectOverview(x0, top, x1, y1);
        break;
    }
}

}  // namespace dusk::companion
