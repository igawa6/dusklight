#pragma once

// Companion-only UI strings: the ones with NO equivalent in the game's
// message archive (everything that does have one is fetched with
// localizedWord() instead — see companion_internal.h).
//
// IMPORTANT: the companion draws through the game's message font, which is
// LATIN-1 and treats every byte as one glyph. Entries below are therefore
// written as \xNN escapes, NOT UTF-8 — a raw UTF-8 literal would render as
// two garbage glyphs per accented character and mis-measure its width.

namespace dusk::companion {

enum StringId {
    STR_RESET,
    STR_SPECIES,
    STR_SCENT,
    STR_WARP,
    STR_NO_MAP,
    STR_LOADING_MAP,
    STR_LOADING_FLOOR,
    STR_MAP_ON_MAIN,
    STR_DRAG_EQUIP,
    STR_COMBINE_BOW,
    STR_BOMB_ARROWS,
    STR_REPLACE,
    STR_NO_LETTERS,
    STR_COMBO_ON,
    STR_NO_EQUIP_MENU,
    STR_NO_EQUIP_WOLF,
    STR_NO_SLOT,
    STR_SLOT_CLEARED,
    STR_SLOT_BOUND,
    STR_NO_GEAR_MENU,
    STR_NO_GEAR_WOLF,
    STR_EQUIPPED,
    STR_GEAR,
    STR_NO_WARP_HERE,
    STR_COUNT,
};

// Returns the entry for the current game language (OSGetLanguage), falling
// back to English for any language whose entry is empty.
const char* txt(StringId id);

}  // namespace dusk::companion
