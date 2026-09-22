# PR plan: dual-screen mod API upstream

Exploration only — nothing here is implemented. Plan for contributing the API
requested in TwilitRealm/dusklight#2562, where the maintainer asked for exactly
this from someone with the hardware.

## Scope

**In:** a way for a native mod to obtain a `Presentation`-backed surface for a
secondary Android display, and present to it through the existing GfxService.

**Out:** the companion dashboard itself. That is application code — HUD, map,
inventory, the phone protocol — and does not belong upstream. Only the
plumbing that lets *any* mod use a second screen goes up.

## Two repos, in order. Not three.

The chain is shorter than it first appears, because **aurora needs no change**.
The fork reaches its second screen through `aurora::auxwin`, but that is the
fork's own renderer path: `lib/aux_window.cpp` does not exist on aurora's
`main` at all. Upstream does not need it, because
`GfxService::register_present_target` already accepts a plain `WGPUSurface`,
and `GfxDeviceInfo` already hands a mod the `WGPUInstance` and `WGPUAdapter`
(`sdk/include/mods/svc/gfx.h`). A mod holding an `ANativeWindow*` can build its
own surface with `wgpuInstanceCreateSurface` today.

So:

### 1. encounter/borealis — the Android plumbing

Borealis owns the activity: upstream `DuskActivity` is 86 lines and extends
`BorealisActivity` (838 lines), which already imports `Display`, `Surface`,
`SurfaceHolder` and owns `createSDLSurface()`. It also already owns a JNI layer
with a clear per-feature convention — `src/http/android.cpp`,
`src/file_select/android.cpp`, `src/ws/android.cpp` — so `src/display/android.cpp`
fits without inventing a pattern.

Contents:

- Java: a `BorealisPresentation extends Presentation` carrying the window flags
  and inset handling (see Traps), plus `DisplayManager` enumeration with
  filtering, and the show/dismiss lifecycle tied to `onPause`/`onResume`.
- Java: a `DisplayManager.DisplayListener` for hotplug.
- JNI + a small C API for the host: enumerate displays, open/close a
  presentation on one, and a surface created/changed/destroyed callback
  delivering an `ANativeWindow*`.
- Non-Android platforms: stubs reporting zero secondary displays, so callers
  need no per-platform branching.

### 2. TwilitRealm/dusklight — the mod-facing service

Follows the existing service pattern exactly: a public C ABI header in
`sdk/include/mods/svc/`, an implementation in `src/dusk/mods/svc/`, and a
`constinit ServiceModule` with an id and major/minor version plus a
`modDetached` cleanup hook (see `src/dusk/mods/svc/window.cpp`, 352 lines, as
the closest template).

Two options, and the choice is upstream's to make:

- **A new `DisplayService`** — `enumerate_displays`, `open_presentation`,
  `close_presentation`, plus attach/detach and surface callbacks. Cleanest
  separation, and honest that this is a different concept from a desktop window.
- **Extend `WindowService`** with a display concept and a minor version bump.
  Fewer new surfaces, and if `open_presentation` returned a `WindowHandle` that
  `register_window_present_target` already accepts, mod code would be
  **identical on desktop and Android** — which is probably worth more than the
  conceptual tidiness.

Also: a `docs/modding.md` section, and a small demo mod. The repo already ships
demo gfx mods, so a "hello second screen" one is the idiomatic way to prove the
path end to end and give reviewers something to run.

## Testability — this is the part that unblocks review

@PJB3005 said this is hard to develop without a dual-screen device. It is
largely testable without one:

**Android's "Simulate secondary displays" developer option creates an overlay
display that appears in `DISPLAY_CATEGORY_PRESENTATION` exactly like a physical
panel.** A reviewer on any Android device or emulator can therefore exercise
enumeration, Presentation creation, surface lifecycle and present, without a
Thor.

That same fact is a trap, and is why display selection cannot simply take the
lowest id: an overlay or virtual display wins that comparison and sends output
somewhere nobody can see, leaving the real second panel on the system
placeholder. Selection must exclude the activity's own display and should be
explicit rather than heuristic.

What still needs real hardware is only the input-focus behaviour (trap 3) and
per-device enumeration quirks, which is worth stating in the PR rather than
implying full coverage.

## The traps to encode

These are behaviours the fork got wrong first and fixed on a real device. Each
is small; each is invisible until it bites. Full detail in
`docs/upstream-dualscreen-api-needs.md`.

1. The activity's own display appears in `DISPLAY_CATEGORY_PRESENTATION` —
   filter it out.
2. `FLAG_KEEP_SCREEN_ON`, or the unused panel sleeps and destroys the surface.
3. `FLAG_NOT_FOCUSABLE`, or the Presentation takes input focus and the game
   stops receiving the controller.
4. Immersive flags belong on the Presentation's own window, not the activity's.
5. Consume insets on the Presentation's decor only — SDL needs real insets on
   the main window.
6. Dismiss on pause, re-show on resume and after a focus-loss overlay.
7. Stage surface teardown; destroying the native window synchronously from the
   `SurfaceHolder` callback while a frame is in flight deadlocks.
8. `DisplayListener` for hotplug.

## Sequencing

1. Open a borealis PR first; dusklight's service cannot land without it.
2. Reference #2562 from both, and cross-link them, since the maintainer there
   has already accepted the problem statement.
3. Keep the dusklight PR to the service, docs and demo mod only — no companion
   code, so the diff stays reviewable and the feature stands on its own.
4. Offer the trap list in the PR description regardless of whether the
   implementation is taken as-is; it is useful to whoever writes this even if
   they rewrite the code.

## Honest risks

- **Two maintainers, two repos.** Borealis is encounter's, dusklight is
  TwilitRealm's. The borealis half has to be acceptable to a maintainer who may
  not care about dusklight's dual-screen use case; framing it as generic
  secondary-display support for any borealis app matters.
- **API shape is theirs to choose.** Arriving with a finished `DisplayService`
  may be less welcome than arriving with the Java plumbing plus a proposed
  shape. Worth asking in #2562 before writing the dusklight half.
- **The fork's code is not directly submittable.** It is entangled with the
  companion — display selection has a swap preference, the Presentation builds a
  companion-sized `SurfaceView`, and the JNI carries companion touch, pinch,
  battery and screenshot entry points. Extraction is real work, not a copy.
