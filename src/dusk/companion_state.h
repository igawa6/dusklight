#pragma once

// Phase-1 addition to the phone-companion-display spike (see
// docs/phone-companion-design.md): a pure, read-only snapshot of the small
// HUD "chrome" values (hearts, rupees, keys, equipped items) independent of
// drawing — the companion dashboard's existing draw code
// (companion_hud.cpp/companion_functional.cpp) re-reads these same
// underlying accessors every frame with no such struct to reuse; this
// centralizes them for the new state-streaming path
// (dualscreen.cpp's pollAndPushSpikeState()) without touching any draw
// call. Precedented by discord_presence.cpp's existing "read state without
// drawing" pattern, generalized.
//
// Gated by DUSK_PHONE_SPIKE_STATE (dualscreen.h) and off by default.

namespace dusk::companion {

struct HudState {
    u16 life = 0;
    u16 maxLife = 0;
    u16 rupees = 0;
    u16 maxRupees = 0;
    u8 keys = 0;
    // Companion page index currently displayed (PAGE_MAP, PAGE_INVENTORY,
    // PAGE_COLLECTION, PAGE_GUIDE).
    u8 page = 0;
    // dItemNo_NONE_e (0xFF) means the slot is empty — callers of
    // gatherHudState() never need to know that sentinel value themselves,
    // it's already dItemNo_NONE_e's actual value (d_item_data.h), kept as
    // raw u8 here rather than translated to avoid a dependency on that
    // header from this small file.
    u8 equipX = 0xFF;
    u8 equipY = 0xFF;
    u8 equipSlot1 = 0xFF;
    u8 equipSlot2 = 0xFF;

    // Lantern oil and underwater oxygen, as PERCENT (0..100) rather than
    // the raw values, and deliberately so.
    //
    // Both drain one unit per frame while active — oxygen across 0..600
    // (ten seconds) and oil across 0..21600 (six minutes) — so putting the
    // raw counters in this struct would make the whole-struct diff in
    // pollAndPushSpikeState() fire EVERY frame for the entire time the
    // player is underwater or carrying a lit lantern. That would quietly
    // invalidate the premise its "send directly on the game thread" comment
    // rests on (tiny messages, only on actual change, rare relative to the
    // frame rate) and turn a rare-diff path into a per-frame one. This is
    // the same trap that got dim/fade deferred out of Phase 1 in the design
    // plan for exactly this reason.
    //
    // A percent is also all the phone can use: these render as a bar a
    // hundred-odd pixels wide, so sub-percent precision isn't visible. At
    // this granularity one step is 0.1s of oxygen or 3.6s of oil.
    //
    // Unlike hearts, neither needs any art fetched: the game draws both by
    // scaling a single flat-colored pane to the fraction
    // (dMeter2Draw_c::drawKantera/drawOxygen — a J2D pane resize(), no
    // per-step textures at all), and the companion dashboard already
    // reimplements both natively with fillRect in drawMeterBar()
    // (companion.cpp), having found the game's own panes unusable. The
    // phone just draws a rectangle.
    u8 oilPct = 0;
    u8 oxygenPct = 0;
    // Whether each gauge should be shown at all, mirroring drawMeterBar()'s
    // own gates so the phone's native bar appears and disappears in step
    // with the streamed dashboard's rather than on its own rules. Oxygen
    // takes priority over oil: the game shares ONE set of panes between
    // them, so they are mutually exclusive on the real HUD too.
    bool oilVisible = false;
    bool oxygenVisible = false;

    bool operator==(const HudState&) const = default;
};

// Fills `out` and returns true if the companion HUD is ready to report state
// (mirrors the hudReady() gate the rest of the companion module already
// uses before touching game state); returns false and leaves `out`
// unmodified otherwise. Pure read: no drawing, no side effects, safe to
// call every frame.
bool gatherHudState(HudState& out);

// The companion page currently on screen, as a plain int. Exported so
// dualscreen.cpp can key the phone's page-ownership handshake on it without
// pulling in the companion module's internal header.
int currentPage();

// The highest `seq` of a phone ACTION message this build has taken off the
// queue and run to a decision (applied OR refused). Streamed back as
// hud_state's "ack" so the phone can retire its optimistic prediction.
//
// A refusal is invisible in the state itself — a denied equip simply leaves
// the savedata alone — so "ack moved past my seq and nothing changed" IS the
// refusal notice. That only works if hud_state is allowed to go out when
// nothing but the ack changed, which is why pollAndPushSpikeState()'s
// suppress-if-identical test carries the ack as well as the struct.
u32 lastAppliedActionSeq();


// --- Chrome: the tab strip, and the layout the strip belongs to ------------
//
// Everything a phone needs to draw the main tab bar itself, none of which it
// can derive: which pages the CURRENT layout shows (Functional drops GUIDE
// and centres MAP; Cinematic shows all four — companion.cpp's
// visiblePages()), what they are CALLED in the language the game is running
// in, and which one is lit.
//
// The geometry deliberately does NOT travel here. drawTabs() states the rule
// it is the only implementation of ("Nothing else may recompute this
// geometry"), and the phone scaling one page list across its own canvas is
// not a second copy of that arithmetic the way a bundled tab table would be.

constexpr int kMaxTabs = 4;  // PAGE_COUNT; Cinematic shows every page

struct TabState {
    u8 page = 0;  // PAGE_MAP / PAGE_INVENTORY / PAGE_COLLECTION / PAGE_GUIDE

    // The tab's label TEXT, already resolved for the running language.
    //
    // These are not the phone's to own. In English they are the dashboard's
    // own words ("MAP", "ITEMS", "COLLECTION", "GUIDE"); in every other
    // language tabName() hands back a GAME ARCHIVE string (msg 0x0062,
    // 0x0061, 0x03E1 — companion_hud.cpp), which the APK may not ship. So the
    // resolved string travels on the wire and the phone caches it for the
    // session, exactly like an item icon.
    //
    // Fixed width rather than a pointer so the whole-struct diff below can
    // compare it; strncpy zero-fills the tail, so two equal labels always
    // compare equal byte for byte.
    char label[32] = {};

    // False while `label` is still the English fallback because the archive
    // was not resident when it was asked for. archiveText() retries 8 times
    // 20 draws apart and then latches the fallback for the session
    // (companion_gfx.cpp), so the phone must not treat an unresolved label as
    // final — and must not cache it as if it were. A label that resolves
    // later changes this struct, so the correction goes out on its own.
    bool labelResolved = false;

    bool operator==(const TabState&) const = default;
};

struct ChromeState {
    // Functional ("3DS style") vs Cinematic. Not a skin of each other: the
    // visible tab set, the corner boxes and the item buttons all differ, and
    // a mode change reshapes the entire surface — so the phone has to know
    // which one it is drawing before it draws anything.
    bool functional = false;

    // The page whose tab is lit. Same value hud_state carries; repeated here
    // so a phone that renders only the tab strip needs one message, and
    // because `guideOpen` below is only meaningful next to it.
    u8 page = 0;

    // The guide reader is an OVERLAY, not a Page, so while it is up NO tab is
    // the thing being shown and drawTabs lights none of them
    // (companion_hud.cpp's `pages[i] == page && !guideIsOpen()`). Without
    // this bit the phone would leave the page's tab lit behind the reader.
    bool guideOpen = false;

    u8 tabCount = 0;
    TabState tabs[kMaxTabs];

    // The Functional left column's context tab, RESOLVED. ContextTabAction in
    // companion_internal.h: 0 NONE, 1 WARP, 2 FLOOR, 3 INFO, 4 HOME, 5 BACK —
    // use contextActionName() rather than hardcoding the numbers.
    //
    // Never let the phone re-derive this. contextTabAction() reaches
    // warpAllowed(), which walks the stage info and Link's own
    // checkAcceptWarp(), and its ITEMS branch is clickable only while an
    // inventory cell is selected — which is precisely the coupling a phone
    // that owned the grid silently would break.
    u8 contextAction = 0;
    bool contextClickable = false;

    bool operator==(const ChromeState&) const = default;
};

// Stable wire name for a ContextTabAction value ("none", "warp", "floor",
// "info", "home", "back"). Lives here so the JSON layer never has to include
// the companion module's internal header just to name an enumerator.
const char* contextActionName(u8 action);

// Fills `out`; same hudReady() gate and same "false means nothing to report"
// contract as gatherHudState(). Pure read.
bool gatherChromeState(ChromeState& out);


// --- Inventory: the ITEMS grid ---------------------------------------------
//
// The grid's CONTENTS, not its geometry. The two cell tables (5x5 and 6x4,
// picked by canvas aspect) are compile-time data with static_asserts already
// proving they agree, so the phone holds its own copy of the positions and
// chooses its own layout for its own aspect; what it cannot know is what is
// IN each cell.
//
// Diffed as a whole array rather than pushed from the equip paths, and
// deliberately so: the inventory changes under the player's feet in places
// nothing in this module can hook. Emptying a bomb bag auto-unequips bomb
// arrows (d_meter2.cpp's moveBombNum) and hot-spring water cools on a timer
// that rewrites slots 11-14 (d_meter2_info.cpp). A phone told only about
// equips would show both of those as stale cells indefinitely.

constexpr int kInvCells = 23;

struct InvCellState {
    u8 slot = 0;  // inventory slot index (0..MAX_ITEM_SLOTS-1), the cell's identity

    // dItemNo_NONE_e (0xFF) IS the empty cell — the grid still draws an empty
    // cell's tinted plate, it just has no icon, so "absent" and "empty" are
    // different states and the phone needs the cell either way.
    u8 itemNo = 0xFF;

    // Quantity chip, resolved the way the GRID resolves it — ammoForItem with
    // the inventory slot, not with a button index. The grid and the item
    // buttons legitimately disagree here (a bomb bag in the grid counts its
    // own bag; the same bombs on a button count that button's selection), so
    // both variants travel: this one, and InvState::buttonAmmo below.
    // -1 means this item shows no quantity at all.
    s16 ammo = -1;

    // Group tint index 0..4 (tools / bottles / bombs / quest / rod). A
    // property of the SLOT, identical in both cell tables, so it is redundant
    // with the phone's own table — sent anyway so the cell list is
    // self-describing and a first phone build can paint the grid with no
    // generated constants at all.
    u8 group = 0;

    bool operator==(const InvCellState&) const = default;
};

struct InvState {
    // Which cell table the PC picked for ITS canvas (6x4 at aspect >= 1.7,
    // 5x5 otherwise). Informational for the phone, which picks for its own
    // aspect: with a native second screen attached the two canvases are not
    // the same shape at all.
    bool wide = false;

    // The selected cell, as a SLOT index and as an index into cells[] below
    // (-1 = nothing selected). Both, because the slot is the identity the
    // equip verbs use and the cell index is what the phone highlights.
    //
    // This is the field it is easiest to think of as purely the phone's. It
    // is not: inEquipMode() reads it (companion.cpp), and so does the context
    // tab's INFO clickability. If the phone selects a cell without telling
    // the PC, the PC never enters equip mode, the drop rects never publish,
    // and a drag-equip has nothing to land on.
    s8 selSlot = -1;
    s8 selCell = -1;

    // Ammo as the four item BUTTONS resolve it (index 0=X, 1=Y, 2=I, 3=II),
    // for the same item hud_state reports for that button. -1 = no chip.
    s16 buttonAmmo[4] = {-1, -1, -1, -1};

    u8 cellCount = 0;
    InvCellState cells[kInvCells];

    bool operator==(const InvState&) const = default;
};

// Fills `out`; same contract as gatherHudState(). Pure read — in particular
// it reads the compile-time cell TABLE, never s_invCells, which only exists
// after the ITEMS page has drawn and is therefore unavailable on every frame
// the page is not on screen.
bool gatherInvState(InvState& out);


// Phase-3 addition: hearts. Unlike items, heart container art isn't a
// static archive texture loadable by identity — it's live, currently-
// animating J2DPicture objects owned by the game's own HUD meter
// (dMeter2Draw_c), so there's no "load state N" call to make. Instead this
// opportunistically probes the 20 live heart slots (dMeter2Draw_c::
// getHeartState(), a new small read-only accessor added alongside the
// existing getHeartPictures() in include/d/d_meter2_draw.h) for one
// CURRENTLY showing `wantedState` (0=empty, 1/2/3=quarter/half/three-
// quarter, 4=full — see getHeartState()'s own doc comment for exactly how
// these map to the game's internal quarter-texture tags), and if found,
// draws that slot's live picture(s) into the CURRENT 2D render context
// (same flat-clear + small-fixed-size treatment as
// a flat background clear and a small fixed draw size) — never mutates
// the live panes' visibility/texture, only reads their current state and
// draws (with the pane's transform matrix saved/restored around the draw,
// same as companion_hud.cpp's drawHeartsRow() already does for the exact
// same reason: draw() clobbers the pane's own position matrix, and this is
// the GAME's shared pane, not something owned by this feature).
//
// Returns false (draws nothing, sends nothing) if no live slot currently
// shows the wanted state — a player who hasn't taken graduated damage may
// not have a heart showing e.g. state 2 (half) available yet. The caller
// (dualscreen.cpp) keeps the request pending and retries on a later frame
// rather than treating this as a failure.
bool drawWantedHeartIcon(u8 wantedState, f32 canvasW, f32 canvasH);

}  // namespace dusk::companion
