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

// Gamepad passthrough (phase 5) lives in phone_spike_pad.h — a controller
// connected to the phone, forwarded as a real second controller for the
// main game via dolphin::PAD's existing virtual-status injection point.
// dispatch_message() (phone_spike_ws.cpp) parses "pad" messages and calls
// straight into phone_spike_pad.h's set_gamepad_state(); nothing gamepad-
// specific needs to live here.

}  // namespace dusk::phone_spike
