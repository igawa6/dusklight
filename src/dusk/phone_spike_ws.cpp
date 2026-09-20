#include "dusk/phone_spike_ws.h"

#include "dusk/dualscreen.h"

#if DUSK_PHONE_SPIKE

#if defined(_WIN32)
#error "phone_spike_ws.cpp is POSIX-sockets-only for now; not wired up for Windows. See file header."
#endif

#include "dusk/companion.h"
#include "dusk/logging.h"
#include "dusk/phone_spike_pad.h"
#include "dusk/phone_spike_pairing.h"
#include "dusk/phone_spike_sha1.h"

#include "miniz.h"

#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace dusk::phone_spike {
namespace {

// Deliberately tiny: a <canvas>, a WebSocket to the same host on this same
// port, binary frames drawn via createImageBitmap, and a rolling frame-
// interval readout so the "real latency number" the spike exists to get is
// visible without needing devtools. No pairing/token — manual URL entry only.
constexpr std::string_view kTestPage = R"HTML(<!DOCTYPE html>
<html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>dusklight companion spike</title>
<style>
  html,body{margin:0;background:#111;color:#ddd;font:14px monospace;height:100%}
  #wrap{display:flex;flex-direction:column;height:100%}
  #stats{padding:6px 10px;background:#222}
  canvas{flex:1;width:100%;height:100%;object-fit:contain;background:#000}
</style></head>
<body><div id="wrap">
<div id="stats">connecting...</div>
<canvas id="c"></canvas>
</div>
<script>
const stats = document.getElementById('stats');
const canvas = document.getElementById('c');
const ctx = canvas.getContext('2d');
// Carries the pairing token from the page's own URL (set by whatever
// scanned the QR code) straight into the WS upgrade request's query string.
const ws = new WebSocket(`ws://${location.host}/${location.search}`);
ws.binaryType = 'arraybuffer';
let lastFrameAt = 0;
let intervals = [];
ws.onopen = () => {
  stats.textContent = 'connected, waiting for first frame...';
  // Native device pixels, not CSS logical pixels — this is what should
  // actually get rendered, matching computeAuxCanvas()'s native-resolution
  // path on the PC side (see dualscreen.cpp).
  const w = Math.round(window.innerWidth * (window.devicePixelRatio || 1));
  const h = Math.round(window.innerHeight * (window.devicePixelRatio || 1));
  ws.send(JSON.stringify({type: 'hello', width: w, height: h}));
};
ws.onclose = () => { stats.textContent = 'disconnected'; };
ws.onerror = (e) => { stats.textContent = 'error (see console)'; console.error(e); };
ws.onmessage = async (ev) => {
  const now = performance.now();
  if (lastFrameAt !== 0) {
    intervals.push(now - lastFrameAt);
    if (intervals.length > 30) intervals.shift();
  }
  lastFrameAt = now;
  const blob = new Blob([ev.data], {type: 'image/png'});
  const bitmap = await createImageBitmap(blob);
  if (canvas.width !== bitmap.width || canvas.height !== bitmap.height) {
    canvas.width = bitmap.width;
    canvas.height = bitmap.height;
  }
  ctx.drawImage(bitmap, 0, 0);
  bitmap.close();
  const avg = intervals.reduce((a, b) => a + b, 0) / (intervals.length || 1);
  stats.textContent = `${bitmap.width}x${bitmap.height} | ` +
    `avg frame interval ${avg.toFixed(1)}ms (${(1000/avg).toFixed(1)} fps) | ` +
    `${ev.data.byteLength} bytes last frame`;
};

// Maps a pointer event to normalized 0-1 content coordinates, undoing the
// CSS object-fit:contain letterbox by hand (there's no DOM API for it) —
// same u/v convention dusk::companion::touchEvent already uses today via the
// Android touch path, so the PC side needs zero translation.
function pointerToUV(e) {
  const rect = canvas.getBoundingClientRect();
  const cw = canvas.width, ch = canvas.height;
  if (cw === 0 || ch === 0 || rect.width === 0 || rect.height === 0) return null;
  const scale = Math.min(rect.width / cw, rect.height / ch);
  const dispW = cw * scale, dispH = ch * scale;
  const offX = (rect.width - dispW) / 2, offY = (rect.height - dispH) / 2;
  const x = e.clientX - rect.left - offX;
  const y = e.clientY - rect.top - offY;
  if (x < 0 || y < 0 || x > dispW || y > dispH) return null;
  return {u: x / dispW, v: y / dispH};
}
canvas.addEventListener('pointerdown', (e) => {
  const p = pointerToUV(e);
  if (p && ws.readyState === WebSocket.OPEN) {
    ws.send(JSON.stringify({type: 'touch', action: 0, u: p.u, v: p.v}));
  }
  canvas.setPointerCapture(e.pointerId);
});
canvas.addEventListener('pointermove', (e) => {
  if (e.buttons === 0) return;
  const p = pointerToUV(e);
  if (p && ws.readyState === WebSocket.OPEN) {
    ws.send(JSON.stringify({type: 'touch', action: 1, u: p.u, v: p.v}));
  }
});
canvas.addEventListener('pointerup', (e) => {
  if (ws.readyState !== WebSocket.OPEN) return;
  const p = pointerToUV(e) || {u: 0, v: 0};
  ws.send(JSON.stringify({type: 'touch', action: 2, u: p.u, v: p.v}));
});

// Gamepad passthrough (phase 5) — a gamepad connected to the PHONE, not the
// PC, forwarded as a REAL second controller for the main game (NOT
// companion navigation — the companion stays touch-only). Continuous full
// state sent every animation frame, not discrete edge-triggered events:
// the PC needs to know what's held right now, matching how a real
// controller is read. Standard Gamepad mapping: buttons[0..3] = A/B/X/Y,
// [4] = left shoulder -> Z (Twilight Princess leans on Z-target enough to
// deserve an easy button), [6]/[7] = analog L/R triggers, [9] = Start,
// [12..15] = D-pad. axes[0..1] = left stick (main), axes[2..3] = right
// stick (C-stick).
function pollGamepad() {
  const pads = navigator.getGamepads ? navigator.getGamepads() : [];
  const pad = pads && pads[0];
  if (pad && ws.readyState === WebSocket.OPEN) {
    const b = pad.buttons;
    const pressed = i => !!(b[i] && b[i].pressed);
    const value = i => (b[i] && b[i].value) || 0;
    ws.send(JSON.stringify({
      type: 'pad',
      up: pressed(12), down: pressed(13), left: pressed(14), right: pressed(15),
      a: pressed(0), b: pressed(1), x: pressed(2), y: pressed(3),
      start: pressed(9), z: pressed(4),
      l: pressed(6), r: pressed(7),
      leftStickX: pad.axes[0] || 0, leftStickY: pad.axes[1] || 0,
      rightStickX: pad.axes[2] || 0, rightStickY: pad.axes[3] || 0,
      leftTrigger: value(6), rightTrigger: value(7),
    }));
  }
  requestAnimationFrame(pollGamepad);
}
requestAnimationFrame(pollGamepad);
</script></body></html>)HTML";

std::mutex g_clientMutex;
int g_clientFd = -1;
std::thread g_acceptThread;
std::atomic<bool> g_running{false};
int g_listenFd = -1;

// Single-slot latest-frame hand-off, same shape as phone_spike_pad.h's
// gamepad state: the game thread only ever needs to hand off the freshest
// RAW capture, never a backlog — see queue_raw_frame()'s doc comment for
// why this exists (a real phone over real Wi-Fi, unlike every earlier
// loopback test, can make either the PNG encode or the send slow enough to
// visibly stall the game thread if done synchronously there).
std::mutex g_pendingFrameMutex;
std::condition_variable g_pendingFrameCv;
// Small JSON state messages waiting to go out, sharing the frame sender's
// thread and mutex. They must NOT be sent straight from the game thread the
// way the rare hud_state messages are: map_player is sent every frame the
// player moves, and a blocking send() at that rate is precisely what made
// the binary frame path stall the game before it was moved off-thread.
std::deque<std::string> g_pendingTexts;
std::vector<uint8_t> g_pendingFramePixels;
uint32_t g_pendingFrameWidth = 0;
uint32_t g_pendingFrameHeight = 0;
bool g_pendingFrameReady = false;
std::thread g_senderThread;

std::mutex g_resizeMutex;
bool g_resizePending = false;
uint32_t g_pendingResizeW = 0;
uint32_t g_pendingResizeH = 0;

// Phase-2 state-streaming addition: pending "item" icon-fetch requests
// (kind: "item" only — see dispatch_message()'s icon_request branch; other
// kinds are reserved for later phases). Unlike resize's single-slot
// latest-value semantics (only the newest size matters), each DISTINCT icon
// request must actually be served — a bounded FIFO, not a single slot, so
// e.g. the phone requesting its X, Y, and slot-I icons back to back all get
// answered instead of the first two being silently dropped in favor of the
// last. Capped small: if this is ever actually overflowing, something
// upstream is requesting unreasonably many icons, not a real usage pattern
// this needs to handle gracefully — drop the oldest and move on rather than
// grow unbounded.
constexpr size_t kMaxPendingIconRequests = 16;
std::mutex g_iconRequestMutex;
std::deque<uint8_t> g_pendingIconRequests;

// Phase-4 addition: the dungeon map's base image. A single latch, not a
// queue and not a want-list — there is only one current base image, so a
// second request arriving while one is already pending is the SAME request
// rather than another unit of work. Shares g_iconRequestMutex because it is
// consumed by the same game-thread capture state machine.
bool g_mapBaseWanted = false;

// Phase-3 addition: wanted heart-container states (0-4). A persistent
// want-list, not a FIFO like the item queue above — see
// request_heart_icon()'s doc comment in the header for why: a heart state
// isn't always immediately servable (it only exists once the player's HP
// has actually produced it), so "dequeue once, drop if not found" would
// silently lose real requests. Same mutex as the item queue: both are tiny,
// low-frequency, and never held long — no benefit to separating them.
bool g_wantedHeartStates[5] = {};

// Throttles heart probing: a live-pane scan cycle costs a full GPU
// capture, even when nothing is found — cheap for items (an archive
// texture load always succeeds immediately) but wasteful to attempt every
// single frame for a heart state that might have no live match for
// seconds. Worse than wasteful: wanting a heart makes
// has_pending_icon_request() report true, which makes the binary
// frame-streaming path yield its capture slot (see dualscreen.cpp's
// pollAndPushSpikeFrame()) — probing every tick would starve normal
// streaming for the WHOLE time a heart state stays unmet, not just
// briefly. Throttled to roughly once every 30 frames; non-probe ticks
// report "nothing wanted" to both the yield check and the arm logic, so
// normal streaming proceeds completely undisturbed between probes.
// has_pending_icon_request() computes and caches the decision (it runs
// first each tick, via the binary path's yield check); take_next_wanted_
// heart_state() reads the cached decision rather than recomputing it, so
// both agree on the same tick.
constexpr int kHeartProbeIntervalFrames = 30;
int g_heartProbeCounter = 0;
bool g_heartProbeDueThisTick = false;
// Round-robin start index for the want-list scan. A plain 0..4 scan
// head-of-line-blocks: whichever wanted state has the LOWEST index is
// retried forever, and the states behind it are never probed at all while
// it stays unavailable. That isn't hypothetical — state 0 (a genuinely
// empty heart container) simply does not exist on screen while the player
// is at full health, so a client asking for all 5 states got none of them,
// not four of them. Rotating the scan start past each state taken means one
// permanently-unavailable state costs a probe slot, not the whole feature.
uint8_t g_heartProbeCursor = 0;

bool read_http_headers(int fd, std::string& out) {
    // Cap well above any real browser request's header size; anything past
    // this is either not HTTP or malformed, either way not worth parsing.
    constexpr size_t kMaxHeaderBytes = 8192;
    char buf[1024];
    while (out.size() < kMaxHeaderBytes) {
        const ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            return false;
        }
        out.append(buf, static_cast<size_t>(n));
        if (out.find("\r\n\r\n") != std::string::npos) {
            return true;
        }
    }
    return false;
}

// Case-insensitive single-header lookup. Good enough for the handful of
// headers this needs to read; not a general HTTP parser.
std::string find_header(const std::string& request, std::string_view name) {
    size_t pos = 0;
    while (pos < request.size()) {
        const size_t lineEnd = request.find("\r\n", pos);
        if (lineEnd == std::string::npos) {
            break;
        }
        const std::string_view line(request.data() + pos, lineEnd - pos);
        if (line.size() > name.size() && line[name.size()] == ':') {
            bool match = true;
            for (size_t i = 0; i < name.size(); ++i) {
                if (std::tolower(static_cast<unsigned char>(line[i])) !=
                    std::tolower(static_cast<unsigned char>(name[i])))
                {
                    match = false;
                    break;
                }
            }
            if (match) {
                size_t valueStart = name.size() + 1;
                while (valueStart < line.size() && line[valueStart] == ' ') {
                    ++valueStart;
                }
                return std::string(line.substr(valueStart));
            }
        }
        pos = lineEnd + 2;
    }
    return {};
}

// Pulls a query-string parameter out of the request LINE ("GET
// /?token=abc HTTP/1.1" -> "abc" for name="token"). No URL-decoding — the
// only value this ever needs to read is a hex token this same server
// generated, which never contains characters that would need it.
std::string find_query_param(const std::string& request, std::string_view name) {
    const size_t lineEnd = request.find("\r\n");
    const std::string_view requestLine(request.data(), lineEnd == std::string::npos ? request.size() : lineEnd);
    const size_t qmark = requestLine.find('?');
    if (qmark == std::string_view::npos) {
        return {};
    }
    const size_t spaceAfter = requestLine.find(' ', qmark);
    const std::string_view query =
        requestLine.substr(qmark + 1, (spaceAfter == std::string_view::npos ? requestLine.size() : spaceAfter) - qmark - 1);
    size_t pos = 0;
    while (pos < query.size()) {
        const size_t amp = query.find('&', pos);
        const std::string_view pair = query.substr(pos, (amp == std::string_view::npos ? query.size() : amp) - pos);
        const size_t eq = pair.find('=');
        if (eq != std::string_view::npos && pair.substr(0, eq) == name) {
            return std::string(pair.substr(eq + 1));
        }
        if (amp == std::string_view::npos) {
            break;
        }
        pos = amp + 1;
    }
    return {};
}

void send_all(int fd, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        const ssize_t n = send(fd, data + sent, len - sent, 0);
        if (n <= 0) {
            return;
        }
        sent += static_cast<size_t>(n);
    }
}

bool recv_exact(int fd, uint8_t* buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        const ssize_t r = recv(fd, buf + got, n - got, 0);
        if (r <= 0) {
            return false;
        }
        got += static_cast<size_t>(r);
    }
    return true;
}

enum class Opcode : uint8_t { Continuation = 0x0, Text = 0x1, Binary = 0x2, Close = 0x8, Ping = 0x9, Pong = 0xA };

// Reads one client->server WS frame. Client frames are ALWAYS masked (RFC
// 6455 §5.1) — unlike send_binary_frame's server->client path, which never
// masks. No fragmentation support (FIN=0 is treated as an error and closes
// the connection): every message this spike receives is a small one-shot
// JSON blob from a real browser, which never fragments payloads this size.
bool read_ws_frame(int fd, Opcode& opcodeOut, std::string& payloadOut) {
    uint8_t head[2];
    if (!recv_exact(fd, head, 2)) {
        return false;
    }
    const bool fin = (head[0] & 0x80) != 0;
    const auto opcode = static_cast<Opcode>(head[0] & 0x0F);
    const bool masked = (head[1] & 0x80) != 0;
    uint64_t len = head[1] & 0x7F;
    if (!fin) {
        return false;  // fragmentation not supported, see above
    }
    if (len == 126) {
        uint8_t ext[2];
        if (!recv_exact(fd, ext, 2)) {
            return false;
        }
        len = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
    } else if (len == 127) {
        uint8_t ext[8];
        if (!recv_exact(fd, ext, 8)) {
            return false;
        }
        len = 0;
        for (int i = 0; i < 8; ++i) {
            len = (len << 8) | ext[i];
        }
    }
    constexpr uint64_t kMaxPayload = 64 * 1024;  // input messages are tiny; anything past this is bogus
    if (len > kMaxPayload) {
        return false;
    }
    uint8_t maskKey[4] = {0, 0, 0, 0};
    if (masked) {
        if (!recv_exact(fd, maskKey, 4)) {
            return false;
        }
    }
    payloadOut.resize(static_cast<size_t>(len));
    if (len > 0 && !recv_exact(fd, reinterpret_cast<uint8_t*>(payloadOut.data()), static_cast<size_t>(len))) {
        return false;
    }
    if (masked) {
        for (size_t i = 0; i < payloadOut.size(); ++i) {
            payloadOut[i] = static_cast<char>(static_cast<uint8_t>(payloadOut[i]) ^ maskKey[i % 4]);
        }
    }
    opcodeOut = opcode;
    return true;
}

void send_control_frame(int fd, Opcode opcode, std::string_view payload = {}) {
    uint8_t head[2] = {static_cast<uint8_t>(0x80 | static_cast<uint8_t>(opcode)),
        static_cast<uint8_t>(payload.size())};
    send_all(fd, reinterpret_cast<char*>(head), 2);
    if (!payload.empty()) {
        send_all(fd, payload.data(), payload.size());
    }
}

void dispatch_message(std::string_view json) {
    nlohmann::json msg;
    try {
        msg = nlohmann::json::parse(json);
    } catch (const nlohmann::json::exception& e) {
        DuskLog.warn("phone spike: malformed input JSON: {}", e.what());
        return;
    }
    const std::string type = msg.value("type", "");
    if (type == "touch") {
        companion::touchEvent(msg.value("action", 0), msg.value("u", 0.0f), msg.value("v", 0.0f));
    } else if (type == "pinch") {
        companion::pinchZoom(msg.value("factor", 1.0f));
    } else if (type == "hello") {
        // Native device pixels, already multiplied by devicePixelRatio on
        // the JS side — see the test page's sendHello(). Bounds/sanity
        // clamping happens on the game thread when this is consumed (see
        // dualscreen.cpp), not here; this just hands the raw report off.
        request_resize(msg.value("width", 0u), msg.value("height", 0u));
    } else if (type == "icon_request") {
        // "item" (Phase 2): equipped X/Y/slot-I/II items, plus the rupee
        // gem and key icon, which are confirmed to already be ordinary
        // archive item icons — see companion_state.cpp's
        // drawPhoneRequestedIcon(). "heart" (Phase 3): one of the 5
        // heart-container states — see companion_state.cpp's
        // drawWantedHeartIcon(). "map_icon" is reserved for a later phase;
        // silently ignored for now rather than warned on, since a phone
        // built against the full protocol may legitimately send it before
        // this PC supports it.
        const std::string kind = msg.value("kind", "");
        if (kind == "item") {
            request_icon(static_cast<uint8_t>(msg.value("id", 0)));
        } else if (kind == "heart") {
            request_heart_icon(static_cast<uint8_t>(msg.value("id", 0)));
        } else if (kind == "map_base") {
            // Phase 4: the dungeon map's base image. A single latch rather
            // than a queue or a want-list — there is only ever one current
            // base image, so a second request while one is pending is the
            // same request, not another unit of work.
            request_map_base();
        }
    } else if (type == "pad") {
        // Full continuous state, not a discrete event — see
        // phone_spike_pad.h. Sent every animation frame by the test page,
        // so this just latches the newest report; applyGamepadPassthrough()
        // (game thread) turns it into a real PADStatus each game frame.
        GamepadState state;
        state.up = msg.value("up", false);
        state.down = msg.value("down", false);
        state.left = msg.value("left", false);
        state.right = msg.value("right", false);
        state.a = msg.value("a", false);
        state.b = msg.value("b", false);
        state.x = msg.value("x", false);
        state.y = msg.value("y", false);
        state.start = msg.value("start", false);
        state.l = msg.value("l", false);
        state.r = msg.value("r", false);
        state.z = msg.value("z", false);
        state.leftStickX = msg.value("leftStickX", 0.0f);
        state.leftStickY = msg.value("leftStickY", 0.0f);
        state.rightStickX = msg.value("rightStickX", 0.0f);
        state.rightStickY = msg.value("rightStickY", 0.0f);
        state.leftTrigger = msg.value("leftTrigger", 0.0f);
        state.rightTrigger = msg.value("rightTrigger", 0.0f);
        set_gamepad_state(state);
    } else {
        DuskLog.warn("phone spike: unknown input message type '{}'", type);
    }
}

// One thread per connection, spawned right after the handshake in
// handle_connection(). Reads on its own — send_binary_frame() writes to the
// same fd from the game thread concurrently, which is fine: POSIX sockets
// are full-duplex and support independent reader/writer threads. Exits on
// its own once the fd it captured is closed (either the client left, or
// set_client() closed it to make room for a newer connection) — nothing
// else needs to track or join this thread.
void client_receive_loop(int fd) {
    for (;;) {
        Opcode opcode;
        std::string payload;
        if (!read_ws_frame(fd, opcode, payload)) {
            return;
        }
        switch (opcode) {
        case Opcode::Text:
            dispatch_message(payload);
            break;
        case Opcode::Ping:
            send_control_frame(fd, Opcode::Pong, payload);
            break;
        case Opcode::Close:
            send_control_frame(fd, Opcode::Close);
            return;
        default:
            break;  // binary/pong/continuation: nothing this spike expects
        }
    }
}

void set_client(int fd) {
    std::lock_guard lock{g_clientMutex};
    if (g_clientFd >= 0) {
        close(g_clientFd);
    }
    g_clientFd = fd;
    if (fd >= 0) {
        // Bounded-blocking sends on the background sender thread: never hang
        // forever on a stalled/dead client, but never silently write a
        // partial frame either — see send_binary_frame's contract in the
        // header. 50ms was tuned back when this ran synchronously on the
        // GAME thread and needed to fail fast; now that it's off that
        // thread (queue_binary_frame() + the sender thread — see above),
        // blocking longer costs nothing, so this can afford to tolerate
        // real Wi-Fi throughput dips instead of treating every one as a
        // dead connection.
        timeval tv{.tv_sec = 2, .tv_usec = 0};
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        const int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }
}

void handle_connection(int fd) {
    std::string request;
    if (!read_http_headers(fd, request)) {
        close(fd);
        return;
    }

    const std::string upgrade = find_header(request, "Upgrade");
    bool isWsUpgrade = upgrade.size() >= 9;
    if (isWsUpgrade) {
        for (size_t i = 0; i < 9; ++i) {
            if (std::tolower(static_cast<unsigned char>(upgrade[i])) !=
                std::tolower(static_cast<unsigned char>("websocket"[i])))
            {
                isWsUpgrade = false;
                break;
            }
        }
    }

    if (!isWsUpgrade) {
        // Plain GET: serve the test page and close. The phone then opens a
        // fresh connection for the WS upgrade.
        const std::string body(kTestPage);
        std::string response = "HTTP/1.1 200 OK\r\n";
        response += "Content-Type: text/html; charset=utf-8\r\n";
        response += "Content-Length: ";
        response += std::to_string(body.size());
        response += "\r\n";
        response += "Connection: close\r\n\r\n";
        response += body;
        send_all(fd, response.data(), response.size());
        close(fd);
        return;
    }

    const std::string key = find_header(request, "Sec-WebSocket-Key");
    if (key.empty()) {
        close(fd);
        return;
    }

    // Token gate: this is the actual security boundary (the plain HTML GET
    // above is unauthenticated on purpose — it's static and harmless; a live
    // session that can read frames and inject input is the sensitive part).
    // Only checked here, not the QR-scan step itself.
    const std::string token = find_query_param(request, "token");
    if (token.empty() || token != current_token()) {
        DuskLog.warn("phone spike: rejected WS upgrade with {} token", token.empty() ? "missing" : "wrong");
        static constexpr std::string_view kForbidden = "HTTP/1.1 403 Forbidden\r\nConnection: close\r\n\r\n";
        send_all(fd, kForbidden.data(), kForbidden.size());
        close(fd);
        return;
    }

    const std::string accept = websocket_accept_key(key);
    const std::string response = "HTTP/1.1 101 Switching Protocols\r\n"
                                 "Upgrade: websocket\r\n"
                                 "Connection: Upgrade\r\n"
                                 "Sec-WebSocket-Accept: " +
        accept + "\r\n\r\n";
    send_all(fd, response.data(), response.size());

    DuskLog.info("phone spike: client connected");
    set_client(fd);
    // Detached: self-terminates when this fd is closed (client left, or a
    // newer connection replaced it in set_client()) — see client_receive_loop.
    std::thread(client_receive_loop, fd).detach();
}

void accept_loop(uint16_t port) {
    while (g_running.load()) {
        sockaddr_in clientAddr{};
        socklen_t clientAddrLen = sizeof(clientAddr);
        const int fd = accept(g_listenFd, reinterpret_cast<sockaddr*>(&clientAddr), &clientAddrLen);
        if (fd < 0) {
            if (!g_running.load()) {
                break;
            }
            continue;
        }
        // One connection handled at a time, synchronously, on this thread —
        // fine for a single-client spike; a stalled handshake just delays
        // the next accept, it can't touch the game thread.
        handle_connection(fd);
    }
}

}  // namespace

void sender_loop();  // defined below, after send_binary_frame

bool start_server(uint16_t port) {
    if (g_running.load()) {
        return true;
    }
    // A send() to a socket whose peer already closed raises SIGPIPE, whose
    // default disposition is to KILL THE WHOLE PROCESS — not just fail that
    // call. Hit this for real: closing the test client mid-push took the
    // entire game down with it, not just the WS connection. Global and
    // process-wide, not per-socket (no portable per-call equivalent that
    // works identically on both Linux's MSG_NOSIGNAL and macOS's
    // SO_NOSIGPIPE), but standard practice for anything doing socket I/O —
    // every failed send/recv already returns EPIPE/an error through the
    // normal return-value path, which this code already checks.
    std::signal(SIGPIPE, SIG_IGN);

    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        DuskLog.warn("phone spike: socket() failed: {}", strerror(errno));
        return false;
    }
    const int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        DuskLog.warn("phone spike: bind() to port {} failed: {}", port, strerror(errno));
        close(fd);
        return false;
    }
    if (listen(fd, 1) != 0) {
        DuskLog.warn("phone spike: listen() failed: {}", strerror(errno));
        close(fd);
        return false;
    }

    g_listenFd = fd;
    g_running = true;
    g_acceptThread = std::thread(accept_loop, port);
    g_senderThread = std::thread(sender_loop);
    DuskLog.info("phone spike: listening on 0.0.0.0:{} (open http://<lan-ip>:{}/ on the phone)", port,
        port);
    return true;
}

void stop_server() {
    if (!g_running.exchange(false)) {
        return;
    }
    if (g_listenFd >= 0) {
        shutdown(g_listenFd, SHUT_RDWR);
        close(g_listenFd);
        g_listenFd = -1;
    }
    if (g_acceptThread.joinable()) {
        g_acceptThread.join();
    }
    {
        std::lock_guard lock{g_pendingFrameMutex};
        g_pendingFrameCv.notify_all();
    }
    if (g_senderThread.joinable()) {
        g_senderThread.join();
    }
    set_client(-1);
}

bool has_client() {
    std::lock_guard lock{g_clientMutex};
    return g_clientFd >= 0;
}

// Shared by send_binary_frame/send_text_frame: RFC 6455 length-prefix
// encoding is identical for both opcodes, only header[0]'s opcode nibble
// differs. Server-to-client frames MUST NOT be masked (RFC 6455 §5.1) —
// header[1]'s top bit stays 0, no masking key follows, for either.
static void encode_frame_header(uint8_t opcode, size_t size, uint8_t (&header)[10], size_t& headerLen) {
    headerLen = 2;
    header[0] = static_cast<uint8_t>(0x80 | opcode);  // FIN=1
    if (size <= 125) {
        header[1] = static_cast<uint8_t>(size);
    } else if (size <= 0xFFFF) {
        header[1] = 126;
        header[2] = static_cast<uint8_t>((size >> 8) & 0xFF);
        header[3] = static_cast<uint8_t>(size & 0xFF);
        headerLen = 4;
    } else {
        header[1] = 127;
        for (int i = 0; i < 8; ++i) {
            header[2 + i] = static_cast<uint8_t>((static_cast<uint64_t>(size) >> ((7 - i) * 8)) & 0xFF);
        }
        headerLen = 10;
    }
}

// Shared by send_binary_frame/send_text_frame: caller already holds
// g_clientMutex and has validated g_clientFd >= 0.
static bool send_frame_locked(uint8_t opcode, const void* data, size_t size) {
    uint8_t header[10];
    size_t headerLen = 0;
    encode_frame_header(opcode, size, header, headerLen);

    const ssize_t hn = send(g_clientFd, header, headerLen, 0);
    if (hn != static_cast<ssize_t>(headerLen)) {
        DuskLog.warn("phone spike: frame header send failed/short, dropping client");
        close(g_clientFd);
        g_clientFd = -1;
        return false;
    }
    size_t sent = 0;
    const auto* bytes = static_cast<const uint8_t*>(data);
    while (sent < size) {
        const ssize_t n = send(g_clientFd, bytes + sent, size - sent, 0);
        if (n <= 0) {
            // Partial frame already on the wire — the client's WS stream is
            // now desynced no matter what we do from here, so the only
            // correct move is to drop the connection, not try to recover.
            DuskLog.warn("phone spike: frame payload send failed/timed out, dropping client");
            close(g_clientFd);
            g_clientFd = -1;
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool send_binary_frame(const void* data, size_t size) {
    std::lock_guard lock{g_clientMutex};
    if (g_clientFd < 0) {
        return false;
    }
    return send_frame_locked(0x2, data, size);  // opcode 2 = binary
}

bool send_text_frame(std::string_view json) {
    std::lock_guard lock{g_clientMutex};
    if (g_clientFd < 0) {
        return false;
    }
    return send_frame_locked(0x1, json.data(), json.size());  // opcode 1 = text
}

// Background sender thread's body: blocks on new raw captures, does the
// PNG encode AND the (potentially slow, over real Wi-Fi) blocking
// send_binary_frame() call here instead of on the game thread. Exits once
// g_running goes false AND there's nothing left to wake it for.
void sender_loop() {
    for (;;) {
        std::vector<uint8_t> pixels;
        std::deque<std::string> texts;
        uint32_t width = 0;
        uint32_t height = 0;
        bool haveFrame = false;
        {
            std::unique_lock lock{g_pendingFrameMutex};
            g_pendingFrameCv.wait(lock, [] {
                return g_pendingFrameReady || !g_pendingTexts.empty() || !g_running.load();
            });
            if (!g_running.load()) {
                return;
            }
            texts.swap(g_pendingTexts);
            haveFrame = g_pendingFrameReady;
            if (haveFrame) {
                pixels = std::move(g_pendingFramePixels);
                width = g_pendingFrameWidth;
                height = g_pendingFrameHeight;
                g_pendingFrameReady = false;
            }
        }
        // Texts first, and before the encode below: these are the tiny,
        // latency-critical state messages (map_player is sent every frame the
        // player moves), while a queued frame costs a multi-millisecond PNG
        // encode. Letting an encode run ahead of them would add exactly the
        // lag this whole state path exists to remove.
        for (const std::string& text : texts) {
            if (!send_text_frame(text)) {
                break;  // client gone; send_text_frame already tore it down
            }
        }
        if (!haveFrame) {
            continue;
        }
        // Level 1, not 6: same tradeoff dualscreen.cpp's screenshot path
        // documents — level 6 costs several hundred extra ms per encode,
        // not worth it for a smaller file here either.
        size_t pngSize = 0;
        void* png = tdefl_write_image_to_png_file_in_memory_ex(
            pixels.data(), static_cast<int>(width), static_cast<int>(height), 4, &pngSize, 1, MZ_FALSE);
        if (png == nullptr) {
            DuskLog.warn("phone spike: PNG encode failed");
            continue;
        }
        send_binary_frame(png, pngSize);
        mz_free(png);
    }
}

void queue_text_frame(std::string json) {
    std::lock_guard lock{g_pendingFrameMutex};
    // Bounded, and drops the OLDEST on overflow. These carry live state
    // (player position, map layers) where the newest message supersedes the
    // older ones, so if the sender ever falls behind, stale positions are
    // exactly what should be thrown away.
    constexpr size_t kMaxPendingTexts = 32;
    if (g_pendingTexts.size() >= kMaxPendingTexts) {
        g_pendingTexts.pop_front();
    }
    g_pendingTexts.push_back(std::move(json));
    g_pendingFrameCv.notify_one();
}

void queue_raw_frame(std::vector<uint8_t> pixels, uint32_t width, uint32_t height) {
    std::lock_guard lock{g_pendingFrameMutex};
    // Takes ownership (moved) — overwrites any frame still waiting to go
    // out, so the connection always carries the freshest capture rather
    // than a growing backlog of stale ones if the sender falls behind.
    g_pendingFramePixels = std::move(pixels);
    g_pendingFrameWidth = width;
    g_pendingFrameHeight = height;
    g_pendingFrameReady = true;
    g_pendingFrameCv.notify_one();
}

void request_resize(uint32_t width, uint32_t height) {
    std::lock_guard lock{g_resizeMutex};
    g_pendingResizeW = width;
    g_pendingResizeH = height;
    g_resizePending = true;
}

bool take_pending_resize(uint32_t& width, uint32_t& height) {
    std::lock_guard lock{g_resizeMutex};
    if (!g_resizePending) {
        return false;
    }
    width = g_pendingResizeW;
    height = g_pendingResizeH;
    g_resizePending = false;
    return true;
}

void request_icon(uint8_t itemNo) {
    std::lock_guard lock{g_iconRequestMutex};
    if (g_pendingIconRequests.size() >= kMaxPendingIconRequests) {
        g_pendingIconRequests.pop_front();
    }
    g_pendingIconRequests.push_back(itemNo);
}

bool take_pending_icon_request(uint8_t& itemNo) {
    std::lock_guard lock{g_iconRequestMutex};
    if (g_pendingIconRequests.empty()) {
        return false;
    }
    itemNo = g_pendingIconRequests.front();
    g_pendingIconRequests.pop_front();
    return true;
}

bool has_pending_icon_request() {
    std::lock_guard lock{g_iconRequestMutex};
    if (!g_pendingIconRequests.empty()) {
        return true;
    }
    // Must also check the heart want-list (Phase 3), not just the item
    // queue: this function is what makes the binary frame path yield the
    // capture slot for a tick (see its call site in dualscreen.cpp's
    // pollAndPushSpikeFrame()) — a heart-only request with nothing in the
    // item queue would otherwise starve exactly the way item requests did
    // before that fix, just for hearts this time. Throttled (see
    // g_heartProbeCounter's doc comment) — this function runs first each
    // tick, so it computes and caches this tick's probe decision for
    // take_next_wanted_heart_state() to reuse.
    g_heartProbeDueThisTick = (++g_heartProbeCounter % kHeartProbeIntervalFrames) == 0;
    if (!g_heartProbeDueThisTick) {
        return false;
    }
    for (bool wanted : g_wantedHeartStates) {
        if (wanted) {
            return true;
        }
    }
    return false;
}

void request_map_base() {
    std::lock_guard lock{g_iconRequestMutex};
    g_mapBaseWanted = true;
}

bool take_pending_map_base_request() {
    std::lock_guard lock{g_iconRequestMutex};
    if (!g_mapBaseWanted) {
        return false;
    }
    g_mapBaseWanted = false;
    return true;
}

bool has_pending_map_base_request() {
    std::lock_guard lock{g_iconRequestMutex};
    return g_mapBaseWanted;
}

void request_heart_icon(uint8_t state) {
    if (state >= 5) {
        return;
    }
    std::lock_guard lock{g_iconRequestMutex};
    g_wantedHeartStates[state] = true;
}

bool take_next_wanted_heart_state(uint8_t& state) {
    std::lock_guard lock{g_iconRequestMutex};
    // Reuses this tick's cached probe decision from has_pending_icon_request()
    // (always called first, see above) rather than recomputing/re-incrementing
    // — both must agree on the same tick.
    if (!g_heartProbeDueThisTick) {
        return false;
    }
    // Rotating scan, not 0..4 — see g_heartProbeCursor.
    for (uint8_t n = 0; n < 5; n++) {
        const uint8_t i = (uint8_t)((g_heartProbeCursor + n) % 5);
        if (g_wantedHeartStates[i]) {
            state = i;
            g_heartProbeCursor = (uint8_t)((i + 1) % 5);
            return true;
        }
    }
    return false;
}

void clear_wanted_heart_state(uint8_t state) {
    if (state >= 5) {
        return;
    }
    std::lock_guard lock{g_iconRequestMutex};
    g_wantedHeartStates[state] = false;
}

}  // namespace dusk::phone_spike

#endif  // DUSK_PHONE_SPIKE
