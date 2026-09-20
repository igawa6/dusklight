#pragma once

// Phase-5 addition to the phone-companion state-streaming path: the COLLECT
// page, decomposed the same way chrome_state/inv_state already are — a plain
// snapshot struct, one pure gather, whole-struct diff, sent only on change.
//
// What this page shows is spread across a dozen unrelated savedata accessors
// (d_save's fish/letter/poe counters, the three equipped-gear selections, the
// bug first-bits, seven event flags for the hidden skills, the collect-menu
// 3D model's own crystal/mirror counts), and the answers are then dressed in
// strings that only the game's message archive has. None of that is derivable
// on the phone, so all of it travels.
//
// TWO things deliberately do NOT travel here, and both are the difference
// between this message being rare and it being per-frame:
//
//   * The long bodies. A hidden skill's description and a letter's text run
//     to kilobytes each, they are only wanted for the ONE entry the reader
//     has open, and pulling them is a full message-archive walk. They move on
//     a request/response channel instead — collectBodyText() below, reached
//     from the wire's "text_request". Same shape as icon_request: ask for one,
//     get one, cache it for the session.
//
//   * s_equipMsgFrames. It is a countdown, decremented every frame the page
//     draws, so putting it in the struct would make the diff fire on every
//     single frame of the ~2s an equip message is up. Only the message TEXT
//     and a visible bit travel — which is all the phone can draw anyway. This
//     is the same trap HudState's oil/oxygen counters document.
//
// Gated by DUSK_PHONE_SPIKE_STATE (dualscreen.h) and off by default.

#include "dusk/dualscreen.h"

#if DUSK_PHONE_SPIKE_STATE

namespace dusk::companion {

// The three gear rows on the overview: 2 swords, 2 shields, 3 clothes. The
// item in a box is not fixed — gearItemFor() picks Master vs Light sword and
// Wooden vs Ordon shield from what the player owns — so the box's identity
// has to be published rather than tabled on the phone.
constexpr int kCollectGearSlots = 7;
constexpr int kCollectBugs = 24;
constexpr int kCollectFish = 6;
constexpr int kCollectSkills = 7;

// Letters the browse list carries. sortedLetters() bounds itself at 64
// because that is the width of the savedata's own order table; 32 is the cap
// on what is worth streaming, and the phone is told the true total separately
// (CollectState::letterTotal) so a store past the cap reads as "32 of 41
// shown" rather than silently as 32.
constexpr int kMaxCollectLetters = 32;

// One gear box.
struct CollectGearState {
    u8 itemNo = 0xFF;
    bool owned = false;  // dimmed to alpha 55 in the dashboard when false
    bool worn = false;   // bright-yellow plate, same as a selected item

    bool operator==(const CollectGearState&) const = default;
};

// One fish species row. Index is the SAVEDATA index (the one getFishNum
// takes), not the order the pause menu lists them in — the dashboard's own
// table had those two confused once and mislabelled three species.
struct CollectFishState {
    u16 caught = 0;
    // The record, already converted to the unit CollectState::recordInches
    // names. Converted here rather than on the phone because the rule is the
    // game's (inches for PAL English, centimetres everywhere else,
    // d_menu_fishing.cpp:152) and re-deriving it needs the disc region and the
    // PAL language byte, neither of which the phone has.
    s32 record = 0;
    char name[40] = {};
    // False while `name` is still the "-" placeholder because the archive was
    // not resident when it was asked for. Same contract as TabState::
    // labelResolved: do not cache an unresolved string, a real one may still
    // arrive and will arrive as its own diff.
    bool nameResolved = false;

    bool operator==(const CollectFishState&) const = default;
};

// One hidden skill row. The ordinal ("Skill One") and the technique name are
// separate strings in the archive and the list draws them in separate
// columns, so they stay separate here.
struct CollectSkillState {
    bool learned = false;
    char ordinal[40] = {};
    char name[48] = {};
    bool nameResolved = false;

    bool operator==(const CollectSkillState&) const = default;
};

// One received letter, in the pause menu's own newest-first order.
struct CollectLetterState {
    // The SAVEDATA letter index, not the row number. It is the identity the
    // body fetch is keyed on, and it survives a new letter arriving and
    // pushing every row down by one — a row number would not.
    u8 index = 0;
    char subject[64] = {};
    char sender[40] = {};
    bool textResolved = false;

    bool operator==(const CollectLetterState&) const = default;
};

struct CollectState {
    // Which view the page is on: 0 overview, 1 bugs, 2 fish, 3 skills,
    // 4 mail — s_collectTab's own numbering, shared with the collect_tab
    // action so the two can never disagree about what "3" means.
    u8 tab = 0;
    // Selected list row (s_collectSel) and the entry whose reader is open
    // (s_readerSel), both -1 for none. Like InvState::selSlot these look like
    // purely the phone's business and are not: the context tab's Read action
    // is clickable only while a row is selected, and the PC's own second
    // screen has to show the same selection the phone is showing.
    s8 sel = -1;
    s8 readerSel = -1;

    // --- overview ---------------------------------------------------------
    CollectGearState gear[kCollectGearSlots];

    // Heart pieces toward the next container, 0..4. getMaxLife() % 5 — the
    // remainder IS the piece count, which is why no piece counter exists in
    // the save.
    u8 heartPieces = 0;

    u16 arrowMax = 0;
    bool hasBow = false;
    // Which quiver texture the overview shows, as a RAW ItemIcon-archive
    // resource index (RAWICON_YADUTU1 + 0/1/2 for small/big/giant). Published
    // as the index rather than as a tier so the phone can hand it straight to
    // icon_request's "raw" kind with nothing to look up.
    u16 quiverIcon = 0;

    u8 bugsHeld = 0;
    // One bit per golden bug, bit i = bugBase + i. A bitmask, not 24 bools: it
    // is 24 independent first-bits that only ever change one at a time, and
    // packing them keeps the whole-struct compare to one word.
    u32 bugBits = 0;
    // dItemNo_M_BEETLE_e. A compile-time constant, carried anyway for the same
    // reason InvCellState::group is: it is the one number that turns bugBits
    // from an opaque mask into 24 item numbers the phone can already fetch art
    // for, and sending it means a first phone build needs no generated item
    // table at all. Constant, so it never contributes to the diff.
    u8 bugBase = 0;

    u8 fishSpecies = 0;  // species with at least one catch
    CollectFishState fish[kCollectFish];
    bool recordInches = false;

    u8 skillsLearned = 0;
    CollectSkillState skills[kCollectSkills];

    u16 poes = 0;
    // dItemNo_POU_SPIRIT_e, the icon the poe counter cell draws. Same reason
    // as bugBase: the count alone does not say what to draw beside it.
    u8 poeItemNo = 0xFF;

    // Letters received in total, and how many of them are listed below. The
    // two differ only when the store runs past kMaxCollectLetters.
    u8 letterTotal = 0;
    u8 letterCount = 0;
    CollectLetterState letters[kMaxCollectLetters];

    // --- scent ------------------------------------------------------------
    // The scent Link is currently tracking as an ITEM number (0xFF = none),
    // plus which collect-archive slot draws it (-1 = none, fetch it with
    // icon_request's "clct" kind). Both, because the icon and the name come
    // from different places: resolveScent() maps five scents onto five
    // archive slots but two of them (pumpkin and Ilia's pouch) share one slot
    // while carrying different names.
    u8 scent = 0xFF;
    s8 scentIconSlot = -1;
    char scentName[40] = {};

    // --- Fused Shadows / Mirror Shards -----------------------------------
    // 0 = neither collected yet (the panel is dimmed and shows "-"),
    // 1 = Fused Shadows, 2 = Mirror Shards. Comes from the collection menu's
    // own 3D model visibility, which is the game's own answer to "which of
    // the two is this save on" — there is no savedata flag that says it.
    u8 maskMdl = 0;
    u8 fsHave = 0;
    u8 fsTotal = 3;
    char fsLabel[40] = {};

    // --- equip feedback ---------------------------------------------------
    // The "can't equip now" / "Equipped X" line under the overview. Text and
    // a visible bit only — see the header comment for why the frame counter
    // itself must never come along.
    bool equipMsgVisible = false;
    char equipMsg[64] = {};

    bool operator==(const CollectState&) const = default;
};

// Fills `out`; same hudReady() gate and same "false means nothing to report"
// contract as gatherHudState(). Pure read — no drawing, no savedata writes.
//
// Not quite free, and the cost is worth knowing: the strings above come from
// the message archive, which is a walk per string, so this interns each one
// on first success behind its own bounded retry budget (8 tries, 20 polls
// apart) exactly the way the draw path's own caches do. Once resolved it is
// pointer-free reads and a memcmp.
bool gatherCollectState(CollectState& out);

// --- Long bodies: the request/response half --------------------------------

// What collectBodyText() will fetch. Matches the reader's own two tabs.
enum CollectTextKind : u8 {
    COLLECT_TEXT_SKILL = 0,   // id = 0..6, the hidden-skill display order
    COLLECT_TEXT_LETTER = 1,  // id = the SAVEDATA letter index, as published
                              // in CollectLetterState::index
};

// Fills the reader's three strings for one entry. Returns false — leaving the
// buffers untouched — when the id is out of range OR when the message archive
// handed back an empty body, which happens routinely for the first frames
// after a stage load.
//
// That second case is the load-bearing one: it must be reported as "not ready"
// rather than as an empty body, because the phone caches what it gets and an
// empty string cached for the session is a reader that never shows anything
// again. The wire answers it with ready:false and the phone re-asks.
//
// GAME THREAD ONLY: dMeter2Info_getStringFull walks the resident message
// archive.
bool collectBodyText(u8 kind, int id, char* outTitle, int titleCap, char* outCorner,
    int cornerCap, char* outBody, int bodyCap);

}  // namespace dusk::companion

#endif  // DUSK_PHONE_SPIKE_STATE
