#include "dusk/phone_spike_ws.h"

#include "dusk/dualscreen.h"

#if DUSK_PHONE_SPIKE

#if defined(_WIN32)
#error "phone_spike_ws.cpp is POSIX-sockets-only for now; not wired up for Windows. See file header."
#endif

#include "dusk/companion.h"
#include "dusk/logging.h"
#include "dusk/phone_spike_pairing.h"
#include "dusk/phone_spike_sha1.h"

#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <csignal>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

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
ws.onopen = () => { stats.textContent = 'connected, waiting for first frame...'; };
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
</script></body></html>)HTML";

std::mutex g_clientMutex;
int g_clientFd = -1;
std::thread g_acceptThread;
std::atomic<bool> g_running{false};
int g_listenFd = -1;

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
        // Bounded-blocking sends from the game thread: never hang forever on
        // a stalled/dead client, but never silently write a partial frame
        // either — see send_binary_frame's contract in the header.
        timeval tv{.tv_sec = 0, .tv_usec = 50000};
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
    set_client(-1);
}

bool has_client() {
    std::lock_guard lock{g_clientMutex};
    return g_clientFd >= 0;
}

bool send_binary_frame(const void* data, size_t size) {
    std::lock_guard lock{g_clientMutex};
    if (g_clientFd < 0) {
        return false;
    }

    uint8_t header[10];
    size_t headerLen = 2;
    header[0] = 0x82;  // FIN=1, opcode=2 (binary)
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
    // Server-to-client frames MUST NOT be masked (RFC 6455 §5.1) — header[1]'s
    // top bit stays 0, no masking key follows.

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

}  // namespace dusk::phone_spike

#endif  // DUSK_PHONE_SPIKE
