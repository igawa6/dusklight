# Can the dual-screen companion be a mod instead of living in the fork?

**Verdict: mostly yes, with one hard exception and one serious caveat.**

The desktop second screen, the whole ~10,000-line dashboard, and the phone companion can all run
as a mod. The **Android second physical display cannot** and must stay in the fork. The caveat is
that the mod would reach the game through by-name symbol resolution keyed to each build, which is
a real long-term fragility rather than a detail.

This assessment was produced by reading the SDK headers and testing the built binary, not by
reading `docs/modding.md` alone — the doc summarizes and omits.

---

## What a mod can do

### It can have its own window and render to it

`WindowService::create_window` (`sdk/include/mods/svc/window.h`) plus
`GfxService::register_window_present_target` / `push_present`
(`sdk/include/mods/svc/gfx.h`, documented at `docs/modding.md:1013-1096`). Windows are hidden
until `show_window`, and surfaces reconfigure automatically on resize.

This replaces `aurora::auxwin` outright. The vendored-renderer change the feature currently
carries (a `CreateInfo::exactPixelSize` flag) simply dissolves — a mod sizes its own window.

### It can draw the dashboard as-is — the key finding

The SDK ships **no GX and no J2D headers**: 27 service headers, and grepping the whole SDK for
`GXBegin`, `J2DPicture` or `fillRect` returns nothing. Taken alone that reads as a blocker,
because the companion's entire drawing layer is built on those primitives.

It is not, because `HookService::resolve()` resolves **any game symbol by name from the build's
symbol manifest, including non-exported statics**, and returns an address
(`sdk/include/mods/svc/hook.h:117-127`). An address can be called, not merely hooked.

Confirmed empirically rather than assumed — `strings` on the built binary:

| probe | matches |
|---|---|
| exact `GXBegin` / `GXSetTevOp` / `GXEnd` | 6 |
| `J2DPicture`-related | 385 |
| companion's own functions (`drawDashboard`, `gatherHudState`) | 8 |

So the manifest is broad, not a narrow curated surface. A mod can resolve and call the game's own
2D primitives, which means **the existing drawing code ports rather than gets rewritten** — the
alternative being a from-scratch reimplementation in raw WebGPU or RmlUI, and losing the
procedural chamfers, bevels and gradients that `docs/companion-visual-spec.md` shows the look is
made of.

The same mechanism covers game state: `dComIfGs_*`, `dMeter2Info_*`, `dMapInfo_*` are all
reachable the same way, so the state-gathering code ports too.

### It can serve the phone

`WebSocketService` is **client-only** — it has `connect` and no listen/accept/bind
(`sdk/include/mods/svc/websocket.h:94`). `HttpService` is likewise request-only. On those two
alone the phone half looks impossible.

But `NetService` **can listen**: `listen()` opens a TCP listener with a `bind` endpoint and
`accepted` handles (`sdk/include/mods/svc/net.h:122-135`). The current implementation already
hand-rolls its own HTTP upgrade and WebSocket framing over raw POSIX sockets
(`src/dusk/phone_spike_ws.cpp`, ~1300 lines); that code is transport-agnostic and ports onto
NetService.

### It can capture frames for the phone

`gfx.h` exposes no readback convenience — only `resolve_pass`, which yields borrowed GPU views
valid for the current frame. But `GfxDeviceInfo` hands over the live `WGPUDevice` and `WGPUQueue`
(`sdk/include/mods/svc/gfx.h:118-125`), plus `get_proc_address`. A mod can therefore do its own
`CopyTextureToBuffer` + `MapAsync`, which is exactly what `aurora::auxwin` does internally.

---

## What a mod cannot do

### The Android second physical display — hard blocker

`window.h` has no display selection: `WindowDesc` carries `x`, `y` and `display_scale` and
nothing that names a monitor or an Android display. `host.h` exposes no JNI, no Activity, no
Presentation surface.

The fork drives a real second display through `src/dusk/android_aux_display.cpp` and a Java
`Presentation`. There is no mod route to that, and the requirement that native dual-screen keep
working is explicit. **This part stays in the fork regardless of what else moves.**

On desktop the equivalent is weaker but adequate: a mod can position its window with `x`/`y` onto
a second monitor, which is not the same as targeting a display but reaches the same result.

### Adding methods to decompiled classes

The feature currently adds `getHeartState()` as a method on `dMeter2Draw_c`
(`include/d/d_meter2_draw.h`). A mod cannot add a method to a game class.

This is a cost, not a blocker: the accessor is a read-only derivation over data already exposed by
`getHeartPictures()`, so a mod reimplements it externally against resolved symbols. The same
applies to the visited-room generation counter added at `dSv_memory2_c`'s mutators — a mod would
instead post-hook those three functions.

---

## The serious caveat: fragility

`CMakeLists.txt:412` describes the manifest as a "hookable-surface name->address map **keyed to
the build**". Every symbol a mod resolves is a dependency on that build.

The companion is not a mod that hooks two functions. Ported this way it would resolve **hundreds**
of game symbols — every GX primitive, every J2D entry point, every state accessor. Each one is a
chance to break on a dusklight update that renames, inlines, folds or reorders it. The header
already warns about ICF folding and aliasing, and about `MOD_CONFLICT` when a C++ name maps to
more than one address (`sdk/include/mods/svc/hook.h:33-35,123`).

Worse, the whole mechanism is conditional: `cmake/SymbolManifest.cmake:76,98` records that if
`symgen` is unavailable at build time, manifest generation is skipped and "by-name hook resolution
will be unavailable". A mod built on hundreds of resolved symbols stops working entirely against
such a build.

A mod must also declare every signature itself, with no headers to check it against. A signature
that drifts from the real one is not a compile error — it is undefined behaviour at runtime.

---

## Recommendation

**A hybrid, not a wholesale move.**

1. **Keep in the fork:** the Android second-display path (no alternative exists), and a small,
   deliberately stable surface for everything the mod would otherwise resolve by name.
2. **Move to a mod:** the dashboard rendering, the state gathering, the desktop second window,
   and the phone server — the bulk of the ~10,000 lines.
3. **Add the surface rather than resolving hundreds of symbols.** The fork already knows how to
   export services (`docs/modding.md:1516`). One companion-oriented service exposing the drawing
   primitives, the state accessors and the map renderer turns hundreds of fragile by-name
   resolutions into one versioned interface. That is the difference between a mod that breaks
   every release and one that does not.

The honest summary: the mod API is more capable than it first appears, and the blocking question
is not capability but **coupling**. Resolving the game's entire 2D layer by name is possible and
would work today; it is simply the most fragile way to build it.

## What would change the answer

- A display index on `WindowDesc`, or any Android Presentation route in `host.h`, would remove the
  one hard blocker.
- A published, versioned drawing/state service would remove the fragility argument, and with it
  most of the reason to keep the feature in-tree.
