#pragma once

// Phase-1/3/5 spike for the phone-companion-display idea (see
// docs/phone-companion-design.md): a minimal single-client HTTP+WebSocket
// server that serves a test page on GET, pushes binary companion frames to
// whatever connects over WS, reads touch/pinch JSON messages back
// (dispatched into dusk::companion::touchEvent()/pinchZoom() — same entry
// points the Android JNI touch path uses), and reads gamepad state from a
// controller connected to the PHONE, passed through as a REAL second
// controller for the main game (see phone_spike_pad.h — NOT companion
// navigation; touch stays the only thing that drives the companion
// display). POSIX sockets only (Linux/macOS); not wired up on Windows yet,
// see phone_spike_ws.cpp.
//
// Gated by DUSK_PHONE_SPIKE (dualscreen.h) and off by default. This entire
// file is throwaway scaffolding to get a real capture->encode->push latency
// number, not a production networking layer.

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace dusk::phone_spike {

// Starts the background accept thread on `port` (all interfaces). Returns
// false if the listen socket couldn't be created/bound. Safe to call once;
// a second call while already running is a no-op that returns true.
bool start_server(uint16_t port);

void stop_server();

// True once a browser has completed the WS handshake and is ready for frames.
bool has_client();

// Increments every time a new client is adopted. Senders that diff against a
// last-sent snapshot must re-send everything when this changes, or a
// reconnecting client sees nothing until a value happens to change by itself.
uint32_t client_generation();

// Push one binary WS frame (a PNG-encoded companion capture): the actual
// blocking socket write (short send timeout — see .cpp), called from the
// background sender thread, never the game thread — a real phone over real
// Wi-Fi (unlike every loopback test this spike was originally verified
// against) can make this slow enough to visibly stall a caller running on
// the game thread. Either the whole frame goes out, or the connection is
// torn down and the caller finds out via the return value / has_client()
// going false on the next check. No silent partial-frame writes, which
// would desync the client's WS byte stream irrecoverably. Returns false if
// there is no client or the send failed.
bool send_binary_frame(const void* data, size_t size);

// Push one text WS frame (a JSON state message — see companion_state.h).
// Same framing/locking/error-handling as send_binary_frame (opcode 1 vs 2),
// but intended to be called DIRECTLY from the game thread: these messages
// are tiny and sent only on actual state changes, unlike the per-frame
// binary path, so a direct blocking send is an intentional v1 choice, not
// an oversight — revisit if it's ever measured to stall the game thread.
bool send_text_frame(std::string_view json);

// Game-thread entry point: hands a raw RGBA8 capture off to the background
// sender thread (PNG encode included) and returns immediately, touching
// neither the encoder nor the socket. Takes ownership of `pixels` (moved) —
// PNG-encoding a multi-megapixel frame every ~66ms is itself real CPU time
// (not just the network send), and a real phone over real Wi-Fi made doing
// either synchronously on the game thread visibly stall it. If a previous
// frame is still queued and unsent, it is dropped in favor of the new one,
// so the connection always carries the freshest capture rather than a
// growing backlog of stale ones if the sender falls behind.
void queue_raw_frame(std::vector<uint8_t> pixels, uint32_t width, uint32_t height);

// Queues one JSON text message onto that same background sender, instead of
// sending it inline the way send_text_frame() does. Use this for anything
// sent at frame rate — map_player goes out every frame the player moves, and
// a blocking send() at that rate on the game thread is exactly what made the
// binary frame path stall before it was moved off-thread. The rare,
// change-only hud_state messages still send inline; that stays fine
// precisely because they are rare.
//
// Also use it for anything LARGE — a base64 icon or map image runs to
// hundreds of kilobytes, which is nothing like the "tiny" send_text_frame()
// is documented for.
//
// `supersedable` says whether a later message makes this one redundant. The
// queue is bounded; on overflow it drops the oldest supersedable message, so
// a stale player position is discarded rather than an icon payload or a
// haptic cue, which are edge-triggered and lost for good if dropped. Queued
// texts go out ahead of any queued frame, since a frame costs a
// multi-millisecond PNG encode first.
void queue_text_frame(std::string json, bool supersedable = false);

// Set by the WS receive thread when a "hello" message reports the phone's
// native pixel resolution; consumed by the game thread (dualscreen.cpp),
// never acted on directly here — aurora::auxwin::create()/destroy() are
// documented main/game-thread-only, and the receive thread is neither.
void request_resize(uint32_t width, uint32_t height);

// True and fills width/height if a resize is pending, clearing the pending
// flag in the same call. False (leaves width/height untouched) if nothing
// is pending. Game thread only, by convention with the above.
bool take_pending_resize(uint32_t& width, uint32_t& height);

// Phase-2 state-streaming addition: icon-fetch-on-demand (see
// docs/phone-companion-design.md's state-streaming design plan). Set by the
// WS receive thread when an "icon_request" message arrives; consumed by the
// game thread, never acted on directly here — rendering an icon means
// drawing it and capturing the GPU surface back, main/game-thread only,
// same reason resize can't happen on the receive thread. A bounded FIFO,
// not a single slot like resize: each distinct request needs to actually be
// served, not have a later one silently replace an earlier unconsumed one.
void request_icon(uint8_t itemNo);

// True and fills itemNo with the oldest still-pending request, removing it
// from the queue, if any is pending. False (leaves itemNo untouched)
// otherwise. Game thread only, by convention with the resize pair above.
bool take_pending_icon_request(uint8_t& itemNo);

// Non-consuming: true if a request is waiting, without removing it. The
// binary frame path (pollAndPushSpikeFrame() in dualscreen.cpp) checks this
// before re-arming its own capture — with the frame-rate cap removed, it
// re-arms within the same tick it frees the capture slot, so without this
// check pollAndServeIconRequests() (called right after, same tick) almost
// never observes the slot free and an icon_request can starve indefinitely.
// This makes the binary path skip exactly one re-arm when something is
// genuinely waiting, handing the now-free slot to the icon path instead —
// found via live testing (zero icon responses ever arrived despite
// hud_state working fine), not anticipated in the original design.
bool has_pending_icon_request();

// Phase-3 state-streaming addition: heart-container icon fetch (see
// docs/phone-companion-design.md's state-streaming design plan and
// companion_state.h's drawWantedHeartIcon() doc comment for why hearts need
// different handling than items). `state` is 0-4 (empty/quarter/half/
// three-quarter/full). Unlike request_icon()'s FIFO — a heart state isn't
// always immediately available (it only exists once the player's HP has
// actually produced it), so this is a persistent WANT flag, not a
// one-shot queued request: set once, and the game thread keeps retrying
// (via take_next_wanted_heart_state()) until it's actually found and
// served, however many frames that takes, rather than being silently
// dropped after one unsuccessful attempt.
void request_heart_icon(uint8_t state);

// Phase 4: the dungeon map's base image (icon_request with kind
// "map_base"). A single latch rather than a queue or a want-list — there is
// only one current base image, so a repeat request while one is pending is
// the same request, not more work. take_ clears it; has_ lets the binary
// frame path yield its capture slot the same way it does for icons.
// Phase 4: one dungeon-map overlay icon by ICON_*_e kind (icon_request with
// kind "map_icon"). A FIFO like the item queue — each distinct kind must be
// served, not superseded. Served from a CPU texture decode, so unlike the
// map base it never touches the capture slot.
// Companion pages the phone renders natively, as a bitmask of page indices.
// While the page on screen is owned, the frame stream is suppressed outright
// — see pollAndPushSpikeFrame(). Zero (the default, and what any older
// client implies) means the PC keeps sending pictures for everything.
void set_client_owned_pages(uint32_t mask);
uint32_t client_owned_pages();

// A DECISION the phone already made, rather than a touch point for the PC to
// hit-test. Once the phone draws the tab strip and the inventory grid itself
// it knows what a tap meant before the packet leaves; sending coordinates
// back for the PC to re-derive is the round trip the whole partition exists
// to delete.
//
// Same threading contract as request_icon() above and for a stronger reason:
// dispatch_message() runs on the WS receive thread, and every executor an
// action reaches writes SAVEDATA. None of it may run there. This is an
// enqueue and nothing else; companion_touch.cpp's
// applyPendingCompanionActions() drains it on the game thread.
struct CompanionAction {
    enum Verb : uint8_t {
        SetPage,     // page
        SelectSlot,  // slot (-1 clears the selection)
        Equip,       // button + slot/itemNo, with combo already resolved
    };
    // How an ambiguous drop (a bomb bag or the hawkeye onto a button that
    // already carries the bow) was resolved. The PC's own touch path raises a
    // modal two-plate chooser for this, but a phone-side chooser needs
    // nothing from the PC — bowComboAmbiguous() reads only the four mix
    // indices, which the phone already holds — so the phone resolves it and
    // sends the answer. The PC chooser is never raised for an action: the
    // phone cannot see it and no phone message can dismiss it.
    enum Combo : uint8_t {
        ComboReplace = 0,  // plain equip; also the default when unspecified
        ComboArm = 1,      // arm the combo
    };

    uint8_t verb = SetPage;
    uint8_t combo = ComboReplace;
    // True when the equip came from a DRAG rather than a tap. The two
    // deliberately differ: a tap-equip clears the inventory selection, a
    // drag-equip leaves it alone (companion_touch.cpp's release path).
    bool fromDrag = false;
    int32_t page = -1;
    int32_t slot = -1;    // inventory slot; -1 means "resolve from itemNo"
    int32_t itemNo = -1;  // -1 means "not given"
    int32_t button = -1;  // 0 = X, 1 = Y, 2 = slot I, 3 = slot II
    // The phone's monotonic intent number, echoed back as hud_state's "ack"
    // once this has been applied OR refused. 0 = unsequenced: the action
    // still runs, it just never moves the ack.
    uint32_t seq = 0;
};

// Enqueue only. A bounded FIFO like the icon queue, not a latest-value slot
// like resize: two taps are two decisions and the first must not be dropped
// in favour of the second. On overflow the OLDEST goes, which for input is
// the right end to lose — a backlog that deep means the game thread is
// stalled and the newest tap is the one the player is waiting on, the same
// rule pushPendingTap() already applies to taps.
void request_action(const CompanionAction& action);

// Pops the oldest queued action. False (leaving `out` untouched) when none
// is queued. Game thread only, by convention with the pairs above.
bool take_pending_action(CompanionAction& out);

void request_map_icon(uint8_t kind);
bool take_pending_map_icon_request(uint8_t& kind);

void request_map_base();
bool take_pending_map_base_request();
bool has_pending_map_base_request();

// True and fills state with a still-wanted heart state (0-4) if one is
// set AND a probe is due this tick (throttled to roughly once every 30
// frames, shared with has_pending_icon_request()'s yield check — see
// g_heartProbeCounter in the .cpp: a capture cycle costs real GPU work
// even when it finds nothing, and unthrottled probing would also starve
// the binary frame-streaming path for as long as a heart state stays
// unmet). Does NOT clear the want flag — the caller (dualscreen.cpp) only
// calls clear_wanted_heart_state() once it actually finds and captures a
// live match; if no live match exists yet, the state stays wanted and
// this returns it again on a later probe. Game thread only, by convention
// with the other pairs above.
bool take_next_wanted_heart_state(uint8_t& state);

// Clears one state's want flag once it has actually been captured and
// sent. A later re-request (e.g. after a reconnect) sets it again.
void clear_wanted_heart_state(uint8_t state);

// Gamepad passthrough (phase 5) lives in phone_spike_pad.h — a controller
// connected to the phone, forwarded as a real second controller for the
// main game via dolphin::PAD's existing virtual-status injection point.
// dispatch_message() (phone_spike_ws.cpp) parses "pad" messages and calls
// straight into phone_spike_pad.h's set_gamepad_state(); nothing gamepad-
// specific needs to live here.

}  // namespace dusk::phone_spike
