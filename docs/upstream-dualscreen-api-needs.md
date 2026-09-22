# What upstream actually needs for a dual-screen mod API

Answer to TwilitRealm/dusklight#2562, grounded in the working implementation in
igawa6/dusklight and in reading the v2.0.0 mod SDK.

**Short version: GfxService needs nothing. The entire gap is one Android-side
API — a way for a native mod to obtain a `Presentation`-backed surface for a
secondary display.** @Pixelsilzavon77's comment in the thread is exactly right;
this document adds the specifics and the traps.

## What already works, and should not change

`GfxService::register_present_target` already takes a plain `WGPUSurface`
(`sdk/include/mods/svc/gfx.h`), and `GfxDeviceInfo` already hands a mod the
`WGPUInstance` and `WGPUAdapter`. So a mod that *has* a surface can present to
it today, on any platform, with no upstream change at all. Desktop is
consequently already solved: `WindowService::create_window` +
`register_window_present_target` is sufficient.

Verified against v2.0.0: `sdk/include/mods/svc/window.h` and `gfx.h` are
untouched in the dual-screen fork, so nothing here depends on fork changes.

## The gap

On Android the chain to a second physical panel is:

    DisplayManager  ->  pick a secondary Display
                    ->  Presentation (a Window on that Display)
                    ->  SurfaceView / SurfaceHolder
                    ->  android.view.Surface
                    ->  ANativeWindow_fromSurface()      [JNI]
                    ->  wgpuInstanceCreateSurface with
                        WGPUSurfaceSourceAndroidNativeWindow
                    ->  WGPUSurface  -> register_present_target  (already fine)

Every step from `DisplayManager` down to `ANativeWindow*` lives at the
activity/Java level. The stock activity exposes none of it, and a native mod
cannot reach it — there is no JNI bridge, no `DisplayService`, and
`WindowDesc` has no notion of a display (only `x`, `y`, `display_scale`).

So the missing piece is small and specific: **enumerate secondary displays, and
open/close a Presentation on one, handing the result back to native code.**

## Suggested shape

Thinnest option, and the one that reuses what exists:

- `enumerate_displays()` -> ids, names, sizes, density, and which is primary.
- `open_presentation(display_id)` -> a handle, plus either an `ANativeWindow*`
  (mod builds its own `WGPUSurface`) or a ready `WGPUSurface`.
- `close_presentation(handle)`.
- Callbacks: display attached/detached, and surface created/changed/destroyed.

Handing back something `GfxService` already accepts — a `WGPUSurface`, or a
`WindowHandle` that `register_window_present_target` takes — would let mod code
be identical on desktop and Android, which seems worth more than saving a
wrapper.

Non-Android platforms can report zero secondary displays, so a mod written
against this degrades to "no second screen here" rather than needing per-platform
branches.

## The traps — this is the part that is hard without the hardware

@PJB3005 noted this is hard to develop without such a device. These are the
things the fork got wrong first and had to fix on real hardware. Each one is a
line or two, and each one is invisible until it bites:

1. **The activity's own display appears in `DISPLAY_CATEGORY_PRESENTATION`.**
   Enumerate with that category, then filter out the current display, or the
   game renders its second screen on top of itself.
2. **`FLAG_KEEP_SCREEN_ON` on the Presentation window.** The second panel
   receives no input during normal play, so it sleeps — and when it sleeps the
   surface is destroyed underneath you.
3. **`FLAG_NOT_FOCUSABLE` on the Presentation window.** A focusable
   Presentation takes input focus and the *game* stops receiving controller
   input. On the Thor this is immediate and total the first time the user
   touches the second screen.
4. **Immersive flags belong on the Presentation's own window.** It is a
   separate `Window` on a separate `Display`; hiding the bars on the activity
   does nothing for it, and the navigation bar draws over the mod's content.
5. **Consume insets on the Presentation's decor only, never on the activity.**
   SDL needs real insets on the main window to place its own views. Hiding the
   bars stops them being drawn but the content frame stays inset, so the second
   screen lays out short by the bar height with a dead band where the buttons
   were.
6. **Dismiss the Presentation in `onPause`, re-show in `onResume`.** A
   Presentation outliving a paused activity leaks its window. It also needs
   re-showing after a focus-loss overlay, which is not the same event.
7. **Surface teardown must not block the UI thread.** Destroying the native
   window synchronously from the `SurfaceHolder` callback while a frame is in
   flight deadlocks; stage the detach instead.
8. **`DisplayManager.DisplayListener` for hotplug**, so the second screen
   appearing or vanishing mid-session is handled rather than requiring a
   restart.

## Reference implementation

The fork solves all of the above:

- `platforms/android/app/src/main/java/com/twilitrealm/dusk/DuskActivity.java`
  — `DisplayManager` enumeration and filtering, the `AuxPresentation`
  subclass carrying the flags and inset handling above, and the pause/resume
  and hotplug lifecycle.
- `src/dusk/android_aux_display.cpp` — the JNI bridge:
  `nativeAuxSurfaceChanged(Surface, w, h)` -> `ANativeWindow_fromSurface()`,
  and a `nativeDualScreenAvailable(bool)` hotplug signal.
- `extern/aurora` `aux_window.cpp` — `set_native_window()` building the
  `WGPUSurface` via `SurfaceSourceAndroidNativeWindow`, plus the staged
  teardown from trap 7.

It is currently shaped as a fork feature rather than a service, so it would
need extracting behind whatever API upstream prefers rather than merging as-is.
The Java half and the trap list are the parts worth taking; the rest of the
fork (the companion dashboard itself) is application code and does not belong
upstream.
