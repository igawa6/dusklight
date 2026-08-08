// Companion dashboard: low-level drawing primitives — rect fills, text,
// TIMG blits, HUD digit textures, and pane-subtree compositing. This TU
// privately owns the shared J2DPicture texture cache (s_iconPic): its
// changeTexture calls are keyed on the last-uploaded pointer, so exactly
// one instance must serve every drawTimg/composite call.

#include "dusk/companion.h"
#include "dusk/companion_internal.h"
#include "dusk/logging.h"

#include "JSystem/J2DGraph/J2DPicture.h"
#include "JSystem/JKernel/JKRExpHeap.h"  // mDoExt heaps are JKRExpHeap*
#include "JSystem/JKernel/JKRHeap.h"
#include "JSystem/JUtility/JUTFont.h"
#include "d/d_com_inf_game.h"
#include "d/d_drawlist.h"
#include "d/d_meter2.h"
#include "d/d_meter2_draw.h"
#include "d/d_meter2_info.h"
#include "dolphin/gx/GXAurora.h"
#include "m_Do/m_Do_ext.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace dusk::companion {

// Definition for the extern in companion_internal.h.
f32 s_drawAlpha = 1.0f;

namespace {

// The shared texture cache: one picture object, retextured on demand.
// Allocated once on the persistent Zelda heap so stage transitions cannot
// invalidate it.
J2DPicture* s_iconPic;
const ResTIMG* s_lastTimg;

// Composite every textured layer of a HUD pane subtree (button base, ring,
// gloss, letter) into a box, preserving each layer's relative geometry and
// tint — this is what makes the buttons look like the real HUD buttons.
struct PaneLayer {
    J2DPicture* pic;
    f32 x0, y0, x1, y1;
};

// Global content alpha, 0..1. Multiplied into every primitive's colour so a
// block of drawing can be faded as a whole WITHOUT painting anything over it —
// the window's backdrop, border and ornaments stay untouched and visible
// through the fade, which a covering veil could not do.
// Always restore to 1.0f: it is deliberately not scoped, because the page
// content is drawn through dozens of call sites.
}  // namespace

u8 mulDrawAlpha(u8 a) {
    if (s_drawAlpha >= 1.0f) {
        return a;
    }
    const f32 v = (f32)a * (s_drawAlpha < 0.0f ? 0.0f : s_drawAlpha);
    return (u8)(v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v));
}

GXColor mulDrawAlpha(GXColor c) {
    c.a = mulDrawAlpha(c.a);
    return c;
}

namespace {

u32 mulDrawAlphaRgba(u32 rgba) {
    return (rgba & 0xFFFFFF00u) | mulDrawAlpha((u8)(rgba & 0xFFu));
}

void drawCompositeLayer(J2DPicture* src, f32 dx, f32 dy, f32 dw, f32 dh, u8 minAlpha) {
    if (src->getTexture(0) == NULL) {
        return;
    }
    const ResTIMG* timg = src->getTexture(0)->getTexInfo();
    if (timg == NULL) {
        return;
    }
    if (s_iconPic == NULL) {
        s_iconPic = createPicture(timg);
        s_lastTimg = timg;
        if (s_iconPic == NULL) {
            return;
        }
    }
    if (timg != s_lastTimg) {
        s_iconPic->changeTexture(timg, 0);
        s_lastTimg = timg;
    }
    s_iconPic->setBlackWhite(src->getBlack(), src->getWhite());
    s_iconPic->setCornerColor(JUtility::TColor(src->getCornerColorRaw(0)),
        JUtility::TColor(src->getCornerColorRaw(1)), JUtility::TColor(src->getCornerColorRaw(2)),
        JUtility::TColor(src->getCornerColorRaw(3)));
    u8 layerAlpha = src->getAlpha();
    if (layerAlpha < minAlpha) {
        layerAlpha = minAlpha;
    }
    s_iconPic->setAlpha(mulDrawAlpha(layerAlpha));
    s_iconPic->draw(dx, dy, dw, dh, false, false, false);
    s_iconPic->setBlackWhite(JUtility::TColor(0x00000000u), JUtility::TColor(0xFFFFFFFFu));
    s_iconPic->setCornerColor(JUtility::TColor(0xFFFFFFFFu));
    s_iconPic->setAlpha(0xFF);
}

void collectPaneLayers(J2DPane* pane, f32 offX, f32 offY, PaneLayer* out, int& count, int max,
    bool isRoot = false, bool includeHidden = false) {
    // The dual-screen block hides some roots (e.g. X/Y buttons) on the main
    // screen; the companion still composites them, so skip the root's own
    // visibility flag and only honor children's.
    if (pane == NULL || (!isRoot && !pane->isVisible()) || count >= max) {
        return;
    }
    const JGeometry::TBox2<f32>& b = pane->getBounds();
    const f32 px = offX + b.i.x;
    const f32 py = offY + b.i.y;
    if ((pane->getKind() == MULTI_CHAR('PIC1') || pane->getKind() == MULTI_CHAR('PIC2'))) {
        J2DPicture* pic = (J2DPicture*)pane;
        if (pic->getTexture(0) != NULL && (includeHidden || pane->getAlpha() != 0)) {
            out[count++] = {pic, px, py, offX + b.f.x, offY + b.f.y};
        }
    }
    for (J2DPane* child = pane->getFirstChildPane(); child != NULL && count < max;
         child = child->getNextChildPane())
    {
        collectPaneLayers(child, px, py, out, count, max, false, includeHidden);
    }
}

}  // namespace

J2DPicture* createPicture(const ResTIMG* timg) {
    JKRHeap* prevHeap = mDoExt_setCurrentHeap(mDoExt_getZeldaHeap());
    J2DPicture* pic = JKR_NEW J2DPicture(timg);
    mDoExt_setCurrentHeap(prevHeap);
    return pic;
}

u32 s_winClip[4] = {};

void setWinScissor(f32 x0, f32 y0, f32 x1, f32 y1) {
    if (x0 < 0.0f) {
        x0 = 0.0f;
    }
    if (y0 < 0.0f) {
        y0 = 0.0f;
    }
    if (x1 < x0) {
        x1 = x0;
    }
    if (y1 < y0) {
        y1 = y0;
    }
    u32 sx = (u32)(x0 * s_pixelScale);
    u32 sy = (u32)(y0 * s_pixelScale);
    u32 ex = (u32)(x1 * s_pixelScale) + 1;
    u32 ey = (u32)(y1 * s_pixelScale) + 1;
    if (s_winClip[2] > 0) {
        const u32 cx1 = s_winClip[0] + s_winClip[2];
        const u32 cy1 = s_winClip[1] + s_winClip[3];
        if (sx < s_winClip[0]) {
            sx = s_winClip[0];
        }
        if (sy < s_winClip[1]) {
            sy = s_winClip[1];
        }
        if (ex > cx1) {
            ex = cx1;
        }
        if (ey > cy1) {
            ey = cy1;
        }
    }
    GXSetScissorRender(sx, sy, ex > sx ? ex - sx : 0, ey > sy ? ey - sy : 0);
}

void applyWinClip() {
    if (s_winClip[2] > 0) {
        GXSetScissorRender(s_winClip[0], s_winClip[1], s_winClip[2], s_winClip[3]);
    } else {
        GXSetScissorRender(0, 0, s_nativeW, s_nativeH);
    }
}

void fillRect(f32 x, f32 y, f32 x2, f32 y2, GXColor color) {
    color = mulDrawAlpha(color);
    dDlst_2DQuad_c quad;
    quad.init((s16)x, (s16)y, (s16)x2, (s16)y2, color);
    quad.draw();
}

namespace {

// Flat-color convex polygon (triangle fan) — dDlst_2DQuad_c::draw's GX
// state with float verts.
void fillPoly(const f32* xy, int count, GXColor color) {
    color = mulDrawAlpha(color);
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetNumChans(1);
    GXSetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_NONE,
        GX_AF_NONE);
    GXSetChanMatColor(GX_COLOR0A0, color);
    GXSetNumTexGens(0);
    GXSetNumTevStages(1);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_SET);
    GXLoadPosMtxImm(cMtx_getIdentity(), GX_PNMTX0);
    GXSetCurrentMtx(GX_PNMTX0);
    GXBegin(GX_TRIANGLEFAN, GX_VTXFMT0, count);
    for (int i = 0; i < count; i++) {
        GXPosition3f32(xy[i * 2], xy[i * 2 + 1], 0.0f);
    }
    GXEnd();
    dComIfGp_getCurrentGrafPort()->setup2D();
}

// Outline points of a chamfered rect, clockwise from the top-left region.
// Writes up to 8 points, returns the count.
int chamferOutline(f32 x0, f32 y0, f32 x1, f32 y1, f32 ch, int cornerMask, f32* o_xy) {
    int n = 0;
    if (cornerMask & 1) {
        o_xy[n * 2] = x0; o_xy[n * 2 + 1] = y0 + ch; n++;
        o_xy[n * 2] = x0 + ch; o_xy[n * 2 + 1] = y0; n++;
    } else {
        o_xy[n * 2] = x0; o_xy[n * 2 + 1] = y0; n++;
    }
    if (cornerMask & 2) {
        o_xy[n * 2] = x1 - ch; o_xy[n * 2 + 1] = y0; n++;
        o_xy[n * 2] = x1; o_xy[n * 2 + 1] = y0 + ch; n++;
    } else {
        o_xy[n * 2] = x1; o_xy[n * 2 + 1] = y0; n++;
    }
    if (cornerMask & 4) {
        o_xy[n * 2] = x1; o_xy[n * 2 + 1] = y1 - ch; n++;
        o_xy[n * 2] = x1 - ch; o_xy[n * 2 + 1] = y1; n++;
    } else {
        o_xy[n * 2] = x1; o_xy[n * 2 + 1] = y1; n++;
    }
    if (cornerMask & 8) {
        o_xy[n * 2] = x0 + ch; o_xy[n * 2 + 1] = y1; n++;
        o_xy[n * 2] = x0; o_xy[n * 2 + 1] = y1 - ch; n++;
    } else {
        o_xy[n * 2] = x0; o_xy[n * 2 + 1] = y1; n++;
    }
    return n;
}

}  // namespace

void fillChamferRect(f32 x0, f32 y0, f32 x1, f32 y1, f32 ch, GXColor color, int cornerMask) {
    f32 pts[16];
    const int n = chamferOutline(x0, y0, x1, y1, ch, cornerMask, pts);
    fillPoly(pts, n, color);
}

namespace {

GXColor lerpGX(GXColor a, GXColor b, f32 t) {
    return {(u8)((int)a.r + (int)(((int)b.r - (int)a.r) * t)),
        (u8)((int)a.g + (int)(((int)b.g - (int)a.g) * t)),
        (u8)((int)a.b + (int)(((int)b.b - (int)a.b) * t)),
        (u8)((int)a.a + (int)(((int)b.a - (int)a.a) * t))};
}


}  // namespace

// A chamfered octagon filled with a smooth top-to-bottom colour gradient
// (per-vertex colours, interpolated by each vertex's y). Same GX setup as
// fillPoly but with a vertex colour channel — used for the raised bevel
// face of the corner buttons.
void fillChamferVGrad(f32 x0, f32 y0, f32 x1, f32 y1, f32 ch, GXColor top, GXColor bot,
    int cornerMask) {
    f32 pts[16];
    const int n = chamferOutline(x0, y0, x1, y1, ch, cornerMask, pts);
    const f32 span = y1 - y0 > 0.001f ? y1 - y0 : 1.0f;
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GXSetNumChans(1);
    GXSetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE,
        GX_AF_NONE);
    GXSetNumTexGens(0);
    GXSetNumTevStages(1);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_SET);
    GXLoadPosMtxImm(cMtx_getIdentity(), GX_PNMTX0);
    GXSetCurrentMtx(GX_PNMTX0);
    GXBegin(GX_TRIANGLEFAN, GX_VTXFMT0, n);
    for (int i = 0; i < n; i++) {
        const f32 vy = pts[i * 2 + 1];
        f32 t = (vy - y0) / span;
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        const GXColor c = lerpGX(top, bot, t);
        GXPosition3f32(pts[i * 2], vy, 0.0f);
        GXColor4u8(c.r, c.g, c.b, mulDrawAlpha(c.a));
    }
    GXEnd();
    dComIfGp_getCurrentGrafPort()->setup2D();
}

// Shared geometry and palette of the beveled-button look (button + border).
namespace {

constexpr GXColor BEVEL_RIM_GOLD = {198, 166, 104, 255};
constexpr GXColor BEVEL_RIM_GREY = {104, 99, 88, 255};
// The face (or the border's bevel frames) sit this far inside the rim.
constexpr f32 BEVEL_FACE_INSET = 2.5f;

f32 bevelChamfer(f32 x0, f32 y0, f32 x1, f32 y1) {
    const f32 side = (x1 - x0) < (y1 - y0) ? (x1 - x0) : (y1 - y0);
    const f32 ch = side * 0.16f;
    return ch > 14.0f ? 14.0f : ch;
}

// Top-left inner highlight and bottom-right shade: two thin, offset chamfer
// frames give the edge a lit/shadowed bevel. Enabled-state only (callers
// skip it for the flat disabled look). Returns nothing to restore — pure
// draws.
void bevelInnerFrames(f32 x0, f32 y0, f32 x1, f32 y1, f32 ch, int M) {
    const f32 r = BEVEL_FACE_INSET;
    f32 fch = ch - r * 0.6f;
    if (fch < 0.0f) {
        fch = 0.0f;
    }
    drawChamferFrame(x0 + r, y0 + r, x1 - r - 1.0f, y1 - r - 1.0f, fch, 1.2f,
        {112, 105, 92, 130}, M);
    drawChamferFrame(x0 + r + 1.0f, y0 + r + 1.0f, x1 - r, y1 - r, fch, 1.2f, {0, 0, 0, 90},
        M);
}

}  // namespace

// A drawn (no game texture) beveled button: a warm face with a soft top-lit
// gradient, a thin gold rim and a top-left inner highlight, so it reads as a
// physical button. Disabled greys the rim and flattens the gradient. The
// corner buttons pass the default all-corner mask; the context tabs pass a
// top-only mask (1|2) so they sit flush against the screen edge below.
void drawBeveledCornerButton(f32 x0, f32 y0, f32 x1, f32 y1, bool enabled, int M) {
    const f32 ch = bevelChamfer(x0, y0, x1, y1);
    // Dark seat, one pixel proud all round, so the rim reads against the
    // stone backdrop instead of blending into it.
    fillChamferRect(x0 - 1.0f, y0 - 1.0f, x1 + 1.0f, y1 + 1.0f, ch + 1.0f, {0, 0, 0, 150}, M);
    // Gold rim: a solid plate under the face (a frame ring here would risk
    // hairline seams against the face's chamfer diagonal).
    fillChamferRect(x0, y0, x1, y1, ch, enabled ? BEVEL_RIM_GOLD : BEVEL_RIM_GREY, M);
    // Face: inset off the rim, top light → bottom dark for a raised look.
    const f32 r = BEVEL_FACE_INSET;
    f32 fch = ch - r * 0.6f;
    if (fch < 0.0f) {
        fch = 0.0f;
    }
    const GXColor top = enabled ? GXColor{80, 75, 65, 255} : GXColor{52, 50, 46, 255};
    const GXColor bot = enabled ? GXColor{37, 35, 30, 255} : GXColor{31, 30, 27, 255};
    fillChamferVGrad(x0 + r, y0 + r, x1 - r, y1 - r, fch, top, bot, M);
    if (enabled) {
        bevelInnerFrames(x0, y0, x1, y1, ch, M);
    }
}

// Just the beveled button's BORDER — the gold rim and its lit/shadowed inner
// bevel — with no face fill, so it can wrap a caller-drawn plate (the context
// tabs keep the game's own plate texture as their fill but take this rim).
void drawBeveledBorder(f32 x0, f32 y0, f32 x1, f32 y1, bool enabled, int M) {
    const f32 ch = bevelChamfer(x0, y0, x1, y1);
    drawChamferFrame(x0, y0, x1, y1, ch, BEVEL_FACE_INSET,
        enabled ? BEVEL_RIM_GOLD : BEVEL_RIM_GREY, M);
    if (enabled) {
        bevelInnerFrames(x0, y0, x1, y1, ch, M);
    }
}

// Plain panel for the reader/detail body boxes: window fill + thin frame.
// Deliberately NOT drawMenuBox — the item-cell plate stretched over a big
// reader box reads wrong (user feedback), the plate stays on actual cells.
void drawDetailBox(f32 x0, f32 y0, f32 x1, f32 y1) {
    fillRect(x0, y0, x1, y1, COL_WINDOW);
    constexpr GXColor frame = {90, 84, 74, 210};
    fillRect(x0, y0, x1, y0 + 1.0f, frame);
    fillRect(x0, y1 - 1.0f, x1, y1, frame);
    fillRect(x0, y0, x0 + 1.0f, y1, frame);
    fillRect(x1 - 1.0f, y0, x1, y1, frame);
}

// Flat-color convex polygon, exposed for callers that need to mask a shape.
void fillPolyPublic(const f32* xy, int count, GXColor color) {
    fillPoly(xy, count, color);
}

// Filled circle as a 20-gon — smooth enough at the sizes this is used for
// (page dots, the day/night glyph) and it needs no texture.
void fillDisc(f32 cx, f32 cy, f32 r, GXColor color) {
    constexpr int SEGS = 20;
    f32 xy[SEGS * 2];
    for (int i = 0; i < SEGS; i++) {
        const f32 a = 6.2831853f * (f32)i / (f32)SEGS;
        xy[i * 2] = cx + cosf(a) * r;
        xy[i * 2 + 1] = cy + sinf(a) * r;
    }
    fillPoly(xy, SEGS, color);
}

// The tab plate drawn into a CHAMFERED silhouette, by stacking horizontal
// strips of the texture whose width follows the chamfer diagonal. Nothing is
// painted outside the shape, so the backdrop shows through the cut corners —
// masking them with a fill colour left visible corner patches instead.
// One over-scaled draw of the plate, clipped to a rectangle by the render
// scissor. Over-scaling pushes the plate's own baked-in frame OUTSIDE the box
// so the rect samples only its interior grain — otherwise that frame paints a
// square inside our chamfered outline.
static void plateStrip(const ResTIMG* plate, f32 ox, f32 oy, f32 ow, f32 oh, f32 rx0, f32 ry0,
    f32 rx1, f32 ry1, u8 alpha, u32 black, u32 white) {
    if (rx1 <= rx0 || ry1 <= ry0) {
        return;
    }
    setWinScissor(rx0, ry0, rx1, ry1);
    drawTimgTinted(plate, ox, oy, ow, oh, alpha, black, white);
}

void drawChamferPlate(f32 x0, f32 y0, f32 x1, f32 y1, f32 ch, bool selected, int cornerMask) {
    const ResTIMG* plate = decoTimg(DECO_TAB_PLATE);
    if (plate == NULL) {
        fillChamferRect(x0, y0, x1, y1, ch, selected ? COL_TAB_ACTIVE : COL_TAB, cornerMask);
        return;
    }
    const u8 alpha = selected ? 0xFF : 230;
    const u32 black = selected ? 0x726C60FFu : 0x1A1814FFu;
    const u32 white = selected ? 0xF4EEDEFFu : 0x5A5448FFu;

    // The plate quad, over-scaled and centred on the box so the box interior
    // samples only the plate's centre grain (its baked frame lands well
    // outside). Every strip below draws THIS same quad; only the scissor
    // rectangle changes, so the plate is continuous across the whole box.
    constexpr f32 SCALE = 1.7f;
    const f32 bw = x1 - x0;
    const f32 bh = y1 - y0;
    const f32 ow = bw * SCALE;
    const f32 oh = bh * SCALE;
    const f32 ox = x0 - (ow - bw) * 0.5f;
    const f32 oy = y0 - (oh - bh) * 0.5f;

    // A band whose two corners are BOTH unmasked is a plain rectangle, so the
    // per-row loop below would emit `ch` identical full-width one-row strips
    // for it — each one a textured draw plus a scissor change. cornerMask 0 is
    // the worst case and is on two per-frame callers (the Cinematic context
    // tab and kOverlayRow, both ch=12): 25 draws collapse to 1. An open floor
    // picker in a tall dungeon was costing up to 325 of them per frame.
    // (These bands tile exactly with the middle one at y0+ch / y1-ch. With the
    // integer ch every caller passes, that is the same coverage the loop
    // produced; a fractional ch would additionally close a sub-pixel hairline
    // the loop left, since it steps only (int)ch rows.)
    const bool topSquare = (cornerMask & (1 | 2)) == 0;
    const bool botSquare = (cornerMask & (4 | 8)) == 0;
    if (topSquare && botSquare) {
        plateStrip(plate, ox, oy, ow, oh, x0, y0, x1, y1, alpha, black, white);
        applyWinClip();
        return;
    }

    // Middle band: full width. Then the chamfered ends as horizontal strips
    // whose width follows the diagonal — the union of scissor rects is the
    // octagon, so the plate shows through it and nothing else.
    plateStrip(plate, ox, oy, ow, oh, x0, y0 + ch, x1, y1 - ch, alpha, black, white);
    if (topSquare) {
        plateStrip(plate, ox, oy, ow, oh, x0, y0, x1, y0 + ch, alpha, black, white);
    }
    if (botSquare) {
        plateStrip(plate, ox, oy, ow, oh, x0, y1 - ch, x1, y1, alpha, black, white);
    }
    const int steps = (topSquare && botSquare) ? 0 : (int)ch;
    for (int i = 0; i < steps; i++) {
        const f32 t0 = (f32)i;
        // Inset at the strip's TOP edge. Biasing to the bottom edge instead
        // (to close the seam against an overlaid frame) makes the plate
        // overshoot the ideal diagonal and chew visible notches out of the
        // frame's own black edge — worse than the seam it fixes.
        const f32 inset = ch - t0;
        // Each end strip insets only on the sides whose corner is masked, so
        // a partial mask (e.g. left-only) leaves the other side square.
        const f32 topL = (cornerMask & 1) ? x0 + inset : x0;
        const f32 topR = (cornerMask & 2) ? x1 - inset : x1;
        const f32 botL = (cornerMask & 8) ? x0 + inset : x0;
        const f32 botR = (cornerMask & 4) ? x1 - inset : x1;
        if (!topSquare) {
            plateStrip(plate, ox, oy, ow, oh, topL, y0 + t0, topR, y0 + t0 + 1.0f, alpha, black,
                white);
        }
        if (!botSquare) {
            plateStrip(plate, ox, oy, ow, oh, botL, y1 - t0 - 1.0f, botR, y1 - t0, alpha, black,
                white);
        }
    }
    applyWinClip();  // restores the window clip mid-page, full screen otherwise
}

// Flat-color annulus, drawn as a fan of quads. Used to ring the Functional
// layout's round X/Y buttons when they are equip drop targets.
void drawRing(f32 cx, f32 cy, f32 radius, f32 thickness, GXColor color) {
    constexpr int SEGMENTS = 32;
    const f32 inner = radius - thickness;
    if (inner <= 0.0f) {
        return;
    }
    for (int i = 0; i < SEGMENTS; i++) {
        const f32 a0 = (f32)i / (f32)SEGMENTS * 6.2831853f;
        const f32 a1 = (f32)(i + 1) / (f32)SEGMENTS * 6.2831853f;
        const f32 c0 = cosf(a0);
        const f32 s0 = sinf(a0);
        const f32 c1 = cosf(a1);
        const f32 s1 = sinf(a1);
        const f32 quad[8] = {
            cx + c0 * inner, cy + s0 * inner,
            cx + c0 * radius, cy + s0 * radius,
            cx + c1 * radius, cy + s1 * radius,
            cx + c1 * inner, cy + s1 * inner,
        };
        fillPoly(quad, 4, color);
    }
}

f32 clampListScroll(f32* io_scroll, f32 contentH, f32 viewH) {
    const f32 maxScroll = contentH > viewH ? contentH - viewH : 0.0f;
    if (*io_scroll < 0.0f) {
        *io_scroll = 0.0f;
    } else if (*io_scroll > maxScroll) {
        *io_scroll = maxScroll;
    }
    return maxScroll;
}

void drawListScrollHint(f32 x1, f32 y0, f32 y1, f32 scroll, f32 maxScroll, f32 viewH,
    f32 contentH) {
    if (maxScroll <= 0.0f) {
        return;
    }
    const f32 track = y1 - y0 - 8.0f;
    const f32 knobH = track * viewH / contentH;
    const f32 knobY = y0 + 4.0f + (track - knobH) * (scroll / maxScroll);
    constexpr GXColor KNOB = {166, 156, 130, 120};
    fillRect(x1 - 5.0f, knobY, x1 - 2.0f, knobY + knobH, KNOB);
}

void drawChamferFrame(f32 x0, f32 y0, f32 x1, f32 y1, f32 ch, f32 t, GXColor color,
    int cornerMask) {
    f32 outer[16];
    f32 inner[16];
    const int n = chamferOutline(x0, y0, x1, y1, ch, cornerMask, outer);
    // Inner chamfer shrunk so the diagonal stays parallel at thickness t.
    f32 chIn = ch - t * 0.414f;
    if (chIn < 0.0f) {
        chIn = 0.0f;
    }
    chamferOutline(x0 + t, y0 + t, x1 - t, y1 - t, chIn, cornerMask, inner);
    for (int i = 0; i < n; i++) {
        const int j = (i + 1) % n;
        const f32 seg[8] = {outer[i * 2], outer[i * 2 + 1], outer[j * 2], outer[j * 2 + 1],
            inner[j * 2], inner[j * 2 + 1], inner[i * 2], inner[i * 2 + 1]};
        fillPoly(seg, 4, color);
    }
}

void drawText(f32 x, f32 y, f32 size, u32 rgba, const char* fmt, ...) {
    rgba = mulDrawAlphaRgba(rgba);
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    JUTFont* font = mDoExt_getMesgFont();
    if (font == NULL) {
        return;
    }
    font->setGX();
    font->setCharColor(JUtility::TColor(rgba));
    font->drawString_scale(x, y, size * 0.85f, size, buf, true);
    dComIfGp_getCurrentGrafPort()->setup2D();
}

// NOT CURRENTLY USED — see the guide reader, which reverted to plain drawText.
//
// On device this produced "draw vertex data overrun: need 596000 bytes at pos
// 34239, have 225861". 596000 bytes for a 4-vertex quad is ~149KB per vertex,
// which is not a vertex count problem: it means the vertex FORMAT in effect
// when the glyph drew was not the one the font assumes. Passing a context
// makes drawChar_scale skip its own pushDrawState(), so the format becomes
// this code's responsibility for the whole span — and something between the
// begin and the glyphs is still changing it. Desktop did not reproduce it.
//
// Re-enabling needs on-device verification, not desktop: the two behave
// differently here.
//
// Batched text draw: begin once, draw many lines, end once.
//
// The font's PC path keeps every glyph in one joined texture, and
// JUTResFont::loadImage rebinds it per glyph unless a FontDrawContext says it
// is already bound. drawString_size_scale takes no context, so drawText pays
// one GXLoadTexObj per glyph — measured at 1600 binds for one screen of prose
// against a whole-frame baseline of 836.
//
// The setup MUST be hoisted, not done per line. setGX() and setup2D()
// reconfigure GX state and invalidate the binding, so calling them between
// lines while the latch still claims the font is resident desyncs the command
// FIFO outright (observed: vertex data parsed as an opcode). That is why this
// is a begin/end pair rather than a drop-in replacement for drawText.
//
// The per-glyph GXBegin is untouched — inherent to drawChar_scale — so this
// halves the added GX traffic rather than eliminating it.

// Exact pixel width of a string at the given size (matches drawText's
// width scale), using the font's own metrics — the per-char estimate was
// off for proportional glyphs, mis-centering words like "Attack"/"Enter".
f32 measureText(f32 size, const char* text) {
    JUTFont* font = mDoExt_getMesgFont();
    if (font == NULL || text == NULL || font->getCellWidth() <= 0) {
        return 0.0f;
    }
    // Sum per-glyph ink widths — exactly the advance drawText's visible pass
    // uses (drawChar_scale with flag=true advances by width.field_0x1 only).
    // The previous drawString-based measure used the flag=false first-char
    // path, overshooting by the leading bearing and biasing labels left —
    // and it silently rasterized the string at the canvas origin.
    const f32 scale = (size * 0.85f) / (f32)font->getCellWidth();
    f32 w = 0.0f;
    for (const u8* c = (const u8*)text; *c != 0; c++) {
        JUTFont::TWidth tw;
        font->getWidthEntry(*c, &tw);
        w += (f32)tw.field_0x1 * scale;
    }
    return w;
}

// Draw text centered on cx using measured width.
// Uppercase in the font's own encoding. The message font is LATIN-1
// (verified: o-umlaut = 0xF6, sharp-s = 0xDF), so the accented lowercase
// block 0xE0-0xFE maps to uppercase by subtracting 0x20 — except 0xF7
// (division sign, not a letter) and 0xDF (sharp s, which has no single-byte
// uppercase form and is left alone).
void toUpperLatin1(char* s) {
    for (; *s != 0; s++) {
        const unsigned char c = (unsigned char)*s;
        if (c >= 'a' && c <= 'z') {
            *s = (char)(c - 0x20);
        } else if (c >= 0xE0 && c <= 0xFE && c != 0xF7) {
            *s = (char)(c - 0x20);
        }
    }
}

// An archive string in whatever language the game is running, interned by
// message ID. Every lookup is a linear scan of the whole .bmg, and the
// hand-rolled `static char buf[N]; if (buf[0] == 0) fetch` idiom had grown to
// eight sites with four different buffer sizes, two IDs fetched twice over,
// and three copies that rescanned every frame whenever the archive legitimately
// came back empty. This is both the safer and the cheaper way to ask for a word.
// Use it for content the game names (item names); use localizedWord below for
// UI labels, where English keeps the dashboard's own wording.
const char* archiveText(u32 msgId, const char* fallback, bool upper);

// A UI LABEL that also exists in the archive. English keeps the dashboard's
// own (terser) wording; every other language takes the game's. Same policy as
// localizedWord, but sharing archiveText's interning so no caller-owned static
// buffer is needed. Use this for labels; archiveText for content the game names.
const char* archiveLabel(u32 msgId, const char* english, bool upper) {
    if (OSGetLanguage() == OS_LANGUAGE_ENGLISH) {
        return english;
    }
    return archiveText(msgId, english, upper);
}

const char* archiveText(u32 msgId, const char* fallback, bool upper) {
    if (msgId == 0) {
        return fallback;
    }
    // 33 distinct (id, upper) pairs are reachable in one non-English session
// (3 tabs + Poe + Info/Back + Hawkeye + Fused Shadows + Mirror Shards +
// Caught/Record + 6 fish + Hidden Skills + 7 skill ordinals + Bugs + Fish
// Journal + Mail + 5 scents), so 32 was one short: the 33rd interner lost its
// name permanently and silently. Sized with headroom, and it now says so.
// RETRY_GAP spaces the attempts out. They used to be consecutive draws, so a
// page opened during a room transition burned all eight inside ~130 ms — well
// within the window where the archive is legitimately absent — and latched the
// English fallback for the rest of the session, which is the exact failure the
// budget exists to prevent. Eight tries 20 draws apart span ~2.7 s instead.
enum { WORD_CACHE_MAX = 64, FETCH_TRIES = 8, RETRY_GAP = 20 };
    static char l_text[WORD_CACHE_MAX][64];
    static u32 l_ids[WORD_CACHE_MAX];
    static u8 l_tries[WORD_CACHE_MAX];
    static u8 l_wait[WORD_CACHE_MAX];
    static bool l_upper[WORD_CACHE_MAX];
    static int l_count = 0;
    int slot = -1;
    for (int i = 0; i < l_count; i++) {
        if (l_ids[i] == msgId && l_upper[i] == upper) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        if (l_count >= WORD_CACHE_MAX) {
            static bool l_warned = false;
            if (!l_warned) {
                l_warned = true;
                DuskLog.warn("companion: archive word cache full ({}); "
                             "further strings fall back to English",
                    (int)WORD_CACHE_MAX);
            }
            return fallback;
        }
        slot = l_count++;
        l_ids[slot] = msgId;
        l_upper[slot] = upper;
        l_tries[slot] = 0;
        l_wait[slot] = 0;
        l_text[slot][0] = 0;
    }
    if (l_text[slot][0] != 0) {
        return l_text[slot];
    }
    // A draw can land before the archive is resident. Retry a few times rather
    // than latching the first miss for the whole session — but bounded, so an
    // ID that genuinely has no entry cannot rescan the .bmg every frame.
    if (l_tries[slot] >= FETCH_TRIES) {
        return fallback;
    }
    if (l_wait[slot] > 0) {
        l_wait[slot]--;
        return fallback;
    }
    l_wait[slot] = RETRY_GAP;
    l_tries[slot]++;
    dMeter2Info_getStringFull(msgId, l_text[slot], sizeof(l_text[0]));
    if (l_text[slot][0] == 0) {
        return fallback;
    }
    if (upper) {
        toUpperLatin1(l_text[slot]);
    }
    return l_text[slot];
}

const char* localizedWord(u32 msgId, const char* english, bool upper) {
    if (OSGetLanguage() == OS_LANGUAGE_ENGLISH) {
        return english;
    }
    return archiveText(msgId, english, upper);
}

// Draws text shrunk just enough to fit maxW, down to minSize. Localized
// strings run much longer than English (German especially) and most of the
// dashboard's plates are fixed width, so anything showing archive text needs
// this rather than a hardcoded size.
f32 fittedTextSize(f32 size, f32 minSize, f32 maxW, const char* text) {
    // No room at all — the floor size is the only honest answer. (Same
    // reading of maxW <= 0 as drawTextEllipsized, which draws the ellipsis
    // alone; these two used to disagree about it.)
    if (maxW <= 0.0f) {
        return minSize;
    }
    const f32 w = measureText(size, text);
    if (w <= maxW || w <= 0.0f) {
        return size;
    }
    // measureText scales linearly with size, so the exact fit is one division
    // away — no need to step down 0.5 at a time re-measuring the whole string.
    // Snap down to the same 0.5 grid the stepping loop produced.
    f32 fitted = (f32)(int)((maxW * size / w) * 2.0f) * 0.5f;
    if (fitted < minSize) {
        fitted = minSize;
    }
    if (fitted > size) {
        fitted = size;
    }
    return fitted;
}

// Left-aligned text clipped to maxW with a trailing ellipsis. Used by the
// list rows, whose columns sit at fixed x positions — a long localized
// subject or technique name would otherwise run straight into the next
// column with nothing to stop it.
// Length in bytes of the longest prefix of `text` that measures <= maxW.
// The font is LATIN-1 (one byte per glyph) and measureText is a plain per-byte
// sum of advances, so prefix widths accumulate exactly — one walk instead of
// re-measuring every candidate prefix, which is what made the list rows and
// the description wrapper O(n^2) per frame.
int fitPrefix(f32 size, f32 maxW, const char* text) {
    JUTFont* font = mDoExt_getMesgFont();
    if (font == NULL || text == NULL || font->getCellWidth() <= 0) {
        return 0;
    }
    const f32 scale = (size * 0.85f) / (f32)font->getCellWidth();
    int n = 0;
    f32 w = 0.0f;
    for (const u8* c = (const u8*)text; *c != 0; c++) {
        JUTFont::TWidth tw;
        font->getWidthEntry(*c, &tw);
        const f32 adv = (f32)tw.field_0x1 * scale;
        if (w + adv > maxW) {
            break;
        }
        w += adv;
        n++;
    }
    return n;
}

void drawTextEllipsized(f32 x, f32 y, f32 size, f32 maxW, u32 rgba, const char* text) {
    if (text == NULL || text[0] == 0) {
        return;
    }
    if (maxW > 0.0f && measureText(size, text) <= maxW) {
        drawText(x, y, size, rgba, "%s", text);
        return;
    }
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", text);
    buf[fitPrefix(size, maxW - measureText(size, "..."), buf)] = 0;
    // Even a bare ellipsis may overrun a degenerate maxW, but drawing it
    // still beats dropping the field silently — an empty column reads as
    // "there is nothing here", which is a lie.
    drawText(x, y, size, rgba, "%s...", buf);
}

void drawTextFittedCentered(f32 cx, f32 y, f32 size, f32 minSize, f32 maxW, u32 rgba,
    const char* text) {
    const f32 fitted = fittedTextSize(size, minSize, maxW, text);
    const f32 w = measureText(fitted, text);
    if (maxW > 0.0f && w > maxW) {
        // Too long even at the floor size (long German compounds in the
        // narrow corner plates) — clip instead of spilling over whatever
        // sits next to the plate.
        drawTextEllipsized(cx - maxW * 0.5f, y, fitted, maxW, rgba, text);
        return;
    }
    drawText(cx - w * 0.5f, y, fitted, rgba, "%s", text);
}

void drawTextCentered(f32 cx, f32 y, f32 size, u32 rgba, const char* text) {
    drawText(cx - measureText(size, text) * 0.5f, y, size, rgba, "%s", text);
}

// Draw a texture through J2DPicture (JUTTexture handles palettes and EFB-copy
// formats), reusing one picture object with changeTexture (no reallocation).
// Drop the retexture latch. drawTimg and its eight siblings all skip
// changeTexture when the incoming pointer equals s_lastTimg, so freeing a blob
// whose address a later allocation reuses would draw the OLD picture — and if
// aurora has since dropped that texobj from its cache, re-upload
// old_w * old_h * 4 bytes out of a possibly-smaller blob. Anyone freeing a
// ResTIMG that may have been drawn calls this first.
//
// Unconditional rather than pointer-matched on purpose: there is one latch
// shared by all nine sites, so clearing it outright cannot miss an entry and
// cannot be forgotten at a new free site. It costs one extra changeTexture.
void gfxForgetTimgLatch() {
    s_lastTimg = NULL;
}

void drawTimg(const ResTIMG* timg, f32 x, f32 y, f32 w, f32 h, u8 alpha) {
    if (timg == NULL || timg->width == 0 || timg->height == 0) {
        return;
    }
    if (s_iconPic == NULL) {
        s_iconPic = createPicture(timg);
        s_lastTimg = timg;
        if (s_iconPic == NULL) { return; }
    }
    if (timg != s_lastTimg) {
        s_iconPic->changeTexture(timg, 0);
        s_lastTimg = timg;
    }
    s_iconPic->setAlpha(mulDrawAlpha(alpha));
    s_iconPic->draw(x, y, w, h, false, false, false);
    s_iconPic->setAlpha(0xFF);
    dComIfGp_getCurrentGrafPort()->setup2D();
}

// Rotated icon draw (degrees, clockwise, around the icon center) — the same
// pane-rotate + immediate-draw pattern the pause map uses for its arrow.
void drawTimgRotatedRect(const ResTIMG* timg, f32 cx, f32 cy, f32 w, f32 h, f32 angleDeg,
    u8 alpha) {
    if (timg == NULL || timg->width == 0 || timg->height == 0) {
        return;
    }
    if (s_iconPic == NULL) {
        s_iconPic = createPicture(timg);
        s_lastTimg = timg;
        if (s_iconPic == NULL) {
            return;
        }
    }
    if (timg != s_lastTimg) {
        s_iconPic->changeTexture(timg, 0);
        s_lastTimg = timg;
    }
    s_iconPic->rotate(w * 0.5f, h * 0.5f, ROTATE_Z, angleDeg);
    s_iconPic->setAlpha(mulDrawAlpha(alpha));
    s_iconPic->draw(cx - w * 0.5f, cy - h * 0.5f, w, h, false, false, false);
    s_iconPic->setAlpha(0xFF);
    s_iconPic->rotate(0.0f);
    dComIfGp_getCurrentGrafPort()->setup2D();
}

void drawTimgRotated(const ResTIMG* timg, f32 cx, f32 cy, f32 size, f32 angleDeg, u8 alpha) {
    if (timg == NULL || timg->width == 0 || timg->height == 0) {
        return;
    }
    if (s_iconPic == NULL) {
        s_iconPic = createPicture(timg);
        s_lastTimg = timg;
        if (s_iconPic == NULL) { return; }
    }
    if (timg != s_lastTimg) {
        s_iconPic->changeTexture(timg, 0);
        s_lastTimg = timg;
    }
    s_iconPic->rotate(size * 0.5f, size * 0.5f, ROTATE_Z, angleDeg);
    s_iconPic->setAlpha(mulDrawAlpha(alpha));
    s_iconPic->draw(cx - size * 0.5f, cy - size * 0.5f, size, size, false, false, false);
    s_iconPic->setAlpha(0xFF);
    s_iconPic->rotate(0.0f);
    dComIfGp_getCurrentGrafPort()->setup2D();
}

// Union of all layer bounds.
void unionLayerBounds(const PaneLayer* layers, int count, f32& minX, f32& minY, f32& maxX,
    f32& maxY, const bool* skip = NULL) {
    bool first = true;
    minX = minY = maxX = maxY = 0.0f;
    for (int i = 0; i < count; i++) {
        if (skip != NULL && skip[i]) {
            continue;
        }
        if (first) {
            first = false;
            minX = layers[i].x0;
            minY = layers[i].y0;
            maxX = layers[i].x1;
            maxY = layers[i].y1;
            continue;
        }
        minX = layers[i].x0 < minX ? layers[i].x0 : minX;
        minY = layers[i].y0 < minY ? layers[i].y0 : minY;
        maxX = layers[i].x1 > maxX ? layers[i].x1 : maxX;
        maxY = layers[i].y1 > maxY ? layers[i].y1 : maxY;
    }
}

// Mark pill-shaped plate/gloss layers for suppression: elongated AND
// covering a large share of the button. Letter overlays are small — always
// kept. Used to swap the X/Y pill for a circular base while keeping the
// overlays' geometry.
void markPlateLayers(const PaneLayer* layers, int count, bool* skip) {
    f32 uMinX, uMinY, uMaxX, uMaxY;
    unionLayerBounds(layers, count, uMinX, uMinY, uMaxX, uMaxY);
    const f32 uArea = (uMaxX - uMinX) * (uMaxY - uMinY);
    for (int i = 0; i < count; i++) {
        const f32 lw = layers[i].x1 - layers[i].x0;
        const f32 lh = layers[i].y1 - layers[i].y0;
        if (lh > 0.0f && lw > 0.0f && lw * lh > uArea * 0.35f &&
            (lw / lh > 1.3f || lh / lw > 1.3f))
        {
            skip[i] = true;
        }
    }
}

// Tinted draw for the menu's intensity textures: black/white feed the
// picture's TEV color pair, exactly how the game's own layouts color them.
void drawTimgTintedMirror(const ResTIMG* timg, f32 x, f32 y, f32 w, f32 h, u8 alpha,
    u32 blackRgba, u32 whiteRgba, bool mirrorX, bool mirrorY) {
    if (timg == NULL || timg->width == 0 || timg->height == 0) {
        return;
    }
    if (s_iconPic == NULL) {
        s_iconPic = createPicture(timg);
        s_lastTimg = timg;
        if (s_iconPic == NULL) {
            return;
        }
    }
    if (timg != s_lastTimg) {
        s_iconPic->changeTexture(timg, 0);
        s_lastTimg = timg;
    }
    s_iconPic->setBlackWhite(JUtility::TColor(blackRgba), JUtility::TColor(whiteRgba));
    s_iconPic->setAlpha(mulDrawAlpha(alpha));
    s_iconPic->draw(x, y, w, h, mirrorX, mirrorY, false);
    s_iconPic->setBlackWhite(JUtility::TColor(0x00000000u), JUtility::TColor(0xFFFFFFFFu));
    s_iconPic->setAlpha(0xFF);
    dComIfGp_getCurrentGrafPort()->setup2D();
}

void drawTimgTinted(const ResTIMG* timg, f32 x, f32 y, f32 w, f32 h, u8 alpha, u32 blackRgba,
    u32 whiteRgba) {
    if (timg == NULL || timg->width == 0 || timg->height == 0) {
        return;
    }
    if (s_iconPic == NULL) {
        s_iconPic = createPicture(timg);
        s_lastTimg = timg;
        if (s_iconPic == NULL) { return; }
    }
    if (timg != s_lastTimg) {
        s_iconPic->changeTexture(timg, 0);
        s_lastTimg = timg;
    }
    s_iconPic->setBlackWhite(JUtility::TColor(blackRgba), JUtility::TColor(whiteRgba));
    s_iconPic->setAlpha(mulDrawAlpha(alpha));
    s_iconPic->draw(x, y, w, h, false, false, false);
    s_iconPic->setBlackWhite(JUtility::TColor(0x00000000u), JUtility::TColor(0xFFFFFFFFu));
    s_iconPic->setAlpha(0xFF);
    dComIfGp_getCurrentGrafPort()->setup2D();
}

void drawMenuBox(f32 x0, f32 y0, f32 x1, f32 y1, u32 fillRgba) {
    // The game's own item/gear cell plate (TT_SPOT_SQUARE3 — every one of
    // the 24 cells on the collection screen uses it), tinted to the
    // caller's fill.
    if (const ResTIMG* plate = decoTimg(DECO_SLOT_PLATE)) {
        auto lift = [](u32 c, int add, u8 a) {
            int r = (int)((c >> 24) & 0xFF) + add;
            int g = (int)((c >> 16) & 0xFF) + add;
            int b = (int)((c >> 8) & 0xFF) + add;
            r = r < 0 ? 0 : (r > 255 ? 255 : r);
            g = g < 0 ? 0 : (g > 255 ? 255 : g);
            b = b < 0 ? 0 : (b > 255 ? 255 : b);
            return (u32)((r << 24) | (g << 16) | (b << 8) | a);
        };
        // The plate is an INTENSITY texture — no alpha of its own — so its
        // dark surround would paint the caller's fill as an opaque square
        // outside the plate shape. Alpha 0 on the black point makes that
        // surround transparent, leaving only the plate itself.
        const u32 black = lift(fillRgba, -6, 0);
        const u32 white = lift(fillRgba, 82, 0xFF);
        const f32 w = x1 - x0;
        const f32 h = y1 - y0;
        // Interior fill first: the plate's inside is as dark as its outside,
        // so the tint alone can't colour it. A chamfered fill whose corner
        // cut matches the plate's rounded corner at this size gives the
        // "filled inside the shape, corners untouched" look.
        {
            const GXColor fill = {(u8)(fillRgba >> 24), (u8)(fillRgba >> 16),
                (u8)(fillRgba >> 8), (u8)fillRgba};
            // The plate is a near-square with only a small corner radius, so
            // the fill's corner cut must be small too — a big chamfer left
            // black triangles between the fill's diagonal and the plate rim.
            // Fixed pixel cut (not a fraction of height) so wide counter
            // cells match the square cells.
            const f32 side = w < h ? w : h;
            f32 ch = side * 0.12f;
            if (ch > 7.0f) {
                ch = 7.0f;
            }
            fillChamferRect(x0 + 1.0f, y0 + 1.0f, x1 - 1.0f, y1 - 1.0f, ch, fill, 0xF);
        }
        const f32 texW = (f32)(u16)plate->width;
        const f32 texH = (f32)(u16)plate->height;
        // Natural width at this height: drawing a wide cell with the plate
        // stretched full-width smears its rounded corners. Draw it
        // three-sliced instead — left and right caps at natural aspect,
        // only the middle stretched — so the corners stay intact.
        const f32 natW = h * texW / texH;
        if (w > natW + 2.0f) {
            const f32 cap = natW * 0.45f;
            setWinScissor(x0, y0, x0 + cap, y1);
            drawTimgTinted(plate, x0, y0, natW, h, (u8)fillRgba, black, white);
            setWinScissor(x1 - cap, y0, x1, y1);
            drawTimgTinted(plate, x1 - natW, y0, natW, h, (u8)fillRgba, black, white);
            // Middle: magnified so ONLY the texture's centre column covers
            // it. Stretching the whole plate here dragged its rounded
            // corner curve into the middle, which stepped the edge line at
            // both seams.
            constexpr f32 MID_FRAC = 0.10f;
            const f32 midW = w - cap * 2.0f;
            const f32 bigW = midW / MID_FRAC;
            setWinScissor(x0 + cap, y0, x1 - cap, y1);
            drawTimgTinted(plate, (x0 + x1) * 0.5f - bigW * 0.5f, y0, bigW, h, (u8)fillRgba,
                black, white);
            applyWinClip();
        } else {
            drawTimgTinted(plate, x0, y0, w, h, (u8)fillRgba, black, white);
        }
        return;
    }
    // Fallback: chamfered rectangle with a subtle warm outline.
    const GXColor fill = {(u8)(fillRgba >> 24), (u8)(fillRgba >> 16), (u8)(fillRgba >> 8),
        (u8)fillRgba};
    const f32 shortSide = x1 - x0 < y1 - y0 ? x1 - x0 : y1 - y0;
    f32 ch = shortSide * 0.18f;
    if (ch > 9.0f) {
        ch = 9.0f;
    } else if (ch < 4.0f) {
        ch = 4.0f;
    }
    fillChamferRect(x0, y0, x1, y1, ch, fill, 0xF);
    constexpr GXColor frame = {110, 104, 92, 190};
    drawChamferFrame(x0, y0, x1, y1, ch, 1.5f, frame, 0xF);
}

void drawTabPlate(f32 x, f32 y, f32 w, f32 h, bool selected) {
    const ResTIMG* plate = decoTimg(DECO_TAB_PLATE);
    if (plate == NULL) {
        fillRect(x, y, x + w, y + h, selected ? COL_TAB_ACTIVE : COL_TAB);
        return;
    }
    // The Save/Options button texture, silvery cream when selected.
    if (selected) {
        drawTimgTinted(plate, x, y, w, h, 0xFF, 0x726C60FFu, 0xF4EEDEFFu);
    } else {
        drawTimgTinted(plate, x, y, w, h, 230, 0x1A1814FFu, 0x5A5448FFu);
    }
}

// o_map (optional, 5 floats): the root-relative -> box affine used for the
// layers — {dstX, dstY, minX, minY, scale}; scale stays 0 when nothing drew.
void drawPaneComposite(J2DPane* root, f32 x, f32 y, f32 boxW, f32 boxH, u8 minAlpha,
    bool alignRight, bool skipLargest, f32* o_map, bool dropPlate) {
    PaneLayer layers[64];
    int count = 0;
    collectPaneLayers(root, 0.0f, 0.0f, layers, count, 64, true, minAlpha > 0);
    bool skip[64] = {};
    if ((skipLargest || dropPlate) && count > 1) {
        markPlateLayers(layers, count, skip);
    }
    if (count == 0) {
        return;
    }
    // dropPlate: backdrop plates are removed entirely — excluded from the
    // fit bounds too, so the remaining art fills the box edge-to-edge.
    f32 minX, minY, maxX, maxY;
    unionLayerBounds(layers, count, minX, minY, maxX, maxY, dropPlate ? skip : NULL);
    const f32 srcW = maxX - minX;
    const f32 srcH = maxY - minY;
    if (srcW <= 0.0f || srcH <= 0.0f) {
        return;
    }
    if (s_iconPic == NULL) {
        s_iconPic = createPicture(layers[0].pic->getTexture(0)->getTexInfo());
        if (s_iconPic == NULL) {
            return;
        }
    }
    const f32 scale = boxW / srcW < boxH / srcH ? boxW / srcW : boxH / srcH;
    const f32 dstX = alignRight ? x + boxW - srcW * scale : x + (boxW - srcW * scale) * 0.5f;
    const f32 dstY = y + (boxH - srcH * scale) * 0.5f;
    if (o_map != NULL) {
        o_map[0] = dstX;
        o_map[1] = dstY;
        o_map[2] = minX;
        o_map[3] = minY;
        o_map[4] = scale;
    }
    for (int i = 0; i < count; i++) {
        if (skip[i]) {
            continue;
        }
        J2DPicture* src = layers[i].pic;
        if (skipLargest) {
            // The pill's translucent specular-gloss overlays (x_btn_l /
            // y_btn_l) belong to the skipped plate — drop them with it. The
            // letter panes are fully opaque. (Y's gloss is elongated enough
            // for the plate heuristic; X's 12x12 one slipped through and
            // rendered as a blob centered on the button.)
            if (src->getAlpha() < 0xFF) {
                continue;
            }
            // With the plate gone, center each remaining layer (the letter)
            // on the box instead of keeping its plate-relative offset.
            const f32 lw = (layers[i].x1 - layers[i].x0) * scale;
            const f32 lh = (layers[i].y1 - layers[i].y0) * scale;
            drawCompositeLayer(src, x + (boxW - lw) * 0.5f, y + (boxH - lh) * 0.5f, lw, lh,
                minAlpha);
            continue;
        }
        const ResTIMG* timg = src->getTexture(0)->getTexInfo();
        if (timg == NULL) {
            continue;
        }
        if (timg != s_lastTimg) { s_iconPic->changeTexture(timg, 0); s_lastTimg = timg; }
        s_iconPic->setBlackWhite(src->getBlack(), src->getWhite());
        s_iconPic->setCornerColor(JUtility::TColor(src->getCornerColorRaw(0)),
            JUtility::TColor(src->getCornerColorRaw(1)),
            JUtility::TColor(src->getCornerColorRaw(2)),
            JUtility::TColor(src->getCornerColorRaw(3)));
        u8 layerAlpha = src->getAlpha();
        if (layerAlpha < minAlpha) {
            layerAlpha = minAlpha;
        }
        s_iconPic->setAlpha(mulDrawAlpha(layerAlpha));
        s_iconPic->draw(dstX + (layers[i].x0 - minX) * scale,
            dstY + (layers[i].y0 - minY) * scale, (layers[i].x1 - layers[i].x0) * scale,
            (layers[i].y1 - layers[i].y0) * scale, false, false, false);
    }
    s_iconPic->setBlackWhite(JUtility::TColor(0x00000000u), JUtility::TColor(0xFFFFFFFFu));
    s_iconPic->setCornerColor(JUtility::TColor(0xFFFFFFFFu));
    s_iconPic->setAlpha(0xFF);
    dComIfGp_getCurrentGrafPort()->setup2D();
}

// X/Y as circles: the A button's circular base texture, tinted with the
// X/Y buttons' own grey black/white + corner gradient.
void drawButtonCircleBase(dMeter2Draw_c* md, int xyIdx, f32 x, f32 y, f32 box) {
    J2DPicture* circle = md->getButtonBasePicture(0);
    J2DPicture* tintSrc = md->getButtonBasePicture(xyIdx);
    if (circle == NULL || circle->getTexture(0) == NULL || tintSrc == NULL) {
        return;
    }
    const ResTIMG* timg = circle->getTexture(0)->getTexInfo();
    if (timg == NULL || timg->width == 0 || timg->height == 0) {
        return;
    }
    if (s_iconPic == NULL) {
        s_iconPic = createPicture(timg);
        s_lastTimg = timg;
        if (s_iconPic == NULL) {
            return;
        }
    }
    if (timg != s_lastTimg) {
        s_iconPic->changeTexture(timg, 0);
        s_lastTimg = timg;
    }
    f32 w = box;
    f32 h = box * (f32)timg->height / (f32)timg->width;
    if (h > box) {
        h = box;
        w = box * (f32)timg->width / (f32)timg->height;
    }
    s_iconPic->setBlackWhite(tintSrc->getBlack(), tintSrc->getWhite());
    s_iconPic->setCornerColor(JUtility::TColor(tintSrc->getCornerColorRaw(0)),
        JUtility::TColor(tintSrc->getCornerColorRaw(1)),
        JUtility::TColor(tintSrc->getCornerColorRaw(2)),
        JUtility::TColor(tintSrc->getCornerColorRaw(3)));
    // Two stamps: the base texture carries baked-in translucency (fine over
    // gameplay on the main HUD, see-through over the companion backdrop) —
    // compounding the alpha reads solid while keeping the grey gradient.
    s_iconPic->draw(x + (box - w) * 0.5f, y + (box - h) * 0.5f, w, h, false, false, false);
    s_iconPic->draw(x + (box - w) * 0.5f, y + (box - h) * 0.5f, w, h, false, false, false);
    s_iconPic->setBlackWhite(JUtility::TColor(0x00000000u), JUtility::TColor(0xFFFFFFFFu));
    s_iconPic->setCornerColor(JUtility::TColor(0xFFFFFFFFu));
    dComIfGp_getCurrentGrafPort()->setup2D();
}

// Render a count with the game's own HUD digit textures (main 2D archive),
void drawHudNumber(int value, f32 x, f32 y, f32 digitH) {
    if (value < 0) {
        value = 0;
    }
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", value);
    dMeter2Draw_c* md = meterDraw();
    if (md == NULL || dComIfGp_getMain2DArchive() == NULL) {
        return;
    }
    const f32 digitW = digitH * 0.72f;
    f32 dx = x;
    for (const char* c = buf; *c != '\0'; c++) {
        ResTIMG* timg = md->getNumberTexture(*c - '0');
        if (timg == NULL) {
            return;
        }
        drawTimg(timg, dx, y, digitW, digitH, 0xFF);
        dx += digitW * 0.9f;
    }
}

dMeter2Draw_c* meterDraw() {
    dMeter2_c* meter = dMeter2Info_getMeterClass();
    return meter != NULL ? meter->getMeterDrawPtr() : NULL;
}

}  // namespace dusk::companion
