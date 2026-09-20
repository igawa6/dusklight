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

#if DUSK_PHONE_SPIKE_STATE
#include "dusk/companion_state.h"
#include "dusk/companion_map_state.h"
#include "dusk/utilities.hpp"

#include <nlohmann/json.hpp>
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

#if DUSK_PHONE_SPIKE_STATE
// Icon-fetch-on-demand (Phase 2): borrows the SAME single-slot
// aurora::auxwin capture the binary frame path above uses, for one
// substituted frame per icon request, since no CPU-side texture decoder
// exists anywhere in this codebase (see companion_state.h's
// drawPhoneRequestedIcon() doc comment) — getting RGBA8 pixels for any
// icon means drawing it and reading the GPU surface back, same as every
// other capture in this feature.
//
// s_iconCaptureArmed mutually excludes s_spikeCaptureArmed from using the
// capture slot on the same tick (see pollAndPushSpikeFrame()'s re-arm
// condition below and pollAndServeIconRequests()) — request_capture() has
// no queue of its own; calling it while a DIFFERENT capture is already
// Armed/InFlight abandons that other one, exactly the class of bug the
// phone-resize capture-state desync (see s_spikeCaptureArmed's own history
// above) already cost real live-testing time to find once. Treat the
// single capture slot as owned by at most one of these two paths per tick.
bool s_iconCaptureArmed = false;
// True across EVERY endHudCapture() call from when a capture is armed
// until pollAndServeIconRequests() actually drains it (successfully or via
// timeout) — NOT just the one frame it's first set on. Found via live
// testing (Phase 3, hearts): request_capture()/take_capture() is async and
// can take several frames to resolve; clearing this after only the first
// substituted frame meant later frames fell back to the REAL dashboard
// (since nothing else holds the substitution), and if take_capture()
// happened to resolve on one of THOSE later frames instead of the first,
// it silently captured the real dashboard instead of the requested
// icon — items were fast/simple enough to rarely lose this race, hearts
// (probe + two-picture draw with matrix save/restore) were slow enough to
// consistently lose it. Holding the substitution stable across the whole
// wait guarantees whichever frame the capture actually resolves against is
// still the intended content, not whatever happened to render after it.
bool s_iconCaptureDrawPending = false;
// How many endHudCapture() calls have actually drawn the substitution and
// handed it to the aux window via set_source(). request_capture() captures
// the next PRESENTED aux frame, and presentation runs a frame behind the
// set_source() that feeds it — so arming in the same tick the substitution
// first draws captures the frame BEFORE it, i.e. the real dashboard.
// Proven by bisection during live testing: replacing the entire heart draw
// with nothing but a bright magenta full-canvas fillRect STILL captured a
// byte-identical dashboard, so the capture was never reading the
// substituted frame at all — the draw was never the problem. Waiting for a
// couple of fully drawn+sourced substituted frames before arming absorbs
// that pipeline delay.
int s_iconCaptureDrawnFrames = 0;
constexpr int kIconSubstitutionWarmupFrames = 2;
u8 s_iconCaptureItemNo = 0;
// Safety net: endHudCapture() can bail out early (companion not active/
// ready, e.g. a stage transition landing between the arm and the draw)
// without ever fulfilling an armed capture — take_capture() would then
// never succeed, and since s_iconCaptureArmed also blocks the BINARY path
// from re-arming (see above), a stuck icon capture would silently wedge
// frame streaming too, not just icon delivery. Give up after a bounded
// number of frames, same shape as the existing companion-screenshot
// feature's kShotTimeoutFrames (dualscreen.cpp, DUSK_COMPANION_CAPTURE).
int s_iconCaptureWaited = 0;
constexpr int kIconCaptureTimeoutFrames = 240;

// Phase 3: hearts. Unlike items (always immediately drawable by identity —
// see drawPhoneRequestedIcon()), a heart state might not have a LIVE match
// on the current frame at all (see companion_state.h's
// drawWantedHeartIcon() doc comment — it's opportunistic, not forced). This
// capture is armed optimistically as soon as a heart state is wanted, but
// whether it actually finds a match is only known once endHudCapture()
// tries the draw, later the same frame — s_heartDrawFound records that
// result for pollAndServeIconRequests() to check once the capture
// resolves. When no match was found, endHudCapture() draws the normal
// dashboard instead (no visual disruption to the phone's own frame stream)
// and the resulting capture is simply discarded once drained — not sent as
// an icon (it's not heart art), not queued as a streamed frame either (it
// wasn't requested through that path) — and the state stays wanted for a
// later retry.
bool s_iconCaptureIsHeart = false;
u8 s_iconCaptureHeartState = 0;
bool s_heartDrawFound = false;
// Phase 4: the dungeon map's base image, a third substitution kind
// alongside item and heart. Like an item it is always servable when the map
// is up (no opportunistic probe), but unlike either it needs the map
// re-rendered at the canonical whole-floor framing first — which cannot
// happen in the same frame the request is taken, because the game's copy-2D
// pass that produces the texture runs earlier in the frame than this. The
// existing two-frame warmup absorbs that by itself: the override is set on
// the frame the request is taken, so the second warmup frame already
// carries the canonical texture and the armed capture lands on a later one.
bool s_iconCaptureIsMapBase = false;
u32 s_mapBaseCaptureGen = 0;
f32 s_mapBaseFitU0 = 0.0f;
f32 s_mapBaseFitV0 = 0.0f;
f32 s_mapBaseFitU1 = 1.0f;
f32 s_mapBaseFitV1 = 1.0f;
// Set by endHudCapture() when a heart substitution was attempted but found
// no live match. Needed because the warmup counter above only advances on
// frames the substitution actually DREW: a heart with no live match never
// draws, so without this the cycle sits in warmup for the full
// kIconCaptureTimeoutFrames before giving up — and because the binary path
// yields its capture slot for the whole time a substitution is pending,
// that stalls frame streaming too. Live testing measured both halves of
// that: zero heart states delivered and frame throughput down to ~2/s.
// A miss is knowable in one frame, so end the cycle in one frame.
bool s_iconCaptureHeartMissed = false;
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

    bool iconWantsCaptureSlot = false;
#if DUSK_PHONE_SPIKE_STATE
    // Not just "icon capture already armed" — also "an icon_request is
    // waiting to be served". Without yielding for a genuinely pending
    // request too, this path re-arms within the SAME tick it frees the
    // slot (no frame-rate cap since it was removed), so
    // pollAndServeIconRequests() — called right after this, same
    // beginHudCapture() tick — almost never observes the slot free and a
    // request can starve indefinitely. Found via live testing (hud_state
    // worked, icon_request never got a response at all). Skipping exactly
    // one re-arm here costs nothing the phone would notice (one frame late
    // at streaming rates), and pollAndServeIconRequests() takes the slot
    // right back over to streaming once it's done.
    // s_iconCaptureDrawPending covers the warmup window between taking a
    // request and actually arming it (see kIconSubstitutionWarmupFrames):
    // an ITEM request has already been popped off its queue by then, so
    // has_pending_icon_request() no longer reports it, and without this the
    // binary path would grab the capture slot mid-warmup and the icon
    // request would stall until its timeout.
    iconWantsCaptureSlot = s_iconCaptureArmed || s_iconCaptureDrawPending ||
        phone_spike::has_pending_icon_request() || phone_spike::has_pending_map_base_request();
#endif
    if (!s_spikeCaptureArmed && !iconWantsCaptureSlot && phone_spike::has_client()) {
        aurora::auxwin::request_capture();
        s_spikeCaptureArmed = true;
    }
}
#endif  // DUSK_PHONE_SPIKE

#if DUSK_PHONE_SPIKE_STATE
// Phase-1 of the state-streaming fast path (see
// docs/phone-companion-design.md and the state-streaming design plan):
// sends small hud_state diffs instead of pushing whole rendered frames for
// values that change rarely. File-local: one caller (beginHudCapture), no
// header declaration — same shape as pollAndPushSpikeFrame above, but a
// clearly separate, additive path: it never touches capture/encode/send,
// only reads companion::gatherHudState() and diffs it.
static void pollAndPushSpikeState() {
    if (!phone_spike::has_client()) {
        return;
    }
    companion::HudState state;
    if (!companion::gatherHudState(state)) {
        return;
    }
    // Sent directly/synchronously on the game thread, NOT through the
    // background sender thread queue_raw_frame() uses for binary frames:
    // these messages are tiny (well under 200 bytes) and sent only on
    // actual change (rare relative to 15-60fps frame pushes), so a single
    // blocking send_text_frame() call — already bounded by the existing
    // 2-second SO_SNDTIMEO in phone_spike_ws.cpp — is very unlikely to be
    // felt. Confirmed in live testing: a fresh connection gets its first
    // hud_state within ~60ms, and reconnecting without any underlying value
    // changing correctly produces silence (the diff-suppression working as
    // intended, not a stall) — revisit only if this is ever felt to stall
    // the game thread, mirroring the lesson already learned for the binary
    // path.
    static companion::HudState s_lastSent;
    static bool s_haveLastSent = false;
    // A new client has no idea what the last-sent snapshot was, and these
    // statics outlive connections — so without this a reconnecting phone
    // received nothing at all until some value happened to change on its
    // own, leaving its overlay blank or stale indefinitely.
    static uint32_t s_lastClientGen = 0;
    const uint32_t clientGen = phone_spike::client_generation();
    if (clientGen != s_lastClientGen) {
        s_lastClientGen = clientGen;
        s_haveLastSent = false;
    }
    if (s_haveLastSent && state == s_lastSent) {
        return;
    }
    s_lastSent = state;
    s_haveLastSent = true;

    auto slotOrNull = [](u8 itemNo) -> nlohmann::json {
        return itemNo == 0xFF ? nlohmann::json(nullptr) : nlohmann::json(itemNo);
    };
    nlohmann::json j;
    j["type"] = "hud_state";
    j["life"] = state.life;
    j["maxLife"] = state.maxLife;
    j["rupees"] = state.rupees;
    j["maxRupees"] = state.maxRupees;
    j["keys"] = state.keys;
    j["equip"] = {
        {"x", slotOrNull(state.equipX)},
        {"y", slotOrNull(state.equipY)},
        {"slot1", slotOrNull(state.equipSlot1)},
        {"slot2", slotOrNull(state.equipSlot2)},
    };
    // Gauges are sent as percent and only while visible — see HudState's
    // own doc comment for why raw counters would break this path's
    // send-only-on-change premise. A hidden gauge reports 0/false rather
    // than being omitted, so the phone never has to distinguish "absent
    // field" from "not showing".
    j["gauges"] = {
        {"oil", state.oilPct},
        {"oilVisible", state.oilVisible},
        {"oxygen", state.oxygenPct},
        {"oxygenVisible", state.oxygenVisible},
    };
    phone_spike::send_text_frame(j.dump());
}

// Phase 4: the dungeon map, split by update frequency. Unlike
// pollAndPushSpikeState() above, the player half of this genuinely changes
// every frame the player moves, so it goes out through the background
// sender (queue_text_frame) rather than inline — a blocking send() at frame
// rate on the game thread is the exact mistake the binary path already made
// once. See companion_map_state.h for why the map decomposes this way and
// why the base image is framed independently of the dashboard's own
// following view.
static void pollAndPushMapState() {
    if (!phone_spike::has_client()) {
        return;
    }
    static companion::MapState s_lastMap;
    static bool s_haveLastMap = false;
    // Same reconnect resync as pollAndPushSpikeState() — see its comment.
    static uint32_t s_lastMapClientGen = 0;
    const uint32_t mapClientGen = phone_spike::client_generation();
    if (mapClientGen != s_lastMapClientGen) {
        s_lastMapClientGen = mapClientGen;
        s_haveLastMap = false;
    }

    companion::MapState state;
    if (!companion::gatherMapState(state)) {
        // Gone inactive (left the dungeon, map page closed, renderer torn
        // down): tell the phone once so it drops the layer, rather than
        // leaving it drawing a stale map forever.
        if (s_haveLastMap && s_lastMap.active) {
            s_lastMap = companion::MapState{};
            nlohmann::json off;
            off["type"] = "map_state";
            off["active"] = false;
            phone_spike::queue_text_frame(off.dump());
        }
        s_haveLastMap = true;
        return;
    }

    // Quantize the player position before diffing, for the same reason the
    // gauges are quantized: these are floats derived from an interpolated
    // position, so they jitter in the low bits even when the player is
    // standing still, and an exact compare would send every single frame
    // forever. A 1/4096 step is far below one pixel of any phone-sized map.
    auto quantU = [](f32 value) { return (int)(value * 4096.0f + 0.5f); };
    const bool playerMoved = !s_haveLastMap ||
        quantU(state.playerU) != quantU(s_lastMap.playerU) ||
        quantU(state.playerV) != quantU(s_lastMap.playerV) ||
        (int)(state.playerHeadingDeg * 4.0f) != (int)(s_lastMap.playerHeadingDeg * 4.0f);
    const bool structureChanged = !s_haveLastMap || !s_lastMap.active ||
        state.floor != s_lastMap.floor || state.baseGen != s_lastMap.baseGen ||
        state.playerFloor != s_lastMap.playerFloor ||
        state.playerFloorKnown != s_lastMap.playerFloorKnown;
    bool iconsChanged = !s_haveLastMap || state.iconCount != s_lastMap.iconCount;
    for (int i = 0; !iconsChanged && i < state.iconCount; i++) {
        const companion::MapIconState& a = state.icons[i];
        const companion::MapIconState& b = s_lastMap.icons[i];
        iconsChanged = a.kind != b.kind || quantU(a.u) != quantU(b.u) ||
            quantU(a.v) != quantU(b.v) || a.rotDeg != b.rotDeg;
    }
    if (!playerMoved && !structureChanged && !iconsChanged) {
        return;
    }
    s_lastMap = state;
    s_haveLastMap = true;

    nlohmann::json j;
    j["type"] = "map_state";
    j["active"] = true;
    j["floor"] = state.floor;
    j["playerFloor"] = state.playerFloorKnown ? nlohmann::json(state.playerFloor)
                                              : nlohmann::json(nullptr);
    j["baseGen"] = state.baseGen;
    j["player"] = {
        {"u", state.playerU},
        {"v", state.playerV},
        {"heading", state.playerHeadingDeg},
    };
    // The icon list rides along only when it actually changed. Several icons
    // track carried objects (a light ball, an iron ball, a pushed statue)
    // and move at actor rate, so this is not always the rare case the plan
    // assumed — but it is still far cheaper than a re-rendered image, and
    // omitting it entirely on a pure-movement frame keeps the common case
    // to three numbers.
    if (iconsChanged || structureChanged) {
        nlohmann::json icons = nlohmann::json::array();
        for (int i = 0; i < state.iconCount; i++) {
            const companion::MapIconState& icon = state.icons[i];
            icons.push_back({
                {"kind", icon.kind},
                {"u", icon.u},
                {"v", icon.v},
                {"rot", icon.rotDeg},
            });
        }
        j["icons"] = std::move(icons);
    }
    phone_spike::queue_text_frame(j.dump(), /*supersedable=*/true);
}

// Phase 2/3 of the state-streaming path: serves one icon_request (item or
// heart) at a time, across as many frames as its capture cycle needs (arm
// this tick, drain a frame or more later once take_capture() succeeds) —
// see the big comment on s_iconCaptureArmed above for why this and the
// binary frame path treat the single aurora::auxwin capture slot as
// mutually exclusive. Called from beginHudCapture() alongside
// pollAndPushSpikeFrame()/pollAndPushSpikeState(); the actual draw
// substitution happens later the same frame, in endHudCapture().
static void pollAndServeIconRequests() {
    if (s_iconCaptureArmed) {
        std::vector<u8> pixels;
        u32 width = 0;
        u32 height = 0;
        if (aurora::auxwin::take_capture(pixels, &width, &height) && width != 0 && height != 0) {
            s_iconCaptureArmed = false;
            // This capture cycle is over (resolved, whether it's a usable
            // hit or a discarded miss) — clear the substitution hold too,
            // together with s_iconCaptureArmed, so a heart miss cleanly
            // ends this cycle rather than leaving endHudCapture() still
            // trying to substitute on frames where nothing is actually
            // armed anymore. A still-wanted heart state gets a fresh cycle
            // on its next throttled re-arm, not a same-tick retry.
            s_iconCaptureDrawPending = false;
            // Whatever ended this cycle, the canonical-view override must not
            // outlive it, or the dashboard keeps rendering the whole-floor
            // framing instead of following the player.
            companion::setMapBaseCanonicalView(false);
            s_iconCaptureWaited = 0;
            // Heart requests that found no live match this cycle: endHudCapture()
            // drew the normal dashboard instead (see s_heartDrawFound's doc
            // comment above) — this capture is a real dashboard frame, not heart
            // art, so discard it silently and leave the state wanted for a later
            // retry rather than sending it mislabeled as an icon.
            const bool discardHeartMiss = s_iconCaptureIsHeart && !s_heartDrawFound;
            if (!discardHeartMiss && phone_spike::has_client()) {
                size_t pngSize = 0;
                void* png = tdefl_write_image_to_png_file_in_memory_ex(pixels.data(),
                    (int)width, (int)height, 4, &pngSize, 1, MZ_FALSE);
                if (png != NULL) {
                    // Defensive backstop for a bug that IS now root-caused and
                    // fixed (the capture/presentation race — see
                    // s_iconCaptureDrawnFrames): before that fix, every heart
                    // request and the key item came back as a byte-identical
                    // ~845KB copy of the full real dashboard instead of the
                    // requested icon. Kept rather than deleted because the
                    // failure mode it catches is silent and actively harmful —
                    // shipping a miniature screenshot of the game's HUD in place
                    // of an icon is worse than shipping nothing, and the whole
                    // point of fetching art over the wire is that only the
                    // user's own instance ever renders it. A correct 128px icon
                    // on a flat background encodes far below this threshold
                    // (measured: rupee ~130KB, key ~160KB, full heart ~88KB), so
                    // this never fires in normal operation — confirmed zero
                    // drops across every post-fix verification run. A dropped
                    // heart request stays wanted and retries later; a dropped
                    // item request is lost for this connection (items have no
                    // retry path today).
                    // Scaled to the capture's own raw size rather than a fixed
                    // byte count: the surface is whatever resolution the phone
                    // negotiated in its hello, so any absolute limit is only
                    // ever calibrated for one screen. The two populations are
                    // far apart as a FRACTION of raw, which is stable across
                    // resolutions: a whole dashboard compresses to ~22% of raw
                    // (measured 865,623 of 3,840,000 at 800x1200), while real
                    // icons on a flat background land at 1-7% (measured, same
                    // surface: sword 46KB, boomerang 88KB, spinner 175KB,
                    // shield 182KB, bow 242KB = 6.3%). An eighth of raw sits
                    // cleanly between them with room on both sides.
                    //
                    // The earlier fixed 200KB limit was calibrated before the
                    // bow was ever requested and silently dropped it as
                    // "likely-wrong content" — a correct icon, discarded by its
                    // own safety net, with the shield and spinner already
                    // within 10% of tripping it too. Keep a backstop, but not
                    // one that clips the real distribution.
                    // Deliberately NOT applied to the map base image: this
                    // backstop catches a small icon that came back as a
                    // full-canvas screenshot, and the map base IS a
                    // full-canvas image by design, so the same size would
                    // mean something completely different there.
                    const size_t rawBytes = (size_t)width * (size_t)height * 4;
                    const size_t maxSaneIconPngBytes = rawBytes / 8;
                    if (!s_iconCaptureIsMapBase && pngSize > maxSaneIconPngBytes) {
                        DuskLog.warn(
                            "phone spike: icon capture ({} {}) suspiciously large ({} bytes, "
                            "limit {}), dropping rather than sending likely-wrong content",
                            s_iconCaptureIsHeart ? "heart" : "item",
                            s_iconCaptureIsHeart ? s_iconCaptureHeartState : s_iconCaptureItemNo,
                            pngSize, maxSaneIconPngBytes);
                        mz_free(png);
                        return;
                    }
                    const u8* pngBytes = static_cast<const u8*>(png);
                    const std::string b64 =
                        dusk::utils::base64_encode(std::vector<u8>(pngBytes, pngBytes + pngSize));
                    nlohmann::json j;
                    if (s_iconCaptureIsMapBase) {
                        j["type"] = "map_base";
                        // The generation this image corresponds to, so the
                        // phone can tell whether a newly-arrived base is
                        // already stale relative to the map_state it is
                        // currently drawing.
                        j["gen"] = s_mapBaseCaptureGen;
                        // Where the square map sits inside the non-square
                        // captured PNG — the phone places its normalized
                        // player/icon coordinates within this sub-rectangle,
                        // not across the whole image.
                        j["fit"] = {
                            {"u0", s_mapBaseFitU0},
                            {"v0", s_mapBaseFitV0},
                            {"u1", s_mapBaseFitU1},
                            {"v1", s_mapBaseFitV1},
                        };
                        j["png"] = b64;
                        // Queued, NOT sent inline: this payload is hundreds
                        // of kilobytes of base64, and send_text_frame() is
                        // documented for tiny messages sent directly from the
                        // game thread. A blocking socket write of that size
                        // here is the same stall the binary frame path was
                        // moved off-thread to avoid.
                        phone_spike::queue_text_frame(j.dump());
                        mz_free(png);
                        companion::setMapBaseCanonicalView(false);
                        return;
                    }
                    j["type"] = "icon";
                    if (s_iconCaptureIsHeart) {
                        j["kind"] = "heart";
                        j["id"] = s_iconCaptureHeartState;
                        phone_spike::clear_wanted_heart_state(s_iconCaptureHeartState);
                    } else {
                        j["kind"] = "item";
                        j["id"] = s_iconCaptureItemNo;
                    }
                    j["png"] = b64;
                    // Queued for the same reason as the map base above: an
                    // icon's base64 payload is far too large for the inline
                    // game-thread send path.
                    phone_spike::queue_text_frame(j.dump());
                    mz_free(png);
                } else {
                    DuskLog.warn("phone spike: icon PNG encode failed ({} {})",
                        s_iconCaptureIsHeart ? "heart" : "item",
                        s_iconCaptureIsHeart ? s_iconCaptureHeartState : s_iconCaptureItemNo);
                }
            }
        } else if (++s_iconCaptureWaited > kIconCaptureTimeoutFrames) {
            // endHudCapture() never fulfilled this (companion not active
            // this stretch, e.g. a stage transition) — give up so this
            // doesn't wedge the binary frame path (which won't re-arm
            // while s_iconCaptureArmed is true, see
            // pollAndPushSpikeFrame() above) forever over one request that
            // can just be asked for again.
            DuskLog.warn("phone spike: icon capture ({} {}) never completed, giving up",
                s_iconCaptureIsHeart ? "heart" : "item",
                s_iconCaptureIsHeart ? s_iconCaptureHeartState : s_iconCaptureItemNo);
            s_iconCaptureArmed = false;
            s_iconCaptureDrawPending = false;
            // Whatever ended this cycle, the canonical-view override must not
            // outlive it, or the dashboard keeps rendering the whole-floor
            // framing instead of following the player.
            companion::setMapBaseCanonicalView(false);
            s_iconCaptureWaited = 0;
        }
        return;  // draining (or timing out) an in-flight capture takes priority
    }

    // A substitution is already being held but not armed yet: it's warming
    // up (see s_iconCaptureDrawnFrames). Arm only once enough substituted
    // frames have actually been drawn AND handed to the aux window, so the
    // frame the capture reads back is the icon rather than whatever was
    // presented before it.
    if (s_iconCaptureDrawPending) {
        if (s_iconCaptureHeartMissed) {
            // No live pane shows this state right now — nothing was ever
            // drawn, so nothing was ever armed and there's no in-flight
            // capture to drain. End the cycle immediately (see
            // s_iconCaptureHeartMissed) and leave the state wanted; the
            // throttled probe picks it up again, and the rotating want-list
            // cursor means the other wanted states get their turn instead of
            // queueing behind this one.
            s_iconCaptureDrawPending = false;
            // Whatever ended this cycle, the canonical-view override must not
            // outlive it, or the dashboard keeps rendering the whole-floor
            // framing instead of following the player.
            companion::setMapBaseCanonicalView(false);
            s_iconCaptureHeartMissed = false;
            s_iconCaptureWaited = 0;
            return;
        }
        if (s_iconCaptureDrawnFrames >= kIconSubstitutionWarmupFrames) {
            aurora::auxwin::request_capture();
            s_iconCaptureArmed = true;
            s_iconCaptureWaited = 0;
        } else if (++s_iconCaptureWaited > kIconCaptureTimeoutFrames) {
            // The substitution never actually drew (companion inactive for
            // this whole stretch, e.g. a stage transition) — drop it rather
            // than hold the capture slot hostage. A still-wanted heart
            // state simply gets picked up again later.
            s_iconCaptureDrawPending = false;
            // Whatever ended this cycle, the canonical-view override must not
            // outlive it, or the dashboard keeps rendering the whole-floor
            // framing instead of following the player.
            companion::setMapBaseCanonicalView(false);
            s_iconCaptureWaited = 0;
        }
        return;
    }

    if (!phone_spike::has_client() || s_spikeCaptureArmed || !companion::hudReady()) {
        return;
    }
    u8 itemNo = 0;
    u8 heartState = 0;
    if (phone_spike::take_pending_map_base_request()) {
        // First: it's the layer everything else on the map page is drawn
        // relative to, and unlike a heart it never needs retries.
        s_iconCaptureIsMapBase = false;
        if (!companion::mapBaseAvailable()) {
            return;  // no map to capture right now; phone re-requests later
        }
        s_iconCaptureIsMapBase = true;
        s_iconCaptureIsHeart = false;
        s_mapBaseCaptureGen = companion::mapBaseGeneration();
        companion::setMapBaseCanonicalView(true);
    } else if (phone_spike::take_pending_icon_request(itemNo)) {
        // Items first: always immediately servable (an archive texture load,
        // never "not found yet" the way a heart state can be), so there's no
        // reason to make an item request wait behind a heart one that might
        // need many retries.
        s_iconCaptureIsHeart = false;
        s_iconCaptureIsMapBase = false;
        s_iconCaptureItemNo = itemNo;
    } else if (phone_spike::take_next_wanted_heart_state(heartState)) {
        s_iconCaptureIsMapBase = false;
        s_iconCaptureIsHeart = true;
        s_iconCaptureHeartState = heartState;
        s_heartDrawFound = false;
        s_iconCaptureHeartMissed = false;
    } else {
        return;
    }
    // Start substituting now, but DON'T arm the capture yet — see the
    // warmup branch above.
    s_iconCaptureDrawPending = true;
    s_iconCaptureDrawnFrames = 0;
    s_iconCaptureWaited = 0;
}
#endif  // DUSK_PHONE_SPIKE_STATE

void beginHudCapture() {
#if DUSK_COMPANION_CAPTURE
    pollScreenshot();
#endif
#if DUSK_PHONE_SPIKE
    pollAndPushSpikeFrame();
#endif
#if DUSK_PHONE_SPIKE_STATE
    pollAndPushSpikeState();
    pollAndPushMapState();
    pollAndServeIconRequests();
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

    bool drewIcon = false;
#if DUSK_PHONE_SPIKE_STATE
    if (!splash && s_iconCaptureDrawPending) {
        // One-off substitution for a pending icon_request (Phase 2/3 of the
        // state-streaming path) — draws ONLY the requested item/heart icon
        // instead of the real dashboard for this one frame, into the exact
        // same render target request_capture() (armed by
        // pollAndServeIconRequests(), earlier this same beginHudCapture()
        // tick) will read back. Deliberately does NOT set s_everPresented:
        // this isn't a real dashboard presentation, and the second screen
        // should keep showing its last real dashboard frame (the aux
        // window's own present/blit still uses whatever set_source() below
        // hands it, same as any other frame) rather than treat this as the
        // first-ever presentation if it happens to land before one.
        if (s_iconCaptureIsMapBase) {
            // Always servable while the map is up, so there is no miss path
            // like the heart's — but it can still go away mid-cycle (leaving
            // the dungeon, the page changing), in which case this falls
            // through to the normal dashboard draw and the existing timeout
            // ends the cycle rather than sending a wrong image.
            drewIcon = companion::drawMapBaseImage((f32)canvasW, (f32)canvasH, s_mapBaseFitU0,
                s_mapBaseFitV0, s_mapBaseFitU1, s_mapBaseFitV1);
        } else if (s_iconCaptureIsHeart) {
            // Opportunistic: the wanted heart state might not have a live
            // match this frame at all (see s_heartDrawFound's doc comment
            // above). When it doesn't, fall through to the normal dashboard
            // draw below instead — no wasted/blank frame, no visual
            // disruption — and pollAndServeIconRequests() discards the
            // resulting capture once drained rather than sending it.
            s_heartDrawFound = companion::drawWantedHeartIcon(
                s_iconCaptureHeartState, (f32)canvasW, (f32)canvasH);
            drewIcon = s_heartDrawFound;
            if (!s_heartDrawFound && !s_iconCaptureArmed) {
                // Missed during warmup (nothing armed yet): let
                // pollAndServeIconRequests() end the cycle next tick instead
                // of holding the capture slot for the full timeout. A miss
                // AFTER arming is a different case — the capture is already
                // in flight and s_heartDrawFound routes it to the existing
                // discard path.
                s_iconCaptureHeartMissed = true;
            }
        } else {
            companion::drawPhoneRequestedIcon(s_iconCaptureItemNo, (f32)canvasW, (f32)canvasH);
            drewIcon = true;
        }
        if (drewIcon) {
            // Counts frames where the substitution genuinely drew and (via
            // set_source() below) became the aux window's source — this is
            // what pollAndServeIconRequests() waits on before arming, so the
            // capture reads back a substituted frame and not the one
            // presented before it. See s_iconCaptureDrawnFrames.
            ++s_iconCaptureDrawnFrames;
        }
        // Deliberately NOT cleared here — see s_iconCaptureDrawPending's
        // doc comment above (found via live testing: clearing it after one
        // frame raced the async capture readback, which could resolve
        // several frames later against whatever got drawn AFTER the
        // substitution — the real, full dashboard, not the icon).
    }
#endif
    if (splash) {
        companion::drawSplash((f32)canvasW, (f32)canvasH);
    } else if (!drewIcon) {
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
