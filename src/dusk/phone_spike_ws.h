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

// Game-thread entry point: hands an already-PNG-encoded frame off to the
// background sender thread and returns immediately, without touching the
// socket. Copies `data` — the caller's buffer can be freed right after this
// returns. If a previous frame is still queued and unsent, it is dropped in
// favor of the new one, so the connection always carries the freshest
// capture rather than a growing backlog of stale ones if the sender falls
// behind (e.g. a slow/congested real Wi-Fi link).
void queue_binary_frame(const void* data, size_t size);

// Set by the WS receive thread when a "hello" message reports the phone's
// native pixel resolution; consumed by the game thread (dualscreen.cpp),
// never acted on directly here — aurora::auxwin::create()/destroy() are
// documented main/game-thread-only, and the receive thread is neither.
void request_resize(uint32_t width, uint32_t height);

// True and fills width/height if a resize is pending, clearing the pending
// flag in the same call. False (leaves width/height untouched) if nothing
// is pending. Game thread only, by convention with the above.
bool take_pending_resize(uint32_t& width, uint32_t& height);

// Gamepad passthrough (phase 5) lives in phone_spike_pad.h — a controller
// connected to the phone, forwarded as a real second controller for the
// main game via dolphin::PAD's existing virtual-status injection point.
// dispatch_message() (phone_spike_ws.cpp) parses "pad" messages and calls
// straight into phone_spike_pad.h's set_gamepad_state(); nothing gamepad-
// specific needs to live here.

}  // namespace dusk::phone_spike
