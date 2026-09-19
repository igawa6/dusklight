# Phone-as-Companion-Screen: Design Sketch

**Status: phases 1 (capture → encode → push), 2 (QR pairing), and 3 (touch
input) built and verified end-to-end on Linux; DPI-clamp loosening and
gamepad passthrough not started.** This documents a feasibility sketch for
letting a phone act as a network-connected companion ("bottom screen")
display for desktop (Windows/Linux/macOS) dusklight, paired by QR code,
mirroring what the AYN Thor already gets for free from having two physical
displays on one device.

Not a commitment to build this — written to have a concrete plan to work
from if/when it's picked up, and so the open questions are visible up front
instead of discovered mid-implementation.

## Goals

- Desktop dusklight gets a companion display (map/items/hearts/quest UI)
  on a phone on the same LAN, no cable, no app install.
- Pairing is a QR-code scan, not manual IP entry.
- Phone can drive the companion via its own touchscreen, and via any
  gamepad paired to the *phone* (not the PC).
- Keep latency low enough that companion navigation (map panning, item
  wheel) feels responsive, not laggy.

## Non-goals

- Mirroring the *main* 3D game view. This is companion-HUD-only, same
  scope as the existing dual-screen feature.
- General Steam-Link-style full remote play.
- Any monetization mechanism — considered and explicitly dropped (ads /
  pay-to-remove is a real risk for a Nintendo-IP-derived project; see
  chat history, not repeated here).

## Why this is cheaper than it sounds

The existing dual-screen feature already does 90% of the hard part
*locally*; the phone case mostly needs a new transport, not a new
renderer or a new capture mechanism.

- **Render target is already resolution-agnostic.** `computeAuxCanvas()`
  in `src/dusk/dualscreen.cpp` sizes the companion's render target off
  `aurora::auxwin::get_surface_size()` at runtime and draws the HUD
  natively at whatever that returns (built to serve the Thor's two
  different companion resolutions, not one fixed size). Point it at a
  phone's reported resolution instead of a real window/display and the
  existing draw pipeline (anchors, `dAnchorHudScale`, etc.) should just
  work, unmodified.
- **Frame capture already exists.** `aurora::auxwin::request_capture()` /
  `take_capture(pixels, &w, &h)` is a working async CPU-readback path,
  currently used by the companion screenshot feature (`DUSK_COMPANION_CAPTURE`,
  off by default). Reuse directly instead of writing new capture code.
- **PNG encoding already exists and is already tuned for speed.** The
  screenshot feature encodes via miniz's `tdefl_write_image_to_png_file_in_memory_ex()`
  (miniz vendored at `CMakeLists.txt:180`) at compression level 1
  specifically because level 6 costs "several hundred extra
  milliseconds" per the existing code comment. Same call, write to a
  socket buffer instead of a file, and per-frame encoding cost is
  already solved and already latency-conscious.
- **The input entry point already exists and needs no new plumbing.**
  `dusk::companion::touchEvent(int action, float u, float v)` and
  `dusk::companion::pinchZoom(float factor)` (`src/dusk/companion.h`,
  implemented in `companion.cpp`) are the real hooks the Android JNI
  shim (`android_aux_display.cpp`) calls into today — plain C++, no
  Android/JNI dependency in the signature. `u`/`v` are already normalized
  0–1, already decoupled from actual pixel resolution. A WebSocket
  handler can call these two functions directly; it's the same shape of
  integration the JNI bridge already does, just a different caller.

## What's genuinely new

- **No HTTP/WebSocket server exists anywhere in the dependency tree.**
  `borealis::http` (`extern/borealis/src/http_internal.hpp`) is
  libcurl-backed (`find_package(CURL...)`) — client-only by
  construction, can't serve. This needs new code.
- **No QR generation library exists anywhere.**
- **Desktop's `aux_window` has zero input handling today.**
  `filter_event()` in `extern/aurora/lib/aux_window.cpp` only inspects
  `SDL_EVENT_WINDOW_*` events (resize, close) and early-returns for
  everything else — mouse/touch on the desktop aux window is currently
  not wired to anything. `companion::touchEvent`/`pinchZoom`'s only
  caller anywhere in the codebase today is the Android JNI shim. This
  path needs to be built for desktop regardless of the phone feature,
  since it's a prerequisite either way.
- **A real offscreen/headless render target.** Today `aux_window` always
  backs either a visible SDL window (desktop) or an Android Presentation
  display (Thor). Streaming to a phone wants a target with no local
  window at all — needs a headless variant of aux_window's surface
  creation, not just a resize.
- **No existing companion input surface for analog/D-pad-style input.**
  Everything that exists (`touchEvent`, `pinchZoom`) is
  touch-and-pinch-shaped. A gamepad physically paired to the phone has
  no natural mapping onto that today — see Open Questions.

## Proposed architecture

```
 ┌─────────────────────────┐        LAN, WebSocket        ┌──────────────────┐
 │  PC (Win/Linux/macOS)    │ ───────────────────────────► │  Phone (browser)  │
 │                          │  binary: PNG frame           │                    │
 │  headless aux_window  ───┼──────────────────────────────┼─► <canvas>         │
 │  (native res = phone's)  │                               │                    │
 │  request_capture() ──┐   │  ◄─────────────────────────── │  touch → JSON      │
 │  take_capture()      │   │  JSON: {type, u, v, action}   │  Gamepad API → JSON│
 │  miniz PNG encode ◄──┘   │                               │                    │
 │                          │                                │                    │
 │  new: WS server ─────────┤                                │                    │
 │  new: QR generator        │  QR encodes ws://<lan-ip>:<port>?token=<short-lived>│
 └─────────────────────────┘                                └──────────────────┘
```

### Pairing

1. PC starts the WS server on an ephemeral port, generates a short-lived
   token, renders a QR code encoding `ws://<lan-ip>:<port>?token=...`
   (or `http://...` serving a tiny static page that opens the socket —
   simpler for the phone if we want a page shell rather than a raw ws:
   URI, since raw `ws://` links don't reliably auto-open in a phone
   browser the way `http://` does).
2. Phone scans, opens the page, page immediately opens the WebSocket
   with the token.
3. First message phone→PC: `{type: "hello", width, height, devicePixelRatio}`.
   PC uses this to size the headless aux_window target (see DPI below).
4. PC starts pushing frames.

Scope boundary: LAN-only, no WAN/relay/NAT traversal. Token is
short-lived and single-use-ish (reject a second `hello` on the same
token) to stop anyone else on the same Wi-Fi from connecting
opportunistically during the pairing window.

### Frame transport

- PC captures via the existing `request_capture()`/`take_capture()`
  path, encodes via the existing miniz call, pushes as a binary
  WebSocket frame.
- Target rate: 10–30 fps. This is a HUD/menu (map, item wheel, hearts),
  not the 3D game view — it doesn't need 60fps to feel responsive, and
  capping it low is the single biggest lever on both bandwidth and PC
  frame-capture overhead.
- Phone side: binary frame → `Blob` → `createImageBitmap` →
  `canvas.drawImage`. All browser-native, no decode library needed.

### Input transport

- Touch: phone canvas touch handlers already give normalized-ish
  coordinates for free (`touch.clientX / canvas.width`, etc. — match
  `companion::touchEvent`'s existing 0–1 convention exactly so the PC
  side does zero translation). Send `{type: "touch", action, u, v}`,
  PC calls `companion::touchEvent(action, u, v)` directly.
- Pinch: browser doesn't give pinch-as-a-gesture as cleanly as Android's
  `ScaleGestureDetector` — either compute a scale factor from two active
  touch points manually in JS, or drop pinch-to-zoom for v1 and rely on
  whatever non-pinch map navigation the companion already supports.
- Phone gamepad: browser [Gamepad API](https://developer.mozilla.org/en-US/docs/Web/API/Gamepad_API)
  polls button/axis state; no native "gamepad → companion" mapping
  exists to receive it (see Open Questions — this is the one piece with
  no existing analog on either platform).

### New dependencies

Per the library research: nothing server-side or QR-capable exists
anywhere in the tree, so this needs *something* new. Two real options:

1. **New C++ library** (e.g. a small vendored WS server lib + a QR-gen
   lib) via the existing CMake `FetchContent` pattern already used for
   miniz/nlohmann_json/etc.
2. **A small Rust crate via the existing Corrosion path.** `extern/aurora`
   already has a working Cargo/Corrosion pipeline for `nod-ffi`, proven
   this session on both desktop and `android-arm64`. Caveat: for the
   common desktop targets (win-x86_64, linux-x86_64, macos-arm64), nod
   actually resolves via a *prebuilt binary download* (`AuroraNodProvider.cmake`'s
   `package` mode), not a local Cargo build — Corrosion is mainly
   exercised today for Android/uncommon archs. Reusing that path for a
   new crate (`tungstenite` for WS, `qrcode` for QR gen) means forcing
   the always-build `vendor` mode rather than getting nod's prebuilt-package
   shortcut, which is extra one-time setup, not zero-cost reuse.

No strong recommendation either way without prototyping both; leaning
Rust/Corrosion slightly since a WS server + QR generation is exactly the
kind of thing with mature, well-maintained crates and comparatively
sparse/unmaintained small C++ equivalents, but this is worth a half-day
spike before committing.

### DPI / resolution

- `hello` message gives real phone resolution + `devicePixelRatio`;
  headless aux target created at `width * devicePixelRatio` ×
  `height * devicePixelRatio` so `computeAuxCanvas()`'s native-resolution
  path (not its 2x-supersample fallback) actually engages.
- **Must loosen `kMaxNativeDim = 2048`** in `computeAuxCanvas()`
  (`src/dusk/dualscreen.cpp`) — a modern phone's long edge (e.g.
  1080×2400, 1440×3200) exceeds it, so unmodified it'd silently fall
  back to a lower-res supersampled canvas instead of true native 1:1.
  Small, targeted change; not free but not architecture-level either.

## Latency budget

| Stage | Estimate / measured |
|---|---|
| Touch/gamepad event → JSON → WS send | ~1 ms (not built yet — estimate) |
| LAN WS round trip | 1–5 ms typical Wi-Fi (not built yet — estimate) |
| PC: `companion::touchEvent()` → HUD reacts → next capture | within one companion tick (not built yet — estimate) |
| Frame capture (`request_capture`/`take_capture`) → miniz encode (level 1) → WS push, sustained, real hardware/software-render loopback | **measured: 55–132 ms between frames, avg 87.5 ms (~11.4 fps)** against a target request interval of 66 ms (~15 fps) — see Phase 1 results below |
| WS send + phone decode + canvas draw | a few ms, browser-native decode (not phone-tested yet) |

Honest framing: transport latency on a LAN was never expected to be the
bottleneck, and Phase 1 confirms it isn't — the measured 55–132 ms
alternating pattern is *slower than the 66 ms request interval itself*,
meaning the async GPU read-back (`request_capture` → `MapAsync` completing)
sometimes spans more than one frame, not the network. See Phase 1 results.
If "feels responsive" ends up needing more than the ~11 fps actually
achieved here, the picture-push design may be the wrong tool and the
"stream UI state, re-render natively" alternative (mentioned as a v2 idea,
not pursued here) becomes the real answer — but that's a call to make once
input is wired up and it can actually be felt, not from this number alone.

## Phase 1 results (built and verified)

Built exactly as scoped: `hidden` flag on `aurora::auxwin::CreateInfo`
(`extern/aurora`), a hand-rolled single-client HTTP+WebSocket server
(`src/dusk/phone_spike_ws.{h,cpp}`, RFC 6455 handshake via a small vendored
SHA-1 in `src/dusk/phone_spike_sha1.h` — nothing else in the dependency tree
does WS serving, see "What's genuinely new" above), and a capture→encode→push
tick added to `beginHudCapture()` in `dualscreen.cpp`, all gated behind
`DUSK_PHONE_SPIKE` (off by default, same idiom as `DUSK_COMPANION_CAPTURE`).

Verified end-to-end on this Linux dev box (Xvfb + llvmpipe, real disc,
`linux-default-relwithdebinfo`, `--cvar game.dualScreen=true`):
listening socket confirmed bound, a real `websockets`-library Python client
completed the actual WS handshake (not a stub), received 20 consecutive
binary frames each verified to start with the PNG magic bytes, and one saved
frame decoded to a correct 1216×896 RGBA PNG showing the real companion boot
splash. Test client kept at `scripts/spike_test_client.py` (explicitly
throwaway, says so in its own docstring).

**Not yet verified:** a real phone/real browser (only tested with a Python
WS client — the HTML test page was fetched via `curl` and eyeballed, not
opened in an actual browser), Windows/macOS at all (POSIX-sockets-only,
deliberately — see `phone_spike_ws.cpp`'s `#error` on `_WIN32`), and
performance against real gameplay content (frame size was constant across
all 20 test frames because the captured content was the idle boot splash,
not live map/HUD redraws — a real companion frame with active content may
encode slower).

**Re-examined, not actually a fixable inefficiency:** traced through the
state machine in `pollAndPushSpikeFrame()` in detail — a new
`request_capture()` already fires the instant the previous one is consumed
(the interval check passes trivially every time, because readback latency
alone already exceeds 66 ms). So the loop is already requesting as fast as
`aux_window`'s single-slot capture API allows; there's no queued-up slack to
squeeze out of the calling code. The measured 55–132 ms alternating pattern
is more likely either (a) inherent to this test environment's software
rendering (Xvfb + llvmpipe has no real vsync/GPU parallelism), or (b) the
capture API's single-in-flight-request design itself (built for one-shot
screenshots, not sustained streaming) — `request_capture()` abandons rather
than queues a second request made while one is already in flight, so there's
no way to pipeline two captures without changing that API. Real GPU hardware
would likely show very different numbers; not re-tested there. The interval
constant stays, now correctly documented as a ceiling for fast hardware
rather than the thing actually governing this environment's measured rate.

## Phase 3 results (built and verified) — done ahead of Phase 2

Touch input landed before QR pairing, on purpose: pairing UX doesn't retire
any technical risk, and the open question worth answering first was whether
picture-push-plus-touch feels workable at all.

Added to `phone_spike_ws.cpp`: a WS frame *reader* (client→server frames are
masked per RFC 6455, unlike the send path) parsing small JSON messages
(`nlohmann::json`, already linked into the `dusklight` target directly —
used elsewhere in `config.cpp`/`save_manager.cpp`/`achievements.cpp`, no new
dependency) of the form `{"type":"touch","action":0-2,"u":..,"v":..}` or
`{"type":"pinch","factor":..}`, dispatched straight into
`dusk::companion::touchEvent()`/`pinchZoom()` — confirmed safe to call from
a non-game thread (both use `std::atomic` internally), matching exactly how
the existing Android JNI touch path already calls them cross-thread. The
test page grew pointer-event handlers that undo the CSS
`object-fit: contain` letterbox by hand to recover normalized 0–1
coordinates (there's no DOM API for it), matching `touchEvent`'s existing
convention exactly.

**Verified end-to-end with a real tap, not just a parse test:** loaded a
real save (`--load-save 1`) so the interactive dashboard (not the boot
splash) was live, captured a frame showing the MAP tab active, sent a
`touch` down+up pair at the ITEMS tab's screen position over the WS
connection, captured another frame, and confirmed **the ITEMS tab was now
genuinely active with the real inventory grid rendered** — a full round
trip from a scripted "phone" through the new server, into real companion
state, and back out through the existing frame-push path.

**Real bug caught by this test, not a hypothetical:** the first attempt
crashed the *entire game process*, not just the WS connection, the moment
the test client disconnected. Root cause: `send()` on a socket whose peer
already closed raises `SIGPIPE`, and `SIGPIPE`'s default disposition is to
terminate the whole process — not just fail that one call. Confirmed with
gdb (`Program terminated with signal SIGPIPE, Broken pipe`). Fixed with a
single `std::signal(SIGPIPE, SIG_IGN)` in `start_server()` (global and
process-wide is the standard, portable fix here — no per-call flag works
identically on both Linux's `MSG_NOSIGNAL` and macOS's `SO_NOSIGPIPE`, and
every socket call in this file already checks its return value through the
normal error path regardless). Re-ran the exact same crash scenario after
the fix; the process now survives a client disconnecting mid-push. This is
exactly the kind of bug that only shows up under a real disconnect, not a
happy-path test — worth remembering for anything else that does raw socket
I/O in this codebase later.

**Not yet tested:** `pinchZoom` (only `touch` was exercised), a real phone
browser (still a scripted Python client), and whether a tap needs the
~0.5s hold used in the successful test or a shorter one also works (a
first attempt with a 0.1s down-to-up gap did *not* register as a tap —
unclear yet whether that's a real minimum-hold requirement in the
companion's gesture code or a fluke; worth a proper look before relying on
timing assumptions for phase 5's gamepad-as-synthetic-gestures idea).

## Phase 2 results (built and verified) — QR pairing

New dependency, as expected (the library research called this one): vendored
[Nayuki's QR-Code-generator](https://github.com/nayuki/QR-Code-generator)
(C version, single `.c`/`.h` pair, MIT-ish license) via `FetchContent`,
gated the same way as the pre-existing `DUSK_ENABLE_OPUS` option — and while
implementing that, **promoted `DUSK_PHONE_SPIKE` from a header `#define` to
a real CMake `option()`** (`-DDUSK_PHONE_SPIKE=ON`), matching the
`DUSK_ENABLE_OPUS` pattern exactly. Strictly better than what Phases 1/3
had: no more hand-editing `dualscreen.h` and rebuilding to toggle it.

New files: `phone_spike_pairing.{h,cpp}` — `generate_token()` (8 random
bytes from `/dev/urandom`, hex-encoded), `discover_lan_ip()`
(`getifaddrs()`, first non-loopback UP IPv4), `write_pairing_qr()` (encodes
via `qrcodegen_encodeText()`, rasterizes the module matrix by hand into an
RGBA bitmap with the QR spec's minimum 4-module quiet zone, encodes via the
**same miniz PNG writer already used twice elsewhere in this codebase** —
no second image dependency), and `start_pairing()` tying it together. Token
is checked in `phone_spike_ws.cpp`'s WS-upgrade handler only (the plain
HTML GET stays unauthenticated on purpose — it's static and harmless; a
live session that can read frames and inject input is the actual boundary).
The test page now forwards its own URL's `?token=...` straight into its
WebSocket connection via `location.search`.

**Verified with a real, independent decoder, not just "produces a PNG":**
installed `opencv-python-headless` and decoded the generated QR with
`cv2.QRCodeDetector()` — it read back the exact pairing URL byte-for-byte.
Then verified the token gate itself with three real connection attempts: no
token → HTTP 403, wrong token → HTTP 403, correct (decoded) token →
connects and a full frame+touch round trip works. Confirmed the process
survives a disconnect afterward (the Phase 3 SIGPIPE fix holding under this
new code path too) and that rejected connection attempts themselves don't
destabilize anything.

**Real-world finding, not hypothetical:** on this dev box (which has both a
normal LAN interface and a Tailscale VPN interface), `discover_lan_ip()`
picked the **Tailscale interface** (a `100.x.x.x` CGNAT address), not the
"real" LAN one — `getifaddrs()`'s interface ordering isn't something this
code controls or filters for. Exactly the multi-NIC caveat already flagged
in this doc's own header comment, now demonstrated rather than assumed. A
real implementation needs either a better heuristic (prefer RFC 1918
ranges, skip known VPN interface name patterns) or a way for the user to
pick if more than one candidate exists — not solved here.

**Not yet done:** single-use/token-rotation policy (the token is valid for
the whole server lifetime right now, not invalidated after first use, which
the earlier "Pairing" section above originally sketched); in-game display of
the QR (it's a PNG on disk, not drawn into any UI yet — there is no
Settings/dual-screen-setup screen surface for it in this repo to hang it off
of without a larger UI task); and, as before, a real phone camera actually
scanning it end-to-end (verified the QR decodes correctly and the pairing
mechanics work, but the full "point a phone camera at a screen and have it
open a browser" loop wasn't physically exercised).

## Suggested phasing

1. ~~**Spike:** headless aux_window target + WS server + raw frame push to
   a phone browser, no input, no QR (manual URL entry). Confirms the
   capture/encode/push loop and gets a real latency number.~~ **Done — see
   Phase 1 results above.**
2. ~~**Add QR pairing** (token handshake, LAN IP discovery/display).~~
   **Done — see Phase 2 results above** (done after input instead of
   before, per the Phase 3 reasoning).
3. ~~**Add touch input** (`companion::touchEvent` wiring, since this is
   also the first time desktop gets *any* aux_window input path).~~ **Done
   — see Phase 3 results above.**
4. **Loosen `kMaxNativeDim`, verify true native-res on a real phone.**
5. **Phone gamepad passthrough** — blocked on the open question below.

## Open questions / risks

- **No existing "analog/D-pad" companion input surface.** Everything
  today is touch/pinch-shaped. Does a phone-paired gamepad make sense
  translated into synthetic swipes/taps (map panning via D-pad, item
  wheel via analog stick angle), or does this need a genuinely new
  companion input mode? Needs product thinking, not just plumbing —
  punt to phase 5, don't let it block touch-only v1.
- **Rust/Corrosion `vendor`-mode setup for a new crate is unproven** —
  nod-ffi's tri-mode provider is more machinery than a first pass needs;
  confirm forcing always-vendor mode for a new crate is actually simple
  before committing to the Rust path over a C++ library.
- **Per-frame (not one-shot) miniz encode cost is unmeasured.** The
  screenshot feature's speed tuning was validated for a single capture,
  not sustained 10-30fps encoding — needs its own measurement.
- **Firewall prompts.** First run on Windows will trigger a Defender
  Firewall prompt for the new listening port — expected, but worth a
  first-run UX note (e.g. explaining what to allow) rather than a silent
  surprise.
- ~~**Headless aux_window surface creation** is new aurora-side work...~~
  **Resolved, turned out much cheaper than feared:** a real `wgpu::Surface`
  still needs *some* backing OS window, but `SDL_WINDOW_HIDDEN` gets one
  that's never mapped to the screen — `present()`/`request_capture()`/
  `take_capture()` all work completely unmodified. Landed as a 5-line change
  (`CreateInfo.hidden` + one flag bit in `aux_window.cpp`), see Phase 1
  results. Not tested on Windows/macOS — a hidden window's swapchain present
  timing under `PresentMode::Fifo` could behave differently without a real
  compositor driving vsync; worked fine under Xvfb+llvmpipe here, which has
  no real vsync either, so this isn't a strong signal either way for a real
  GPU+compositor.
- **Sound + haptic feedback**, raised separately during scoping: the
  companion already has a feedback hook to build on —
  `companion::queueSound(sfxId, haptic)` / `queueHaptic(haptic)`
  (`companion.cpp:1428-1463`) fires on companion touch today and already
  calls `aurora::device::rumble(...)`, gated by the existing
  `game.dualScreenHaptics` setting. Redirecting that to a phone would need a
  new PC→phone message (this spike is push-only frames, one direction) but
  no new companion-side concept. Real platform gap: the phone's own
  vibration motor is reachable from a browser via `navigator.vibrate()` on
  Android, but **iOS Safari has never implemented it** — not a bug to fix,
  a permanent limitation of the no-app-install approach on iPhone
  specifically. A gamepad's own rumble motor (if the phone has one paired)
  is separately reachable via the Gamepad Haptics API, with patchier
  cross-browser support. Not scoped into any phase above; add as its own
  phase if wanted.
