# THE PLAN — Native companion rendering on the phone, staged so latency improves in week two

## 0. The one-paragraph version

The PNG stream is retired in stages, but **the first two stages are pure latency work and land in ~5-6 weeks**, not at the end. Stage 1 makes the existing stream 2-4x cheaper with no architectural change. Stage 2 moves the *chrome* to the phone — the tabs, corner buttons, X/Y, context tab — which is where every touch the user makes actually lands, so interaction stops round-tripping, and the streamed region shrinks to the content window only. Everything after that is page-by-page retirement of a stream that is already much smaller and no longer in the input path. Full retirement is realistically 5-7 months of focused single-developer work; the part that fixes the user's actual complaint is the first six weeks.

**The C++ companion renderer is not touched by any of this.** See §2 — that is the load-bearing constraint, not a footnote.

---

## 1. Corrections that reshape the plan

Five findings from the surveys change the plan's shape. They are stated here once so the stages below don't have to re-argue them.

1. **The page-ownership handshake does not exist.** It is a decision, not code. `dispatch_message` handles exactly five inbound types — touch, pinch, hello, icon_request, pad (`phone_spike_ws.cpp:439-497`), `hello` carries only `{width,height}` (`:444-449`), and the APK parses three outbound types (`CompanionClient.kt:63-73`). It is new work on both sides and it is Stage 0.

2. **The chrome is not a page, so page suppression alone buys nothing.** The backdrop, top bar, four corner boxes, left column, item buttons, window frame and tab strip are drawn every frame around whatever page is showing (`companion.cpp:1631-1666`), and the capture is a whole-surface readback. Suppressing a page's draw leaves a hole in a frame that is still captured, encoded and sent. **Chrome is ownership unit zero**, and the intermediate that makes per-page retirement pay off before the last page lands is *cropping the readback to the published content rect*.

3. **A CPU texture decoder exists.** `companion_state.h:86-88` asserts none does, and that claim is the entire justification for the render-and-capture icon pipeline. It is wrong: aurora ships `convert_texture()` / `convert_texture_palette()` covering every GX format including CI8 and CMPR (`extern/aurora/lib/gfx/texture_convert.hpp:54-55`). Item icons (`companion_icons.cpp:24,31,93`), collect icons (`:230-244`), deco plates (`:256-263`), the 14 map icons (`companion_pages.cpp:170-183`) and the floor markers already have their BTI bytes in CPU RAM. Almost no art needs a GPU capture. Fix the comment in the same PR as the code.

4. **The Android dual-screen host can never be the phone host.** `docs/phone-companion-design.md` Phase 5.5 establishes that SDL's Android backend hard-blocks a second window, so `aurora::auxwin::create()` always fails there. The phone-as-bottom-screen feature is desktop-only. The AYN Thor path and the phone path therefore never contend for the same aux surface on the same machine. This substantially de-risks §2 — but it must be enforced at runtime, not assumed, because the desktop *can* have both.

5. **The stream is not per-frame-pushed and the state path is slightly net-negative.** At most one capture is in flight, re-armed only after the previous one is consumed (`dualscreen.cpp:540-543`); encode and send already run off the game thread (`queue_raw_frame`, `phone_spike_ws.h:58-70`). The `~15-19 fps` figure is stale — the interval cap was deliberately removed (`dualscreen.cpp:220-231`). Meanwhile the icon path *takes the capture slot away from* the frame stream whenever a request is pending (`dualscreen.cpp:518-543`), so fetching art visibly drops the frame rate. The premise of the port holds; the accounting is one notch worse than the brief states.

Two smaller ones: there is **no long-press anywhere** in the companion's gesture vocabulary (tap, tap-drag past a 10px slop, drag-scroll only); and `src/dusk/companion_dmap_icons.inc:1-5` embeds three byte-identical `dmapres.arc` BTIs in the PC binary, which contradicts `README.md:48`. The APK rule stays absolute regardless; see §8 decision D5.

---

## 2. How the native dual-screen paths stay working

This section is the constraint, not a caveat. **"All companion logic on the phone" applies only to a phone acting as the bottom screen over Wi-Fi. It does not apply to the AYN Thor's second physical display, and it does not apply to a desktop second window.**

### 2.1 The C++ renderer is maintained, not deprecated

`companion*.cpp` stays whole. No stage in this plan deletes a draw function, a layout constant or a gesture handler from it. The only deletions permitted are in the *phone transport* (`phone_spike_ws.cpp`, the `DUSK_PHONE_SPIKE*` paths in `dualscreen.cpp`) and, at the very end, the per-frame capture/encode/send path — none of which the native paths use. The guide reader's ~600 lines of renderer (`companion_guide.cpp:417-1346`) are the single biggest tempting deletion in the codebase; **they stay**, because the Thor and the desktop second window render the guide through them.

### 2.2 The suppression rule, stated exactly

Three gates, and only the first two are ever driven by phone ownership:

| Gate | File:line | Condition to skip |
|---|---|---|
| Re-arm `request_capture()` | `dualscreen.cpp:540-543` | phone owns every visible unit |
| `queue_raw_frame()` | `dualscreen.cpp:505-514` | phone owns every visible unit |
| `drawDashboard()` + `set_source()` | `dualscreen.cpp:1150-1163` | phone owns every visible unit **AND** `!auxHasLocalViewer()` |

`auxHasLocalViewer()` is new, runtime, and returns true when a native window is attached (`aurora::auxwin::set_native_window`, `android_aux_display.cpp:18-31`) or a visible SDL aux window exists. The rule in words: **never skip the draw while anything local is looking at it.** Skipping the draw is a pure CPU/GPU saving available only in the headless phone-only configuration, where the aux window is created with `.hidden = true` (`dualscreen.cpp:142-146`). The aux window keeps presenting its last texture on frames where `set_source` is not called (`aux_window.hpp:41-44`), so a wrong gate here freezes the Thor's panel on a stale image — that is the specific failure mode to test for, every stage.

Also make that `.hidden` a runtime property rather than a `#if DUSK_PHONE_SPIKE` compile-time one, so a desktop build can have a real second window *and* a phone client. Today they are mutually exclusive by build, which is a latent trap once this feature becomes real.

### 2.3 What may change in shared code, and what may not

- **Additive only.** New state-gathering functions (`gatherCollectState()`, `gatherFloorsState()`), new semantic entry points, new protocol code. These are new callers of existing logic, not rewrites of it.
- **The one real refactor: input.** Hit-testing currently runs at the *tail of the draw*, against rects that frame's draw just published (`companion.cpp:1788-1789`, and `companion_touch.cpp:1-5` says so explicitly). Semantic actions need a second entry point that does not depend on the painter. **The refactor is: lift the *effect* out of each handler into a named function, and have the existing touch handler call it.** `equipFromCompanion()` (`companion_touch.cpp:95-154`), `equipGear()` (`:222-259`) and `requestOoccooQuickUse()` (`:558-589`) are already shaped that way; the rest (`setPage`, context-tab dispatch, floor select, guide nav) need the same treatment. The native path keeps calling them through `handleTouch()`, unchanged and in the same order. The phone path calls them from `beginFrameCompanionInput()` (`companion.cpp:1216`), which already runs on the game frame loop and already owns the hold tick and the Ooccoo tick. **Risk to the native path: ordering.** The hit-test order is load-bearing — the floor picker is tested before the corner buttons (`companion_touch.cpp:709-739`) because a tap once fired through an opaque slab. Do not reorder anything; only extract.
- **Layout constants are frozen in place.** `companion_internal.h:246-285` stays the single source of truth. It is never "moved to a shared header the phone also uses" — see §2.4.
- **Forbidden:** changing `drawDashboardFunctional`/`drawDashboardCinematic` structure, changing `currentDim()`'s state machine (`companion.cpp:847-967`), changing `TAP_HOLD_FRAMES` semantics, changing `DROP_GRAB`.

### 2.4 Keeping two renderers from drifting

Be honest: this is the long-term cost of the approach, and there is no way to make it zero. What is realistic:

**Genuinely shared (not duplicated):**
- *Everything that writes savedata.* `plainEquip` (`companion_touch.cpp:158-200`), `tryBowCombo` (`:48-91`), `equipGear` (`:222-259`), the Ooccoo slot borrow (`companion.cpp:754-771`). The phone sends intent; the PC executes. This is real sharing — both renderers drive one implementation — and it is the single most important anti-drift measure, because behaviour divergence in *semantics* is the kind that corrupts saves.
- *All state derivation.* `gatherHudState()` and its successors are C++, run once, feed both renderers.
- *Layout geometry, by codegen.* Add a build step that parses the `constexpr f32` block at `companion_internal.h:246-285` (plus the two inventory cell tables at `companion_pages.cpp:600-627` and the animation rates at `:804-816`) and emits a checked-in `CompanionLayout.kt`. CI regenerates and fails on a diff. This is a ~150-line script and it removes the entire class of "someone nudged a constant on one side" drift, which is the most common kind.

**Not realistic, and rejected explicitly:** compiling `companion_*.cpp` into the APK over the NDK. The renderer draws through GX/J2D/`dDlst_2DQuad` (`companion_gfx.cpp:189-570`) and reads game state through `dComIfGs_*`. Porting it means porting the game. Dead end; don't spend a week discovering that.

**What is left is disciplined duplication, with three guards:**
1. A `docs/companion-parity.md` listing every behaviour that exists in both renderers, with the C++ file:line as the normative reference and a Kotlin file:line beside it. New behaviour is added to the table in the same PR that adds it.
2. A PR checklist item on any file matching `src/dusk/companion*`: *"Does this change the dashboard's appearance or behaviour? If yes, is there a matching APK issue/PR, or an explicit 'phone deviates here' entry in companion-parity.md?"*
3. A per-release golden-image review: `requestScreenshot()` (`dualscreen.h:66-72`, `DUSK_COMPANION_CAPTURE`) dumps the C++ dashboard; `adb exec-out screencap` dumps the phone's; both at the same canvas aspect, from the same save fixture, reviewed side by side. Automated pixel diffing is useless once the fonts differ — this is a human eyeball pass, ~20 minutes, and it catches the drift that matters.

**And accept, in writing, that the two will not be pixel-identical.** The phone uses a neutral bundled font (§8 D1), so every fitted, ellipsized and centred label re-flows. Agree on *"close, not identical"* before the work starts, not after someone compares screenshots.

---

## 3. The handshake (Stage 0 deliverable, everything depends on it)

```
C->S  hello        { type, width, height, protocol:2, caps:[...] }
S->C  server_hello { type, protocol:2, layout:"functional"|"cinematic",
                     canvasW, canvasH, features:[...], gen:N }
C->S  own          { type, units:["chrome","page:items"], gen:N }
S->C  own_ack      { type, units:[accepted...], gen:N,
                     stream:"full"|"content_rect"|"none", rect:[x,y,w,h] }
```

Rules, each of which exists to stop a specific failure:

- **Units, not pages.** `chrome`, `page:items`, `page:collect`, `page:guide`, `map:dungeon`, `map:field`. The map's two contexts retire on very different timelines (§7 Stage 8), so they must be separately ownable.
- **Ownership is per (unit, layout).** `mainHudRestored()` (`dualscreen.cpp:1180-1183`) can flip mid-session and changes which widgets exist at all. On a flip the PC bumps `gen`, re-sends `server_hello`, and **clears all ownership**; the phone re-declares. The PC's own code carries a scar from exactly this class of bug — a stranded floor-picker ramp swallowed every tap for the rest of the session (`companion.cpp:1753-1765`).
- **The phone stops drawing streamed pixels only on `own_ack`, never on send.** An old PC warn-and-drops unknown types (`phone_spike_ws.cpp:497-499`), so a phone that assumed ownership would show a live picture that ignores its own input.
- **A new PC defaults to `ownedUnits = {}`** and keeps streaming, so an old APK is unaffected.
- **`stream:"content_rect"`** is the intermediate that makes staged retirement pay: the PC draws the full dashboard (native path untouched) and crops the readback to `s_contentRect` before encode. Honest caveat: `take_capture()` returns the whole surface, so the *readback* cost is unchanged until aurora gains a rect capture — but the encode, which is the dominant cost (level 1 was chosen over level 6 specifically because level 6 costs "several hundred extra milliseconds"), scales with the crop.

Also add to the frequent channel (`queue_text_frame`, `phone_spike_ws.h:72-84`): a **quantised u8 dim level**, sent only while non-zero and changed. `currentDim()` (`companion.cpp:594-607`) reaches the phone today only because the fade is applied in the same present blit that feeds the capture (`dualscreen.cpp:1041-1055`). The moment the stream dies, the phone goes undimmed through every load and cutscene. It must **not** go in `HudState` — a per-frame float would fire the whole-struct diff every frame, which is exactly the trap `companion_state.h:36-47` documents.

---

## 4. Stage table

| # | Stage | Ship size | What the user feels |
|---|---|---|---|
| 0 | Measurement, handshake, encoding safety, fixtures | ~1 wk | Nothing (but numbers exist) |
| 1 | Cheap stream | ~1 wk | **2-4x fewer bytes/frame, visibly smoother** |
| 2 | Chrome ownership (Functional) | ~3-4 wk | **Buttons respond instantly; another ~2x on bytes** |
| 3 | Asset pipeline v2 (CPU decode) | ~2 wk | Art fetches stop stalling the stream |
| 4 | `page:items` native | ~2-3 wk | Inventory is instant |
| 5 | `map:dungeon` native | ~2 wk | Map pan/zoom is instant |
| 6 | `page:collect` native | ~3-4 wk | Collection is instant |
| 7 | `page:guide` native | ~2-3 wk | Guide scrolls like a phone app |
| 8 | `map:field` (overworld minimap) | ~2-3 wk | The last continuous stream dies |
| 9 | Retire the per-frame path | ~1 wk | — |

---

## 5. Stages in detail

### Stage 0 — Measurement, handshake, encoding safety, fixtures (~1 week)

**Ships:**
- Telemetry, both sides: capture-arm → `take_capture` ms, encode ms, send ms, phone decode→display ms, and **touch→visible-response** p50/p95. Surfaced in a debug overlay on the phone and logged on the PC. Without this, "latency improved" is unfalsifiable and every later stage is arguing from feel.
- The handshake from §3 (`hello` v2, `server_hello`, `own`, `own_ack`), with **no unit accepted yet** — the PC replies `stream:"full"` always. Pure plumbing, zero behaviour change, fully revertable.
- **LATIN-1 → UTF-8 on every string that crosses the wire.** Game text from the `.bmg` is LATIN-1 (`companion_strings.h:7-10`); the wire is `nlohmann::json::dump()` (`dualscreen.cpp:39,610,702,819,834`), which throws `type_error.316` on invalid UTF-8, and there is no try/catch at any call site. Nothing has exercised it because every string sent so far is ASCII. The first accented German/French/Spanish skill name will throw *inside the send*. Two-line byte expansion; do **not** use `error_handler_t::replace`, which mangles accents into replacement characters.
- **A transcript recorder and a mock server.** PC-side env var writes every outbound message to JSONL plus any binary frames; a small Kotlin/Node mock server in the APK repo replays a transcript. This is the highest-leverage item in the whole plan: it lets all Kotlin work proceed against recorded real-game state without a disc, a save, or a running PC, and it largely defuses the early-save blocker (§9).

**Verify:** replay a recorded transcript into the APK and get a byte-identical render to the live session. Round-trip an accented string end to end. Native-path regression checklist (§10) — should be untouched, and must be run anyway to establish the baseline.

**Revert:** delete the new message types; the PC ignores unknown inbound types already.

---

### Stage 1 — Make the existing stream 2-4x cheaper (~1 week)

No architecture. This is the stage that makes the user stop complaining first.

1. **Decouple stream resolution from the phone's native resolution.** `computeAuxCanvas()` (`dualscreen.cpp:178-202`) sets the capture target to the phone's reported pixel size, up to `kMaxNativeDim = 4096` (`:169-172`). A 1440×3200 phone therefore produces an 18MB raw capture and a proportionally enormous PNG every round trip. Add a negotiated `stream_scale` (default: long edge ≤ 1280) applied to the *capture* target only. The measured whole-dashboard PNG at 800×1200 is 865,623 bytes (`dualscreen.cpp:766-771`); this is where most of that goes.
2. **Encode 3 channels, not 4.** Alpha is stamped opaque anyway (`stampOpaqueAlpha`, end of `endHudCapture`). `tdefl_write_image_to_png_file_in_memory_ex` takes `num_chans` (`phone_spike_ws.cpp:837`); pack RGB on the sender thread. Removes a constant plane from the deflate input.
3. **Crop icon and map_base captures to the drawn rect before encode** (`dualscreen.cpp:713-835`; scale factor is already published via `setNativeCanvas`, `:1083`). A 128px icon currently reads back and encodes the entire surface — ~12MB RGBA at 1270×2416 for an icon that is 3KB on disc. This is why in-tree icon sizes run 46KB-242KB with an 865KB outlier.
4. **Fix the icon request queue.** `request_icon` drops the *oldest* past `kMaxPendingIconRequests = 16` (`phone_spike_ws.cpp:211,892-898`) and there is no retry (`dualscreen.cpp:757-760`). Opening the inventory grid needs up to 23 icons; the first ~7 are lost for the life of the connection. Dedupe on insert, raise the bound, and add a phone-side in-flight timeout — the APK marks a key in-flight and never times it out (`HudChromeOverlayView.kt:145-152`).

**Verify:** Stage 0 telemetry, before/after, same save, same route, same phone. Report p50/p95 frame interval and bytes/frame in the PR. Eyeball the streamed image at reduced scale for text legibility — this is the one place the stage can look *worse*, and it is why `stream_scale` is negotiated rather than hardcoded.

**Revert:** each item is an independent flag.

---

### Stage 2 — Chrome ownership, Functional layout (~3-4 weeks)

The biggest single win and the gate for everything after. Functional first, deliberately: in Functional mode the companion **does not draw** hearts, the Vessel of Light, A/B/Z, Midna, the d-pad or the special-action panels at all — they are on the main screen (`dualscreen.h:98-106`). That removes almost the entire live-pane-composite art bill from the chrome port. Its look is code, not assets: the four 84×84 corner boxes, the left column's bleed panels (`companion_functional.cpp:43-49`), the context-tab bridge bed (`:578-601`), the tab bed and side rules (`companion_hud.cpp:493-501`), the oil/oxygen bar (`companion.cpp:324-335`), the battery (`companion.cpp:364-390`), the carousel dots and the authored sun/moon glyph (`companion_functional.cpp:286-342`) are all primitives.

**Ships:**
- Kotlin chrome renderer on a **SurfaceView with its own Choreographer loop**, not repaint-on-message. The existing overlay repaints only on message arrival, which is why it had to skip the low-gauge pulse (`HudChromeOverlayView.kt:214-219`); a real loop removes that excuse and is what makes press animations possible.
- Semantic actions for chrome controls only: `page`, `button{id,phase}` (X/Y/I/II — **down and up as separate messages**, never a tap: the hold is what aims and fires bow-class items, `companion_touch.cpp:269-310`), `transform`, `z`, `ctx`, `left_box_page`. The `TAP_HOLD_FRAMES = 24` deferral stays on the PC (`companion_internal.h:396-400`, ticked in `beginFrameCompanionInput`) — it runs on the pad's clock, not the render clock, and a same-frame tap would clear the mask before the pad ever read it.
- An **ack channel**: `{action_result, ok, reasonStringId|reasonText, cue:"confirm"|"deny"}`. Refusal reasons are dusklight-authored string ids (`companion_strings.h:14-46`) and can be bundled/translated locally; item names inside them are game text and travel as text. Sound stays on the PC (`flushQueuedSounds`, `companion.cpp:1447`) so the game's own SE still fires; the phone plays a bundled original click for purely local navigation.
- New state fields the chrome needs and `hud_state` doesn't carry: layout mode, per-button usability (`md->isItemUsable`, `companion_functional.cpp:867`), per-button ammo (`ammoForItem`, `companion_internal.h:954`), menu/window status, button-cluster visibility, dungeon-item ownership, the transform's two availability bits (`:981`, `:988`), Midna's active flag, the resolved context-tab action + clickable (`contextTabAction`, `companion_internal.h:506-518` — the PC owns this resolution, and `:515-517` is explicit that draw and touch must not both compute it; that rule now spans the wire), the small-key visibility rule (`companion_functional.cpp:159-162`), and the localized action words.
- **The dim field** (§3).
- PC replies `stream:"content_rect"` with `s_contentRect`; the phone composites the streamed page content inside its native chrome.
- Art fetched (all small, one-time, all via the existing icon channel plus a new `deco` kind): the tab plate, context-tab plate, X/Y circle base, backdrop tile, 10 HUD digits, the two transform portraits, the heart-piece wedges for the progress page. **v1 can ship with none of the plates** — `drawChamferPlate` and `drawTabPlate` already have flat-colour fallbacks (`companion_gfx.cpp:431-435`, `1112-1115`), which is exactly the look to start from.

**Verify:** touch→visible-response p50 should drop from a full round trip (150-300ms) to one phone frame. Golden-image review against the C++ Functional chrome. Exercise the layout flip mid-session (settings toggle while connected) and confirm ownership revocation. Run the native checklist.

**Revert:** the phone stops sending `own`; the PC reverts to `stream:"full"` automatically.

**Explicitly deferred:** Cinematic chrome. See §8 D2.

---

### Stage 3 — Asset pipeline v2 (~2 weeks)

Replaces render-and-capture with CPU decode for everything that has bytes in RAM.

- `asset_manifest` / `asset_request{kind,id}` / `asset{kind,id,png}` with `(kind,id)` identity. Kinds: `item`, `clct` (13 collect-archive slots, `companion_icons.cpp:230-244`), `rawicon` (archive resource index, `:291-303`), `deco` (6 plates, `:256-263`), `mapicon` (13 distinct — the table has 14 rows but `ICON_OOCCOO_e` and `ICON_OOCCOO_JR_e` both resolve to `ni_obacyan.bti`, `companion_pages.cpp:159-160`), `msgicon` (outfont index), `dmapface`.
- Path: BTI bytes → `aurora::gfx::convert_texture()` → RGBA8 → miniz PNG → base64. **No GPU, no capture slot, no warmup frames, no contention with the frame stream.** Batch requests allowed.
- **Intensity/tint textures are fetched untinted** and tinted on the phone with a ColorMatrix — the PC tints them per use via TEV black/white points (`drawTimgTinted`, `companion_gfx.cpp:1003-1023`), so one fetch serves every tint.
- Item icons are 1-2 layer composites with per-ItemType tints (`companion_icons.cpp:93`, tint table `d_meter2_info.cpp:1173-1193`): send both layers plus the tint pair and composite on the phone, or composite PC-side into one RGBA — decide by measurement, prefer sending layers so the phone can re-tint.
- **Fix two icon-identity bugs while here.** The lantern already folds oil==0 into the cache key (`companion_icons.cpp:69-72`); the Dominion Rod does not — `readItemTexture` picks `ST_COPY_ROD_B` from live `checkCopyRodTopUse()` (`d_meter2_info.cpp:973-975`) but the key is itemNo only (`:73`), so the PC dashboard already latches the wrong rod icon and a phone cache would inherit it. Also keep `iconTextureOverride()` (`companion_icons.cpp:52-61`) — it corrects two genuinely wrong `item_resource` entries and must not be "simplified" away.
- Capture remains, for exactly three classes: the dungeon map base (`companion_map_state.h:20-25` — genuinely GPU-only), heart-container art (live J2D panes, `companion_state.cpp:148-196`), and pane composites. Those keep the existing want-list/retry machinery.

**Verify:** fetch all 68 worst-case inventory icons + 24 bug icons + 13 collect slots on connect and confirm zero drops, zero frame-rate dip (the dip is the current behaviour and is the regression signal), and byte-correct decode against a PC-side reference dump of the same textures.

---

### Stage 4 — `page:items` native (~2-3 weeks)

23 cells from two static tables (`companion_pages.cpp:600-611`, `:620-627`; slot 7 is deliberately absent — it exists in the save struct but is written nowhere, and a 24th value-initialized entry once redrew slot 0 and published a duplicate hit rect, `:594-598`), three accessor calls each, one caption, one drag-scrolled reader.

- **The drop targets are chrome**, which Stage 2 already owns. This is why items comes after chrome and not before — otherwise a drag starts in a native grid and ends on a streamed button.
- **Equip semantics never move to Kotlin.** `plainEquip` (`companion_touch.cpp:158-200`) and `tryBowCombo` (`:48-91`) write savedata under the item wheel's swap-aside rules — a button whose select is taken receives the previous select+mix pair whole, trade items never land on a slot (`:121-125`), equipping the combo's mix partner dissolves the combo. The phone sends `equip{from, to, resolve:auto|combo|replace}`; sending `resolve` explicitly also removes the modal chooser's round trip (`companion_touch.cpp:142-147`).
- Description text is keyed on **`(itemNo, xyBtn)`**, not itemNo: `getStringFull(0x265 + itemNo, ..., xyBtn)` picks its X-or-Y button tags from where the item is currently equipped (`companion_pages.cpp:1446-1447`). Invalidate on equip. Send the **raw string with its `0x02` glyph markers intact**, never pre-wrapped lines — the PC cannot wrap for a font it does not have.
- Animations ported: reader grow/shrink out of the tapped cell, grid pop-down underneath, ghost pickup pop, ghost fly-out, selection tint, drop-target hot highlight (**with exactly the 8px `DROP_GRAB` margin** — a 2px disagreement once made drops succeed silently with no feedback, `companion_internal.h:341-346`), deny flash, equip-message line as `{text, durationMs}` with the phone running the timer.

**Verify:** a save with a full inventory (§9 F2). Equip every slot to every target; diff savedata against the same sequence performed through the C++ dashboard — byte-identical is the bar, and this is the one page where it genuinely is.

---

### Stage 5 — `map:dungeon` native (~2 weeks)

PC side is mostly done: `gatherMapState()` (`companion_map_state.cpp:68-124`) and `map_base` at canonical framing (`:126-150`, sent `dualscreen.cpp:801-822`) both ship. **Neither has ever run against a client** — `CompanionClient.kt:63-73` has no branch for either. So Stage 5 is the first time this code is exercised at all; budget for it being wrong in small ways.

Remaining PC work is mechanical: the floor list (`getTopBottomFloorNo`, `companion_dmap.cpp:311-313`; per-floor availability bitmask `:340-361`; boss floor `:158-160`; localized floor names `companion_pages.cpp:71-97`), the map-name plate (`:1281-1307`), Ooccoo availability and form, the "game's own dungeon map is up, yield" flag (`companion_dmap.cpp:304-308`), and three semantic actions: `select_floor{value}` (value, not row index, so the two sides cannot disagree about ordering), `ooccoo_use`, `map_reset`.

Pan, zoom and the room-follow glide become phone-local and stop crossing the wire entirely. **`pinch` must stop being sent** or the phone's local zoom fights the PC's render zoom (`companion_pages.cpp:371-386`). Drop the PC's glide for owned pages. Do **not** add phone-side smoothing to the player arrow — `getMapPlayerPos/AngleY` are already run through the frame interpolator (`companion_map_state.cpp:92-95`).

---

### Stage 6 — `page:collect` native (~3-4 weeks)

Largest in logic, smallest in bandwidth: ~35 reads, all small ints/bools/short strings, nearly all changing a handful of times per playthrough. Five sub-views, two scrolling lists, two nested zoom transitions. The only interaction that mutates game state is `equip_gear{0-6}` (`companion_touch.cpp:222-259`), which round-trips and is re-validated PC-side.

The real work is the **text pipeline**: `wrapBody` (`companion_collect.cpp:667-790`) is ~120 lines that do far more than wrap — it swallows the source's hard newlines except at sentence ends, forces a break before a bullet, collapses blank-line runs, and carries a hanging indent under bullet text. Those heuristics exist because the source strings are hard-wrapped for the game's narrow dialog box. Android gives you line breaking; it gives you none of those four. Budget ~150 lines of Kotlin and expect to eyeball it against real skill descriptions.

Prefetch the 24 bug icons on connect, not on section open — unowned bugs are drawn at alpha 55 rather than hidden (`:353-358`), so all 24 are needed regardless of save state. Do **not** port `s_collectSel` (`companion_collect.cpp:971`, `:1143`); it is only ever assigned -1 and the highlight never fires.

---

### Stage 7 — `page:guide` native (~2-3 weeks)

PC side is unusually clean and needs **no GPU at all**: the data model already serialises (`guide_doc.hpp:82-98`), the index is already JSON on disk (`store.hpp:47-49`), and images are already stored as compressed source bytes (`image.hpp:31-35`). Four straight-read messages cover it: `guide_index`, `guide_section` (serialize() output verbatim), `guide_image` (file bytes), `guide_position{chapter, section, wolf}`. ~200 lines.

Phone side is a scrollable rich-text reader with reserved-height inline images and a two-level expanding browse list — ~600-900 lines of Kotlin. Note the modality rule: the reader consumes taps only inside the content rect; corner buttons, X/Y, slots and the tab strip stay live underneath (`companion_touch.cpp:752-761`). Don't make it fullscreen-modal.

Note that `PAGE_GUIDE` is a tab only in Cinematic; in Functional it opens from the left column (`companion.cpp:636-648`). With Cinematic deferred, the guide unit is "the reader overlay", opened by `guide_open`. The PC still needs that message because `guideOpen()` is what kicks `begin_import()` (`companion_guide.cpp:1003-1023`).

A phone-side font also deletes `utf8_to_latin1()`'s reason to exist for guide text (`guide_doc.hpp:59-66`), which currently folds curly quotes and em-dashes and drops anything with no Latin-1 equivalent — pure damage to imported third-party prose.

---

### Stage 8 — `map:field`, the overworld minimap (~2-3 weeks)

**The genuinely hard one, but not for the reason the brief gives.** Cursor and icon suppression is ~20 lines: `isRendCursor()` gates both cursors (`d_map_path_dmap.cpp:807`, `:834`) and `isRendIcon()` gates both treasure passes (`:830`, `:840`); `dMap_c` overrides neither (`d_map.cpp:817-819`, `d_map.h:124`), and dusklight already adds `TARGET_PC` companion overrides to that exact class (`d_map.cpp:543-565`). It is safe: `dMap_c` is a singleton (`d_meter_map.cpp:474`) and the minimap lives permanently on the companion in both modes (`:1004-1007`). **Do not build a second `dMap_c`** — the constructor assigns the file-scope global `dMap_HIO_prm_res_dst_s::m_res` (`d_map.cpp:1183`) and a second instance stomps it.

The overworld icons need **no art at all**: 7 shared 4-bit intensity shapes tinted per type-group (`d_map_path.cpp:71-80`, colours `d_map_path_dmap.cpp:1026-1031`, `:1098-1103`), 5 of which dusklight already repaints procedurally (`hq_minimap.cpp:74-143`). Draw all 7 as Canvas primitives. The player cursor is a bare 3-vertex triangle (`d_map_path_dmap.cpp:1189-1225`).

The hard part is **framing**. The minimap render target is 96-216 logical texels (`d_meter_map.cpp:440-465`) and room-centred, versus the dungeon's 448². There is no cheap canonical whole-stage image. Two options:

- **(A) Whole-stage canonical.** Needs a bigger texture, which touches `mTexSizeX/Y` (fixed in the constructor, `d_map.cpp:1223-1240`; `changeTextureSize()` is DEBUG-only) and moves `mPackX/Z`, `mRightEdgePlus`, `mTopEdgePlus`, which are in pixel space. ~2-3 days plus real regression risk on the main-screen minimap.
- **(B) Re-capture the room-centred view on room change.** Sidesteps the texture entirely. Costs one PNG per room transition and a visible seam when crossing a boundary. Still ~0 amortized bandwidth versus a continuous frame stream.

**Recommend (B) first** because it is reversible and does not touch shared decomp geometry. Also note the copy-2D drawlist holds exactly 4 entries and `dDlst_list_c::set` drops overflow **silently** (`d_drawlist.h:520`, `d_drawlist.cpp:2002-2009`) — `companion_dmap.cpp:300-308` already documents hitting this. Any new render pass competes for that budget.

---

### Stage 9 — Retire the per-frame path (~1 week)

When every unit is owned for the Functional layout, the PC replies `stream:"none"` and the per-frame capture/encode/send path is deleted **for phone clients**. What survives: the on-demand image channel (map base, hearts, pane composites) and, if Cinematic phone support was kept (§8 D2), the Cinematic stream in full. `endHudCapture()`'s draw and `set_source()` survive untouched for the Thor and the desktop second window.

Be honest in the changelog: *the per-frame PNG stream dies; the capture mechanism does not.*

---

## 6. What gets dropped rather than ported

| Dropped | Where | Why |
|---|---|---|
| Midna "pikari" sparkle, Vessel of Light tear sparkles | `companion_hud.cpp:322`, `:694-696` | Live particle draws issued by the game's own meter; no numeric driver and no static representation. In Functional they render on the main screen anyway. |
| `drawSpecialPanel` (fish weight, "you got" strings) | `companion.cpp:1531-1564` | Re-hosts arbitrary game 2D display lists. Not state-reducible by any decomposition. Cinematic-only; stays streamed or the layout drops it. |
| Kantera radial gauge art | `companion_hud.cpp:102-111` | Functional already replaces it with a drawn bar (`companion.cpp:324-337`). Use the bar in both. |
| The game's stylised HUD digits | `companion_gfx.cpp:1255-1273` | Phone font instead. A visible look change; revisit as a 10-texture fetch if the user objects (§8 D1). |
| Pixel-exact typography: fitted sizes, ellipsis points, the 40%-of-lane sender cap, the title-vs-Back-plate collision guard | `companion_gfx.cpp:763-834`, `companion_collect.cpp:1155-1158`, `:498-509` | All tuned against the game font's metrics. Re-derived on Android with `TextUtils.ellipsize` + autosizing. |
| No-inertia hard-clamped scrolling | `companion_gfx.cpp:531-539` | Replaced by native fling. A deviation, and an improvement — it is half the point of moving the reader onto the phone. |
| Custom scroll-knob | `companion_gfx.cpp:541-551` | Platform scrollbar. |
| `s_collectSel` row-selection highlight | `companion_collect.cpp:971`, `:1143` | Dead: only ever assigned -1. There is no `CTX_READ`. |
| Frame-accurate dim curve | `companion.cpp:847-967` | The phone gets a quantised u8 on the frequent channel. The three-phase ratchet/wake state machine stays PC-side and is not reproduced. |
| Cinematic ornament frame (LINE2 rules, KAZARI flourishes) | `companion_hud.cpp:707-743` | Only if Cinematic is ported at all (§8 D2). |

Two things that are **not** dropped despite looking droppable: the low oil/oxygen danger pulse (`companion.cpp:313-323`) — the overlay skipped it only because it repaints on message arrival, and a real render loop removes that constraint; and the 8px `DROP_GRAB` margin, which must match `dropTargetAt` exactly.

---

## 7. The genuinely hard parts, not understated

**Fonts.** Every label everywhere goes through `mDoExt_getMesgFont()` (`companion_gfx.cpp:580-586`), and text is *measured* with that font's metrics for centring and fit (`:623-641`). The font is `rodan_b_24_22.bfn`, the single resource in the disc's `fontres.arc` — game art, cannot be bundled. Streaming the atlas is possible but worse than it sounds: the metrics are unknown from source alone (they live in the .bfn's INF1/GLY1/WID1/MAP1 blocks and nobody has dumped them); `JUTResFont::loadImage` indexes the cell grid with `cellRow = idx / numRows; cellCol = idx - cellRow * numRows` (`JUTResFont.cpp:485-487`) — `numRows` where `numColumns` belongs, correct only for a square grid, and a phone reimplementation must copy the quirk verbatim; the companion canvas is only 456 logical units tall, so a ~24×22 bitmap font upscales ~5x on a modern phone, which is a legibility *regression* on the one surface that is nothing but prose; and JPN/CN defeat it entirely (paged `JUTCacheFont` with Shift-JIS lead bytes, `m_Do_ext.cpp:3762-3773`; `initJoinedTexture` requires `mGly1BlockNum==1`). **Recommendation: bundle a neutral font.** Keep the atlas as an optional later fidelity pass for short chrome labels only.

**The asset pipeline.** Smaller than the brief assumes once `convert_texture` is used, but it is four new kinds, a two-layer tint composite, TLUT handling for CI8, and identity keys that are not always just itemNo. The current pipeline does not merely cost too much — it *does not complete correctly* for 23 simultaneous requests.

**The overworld minimap.** §7 Stage 8. The framing decision is a design question, not a coding one, and it is the one place where the plan touches shared decomp geometry.

**Input ordering.** Hit-testing lives inside the draw (`companion.cpp:1788-1789`). The extraction is mechanical but the ordering it encodes was tuned against live bugs, and a mis-ordered port re-introduces them on the phone only, where nobody is looking.

---

## 8. Decisions for the user, not for the implementer

- **D1 — Font.** Bundle a neutral font (recommended: zero transfer, better legibility, works in JPN/CN, deletes `utf8_to_latin1`'s damage) and accept that the phone is "close, not identical"? Or invest in the atlas for fidelity?
- **D2 — Cinematic on the phone.** Support it natively (large: pane composites, counter panes, Vessel of Light, ornament frame, `drawSpecialPanel`), keep streaming it indefinitely, or tell phone users the phone is a Functional-mode device? **Recommend: Functional-only natively, Cinematic keeps streaming.** This changes the end state — the per-frame path would not be fully deleted.
- **D3 — Phone art cache lifetime.** Session-only (recommended default, and what the policy implies) or persistent on disk? Persistent makes the APK a container of game art that survives across sessions, backups and device sharing. The guide's third-party images are a separate question (§8 D6) and arguably the opposite answer.
- **D4 — Overworld framing.** Room-change re-capture (recommended, reversible) vs whole-stage canonical (touches shared decomp code).
- **D5 — `companion_dmap_icons.inc`.** Three byte-identical `dmapres.arc` BTIs are compiled into the PC binary, contradicting `README.md:48`. Remove them and read from disc, or amend the policy statement? The APK rule stays absolute either way, but the plan should not cite the PC tree as precedent.
- **D6 — Guide content.** It is neither Nintendo art nor dusklight data — it is zeldadungeon.net prose and screenshots the user saved with their own browser. A ~23-chapter guide is ~2000 images at ~40KB; "session-only cache" means re-fetching tens of megabytes on every connect. Does the session-only rule apply to content the user downloaded themselves?
- **D7 — Image codec.** Only miniz (PNG/deflate) is vendored; there is no JPEG or WebP encoder. Adding one would shrink the residual image channel further. Worth a new dependency, or stay PNG?
- **D8 — Fidelity bar.** Agree "close, not identical" in writing, before the first screenshot comparison.
- **D9 — Dominion Rod icon.** Fix the identity bug on both sides (add charge state to the key) or accept the existing latched-icon behaviour?

---

## 9. Test fixtures and save states

The available save is very early game — 3 hearts, no lantern, never enters a dungeon — which already blocks verifying finished features (hearts beyond 3 states, the oil gauge, `map_state`, `map_base`, floor lists, dungeon items). This is a real blocker and the plan addresses it two ways.

**Primary: the transcript recorder + mock server (Stage 0).** Record real protocol traffic once, from a real session on a real save, and replay it into the APK forever. Most Kotlin work then needs no disc, no PC and no save. This is why it is Stage 0 and not a nice-to-have.

**Still needed — real saves, for PC-side and end-to-end verification:**

| # | Fixture | Unblocks |
|---|---|---|
| F1 | Post-Forest-Temple, inside a dungeon | `map_state`/`map_base`, floor list, boss floor, dungeon items, small keys, Ooccoo |
| F2 | Late game, near-full 23-slot inventory incl. bottles, bow + all 3 bomb types, Dominion Rod (both charge states), Ooccoo in slot 18 | Inventory grid, combo chooser, icon burst, the rod identity bug |
| F3 | 8+ hearts with heart pieces at each of the 5 container states and each of the 4 wedge states | Heart art fetch, progress page |
| F4 | Lantern with oil >0 and oil ==0; an underwater spot | Oil/oxygen gauges, lantern icon identity flip |
| F5 | Wolf form, mid-morph reachable | Wolf blend, wolf action words, slot readouts, `map:field` wolf marker |
| F6 | Several hidden skills learned, several letters received, fish caught, golden bugs partially collected | Collection page, and the first game text that is not ASCII |
| F7 | A PAL/EU save with a non-English language | The LATIN-1→UTF-8 fix, inches-vs-cm record units (`companion_collect.cpp:392-393`) |
| F8 | A dark-area save with the Vessel of Light partially filled | Vessel state (Cinematic; Functional puts it on the main screen) |
| F9 | A save with a guide imported and a chapter's images present | Guide index/section/image transport |

**Cheapest way to produce most of these:** `src/dusk/commands.cpp` already exists. A dev-only console command that sets savedata bits (grant item, set max life, set dungeon items, set event bit, set language) turns F2/F3/F4/F6 from "play the game for 20 hours" into "type four commands". Verify it exists/can be added before assuming it; it is likely the single highest-leverage test investment after the mock server.

---

## 10. Verification strategy

**Every stage, without exception — the native-path regression checklist:**
1. Desktop, second window, no phone connected, Functional: dashboard renders, tabs switch, drag-equip works, map pans.
2. Same, Cinematic.
3. Android (AYN Thor) on a real second display, both modes. Specifically confirm the second panel **is not frozen on a stale frame** — that is the failure mode the suppression gate introduces (`aux_window.hpp:41-44`).
4. Both of the above **with a phone connected**, confirming the phone's ownership does not affect what the local panel shows.
5. Mid-session HUD-mode flip while a phone is connected: ownership revoked, re-declared, no stranded animation ramp, no swallowed taps (`companion.cpp:1753-1765`).
6. Disconnect and reconnect: phone caches cleared, no stale animation state.

**Per-stage additions** are listed inline above. Two cross-cutting ones:

- **Latency numbers in every PR** from Stage 0 telemetry: frame interval p50/p95, bytes per frame, touch→visible-response p50/p95, same save and same route as the previous stage. A stage that does not move a number it claimed to move gets reverted, not argued about.
- **Golden-image review per stage** for anything visual: `requestScreenshot()` for the C++ side, `adb exec-out screencap` for the phone, side by side, human eye. Not SSIM — the fonts differ by design.

---

## 11. Honest summary of the end state

- **Two renderers of the same dashboard, permanently.** The C++ one for the Thor and the desktop second window; the Kotlin one for phone clients. This is the cost of the approach. It is managed by generated layout constants, a parity table, a PR checklist and a per-release eyeball pass — not eliminated.
- **The PNG *stream* dies for phone clients. The *capture mechanism* does not** — it still serves the dungeon map base, heart art and pane composites on demand, and it still feeds the local panel with `set_source` every frame.
- **The phone will not be pixel-identical.** Different font, no sparkles, no special panel, platform scrolling.
- **Anything that writes savedata stays on the PC, verbatim.** That is not a compromise; it is the correct place for it, and it is the one thing genuinely shared between the two renderers.
- **If only the first two stages ever ship**, the user's complaint is still largely fixed: the stream is a quarter the size and every button they press responds locally. Every stage after that is architecture paying down, not lag paying down. That is the point of the ordering.