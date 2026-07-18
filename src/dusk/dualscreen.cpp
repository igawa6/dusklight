#include "dusk/dualscreen.h"

#include "dusk/companion.h"
#include "dusk/main.h"
#include "dusk/settings.h"
#include "d/d_com_inf_game.h"
#include "JSystem/J2DGraph/J2DOrthoGraph.h"
#include "m_Do/m_Do_graphic.h"
#include "m_Do/m_Do_mtx.h"

#include <atomic>

#include <aurora/aux_window.hpp>
#include <aurora/gfx.hpp>

namespace dusk::dualscreen {
namespace {

bool s_active = false;
bool s_auxWindowFailed = false;
// Whether the boot/loading splash should draw this frame (dual screen on,
// but the dashboard has never presented yet).
bool s_splash = false;
// Written from the Android UI thread (display add/remove), read by the
// game thread's per-frame gates — hence atomic.
std::atomic<bool> s_everPresented{false};
// Second physical display present? Android reports via setDisplayAvailable;
// desktop opens its own window on demand.
#if defined(TARGET_ANDROID) || defined(__ANDROID__) || defined(ANDROID)
std::atomic<bool> s_displayAvailable{false};
#else
std::atomic<bool> s_displayAvailable{true};
#endif

bool isEnabled() {
    // The logo scene draws its 2D lists through the same pass; only redirect
    // once actual gameplay is running. Without a physical second display
    // the setting is inert (single-screen devices keep their HUD).
    return getSettings().game.dualScreen.getValue() && s_displayAvailable &&
           dusk::IsGameLaunched;
}

// The game leaves alpha writes disabled for most 2D drawing, so the capture's
// alpha channel stays at the pass-clear value (0) and alpha-blended consumers
// render it invisible. Stamp alpha=1 across the target, leaving RGB untouched,
// before resolving.
void stampOpaqueAlpha() {
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_S16, 0);
    GXSetNumChans(1);
    GXSetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_NONE,
        GX_AF_NONE);
    GXSetChanMatColor(GX_COLOR0A0, {0xFF, 0xFF, 0xFF, 0xFF});
    GXSetNumTexGens(0);
    GXSetNumTevStages(1);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_SET);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
    GXSetZMode(GX_DISABLE, GX_ALWAYS, GX_DISABLE);
    GXSetCullMode(GX_CULL_NONE);
    GXSetClipMode(GX_CLIP_DISABLE);
    GXSetColorUpdate(GX_FALSE);
    GXSetAlphaUpdate(GX_TRUE);
    GXLoadPosMtxImm(cMtx_getIdentity(), GX_PNMTX0);
    GXSetCurrentMtx(GX_PNMTX0);
    GXBegin(GX_QUADS, GX_VTXFMT0, 4);
    GXPosition3s16(-0x4000, -0x4000, 0);
    GXPosition3s16(0x4000, -0x4000, 0);
    GXPosition3s16(0x4000, 0x4000, 0);
    GXPosition3s16(-0x4000, 0x4000, 0);
    GXEnd();
    GXSetColorUpdate(GX_TRUE);
    GXSetAlphaUpdate(GX_TRUE);
    dComIfGp_getCurrentGrafPort()->setup2D();
}

// Open or close the second OS window to match the setting, and honor a
// user-initiated close of that window.
void updateAuxWindow(bool enabled) {
#if defined(TARGET_ANDROID) || defined(__ANDROID__) || defined(ANDROID)
    // On Android the aux surface lifecycle is driven by DuskActivity's
    // Presentation on the secondary display; just blank it when disabled.
    if (!enabled) {
        aurora::auxwin::set_source({}, 0, 0);
    }
#else
    if (aurora::auxwin::consume_close_request()) {
        getSettings().game.dualScreen.setValue(false);
        enabled = false;
    }
    if (enabled && !aurora::auxwin::is_open() && !s_auxWindowFailed) {
        const aurora::auxwin::CreateInfo info{
            .title = "Dusklight — Second Screen",
            .width = FB_WIDTH * 2,
            .height = FB_HEIGHT * 2,
            .posX = getSettings().game.dualScreenPosX.getValue(),
            .posY = getSettings().game.dualScreenPosY.getValue(),
            .displayIndex = getSettings().game.dualScreenDisplay.getValue(),
        };
        s_auxWindowFailed = !aurora::auxwin::create(info);
    } else if (!enabled) {
        if (aurora::auxwin::is_open()) {
            aurora::auxwin::destroy();
        }
        s_auxWindowFailed = false;
    }
#endif
}

// Canvas geometry limits: logical canvas width clamp and the accepted
// native aux-surface size range (fallback to 2x supersample outside it).
constexpr u32 kMinCanvasW = 320;
constexpr u32 kMaxCanvasW = 1024;
constexpr u32 kMinNativeDim = 320;
constexpr u32 kMaxNativeDim = 2048;

// Match the dashboard canvas to the second screen's aspect ratio (e.g. 8:7
// on the AYN Thor) so the blit fills it edge to edge without skew, and
// render at the aux panel's native resolution (1:1 blit, no scaling blur);
// logical layout units stay at canvas size via the decoupled ortho
// projection. Falls back to 2x supersample when the surface size is
// unknown or out of range.
void computeAuxCanvas(u32& canvasW, u32& canvasH, u32& texW, u32& texH) {
    f32 aspect = (f32)FB_WIDTH / (f32)FB_HEIGHT;
    u32 auxWidth = 0;
    u32 auxHeight = 0;
    if (aurora::auxwin::get_surface_size(&auxWidth, &auxHeight) && auxHeight != 0) {
        aspect = (f32)auxWidth / (f32)auxHeight;
    }
    canvasH = FB_HEIGHT;
    canvasW = (u32)((f32)canvasH * aspect + 0.5f);
    if (canvasW < kMinCanvasW) {
        canvasW = kMinCanvasW;
    } else if (canvasW > kMaxCanvasW) {
        canvasW = kMaxCanvasW;
    }
    texW = canvasW * 2;
    texH = canvasH * 2;
    if (auxWidth >= kMinNativeDim && auxHeight >= kMinNativeDim &&
        auxWidth <= kMaxNativeDim && auxHeight <= kMaxNativeDim)
    {
        texW = auxWidth;
        texH = auxHeight;
    }
}

}  // namespace

void beginHudCapture() {
    const bool wanted = getSettings().game.dualScreen.getValue() && s_displayAvailable;
    const bool enabled = isEnabled();
    updateAuxWindow(wanted);
    s_active = enabled;
    // Boot/loading: keep the second screen on the branded splash instead of
    // black until the dashboard has real content.
    s_splash = wanted && !s_everPresented;
    if (enabled) {
        companion::update();
    }
}

void endHudCapture() {
    const bool splash = s_splash && (!s_active || !companion::hudReady());
    s_splash = false;
    if (!s_active && !splash) {
        return;
    }
    s_active = false;

    // During stage transitions the HUD meter is destroyed and rebuilt; skip
    // the capture entirely so the second screen keeps its last frame instead
    // of flashing a half-empty dashboard (the splash covers first boot).
    if (!splash && !companion::hudReady()) {
        return;
    }

    u32 canvasW, canvasH, texW, texH;
    computeAuxCanvas(canvasW, canvasH, texW, texH);
    if (!aurora::gfx::create_pass(texW, texH)) {
        return;
    }

    // Pixel-exact 2D context for the dashboard, independent of the main
    // screen's (possibly widescreen) ortho extents.
    J2DGrafContext* prevPort = dComIfGp_getCurrentGrafPort();
    if (prevPort == NULL) {
        // Should never happen during mDoGph_Painter — bail out clean.
        aurora::gfx::ResolvedTargets discarded;
        aurora::gfx::resolve_pass({.color = false, .depth = false}, discarded);
        return;
    }
    // The ortho context provides the canvas-unit projection; the actual
    // full-texture viewport/scissor are applied via the aurora passthrough
    // commands, which bypass the logical-fb mapping deterministically.
    J2DOrthoGraph ortho(0.0f, 0.0f, (f32)canvasW, (f32)canvasH, -1.0f, 1.0f);
    companion::setNativeCanvas(texW, texH, (f32)texH / (f32)canvasH);
    dComIfGp_setCurrentGrafPort(&ortho);
    ortho.setPort();
    companion::applyNativeViewport();

    if (splash) {
        companion::drawSplash((f32)canvasW, (f32)canvasH);
    } else {
        companion::drawDashboard((f32)canvasW, (f32)canvasH);
        s_everPresented = true;
    }
    stampOpaqueAlpha();

    dComIfGp_setCurrentGrafPort((J2DOrthoGraph*)prevPort);

    aurora::gfx::ResolvedTargets targets;
    if (aurora::gfx::resolve_pass({.color = true, .depth = false}, targets) && targets.color) {
        if (aurora::auxwin::is_open()) {
            aurora::auxwin::set_source(targets.color, targets.width, targets.height);
        }
    }

    prevPort->setPort();  // never NULL — the capture bails out above
}

bool hudOnCompanion() {
    return getSettings().game.dualScreen.getValue() && s_displayAvailable;
}

void setDisplayAvailable(bool available) {
    s_displayAvailable = available;
    if (!available) {
        s_everPresented = false;
    }
}

}  // namespace dusk::dualscreen
