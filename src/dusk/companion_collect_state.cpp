#include "dusk/companion_collect_state.h"

#if DUSK_PHONE_SPIKE_STATE

#include "dusk/companion.h"
#include "dusk/companion_internal.h"

#include "d/d_com_inf_game.h"
#include "d/d_item_data.h"
#include "d/d_menu_collect.h"
#include "d/d_meter2_info.h"
#include "dusk/version.hpp"

#include <cstring>

namespace dusk::companion {

namespace {

// --- Bounded interning -----------------------------------------------------
//
// Every string below costs a linear scan of the whole .bmg, and this gather
// runs once per frame whether or not the COLLECT page is even on screen — so
// nothing here may walk the archive twice for the same message ID.
//
// A cache of its OWN rather than companion_gfx.cpp's archiveText(): that one
// is shared with the draw, and its retry budget is counted in CALLS. Adding a
// per-frame second caller to it would halve the wall-clock window an
// unresolved string gets before the English fallback latches for the session
// — i.e. the state path would degrade the dashboard's own text. The two
// budgets are deliberately independent; the numbers (8 tries, 20 polls apart,
// ~2.7s at 60Hz) are copied from it because the reason for them is the same:
// a poll can land while the archive is not resident, and latching the first
// miss leaves a name blank forever.
constexpr int kInternMax = 32;  // 6 fish + 7 ordinals + 7 skill names, with headroom
constexpr u8 kFetchTries = 8;
constexpr u8 kRetryGap = 20;

struct InternedText {
    u32 msgId;
    char text[64];
    u8 tries;
    u8 wait;
};
InternedText s_intern[kInternMax];
int s_internCount = 0;

// The archive string for `msgId`, or NULL while it is still unresolved — the
// caller publishes its own placeholder and clears the row's "resolved" bit,
// which is what stops the phone caching a name that has not arrived yet.
const char* internText(u32 msgId) {
    if (msgId == 0) {
        return NULL;
    }
    int slot = -1;
    for (int i = 0; i < s_internCount; i++) {
        if (s_intern[i].msgId == msgId) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        if (s_internCount >= kInternMax) {
            return NULL;
        }
        slot = s_internCount++;
        s_intern[slot].msgId = msgId;
        s_intern[slot].text[0] = 0;
        s_intern[slot].tries = 0;
        s_intern[slot].wait = 0;
    }
    InternedText& entry = s_intern[slot];
    if (entry.text[0] != 0) {
        return entry.text;  // resolved once, pointer-free from here on
    }
    if (entry.tries >= kFetchTries) {
        return NULL;  // budget spent; this ID genuinely has no entry
    }
    if (entry.wait > 0) {
        entry.wait--;
        return NULL;
    }
    entry.wait = kRetryGap;
    entry.tries++;
    dMeter2Info_getStringFull(msgId, entry.text, sizeof(entry.text));
    // Latch on SUCCESS, never on attempt: getStringFull hands back an EMPTY
    // buffer while the message resource is not resident, which it routinely
    // is not for the first frames after a stage load.
    return entry.text[0] != 0 ? entry.text : NULL;
}

// strncpy with an explicit terminator, not a length-checked copy: strncpy
// zero-FILLS the tail, and the whole-struct diff compares those bytes. A copy
// that only terminated would leave whatever a previous, longer string wrote
// past the NUL, and the diff would report a change that is not one — which on
// this struct means resending five kilobytes every frame.
void copyText(char* dst, int cap, const char* src) {
    std::strncpy(dst, src == NULL ? "" : src, (size_t)cap - 1);
    dst[cap - 1] = 0;
}

// --- Letter subject/sender cache -------------------------------------------
//
// Keyed by the SAVEDATA letter index, like drawLettersContent's own cache and
// for the same reason (a message walk per letter is far too heavy per frame),
// but with its own storage so the state path and the draw path cannot spend
// each other's retries. Sized 64 — the streamed list caps at 32 rows, the
// INDEX space does not.
constexpr int kLetterSlots = 64;
char s_letterSubject[kLetterSlots][64];
char s_letterSender[kLetterSlots][40];
bool s_letterCached[kLetterSlots];
u8 s_letterTries[kLetterSlots];
u8 s_letterWait[kLetterSlots];
u32 s_letterCacheGen = 0;
bool s_letterCacheInit = false;

// Fills the two strings for one letter. Returns whether the SUBJECT came back
// real — the sender is legitimately empty for some letters, so it is not part
// of the verdict.
bool letterText(int index, const char*& outSubject, const char*& outSender) {
    if (index < 0 || index >= kLetterSlots) {
        outSubject = "";
        outSender = "";
        return false;
    }
    // Dropped on the same hook the draw's cache uses: the letter store is
    // keyed by index alone, so file A's subjects happily survive into file B
    // whenever the two hold a letter at the same index. Keyed on the
    // generation only, never on the letter COUNT the draw also watches — a
    // new letter arrives at an index nothing has cached yet, so a count
    // change is not a reason to throw away 63 resolved strings.
    if (!s_letterCacheInit || s_letterCacheGen != s_lettersCacheGen) {
        s_letterCacheInit = true;
        s_letterCacheGen = s_lettersCacheGen;
        std::memset(s_letterCached, 0, sizeof(s_letterCached));
        std::memset(s_letterTries, 0, sizeof(s_letterTries));
        std::memset(s_letterWait, 0, sizeof(s_letterWait));
        std::memset(s_letterSubject, 0, sizeof(s_letterSubject));
        std::memset(s_letterSender, 0, sizeof(s_letterSender));
    }
    if (!s_letterCached[index]) {
        if (s_letterWait[index] > 0) {
            s_letterWait[index]--;
        } else {
            s_letterSubject[index][0] = 0;
            s_letterSender[index][0] = 0;
            dMeter2Info_getStringFull(dMenu_Letter::getLetterSubject(index),
                s_letterSubject[index], sizeof(s_letterSubject[0]));
            dMeter2Info_getStringFull(dMenu_Letter::getLetterName(index),
                s_letterSender[index], sizeof(s_letterSender[0]));
            if (s_letterSubject[index][0] != 0 || ++s_letterTries[index] >= kFetchTries) {
                s_letterCached[index] = true;  // got it, or gave up
            } else {
                s_letterWait[index] = kRetryGap;
            }
        }
    }
    outSubject = s_letterSubject[index];
    outSender = s_letterSender[index];
    return s_letterSubject[index][0] != 0;
}

}  // namespace

bool gatherCollectState(CollectState& out) {
    if (!hudReady()) {
        return false;
    }

    out.tab = (u8)s_collectTab.load();
    // Both are the pause menu's own identities rather than row numbers: on the
    // mail tab they hold the SAVEDATA letter index (drawLettersList compares
    // them against `li`, not against `i`), which is exactly what
    // CollectLetterState::index publishes and what a body fetch is keyed on.
    out.sel = (s8)s_collectSel;
    out.readerSel = (s8)s_readerSel;

    // --- overview ----------------------------------------------------------
    const u8 curSword = dComIfGs_getSelectEquipSword();
    const u8 curShield = dComIfGs_getSelectEquipShield();
    const u8 curClothes = dComIfGs_getSelectEquipClothes();
    for (int i = 0; i < kCollectGearSlots; i++) {
        // gearItemFor(), not a table: box 1 is the Master sword until the
        // Light sword exists and box 2 is the Wooden shield only while the
        // Ordon one does not, so the box's identity is a live decision.
        const u8 itemNo = gearItemFor(i);
        out.gear[i].itemNo = itemNo;
        out.gear[i].owned = dComIfGs_isItemFirstBit(itemNo) != 0;
        out.gear[i].worn = itemNo == curSword || itemNo == curShield || itemNo == curClothes;
    }

    out.heartPieces = (u8)(dComIfGs_getMaxLife() % 5);

    const int arrowMax = dComIfGs_getArrowMax();
    out.arrowMax = (u16)arrowMax;
    out.hasBow = dComIfGs_isItemFirstBit(dItemNo_BOW_e) != 0;
    // Same tier split drawCounterGrid uses, published as the archive resource
    // index so the phone hands it straight to icon_request's "raw_icon" kind.
    out.quiverIcon =
        (u16)(RAWICON_YADUTU1 + (arrowMax >= 100 ? 2 : arrowMax >= 60 ? 1 : 0));

    out.bugBase = (u8)dItemNo_M_BEETLE_e;
    u32 bugBits = 0;
    u8 bugsHeld = 0;
    for (int i = 0; i < kCollectBugs; i++) {
        if (dComIfGs_isItemFirstBit((u8)(dItemNo_M_BEETLE_e + i))) {
            bugBits |= 1u << i;
            bugsHeld++;
        }
    }
    out.bugBits = bugBits;
    out.bugsHeld = bugsHeld;

    // The record unit is the GAME's rule, not the phone's: inches for PAL
    // English, centimetres everywhere else (d_menu_fishing.cpp:152). Resolving
    // it here is the only place it can be resolved — it needs the disc region
    // and the PAL language byte.
    const bool inches = dusk::version::isRegionPal() &&
        dComIfGs_getPalLanguage() == dSv_player_config_c::LANGUAGE_ENGLISH;
    out.recordInches = inches;
    u8 species = 0;
    for (int i = 0; i < kCollectFish; i++) {
        const u16 caught = dComIfGs_getFishNum((u8)i);
        out.fish[i].caught = caught;
        const s32 size = (s32)dComIfGs_getFishSize((u8)i);
        out.fish[i].record = caught > 0 ? (inches ? (s32)((f32)size / 2.54f) : size) : 0;
        const char* name = internText(collectFishNameMsg(i));
        copyText(out.fish[i].name, sizeof(out.fish[i].name), name != NULL ? name : "-");
        out.fish[i].nameResolved = name != NULL;
        if (caught > 0) {
            species++;
        }
    }
    out.fishSpecies = species;

    u8 skillsLearned = 0;
    for (int i = 0; i < kCollectSkills; i++) {
        const bool learned = collectSkillLearned(i);
        out.skills[i].learned = learned;
        if (learned) {
            skillsLearned++;
        }
        // The ordinal travels for every row, learned or not: the list draws it
        // dimmed rather than hiding it. The English fallback goes out while
        // the archive has not answered, exactly like TabState::label — with
        // nameResolved false, so the phone redraws rather than caching it.
        const char* ordinal = internText(collectSkillOrdinalMsg(i));
        copyText(out.skills[i].ordinal, sizeof(out.skills[i].ordinal),
            ordinal != NULL ? ordinal : collectSkillOrdinalEnglish(i));
        bool resolved = ordinal != NULL;
        if (learned) {
            // Only once learned. The list draws "???" until then, and a phone
            // mirroring this page must not be the one place the technique
            // names leak out of an unfinished save.
            const char* name = internText(collectSkillNameMsg(i));
            copyText(out.skills[i].name, sizeof(out.skills[i].name), name);
            if (name == NULL) {
                resolved = false;
            }
        }
        out.skills[i].nameResolved = resolved;
    }
    out.skillsLearned = skillsLearned;

    out.poes = dComIfGs_getPohSpiritNum();
    out.poeItemNo = (u8)dItemNo_POU_SPIRIT_e;

    // --- mail ---------------------------------------------------------------
    const int total = (int)dMeter2Info_getRecieveLetterNum();
    out.letterTotal = (u8)(total > 255 ? 255 : total);
    int idxs[64];
    // The pause menu's own order (the receive table read backwards, with the
    // range check and the old-save fallback), borrowed rather than reproduced.
    const int listed = collectSortedLetters(idxs);
    out.letterCount = (u8)(listed > kMaxCollectLetters ? kMaxCollectLetters : listed);
    for (int i = 0; i < out.letterCount; i++) {
        const int index = idxs[i];
        out.letters[i].index = (u8)index;
        const char* subject = NULL;
        const char* sender = NULL;
        const bool resolved = letterText(index, subject, sender);
        copyText(out.letters[i].subject, sizeof(out.letters[i].subject), subject);
        copyText(out.letters[i].sender, sizeof(out.letters[i].sender), sender);
        out.letters[i].textResolved = resolved;
    }

    // --- scent --------------------------------------------------------------
    const u8 scent = dComIfGs_getCollectSmell();
    int scentSlot = -1;
    // resolveScent() answers both halves at once and interns the name itself,
    // so this is one shared archive lookup per poll — the same cost the wolf
    // HUD's own per-frame path already pays for it.
    const char* scentName = resolveScent(scent, &scentSlot);
    out.scent = scent;
    out.scentIconSlot = (s8)scentSlot;
    copyText(out.scentName, sizeof(out.scentName), scentName);

    // --- Fused Shadows / Mirror Shards ---------------------------------------
    const u8 maskMdl = dMenu_Collect3D_c::getMaskMdlVisible();
    const char* fsLabel;
    int fsHave = 0;
    int fsTotal = 3;
    if (maskMdl == 2) {
        fsLabel = localizedWord(0x020A, "Mirror Shards");
        fsHave = dMenu_Collect3D_c::getMirrorNum();
        fsTotal = 4;
    } else {
        // Fetched in the branch rather than before it, unlike the draw: only
        // the label actually shown may spend a tick of the shared archive
        // retry budget, and this runs every frame the page is NOT open too.
        fsLabel = localizedWord(0x05AA, "Fused Shadows");
        if (maskMdl == 1) {
            fsHave = dMenu_Collect3D_c::getCrystalNum();
        }
    }
    if (fsHave > fsTotal) {
        fsHave = fsTotal;  // the save tracks a 4th story crystal
    }
    out.maskMdl = maskMdl;
    out.fsHave = (u8)fsHave;
    out.fsTotal = (u8)fsTotal;
    copyText(out.fsLabel, sizeof(out.fsLabel), fsLabel);

    // --- equip feedback ------------------------------------------------------
    // The frame COUNTER deliberately stays behind (see the header): it ticks
    // every frame the page draws, so carrying it would fire this whole message
    // sixty times a second for the ~2s a message is up. Only the bit and the
    // text travel, and the text is cleared with the bit so an expired message
    // cannot linger in the diff as a byte-identical ghost.
    out.equipMsgVisible = s_equipMsgFrames > 0;
    if (out.equipMsgVisible) {
        copyText(out.equipMsg, sizeof(out.equipMsg), s_equipMsg);
    }
    return true;
}

bool collectBodyText(u8 kind, int id, char* outTitle, int titleCap, char* outCorner,
    int cornerCap, char* outBody, int bodyCap) {
    if (outTitle == NULL || outCorner == NULL || outBody == NULL || titleCap <= 0 ||
        cornerCap <= 0 || bodyCap <= 0)
    {
        return false;
    }
    // Fetched into locals and only copied out on success, so a caller's
    // buffers are genuinely left untouched when this returns false.
    char title[96];
    char corner[96];
    title[0] = 0;
    corner[0] = 0;
    if (kind == COLLECT_TEXT_SKILL) {
        if (id < 0 || id >= kCollectSkills) {
            return false;
        }
        // Same three strings drawReaderDetail() composes: the ordinal on the
        // left, the technique name on the right, the description below.
        const char* ordinal = internText(collectSkillOrdinalMsg(id));
        copyText(title, sizeof(title),
            ordinal != NULL ? ordinal : collectSkillOrdinalEnglish(id));
        dMeter2Info_getStringFull(collectSkillNameMsg(id), corner, sizeof(corner));
        dMeter2Info_getStringFull(collectSkillTextMsg(id), outBody, bodyCap);
    } else if (kind == COLLECT_TEXT_LETTER) {
        // The SAVEDATA index, not a row: letter_data is a flat 64-entry table
        // and the browse list's order is only a view onto it.
        if (id < 0 || id >= 64) {
            return false;
        }
        dMeter2Info_getStringFull(dMenu_Letter::getLetterSubject(id), title, sizeof(title));
        dMeter2Info_getStringFull(dMenu_Letter::getLetterName(id), corner, sizeof(corner));
        dMeter2Info_getStringFull(dMenu_Letter::getLetterText(id), outBody, bodyCap);
    } else {
        return false;
    }
    if (outBody[0] == 0) {
        // Not "an empty body" — "not resident yet", which is the normal answer
        // for the first frames after a stage load. Reported as not-ready so
        // the phone re-asks; a phone that cached an empty string for the
        // session would have a reader that never shows this entry again.
        outBody[0] = 0;
        return false;
    }
    copyText(outTitle, titleCap, title);
    copyText(outCorner, cornerCap, corner);
    return true;
}

}  // namespace dusk::companion

#endif  // DUSK_PHONE_SPIKE_STATE
