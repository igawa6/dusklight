# Companion dashboard — visual design spec

The definitive description of how the in-process C++ companion dashboard
(`src/dusk/companion*.cpp`) actually looks, written so the phone's Kotlin views can
**match** it rather than approximate it.

Every number here is copied from the source, with a `file:line` so it can be re-checked
when the C++ moves. Where a value is a formula in the code, the formula is given, not a
sample of it.

Scope note: this document describes the **renderer as it exists today**. It does not
propose layout changes. Two places where the phone *cannot* currently match without new
plumbing are called out explicitly, with a recommendation: game-art plates (§13) and text
(§12).

---

## Table of contents

1. [Coordinate model and how to read this document](#1-coordinate-model)
2. [Canvas, layouts, and which one is on screen](#2-canvas-and-layout-selection)
3. [Colour palette — every constant](#3-colour-palette)
4. [Drawing primitives — exact construction](#4-drawing-primitives)
5. [Backdrop](#5-backdrop)
6. [Tab strip](#6-tab-strip)
7. [Content window: frame, ornaments, insets, page transition](#7-content-window)
8. [Context tab](#8-context-tab)
9. [Corner boxes and equip buttons](#9-corner-boxes-and-equip-buttons)
10. [Pages](#10-pages) — [inventory](#101-inventory-items), [map](#102-map), [collection](#103-collection), [guide](#104-guide)
11. [Animation table](#11-animation-table)
12. [Text](#12-text) ← the hard one; read the recommendation
13. [Art inventory: what is procedural, what is game art](#13-art-inventory)
14. [What could not be pinned down](#14-what-could-not-be-pinned-down)

---

## 1. Coordinate model

* **Units.** Everything in the C++ is in *logical canvas units*, `0..s_canvasW` x
  `0..s_canvasH`, published every frame by `drawDashboard`
  (`companion.cpp:1766`, sets `s_canvasW` / `s_canvasH` at the top). The physical
  texture is `s_nativeW` x `s_nativeH` with `s_pixelScale` converting logical → pixels
  (`companion_internal.h`, "Native offscreen canvas"). The phone should treat the logical
  unit as its design unit and scale once, at the root — **not** re-tune individual numbers.
* **Y grows downward.** `x0,y0` is always top-left, `x1,y1` bottom-right. Rect params are
  consistently `(x0, y0, x1, y1)` except `drawTabPlate`/`drawMenuBox` variants noted inline.
* **Text `y` is a BASELINE**, not a top. See §12. This is the single most common way a
  port drifts: every `+ 5.0f` you see next to a row centre is a baseline nudge, e.g.
  `ry + rowH * 0.5f + 5.0f` (`companion_collect.cpp:1152`).
* **Alpha.** A module-global `s_drawAlpha` (0..1, `companion_gfx.cpp:31`) multiplies into
  every primitive's alpha. It is how whole blocks fade without being painted over. Ports
  need the same global: several transitions (page slide, reader zoom, collect zoom) rely on
  *multiplying* it, never assigning (`companion_collect.cpp:920` comment).

---

## 2. Canvas and layout selection

There are two layouts. `dusk::dualscreen::mainHudRestored()` selects
(`companion.cpp:1827`):

| | Cinematic (`mainHudRestored() == false`) | Functional (`== true`) |
|---|---|---|
| Entry point | `drawDashboardCinematic` `companion.cpp:1649` | `drawDashboardFunctional` `companion.cpp:1707` |
| Meaning | the whole status HUD lives on this screen | gameplay HUD is back on the main screen; this is a control surface |
| Tabs | 4: MAP, ITEMS, COLLECTION, GUIDE | 3: **COLLECTION, MAP, ITEMS** (map centred, GUIDE dropped) |
| Tab strip dress | floating plates on a scrim | a bed that continues the content window |
| Content window | `drawContentWindow(12, w-158, HEARTS_H+8, h-TABS_H-6)` | `drawContentWindow(sideInset, w-sideInset, FN_TOPBAR_H+4, h-TABS_H-4)` |
| Context action | in-window floating button | left column's middle zone |
| Corner boxes | none (controller diamond down the right instead) | four, one per screen corner |

Tab order is `visiblePages()` (`companion.cpp:667`) and it is **not** page-enum order in
Functional. The phone gets this list in `chrome_state` (`companion_state.h`, `TabState`);
`companion_state.cpp:145` sets `out.functional`. Do not re-derive it.

Functional's `sideInset = FN_CORNER_MARGIN + FN_CORNER + FN_GAP` = `6 + 84 + 6` = **96**
(`companion.cpp:1716`).

### Fixed layout constants (`companion_internal.h`)

| Constant | Value | Line | Meaning |
|---|---|---|---|
| `HEARTS_H` | 26 | 241 | Cinematic top bar height (bar is drawn `HEARTS_H + 6` tall) |
| `TABS_H` | 44 | 242 | tab strip height, both layouts |
| `FN_TOPBAR_H` | 28 | 250 | Functional status strip height |
| `FN_BTN` | 76 | 251 | X / Y circular button diameter |
| `FN_CORNER` | 84 | 254 | corner box side (square) |
| `FN_GAP` | 6 | 257 | the one padding value: edge→box→window |
| `FN_CORNER_MARGIN` | 6 | 258 | `= FN_GAP` |
| `FN_CORNER_CHAMFER` | 14 | 259 | corner-box chamfer used by the equip frame |
| `FN_CORNER_LABEL` | 12 | 262 | corner label text size |
| `FN_LABEL_DX` / `FN_LABEL_DY` | 8 / 7 | 263–264 | label inset from box top-left |
| `FN_COL_W` | 96 | 267 | `FN_CORNER + 2*FN_CORNER_MARGIN` |
| `FN_XY_STAGGER` | 8 | 272 | `FN_CORNER - FN_BTN`; Y sits this far left of X |
| `FN_LEFT_GAP` | 8 | 277 | between the left column's three zones |
| `FN_RUPEE_H` / `FN_CTXTAB_H` / `FN_DUNGEON_H` | 44 / 44 / 104 | 278–280 | left column zone heights |
| `CLUSTER_BTN` | 38 | 564 | Cinematic diamond button size |
| `DROP_GRAB` | 8 | 354 | drop-target grab margin (highlight **must** use the same value) |
| `DENY_FLASH_FRAMES` | 14 | 420 | rejected-tap flash length, in **game** frames |
| `TAP_HOLD_FRAMES` | 24 | 411 | minimum synthetic button hold, game frames |

---

## 3. Colour palette

### 3.1 Core palette — `companion_internal.h:115–129`

| Name | RGBA | Hex | Line | Use |
|---|---|---|---|---|
| `COL_BG` | 17, 17, 16, 255 | `#111110FF` | 115 | backdrop base fill |
| `COL_TAB` | 38, 36, 32, 215 | `#262420D7` | 116 | inactive tab plate **fallback only** |
| `COL_TAB_ACTIVE` | 214, 206, 186, 255 | `#D6CEBAFF` | 117 | active tab plate **fallback only** |
| `COL_WINDOW` | 24, 24, 22, 248 | `#181816F8` | 121 | content window fill, tab bed, guide panel, detail boxes |
| `COL_FRAME` | 108, 102, 90, 255 | `#6C665AFF` | 122 | frame lines, guide rules, ornament fallback |
| `COL_SIDE_RULE` | 74, 68, 58, 235 | `#4A443AEB` | 125 | window side rules; the tab bed repeats it so the two read as one line |
| `TEXT_MAIN` | `0xF0E8D0FF` | parchment | 126 | body text |
| `TEXT_DIM` | `0xA69C82FF` | | 127 | secondary / disabled text |
| `TEXT_ACCENT` | `0xE9CE8EFF` | gold | 128 | headings, values, guide headings |
| `TEXT_TAB_ACTIVE` | `0x2E2415FF` | dark ink | 129 | text on a light (selected) plate |

`COL_TAB` / `COL_TAB_ACTIVE` are **only** reached when the game plate texture is missing
(`drawTabPlate` `companion_gfx.cpp:1110`, `drawChamferPlate` `companion_gfx.cpp:430`).
A phone with no plate art fetched is in exactly that fallback state — which is a large part
of why the native render currently does not look like the PC's.

### 3.2 Bevel palette — `companion_gfx.cpp:315–331`

| Name | RGBA | Line |
|---|---|---|
| `BEVEL_RIM_GOLD` | 198, 166, 104, 255 | 315 |
| `BEVEL_RIM_GREY` | 104, 99, 88, 255 | 316 |
| `BEVEL_FACE_INSET` | 2.5 (a length, not a colour) | 318 |
| face top, enabled | 80, 75, 65, 255 | 363 |
| face bottom, enabled | 37, 35, 30, 255 | 364 |
| face top, disabled | 52, 50, 46, 255 | 363 |
| face bottom, disabled | 31, 30, 27, 255 | 364 |
| seat (under the rim) | 0, 0, 0, 150 | 353 |
| inner highlight | 112, 105, 92, 130 | 337 |
| inner shade | 0, 0, 0, 90 | 338 |

### 3.3 Everything else, by site

| Colour | RGBA / hex | Where | Line |
|---|---|---|---|
| `COL_SCRIM` | 11, 10, 8, 170 | Cinematic top bar, Cinematic tab-strip bed, Functional top bar | `companion_hud.cpp:613`, `companion.cpp:1675`, `companion_functional.cpp:1034` |
| top-bar separator tint | white point `0xA89C74FF`, black `0x00000000`, alpha 150 | `LINE2` under both top bars | `companion_hud.cpp:620`, `companion_functional.cpp:1036` |
| backdrop block tint | black `0x1B1B1AFF`, white `0x605D57FF` | tiled stone backdrop | `companion_hud.cpp:606` |
| window rule `LIT` | `0x9A8F79FF` | outer top + bottom rule | `companion_hud.cpp:740` |
| window rule `LIT_IN` | `0x6E6555FF` | inner top rule only | `companion_hud.cpp:741` |
| ornament `ORN` | `0xB6A886FF` | corner flourishes | `companion_hud.cpp:742`, `companion_hud.cpp:504` |
| window backing tint | black `0x0C0B0AFF`, white `0x1F1D19FF`, alpha 160 | `YAKUSHIMA` mottle inside the window | `companion_hud.cpp:791` |
| tab plate, selected | black `0x726C60FF`, white `0xF4EEDEFF`, alpha 255 | `drawChamferPlate` / `drawTabPlate` | `companion_gfx.cpp:446`, `:1119` |
| tab plate, unselected | black `0x1A1814FF`, white `0x5A5448FF`, alpha 230 | same | `companion_gfx.cpp:444–446`, `:1121` |
| `COL_CELL_SEL` | 236, 208, 84, 255 | selected / dragged inventory cell, worn gear (`0xECD054FF`) | `companion_pages.cpp:742`, `companion_collect.cpp:100` |
| group tints (5) | see §10.1 | inventory cell plate tint | `companion_pages.cpp:690` |
| `CELL_RGBA` | `0x22201DFF` | every collection overview cell, list rows | `companion_collect.cpp:26` |
| collect row selected | `0x5A4A2AFF` | skills / mail selected row | `companion_collect.cpp:974`, `:1141` |
| ammo chip | 10, 9, 7, 210 | inventory cell chip, Functional button chip | `companion_pages.cpp:763`, `companion_functional.cpp:956` |
| bar background | 10, 9, 7, 210 | Fused Shadows meter track | `companion_collect.cpp:294` |
| bar fill | 235, 200, 90, 255 | Fused Shadows meter | `companion_collect.cpp:297` |
| `TEXT_WARN` | `0xF0A050FF` | equip-refused caption (items / collect / map) | `companion_pages.cpp:786`, `companion_collect.cpp:327`, `companion_pages.cpp:1431` |
| X ring | 110, 145, 210, 255 | equip drop ring on X | `companion_functional.cpp:1184` |
| Y ring | 105, 190, 130, 255 | equip drop ring on Y | `companion_functional.cpp:1184` |
| ring hot | 235, 200, 90, 255 | ring under the dragged finger | `companion_functional.cpp:1185` |
| deny flash | 225, 62, 50, α | fading rejected-tap ring/frame | `companion_functional.cpp:1198`, `:892` |
| slot drop frame | 110, 145, 210, 235 | I / II box equip frame | `companion_functional.cpp:815` |
| slot drop hot | 235, 200, 90, 255 | same, under the finger | `companion_functional.cpp:814` |
| detail box frame | 90, 84, 74, 210 | `drawDetailBox` | `companion_gfx.cpp:389` |
| scroll knob | 166, 156, 130, 120 | every list's scroll hint | `companion_gfx.cpp:548` |
| `drawMenuBox` fallback frame | 110, 104, 92, 190 | only when the plate texture is missing | `companion_gfx.cpp:1107` |
| map name plate | 20, 18, 15, 150 | map top-left banner | `companion_pages.cpp:1382` |
| guide `COL_IMG_PENDING` | 96, 91, 80, 190 | outline of a decoding image | `companion_guide.cpp:436` |
| floor-change veil | 0, 0, 0, `230 * fade` | dungeon floor change | `companion_pages.cpp:419` |
| wolf action text | `0xFFFFFFFF` on `0x000000C0` 8-way outline | "Sense" / "Dig" on X/Y | `companion_functional.cpp:1169–1175` |

---

## 4. Drawing primitives

These are the shapes everything else is built from. Reproduce these first; the pages then
fall out.

### 4.1 Chamfered rectangle — `chamferOutline` `companion_gfx.cpp:226`

An octagon: each selected corner is replaced by a 45° cut of size `ch`.
`cornerMask` bits: **1 = TL, 2 = TR, 4 = BR, 8 = BL**, `0xF` = all.
Walk clockwise from top-left; a cut corner emits two points (`x0, y0+ch`) and
(`x0+ch, y0`), an uncut one emits the corner itself. Filled as a triangle fan
(`fillChamferRect`, `:257`).

### 4.2 Chamfered vertical gradient — `fillChamferVGrad` `companion_gfx.cpp:279`

Same outline, per-vertex colour, `t = (vy - y0) / (y1 - y0)` clamped to 0..1, linear in
each of R/G/B/A. On Android this is a `LinearGradient` shader over the same `Path`.

### 4.3 Chamfered frame — `drawChamferFrame` `companion_gfx.cpp:553`

Outer outline at `(x0,y0,x1,y1,ch)`; inner outline at `(x0+t, y0+t, x1-t, y1-t)` with
`chIn = max(0, ch - t * 0.414)` — the 0.414 keeps the diagonals parallel at thickness `t`.
Each edge is a quad between corresponding outer/inner points. **Not** a stroked path:
stroking a chamfered path gives mitred corners, not parallel diagonals.

### 4.4 Beveled corner button — `drawBeveledCornerButton` `companion_gfx.cpp:349`

This is the "chamfered plate, gradient, gold rim" construction the corner boxes and the
Functional tabs use. Fully procedural — **no game art** — so the phone can match it exactly.

```
ch = min(14, 0.16 * min(w, h))                      // bevelChamfer, :320
r  = 2.5                                            // BEVEL_FACE_INSET
fch = max(0, ch - r * 0.6)

1. seat   fillChamferRect(x0-1, y0-1, x1+1, y1+1, ch+1, rgba(0,0,0,150), M)
2. rim    fillChamferRect(x0, y0, x1, y1, ch, enabled ? GOLD : GREY, M)
3. face   fillChamferVGrad(x0+r, y0+r, x1-r, y1-r, fch,
                           top = enabled ? (80,75,65,255) : (52,50,46,255),
                           bot = enabled ? (37,35,30,255) : (31,30,27,255), M)
4. if enabled, bevelInnerFrames (:330):
     drawChamferFrame(x0+r,   y0+r,   x1-r-1, y1-r-1, fch, 1.2, (112,105,92,130), M)
     drawChamferFrame(x0+r+1, y0+r+1, x1-r,   y1-r,   fch, 1.2, (0,0,0,90),      M)
```

Note the rim is a **solid plate under the face**, not a ring — a ring left hairline seams
against the face's diagonal (`:355` comment). Step 4's two frames are offset by 1px in
opposite directions: that 1px is the entire bevel.

`drawBeveledBorder` (`:374`) is steps 2+4 only with thickness `BEVEL_FACE_INSET`, for
wrapping a caller-drawn fill (the Functional tab plates).

### 4.5 Tint formula for intensity textures — `drawTimgTinted` `companion_gfx.cpp:1003`

Every piece of menu art in the dashboard is an **intensity** texture drawn through a
black/white pair. The TEV (`J2DPicture.cpp:745–751`) computes, per channel *including
alpha*:

```
out   = black + (white - black) * texel        // texel in 0..1, per channel
out.a = black.a + (white.a - black.a) * texel.a
out.a *= pictureAlpha / 255                    // the `alpha` argument
```

So on the phone: decode the texture to a single-channel intensity image, then
`lerp(black, white, intensity)` per pixel and multiply alpha. A `ColorMatrixColorFilter`
or a two-stop `LinearGradient` shader both express this. **An alpha-0 black point is how
the code makes an intensity texture's surround transparent** — e.g. `drawMenuBox`
(`companion_gfx.cpp:1046`) sets `black = fill - 6 with alpha 0`, `white = fill + 82 with
alpha 255`.

### 4.6 `drawMenuBox` — the item/gear cell plate — `companion_gfx.cpp:1025`

Used by every inventory cell, every collection cell, every skills/mail row.

1. **Interior fill first** (the plate's inside is as dark as its outside, so a tint alone
   cannot colour it): `fillChamferRect(x0+1, y0+1, x1-1, y1-1, ch, fill, 0xF)` with
   `ch = min(7, 0.12 * min(w,h))` — a *fixed pixel cap*, so wide counter cells match square
   ones (`:1061`).
2. **Plate on top**, tinted: `black = lift(fill, -6, α=0)`, `white = lift(fill, +82, α=255)`,
   picture alpha = `fill.a`, where `lift` adds to each of R/G/B and clamps (`:1030`).
3. **Three-slice if wide.** `natW = h * texW / texH`. If `w > natW + 2`:
   left cap `natW * 0.45` wide, right cap the same, middle drawn at
   `bigW = midW / 0.10` centred — i.e. magnified 10x so only the texture's centre 10%
   column covers the middle. Each slice is scissored to its own rect (`:1072–1088`).
   Stretching the whole plate instead drags the corner curve into the middle and steps the
   edge at both seams.
4. Fallback with no plate texture: chamfered fill, `ch = clamp(0.18*shortSide, 4, 9)`, plus
   a `drawChamferFrame(..., 1.5, (110,104,92,190))`.

### 4.7 `drawChamferPlate` — plate art inside a chamfered silhouette — `companion_gfx.cpp:430`

Used by the Functional tab plates, the context tab and the Functional corner-box family.
The plate texture has a baked-in frame, so:

* the quad is drawn **over-scaled by `SCALE = 1.7`** and centred on the box, pushing that
  baked frame outside; the box interior samples only centre grain (`:444–456`);
* the *same* quad is drawn repeatedly, only the **scissor** changes, so the plate is
  continuous across the whole shape;
* the octagon is covered as: one full-width middle band `y0+ch .. y1-ch`, plus `(int)ch`
  one-row strips at each chamfered end whose width follows the diagonal
  (`inset = ch - i`, applied only on the sides whose corner bit is set);
* **fast path**: if neither the top pair nor the bottom pair of corners is cut, one single
  strip covers the box (`:462–474`). This matters — an open floor picker was costing 325
  textured draws per frame before it existed.
* alpha / tint: selected `255, 0x726C60FF → 0xF4EEDEFF`; unselected `230,
  0x1A1814FF → 0x5A5448FF`.
* After it finishes it calls `applyWinClip()` — restore to the **window clip**, never to
  full screen (`companion_gfx.cpp:181`); a full-screen restore mid-page lets content paint
  over the chrome during the page slide.

### 4.8 Others

| Primitive | Construction | Line |
|---|---|---|
| `fillDisc` | 20-gon triangle fan | `companion_gfx.cpp:402` |
| `drawRing` | 32-segment annulus of quads, `inner = radius - thickness` | `:508` |
| `drawDetailBox` | `COL_WINDOW` fill + 1px `(90,84,74,210)` frame on all four edges. Deliberately **not** `drawMenuBox` — the cell plate stretched over a big reader box reads wrong (user feedback, `:386`) | `:386` |
| `drawListScrollHint` | knob at `x1-5 .. x1-2`, track `y0+4 .. y1-4`, `knobH = track * viewH / contentH`, `knobY = y0 + 4 + (track - knobH) * scroll/maxScroll`, colour `(166,156,130,120)`. Nothing drawn when `maxScroll <= 0` | `:541` |
| `clampListScroll` | clamps to `[0, contentH - viewH]`, returns the max. **Call it before placing rows**, not after — clamping afterwards draws one frame of overscroll every frame the finger pulls, which reads as the list shaking (`companion_guide.cpp:1143` comment) | `:531` |
| `drawTabPlate(x, y, w, h, selected)` | flat (unchamfered) plate draw, same tints as §4.7. Takes **w/h**, not x1/y1 | `:1110` |
| `drawHudNumber(value, x, y, digitH)` | the game's own HUD digit textures. `digitW = digitH * 0.72`, advance `digitW * 0.9`, `y` is the glyph **TOP** | `:1255` |

---

## 5. Backdrop

`drawBackdrop(w, h)` — `companion_hud.cpp:595`. Drawn first, every frame, both layouts.

1. `fillRect(0, 0, w, h, COL_BG)`.
2. The pause menu's stone-block texture (`DECO_BLOCKS` = `TT_BLOCK128`) tiled at
   **172 x 172** logical units from (0,0), alpha 255, tinted
   `black = 0x1B1B1AFF`, `white = 0x605D57FF`.

A flat `COL_BG` fill with no tiling is visibly flatter and darker than the real thing.
This is one of the clearest "not same as current companion" tells.

---

## 6. Tab strip

`drawTabs(x0, x1, h)` — `companion_hud.cpp:512`. `h` is the strip's **bottom** edge (the
canvas bottom in both layouts). Nothing else may recompute this geometry; the draw
publishes `s_tabRects` / `s_tabRectPage` / `s_tabRectCount` for hit tests.

### 6.1 Geometry

```
GAP   = 3
count = visiblePages(pages)                  // 3 Functional, 4 Cinematic
tabW  = (x1 - x0 - GAP * (count - 1)) / count
top   = h - TABS_H                           // TABS_H = 44
x_i   = x0 + i * (tabW + GAP)
ty0   = top + 4  - tabRaise(page, active)    // Functional only subtracts the raise
ty1   = functional ? h : h - 6
```

### 6.2 The raise animation

`tabRaise` — `companion_hud.cpp:477`. Per-page static, eased every render frame:

```
target = active ? 8.0 : 0.0
raise += (target - raise) * ANIM_RATE_GLIDE   // 0.12
snap to 0 below 0.05, to 8 above 7.95
```

Only the **top** edge moves; all bottoms stay on one line. Two tabs animate at once when
the selection moves — the plates trade height. Note the snap thresholds here are in the
0..8 unit, *not* `ANIM_DONE`/`ANIM_ZERO` (which are for 0..1 quantities).

### 6.3 Functional dress

* **Bed**: `bedY0 = top - 8`. `fillRect(x0, bedY0, x1, h, COL_WINDOW)` — it starts *above*
  the window's bottom rule and paints over it, so the window opens downward into the strip.
* **Side rules**: `fillRect(x0+0.5, bedY0, x0+2.5, h, COL_SIDE_RULE)` and
  `fillRect(x1-2.5, bedY0, x1-0.5, h, COL_SIDE_RULE)` — the same inset, width and colour as
  the content window's side rules, so the two read as one continuous line.
* **No bottom rule**: the strip runs off the screen edge.
* **Bottom-left flourish** (`drawBottomFlourish`, `:495`) drawn here — over the bed, under
  the plates: `KAZARI` at width 40, height `40 * texH/texW`, at
  `(x0 + 3, y1 - kh - 3 + kh * 0.75)` where `y1` is the **content window's** bottom
  (`s_contentRect[3]`), alpha 210, `black = 0x00000000`, `white = 0xB6A886FF`,
  **mirrored vertically**.
* **Plate**: `ty1e = ty1 + 8` (runs past the screen edge so the bottom rim falls off-canvas),
  then `drawChamferPlate(x, ty0, x+tabW, ty1e, ch=10, active, mask = 1|2)` — **top corners
  only** — wrapped in `drawBeveledBorder(x, ty0, x+tabW, ty1e, active, 1|2)`. Selected reads
  as the gold "enabled" rim.

### 6.4 Cinematic dress

A `COL_SCRIM` (11,10,8,170) bed behind the strip
(`companion_hud.cpp:675`), then `drawTabPlate(x, ty0, tabW, ty1-ty0, active)` — flat, no
chamfer, no bevel border, no raise.

### 6.5 Labels

* **One text size for the whole strip**, computed as the minimum over all tabs of
  `fittedTextSize(15, 10, tabW - 12, label)` (`:543–549`). Fitting each tab independently
  made a long localized name visibly smaller than its neighbours in the same row.
* Baseline: Functional `(ty0 + h - 6) * 0.5 + 5` — centred on the plate's **visible** span,
  not its full height, because the plate runs past the screen edge. Cinematic: `h - 18`.
* Colour: active → `TEXT_TAB_ACTIVE` (dark ink on the light parchment plate); inactive →
  `TEXT_DIM`.
* Labels come from `tabName(page)` (`companion_hud.cpp:54`): English keeps the dashboard's
  own uppercase words `MAP / ITEMS / COLLECTION / GUIDE`; other languages take archive ids
  `0x0062 / 0x0061 / 0x03E1 / —`, uppercased. **The phone must not own these strings** — they
  arrive resolved in `chrome_state`, with a `resolved` flag (`tabLabelResolved`,
  `companion_hud.cpp:82`) that says whether it is safe to cache for the session.
* Active-tab rule: `active = pages[i] == page && !guideIsOpen()` — while the guide reader is
  up, **no** tab is selected.
* Published hit rect: `x .. x+tabW`, top `= min(ty0, top)` in Functional (so the raise is
  tappable), bottom `= h`.

---

## 7. Content window

`drawContentWindow(wx0, wx1, cy0, cy1)` — `companion_hud.cpp:777`. Draw order matters and is
the thing most likely to be got wrong.

### 7.1 Order

```
1. publish s_contentRect = {wx0, cy0, wx1, cy1}
2. fillRect(window, COL_WINDOW)
3. YAKUSHIMA mottle: drawTimgTinted(bg, wx0+1.5, cy0+1.5, w-3, h-3,
                                    alpha 160, 0x0C0B0AFF, 0x1F1D19FF)
4. set the window scissor (s_winClip, in NATIVE pixels) and applyWinClip()
5. page content  (with the page-change transition, §7.3)
6. s_drawAlpha = 1.0   ← single choke point, every page draws above this line
7. drawCinematicContextTab  (inside the scissor, Cinematic only)
8. clear the scissor to full screen
9. drawWindowOrnaments      ← the frame is drawn LAST, on top of the page
10. drawGuideOverlay        ← guide LAST of all, over the ornaments
```

Steps 9 and 10 are deliberate and were arrived at by fixing visible bugs:

* The frame on top (`:915` comment): the page rect *is* the full window and content only
  insets a few pixels, so the page overlapped the frame. When the content faded, the frame
  underneath was revealed and the whole border appeared to animate.
* The guide above the ornaments (`:921` comment): its `<` `>` buttons sit exactly where the
  corner flourishes are, and a flourish painted over them looked like damage.

### 7.2 Frame and ornaments — `drawWindowOrnaments` `companion_hud.cpp:737`

Converged over many visual iterations; the asymmetry is intentional.

| Piece | Geometry | Alpha | Tint (black → white) |
|---|---|---|---|
| Top outer rule | `LINE2` at `(x0, y0-1, w, 5)` | 235 | `0x00000000` → `0x9A8F79FF` |
| Top inner rule | `LINE2` at `(x0, y0+3, w, 4)` | 185 | `0x00000000` → `0x6E6555FF` |
| Bottom rule | `LINE2` at `(x0, y1-4, w, 5)` — **one only** | 235 | `0x00000000` → `0x9A8F79FF` |
| Left side rule | `fillRect(x0+0.5, y0, x0+2.5, y1, COL_SIDE_RULE)` | — | drawn, not textured |
| Right side rule | `fillRect(x1-2.5, y0, x1-0.5, y1, COL_SIDE_RULE)` | — | — |
| TL flourish | `KAZARI` at `(x0+3, y0+3 - kh*0.25)`, `w = 40`, `kh = 40*texH/texW` | 210 | `0x00000000` → `0xB6A886FF` |
| BL flourish | same, **mirrored vertically**, at `(x0+3, y1 - kh - 3 + kh*0.75)` | 210 | same |

* **Double** rule on top, **single** on the bottom: the tab strip sits right under the
  bottom, and a second line there crowds the context tab.
* The sides are drawn rather than textured because `LINE2` is a horizontal rule — squeezed
  into a tall thin strip, its gradient faded out before the bottom (`:756` comment).
* In **Functional** the bottom flourish is *not* drawn here; `drawTabs` draws it
  (§6.3) so it lands on the strip's bed but under its plates.
* Fallback with no `LINE2`: four 1.5px `COL_FRAME` edges.

### 7.3 Page-change transition — `companion_hud.cpp:812–898`

The incoming page **grows out of its own tab**; the outgoing page **stays put and fades**.

```
sGrowFrom = the incoming page's own tab rect, captured from LAST frame's s_tabRects
sGrowT    = 0 at the change, then  sGrowT += (1 - sGrowT) * ANIM_RATE_SETTLED  (0.20)
            done at ANIM_DONE (0.97)
no tab rect for that page (layout change) → snap, no animation
a second change mid-transition → snap
```

Per frame while growing:

* outgoing page drawn at the **full window rect** with `s_drawAlpha = 1 - t`;
* incoming page drawn at `lerp(sGrowFrom, windowRect, t)`, **only when `t > 0.2`** (below
  that the box is too small for the page to lay out sensibly);
* `s_pageSliding = true` — **page-content touch is suppressed** for the whole transition
  (both pages publish rects mid-slide); tabs and permanent chrome stay live.

The outgoing page is *not* shrunk back into its tab — two things moving in opposite
directions read as busy — and not left opaque either, so the backdrop never flashes
through (`:875` comment).

### 7.4 Per-page insets

`drawContentWindow`'s `drawPage` lambda (`companion_hud.cpp:846`):

| Page | Rect handed to the page |
|---|---|
| `PAGE_INVENTORY` | `(x0+4, y0+8, x1-2, y1-8)` |
| `PAGE_COLLECTION` | `(x0+4, y0+8, x1-2, y1-8)` |
| `PAGE_MAP` | `(x0+4, y0+4, x1-4, y1-4)` |
| `PAGE_GUIDE` | no content — it only calls `guideOpen()`; the reader is the overlay |
| guide overlay | `(wx0+4, cy0+4, wx1-4, cy1-4)` |
| Cinematic context tab | `(wx0+4, cy0+4, wx1-4, cy1-4)` |

Note the **asymmetric** `x1-2` on items/collect (the scroll-hint lane) versus `x1-4` on map.

### 7.5 Scissor discipline

Any page that scissors internally **must** restore via `applyWinClip()`
(`companion_gfx.cpp:181`) and must set inner clips through `setWinScissor()`
(`companion_gfx.cpp:145`), which intersects with the active window clip. During the page
slide the page rects extend past the window, and a raw scissor computed from them
*replaces* the window clip and bleeds over the chrome; negative coords also wrap the u32
casts.

---

## 8. Context tab

`drawContextTab(x0, y0, x1, y1)` — `companion.cpp:546`. One permanent control whose action
depends on page + world state. `contextTabAction(&clickable)` returns one of
`CTX_NONE / CTX_WARP / CTX_FLOOR / CTX_INFO / CTX_HOME / CTX_BACK`
(`companion_internal.h:531`); an unavailable action still returns its id so the tab can
label itself, but draws dim and does nothing.

* Plate: `drawChamferPlate(x0, y0, x1, y1, ch = 12, selected = clickable, mask)` where
  `mask = (1|8)` in Functional — **left corners only**, echoing the screen edge, right side
  square so it runs flush into the bridge across to the window — and `mask = 0` in Cinematic,
  where it is a floating in-window button with nothing to hang a left chamfer off.
* Text (Info / Back / `< COLLECTION`): `drawTextFittedCentered(cx, (y0+y1)*0.5 + 5, 13,
  minSize 8 (7.5 for `< COLLECTION`), maxW = w - 10, clickable ? TEXT_TAB_ACTIVE : TEXT_DIM)`.
* `CTX_WARP` / `CTX_FLOOR` draw their own icon+label content over the plate
  (`drawWarpTab` `companion_pages.cpp:855`, `drawFloorTab`): icon 28 left, label right,
  the group centred on one baseline; the warp icon is drawn at alpha 255 when active and
  130 when the portals are already shown. **Before Midna grants warping the plate is left
  EMPTY** rather than advertising a dead control.
* Placement, Cinematic (`drawCinematicContextTab`, `companion_pages.cpp:1394`):
  `tw = 70`, `th = 28`, `tx = x1 - 4 - tw`; `ty = y0 + 4` on the **map** page,
  `y1 - 4 - th` elsewhere. `CTX_NONE`, and `CTX_BACK` on the items page, publish no rect.
* Placement, Functional: the left column's middle zone, `FN_CTXTAB_H = 44` tall.

---

## 9. Corner boxes and equip buttons

Functional only. `drawFunctionalCorners` — `companion_functional.cpp:931`.

### 9.1 Corner geometry

```
lx0 = FN_CORNER_MARGIN            = 6          ty0 = FN_TOPBAR_H + FN_CORNER_MARGIN = 34
lx1 = lx0 + FN_CORNER             = 90         ty1 = ty0 + FN_CORNER                = 118
rx1 = w - FN_CORNER_MARGIN                     by1 = h - FN_CORNER_MARGIN
rx0 = rx1 - FN_CORNER                          by0 = by1 - FN_CORNER
```

* top-left = **transform** (`drawTransformCorner` `:970`)
* top-right = **slot I**, bottom-right = **slot II** (`drawFunctionalSlotCorners` `:805`)
* bottom-left = **Z / Midna** (`drawFunctionalZCorner` `:897`)

The four boxes are **persistent chrome**: an unavailable action dims, it never vanishes, so
the corners never move.

### 9.2 The box itself — `drawCornerBox` `companion_functional.cpp:1058`

`drawBeveledCornerButton(x0, y0, x1, y1, enabled)` (§4.4, all four corners cut), then the
label at `(x0 + FN_LABEL_DX, y0 + FN_LABEL_DY + FN_CORNER_LABEL)` = `(x0+8, y0+19)` —
a **baseline** — at size `FN_CORNER_LABEL = 12`, `enabled ? TEXT_MAIN : TEXT_DIM`.
At `FN_CORNER = 84` the bevel chamfer is `min(14, 0.16*84 = 13.44)` = **13.44**.

### 9.3 Press / pop / morph pinch

Every button face pinches toward its centre while held:

```
slots I/II  pin  = FN_CORNER * (0.07 * pressAnim[2+i] - 0.05 * popAnim[2+i]) + morphPin
X / Y       pinch= FN_BTN    * (0.07 * pressAnim[xy]  - 0.05 * popAnim[xy])  + morphPinch
transform   pin  = (x1-x0)   *  0.07 * pressAnim[4]
Z           zPin = FN_CORNER *  0.07 * pressAnim[5]
morph       = SIZE * (1 - |2*wolfBlend - 1|) * 0.45        // crossfade through a pinch
applied as: x0 += pin/2, y0 += pin/2, x1 -= pin/2, y1 -= pin/2
```

A fresh equip gives a **negative** pinch (`popAnim`) — the face bulges briefly so the eye
finds where the item went. An active press outweighs it.
**The touch rect, the labels and the rings all keep the resting geometry** — only the
painted face moves (`:850`, `:1124`).

### 9.4 Slot I / II contents

* icon: `pside * 0.62` square, at `(px0 + (pside-icon)/2, py0 + (pside-icon)*0.55)` —
  note **0.55**, not 0.5, vertically.
* alpha `255` if the button is usable, `128` if not (matches the main HUD's own dimming).
* ammo, if any: `drawAmmoCountCentered(ammo, px0 + pside*0.5, py1 - 10)`.
* equip mode: `drawChamferFrame(rx0, y0, rx1, y1, FN_CORNER_CHAMFER = 14, t = 3,
  hot ? (235,200,90,255) : (110,145,210,235), 0xF)` at resting geometry (`:884`).
* deny flash: same frame, `(225, 62, 50, 255 * denyFlash / DENY_FLASH_FRAMES)`.

### 9.5 X / Y round buttons — `drawFunctionalItemButtons` `companion_functional.cpp:1074`

```
step  = (iiCenterY - iCenterY) / 3            // I, X, Y, II are 4 evenly spaced slots
xTop  = iCenterY + step     - FN_BTN/2
yTop  = iCenterY + step * 2 - FN_BTN/2
baseX = colX + FN_COL_W - FN_CORNER_MARGIN - FN_BTN      // X flush with the corner boxes
X at (baseX, xTop)         Y at (baseX - FN_XY_STAGGER, yTop)     // Y is 8 left of X
```

* Face: `drawButtonCircleBase` (`companion_gfx.cpp:1212`) — the **A button's circular base
  texture from the live HUD**, tinted with the X/Y button's own black/white *and its
  four corner colours*, aspect-fit inside the box, and **stamped twice** because the base
  carries baked-in translucency that reads see-through over the companion backdrop.
  This is game art with no fetch path today — see §13.
* Item icon: `psz * 0.74` square, centred on the pinched face, alpha `255` / `128`.
* Ammo: centred under the icon at `(bx + FN_BTN/2, pby + psz - 8)`.
* Letter: at `(bx + FN_LABEL_DX - 6, by + FN_LABEL_DY + FN_CORNER_LABEL)` = `(bx+2, by+19)`
  baseline, size 12, `TEXT_MAIN` — **outside** the circle, top-left, so the item icon owns
  the face.
* Equip ring, drawn **last** so it replaces the button's own border:
  `drawRing(cx, cy, FN_BTN/2 - 3, thickness 5, hot ? (235,200,90) : X=(110,145,210) / Y=(105,190,130))` (`:1184–1192`).
  `hot` = the drag is within `8` of the button box — the same `DROP_GRAB` the drop test uses.
* Deny flash: same ring, `(225,62,50, 255*denyFlash/14)` (`:1195–1198`).
* Wolf action text ("Sense"/"Dig"): 13px white, centred at `(bx + FN_BTN/2, by + FN_BTN/2 + 5)`,
  with a 1px 8-way `0x000000C0` outline.

### 9.6 Ammo chip — `drawAmmoCountCentered` `companion_functional.cpp:955`

```
digitH = 12,  digits = ammo >= 100 ? 3 : ammo >= 10 ? 2 : 1
w  = digitH * 0.72 * (0.9 * (digits - 1) + 1)
x0 = cx - w/2,  y0 = bottomY - digitH
chip: fillRect(x0 - 4, y0 - 2, x0 + w + 4, bottomY + 2, (10, 9, 7, 210))
then drawHudNumber(ammo, x0, y0, 12)
```

The inventory-cell variant is different — see §10.1.

### 9.7 Cinematic equivalent

No corner boxes. A controller diamond of `CLUSTER_BTN = 38` buttons top-right
(`drawItemCluster`, `companion_hud.cpp:357`) publishing the same drop rects, plus a right
band stacked bottom-up with `BAND_GAP = 8` and `BAND_GAP_STATUS = 23`: status row, d-pad
glyph, transform button, each reporting its consumed height so hidden pieces collapse
(`companion.cpp:1686–1702`).

---

## 10. Pages

### 10.1 Inventory (ITEMS)

`drawInventoryContent` — `companion_pages.cpp:1572`. Rect `(x0+4, y0+8, x1-2, y1-8)`.

#### Which table

`invWideLayout()` (`:713`) — **`s_canvasW / s_canvasH >= 1.7`** selects the 6x4 table,
otherwise 5x5. On a 16:9 canvas the 5x5 grid is limited by height and sat small with a
third of the width unused. This is streamed as `wide` and also answered by
`invGridWide()`. **23 cells** (`INV_CELLS`, `:599`) — not 24.

Both tables list the **same slot with the same group at the same index**
(`static_assert inv_tables_share_cell_order`, `:686`), so a cell index means the same item
in both. Only `gx`/`gy` differ.

`l_invCells` (5x5, `:601`) and `l_invCellsWide` (6x4, `:621`), as `{slot, gx, gy, group}`:

| idx | slot | 5x5 (gx,gy) | 6x4 (gx,gy) | group |
|---|---|---|---|---|
| 0–4 | 0,1,2,3,4 | (0..4, 0) | (0..4, 0) | 0 tools |
| 5–9 | 5,6,8,9,10 | (0..4, 1) | (0..4, 1) | 0 tools |
| 10–12 | 15,16,17 | (0..2, 2) | (0..2, 2) | 2 bombs |
| 13–14 | 20,23 | (3,2),(4,2) | (3,2),(4,2) | 4 rod/slingshot |
| 15–18 | 18,19,21,22 | (0,3),(1,3),(0,4),(1,4) | (0,3),(1,3),(2,3),(3,3) | 3 quest |
| 19–22 | 11,12,13,14 | (2,3),(3,3),(2,4),(3,4) | (5,0),(5,1),(5,2),(5,3) | 1 bottles |

In 6x4 the bottles leave their 2x2 block and become the full-height right strip; the empty
cell is (4,3). In 5x5 the empty column is the far right of rows 3–4.

#### Group tints — `companion_pages.cpp:690`

| group | RGBA | name |
|---|---|---|
| 0 | 45, 41, 33, 255 | tools (stone brown) |
| 1 | 38, 44, 40, 255 | bottles (moss green) |
| 2 | 54, 36, 30, 255 | bombs (rust red) |
| 3 | 52, 44, 28, 255 | quest (gold-brown) |
| 4 | 42, 42, 34, 255 | rod/slingshot (olive grey) |

#### Grid metrics — `computeInvGrid` `companion_pages.cpp:717`

```
INV_CAPTION_H = 26,  INV_GROUP_GAP = 8
cols, rows = wide ? (6, 4) : (5, 5)
availW = x1 - x0
availH = y1 - y0 - INV_CAPTION_H
cell   = availW / cols
if (cell * rows + INV_GROUP_GAP * 2 > availH)  cell = (availH - INV_GROUP_GAP*2) / rows
gridW  = cell * cols
gridH  = cell * rows + INV_GROUP_GAP * 2
originX = x0 + (availW - gridW) / 2
originY = y0 + (availH - gridH) / 2
pad     = cell * 0.08
icon    = cell - pad*2 - 4
```

Cell position (`drawInvCell`, `:741`) — the two group gaps are **cumulative** and inserted
before rows 2 and 3:

```
cx = originX + gx * cell
cy = originY + gy * cell + (gy >= 2 ? 8 : 0) + (gy >= 3 ? 8 : 0)
```

#### Cell art

```
tint = (slot == s_selSlot || (dragging && slot == s_dragSlot))
         ? (236, 208, 84, 255)            // COL_CELL_SEL
         : l_groupTint[group]
drawMenuBox(cx+2, cy+2, cx+cell-2, cy+cell-2, tint)      // §4.6
empty slot → stop here
drawItemIcon(slot, itemNo, cx + pad + 2, cy + pad + 2, icon)
```

Ammo chip (note: **different** from the button chip in §9.6):

```
chipW = (ammo >= 100 ? 3 : ammo >= 10 ? 2 : 1) * 9 + 6
fillRect(cx+3, cy + cell - 19, cx + 3 + chipW, cy + cell - 3, (10, 9, 7, 210))
drawHudNumber(ammo, cx + 6, cy + cell - 17, digitH = 11)
```

#### Caption — `drawInventoryCaption` `companion_pages.cpp:775`

One 13px baseline at `y1 - 5`, x `= x0 + 18`. Priority:

1. `s_equipMsgFrames > 0` → `s_equipMsg` in `TEXT_WARN` (`0xF0A050FF`);
2. a dragged or selected item → its name (archive `0x165 + itemNo`) in `TEXT_MAIN`;
3. otherwise the "drag to equip" hint in `TEXT_DIM`, **ellipsized** to
   `(x1 - 24) - (x0 + 18)`.

#### Item-info reader — `drawItemInfo` `companion_pages.cpp:1483`

Zooms out of the tapped cell (`readerZoomStep`, §11). While it travels the grid stays
underneath and **pops down**: redrawn at `shrink = 0.06 * t` inset on each side with
`s_drawAlpha *= (1 - t)`, and `s_invGeomValid = false` so the cells are not tappable as a
backdrop. Cinematic keeps an in-window Back plate — `drawTabPlate(x1-100, y1-38, 90, 30,
false)` with the label centred at `(x1-55, fy+20)`, size 14, min 9, maxW 84, `TEXT_MAIN`;
Functional retires it (the context tab is Back) and the body reclaims the space
(`readerTextBottom`, `companion_internal.h`: `by1 - (functional ? 6 : 44)`).

#### Drag ghost — `drawDragGhost` `companion_hud.cpp:1076`

```
pickup pop: ghostPop *= ANIM_DECAY_SOFT;  g = 52 * (1 + 0.15 * ghostPop)
silhouette: drawItemIconSilhouette(slot, item, gx-4, gy-4, g+8, 0xECD054FF)
icon:       drawItemIcon(slot, item, gx, gy, g)     with gx = dragX - g/2
fly-out:    ghostFlyT += (1 - ghostFlyT) * ANIM_RATE_FAST
            pos = lerp(from, to, t);  g = 52 * (1 - 0.75 * t)
```

### 10.2 Map

`drawMapContent` — `companion_pages.cpp:1438`. Rect `(x0+4, y0+4, x1-4, y1-4)`.
Two modes: dungeon floor map (`s_dmapAvailable`) or overworld minimap; the chrome is shared.

| Element | Geometry | Line |
|---|---|---|
| Name plate | top-left, flush: `fillChamferRect(x0, y0, x0+pw, y0+22, ch=7, (20,18,15,150), mask = 4\|8)` with `pw = 18 + measureText(12, name)`; text at `(x0+9, y0+15.5)` baseline, 12px `TEXT_MAIN`. **Not drawn in Functional** — the left column already names the region | `:1370` |
| Reset button | bottom-right: `drawTabPlate(x1-70, y1-30, 66, 26, viewMoved)`; label centred at `(x1-37, y1-12)`, size 13 min 8 maxW 60, `viewMoved ? TEXT_TAB_ACTIVE : TEXT_DIM`. Rect `x1-70 .. x1-4`, `y1-30 .. y1-4` | `:267` |
| Ooccoo button | bottom-left, mirrors Reset: `drawTabPlate(x0+4, y1-30, W=84, 26, false)`; icon 20 at `(x0+7, y1-27)`; label centred at `(x0+56, y1-12)`, size 12 min 8 maxW 50, `TEXT_DIM`. Wider than Reset so "Ooccoo Jr." fits. Hidden (zero-width rect) when unavailable | `:286` |
| Caption | `TEXT_WARN` at `(x0+14, y1-10)`, 13px, only while `s_equipMsgFrames > 0` | `:1431` |
| Placeholder | Dusklight logo 96px centred, caption `drawTextCentered(mcx, mcy-60, 17, TEXT_DIM)` | `:1142` |
| Floor picker | expands the context tab **in place**, up and down, rather than sliding a panel over the map; `ANIM_RATE_FAST` open / `ANIM_DECAY_FAST` close | `companion_pages.cpp:991–1000` |

### 10.3 Collection

`drawCollectionContent` — `companion_collect.cpp:1252`. Rect `(x0+4, y0+8, x1-2, y1-8)`;
the overview is then drawn at `(x0, y0+4, x1, y1)`.

`s_collectTab`: `0` overview, `1` bugs, `2` fish, `3` skills, `4` mail.

#### Overview — `drawCollectOverview` `companion_collect.cpp:308`

```
gearS  = 50,  gap = 9,  gearX = x0 + 16
blockTop    = y0 + 2
blockBottom = blockTop + 3*gearS + 2*gap          // = blockTop + 168
rowInset = 16
row4 = blockBottom + 8            (two rows, rowH 44, rowGap 8)
row5 = row4 + 2*44 + 8 + 8        (one row, rowH 44)
```

* **Gear boxes** (`:77`): 7 boxes — row 0 swords (2), row 1 shields (2), row 2 clothes (3).
  `bx = gearX + col*(gearS+gap)`, `by = blockTop + row*(gearS+gap)`.
  `drawMenuBox(bx, by, bx+50, by+50, worn ? 0xECD054FF : CELL_RGBA)`, icon
  `drawItemIcon(..., bx+3, by+3, gearS-6 = 44, owned ? 255 : 55)`.
* **Heart-piece progress** (`:110`): the game's own art — empty base plus one cumulative
  wedge per piece (`pieces = maxLife % 5`, max 4 wedges), `hIcon = 132`, centred at
  `cx = min((gearX + 2*(gearS+gap) + x1)/2 + 25, x1 - 14 - 66)`,
  `hy = (blockTop + blockBottom)/2 - 66`.
* **Counter grid** (`:140`): 6 cells in 3 columns x 2 rows, `gap = 14`,
  `cellW = (x1 - x0 - 2*gap)/3`, `rowH = 44`, `rowGap = 8`, `icon = rowH - 18 = 26`.
  `drawMenuBox(cell, CELL_RGBA)`; icon at `(cx+8, cy + (rowH-icon)/2)`; text baseline at
  `(cx + 8 + icon + textPad, cy + rowH*0.5 + 5)`, 14px `TEXT_MAIN`, `textPad = 8`
  (**12** for the skills cell, whose rotated scroll needs the room).
  Order: `0` quiver `arrowMax`, `1` bugs `n/24`, `2` skills `n/7`, `3` poe `n`,
  `4` fish `n/6`, `5` mail `n`. Cells 1, 4, 2, 5 are tappable and publish
  `s_collectIconRects[0..3]` = bugs, fish, skills, mail. Held-none icons draw at alpha **55**.
* **Scent + Fused Shadows row** (`:244`): `gap = 14`, `half = (x1-x0-gap)/2`.
  Scent panel left: icon 30 at `(x0+10, rowY + (rowH-30)/2)`, name ellipsized from
  `x0+46` to `x0+half-6`, baseline `rowY + rowH*0.5 + 5`, 14px.
  Right panel: label at `(fx0+10, rowY+19)` fitted 13→10 to the space before the count;
  count right-aligned by measurement at `x1 - 10 - measureText(13, count)`, same baseline,
  `TEXT_ACCENT` when live else `TEXT_DIM`; meter track
  `fillRect(fx0+10, rowY+26, x1-10, rowY+38, (10,9,7,210))` with fill
  `fillRect(bx0+1, rowY+27, bx0+1 + (bx1-bx0-2)*have/total, rowY+37, (235,200,90,255))`.
* **Equip message**: `TEXT_WARN`, 14px, at `(x0 + 16, (row5 + 44 + y1)/2 + 5)`.

#### Section header — `drawSectionHeader` `companion_collect.cpp:473`

Shared by bugs / fish / skills / mail / reader.

```
BACK_PLATE_X = 2,  BACK_PLATE_W = 78
detail: drawTabPlate(x0+2, y0+1, 78, bh, selected = true)   bh = iconSize >= 24 ? 26 : 22
        "< Back" centred at (x0 + 2 + 39, y0 + 1 + bh/2 + 5), 13 min 8, maxW 70,
        TEXT_TAB_ACTIVE;  publishes rect id -4
iconX     = x1 - 6 - iconSize                       // caller draws its icon here
leftLimit = x0 + (detail ? 2 + 78 + 8 : 6)
titleMax  = (iconX - 8) - leftLimit
ts        = fittedTextSize(titleSize, titleSize - 3, titleMax, title)
titleY    = y0 + (iconSize >= 24 ? 20 : 15)
title is RIGHT-aligned at iconX - 8 - measureText(ts, title), TEXT_DIM
  — unless it overruns titleMax, then ellipsized from leftLimit
```

Section titles are `"<name>  <have>/<total>"` (`sectionTitle`, `:458`) — the count is a
**trailing** suffix because placement varies by language.

#### Bugs — `drawCollectBugs` `companion_collect.cpp:340`

Header icon 26, title 15. Then `y0 += 26 + 14`.
6 columns, `bugCell = (x1-x0)/6`, `bugIcon = min(44, bugCell - 8)`, 24 icons at
`bx = x0 + (i%6)*bugCell + (bugCell-bugIcon)/2`, `by = y0 + 4 + (i/6)*(bugIcon+6)`,
alpha 255 if caught else **55**.

#### Fish — `drawCollectFish` `companion_collect.cpp:362`

Three columns: `colName = x0+10`, `colCaught = x0 + 0.58*availW`,
`colRecord = x0 + 0.80*availW`. Header row baseline `headY = y0 + 26 + 28`, 13px `TEXT_DIM`.
Six rows at `headY + 26 + i*26`, 15px, `TEXT_MAIN` if caught else `TEXT_DIM`, species name
ellipsized to `colCaught - 8 - colName`. Record is inches for PAL-English, centimetres
otherwise.

#### Skills list — `drawSkillsList` `companion_collect.cpp:931`

```
header icon 26, title 15; y0 += 40
rowH = 44, gap = 8, 7 rows, contentH = 7*44 + 6*8 = 356
rx1  = x1 - 12                       // rows stop short: the scroll hint gets its own lane
ry   = y0 + i*(rowH+gap) - s_scrollSkills   (skip if ry < y0-rowH or ry > y1)
drawMenuBox(x0, ry, rx1, ry+rowH, i == s_collectSel ? 0x5A4A2AFF : CELL_RGBA)
scroll icon 30 at (x0+12, ry + (44-30)/2)
ORD_X = 54, NAME_X = 210, baseline ry + rowH*0.5 + 5, size 15
  ordinal  ellipsized to (NAME_X - ORD_X) - 6,  TEXT_MAIN / TEXT_DIM
  name     ellipsized to (rx1 - 34) - (x0 + NAME_X), TEXT_ACCENT  (or "???" TEXT_DIM)
  chevron  ">" at rx1 - 26, TEXT_DIM
```

Only the **visible** part of a row is published as a tap rect, and only if taller than 12px.

#### Mail list — `drawLettersList` `companion_collect.cpp:1058`

`rowH = 40`, `gap = 6`, newest first. Icon 28 at `(x0+12, ry + (40-28)/2)`.
`textX = x0 + 52`, `laneW = (rx1 - 30) - textX`.
Sender right-aligned at `rx1 - 30 - senderW`, 13px `TEXT_DIM`, **capped at 40% of the lane**
(unclamped, a long localized name drove the subject width negative and the subject vanished).
Subject at `textX`, 14px `TEXT_MAIN`, ellipsized to `laneW - senderW - 12`.
Chevron `">"` 15px `TEXT_DIM` at `rx1 - 20`. Empty state: centred 15px `TEXT_DIM`.

#### Reader detail — `drawReaderDetail` `companion_collect.cpp:846`

```
wrap width = x1 - x0 - 36      (computed BEFORE the zoom, so the animation never rewraps)
header: iconSize 20, titleSize 12, detail = true  (so the "< Back" plate is drawn)
y0 += 28
title   drawText(x0 + 4, y0 + 16, 16, TEXT_ACCENT)
corner  drawText(x1 - measureText(14, corner) - 6, y0 + 16, 14, TEXT_MAIN)
body box: drawDetailBox(x0, y0 + 26, x1, y1 - 2)
lineH 21, first baseline by0 + 22, x = x0 + 16 + indent, 14px TEXT_MAIN
text region clipped to (x0, by0 + 6, x1, readerTextBottom(by1))
scroll hint at x1 - 4, by0 + 4 .. textBottom - 4
```

#### Section zoom — `companion_collect.cpp:1266–1310`

Opening a section grows it out of the tapped library cell; closing shrinks it back and
flips `s_collectTab` to 0 when it lands. While it travels:

* the **overview stays underneath and pops down**: drawn at `shrink = 0.06 * t` inset with
  `s_drawAlpha = prev * (1 - t)`;
* the section is drawn at `lerp(zoomFrom, fullRect, t)` with `s_drawAlpha = prev * t`;
* **no panel fill while travelling** — the section fades in over the overview instead;
* tap rects are suppressed for the whole animation (`collectZoomActive()`), or a second tap
  during the ~170 ms re-opens whichever cell drifted under the finger.

### 10.4 Guide

`drawGuideOverlay(x0, y0, x1, y1)` — `companion_guide.cpp:1244`. An **overlay over the
content window, not a Page** (see the file header for why: `drawDashboard` force-resets
`s_page` for any page missing from `visiblePages()`, and `l_tabNames` is a `[PAGE_COUNT]`
array). Rect `(wx0+4, cy0+4, wx1-4, cy1-4)`, drawn last of everything in the window.

#### Constants — `companion_guide.cpp:614–625`

| Name | Value | Meaning |
|---|---|---|
| `TEXT_SIZE` | 14 | body text |
| `HEAD_SIZE` | 17 | headings |
| `LINE_H` | 20 | one wrapped line (headings get `LINE_H + 6` = 26) |
| `PARA_GAP` | 8 | paragraph spacer line (rules get `PARA_GAP + 6` = 14) |
| `HEADER_H` | 30 | top strip |
| `BTN_H` | 26 | prev / next / list plates |
| `ROW_H` | 30 | one browse-list row (**plate is `ROW_H - 4` = 26 tall**) |
| `BODY_INSET` | 26 | total horizontal margin around body text |
| `IMG_ALT_H` | 26 | flow height reserved for a missing image |
| `LIST_INDENT` | 14 | per nesting level |

#### Panel

```
open/close ramp: guideT += (1 - guideT) * ANIM_RATE_FAST  /  guideT *= ANIM_DECAY_FAST
                 returns immediately at guideT <= 0
s_drawAlpha = prev * guideT
fillChamferRect(x0, y0, x1, y1, 10, COL_WINDOW, 0xF)
drawChamferFrame(x0, y0, x1, y1, 10, 1.5, COL_FRAME, 0xF)
bodyY0 = y0 + HEADER_H + 6        viewH = y1 - bodyY0 - 8
bodyX  = x0 + 10                  bodyW = x1 - x0 - BODY_INSET
```

It must be **opaque**: the page underneath keeps drawing (it owns the tab strip's grow
animation), and a chamfered bed plus the window rule makes it read as a separate surface
rather than the map with text painted on it.

#### Header — `drawGuideHeader` `companion_guide.cpp:1068`

Only while reading a section: two nav plates, `bw = 34`, `next` at `x1 - 6 - bw`, `prev` at
`next - 4 - bw`, both `drawTabPlate(bx, y0+2, 34, BTN_H, true)` with `"<"` / `">"` centred at
`(bx + 17, y0 + 21)`, size 16 min 9 maxW 28, `TEXT_TAB_ACTIVE`.
Title at `(x0 + 8, y0 + 19)` baseline, `TEXT_SIZE`, `TEXT_ACCENT`, ellipsized to
`titleRight - textX` where `titleRight = prevX - 10` (or `x1 - 10` on the list).
If the section is the player's current position, the "here" marker (Link's own portrait,
wolf-aware, `drawHereMarker` `:340`) is drawn 18px at `(titleX, y0+14)` and the title shifts
right by 22.
Rule under the header: `fillRect(x0+6, y0+HEADER_H, x1-6, y0+HEADER_H+1, COL_FRAME)`.

#### Browse list — `drawGuideBrowseList` `companion_guide.cpp:1134`

One row per chapter header plus its sections while expanded (**one chapter at a time**: 23
chapters x ~8 sections = 200 rows, and the point of grouping is that you scroll chapters).

```
contentH = rows * ROW_H;  clamp scroll BEFORE placing rows
ry = bodyY0 - scroll, stepping ROW_H
clip to (x0+2, bodyY0, x1-2, y1-3)         // inset, or rows paint over the bottom rule
header row: rx = bodyX,      rw = bodyW
section row: rx = bodyX+18,  rw = bodyW-18
drawTabPlate(rx, ry, rw, ROW_H - 4, open)
header disclosure "+"/"-" at (rx+8, ry+19), TEXT_SIZE, open ? TEXT_TAB_ACTIVE : TEXT_ACCENT
label at (rx + (header ? 24 : 8), ry + 19), ellipsized to rw - (header ? 32 : 16) - markerW
  colour: open ? TEXT_TAB_ACTIVE : (header ? TEXT_ACCENT : TEXT_MAIN)
"here" marker 18px at (rx + rw - 24, ry + (ROW_H-4)/2), markerW = 22 when shown
```

Rows are **candidates**, not actions: the tap is remembered and fires on release, so a drag
that starts on a row scrolls instead of opening.

#### Section body — `drawGuideSectionBody` `companion_guide.cpp:1188`

Greedy word wrap against real glyph metrics, run **once per section open or width change**,
never per frame (`rewrapIfNeeded`, `:716`). The wrap is keyed on (guide, section, width) —
Cinematic and Functional give different widths, so a HUD-mode switch re-wraps.

Line kinds (`drawBodyLine`, `:754`), first baseline `bodyY0 + TEXT_SIZE - scroll`, advancing
by each line's own stored height:

| kind | draw |
|---|---|
| spacer (`0xFF`) | nothing, just advance `PARA_GAP` |
| `Rule` | `fillRect(x, y-6, x + wrappedWidth, y-5, COL_FRAME)` |
| `Heading` | `drawText(x, y, HEAD_SIZE, TEXT_ACCENT)` |
| `ListItem` | `drawText(x + LIST_INDENT * level, y, TEXT_SIZE, TEXT_MAIN)`; the `"* "` bullet is part of the first line's text so the wrap accounts for its width |
| `Paragraph` | `drawText(x, y, TEXT_SIZE, TEXT_MAIN)` |
| `Image`, loaded | `drawTimg(t, x, y - TEXT_SIZE, drawW, drawW * th/tw)` with `drawW = min(wrappedWidth, texW)` |
| `Image`, pending | `drawChamferFrame(x, py0, x+pw, py1, 6, 1, (96,91,80,190), 0xF)` at the image's own footprint + centred alt text — **an outline, never a filled plate** (a solid block of image size under a real image reads as the image rendered twice) |
| `Image`, failed | alt text ellipsized to `wrappedWidth`, `TEXT_DIM` |

Image slot heights are reserved from **recorded** dimensions at wrap time, never by decoding,
so the flow does not resize when a blob loads and jump under the finger.

Cinematic only, bottom-right: a "list" plate `drawTabPlate(x1-68, y1-6-BTN_H, 62, BTN_H,
true)` with `TEXT_SIZE` `TEXT_TAB_ACTIVE` text centred at `(bx+31, by+18)`. Functional
retires it — the context tab is Back.

Empty state: centred `TEXT_SIZE` `TEXT_DIM` at the panel centre.

---

## 11. Animation table

Every companion animation is one of two shapes, stepped **once per render frame**:
an APPROACH `x += (target - x) * rate` or a DECAY `x *= rate`. The constants
(`companion_internal.h:823–835`) are medians of the eight ad-hoc literals they replaced.
Pick by **intent**, not by number.

| Constant | Value | Shape | Used by |
|---|---|---|---|
| `ANIM_RATE_SNAP` | 0.50 | approach | button press attack — must land under the finger (`companion.cpp:1021`) |
| `ANIM_RATE_FAST` | 0.30 | approach | pop-ups opening: reader zoom (`companion.cpp:502`), collect zoom (`companion_collect.cpp:1280`), floor picker (`companion_pages.cpp:991`), drag-ghost fly (`companion_hud.cpp:1081`), guide open (`companion_guide.cpp:1254`), map reset zoom (`companion.cpp:1893`) |
| `ANIM_RATE_SETTLED` | 0.20 | approach | the page transition, and only that (`companion_hud.cpp:839`) |
| `ANIM_RATE_GLIDE` | 0.12 | approach | continuous follow: tab raise (`companion_hud.cpp:483`), dmap room-follow (`companion_dmap.cpp:399–405`) |
| `ANIM_DECAY_FAST` | 0.70 | decay | dismissals: reader-zoom close (`companion.cpp:495`), collect-zoom close (`companion_collect.cpp:1271`), guide close (`companion_guide.cpp:1259`), picker close (`companion_pages.cpp:996`), press release (`companion.cpp:1026`), left-box swipe settle (`companion_functional.cpp:514`), map pan/zoom settle (`companion.cpp:1890–1895`) |
| `ANIM_DECAY_SOFT` | 0.80 | decay | tails meant to be felt: transform/Z press tail (`companion.cpp:1033`), equip pop (`companion.cpp:1057`), drag pickup pop (`companion_hud.cpp:1101`), floor-change fade (`companion_pages.cpp:417`) |
| `ANIM_DONE` | 0.97 | threshold | pin an approach at 1.0 past this |
| `ANIM_ZERO` | 0.03 | threshold | pin a decay at 0 below this |

`ANIM_DONE` / `ANIM_ZERO` are for quantities **normalised to 0..1 only**. `tabRaise`'s 0..8
raise has its own thresholds (0.05 / 7.95).

Animations with their own shape, not in the table:

| Animation | Rule | Line |
|---|---|---|
| Tab raise | `target = active ? 8 : 0`, `GLIDE` | `companion_hud.cpp:477` |
| Page transition | `SETTLED`; incoming grows from its tab, outgoing fades in place; incoming skipped below `t = 0.2` | `companion_hud.cpp:839` |
| Reader / collect zoom "pop down" underlay | `shrink = 0.06 * t` inset each side, `alpha *= (1 - t)` | `companion_collect.cpp:914`, `:1041`, `:1295`, `companion_pages.cpp:1586` |
| Press pinch | `SIZE * 0.07 * pressAnim` | `companion_functional.cpp:852`, `:975`, `:1126` |
| Equip pop | `SIZE * -0.05 * popAnim`, `popAnim` set to 1 on a SELECT change, `DECAY_SOFT` | `companion.cpp:1040–1062` |
| Wolf morph pinch | `SIZE * (1 - abs(2*wolfBlend - 1)) * 0.45` | `companion_functional.cpp:812`, `:1081` |
| Drag pickup pop | `g = 52 * (1 + 0.15 * ghostPop)`, `DECAY_SOFT` | `companion_hud.cpp:1101` |
| Drag fly-out | `g = 52 * (1 - 0.75 * t)`, `FAST` | `companion_hud.cpp:1085` |
| Deny flash | `alpha = 255 * denyFlash / 14`, ticked in **game** frames | `companion_functional.cpp:1196`, `:890` |
| Floor-change veil | `fade *= DECAY_SOFT`, `fillRect(..., alpha 230 * fade)` | `companion_pages.cpp:417` |

**Rate caveat for the phone.** These are per-*render*-frame multipliers on the PC, tied to
whatever rate the dashboard paints at. `DENY_FLASH_FRAMES` and `TAP_HOLD_FRAMES` are per-
*game*-frame. A phone running at a different refresh must convert to a time constant — for a
rate `r` assumed at 60 Hz, use `1 - pow(1 - r, dt * 60)`; for a decay `d`, `pow(d, dt * 60)`.
Copying the literal per-frame rate onto a 120 Hz panel makes every animation twice as fast.

---

## 12. Text

This is the part that cannot be matched by copying numbers, so it gets the most detail.

### 12.1 How text is drawn today

One function: `drawText` — `companion_gfx.cpp:572`.

```cpp
JUTFont* font = mDoExt_getMesgFont();
font->setGX();
font->setCharColor(JUtility::TColor(rgba));
font->drawString_scale(x, y, size * 0.85f, size, buf, true);
```

So the *only* text style in the whole dashboard is: the game's message font, one flat
colour, **`scale_x = size * 0.85`, `scale_y = size`**. There is no bold, no italic, no
letter-spacing, no second face. Colour is always one of the four `TEXT_*` constants (plus
`TEXT_WARN` `0xF0A050FF`, `0xFFFFFFFF` for the wolf action label and `0x000000C0` for its
outline).

### 12.2 The font

`mDoExt_getMesgFont()` → `mDoExt_font0`, created by `mDoExt_initFont0`
(`src/m_Do/m_Do_ext.cpp:3761`) from **`rodan_b_24_22.bfn`**, loaded out of
`dComIfGp_getFontArchive()`.

* Non-JPN (and PC non-JPN): a `JUTResFont` — the whole `.bfn` stays resident.
* JPN: a `JUTCacheFont` with a 512-page/200-cell cache.

It is a **game archive resource**. It cannot be bundled in the APK. Full stop.

### 12.3 Exact geometry of a glyph

From `JUTResFont::drawChar_scale` (`libs/JSystem/src/JUtility/JUTResFont.cpp:256`), with
`scale_x = size*0.85`, `scale_y = size`, and `flag == true` for every character
(`drawString_size_scale` sets `a7 = 1` after the first, and `drawText` passes `true`):

```
cellScale = scale_x / getCellWidth()            // GLY1.cellWidth
left   = pos_x - width.field_0x0 * cellScale    // the left bearing is SUBTRACTED
right  = left + scale_x                         // the drawn cell is always scale_x wide
top    = pos_y - getAscent()  * scale_y / (getAscent() + getDescent())
bottom = pos_y + getDescent() * scale_y / (getAscent() + getDescent())
advance= width.field_0x1 * cellScale
```

Consequences a port must honour:

1. **`pos_y` is the baseline.** Every `y` in this document that reaches `drawText` is a
   baseline.
2. **The drawn cell is exactly `size` tall and `size * 0.85` wide**, with the baseline
   `ascent / (ascent + descent)` of the way down. Android's `Paint.setTextSize` sets the
   em size, not this — to match, solve for the text size that makes
   `(-fm.ascent + fm.descent) == size`, i.e. `textSize = size * requested / (ascent+descent)`
   measured from the chosen face, and then re-check per face.
3. **Advance is `field_0x1` only** (the ink width), with the cell drawn `field_0x0` to the
   left of the pen. Adjacent cells therefore overlap; there is no added tracking. This is
   exactly what `measureText` sums (`companion_gfx.cpp:623`), which is the authority for
   every centred, right-aligned and ellipsized label in the dashboard.
4. `getHeight() == getAscent() + getDescent()` (`JUTCacheFont.cpp:523`).
5. Encoding is **Latin-1** for the PAL font — verified in `toUpperLatin1`
   (`companion_gfx.cpp:649`): ö = 0xF6, ß = 0xDF; 0xE0–0xFE maps to uppercase by `-0x20`
   except 0xF7 (division sign) and 0xDF (no single-byte uppercase).

### 12.4 Text helpers the layout depends on

| Helper | Behaviour | Line |
|---|---|---|
| `measureText(size, text)` | sum of `field_0x1 * (size*0.85 / cellWidth)` per byte | `companion_gfx.cpp:623` |
| `drawTextCentered(cx, y, size, rgba, text)` | `drawText(cx - measureText/2, ...)` | `:848` |
| `fittedTextSize(size, minSize, maxW, text)` | shrinks from `size` toward `minSize` until it fits | `:763` |
| `fitPrefix(size, maxW, text)` | longest byte prefix that fits | `:796` |
| `drawTextEllipsized(x, y, size, maxW, rgba, text)` | `fitPrefix` + `"..."` | `:817` |
| `drawTextFittedCentered(cx, y, size, minSize, maxW, rgba, text)` | fit then centre | `:834` |
| `toUpperLatin1` | in-place uppercase, Latin-1 aware | `:649` |
| `archiveText` / `archiveLabel` / `localizedWord` | interned archive lookups; **English keeps the dashboard's own wording for UI labels**, other languages take the game's | `:681`, `:674`, `:752` |

`archiveText` gives up after **8 tries spaced 20 calls apart** and latches English for the
rest of the session. That retry budget is why `tabLabelResolved` takes the label the caller
already has rather than re-fetching (`companion_hud.cpp:82`) — and why the phone must never
cache a string the stream has not marked resolved.

### 12.5 Can the font be fetched at runtime?

**Yes, and it is unusually cheap here — cheaper than for any other art in the dashboard.**

The reasoning:

* The glyph sheets are plain archive bytes sitting in CPU RAM. `ResFONT::GLY1`
  (`libs/JSystem/include/JSystem/JUtility/JUTFont.h:54`) carries `textureFormat`,
  `textureWidth`, `textureHeight`, `cellWidth`, `cellHeight`, `numRows`, `numColumns`,
  `startCode`, `endCode` and a trailing `data[]`. Nothing GPU-side is involved in reading it.
* **On PC the atlas is already one texture.** `JUTResFont::initJoinedTexture`
  (`JUTResFont.cpp:60`) asserts `mGly1BlockNum == 1` and builds a single `GXTexObj` of
  `textureWidth x (textureHeight * pageCount)` directly over `block.data` — i.e. all pages
  are contiguous in that one buffer. There is no per-page assembly to reproduce.
* This is exactly the situation `companion_icon_decode.cpp` already exploits for item icons:
  its header says outright that the "no CPU-side texture decoder exists" claim was false, and
  that `aurora::gfx::convert_texture()` decodes GX textures on the CPU
  (`companion_icon_decode.h:11–18`). The same call takes a font sheet.
* The metrics the phone needs are equally plain: `INF1` gives `ascent`, `descent`, `width`,
  `leading`, `defaultCode`; `WID1` blocks give the per-code `{field_0x0, field_0x1}` pairs;
  `MAP1` blocks give code → font-code with four mapping methods (`getFontCode`,
  `JUTResFont.cpp:404`), of which a Latin font uses method 0 (`code - startCode`) or 2 (a
  direct lookup table).
* Cell addressing, for the phone's own atlas sampler (`loadImage`, `JUTResFont.cpp:463`):

  ```
  code     -= GLY1.startCode
  pageIdx   = code / (numRows * numColumns)
  cellInPg  = code % (numRows * numColumns)
  cellRow   = cellInPg / numRows          // yes, numRows — the game divides by rows
  cellCol   = cellInPg - cellRow * numRows
  u = cellCol * cellWidth
  v = cellRow * cellHeight + textureHeight * pageIdx
  ```

**What it would take** (roughly, in the shape of the existing icon path):

1. `companion_icon_decode.cpp`: add `decodeFontAtlasRgba(out, w, h)` — one
   `convert_texture` of `mpGlyphBlocks[0]->data` at
   `(textureWidth, textureHeight * pageCount)` in `GLY1.textureFormat`. The joined texture
   is already exactly this; the page count is recomputable from `startCode`/`endCode` and
   `numRows*numColumns`, the same way `initJoinedTexture` does it.
2. `dualscreen.cpp`: a new fetch kind `"font"` alongside `"item"` / `"heart"` / `"map_icon"`
   / `"map_base"` (`pollAndServeIconRequests`, `:1170`). It serves off the **CPU decode**
   branch, so it costs **no capture slot and no stolen video frame** — the reason items were
   moved there (`:1176` comment).
3. The same message carries the metrics JSON: `ascent`, `descent`, `width`, `leading`,
   `cellWidth`, `cellHeight`, `numRows`, `numColumns`, `textureWidth`, `textureHeight`,
   `pageCount`, `startCode`, the `WID1` table and the resolved `MAP1` ranges. A few KB.
4. Kotlin: cache the atlas `Bitmap` for the session, and a `drawText` that walks bytes,
   maps code → font code, blits `cellWidth x cellHeight` source rects into
   `size*0.85 x size` destination rects with the baseline maths above, tinted by a
   `PorterDuff.SRC_IN` colour (the atlas is intensity, so `SRC_IN` over a solid colour is the
   exact equivalent of `setCharColor`).

**Cost.** One transfer, once per session, of a single PNG. The atlas is intensity data, which
PNG-8 grey compresses well. I could not measure the byte size — the `.bfn` is a disc asset and
is not in this repo (see §14) — but it is bounded by one texture and is a one-off, versus item
icons which are fetched continuously.

**Risks, stated honestly:**

* **JPN uses `JUTCacheFont`, not `JUTResFont`** — pages are streamed on demand and
  `mJoinedTextureHeight` is 0 (`JUTResFont.cpp:287` fallback). A whole-atlas fetch is not
  valid there. Gate the fetch on non-JPN and fall back to §12.6 for JPN. The test machine is
  PAL, so the JPN path will not be exercised by testing here.
* A CJK font's atlas is far larger than a Latin one; the same gate covers that.
* `mGly1BlockNum != 1` is a hard `CRASH` in `initJoinedTexture`, so a font with multiple GLY1
  blocks already cannot boot on PC — the single-block assumption is safe, but the new decoder
  should return false rather than assume it.

### 12.6 The alternative: substitute a device font

Ship no font; use an Android system face (or one the *app author* licenses, which is not game
art and is allowed) and accept the difference.

What breaks, specifically:

* Glyph shapes differ — this is the visible part, and it is the part the user is complaining
  about when they say "not same".
* **Advances differ**, so every centred, right-aligned, fitted and ellipsized label lands
  somewhere else. The phone would have to re-measure with its own face, which means its
  `fittedTextSize` results differ from the PC's and the same label can shrink on one and not
  the other.
* Localized strings are the worst case — the C++ comments record real overruns fixed by
  measurement (`companion_hud.cpp:582` ES "COLECCION"; `companion_collect.cpp:497` DE
  "Verborgene Fähigkeiten 3/7"; `companion_collect.cpp:1148` long sender names). A different
  face re-opens all of them.

### 12.7 Recommendation

**Fetch the atlas.** Add a `"font"` fetch kind and do §12.5.

Reasons, in order:
1. It is the only option that actually answers the user's complaint, which is specifically
   about fidelity.
2. It is a *one-off* fetch on a path that already exists and already costs nothing
   (CPU decode, no capture slot).
3. It removes an entire class of layout drift: with the real widths, the phone's
   `measureText` equals the PC's, so every centred/fitted/ellipsized label lands in the same
   place in every language without re-tuning.
4. The PC side has already done the hard half — `initJoinedTexture` proves the atlas is a
   single contiguous texture on this target.

**Ship the substitute font as the fallback**, not as the plan: use it before the atlas
arrives, and on JPN where `JUTCacheFont` makes the whole-atlas fetch invalid. Keep the
fallback's metrics plumbed through the same `measureText` seam so the two paths differ only
in the glyph source.

---

## 13. Art inventory

Nothing originating from the game may be bundled. This table is the honest accounting of what
the dashboard's look is made of.

### 13.1 Fully procedural — the phone can match these today, exactly

* every `COL_*` / `TEXT_*` fill;
* chamfered rects, gradients, frames (§4.1–4.3);
* the whole beveled corner-button construction (§4.4) — rim, seat, gradient face, inner bevel;
* rings, discs, scroll hints, meter bars, chips;
* all layout arithmetic and all animations.

### 13.2 Fetched today

`icon_request` / `icon` with kind `item`, `heart`, `map_icon`, `map_base`
(`dualscreen.cpp:1170–1240`). Item and map icons decode on the CPU
(`companion_icon_decode.{h,cpp}`); heart and map base go through the capture path.

### 13.3 Game art with **no** fetch path — the gap

These are the reason the native pages do not look like the PC's, and each one has a
straightforward CPU-decode route because they are all plain archive bytes read into owned
buffers by `JKRReadIdxResource` (`companion_icons.cpp:267`).

| Art | `DECO_*` | Archive resource | Used by |
|---|---|---|---|
| Stone block backdrop | `DECO_BLOCKS` | `TT_BLOCK128_00` | the entire backdrop (§5) |
| Button/tab plate | `DECO_TAB_PLATE` | `IM_DUNGEON_MAP_FLOOR_PARTS_10` (the wi_save plate) | every tab, every context tab, Back/Reset/Ooccoo/guide-row plates |
| Soft rule | `DECO_LINE` | `TT_LINE2` | window top ×2 and bottom rules, both top-bar separators |
| Mottled backing | `DECO_YAKUSHIMA` | `TT_YAKUSHIMA` | the content window's interior |
| Cell plate | `DECO_SLOT_PLATE` | `TT_SPOT_SQUARE3` | every inventory cell, every collection cell, every list row |
| Corner flourish | `DECO_KAZARI` | `TT_KAZARI_2ND_OKAN_64` | window TL + BL ornaments |

Also unfetched: the HUD digit textures (`drawHudNumber`, every ammo count) and the A-button
circular base used for the X/Y faces (`drawButtonCircleBase`).

**Recommendation.** Add one fetch kind `"deco"` with id = the `DECO_*` slot (0–5), served by
the same CPU-decode branch as `"item"`. Six textures, fetched once per session, each small.
They are **intensity** textures, so send the intensity channel (grey PNG) and let the phone
apply the black/white tint per §4.5 — the tints differ per call site (the same plate is used
selected and unselected, the same `LINE2` at three different white points), so sending a
pre-tinted RGBA would collapse six textures into a dozen and still be wrong somewhere. Digits
and the button base are a second, smaller kind if the buttons are wanted at full fidelity.

Every one of these is the *user's own running instance* serving art over the existing channel,
which is exactly what constraint 1 permits and what it asks for when a page needs game art
with no fetch path yet.

---

## 14. What could not be pinned down

Stated plainly rather than guessed:

1. **The font's actual metric numbers.** `ascent`, `descent`, `width`, `leading`,
   `cellWidth`, `cellHeight`, `numRows`, `numColumns`, `textureWidth`, `textureHeight`,
   `textureFormat` and the page count all live inside `rodan_b_24_22.bfn`, a disc asset that
   is not in this repo. The *formulas* above are complete and exact; the constants they
   consume are not knowable without reading the file. The name suggests a 24x22 cell but I
   will not assert which of those is width. Anyone implementing §12.5 should dump them first —
   a few `OS_REPORT`s in `initJoinedTexture` next to the existing `CRASH` checks give all of
   them in one boot.
2. **Atlas byte size**, for the same reason. It is one texture, fetched once; that is all I
   can honestly say about the cost.
3. **The `DECO_*` texture dimensions and formats.** `s_decoBuf` is `0x9000` bytes per slot,
   "sized for the largest piece, `TT_BLOCK128` (128x128)" (`companion_icons.cpp:218`), so
   128x128 is an upper bound on one of them; the rest are unknown without the archive. The
   draw code only ever uses `width`/`height` as an aspect ratio (`kh = 40 * texH / texW` for
   the flourish), so the *layout* does not depend on the numbers — only a fetch would.
4. **Whether the phone currently renders Functional, Cinematic, or a mix.** `ChromeView.kt`
   carries the Functional palette and the FN ring colours, which suggests Functional, but I
   did not audit the Kotlin layout against this spec — that is the implementation step, not
   the spec step. `chrome_state.functional` is the authority at runtime.
5. **Anything gated on late-game state.** The only save on the test machine is early game
   (3 hearts, no lantern, never enters a dungeon), so the dungeon floor map and its picker,
   the Vessel of Light, the wolf-form corner cells, the fused-shadow meter past 0, the
   skills list past 0/7 and the mail list past empty are all documented **from the source
   only** and have not been seen rendered. The geometry is transcribed, not verified.
6. **Frame-rate assumptions.** The per-frame animation rates are correct as written for the
   PC's render loop; I could not measure what rate that loop actually runs at on the test
   machine, which is why §11 gives the conversion rather than a number.
