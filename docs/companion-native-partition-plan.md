# THE PLAN — Making the phone a real companion app

**Goal, in the user's words:** *"phone screen is acting like companion app which had their own logic with game data state come from stream, so switching tab items, map etc, selecting and equipting should instant, interacting with map should smooth."*

**Thesis:** the PC stops sending pictures to phone clients and starts sending *game state*. The phone owns layout, animation, gesture, view state and hit-testing. The PC stays the only authority on savedata and on anything only the game knows. The in-process C++ renderer is untouched and keeps driving the AYN Thor and the desktop aux window.

**Current baseline:** the PC renders the whole bottom screen, reads it back off the GPU, PNG-encodes it (865,623 bytes per capture, ~66 ms cycle — `dualscreen.cpp:505-516`) and pushes it. The phone forwards raw touch coordinates; the PC hit-tests them at the *tail* of the draw (`companion.cpp:1818-1822`), against rects that same frame published, at display rate. That round trip is the lag, and it is also **lossy**: `handleTouch` consumes exactly one queued tap per frame (`companion_touch.cpp:661-664`) and `touchEvent` overwrites the pending slot on every down (`companion.cpp:707`). Two taps inside one companion frame lose the first, today.

---

## 0. Three constraints, and a correction to one of them

**C1 — No game art, fonts or audio in the APK. Ever.** Everything from the disc is fetched at runtime from the user's own running instance and cached for the session. Original assets authored for Dusklight are fine (the UI click sounds, the player arrow, `companion_logo.inc`).

**C2 — The native dual-screen path keeps working, fully intact.** "All logic on the phone" applies *only* to a phone acting as the bottom screen over Wi-Fi.

> **C2 is already violated by the spike build, and fixing it is a prerequisite, not a consequence.** Verified:
> - `dualscreen.cpp:143-148` — when `DUSK_PHONE_SPIKE` is compiled in, the desktop aux window is created with `.hidden = true` unconditionally.
> - `dualscreen.cpp:437-472` — every `hello` **destroys and recreates** the single aux window at the phone's reported resolution.
> - `computeAuxCanvas()` derives the dashboard's canvas aspect from `aurora::auxwin::get_surface_size()`, so a connecting phone silently reshapes what a native second screen would show.
>
> A phone does not currently *share* the aux surface with a native second screen — it *takes it over*. Stage 0 fixes this (§3.0). Nothing else in this plan is safe to build until it does.

**C3 — Verification is blocked.** The only save on the test machine is 3 hearts, no lantern, never enters a dungeon. Eight completed features have never executed their success path. Every stage below states its fixtures; §8 is the master list.

---

# Part I — The partition

This is the centrepiece and the checkable part. If an interaction is in §1.1 it must never touch the network. If it is in §1.2 the phone must paint it before the packet leaves. If it is in §1.3 the phone must never guess it.

### The rule used to classify

> **Local** if the output is a pure function of (gesture, phone-held data, phone-held layout) — *and nothing outside the companion reads the state it mutates*.
> **Local-then-notify** if the phone can paint the outcome correctly from what it already holds, but the *truth* lives in savedata or the actor world.
> **PC-driven** if deriving it requires a game accessor, a live pane, or a NULL-prone pointer. Stream the *resolved answer*, never the inputs.

Two buckets the three-way split misses and the design needs anyway: **§1.4 PC→phone commands** (the PC force-navigates in five places) and **§1.5 never local** (rendered pictures with no state decomposition).

---

## 1.1 PURELY LOCAL — instant, no PC involvement

### A. Gesture and animation (all of it, without exception)

Every animation on the dashboard is one of two shapes and is already consolidated into one constant table on the PC (`companion_internal.h:784-808`). None of it is ever streamed; the phone runs its own clock.

| Thing | PC reference |
|---|---|
| Tab raise (`static f32 s_raise[PAGE_COUNT]` easing 8.0/0.0 at `ANIM_RATE_GLIDE`, keyed only on "is this the active page") | `companion_hud.cpp:447-463` |
| Page-change transition — grow-out-of-tab + outgoing crossfade | `companion_hud.cpp:800-880` |
| Button press depress `FN_CORNER * (0.07*pressAnim - 0.05*popAnim)` | `companion_functional.cpp:855-860, :1122-1126, :974-978, :901-905` |
| Deny-flash fade `255 * s_denyFlash[i] / DENY_FLASH_FRAMES` | `companion_functional.cpp:889-893, :1176-1180` |
| Low-gauge danger pulse (`static f32 sPulse`, render-rate) | `companion.cpp:314-322` |
| Rupee / Poe count roll-up (`sShownRupee`, `sShownPoes` chasing the target) | `companion_functional.cpp:61-78, :771-780` |
| Wolf morph blend `s_wolfBlend` (a per-frame ease off **one bit**) | `companion.cpp:1036-1056` |
| Left-box carousel swipe physics + decay at `ANIM_DECAY_FAST` | `companion_functional.cpp:508-521` |
| Floor-change fade (function-local statics, zero game state) | `companion_pages.cpp:402-425` |
| Floor-picker open/close ease + per-row interpolation out of the tab rect | `companion_pages.cpp:932-942, :996-1023` |
| Floor-picker detent haptics as the finger crosses rows | `companion_touch.cpp:1069-1085` |
| Reader / collect zoom open-close (grow out of the tapped cell, grid pops down and fades) | `companion_touch.cpp:840-852`, `companion_pages.cpp:1517-1541`, `companion_collect.cpp:1270-1310` |
| Guide open/close ramp `s_guideT` | `companion_guide.cpp:1253-1263` |
| Drag pickup: 10px slop promotion, ghost pop, `HAPTIC_LIGHT` | `companion_touch.cpp:1026-1043, :1089-1101` |
| Ghost follow render (52px icon + coloured silhouette halo riding the finger) | `companion_hud.cpp:1062-1077` |
| Ghost fly-out: shrink into the consumed target, or fly home on miss/deny | `companion_touch.cpp:1188-1218`, `companion_hud.cpp:1050-1061` |
| Drop-target hot highlight (point-in-rect, 8px grab margin) | `companion_internal.h:334-340` |
| Tap-vs-drag disambiguation (100px² slop clears the pending row tap; deferred tap fires on release) | `companion_touch.cpp:1089-1101, :1150-1168` |
| Pinch cancels an in-flight drag and any deferred tap | `companion_touch.cpp:977-991` |
| Map reset-view camera ease (the *camera* half only) | `companion.cpp:1844-1867` |
| Equip-message countdown (once the PC sends the text + a duration in **ms**) | `companion.cpp:1735-1737` |

### B. Layout and geometry

| Thing | PC reference | Note |
|---|---|---|
| Tab strip plate rects from `(x0, x1, h)` + a page list | `companion_hud.cpp:482-557` | **Received as data, not recomputed** — see §4.2 |
| Backdrop tiling (`DECO_BLOCKS` at 172px, fixed tint) | `companion_hud.cpp:565-580` | pure after one art fetch |
| Window frame + ornaments (`DECO_LINE`, `DECO_KAZARI`, four fillRects) | `companion_hud.cpp:707-745, :465-476` | zero game state |
| Corner-box plate — chamfered fills, vertical gradient, gold rim | `companion_gfx.cpp:349-369` | **100% procedural, zero game art**, documented as never moving |
| Gauge bar construction (five fillRects, hardcoded colours) | `companion.cpp:328-345` | Functional only; Cinematic's is a live game object — §1.5 |
| Day/night glyph (24-step scanline lune; 8 tapered fillPoly rays + fillDisc) | `companion_functional.cpp:289-345` | procedural |
| D-pad glyph | `companion_hud.cpp:911-919` | hand-drawn *because* the game's is unreproducible |
| Content-window fill + rect publish | `companion_hud.cpp:757-772` | |
| Carousel page dots | `companion_functional.cpp:451-461` | |
| Inventory grid: 23 cells, two compile-time tables (5×5, 6×4) chosen by aspect ≥ 1.7 | `companion_pages.cpp:601-628, :693-717` | `static_assert`s already prove the tables agree (`:666-668`) — generate both into Kotlin, §4.1 |
| Inventory cell hit test (linear scan, square of side `s_invCell`) | `companion_touch.cpp:402-411` | |
| Cell painting: 5 group tints, selection highlight, plate | `companion_pages.cpp:670-676, :721-736` | |
| Down-on-content gating (did the gesture start over the content window) | `companion.cpp:694-703` | resolved at ingress, before any per-frame pass |
| Corner grace margin | `companion_touch.cpp:763-777` | **belongs on the phone**: `CORNER_GRACE=12` exists because corners "hug the screen edge, where devices shave touchable area". The phone knows its own cutouts and gesture insets; the PC does not |

### C. View state — mutates nothing outside the companion

| Thing | PC reference | Proof it's inert |
|---|---|---|
| All four list scrolls (`s_scrollSkills`, `s_scrollMail`, `s_scrollBody`, `s_scrollGuide`) + `s_scrollItemInfo` | `companion_touch.cpp:1061-1063`, `companion_gfx.cpp:531-539` | raw pixel accumulators clamped by arithmetic; no game call anywhere in the path |
| Inventory cell selection `s_selSlot` | `companion_touch.cpp:1224-1235`, `companion.cpp:64` | plain `int`; **but it is read by `inEquipMode()` — see §1.2 footnote** |
| Empty-cell tap swallow (consumes the gesture, deliberately does *not* deselect) | `companion_touch.cpp:1225-1235` | |
| Grid untappable while the info panel is up or animating (`s_invGeomValid=false`, cells still drawn as a fading backdrop) | `companion_pages.cpp:1526-1541`, `companion_touch.cpp:996-997` | deliberately dead cells |
| Map pan and zoom — the *gesture* | `companion_pages.cpp:372-398`, `MapOverlayView.kt:220-257` | against a canonical base image this is a pure 2D transform |
| Reader "< Back" (rect id **-4**, not -2 as `companion_internal.h:620-624` claims) | `companion_collect.cpp:485-491`, `companion_touch.cpp:485-490` | |

### D. Content the phone holds outright

| Thing | Why local | Reference |
|---|---|---|
| **Guide browse list, expand/collapse, prev/next, wrap, image decode** | `rebuildBrowseList` reads `index.json` and sorts by chapter — not one game accessor | `companion_guide.cpp:347-404, :921-948, :960-997, :645-748, :484-605` |
| Guide chapter expand with scroll correction (holds the tapped row under the finger) | pure list-model work | `companion_guide.cpp:921-948` |
| Companion's own UI strings (`RESET`, `OOCCOO`, `WARP`, `NO MAP`, `LOADING FLOOR`, `MAP ON MAIN`, `STR_DRAG_EQUIP`, …) | authored for Dusklight, **no game-archive equivalent** — the APK may ship and localize them | `companion_strings.h:1-11`, `companion_strings.cpp` |
| Map placeholder logo | `companion_logo.inc` is original project art | `companion_pages.cpp:1087` |
| **The phone's own battery** | see §1.5-drop | |

### E. Feedback

UI sounds and haptics for everything above. The PC already downgrades game SE ids to seven semantic cues (`ok`, `cancel`, `error`, `page`, `equip`, `cursor`, `warp`) plus four haptic weights *precisely so the phone can synthesize its own* (`companion.cpp:1467-1485, :1516-1523`), and the APK already ships its own `ui_*` wavs in `res/raw/`. A locally-decided tap plays a locally-owned sound with no round trip.

---

## 1.2 LOCAL THEN NOTIFY — the phone paints it, then tells the PC

**Shape:** sequenced intents with state-echo reconciliation. The phone assigns a monotonic `seq`, applies the intent to its own model immediately, and queues `(seq, verb, args)`. Every PC→phone state message carries `ack` = highest seq fully processed. On receipt the phone drops everything `seq <= ack`, rebases on the received state, and replays what is still pending.

**There is no ack/nack message.** If the PC refused, the echoed state simply does not contain the change; when `ack` passes the intent's seq the prediction evaporates. *`ack` advancing with no state change **is** the nack.*

> **One required change makes this work.** `pollAndPushSpikeState()` currently returns early when nothing changed (`dualscreen.cpp:588-590`, verified). It must become `if (s_haveLastSent && state == s_lastSent && s_lastSentAck == lastAppliedIntentSeq()) return;` — otherwise a refused action hangs the phone's prediction until its 500 ms timeout. This is the single most load-bearing line in the reconciliation design.

| Verb | Phone paints instantly | PC mutates | Predicted? | Refusal outcomes |
|---|---|---|---|---|
| `set_page` | tab raise + page transition | `s_page` (+ `guideOpen`/`guideClose` side effects) | full | forced back by §1.4 |
| `select_slot` | cell highlight | `s_selSlot` — **must be sent**, see footnote | full | — |
| `collect_tab`, `reader_open/close`, `item_info`, `left_box_page`, `guide_*` | zoom/slide animation | companion-only view state | full | forced back by §1.4 |
| `equip_item_plain` | ghost flies into target, icon swaps | `dComIfGs_setMixItemIndex(btn,NONE)` → redistribution loop → `setSelectItemIndex(btn,slot)` last. **8 bytes of savedata, order load-bearing** (`companion_touch.cpp:158-200`) | full — deterministic, ~40 lines of Kotlin given `checkTradeItem` (cacheable per itemNo) | `refused:menu`, `refused:wolf`, `refused:trade`, `refused:borrowed` |
| `equip_item_combo` (bow + bomb bag / hawkeye) | combo badge | clears existing bow mix on other buttons, clears a partner elsewhere, `setMixItemIndex(btn,SLOT_4)` + `setSelectItemIndex(btn,slot)` | full | as above |
| `slot_unbind` | binding clears | `setSlotBinding(which,-1)` → mix=NONE then select=NONE | full | — |
| `context_action` (**one verb, no action id**) | tab press | PC re-resolves via `contextTabAction()` (`companion.cpp:1365-1433`) exactly as the touch path does | press only | the resolved pair is streamed; phone never re-derives |
| `button_down` / `button_up` (X, Y, I, II) | depress at `ANIM_RATE_SNAP` — documented as "must land under the finger" | `beginHold`/`releaseHold` → `s_padHoldMaskState` → `mDoCPd_c::read` | press only; verdict is PC's | `button_verdict {id, accepted, reason}` → deny flash |
| `transform` | corner pulse `s_pressAnim[4]` | `s_transformReq`, consumed by `consumeTransformRequest()` | pulse only | — |
| `z_press` | corner pulse `s_pressAnim[5]` | `s_zPressReq`, ORed into pad 0 for that frame | pulse only | — |
| `floor_select` | **row highlights instantly** | `s_dmapFloorSel` → `companion_dmap.cpp:435` | highlight only | image cannot be predicted — multi-frame capture |
| `map_reset` | camera eases home over cached pixels | `s_mapResetGlide`, `s_dmapResetReq`, `s_dmapFollow` | camera half only | room-fit zoom needs `getRoomMinMaxXZ` — PC-only |
| `equip_gear` (7 boxes) | pressed/pending state, **not** the worn plate | `dMeter2Info_setSword` / `setShield`+`setShieldChange()` / `setCloth`+`setClothesChange(0)` | **never predict** — one path does a full Link-archive reload and takes many frames | `refused:menu`, `refused:wolf`, `refused:reloading` (silent today), `refused:unowned` |
| `ooccoo` | plate flash | multi-frame borrow-and-restore choreography (`companion_touch.cpp:558-630`) | **never predict** | `unavailable` |
| `warp` / `warp_toggle` | button press | `s_warpReq` / `requestWarpToggle()` → `f_ap_game.cpp:808-813` | press only | **the effect lands on the MAIN screen** — there is nothing on the phone to predict |

> **Footnote — `s_selSlot` is the easiest coupling to miss.** It looks like pure phone state. It is read by `inEquipMode()` (`companion.cpp:788-792`) and by `contextTabAction`'s `CTX_INFO` clickability (`companion.cpp:1408-1409`). If the phone owns the grid and does not stream the selection, `inEquipMode()` reads false, the drop rects never publish, and the context tab the phone renders from the PC's resolved pair goes dead *exactly when the user selects something*. Stream `select_slot` and a `dragging` bit, or give `inEquipMode` a phone-supplied override. Budget for it.

**The combo chooser never crosses the wire.** `bowComboAmbiguous` needs only the four mix indices (`companion_touch.cpp:507-514`), which the phone already has. The phone raises its own chooser and sends the *resolved* verb. This deletes an entire trap: `equipFromCompanion` returning `true` does **not** mean an equip happened — the ambiguous branch returns true having mutated nothing (`companion_touch.cpp:142-147`) and the caller then plays the succeeded-animation anyway (`:1195-1198`).

**Button presses are not intents.** They go through a separate, unsequenced path. Their payload is a *duration*: bow-class items aim on hold and fire on release, and short taps are stretched to `TAP_HOLD_FRAMES=24` game frames (`companion_internal.h:385-392`). Two events, never one "pressed". Add a **hold lease**: force-release after ~24 frames of silence, implemented in the action layer and *never* inside `beginHold`/`releaseHold`, which are shared with the native path and the physical `USE_SLOT_ITEM_1/2` binds (`companion.cpp:1296-1303`). A lost `button_up` over Wi-Fi otherwise leaves the bow drawn indefinitely.

**Refusal presentation.** The phone fires accept cues locally and immediately. It fires **no deny cue of its own** — every PC refusal path already calls `queueSound(Z2SE_SYS_ERROR, HAPTIC_DENY)` and that already reaches the phone as a semantic cue. The rollback *animates* (ghost fly-home + deny flash), it does not snap. The reason rides on the state as `{"equipMsg": "...", "equipMsgSeq": N}` — `setEquipMsg` already writes localized text, and two of those strings interpolate archive content (item names, "Hawkeye"), so **the phone must never author refusal text**.

---

## 1.3 PC-DRIVEN — only the game knows it

Organised by change rate, because rate decides the channel.

### Tier A — per-frame (`hot` channel, ≤1 message/game frame, only when a quantized field changed)

| Field | Source | Quantization |
|---|---|---|
| Player `u`, `v` | `dMapInfo_n::getMapPlayerPos()` — room-origin-corrected, frame-interpolated; **not** the actor's raw transform, so the phone genuinely cannot derive it (`companion_map_state.cpp:92-97`) | 1/4096, exists (`dualscreen.cpp:663-668`) |
| Player heading | `getMapPlayerAngleY()` | 0.25° — **and fix the truncation** (`dualscreen.cpp:671` uses `(int)`, u/v round; use `floorf`) |
| `usable0..3` | `md->isItemUsable(b)` — a snapshot taken once per game frame in a narrow truthful window (`d_meter2_draw.cpp:4354-4359`) | 4 bits |
| `clusterVisible`, `xyHidden`, `menuOpen`, `useBlocked`, `wolf`, `eventRunning`, `midnaActive`, `midnaAvailable`, `gearAllowed`, `borrow1/2`, `transitionArmed` | see §5 collapse rules | 1 bit each |
| `winStatus` | `dMeter2Info_getWindowStatus()` — **status 2 (item wheel) keeps X/Y visible where every other status hides them** | u8 |
| **`dim`** | `currentDim()` — see §6 defect D1 | u8 0..255 |
| `oil%`, `oxygen%` + 2 visible bits | `gatherGauges` (`companion_state.cpp:65-91`) — **move out of `hud_state`** (§5 R1) | already % |
| `fps` | only when `enableFpsOverlay && fpsOverlayCorner == kFpsCornerCompanion` | int, 2 Hz cap |
| moving map icons (light ball, cannon ball, pushed statue) | `companion_dmap.cpp:104-172`; conceded at `dualscreen.cpp:701-706` | 1/1024 (sub-texel at 448 logical texels), as `(index,u,v)` deltas |

**Budget: ~24 bytes binary / ~170 bytes JSON per frame, ≤60 Hz → ≤10 KB/s worst case**, against ~13 MB/s for the PNG stream it replaces. Three orders of magnitude.

### Tier B — on-change

| Channel | Contents | Rate |
|---|---|---|
| `hud` | `life`/`maxLife` (**note the two divisors: life is 4 units/heart, maxLife is 5** — `dualscreen.cpp:1073-1075`), rupees, keys, **`keysVisible`** (a stage flag via `dStage_stagInfo_ChkKeyDisp` plus a field/dungeon distinction — changes on *room change, not with the count*), `lowLifePopIn` (latched bit, hysteresis already applied PC-side) | seconds |
| `equip` | 4× display binding from `equippedItemForPhone()` (**display, not raw selection** — the Ooccoo borrow stays invisible, and the lantern→`KANTERA2` swap on empty oil); 4× **resolved** item from `dComIfGp_getSelectItem` (bow+bomb→`BOMB_ARROW` etc., `d_com_inf_game.cpp:2036-2085`); 4× select + 4× mix index. **Must also push when nothing on the phone did it** — `sanitizeSlotBindings()` runs unconditionally every frame and can clear a binding the ring menu invalidated (`companion.cpp:1176-1214`) | per equip |
| `inv` | 23 slot itemNos — **diff the whole array, never hook the equip paths**: an emptied bomb bag auto-unequips bomb arrows (`d_meter2.cpp:2727-2760`) and hot-spring water cools on a timer rewriting slots 11-14 (`d_meter2_info.cpp:1619-1649`), both far from any companion code. Plus **raw** ammo (arrows, 3 bomb bags, seeds, 4 bottle counts) so the phone can reproduce *both* accessor variants — the grid path and the button path legitimately disagree | per pickup / per shot |
| `map` | `active`, `floor`, `playerFloor`, `baseGen`, **plus everything the floor picker needs and none of which is streamed today**: `floorTop`/`floorBottom` (`getTopBottomFloorNo`), `floorAvail` u16 mask (`companion_dmap.cpp:340-361`), `bossFloor` (`:158-160`), floor name strings, `dmapAvailable` vs `dmapReady`, `yieldedToMainScreen` | floor/room change |
| `map_icons` | **split static from moving** — static set (chest, key, boss, destination, dungeon enter, Lv8 warp, light drop) on status change; moving set in `hot`. Today the *entire* array resends when any one icon moves, and `rot` is compared **exactly** while u/v are quantized (`dualscreen.cpp:684`) — the restart marker is the one rotated icon, so a jittering angle fires all 96 every frame | mixed |
| `chrome` | `layoutMode` (`mainHudRestored()`); `visiblePages[]` (3 in Functional, 4 in Cinematic); **resolved `(contextAction, clickable)`** — never let the phone re-derive it, it reaches `alink->checkAcceptWarp()` and a NULL-prone `getStagInfo()`; `warpPortalsShown`; 3 dungeon-item bits; `leftBoxPages[]` + forced page | menu/room/layout change |
| `chrome.words` | `getActionTextA/B/XY`, `getDpadLabel`, menu/map prompt words — **change several times a second**. String-intern them: `str_def {sid, utf8}` once, reference `sid` (u16) after. Key on a **content hash, not the `const char*`** — these point into reusable game buffers | several/sec |
| `place` | hour/minute quantized to **5 game minutes** (a game minute ≈ 1 real second — `companion_functional.cpp:347-352`), `night` bool, area name as `sid` | ~0.2 Hz |
| `collect` | 24 bug bits, 6× fish (num, size), 7 skill bits, `arrowMax`, poes, letter count, `maxLife % 5`, scent, `maskMdlVisible` + crystal/mirror count | tens per playthrough |
| `gear` | worn sword/shield/clothes + **7 gear box identities** — `gearItemFor()` branches on save state for boxes 1 and 2 (Light vs Master sword, Wooden vs Ordon shield), so send the itemNo per box, never a fixed table — plus ownership bits | ~9 per playthrough |
| `guide_pos` | `(chapter, section, isWolf)` — two ints and a bool, derived from `getStartStageName`, 4 dungeon flags, ~20 event bits and one item bit. **Stream the position, never the marker's pixels** | stage load |

### Tier C — one-shot, identity-keyed

Art (§5), strings by msg id (§5), the guide store (§3.2), the layout description (§4.2), the constants blob (§4.1).

---

## 1.4 PC→PHONE COMMANDS — the bucket the three-way split misses

The PC force-changes companion navigation in **five** places. A phone that owns its own navigation and is not told will silently diverge.

| Site | Trigger |
|---|---|
| `companion.cpp:1744-1750` | `drawDashboard` resets `s_page` to `PAGE_MAP` whenever the current page is not in the layout's visible set — fires on a Cinematic→Functional switch taken while on GUIDE |
| `companion.cpp:622-626, :652-671` | F9 → `nextPage()` |
| `companion.cpp:1110-1135` | `cancelReadersOnDamage` yanks COLLECT out of any reader when Link takes a hit |
| `companion.cpp:1146-1175` | `resetCompanionOnLeaveGameplay` on quit-to-title / game-over / file-select |
| `companion_functional.cpp:479-488` | `drawLeftInfoBox` force-stores `s_leftBoxPage = LEFT_BOX_CONTEXT` on the rising edge of entering a dungeon or starting a tears quest |

Plus the chooser self-cancel (`companion_hud.cpp:1010-1013`) and the guide's refusal to close on `PAGE_GUIDE` (`companion_guide.cpp:1033-1038`).

**Mechanism:** a `nav` message carrying `{page, collectTab, readerSel, itemInfoSlot, leftBoxPage, guideOpen, guideSection, floorSel, floorPickOpen, gen}`, **not supersedable**. The phone discards its own optimistic value whenever an incoming `nav.gen` post-dates the `seq` it last sent. Without this the tab strip bounces and nobody will be able to explain why.

Also in this bucket: `presentation {mode: "dashboard"|"splash"|"held", dim}`. `mode:"held"` is required because during a stage transition the PC deliberately skips the capture so the second screen holds its last frame (`dualscreen.cpp:1126-1129`) — a phone rendering locally has no equivalent signal and would draw a live-looking dashboard from stale state through every load.

---

## 1.5 NEVER LOCAL — permanently streamed, or dropped

**Permanently streamed pictures (Cinematic only), because no state decomposition exists or is plausible:**

| Element | Why |
|---|---|
| `drawSpecialPanel` | draws the game's own `dMeterSub_c`/`dMeterString_c` display lists straight into the companion ortho (`companion.cpp:1563-1597`). Arbitrary game 2D content |
| Vessel of Light | composites a live pane subtree, then re-maps each of 16 tear panes through the composite's affine and redraws the game's sparkle overlay at those positions (`companion_hud.cpp:636-704`) |
| `dKantera_icon_c` radial oil meter | a live animated game object: `setScale/setPos/setNowGauge/setAlphaRate/drawSelf` (`companion_hud.cpp:99-108`) |
| Rupee counter pane | `drawPaneComposite(md->getCounterPane(0), …)`. **The proof that pane-compositing cannot substitute for state sits next to it**: the key counter uses digits instead *because* every `key_n` pane is hidden at zero and a composite can never render a "0" (`companion_hud.cpp:600-618`) |
| Hearts | `drawHeartsRow` composites the game's live `J2DPicture` heart panes; the art **cannot be loaded by identity** — `drawWantedHeartIcon` probes the 20 live slots for a state currently on screen (`companion_state.cpp:112-160`). Cinematic-only anyway (`companion.cpp:1604-1607`) |
| Midna Z-button portrait | a live pane whose **alpha carries meaning**, plus `drawMidnaPikariAt` |

**Dropped from the wire entirely:** the battery. `drawBattery` reads `s_batteryPct`, fed by `setBatteryStatus` from the Android UI thread **of the machine running the game** (`companion.cpp:673-676`). On a phone client that is either a desktop with no battery or an AYN Thor's battery displayed on a different phone. The phone reads its own; the PC stops sending it.

---

# Part II — The three mechanisms

Everything in Part I rides on exactly three pieces of plumbing. They are built once, in Stage 1.

### M1 — The intent channel (phone → PC)

`dispatch_message()` runs on `client_receive_loop` (`phone_spike_ws.cpp:531-551`), a **network thread**. It must never touch savedata. The correct precedent is already in the file — `request_icon`'s bounded FIFO, whose header states exactly why (`phone_spike_ws.h:104-116`). Add the same pair: `request_intent()` (enqueue only, bounded at 32, drop oldest on overflow, validate `seq` monotonicity) and `take_pending_intent()`.

Drain inside `beginFrameCompanionInput()` (`companion.cpp:1216`, verified called from `f_ap_game.cpp:763`), **whole queue per frame**, positioned:
- **after** the injection gates (`companion.cpp:1225-1240`) so an intent arriving during a menu or cutscene is refused on exactly the condition the touch path uses;
- **before** `sanitizeSlotBindings()` so the intent is applied *and sanitized in the same frame* — the `ack` the phone receives always describes post-sanitize truth, and the phone never sees a state it has to un-believe.

### M2 — The action module (`companion_actions.cpp`)

Every executor the protocol needs is **already geometry-free**. `equipFromCompanion` takes `(btn, slot)` and touches no rect (`companion_touch.cpp:95`). What is fused to the painter is the *hit test and the priority ordering around it*. So the refactor is "add a second caller", not "rewrite the equip logic".

**Tier 1 — linkage change only, behaviour cannot change:** `setEquipMsg`, `tryBowCombo`, `equipFromCompanion`, `plainEquip`, `gearChangeBlocked`, `equipGear`, `touchUseBlocked`, `beginHold`, `releaseHold` — all currently in `companion_touch.cpp`'s anonymous namespace (`:24`), all just need to leave it.

**Tier 2 — verdict logic fused to a hit-test branch, must be split:** `pressItemButtonXY(xy)` from `handleXYTap`'s body (`:315-355` — the whole body is verdict + feedback, zero geometry), `pressItemButtonSlot(which)` from `handleSlotTap` (`:361-398` — keep separate: differs on wolf, borrow, and item source), `applyContextAction()`, `applySetPage`, `applyCollectTab`, `applyReaderOpen`, `applyFloorSelect`, `applyMapReset`, `applyLeftBoxPage`, `applyComboChoice`.

**Tier 3 — never lifted, stays native-only:** `invCellIndexAt`, `dropTargetAt`, the ghost fly-out endpoint search, the floor detent scan, `readerZoomOpenFrom`. The phone reimplements these over its own layout.

`handleTouch` then calls the same functions after its own hit-test. **One implementation, two callers.** Gate the drain behind `DUSK_PHONE_SPIKE` and leave `handleTouch` byte-identical and the native renderer is unchanged by construction.

### M3 — Ownership grant (PC → phone)

```json
phone → PC:  {"type":"hello","width":1270,"height":2416,"proto":2,
              "owns":["chrome","map","inventory","collection","guide"]}
PC → phone:  {"type":"ownership","proto":2,"granted":["chrome","guide"],"streaming":true}
```

**The PC is the authority, not the phone.** The phone may not stop displaying frames because it asked; it stops when the grant says so. That one rule is the entire compatibility and failure-mode story.

**Ownership is per-surface and total.** A surface is rendered by the phone or streamed, never both. The APK already carries the evidence: `HudChromeOverlayView` is `visibility="gone"` with a comment (verified in `activity_companion.xml`) recording that drawing hearts/rupees/keys natively *on top of* the streamed dashboard made the same values appear twice, out of sync. `MapOverlayView` has no such gate and is `match_parent` above `CompanionView` — the moment `state.active` goes true it double-draws **and** swallows every touch on the whole screen (`MapOverlayView.kt:246-262`, verified: `return true` for all events).

Surfaces: `chrome`, `content:map`, `content:inventory`, `content:collection`, `content:guide`, `splash`. Each content surface has exactly one PC dispatch site to gate. `splash` can never be phone-owned; it is published through `presentation.mode`, which finally gives `drawSplash` the owning unit it has never had (`companion.cpp:562-574`, lifecycle scattered across `dualscreen.cpp:1033, :1101-1104, :1216, :1257-1263`).

**Transitions — four rules:** (1) grants change only at the top of `beginFrameCompanionInput`, never inside a draw; (2) overlap in both directions, never a gap — revoking resumes frames *first* then sends `ownership`; granting sends full state *first*, then `ownership`, then stops frames one frame later; (3) the phone cross-fades over ~6 frames, the PC never needs to know; (4) **a layout-mode change revokes everything** and re-grants, because `mainHudRestored()` reshapes the entire surface including the visible tab set, and both documented near-fatal bugs in `drawDashboard` are mode-switch bugs (`companion.cpp:1771-1776`, `:1786-1797`).

**Compatibility.** There is no `proto` field today — `hello` is `{type,width,height}`. Both directions must be absence-tolerant: an old PC sends no `ownership`, so **no `ownership` within 2 s of connect ⇒ assume `granted:[]` and stay on the stream**. A new PC receiving no `owns` replies `granted:[]`, byte-identical to today. `dispatch_message`'s final `else` currently **warns** on unknown types (`phone_spike_ws.cpp:519-521`) — demote to debug so a new APK against an old PC does not spam the log. Reconnect must clear the grant in the same `client_generation()` check that already resets the diff snapshots (`dualscreen.cpp:578-586`), or a reconnecting phone inherits a grant it never asked for and shows nothing.

---

# Part III — Staged migration

Each stage is independently shippable, testable and revertable. Revert for every stage from 2 onward is the same one-word change: remove the surface from `granted`.

### What the user feels, by stage

| Stage | What changes under their finger |
|---|---|
| 0 | nothing (unbreak + coexist) |
| **1** | **taps stop being dropped; input stops stalling during icon fetches and stage transitions; the screen finally dims during loads and cutscenes** |
| 2 | guide scrolling becomes smooth |
| 3 | nothing (art pipeline) |
| **4** | **tab switching is instant; gauges, corner buttons and item-button presses respond under the finger** |
| **5** | **selecting and equipping is instant; the drag ghost tracks the finger** |
| **6** | **map pan and zoom are smooth; floor picker is instant** |
| 7 | collection lists scroll smoothly |

---

## Stage 0 — Unbreak, coexist, instrument (~2 weeks)

None of this is new architecture. All of it is currently shipping-broken-but-unexecuted, or is a prerequisite for C2.

1. **Stop hijacking the aux window.** Remove the unconditional `.hidden = true` under `DUSK_PHONE_SPIKE` (`dualscreen.cpp:143-148`) and skip the destroy/recreate at `dualscreen.cpp:437-472` when a native display is attached or when full ownership is granted. **This is what makes a phone and an AYN Thor coexist at all.**
2. **Fix `MapOverlayView`'s screen-wide touch sink** (`MapOverlayView.kt:246-262` + the `match_parent` stack). It is armed, not dormant: it is visible by default, fully wired, and has never fired only because `state.active` has never been true. The first dungeon a real phone enters bricks the companion's input.
3. **Clear `s_dmapReady` when the page is not MAP.** `dmapUpdate` returns early without clearing it (`companion_dmap.cpp:293-297`), and `gatherMapState` keys purely on it (`companion_map_state.cpp:70-72`) — so inside a dungeon, once you have visited MAP, the phone receives `active:true` on every other page, and with (2) the overlay eats the whole companion for the rest of the dungeon.
4. **Fix the art composites:** `decodeMapIconRgba` forces `dst[3]=255` (`companion_icon_decode.cpp:248-258`) so every chest/key/boss marker would paint a solid background tile onto the floor image; and `ICON_LIGHT_DROP_e` loses its green tint entirely, rendering as a grey blob.
5. **Fix the quantization defects:** heading truncates where u/v round (`dualscreen.cpp:668` vs `:671`); icon `rot` is compared exactly; `map_state` couples structure to motion so every movement frame resends `floor`/`playerFloor`/`baseGen`; icon mirror-rotation misses the flip that the player heading correctly applies twelve lines earlier (`companion_map_state.cpp:99-101` vs `:120`).
6. **Ship `presentation {mode, dim}`.** Fixes defect D1 below.
7. **Stop sending the PC's battery to phone clients.**
8. **Build the parity harness** (§4.3).

**Defect D1, restated because it is a live bug in today's stream:** the dim is applied only to the aux **window's** present blit (`aurora::auxwin::set_dim(companion::currentDim())`, `dualscreen.cpp:1121`), and `drawDashboard` deliberately does not paint it (`companion.cpp:1808-1812`, verified — baking it in froze the fade whenever the capture stopped refreshing). **A phone client today shows a fully-lit dashboard through every load and every cutscene.**

**Verification:** all of it on the current save except (2)(3)(4), which need a dungeon. The harness needs no save at all once fixtures are recorded.
**Revert:** each item independently.

---

## Stage 1 — The three channels, `granted: []` (~3-4 weeks)

Ships M1, M2, M3 and the channelized state (§1.3) with **no surface granted**. Rendering is byte-identical to today.

**Why this is an early, visible responsiveness win despite changing no pixels:** input moves off the painter. Today `handleTouch` runs from `drawDashboard`'s tail (`companion.cpp:1818-1822` — verified: *"Touch runs LAST so it hit-tests against the geometry this frame's draw just published"*), which runs from `endHudCapture` behind **five** paths that skip it (`dualscreen.cpp:1103, :1127, :1134, :1142, :1214`). Companion input therefore already stalls for the entire duration of every icon fetch and every stage transition, and already drops taps. Converting the phone's `touch` messages into intents drained on the game thread fixes both. **Moving input out of the painter fixes the native second screen too — it is not a phone-only feature.**

Also ships: the `hud_state` diff fix (`&& s_lastSentAck == lastAppliedIntentSeq()`), the `str_def`/`str_miss` interning with the residency rule, the six generations (§3.7), and `resync_done` so the phone can distinguish *not yet sent* from *legitimately absent*.

**The string residency rule is not optional.** `archiveText()` gives up after `FETCH_TRIES=8` attempts spaced `RETRY_GAP=20` draws apart and latches the English fallback for the session — the comment records a page opened during a room transition burning all eight attempts inside ~130 ms (`companion_gfx.cpp:681-757`). The letter caches have the same shape (`companion_collect.cpp:1110-1141`). **The PC must never answer a string request with a fallback**: `str_def` carries `resolved:true` only on a genuine non-empty fetch, otherwise `str_miss {sid, retryAfterMs}`.

**Build a debug `force_refuse` verb.** `applyIntent` returns `Refused` with a chosen reason on a debug intent. This exercises the entire reconciliation loop — prediction, ack-with-no-change, rollback animation, deny cue, reason line — **with no save state at all**. Costs about an hour and is the highest-value test affordance in the plan, because on the current save only `anyMenuOpen` is a reachable refusal.

**Verification:** intent queue with zero verbs, prove native unchanged; then navigation verbs, which are the only ones the current save can exercise end to end; then `equip_item` on the one item the save holds; pad verbs last, because they are the ones that can leave a button asserted in the game.
**Revert:** `#if DUSK_PHONE_SPIKE` gate on the drain.

---

## Track T — Transport rewrite (parallel from Stage 1; **must land before Stage 4 begins**)

`DUSK_PHONE_SPIKE` and `DUSK_PHONE_SPIKE_STATE` both default to 0 (verified, `dualscreen.h:27-41`), it is POSIX-only with no Windows support, single-client, and its own header calls it *"throwaway scaffolding to get a real capture→encode→push latency number, not a production networking layer"* (verified, `phone_spike_ws.h:14-17`).

Needed: a real protocol layer, multi-client (a phone **and** an AYN Thor, or two phones), Windows, pairing/discovery, reconnect, backpressure. **Nothing past Stage 2 ships to a user until this lands.** It is placed before Stage 4 because that is the first stage where rework cost exceeds the parallelism saving. Estimate: 3-5 weeks, and it is the least-specified item in this plan because it is the least researched.

---

## Stage 2 — Guide reader local: the rehearsal (~1.5 weeks)

**Chosen first among surfaces deliberately.** Not because the guide is the most-used page — it is not on the user's list — but because it proves grant / cross-fade / local scroll / stream-suppression end to end **at the smallest possible art cost and with no fixture requirement**. It also delivers a real felt win: a picture stream at 66 ms per frame is worst precisely at continuous gestures, and a long walkthrough section is the purest form of that.

The guide's content is **neither Dusklight-authored nor game-extracted**: it is third-party fan-site prose (every document carries `!url https://www.zeldadungeon.net/...`), saved by the user in their own browser and dropped into an import folder (`guide/store.hpp:14-16`). It never touches the disc. Measured on this machine: **10 documents / 104 KB text / 38 sections / 7 KB index, plus 53 images at 1.36 MB** — 3.4 MB, one transfer at connect, keyed on `import_generation()`.

Porting it **deletes PC work per phone client**: the 512×200 wrap buffer, the background decode thread with its epoch guard, the 6 MiB LRU and the per-frame cull loop (`companion_guide.cpp:484-605, :408-414`) all stop running. Android's `BitmapFactory` + an `LruCache` replaces the lot.

**But the guide's *rendering* is not copyright-clean, which is easy to miss.** Its "you are here" marker is the game's Link/wolf portrait (`companion_dmap.cpp:180-182`), its left-column icon is a clctres BTI, and every glyph goes through `mDoExt_getMesgFont()`. The marker is replaced with original art (it is a legend-free "you" marker). The font is the Stage 3 decision.

**One non-obvious notify:** `guideOpen()` is not a read — it unconditionally kicks `dusk::guide::begin_import()`, a folder scan plus blocking image downloads on a worker (`companion_guide.cpp:1003-1022`). A phone opening its own reader must still send `guide_opened` or pages saved mid-session are never picked up.

**Verification:** fully exercisable on the current save. **Not** exercisable: the dungeon branch of `currentPosition` (needs a dungeon save), the 512-line overflow notice (needs a synthetic document — the largest real section is ~235 lines), and the image-ready path — **273 of 326 image nodes have no file on disk** because the source site 403s non-browser fetches (`guide/fetch.hpp:11-15`). Four of the ten documents contain one boilerplate node in total. That is a pre-existing content gap, not port work.

---

## Stage 3 — Art and text pipeline (~2-3 weeks)

Pure prerequisite; no user-visible change. Independently shippable: the PC serves new kinds, the phone caches them, nothing renders from them yet.

**Breaking change first.** `decodeItemIconRgba` seeds the buffer with `COL_BG` and forces alpha to 255 on every pixel (`companion_icon_decode.cpp:192-200, :223-228`), documented as a deliberate wire-compatibility choice. On a streamed frame that was invisible because the whole dashboard shares that background. On a phone-composited grid **each icon becomes a solid background-coloured square sitting on a group-tinted plate**, and `drawItemIconSilhouette`'s drag-ghost halo becomes impossible. **The one art path that currently works must break.** Do it here, versioned under `proto`.

**New fetch kinds** (all CPU-decode, none steal a GPU capture): `digit` (10 HUD digit textures from the Main2D archive — one fetch unlocks rupees, keys, ammo chips and the Poe count locally); `deco` (the 6 `DECO_*` — note they are **INTENSITY textures drawn through `drawTimgTinted` with explicit black/white RGBA pairs**, so the phone needs the tint pipeline, not just bitmaps); `btnface` (X/Y circle + 2×6 tint values); `face` (Link/wolf portrait — **already in PC memory as compiled-in BTIs, no archive mount and no capture, the cheapest fetch on the list**); `warp`; `quiver` (raw archive index `RAWICON_YADUTU1+0/1/2`, not an item number); `clct` (13 collect BTIs); `outfont` (≤70 inline message glyphs — the A/B/X/Y/Z/L/R button glyphs embedded as `0x02` markers in skill and letter prose, each needing a two-colour TEV tint).

**Add `icon_miss {kind, id, reason}`.** There are three different retry models today (icon FIFO, map-base latch, heart want-list) and two of them lose a request permanently on a stage transition — the capture path times out and logs "never completed" (`dualscreen.cpp:861-875`) with no reply at all.

**Dungeon map icon art is only reachable inside a dungeon.** `dmapIconTimg` reads `dComIfGp_getDmapResArchive()`, which is not mounted in the field, and the game calls `removeResourceAll()` on it every time the dungeon map screen closes — the documented cause of an Android crash, and the reason `copyTimgOwned` exists (`companion_pages.cpp:99-128, :296-300`). **Pre-warm all 14 icon kinds on dungeon entry**, or first entry always has a cold-cache window with no icons.

**Then the font decision — see §9.1.** This stage cannot complete without it.

---

## Stage 4 — Chrome local, Functional only (~4-5 weeks) — *first big felt win*

Tab strip, backdrop, window frame, four corner boxes, X/Y buttons, gauges, context tab, left box, place box, progress box.

Ships: the `layout` description (§4.2), the generated constants (§4.1), `gatherChromeState()`, and the phone-side Functional chrome renderer.

**After this stage, a tab tap raises the tab and runs the page transition instantly**, a corner button depresses under the finger, the gauges breathe at the phone's refresh, and the rupee counter rolls locally. The *content* inside the window is still a streamed picture until Stages 5-7.

**Functional only, and say so now rather than discovering it in week five.** Cinematic is not a skin of Functional — it is a different surface. Functional has four corner boxes (transform, slot I, Z, slot II) with X/Y as separate round buttons on the right edge; Cinematic has **no corner boxes, no Z corner and no I/II on screen at all** — X/Y/B/A/Z are a diamond cluster and the transform is a plate above the d-pad. Plus the four undecomposable composites in §1.5. **Two renderers of Functional, permanently. One renderer of Cinematic, permanently streamed.** That halves the drift surface and it is the honest answer.

**The landmines this stage must carry, each with a named owner.** `drawDashboard()` is not a pure painter. Rule: *anything that reads or writes GAME state moves to `beginFrameCompanionInput()`; anything that reads or writes RECT state stays in the painter.*

| Side effect | Line | Disposition |
|---|---|---|
| `mdAlpha->forceCompanionAlpha()` | `companion.cpp:1730` | **move** |
| `loadCollectIcons()` | `:1732` | **move** — one-shot latch, trivial. Note it also loads the six DECO textures **every page** depends on; suppressing the painter without this costs the whole dashboard its chrome |
| `s_equipMsgFrames--` | `:1736` | **move** — but flag it: it is display-rate today, so on a 144 Hz desktop aux window a 120-frame message lasts 0.83 s, not 2 s. Arguably a bugfix; still a native behaviour change |
| page force-reset to `PAGE_MAP` | `:1744-1750` | **move**, emit `nav.forcePage` |
| drop-rect / ctx / Z / left-box / slot / XY rect clears | `:1754-1776` | **stays** — the two documented stale-rect bugs live here |
| Cinematic `s_dmapFloorPickT = 0.0f` unstick | `:1786-1797` | **stays, and gets a phone-side mirror.** Without it the phone reproduces the documented session-killer where every tap is swallowed with a cancel beep for the rest of the session |
| cutscene drag cancel, page-change geometry invalidation, off-MAP pinch clear | `:1827-1839, :1876-1880` | **move** |
| reset-view glide | `:1841-1874` | **stays** — render-rate ease; the phone runs its own |

**Suppressing send is not suppressing draw.** With a native second screen attached, the aux window still needs `drawDashboard` every frame; ownership suppresses **capture/encode/send only** — one condition on `dualscreen.cpp:540-543` (verified), which retires capture, PNG encode and send together because everything downstream is gated on `s_spikeCaptureArmed`. Only in a phone-only session can the draw genuinely be skipped.

**Fixtures:** dungeon (floor tab, key visibility's dungeon branch, dungeon-item box), lantern at low oil, post-shadow-crystal for wolf, Midna for the Z corner, ≥10 hearts on graduated damage — and **a live `dualScreenHudMode` toggle on every page, in and out of a dungeon, with the floor picker open**. That last one is the highest-risk untested path in the whole project, and a phone caching chrome adds a third state machine to it.

---

## Stage 5 — Inventory local (~3-4 weeks) — *"selecting and equipping should be instant"*

Grid, selection, drag, ghost, drop targets, six animations, combo chooser, info panel with wrapped scrolling text.

Sequenced **after** chrome deliberately: the drop targets for X/Y/I/II live on the chrome, so once chrome is phone-owned the entire drag-equip gesture lives inside phone-owned surfaces. Running inventory first would leave the ghost phone-drawn and the drop rings PC-drawn across a one-frame-late boundary.

**The drop-rect trap this sequencing avoids.** `s_dropRect` is cleared at the top of every `drawDashboard` and re-published only while `inEquipMode()` was true *during that frame's draw* — so on the frame a selection is first made the targets do not exist yet. And the two layouts publish **different subsets at different sizes**: Cinematic never publishes I/II at all; Functional publishes at `FN_BTN=76` where Cinematic's cluster uses `CLUSTER_BTN=38`. Shipping the drop rects inside `layout`, keyed to the mode, means the phone inherits the clear-on-mode-change for free instead of reimplementing it and getting it wrong.

**Item descriptions are not cacheable by item number.** `drawItemInfo` picks the X-or-Y button glyph from **live equip state** before fetching (`companion_pages.cpp:1447`) and `getStringFull` substitutes that choice into `MSGTAG_XYBTN` — plus the player name and horse name (`d_meter2_info.cpp:592-611`). The same item's description changes when you re-equip it and differs between save files. Mark them `perSave: true` and key the cache on `(saveGen, itemNo, xyBtn)`.

**Faithful-port oddities to preserve or deliberately change (§9):** a drag-equip never clears the selection while a tap-equip does (`:1219-1221` vs `:1241`); a finger that went down on X/Y and then dragged onto the grid still fires the item on release (`:1108-1111`); the same item can legitimately show a different ammo number in the grid than on the button it is equipped to.

**Fixtures:** post-Goron-Mines with bow + 2 bomb bags + hawkeye + 2 bottles (combos, chooser, ammo); a bomb bag at 1 bomb with bomb arrows equipped (`moveBombNum`'s auto-unequip); fishing rod + bait; a trade item held; Ooccoo in a dungeon; a bottle of hot spring water.

---

## Stage 6 — Map local, dungeon only (~3-4 weeks) — *"interacting with map should smooth"*

Pan, zoom, floor picker, fade, plates, Reset, Ooccoo, Warp buttons.

Sequenced last of the three the user named **only because it is the one that cannot be verified at all today**. `s_dmapAvailable` is false for the entire test save, so literally none of the map path has executed its success case — including `MapOverlayView`, which is fully wired, visible by default and has never rendered. *Fix the fixture before assuming the Kotlin is at fault.*

**Prerequisite inside this stage: a dedicated offscreen capture target.** Today, fetching a floor's base image substitutes the whole dashboard into the **single shared aux capture slot** for several frames (`dualscreen.cpp:1157-1177`), makes the binary frame path yield (`:538-539`, verified), and **visibly re-frames the native second screen** via `setMapBaseCanonicalView(true)`. So until the stream is retired, switching floors makes the phone *more* laggy, not less — and it yanks the AYN Thor's map framing. Pre-fetch every available floor on dungeon entry through a dedicated target, not on every floor tap through the shared one.

**A hard limitation to state rather than paper over:** `mapViewAdjust` (`companion.cpp:1547-1556`) is read **inside `dMap_c::_draw`** (`d_map.cpp:1958-1963`) and even inside `getPlayerCursorSize` to rescale the baked cursor, gated only on `hudOnCompanion()` which is true in both HUD modes. The map camera is global game state, not a view. **A phone and a native AYN Thor cannot have independent map cameras.** Adding one is a render-pass change, not a protocol change. Do not let any plan claim otherwise.

**Sanctioned quality loss:** the PC's zoom is a *real re-render* — `cmPerTexel` is divided by the zoom (`companion_dmap.cpp:380`) so it stays sharp at 4×. The phone magnifies a cached 448-texel base. Instant but visibly softer at high zoom. The two clamps already disagree (PC 1..4, phone 1..6) and must be unified by the generated constants.

**Fixtures:** a multi-floor dungeon **with** map + compass (for `floorAvail = 0x1FFF` and `bossFloor`); the same dungeon **without** the map (for the 2 × 64-room visited scan and the dimmed unselectable rows); Ooccoo Sr. and Jr.; City in the Sky for `ICON_LV8_WARP_e`; a twilight/light-drop state; post-M_021 overworld for the warp button; a boss arena for the 90-frame no-minimap grace path; **and a mirror-mode run, because three of the confirmed defects are mirror-only.**

---

## Stage 7 — Collection local (~2 weeks)

Overview (7 gear boxes, counters, scent, Fused Shadows / Mirror Shards), skills list, mail list, shared reader. **Bugs and fish are dropped, not ported — see §6.**

**Gear equip is the only genuinely hard interaction on the page** and the reason this is not Stage 2: it is geometry-coupled, triple-gated (`anyMenuOpen`, `checkWolf`, three change-wait timers, two of which refuse *silently*), and one path does a full Link-archive reload. It is the one action classified **never predict**.

Two stale comments in `companion_internal.h` would corrupt a protocol written from the docs: `:620-624` says reader rect id `-2` means "back to list" (the code uses **-4**), and `:609-612` documents a tap-to-select/deselect model for `s_collectSel` that **no longer exists** — `s_collectSel` is dead, assigned `-1` at all five write sites, so the two row-highlight reads are unreachable. Do not port it.

**Mail list order is a save-format bug workaround, not a sort.** `sortedLetters` reverse-walks `dComIfGs_getGetNumber` with an explicit range guard because a corrupt order table was writing past five 64-entry caches, and falls back to a raw flag scan for older saves (`companion_collect.cpp:1002-1025`). **Send the ordered id list, not the raw table.**

**Fixtures:** 10+ received letters (the mail reader, its 5 × 64-entry caches and its 8-try residency retry have **never** run); several hidden skills learned (skill rows only publish a tap rect inside `if (got)`, so `drawReaderDetail(tab=3)` has never run either — and with both, the entire 70-glyph inline-icon path is unexercised); `maxLife % 5 ∈ {1,2,3,4}` for the heart-piece wedges (at 3 hearts, `15 % 5 == 0` and the wedge loop runs zero times); all 7 gear entries owned; a PAL-English save with caught fish; a non-English save for the fitted/ellipsized title paths.

---

## Stage 8 — Overworld minimap (optional, ~2-4 weeks) — §9.5

**Cheaper than the brief implies, and the reason is worth stating.** Its framing is **already player-independent**: `dMap_c::_move` recomputes `mCenterX/mCenterZ/cmPerTexel` only when the stay *room* changes (`d_map.cpp:1725-1740`), fitting the map-path extents into the texture. So there is no "following minimap" problem to solve — the canonical-view invention that was the expensive half of the dungeon work is not needed.

The confirmed blocker is that the cursor and icons are **baked into the same render pass** (`renderingAmap_c : renderingPlusDoorAndCursor_c`, whose `afterDrawPath` draws restart cursor, treasure, player cursor and post-player treasure — `d_map_path_dmap.cpp:803-841`). But the suppression hook already exists: `dMap_c::getPlayerCursorSize`/`getRestartCursorSize` are `TARGET_PC` overrides (`d_map.cpp:547-564`), and returning 0 from them is **exactly** how the dungeon renderer suppresses its own cursor (`d_menu_dmap_map.cpp:129-131`). Icons need one more override (`isRendIcon` is inline-true at `d_map.h:124`). **Two virtuals, not a re-architecture.**

**The genuine unknown:** the field icon textures come from `dMpath_n::m_texObjAgg` (`d_map_path_dmap.cpp:1051`), a **different source** from the dungeon BTI archive, with no existing fetch path. Call it +1 week of discovery, and treat the estimate as the least reliable in this document.

---

## Honest total

Roughly **5-7 months** of one engineer for everything through Stage 7, with Track T in parallel. **Verification is the schedule risk, not the code** — seven of the blockers are missing save states, and without them a "done" surface has still never executed its success path, which is exactly how eight completed features got here.

**A defensible minimal version, if the schedule is the constraint:** Stages 0, 1, 3, 4, 5 + Track T. That is chrome and inventory local — the user's "switching tab" and "selecting and equipping" — with map, collection and guide still streamed. ~3-4 months.

---

# Part IV — Keeping two renderers from drifting

## 4.0 The honest framing

**Sharing *logic* across C++ and Kotlin is not realistic here. Sharing *layout and constants as data* is, and that is the answer.**

Three reasons code sharing fails *specifically in this codebase*: text layout is already unshareable (§7.1); the draw primitives are GX/J2D and there is no path by which Kotlin's `Canvas` calls them; and a shared C++ core via NDK would drag in `d_com_inf_game`, `d_meter2_draw`, J2D and the archive layer — i.e. the whole game.

**So: disciplined duplication, with a generated single source of truth for what can be data, a runtime layout description for what cannot, and a numeric harness that makes drift fail the build. In that order.**

Drift is not a risk here; it is **certain, and it has already happened five times before a line of this plan is written**: map zoom clamp 4 vs 6, icon mirror-rotation, icon opacity, the missing LIGHT_DROP tint, arrow art. And twice *inside a single renderer*: `inEquipMode` has one canonical version plus two inline copies (`companion.cpp:788-792` vs `companion_hud.cpp:372-373` vs `companion_functional.cpp:1186-1188`), with a comment recording that this exact drift already caused a chooser that could not resolve (`companion_touch.cpp:501-506`); and the floor row was written out twice with the copies "already drifted apart on chamfer, text size and baseline" (`companion_pages.cpp:863-867`).

## 4.1 Generated constants — build first

The codebase has already made this argument about itself:

> *"These used to be eight ad-hoc literals scattered across six files, so widgets that should have matched — the reader zoom and the collect zoom, the floor picker and the page transition — drifted apart by a few hundredths for no reason."* — `companion_internal.h:784-793`

Make that consolidation the *shared* source: one `companion_layout.def` generating `companion_layout_gen.h` (`constexpr`) and `CompanionLayout.kt` (`const val`).

In scope: `ANIM_RATE_SNAP/FAST/SETTLED/GLIDE`, `ANIM_DECAY_FAST/SOFT`, `ANIM_DONE`, `ANIM_ZERO`; `TAP_HOLD_FRAMES = 24` and its derived `RESTORE_MARGIN`/`RESTORE_CAP`; `DENY_FLASH_FRAMES`; `DMAP_FLOOR_COUNT`, `DMAP_FLOOR_FOLLOW`; the 10px drag slop and the 8px drop grab margin; **the map zoom clamps** (an already-shipped divergence); the 5 inventory group tints and the selection colour; and **`l_invCells` / `l_invCellsWide` + the 1.7 aspect threshold** — the best case in the tree, compile-time constant data with `static_assert`s already proving the two tables agree.

Explicitly **out** of scope: `CORNER_GRACE`. Generate it as a *default* the phone overrides, and document it as an intentional divergence.

## 4.2 A runtime layout description — do not reimplement the tab strip

`drawTabs` states the rule and a Kotlin tab strip is precisely the violation it names:

> *"Nothing else may recompute this geometry: the two layouts show different tab counts, and a second copy of the maths would drift out of sync with the drawn plates."* — `companion_hud.cpp:479-481`

So don't reimplement it — **publish it**. Factor the arithmetic (already game-accessor-free, `companion_hud.cpp:482-557`) into `computeTabGeometry()`; `drawTabs` calls it, and so does a new `gatherLayout()`:

```json
{"type":"layout","mode":"functional","canvas":[800,1200],
 "tabs":[{"page":0,"rect":[x0,y0,x1,y1],"labelSid":41}, ...],
 "content":[x0,y0,x1,y1],
 "corners":{"transform":[...],"slot1":[...],"z":[...],"slot2":[...]},
 "drops":[[...],[...],[...],[...]]}
```

Sent on connect and on layout-mode change; the phone scales to its own canvas. Same shape as the action refactor: **one function, two callers, no second copy.**

Two things fall out free: a re-sent `layout` **corrects a latched English tab-label fallback**, which a bundled table never could; and the drop rects arrive keyed to the mode, so the phone inherits the clear-on-mode-change lifecycle instead of reimplementing it.

## 4.3 A numeric parity harness — the only thing that will hold

Prose "keep them in sync" will not hold; see 4.0.

1. Record a fixture: `{layout, chrome_state, hud_state, map_state, inventory_state}` + a canvas size. JSON, checked in.
2. A C++ test entry point feeds it to the geometry functions — `computeTabGeometry()`, `computeInvGrid()` (`companion_pages.cpp:697-717`), the map projection (`companion_map_state.cpp:79-120`) — and dumps every published rect and every projected icon/player position **as numbers**.
3. The same entry point in Kotlin, as a **JVM unit test**. No device needed.
4. Diff at 0.5px tolerance. **Fail the build.**

This targets geometry and semantics, not pixels — the right target, because pixel parity is already impossible (§7.1) and nobody ever sees both screens at once. It catches every divergence already confirmed on the map page.

**A `DUSK_COMPANION_CAPTURE` screenshot comparison is not worth building first**: it is off by default, has a documented history of being silently compiled away in every build (`dualscreen.h:9-19`), and it would compare things that are legitimately allowed to differ.

**This is also the only part of the whole plan whose success path can execute today**, on the current save, with hand-written fixtures for unreachable states. That is the argument for building it in Stage 0 rather than at the end.

## 4.4 Sanctioned divergences — write them down so nobody "fixes" one

| Divergence | Why it is correct |
|---|---|
| Text metrics, wrapping, fitted sizes | different fonts by necessity. **Send raw text; let each renderer wrap in its own metrics.** What must stay identical is the *semantic* layer — which rows exist, in what order, which are tappable, what each tap means |
| Player arrow art | PC uses the game's BTI; phone uses original geometry. **Keep the original** — copyright-clean, always available, costs nothing |
| Map zoom sharpness | different operations (re-render vs magnify). Match the *clamps*; accept the softness |
| Corner grace margins | device-specific, phone-owned |
| Battery | deliberately unshared — wrong device (§1.5) |

---

# Part V — Art and copyright, as an enforceable rule

**Rule: the APK ships nothing that came off the disc. Not art, not fonts, not audio, not text tables.**

| Class | Disposition |
|---|---|
| `DECO_*` (6), HUD digits (10), item icons, heart art, collect BTIs (13), quiver tiers (3), Link/wolf portraits, warp glyph, X/Y button face, outfont glyphs (≤70), dmap overlay icons (14), the floor base image | **fetched at runtime, cached for the session.** Never bundled |
| Floor names, area names, item names, item descriptions, tab labels, fish/skill/mail text | **fetched as strings**, with the residency rule. The APK must not ship a table of them |
| The message font | **see §9.1 — the open decision** |
| Guide documents and images | the user's own imported files; a plain file copy, no decode, no archive, no disc |
| Bevelled corner plate, day/night glyph, d-pad glyph, gauge bar, carousel dots, every chamfer/ring/disc/gradient, `companion_logo.inc`, the `STR_*` table, the UI click sounds, the player arrow | **original, authored for Dusklight — the APK may ship these** |

Two consequences worth calling out: the PC repo itself *does* embed three game BTIs (`companion_dmap_icons.inc` says so in its own header, used at `companion_dmap.cpp:180-210`), so "dusklight ships zero Nintendo assets" is the APK-side policy, not a statement about the PC binary. Practically this is good news — those three need no archive mount and no capture, so serving them is the cheapest fetch on the list.

And: **recommend the APK ship *original* plates and frames rather than fetching the six `DECO_*` textures.** Both `drawMenuBox` and `drawTabPlate` already have `fillRect` fallbacks for when the archive is absent, so this is a supported path, and it removes the single largest source of pixel-drift pressure between the two renderers.

---

# Part VI — What gets DROPPED rather than ported

| Dropped | Why |
|---|---|
| **The entire Cinematic layout** | Four elements with no state decomposition and no plausible one (§1.5). Permanently streamed. Halves the drift surface |
| **The Bugs page** | `drawCollectBugs` paints a header plus 24 fixed icons and **publishes no rect at all** (`companion_collect.cpp:340-360`); `drawSectionHeader` is called with `detail=false` so its Back branch never runs. Zero interactions — a streamed still is behaviourally identical. Struck from the port list permanently, not deprioritised |
| **The Fish page** | Same: a 3-column static table, no rect, no scroll var. Confirmed from the other side — the drag handler's `scrollVar` selection covers only collect tabs 3 and 4 |
| **The battery readout** | Wrong device (§1.5). The phone reads its own |
| **Per-viewer map cameras** | Impossible without a render-pass change (§Stage 6). A phone and an AYN Thor share one camera |
| **Pixel parity with the C++ renderer** | Abandoned as a goal (§4.4). Semantic parity is enforced instead |
| **Heart art fetched by identity** | Not possible — `drawWantedHeartIcon` opportunistically probes 20 live slots for a state currently on screen and returns false otherwise. Hearts stay Cinematic-only and stay streamed |
| **`s_collectSel`** | Dead code. Five write sites, all `-1`. The header still documents an interaction that does not exist |
| **`s_dropRect`'s publish/clear lifecycle on the phone** | Replaced by the `layout` message rather than reimplemented (§4.2) |
| **Item descriptions cached by itemNo** | Not cacheable — live equip state and the save file both change the text |
| **`/root/dualscreen-companion/README.md` as the protocol contract** | Stale to the point of being wrong: no `sound`, no `haptic`, no `map_state`, no `map_base`, no `map_icon`; `:193` says map state lands "in later phases"; `:38` says pinch is unimplemented while `CompanionView.kt` implements it and the client sends it. **Rewriting it is part of this work, not adjacent to it** — a client written from the current README cannot talk to the current server |

---

# Part VII — The hard parts, not understated

## 7.1 Fonts and text — the largest un-flagged asset dependency

Every string on the dashboard renders through `mDoExt_getMesgFont()`, and `measureText` sums **per-glyph ink widths** from `font->getWidthEntry` (`companion_gfx.cpp:572-588, :623-641`). `companion_strings.h:7-10` states outright that the companion's *own* strings are written as LATIN-1 `\xNN` escapes **because of that font**.

So `fittedTextSize`, `drawTextFittedCentered` and `fitPrefix` all depend on copyrighted font metrics. A phone that lays out with a bundled font will not merely look different — **its fitted text sizes will differ**, which is precisely the drift the two-renderer constraint exists to prevent. The code's own German-overrun comment (`companion_collect.cpp:494-499`) is the proof that these decisions are metric-sensitive.

There is no cheap answer. There are two answers and they must be chosen between (§9.1).

Compounding it: **inline message icons.** Item descriptions, skill text and letter bodies are not plain text — `getStringFull` emits `0x02` + outfont index escapes for button glyphs, and `drawBodyText` resolves each to a tinted BTI (`d_meter2_info.cpp:602-611`, `companion_collect.cpp:601-633`). Without those ≤70 glyphs fetched and tinted, skill and letter prose renders with holes. And `wrapBody` (collect) and `wrapNode` (guide) are both quirky and bug-fixed over time — sentence-end-only newline rules, bullet hanging indents, a swallowed-indentation guard, LATIN-1 per-byte measurement. **Do not reimplement them for parity; reimplement them for the phone's own font, and accept the divergence.**

## 7.2 The overworld minimap

Cheaper than it looks (§Stage 8) but genuinely optional, and it carries the one art source with no existing fetch path at all. Do not commit to it until Stage 6 has actually run in a dungeon.

## 7.3 The guide page

Cheapest to port and most misread. Its **content** is third-party fan content the user imported themselves — the safest thing to transfer in the project. Its **rendering** is not clean: game portrait, clctres BTI, game font. And 273 of its 326 image nodes have no file on disk because the source site refuses non-browser fetches — a pre-existing content gap that will look like a port bug.

## 7.4 The transport is a spike

Compile-disabled by default (verified), POSIX-only with no Windows support, single-client, and self-described as throwaway scaffolding (verified). **Nothing past Stage 2 ships to a user until Track T lands.** Sections I-IV all ride on it; only the parity harness (§4.3) does not, which is the second reason to build that first.

## 7.5 Eight finished features have never run

This is the project's actual failure mode, and it will recur unless the fixtures land. The map renderer, the vessel, the wolf chrome, the lantern path, the oxygen path, the mail reader, the skills reader and the dungeon half of the chrome have all been *completed* and never *executed*. **Treat the first end-to-end dungeon test as a discovery event, not a validation step.**

---

# Part VIII — Verification and fixtures

## 8.1 What is provable today, on the 3-hearts / no-lantern / no-dungeon save

Stage 0's items 1, 5, 6, 7, 8. Stage 1's navigation verbs, `equip_item` on the one held item, `button_down/up`, the `force_refuse` loop, grant/revoke overlap, the 2 s no-reply fallback. Stage 2 entirely except the dungeon here-marker. The parity harness in full. Splash↔dashboard transitions, `mode:"held"` on any stage change, `dim` on any load (which will immediately demonstrate the bug it fixes), and a live layout-mode toggle on chrome / inventory / collection.

## 8.2 The fixture list

| # | Save | Unblocks |
|---|---|---|
| F1 | **Multi-floor dungeon, map + compass + boss key, ≥2 visited floors** | the entire map surface; floor picker modality and its mode-switch unstick; `floorAvail`/`bossFloor`; `baseGen` churn; the icon split; the dungeon chrome; the guide's dungeon branch |
| F2 | Same dungeon **without** the map item | the 2 × 64-room visited scan; dimmed unselectable rows |
| F3 | Post-Goron-Mines: bow + 2 bomb bags + hawkeye + 2 bottles | combo arm/off, the chooser resolving **locally on the phone**, all ammo readouts, `mixIndex` |
| F4 | Bomb bag at 1 bomb with bomb arrows equipped | `moveBombNum`'s auto-unequip — the "inventory changes without the player touching anything" case |
| F5 | Lantern equipped, partly drained; and one below 15% oil | oil gauge, empty-lantern substitution, `tickGaugeWarning` |
| F6 | Anything that can go underwater | oxygen, its 25% blue→red flip, its priority over oil, and the 10 Hz case that motivates R1 |
| F7 | ≥10 hearts sitting on **graduated** damage; and one above 10 | heart states 0-3, `drawHeartsRow`'s two-row branch |
| F8 | `maxLife % 5 ∈ {1,2,3,4}` | heart-piece wedges |
| F9 | Post-shadow-crystal (M_077) | wolf chrome, scent readout, inert slot taps, `transform` |
| F10 | Midna available | Z corner, `z_press`, the portrait alpha |
| F11 | Post-M_021 on an overworld stage | `CTX_WARP`, `warpAllowed` |
| F12 | Ooccoo in SLOT_18 (Sr. **and** Jr.), inside a dungeon | quick-use choreography; **the silent borrow refusal — the one case with no cue** |
| F13 | All 7 gear entries owned | `equipGear`'s shield and clothes archive-reload branches and their wait timers |
| F14 | A trade item held (mid Malo/Ilia) | `STR_NO_SLOT` refusal path |
| F15 | Mid-tears-quest in a twilight region | Vessel of Light, the left-box forced page jump |
| F16 | 10+ received letters | the mail reader and its caches (never run) |
| F17 | Several hidden skills learned | the skills reader (never run) — and with F16, the 70-glyph inline-icon path |
| F18 | Fishing rod + bee/worm/jewel bait | rod combos, which `getSelectItem` resolves but the companion never authored a path for |
| F19 | A second save file | `saveGen` and the count-keyed letter cache that "happily shows file A's subjects for file B" |
| F20 | PAL-English with caught fish; plus a non-English save | the inches branch; fitted/ellipsized titles |
| F21 | A boss arena | the 90-frame no-minimap grace path |
| F22 | A mirror-mode run | three confirmed defects are mirror-only |
| F23 | **A second physical display attached alongside a phone** | **C2 itself.** Nothing else proves the two coexist |
| F24 | A browser-saved guide page **with its `_files` folder** | the guide's image-ready path |
| F25 | A synthetic >512-line guide section | the overflow notice |

**F1 and F23 are the two that gate the most work.** F1 unblocks Stages 4 and 6 and four of the eight never-run features. F23 is the only proof of the constraint the whole project is built around. Get those two first.

## 8.3 Per-stage gates

No stage is "done" until: (a) its parity harness diff passes; (b) every verb it adds has executed its success path **and at least one refusal path** on a real device; (c) the native second screen has been verified unchanged with a phone connected and disconnected; (d) a layout-mode toggle mid-session has been performed on every page the stage touches.

---

# Part IX — Decisions needed from the user

These are choices, not unknowns. Each changes the plan materially.

**9.1 — The font. The single biggest open design question.**
- **(a) Ship an original font (or use the Android system font) and accept that phone text layout differs from the C++ renderer's.** Cost: ~0.5 days plus re-encoding the `STR_*` table from LATIN-1 to UTF-8. Zero bundled game assets. Permanent visual divergence in every fitted, centred, ellipsized and wrapped string.
- **(b) Stream a glyph atlas + width table.** Cost: ~3-5 days plus per-session bandwidth and a new fetch kind. Near-parity.
- **Recommendation: (a).** Nobody ever sees both screens at once; §4.4 already sanctions the divergence; and (b) buys parity in the one dimension where parity has no user-visible value.

**9.2 — Confirm Cinematic is never de-streamed.** The plan assumes yes. If Cinematic must go local too, add roughly 6-10 weeks and accept that four elements (special panel, vessel, kantera meter, counter pane) will have to be streamed as sub-pictures inside a locally-rendered surface — which reintroduces the double-draw class of bug on purpose.

**9.3 — Guide content transfer.** 3.4 MB of third-party fan-site content copied from the user's machine to the user's phone. Same posture the project already takes for game art (user's own files, user's own devices), but it is content neither Nintendo's nor Dusklight's, so confirm.

**9.4 — Confirm Bugs and Fish are dropped permanently** (§6). They are behaviourally identical as streamed stills.

**9.5 — Is the overworld minimap in scope at all?** It is Stage 8, optional, and carries the one genuinely unknown art source.

**9.6 — Multi-client.** Does Track T need a phone **and** an AYN Thor connected simultaneously? Two phones? This is the biggest single variable in the transport estimate.

**9.7 — Faithful port vs deliberate fix, three cases.** Each is a behaviour change smuggled in by a port unless decided:
- The Ooccoo-borrowed-slot drop is refused **completely silently** — no message, no sound, no haptic (`companion_touch.cpp:116-118`). Add a cue, or stay faithful?
- Item descriptions always render the **X** glyph even for an item bound to slot I/II, because the test reads only button index 1 (`companion_pages.cpp:1447`). Arguably a bug. Reproduce it or fix it?
- Bugs and Fish **swallow vertical drags silently** (no scroll var). On a phone that reads as broken. Leave, or make them inert-with-feedback?

**9.8 — `s_equipMsgFrames` moving from display rate to game rate** changes native behaviour: a 120-frame message currently lasts 0.83 s on a 144 Hz aux window and would become 2 s. Arguably a bugfix. Accept?

**9.9 — Map zoom: instant-but-softer, or sharp-but-latent?** The plan assumes instant. An alternative is instant magnify plus a PC re-render request at zoom detents, which restores sharpness after ~100 ms. Costs a verb and a capture cycle.

**9.10 — Which fixtures can actually be produced, and how?** Save editor, played-through saves, or imported ones. This is the schedule's critical path, not the code.

**9.11 — Keep a permanent "stream everything" fallback mode?** Recommend **yes**: it is already the `granted:[]` path, it costs nothing to keep, and it is the recovery mode for every failure in Part III.