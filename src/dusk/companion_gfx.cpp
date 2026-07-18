// Companion dashboard: low-level drawing primitives — rect fills, text,
// TIMG blits, HUD digit textures, and pane-subtree compositing. This TU
// privately owns the shared J2DPicture texture cache (s_iconPic): its
// changeTexture calls are keyed on the last-uploaded pointer, so exactly
// one instance must serve every drawTimg/composite call.

#include "dusk/companion.h"
#include "dusk/companion_internal.h"

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

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace dusk::companion {
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
    s_iconPic->setAlpha(layerAlpha);
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

void fillRect(f32 x, f32 y, f32 x2, f32 y2, GXColor color) {
    dDlst_2DQuad_c quad;
    quad.init((s16)x, (s16)y, (s16)x2, (s16)y2, color);
    quad.draw();
}

namespace {

// Flat-color convex polygon (triangle fan) — dDlst_2DQuad_c::draw's GX
// state with float verts.
void fillPoly(const f32* xy, int count, GXColor color) {
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
void drawTextCentered(f32 cx, f32 y, f32 size, u32 rgba, const char* text) {
    drawText(cx - measureText(size, text) * 0.5f, y, size, rgba, "%s", text);
}

// Draw a texture through J2DPicture (JUTTexture handles palettes and EFB-copy
// formats), reusing one picture object with changeTexture (no reallocation).
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
    s_iconPic->setAlpha(alpha);
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
    s_iconPic->setAlpha(alpha);
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
    s_iconPic->setAlpha(alpha);
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
    s_iconPic->setAlpha(alpha);
    s_iconPic->draw(x, y, w, h, false, false, false);
    s_iconPic->setBlackWhite(JUtility::TColor(0x00000000u), JUtility::TColor(0xFFFFFFFFu));
    s_iconPic->setAlpha(0xFF);
    dComIfGp_getCurrentGrafPort()->setup2D();
}

void drawMenuBox(f32 x0, f32 y0, f32 x1, f32 y1, u32 fillRgba) {
    // Chamfered rectangle with a subtle warm outline (drawn — replaces the
    // SPOT_SQUARE3 texture plate).
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
    const f32 dstX = alignRight ? x + boxW - srcW * scale
                                : x + (boxW - srcW * scale) * 0.5f;
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
        s_iconPic->setAlpha(layerAlpha);
        s_iconPic->draw(dstX + (layers[i].x0 - minX) * scale, dstY + (layers[i].y0 - minY) * scale,
            (layers[i].x1 - layers[i].x0) * scale, (layers[i].y1 - layers[i].y0) * scale, false,
            false, false);
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
