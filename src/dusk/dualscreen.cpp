#include "dusk/dualscreen.h"

#include "dusk/companion.h"
#include "dusk/main.h"
#include "dusk/settings.h"
#include "d/d_com_inf_game.h"
#include "f_pc/f_pc_name.h"
#include "JSystem/J2DGraph/J2DOrthoGraph.h"
#include "m_Do/m_Do_graphic.h"
#include "m_Do/m_Do_mtx.h"

#include "data.hpp"
#include "dusk/logging.h"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include <aurora/aux_window.hpp>
#include <aurora/gfx.hpp>

#include "miniz.h"

#if DUSK_PHONE_SPIKE
#include "dusk/phone_spike_pad.h"
#include "dusk/phone_spike_pairing.h"
#include "dusk/phone_spike_ws.h"

#include <chrono>
#endif

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
// Set when the current scene change leaves gameplay (title / file select):
// the dashboard keeps drawing through the fade-out, and once the HUD meter
// dies the splash returns instead of a frozen last frame. Play-to-play
// stage transitions clear it, keeping the existing hold-last-frame skip.
bool s_leftGameplay = false;
// Low-health hearts pop-in latch (Cinematic only); see lowLifePopIn().
bool s_lowLifeLatch = false;
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
    // "Gameplay is running" is asked TWO ways on purpose. IsGameLaunched is the
    // intended signal, but it is set in exactly two places, both inside
    // prelaunch.cpp — so any route that reaches gameplay without that screen
    // leaves it false forever, and the second screen then sits on the boot
    // splash for the whole session while the game plays normally on the main
    // display. companion::hudReady() is direct evidence: the game's own HUD
    // meter exists, which it does not during the logo scene, so it cannot
    // re-introduce the problem the flag was added to avoid.
    return getSettings().game.dualScreen.getValue() && s_displayAvailable &&
           (dusk::IsGameLaunched || companion::hudReady());
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
#if DUSK_PHONE_SPIKE
            // The spike pushes frames over the network instead of showing a
            // local window — see docs/phone-companion-design.md phase 1.
            .hidden = true,
#endif
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
// kMaxNativeDim was 2048 — too small for a lot of real hardware: a modern
// phone's long edge commonly exceeds it (e.g. 1080x2400, 1440x3200), and so
// does an ordinary 4K second monitor (3840x2160), meaning EITHER would have
// silently fallen back to the lower-res supersampled canvas instead of true
// native 1:1. Raised for both cases, not just the phone spike. Tradeoff:
// this bounds the aux render target AND its capture/staging buffer (see
// aux_window.cpp's ensure_capture_locked) each at up to ~4096x4096 RGBA8,
// ~67MB apiece — fine for anything that can run this game, but worth
// knowing before raising it further.
constexpr u32 kMinCanvasW = 320;
constexpr u32 kMaxCanvasW = 1024;
constexpr u32 kMinNativeDim = 320;
constexpr u32 kMaxNativeDim = 4096;

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

#if DUSK_COMPANION_CAPTURE
// Pending screenshot destination. Written from whichever thread asked (the
// Android UI thread for the DUMP broadcast), read on the game thread.
std::mutex s_shotMutex;
std::string s_shotPath;
bool s_shotPending = false;
// Frames the request has been outstanding. The capture can be abandoned
// without ever completing (surface torn down, display detached), and without
// this the request latched forever and polled silently for the rest of the
// session.
int s_shotWaited = 0;
constexpr int kShotTimeoutFrames = 240;
#endif

#if DUSK_PHONE_SPIKE
constexpr uint16_t kSpikePort = 8765;
// No artificial rate cap: a prior ~15fps (66ms) interval gate here was
// measured inert on the original (llvmpipe/Xvfb) test box — GPU read-back
// latency alone already exceeded it — so it was kept as a ceiling for
// faster hardware. On real GPU hardware, with the game-thread stalls from
// the capture/encode/send pipeline already fixed (see
// docs/phone-companion-design.md), the user still felt visible latency, and
// this cap was the next suspect: it's now removed entirely so a new capture
// is always requested the instant the previous one is consumed, as fast as
// aux_window's request_capture()/take_capture() pipeline actually allows.
// If this doesn't meaningfully help, the real fix is architectural (stream
// UI state and render natively on the phone instead of pushing rendered
// frames) — see the doc's latency section.
bool s_spikeCaptureArmed = false;
bool s_spikeServerStarted = false;
#endif

}  // namespace

#if DUSK_COMPANION_CAPTURE
void requestScreenshot(const char* path) {
    {
        std::lock_guard lock{s_shotMutex};
        if (path != nullptr && path[0] != '\0') {
            s_shotPath = path;
        } else {
            // Desktop default; on Android the caller passes the app's
            // external files dir, which is the only place adb can reach.
            s_shotPath = (data::configured_data_path() / "companion-screenshot.png").string();
        }
        s_shotPending = true;
        s_shotWaited = 0;
    }
    aurora::auxwin::request_capture();
}

// Writes the PNG once the read-back lands; no-op while nothing is pending.
// File-local: one caller (beginHudCapture), no header declaration.
static void pollScreenshot() {
    {
        std::lock_guard lock{s_shotMutex};
        if (!s_shotPending) {
            return;
        }
    }
    std::vector<u8> pixels;
    u32 width = 0;
    u32 height = 0;
    if (!aurora::auxwin::take_capture(pixels, &width, &height) || width == 0 || height == 0) {
        std::lock_guard lock{s_shotMutex};
        if (++s_shotWaited > kShotTimeoutFrames) {
            s_shotPending = false;
            DuskLog.warn("companion screenshot: no frame captured, giving up");
        }
        return;  // still in flight
    }
    std::string path;
    {
        std::lock_guard lock{s_shotMutex};
        s_shotPending = false;
        path = s_shotPath;
    }
    // Level 1, not 6: this encodes a full-panel RGBA frame (~4 MB at 1240x1080)
    // synchronously on the GAME thread inside beginHudCapture, so the hitch is
    // paid by the running game. Acceptable for a debug aid, but not worth
    // several hundred extra milliseconds for a smaller file.
    size_t pngSize = 0;
    void* png = tdefl_write_image_to_png_file_in_memory_ex(pixels.data(), (int)width, (int)height,
        4, &pngSize, 1, MZ_FALSE);
    if (png == NULL) {
        DuskLog.warn("companion screenshot: PNG encode failed");
        return;
    }
    FILE* file = fopen(path.c_str(), "wb");
    if (file != NULL) {
        const bool ok = fwrite(png, 1, pngSize, file) == pngSize;
        fclose(file);
        if (ok) {
            DuskLog.info("companion screenshot: wrote {}x{} to {}", width, height, path);
        } else {
            DuskLog.warn("companion screenshot: short write to {}", path);
        }
    } else {
        DuskLog.warn("companion screenshot: cannot open {}", path);
    }
    mz_free(png);
}

#endif  // DUSK_COMPANION_CAPTURE

#if DUSK_PHONE_SPIKE
void ensurePhoneSpikeStarted() {
    if (s_spikeServerStarted) {
        return;
    }
    s_spikeServerStarted = phone_spike::start_server(kSpikePort);
    if (s_spikeServerStarted) {
        const std::string qrPath = (data::configured_data_path() / "phone-pairing-qr.png").string();
        phone_spike::start_pairing(kSpikePort, qrPath);
    }
}

// File-local: one caller (beginHudCapture), no header declaration — same
// shape as pollScreenshot above, continuous instead of one-shot.
static void pollAndPushSpikeFrame() {
    ensurePhoneSpikeStarted();

    // Runs BEFORE updateAuxWindow() below (called later in beginHudCapture):
    // if this creates/resizes the aux target here, updateAuxWindow() sees
    // is_open() already true and leaves it alone instead of recreating it
    // at the generic default size. create()/destroy() are main/game-thread
    // only (aux_window.hpp) — this is why the WS receive thread hands the
    // request off via request_resize() instead of acting on it directly.
    u32 reqW = 0;
    u32 reqH = 0;
    if (phone_spike::take_pending_resize(reqW, reqH)) {
        if (reqW < kMinNativeDim) {
            reqW = kMinNativeDim;
        } else if (reqW > kMaxNativeDim) {
            reqW = kMaxNativeDim;
        }
        if (reqH < kMinNativeDim) {
            reqH = kMinNativeDim;
        } else if (reqH > kMaxNativeDim) {
            reqH = kMaxNativeDim;
        }
        u32 curW = 0;
        u32 curH = 0;
        const bool haveCur = aurora::auxwin::get_surface_size(&curW, &curH);
        if (!haveCur || curW != reqW || curH != reqH) {
            DuskLog.info("phone spike: resizing aux target to {}x{} (phone-reported)", reqW, reqH);
            if (aurora::auxwin::is_open()) {
                aurora::auxwin::destroy();
            }
            const aurora::auxwin::CreateInfo info{
                .title = "Dusklight — Phone Spike",
                .width = reqW,
                .height = reqH,
                .hidden = true,
                // reqW/reqH are already the phone's real device pixels
                // (width*devicePixelRatio, resolved client-side) — without
                // this, SDL_WINDOW_HIGH_PIXEL_DENSITY treats them as
                // DPI-independent points and multiplies by the desktop's
                // OWN display scale again, silently creating an
                // oversized surface (measured: a 1270x2416 request became
                // a 1905x3624 surface on a 1.5x-scaled display — 2.25x the
                // pixels to capture/encode/send every cycle, enough to
                // visibly lag the main game thread).
                .exactPixelSize = true,
            };
            if (!aurora::auxwin::create(info)) {
                DuskLog.warn("phone spike: failed to recreate aux target at {}x{}", reqW, reqH);
            }
            // destroy() silently abandons any capture that was
            // Armed/InFlight against the OLD surface (aux_window.cpp's
            // release_capture_locked() resets ITS internal state to Idle)
            // — but this file's own s_spikeCaptureArmed has no way to know
            // that happened and was left stuck true, since nothing else
            // ever clears it except a successful take_capture(). With it
            // stuck true, the re-arm branch below (`if
            // (!s_spikeCaptureArmed && has_client())`) never runs again:
            // no capture is ever requested against the NEW surface, so no
            // frame is ever pushed again — a real bug, caught by live
            // testing (looked like "waiting for first frame" forever after
            // the phone's resize on connect). Reset it here so the next
            // tick starts a fresh request against the surface that
            // actually exists now.
            s_spikeCaptureArmed = false;
        }
    }

    // Applies the latest phone-gamepad report (if any) as a real second
    // controller for the main game — see phone_spike_pad.h. Every frame,
    // not just on change: PADSetVirtualStatus/PADClearVirtualStatus need
    // refreshing continuously, matching touch_controls.cpp's own
    // sync_virtual_input(), which this mirrors.
    phone_spike::applyGamepadPassthrough();

    if (s_spikeCaptureArmed) {
        std::vector<u8> pixels;
        u32 width = 0;
        u32 height = 0;
        if (aurora::auxwin::take_capture(pixels, &width, &height) && width != 0 && height != 0) {
            s_spikeCaptureArmed = false;
            if (phone_spike::has_client()) {
                // queue_raw_frame(), not an in-line PNG encode +
                // queue_binary_frame(): both the encode AND the socket
                // write now happen on the background sender thread, never
                // here — see phone_spike_ws.h. A real phone over real
                // Wi-Fi made doing either synchronously on the game thread
                // visibly stall it; this runs every ~66ms, so even a
                // "cheap" per-call cost adds up to real, visible lag.
                phone_spike::queue_raw_frame(std::move(pixels), width, height);
            }
        }
    }

    if (!s_spikeCaptureArmed && phone_spike::has_client()) {
        aurora::auxwin::request_capture();
        s_spikeCaptureArmed = true;
    }
}
#endif  // DUSK_PHONE_SPIKE

void beginHudCapture() {
#if DUSK_COMPANION_CAPTURE
    pollScreenshot();
#endif
#if DUSK_PHONE_SPIKE
    pollAndPushSpikeFrame();
#endif
    const bool wanted = getSettings().game.dualScreen.getValue() && s_displayAvailable;
    const bool enabled = isEnabled();
    updateAuxWindow(wanted);
    s_active = enabled;
    // Boot/loading: keep the second screen on the branded splash instead of
    // black until the dashboard has real content. Leaving gameplay (quit to
    // title, game over) re-arms it — endHudCapture keeps the dashboard while
    // the meter still lives (the fade-out), then falls back to the splash.
    s_splash = wanted && (!s_everPresented || s_leftGameplay);

    // Reconcile the Java-side mirror with the config. The settings toggle
    // publishes on change, so this is only for the cases the toggle never saw:
    // a first run, or a config.json carried in from another install (a moved
    // data folder) whose swap value the SharedPreferences file knows nothing
    // about. Compare-and-skip, so the steady state costs one bool test.
    {
        const bool swap = getSettings().game.dualScreenSwap.getValue();
        static int s_lastPublished = -1;
        if (s_lastPublished != (swap ? 1 : 0)) {
            s_lastPublished = swap ? 1 : 0;
            publishSwapPreference(swap);
        }
    }

    // One line whenever the second screen's state changes. "The bottom screen
    // just shows the logo" is impossible to triage from a description, because
    // three very different faults look identical from outside:
    //   displayAvailable=0 -> nothing is presented; the user is seeing the
    //                         HANDHELD's own app icon, not ours
    //   launched=0         -> our splash forever (isEnabled needs IsGameLaunched)
    //   hudReady=0         -> the game's HUD meter does not exist
    // Logged on change only, so it costs nothing per frame.
    {
        const bool ready = companion::hudReady();
        const int state = (wanted ? 1 : 0) | (enabled ? 2 : 0) | (ready ? 4 : 0) |
            (s_splash ? 8 : 0) | (s_displayAvailable ? 16 : 0) |
            (s_everPresented ? 32 : 0) | (s_leftGameplay ? 64 : 0);
        static int s_lastState = -1;
        if (state != s_lastState) {
            s_lastState = state;
            DuskLog.info("dualscreen: displayAvailable={} setting={} launched={} wanted={} "
                         "enabled={} hudReady={} splash={} everPresented={} leftGameplay={} swap={}",
                (bool)s_displayAvailable, getSettings().game.dualScreen.getValue(),
                (bool)dusk::IsGameLaunched, wanted, enabled, ready, s_splash,
                (bool)s_everPresented, s_leftGameplay,
                getSettings().game.dualScreenSwap.getValue());
        }
    }

    // Low-health hearts pop-in (Cinematic only). Life is 4 units/heart, max
    // is 5 units/heart (drawLife's own divisors). Threshold scales with max
    // hearts, clamped to 1..2; the release point is one full heart higher so
    // the boundary can't flicker.
    if (!enabled || mainHudRestored() || !companion::hudReady()) {
        s_lowLifeLatch = false;
    } else {
        int showHearts = (dComIfGs_getMaxLife() / 5) / 4;
        if (showHearts < 1) {
            showHearts = 1;
        } else if (showHearts > 2) {
            showHearts = 2;
        }
        const u16 life = dComIfGs_getLife();
        if (life <= (u16)(showHearts * 4)) {
            s_lowLifeLatch = true;
        } else if (life > (u16)((showHearts + 1) * 4)) {
            s_lowLifeLatch = false;
        }
    }

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

    // The dim is applied by the aux window to the PRESENT blit, not painted
    // into the dashboard. That is the whole point: on the frames below where
    // the capture is skipped, the second screen keeps presenting its last
    // texture and the fade still advances over it. The splash owns its own
    // look and is never dimmed.
    if (splash) {
        // The splash owns the panel and carries no fade of its own. Clear the
        // dashboard's ramp too, or the level it had already reached is still
        // sitting there when the splash hands back and snaps a lit logo to
        // black in one frame.
        companion::resetDim();
        aurora::auxwin::set_dim(0.0f);
    } else {
        aurora::auxwin::set_dim(companion::currentDim());
    }

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

#if !defined(TARGET_ANDROID) && !defined(__ANDROID__) && !defined(ANDROID)
// Swapping which panel each WINDOW opens on is an Android idea; a desktop aux
// window has no launcher to hand the choice to. Defined here so the call sites
// need no platform guard of their own — android_aux_display.cpp supplies the
// real one.
void publishSwapPreference(bool) {}
#endif

bool hudOnCompanion() {
    return getSettings().game.dualScreen.getValue() && s_displayAvailable;
}

bool mainHudRestored() {
    return hudOnCompanion() &&
           getSettings().game.dualScreenHudMode.getValue() == kDualHudFunctional;
}

bool mainHudActive() {
    return !hudOnCompanion() || mainHudRestored();
}

bool lowLifePopIn() {
    return s_lowLifeLatch;
}

void onSceneChangeReq(short procName) {
    s_leftGameplay = procName != fpcNm_PLAY_SCENE_e;
}

bool leftGameplay() {
    return s_leftGameplay;
}

void setDisplayAvailable(bool available) {
    s_displayAvailable = available;
    if (!available) {
        s_everPresented = false;
    }
}

}  // namespace dusk::dualscreen
