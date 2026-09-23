# Migrating the fork's dual-screen to a mod

Reference: https://github.com/SiggeMcKvack/dusklight-dualscreen-companion (CC0), which is already a
working port of this fork's companion into a pure mod, verified on a Thor.

## My earlier assessment was wrong on two points

`docs/dualscreen-as-mod-assessment.md` called two things blockers. The reference solves both, and
the corrections matter more than the original conclusions:

1. **"A mod cannot reach an Android Presentation."** It can. `src/jni_bridge.cpp` obtains a
   `JavaVM` through `SDL_GetAndroidJNIEnv` — resolved from the symbol manifest, falling back to
   hooking SDL's own `Java_*` entry points — then loads an **embedded DEX** with
   `InMemoryDexClassLoader` and binds natives with `RegisterNatives`. A mod can therefore ship its
   own Java classes, including a `Presentation`. I had assumed no JNI path existed from a mod.
2. **"A mod cannot add methods to decompiled classes."** It does not need to. `game_access.h`
   declares data-free, non-virtual subclasses (`struct dMeter2DrawAccess : dMeter2Draw_c`) carrying
   the same methods the fork added to the real headers, reached via `ga::cast(ptr)->method()`. The
   object is genuinely the base type; a derived view that adds no data and no virtuals is
   ABI-compatible.

What I got right: the fragility. The reference pins to **Dusklight v2.0.1 exactly** — "the hooks
are matched to that release's binary" — which is the by-name coupling cost, accepted rather than
avoided.

## The gap, measured

Companion sources are near-identical; the reference is a slightly older snapshot:

| file | reference | fork | delta |
|---|---|---|---|
| companion_gfx.cpp | 51,670 | 51,530 | ~0 |
| companion_hud.cpp | 48,121 | 49,096 | +2% |
| companion_pages.cpp | 67,909 | 69,900 | +3% |
| companion_collect.cpp | 54,279 | 55,940 | +3% |
| companion.cpp | 71,702 | 76,124 | +6% |
| companion_touch.cpp | 53,403 | 63,224 | **+18%** |
| dualscreen.cpp | 10,844 | 83,110 | **+666%** |

The last two are the whole story. `companion_touch.cpp` grew with the semantic-action work.
`dualscreen.cpp` grew by ~72 KB that is **entirely the phone-over-WiFi companion** — state
streaming, the poll functions, icon serving, page ownership — none of which exists in the
reference.

Also fork-only, absent from the reference: `phone_spike_ws.cpp`, `companion_state.cpp`,
`companion_map_state.cpp`, `companion_icon_decode.cpp`, `companion_collect_state.cpp`.

## Two mods, not one

These are separate features and should not be forced into one package:

- **Mod A — Android second screen.** What the reference already is. Ships the DEX, opens a
  Presentation, presents through GfxService. Android only.
- **Mod B — phone companion over Wi-Fi.** The fork's newer work. Desktop-capable, since the PC
  serves and the phone connects. Needs `NetService::listen()` (verified present:
  `sdk/include/mods/svc/net.h`) because `WebSocketService` is client-only — the existing
  hand-rolled HTTP upgrade and WS framing in `phone_spike_ws.cpp` is transport-agnostic and ports
  onto it.

Both share the companion drawing layer, which argues for a shared source tree with two mod targets
rather than two copies.

## Plan

**Step 1 — adopt the reference as the base.** CC0, proven on hardware, and already solves the four
hard parts: DEX/JNI, hooks, present target, and `.dusk` packaging. Starting from it skips the work
that took someone else real hardware time to get right.

**Step 2 — reconcile the companion sources.** Diff the reference's `src/companion/*` against this
fork's `src/dusk/companion*` and bring the fork's newer drawing work forward. Small for most files;
`companion_touch.cpp` is the real one.

**Step 3 — port the phone half.** Move `phone_spike_ws.cpp` from POSIX sockets onto `NetService`,
and the state/icon-decode modules across. Their game access goes through `game_access.h` the same
way the drawing code does.

**Step 4 — decide what the fork is still for.** If both mods carry the feature, the fork's reason
to exist is the Android *host* changes only — and per `docs/upstream-dualscreen-api-needs.md`,
those are what should go upstream rather than live in a fork indefinitely.

## Open question worth settling first

Whether to contribute to the reference repo rather than starting a parallel one. It is CC0 and
already does Mod A; a second implementation of the same thing helps nobody. Reaching out before
duplicating seems right, and the fork's newer work is exactly what that repo lacks.
