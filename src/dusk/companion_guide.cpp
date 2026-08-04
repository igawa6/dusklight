#include "dusk/companion_internal.h"
#include "dusk/companion_strings.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include "JSystem/JUtility/JUTFont.h"

#include <cstring>
#include <vector>

#include "dusk/dualscreen.h"
#include "dusk/settings.h"

#include "dusk/guide/fetch.hpp"
#include "dusk/guide/image.hpp"
#include "dusk/guide/store.hpp"

// Guide reader: an OVERLAY inside the content window, not a Page.
//
// Not a Page on purpose. drawDashboard force-resets s_page to PAGE_MAP for any
// page missing from visiblePages(), so a tab-less page is impossible; and
// l_tabNames/l_msg are [PAGE_COUNT] arrays with four initialisers, so adding an
// enumerator silently hands NULL to localizedWord. Drawing over the window
// sidesteps both, changes no tab layout, and leaves the tabs, the I/II slots,
// the corner buttons and the top bar live underneath — which is the point: the
// HUD keeps working while you read.

namespace dusk::companion {

// --- state ------------------------------------------------------------------

bool s_guideOpen = false;
f32 s_guideT = 0.0f;        // open/close ramp, drives the fade
f32 s_scrollGuide = 0.0f;
int s_guideSection = -1;    // -1 = section list, >= 0 = reading that section
f32 s_guidePrevRect[4] = {0.0f, 0.0f, 0.0f, 0.0f};
f32 s_guideNextRect[4] = {0.0f, 0.0f, 0.0f, 0.0f};
// Wii U only: the reader is a PAGE there and covers the context tab, so its
// "back to the list" control has to live inside the panel.
f32 s_guideListRect[4] = {0.0f, 0.0f, 0.0f, 0.0f};

dusk::guide::Document s_doc;
bool s_docLoaded = false;
std::string s_docId;

// Every section of every saved guide, flattened.
//
// A multi-page walkthrough imports as ONE GUIDE PER CHAPTER, and the reader
// used to open only entries[0] — so 23 saved chapters showed up as whichever
// two sections happened to be first. index.json already carries each guide's
// section list, so the browse list costs one small file read, not 23.
struct BrowseRow {
    std::string guideId;
    std::string label;
    int section;      // -1 for a chapter header row
    int guideIndex;   // which guide this row belongs to
    // Chapter/section as the SITE numbers them ("13.5" -> 13, 5), which is
    // what the position marker matches against. 0 when the row is unnumbered.
    int chapterNo = 0;
    int sectionNo = 0;
};
std::vector<BrowseRow> s_browse;
// Which chapter is expanded, or -1 for none. One at a time: 23 chapters at
// ~8 sections each is 200 rows, and the whole point of grouping is that you
// scroll chapters, not sections.
int s_expanded = -1;

// Cached: the left column asks every frame, and load_index() reads JSON off
// disk. Refreshed whenever the browse list is rebuilt, which is the only way
// the answer can change.
bool s_guideAvailable = false;

// Chapter headers always; their sections only while that chapter is open.
// Chapter number a guide belongs to, read from the first of its sections that
// is numbered ("13.1 First Poe Soul" -> 13). Guides carry no number of their
// own and the index is in import order, which is whatever order the pages were
// saved in -- so without this the list ran 17, 9, 16, 8, 3, 13, ...
//
// The "is it numbered" test is dusk::guide::section_number, NOT a second copy:
// the store names that section's images with the same prefix, and a list that
// disagreed with the folder about which sections are numbered would be worse
// than either behaviour on its own.
int chapterNumberOf(const dusk::guide::IndexEntry& e) {
    for (const auto& sec : e.sections) {
        const std::string num = dusk::guide::section_number(sec.title);
        if (!num.empty()) {
            return std::atoi(num.c_str());
        }
    }
    return 9999;  // unnumbered: keep it after everything numbered
}


// --- Where the player is, mapped onto the guide -----------------------------
//
// The marker only ever POINTS; it never moves the reader. A guide is whatever
// the player downloaded, so every step here degrades to "no marker" rather
// than to a wrong one.
//
// Chapter comes from the stage code. EVERY entry below is confirmed, by one
// of two means:
//   * booted on the headless rig and read off the game's own place label
//     (D_MN01, D_MN05, D_MN06, D_MN10, D_MN11, F_SP103)
//   * named outright in the decomp's own comments (the rest)
// Nothing here is a guess. Cave of Ordeals is deliberately ABSENT: D_MN54
// looked like it, but d_a_door_bossL1.cpp lists it as having a boss door and
// d_s_menu.cpp uses it in item-clearing debug setup, so it is something else.
// An unlisted stage simply has no marker, which is also the right answer for
// a shop, a grotto or a cutscene stage.
struct StageChapter {
    const char* stage;  // prefix match: D_MN05, D_MN05A and D_MN05B are one dungeon
    int chapter;
};
constexpr StageChapter kStageChapters[] = {
    {"F_SP104", 1},   // Ordon Spring          (comment-confirmed)
    {"F_SP103", 1},   // Ordon Village        (rig-confirmed)
    {"R_SP107", 2},   // Hyrule Castle Sewers  (comment-confirmed)
    {"F_SP108", 3},   // Faron Woods           (comment-confirmed)
    {"D_MN05", 4},    // Forest Temple        (rig-confirmed)
    {"F_SP121", 5},   // Hyrule Field          (comment-confirmed)
    {"F_SP109", 5},   // Kakariko Village      (comment-confirmed)
    {"D_SB10", 5},    // Kakariko interiors    (comment-confirmed)
    {"D_MN04", 7},    // Goron Mines          (comment-confirmed)
    {"F_SP115", 9},   // Lake Hylia            (comment-confirmed)
    {"D_MN01", 10},   // Lakebed Temple       (rig-confirmed)
    {"F_SP117", 11},  // Sacred Grove          (comment-confirmed)
    {"F_SP118", 12},  // Bulblin Camp          (comment-confirmed)
    {"D_MN10", 13},   // Arbiter's Grounds    (rig-confirmed)
    {"D_MN11", 15},   // Snowpeak Ruins       (rig-confirmed)
    {"D_MN06", 17},   // Temple of Time       (rig-confirmed)
    {"D_MN07", 19},   // City in the Sky       (comment-confirmed)
    {"D_MN08", 20},   // Palace of Twilight   (D_MN08D = Zant, comment-confirmed)
    {"D_MN09", 22},   // Hyrule Castle        (comment-confirmed)
};

// True for the chapters that are a dungeon, where the item ordinal below is
// meaningful. Overworld chapters get a chapter-level marker only.
bool chapterIsDungeon(int chapter) {
    switch (chapter) {
    case 4: case 7: case 10: case 13: case 15: case 17: case 19: case 20: case 22:  // 21 absent: see kStageChapters
        return true;
    default:
        return false;
    }
}

int currentChapter() {
    const char* stage = dComIfGp_getStartStageName();
    if (stage == NULL) {
        return 0;
    }
    for (const StageChapter& e : kStageChapters) {
        const std::size_t n = std::strlen(e.stage);
        if (std::strncmp(stage, e.stage, n) == 0) {
            // Guard against D_MN0 matching D_MN01 and D_MN04 alike: the next
            // character must end the code or be a room suffix letter.
            const char c = stage[n];
            if (c == '\0' || (c >= 'A' && c <= 'Z')) {
                return e.chapter;
            }
        }
    }
    return 0;
}

// Section within a dungeon, by how many of its keyed items you hold. The site
// names dungeon sections after exactly these ("17.1 Dungeon Map", "17.2 The
// Compass", "13.6 The Big Key"), so the count IS the section you are working
// on. Ordinal rather than title-matched: the reader must not depend on the
// wording of a page it did not write.
int currentDungeonSection() {
    int n = 1;
    if (dComIfGs_isDungeonItemMap()) {
        n++;
    }
    if (dComIfGs_isDungeonItemCompass()) {
        n++;
    }
    if (dComIfGs_isDungeonItemBossKey()) {
        n++;
    }
    return n;
}

// Section-completion flags, for the chapters that are not dungeons.
//
// A section counts as DONE when its flag is set, so the marker sits on the
// first section of the chapter whose flag is NOT yet set. That ordering is the
// whole design: it needs no per-section exactness, only that the flags fire in
// the same order the sections are written, and it degrades sensibly for a
// player who skipped optional content.
//
// MUST stay sorted by (chapter, section) — the search below takes the first
// unset entry it meets. Entries are added only once confirmed; an unmapped
// chapter simply gets a chapter-level marker, which is what shipped in 2.5.
// What marks a section complete. Event bits cover the overworld; dungeons are
// better served by their own items, because the site does NOT order dungeon
// sections map-compass-key. Forest Temple runs map, three monkeys, boomerang,
// compass, big key — so counting items held put the marker four sections early.
enum class SectionSignal : u8 {
    EventBit,        // value = dSv_event_flag_c::X
    DungeonMap,      // value unused
    DungeonCompass,  // value unused
    DungeonBossKey,  // value unused
    ItemFirstBit,    // value = dItemNo_X_e
};

struct SectionFlag {
    int chapter;
    int section;
    SectionSignal kind;
    u16 value;
};

bool sectionDone(const SectionFlag& e) {
    switch (e.kind) {
    case SectionSignal::EventBit:
        return dComIfGs_isEventBit(e.value) != 0;
    case SectionSignal::DungeonMap:
        return dComIfGs_isDungeonItemMap() != 0;
    case SectionSignal::DungeonCompass:
        return dComIfGs_isDungeonItemCompass() != 0;
    case SectionSignal::DungeonBossKey:
        return dComIfGs_isDungeonItemBossKey() != 0;
    case SectionSignal::ItemFirstBit:
        return dComIfGs_isItemFirstBit((u8)e.value) != 0;
    default:
        return false;
    }
}
constexpr SectionFlag kSectionFlags[] = {
    // Chapter 1 - Ordon Village
    {1, 1, SectionSignal::EventBit, dSv_event_flag_c::F_0019},  // Spoke with Ilia at the spring
    {1, 2, SectionSignal::EventBit, dSv_event_flag_c::F_0011},  // Fence jumping complete -- 1.2 covers the
                                       // fence jump too, not just the herding
    {1, 3, SectionSignal::EventBit, dSv_event_flag_c::F_0024},  // Spoke with Talo/Malo/Beth
    {1, 4, SectionSignal::EventBit, dSv_event_flag_c::F_0015},  // Slingshot tutorial ends
    {1, 5, SectionSignal::EventBit, dSv_event_flag_c::F_0014},  // Sword tutorial ends
    {1, 6, SectionSignal::EventBit, dSv_event_flag_c::M_095},   // First time meeting Coro (obtain lantern)
    {1, 7, SectionSignal::EventBit, dSv_event_flag_c::F_0625},  // Saved Talo and a monkey
    {1, 8, SectionSignal::EventBit, dSv_event_flag_c::F_0630},  // Right after Link is captured (wolf)

    // Chapter 2 - The Twilight. 2.2 stays unmapped ON PURPOSE: the only
    // candidate, F_0204, fires when you first talk to Midna through the bars,
    // near the START of the sewers — mapping it would advance the marker to
    // 2.3 while the player is still down there, which is exactly the running-
    // ahead this table is built to avoid. Unmapped, the marker holds on 2.2
    // until Zelda, lagging rather than lying.
    {2, 1, SectionSignal::EventBit, dSv_event_flag_c::M_009},   // [cutscene 6B] Prison escape
    {2, 3, SectionSignal::EventBit, dSv_event_flag_c::M_012},   // [cutscene 7] Meet Princess Zelda
    {2, 4, SectionSignal::EventBit, dSv_event_flag_c::M_013},   // First heard about Twilight gate from Midna

    // Chapter 3 - Faron Woods: Twilight. 3.4 needs no flag: it is the last
    // section, so once 3.3 completes the marker rests on it anyway.
    {3, 1, SectionSignal::EventBit, dSv_event_flag_c::F_0055},  // Received Vessel of Light from Faron
    {3, 2, SectionSignal::EventBit, dSv_event_flag_c::F_0059},  // Conversation after tears complete
    {3, 3, SectionSignal::EventBit, dSv_event_flag_c::F_0218},  // Bought jar of oil from Coro

    // Chapter 4 - Forest Temple. Mapped by ITEM, not by the ordinal: the site
    // interleaves four monkey-rescue sections between the map, the boomerang,
    // the compass and the big key, so counting items held marked the player
    // several sections early. The unmapped ones (the monkeys, and the boss)
    // hold the marker at the previous mapped section, which is correct — you
    // are working through them with no new item to show for it.
    {4, 1, SectionSignal::DungeonMap, 0},                       // 4.1 Dungeon Map
    {4, 5, SectionSignal::ItemFirstBit, dItemNo_BOOMERANG_e},   // 4.5 The Gale Boomerang
    {4, 6, SectionSignal::DungeonCompass, 0},                   // 4.6 The Compass
    {4, 7, SectionSignal::DungeonBossKey, 0},                   // 4.7 The Big Key
    {4, 9, SectionSignal::EventBit, dSv_event_flag_c::M_022},   // 4.9 Forest Temple clear
};

// The section after the last COMPLETED one — not the first unmapped-or-unset
// entry. The difference is what happens to a section with no flag: several
// have none (the save simply does not record "found Talo's wooden stick"), and
// searching for the first unset entry skipped straight past them, marking the
// next mapped section while the player was still reading the unmapped one.
// Counting from the last completed section instead makes an unmapped section
// HOLD the marker until the following mapped one completes, so the marker can
// only ever lag, never run ahead.
//
// 0 when the chapter has no mappings at all, which suppresses the section
// marker rather than guessing.
int currentOverworldSection(int chapter) {
    int lastDone = 0;
    bool anyMapped = false;
    for (const SectionFlag& e : kSectionFlags) {
        if (e.chapter != chapter) {
            continue;
        }
        anyMapped = true;
        if (sectionDone(e) && e.section > lastDone) {
            lastDone = e.section;
        }
    }
    return anyMapped ? lastDone + 1 : 0;
}

// Clamp to a section this guide actually has. Both the ordinal count and
// "last done + 1" can point past the end — a chapter covered in three sections
// but holding four dungeon items, or a fully completed chapter — and an
// out-of-range section would silently drop the marker exactly when the player
// is furthest along.
int clampToChapter(int chapter, int section) {
    if (section == 0) {
        return 0;
    }
    int last = 0;
    for (const BrowseRow& r : s_browse) {
        if (r.chapterNo == chapter && r.sectionNo > last) {
            last = r.sectionNo;
        }
    }
    return (last != 0 && section > last) ? last : section;
}

// Chapter and section the player is currently in, or 0 for "unknown".
void currentPosition(int* o_chapter, int* o_section) {
    *o_chapter = currentChapter();
    *o_section = 0;
    if (*o_chapter == 0) {
        return;
    }
    // The table wins wherever it has entries, dungeon or not. The item ordinal
    // survives only as the fallback for dungeons nobody has mapped yet.
    *o_section = currentOverworldSection(*o_chapter);
    if (*o_section == 0 && chapterIsDungeon(*o_chapter)) {
        *o_section = currentDungeonSection();
    }
    *o_section = clampToChapter(*o_chapter, *o_section);
}

// The player's own portrait, wolf-aware — the same art the transform button
// uses, so the marker reads as "you" without needing a legend.
void drawHereMarker(f32 x, f32 cy, f32 size) {
    const ResTIMG* face = dmapFloorFaceTimg(companionWolf());
    if (face != NULL) {
        drawTimg(face, x, cy - size * 0.5f, size, size * 1.025f, 0xFF);
    }
}

void rebuildBrowseList() {
    s_browse.clear();
    const dusk::guide::Index idx = dusk::guide::load_index();
    s_guideAvailable = !idx.entries.empty();
    // Presentation order only. The index itself is left alone, and rows keep
    // their ORIGINAL entry index in guideIndex, so s_expanded and every id
    // lookup stay valid.
    std::vector<std::size_t> order(idx.entries.size());
    for (std::size_t i = 0; i < order.size(); i++) {
        order[i] = i;
    }
    std::stable_sort(order.begin(), order.end(),
        [&idx](std::size_t a, std::size_t b) {
            return chapterNumberOf(idx.entries[a]) < chapterNumberOf(idx.entries[b]);
        });
    for (const std::size_t g : order) {
        const auto& e = idx.entries[g];
        // "Twilight Princess Walkthrough - Forest Temple" is the same words on
        // every row; the chapter is the part after the dash.
        std::string title = e.title;
        const std::size_t dash = title.rfind(" - ");
        if (dash != std::string::npos && dash + 3 < title.size()) {
            title = title.substr(dash + 3);
        }
        const int chapterNo = chapterNumberOf(e);
        s_browse.push_back({e.id, title, -1, (int)g, chapterNo == 9999 ? 0 : chapterNo, 0});
        if ((int)g != s_expanded) {
            continue;
        }
        for (std::size_t i = 0; i < e.sections.size(); i++) {
            // Skip the page's own heading. On every real chapter, section 0's
            // title is the document title verbatim and its body is one line of
            // boilerplate ("This chapter covers the normal mode of..."), so it
            // was listed as an "Overview" row that repeated the chapter name
            // and led to nothing worth reading. Matched on the title rather
            // than on the index, so a page that genuinely opens with content
            // under its own subheading still keeps it.
            // ...but only when there is something else to show. A page with no
            // subheadings converts to ONE section named after itself; hiding
            // that left a chapter row that expanded to nothing and could never
            // be opened.
            if (i == 0 && e.sections.size() > 1 && e.sections[i].title == e.title) {
                continue;
            }
            // "13.5 The Spinner" -> section 5 of chapter 13.
            const std::string num = dusk::guide::section_number(e.sections[i].title);
            int secNo = 0;
            if (!num.empty()) {
                const std::size_t dot = num.find('.');
                if (dot != std::string::npos) {
                    secNo = std::atoi(num.c_str() + dot + 1);
                }
            }
            s_browse.push_back({e.id, e.sections[i].title, (int)i, (int)g,
                chapterNo == 9999 ? 0 : chapterNo, secNo});
        }
    }
}

namespace {

// Own wrap buffer. Deliberately NOT readerWrapBody(): that fills a single
// global s_bodyLines[64][228] shared with the mail and item-info readers, so a
// walkthrough section would both overflow it and clobber whichever reader used
// it last. 512 lines x 200 bytes is ~100 KB, which covers a long dungeon
// section without paging.
constexpr int GUIDE_LINE_MAX = 200;
constexpr int GUIDE_LINES_MAX = 512;

char s_lines[GUIDE_LINES_MAX][GUIDE_LINE_MAX];
u8 s_lineKind[GUIDE_LINES_MAX];   // NodeKind of the source node
u8 s_lineLevel[GUIDE_LINES_MAX];
// Per-line advance. Text lines all share LINE_H, but an image occupies its own
// scaled height, so the flow cannot derive advance from kind alone.
f32 s_lineH[GUIDE_LINES_MAX];
std::string s_lineRef[GUIDE_LINES_MAX];  // image file, for Image lines
// Total wrapped height. Summed once per wrap rather than re-summed over every
// line on every frame -- it cannot change without a rewrap. See rewrapIfNeeded.
f32 s_contentH = 0.0f;
int s_lineCount = 0;

// Converted-image cache, bounded. Blobs are linear RGBA8 (image.cpp), so a
// 512x512 image is 1 MiB and an unbounded cache would quietly grow past what
// the second screen is worth. Evicted whole-entry, oldest first.
constexpr std::size_t IMG_CACHE_BUDGET = 6u * 1024u * 1024u;
struct CachedImage {
    // Doc and ref kept APART rather than as one "doc/ref" key. Joining them
    // meant building that string on every imageFor() call -- once per visible
    // image per frame, on the draw path -- purely to compare it and throw it
    // away. Two compares of existing strings allocate nothing.
    std::string doc;
    std::string ref;
    std::vector<std::uint8_t> blob;
};
std::vector<CachedImage> s_imgCache;
std::size_t s_imgCacheBytes = 0;

// Returns the blob for `ref`, loading it on first use. Null when the image was
// never acquired or failed to convert — the caller then draws the alt text.
// Budget so a scroll can never stall on a queue of decodes. One per frame is
// enough to fill a screen in a few frames and is invisible; unbounded was the
// per-frame re-decode cliff once the cache started evicting.
int s_decodeBudget = 0;

const ResTIMG* imageFor(const std::string& ref) {
    if (ref.empty()) {
        return nullptr;
    }
    // Keyed on doc AND ref, not ref alone. A multi-chapter walkthrough imports
    // as one guide per chapter from one site, so "image1.jpg" collides
    // constantly and chapter 2 was rendering chapter 1's pictures.
    for (const CachedImage& c : s_imgCache) {
        if (c.ref == ref && c.doc == s_docId) {
            return c.blob.empty() ? nullptr : (const ResTIMG*)c.blob.data();
        }
    }
    if (s_decodeBudget <= 0) {
        return nullptr;  // not resident yet; alt text this frame
    }
    s_decodeBudget--;
    CachedImage entry;
    entry.doc = s_docId;
    entry.ref = ref;
    entry.blob = dusk::guide::load_timg_blob(dusk::guide::images_dir(s_docId) / ref);
    if (!entry.blob.empty() && !isSaneTimg((const ResTIMG*)entry.blob.data())) {
        entry.blob.clear();  // corrupt or unsupported: fall back to alt text
    }
    // Evict oldest until the new entry fits. Ordered so the new entry is
    // still outside the list, which is what actually makes it un-evictable.
    while (!s_imgCache.empty() && s_imgCacheBytes + entry.blob.size() > IMG_CACHE_BUDGET) {
        s_imgCacheBytes -= s_imgCache.front().blob.size();
        s_imgCache.erase(s_imgCache.begin());
    }
    s_imgCacheBytes += entry.blob.size();
    s_imgCache.push_back(std::move(entry));
    const CachedImage& back = s_imgCache.back();
    return back.blob.empty() ? nullptr : (const ResTIMG*)back.blob.data();
}

// The wrap is keyed on (guide, section, width): Cinematic and Functional give
// the window different widths, so switching HUD mode must re-wrap.
f32 s_wrappedWidth = 0.0f;
int s_wrappedSection = -2;



constexpr f32 TEXT_SIZE = 14.0f;
constexpr f32 HEAD_SIZE = 17.0f;
constexpr f32 LINE_H = 20.0f;
constexpr f32 PARA_GAP = 8.0f;
// Layout. Named by what they measure, because several share a value by
// coincidence and editing one used to mean editing all of them by accident.
constexpr f32 HEADER_H = 30.0f;     // top strip: title + prev/next
constexpr f32 BTN_H = 26.0f;        // prev/next/list tab plates
constexpr f32 ROW_H = 30.0f;        // one browse-list row
constexpr f32 BODY_INSET = 26.0f;   // total horizontal margin around body text
constexpr f32 IMG_ALT_H = 26.0f;    // flow height reserved for missing-image text
constexpr f32 LIST_INDENT = 14.0f;  // per nesting level of a list item

void pushLine(const char* text, u8 kind, u8 level, f32 h, const std::string& ref = {}) {
    if (s_lineCount >= GUIDE_LINES_MAX) {
        return;
    }
    std::snprintf(s_lines[s_lineCount], GUIDE_LINE_MAX, "%s", text);
    s_lineKind[s_lineCount] = kind;
    s_lineLevel[s_lineCount] = level;
    s_lineH[s_lineCount] = h;
    s_lineRef[s_lineCount] = ref;
    s_lineCount++;
}

// Greedy word wrap against the real glyph metrics. Runs once per section open
// (or width change), never per frame — measureText is a per-byte scan and the
// existing wrapper measures the whole candidate line for every word, which is
// exactly the cost we do not want on the draw path.
void wrapNode(const dusk::guide::Node& n, f32 width) {
    using dusk::guide::NodeKind;
    const f32 size = n.kind == NodeKind::Heading ? HEAD_SIZE : TEXT_SIZE;
    const f32 indent = n.kind == NodeKind::ListItem ? LIST_INDENT * (f32)n.level : 0.0f;
    const f32 avail = width - indent;
    if (n.kind == NodeKind::Rule) {
        pushLine("", (u8)NodeKind::Rule, 0, PARA_GAP + 6.0f);
        return;
    }
    if (n.kind == NodeKind::Image) {
        // Reserve the scaled height NOW so scrolling is stable: the flow must
        // not resize when a blob happens to load, or the reader would jump
        // under the finger. Aspect comes from the converted header when the
        // image is present, and a fixed slot stands in when it is not.
        // Sized from the RECORDED dimensions, not by decoding. This used to
        // call imageFor() for every image node just to read an aspect ratio,
        // which meant a full file read + JPEG decode each — 20 of them inside
        // one draw call for a typical section, evicting the very entries the
        // draw was about to need.
        f32 h = IMG_ALT_H;  // alt-text fallback: one line
        const std::string& file = n.ref;
        if (n.imgW > 0 && n.imgH > 0) {
            const f32 tw = (f32)n.imgW;
            const f32 drawW = width < tw ? width : tw;
            h = (f32)n.imgH * (drawW / tw) + 8.0f;
        }
        pushLine(n.text.c_str(), (u8)NodeKind::Image, 0, h, file);
        return;
    }
    // Bullet marker is part of the first line's text so the wrap accounts for
    // its width rather than letting it overhang.
    std::string pending;
    if (n.kind == NodeKind::ListItem) {
        pending = "* ";
    }
    const char* p = n.text.c_str();
    std::string line = pending;
    std::string word;
    auto flushLine = [&]() {
        if (!line.empty()) {
            pushLine(line.c_str(), (u8)n.kind, n.level,
                n.kind == NodeKind::Heading ? LINE_H + 6.0f : LINE_H);
            line.clear();
        }
    };
    for (;; p++) {
        if (*p != '\0' && *p != ' ') {
            word.push_back(*p);
            continue;
        }
        if (!word.empty()) {
            std::string cand = line.empty() ? word : line + " " + word;
            if (measureText(size, cand.c_str()) > avail && !line.empty()) {
                flushLine();
                line = word;
            } else {
                line = std::move(cand);
            }
            word.clear();
        }
        if (*p == '\0') {
            break;
        }
    }
    flushLine();
    if (s_lineCount == 0 || n.kind != NodeKind::ListItem) {
        pushLine("", 0xFF, 0, PARA_GAP);  // paragraph gap marker
    }
}

void rewrapIfNeeded(f32 width) {
    if (!s_docLoaded || s_guideSection < 0 ||
        s_guideSection >= (int)s_doc.sections.size())
    {
        return;
    }
    if (s_wrappedSection == s_guideSection && s_wrappedWidth == width) {
        return;
    }
    s_lineCount = 0;
    for (const auto& n : s_doc.sections[(std::size_t)s_guideSection].nodes) {
        wrapNode(n, width);
    }
    if (s_lineCount >= GUIDE_LINES_MAX) {
        // The cap is a fixed buffer, not a policy. Measured against the real
        // store, the largest chapter section wraps to ~235 lines, so this is
        // roughly 2x headroom — but if a page ever does hit it, say so instead
        // of just stopping mid-sentence with no indication anything is missing.
        std::snprintf(s_lines[GUIDE_LINES_MAX - 1], GUIDE_LINE_MAX, "%s",
            "[section too long to display in full]");
        s_lineKind[GUIDE_LINES_MAX - 1] = (u8)dusk::guide::NodeKind::Paragraph;
        s_lineLevel[GUIDE_LINES_MAX - 1] = 0;
        s_lineH[GUIDE_LINES_MAX - 1] = LINE_H;
        s_lineRef[GUIDE_LINES_MAX - 1].clear();
    }
    // After any adjustment above, so the marker's height is included.
    s_contentH = 0.0f;
    for (int i = 0; i < s_lineCount; i++) {
        s_contentH += s_lineH[i];
    }
    s_wrappedSection = s_guideSection;
    s_wrappedWidth = width;
}

// The one place glyphs reach the screen. Isolated because the draw-call
// measurement (836 baseline vs +1660 for a screen of prose, one GXBegin AND one
// GXLoadTexObj per glyph) says this is the hot spot: if it has to become a
// batched draw or a cached page texture, only this function changes.
void drawBodyLine(int idx, f32 x, f32 y, FontDrawContext* ctx) {
    using dusk::guide::NodeKind;
    const u8 kind = s_lineKind[idx];
    if (kind == 0xFF) {
        return;  // spacer
    }
    if (kind == (u8)NodeKind::Rule) {
        fillRect(x, y - 6.0f, x + s_wrappedWidth, y - 5.0f, COL_FRAME);
        return;
    }
    if (kind == (u8)NodeKind::Image) {
        const ResTIMG* t = imageFor(s_lineRef[idx]);
        if (t == nullptr) {
            // Never acquired, or not decodable (SDL has no JPEG loader). The
            // alt text is why the converter keeps it.
            drawTextEllipsized(x, y, TEXT_SIZE, s_wrappedWidth, TEXT_DIM,
                s_lines[idx][0] != '\0' ? s_lines[idx] : "[image]");
            return;
        }
        const f32 tw = (f32)(u16)t->width;
        const f32 th = (f32)(u16)t->height;
        const f32 drawW = s_wrappedWidth < tw ? s_wrappedWidth : tw;
        const f32 drawH = th * (drawW / tw);
        drawTimg(t, x, y - TEXT_SIZE, drawW, drawH, 0xFF);
        return;
    }
    const bool head = kind == (u8)NodeKind::Heading;
    const f32 indent = kind == (u8)NodeKind::ListItem ? LIST_INDENT * (f32)s_lineLevel[idx] : 0.0f;
    // One draw call per glyph, deliberately. A screen of prose here costs
    // ~1600 calls and as many texture binds, so batching them is tempting --
    // it was tried and reverted, and the reason is worth keeping:
    //
    //   drawChar_scale calls pushDrawState() ONLY when no FontDrawContext is
    //   passed. Hand it one and the caller now owns the vertex format; get
    //   that wrong and the FIFO desyncs outright. On device it surfaced as
    //   "draw vertex data overrun: need 596000 bytes" -- ~149KB per vertex,
    //   i.e. not the format the font expected.
    //
    // Anyone re-attempting it must set up the state the font would have set
    // up. The draw-call count is an optimisation; this path is correct.
    (void)ctx;
    drawText(x + indent, y, head ? HEAD_SIZE : TEXT_SIZE,
        head ? TEXT_ACCENT : TEXT_MAIN, "%s", s_lines[idx]);
}

f32 lineAdvance(int idx) {
    return s_lineH[idx];
}


// Rebuilds the browse list and reports whether anything is saved. Cheap: one
// small index read, no documents touched.
void loadFirstDocument() {
    rebuildBrowseList();
    s_docLoaded = !s_browse.empty();
    s_wrappedSection = -2;
}

// Loads the document a browse row points at, if it is not already resident.
bool ensureDocument(const std::string& id) {
    if (s_docId == id && !s_doc.sections.empty()) {
        return true;
    }
    if (auto d = dusk::guide::load_document(id)) {
        s_doc = std::move(*d);
        s_docId = id;
        s_wrappedSection = -2;
        return true;
    }
    return false;
}

}  // namespace

bool guideAvailable() {
    if (!getSettings().game.guideEnabled.getValue()) {
        return false;  // the setting existed but nothing read it
    }
    // Checked once, lazily. It used to be set only by rebuildBrowseList, which
    // runs from guideOpen — so the left-column page that OPENS the guide only
    // appeared after the guide had already been opened. Nothing could ever
    // reach it.
    // Re-checked while empty. Latching the first answer meant a store that was
    // empty at boot stayed "empty" forever — and since the left-column box is
    // the only way to reach guideOpen(), which is the only caller of
    // begin_import(), nothing saved during the session could ever be picked up
    // without restarting. Once something exists the answer is stable and all of
    // this stops.
    // A converter bump has to reach installs that ALREADY have guides -- those
    // are the ones with something to re-convert, and the poll below only fires
    // while the store is empty, so on its own it reaches none of them. One
    // shot per launch; scan_import_folder is idempotent and no-ops when the
    // stamp is current.
    static bool sReconvertChecked = false;
    if (!sReconvertChecked) {
        sReconvertChecked = true;
        if (dusk::guide::index_needs_reconvert()) {
            dusk::guide::begin_import();
        }
    }
    if (!s_guideAvailable) {
        // Refreshed on the import's COMPLETION EDGE, not on a timer. This used
        // to re-read and re-parse index.json every ~4s for the whole session on
        // any install with no guides — which is the default state — purely to
        // notice a change that only an import can cause. The generation bump is
        // exactly that event, and testing it is an atomic load instead of a
        // file read plus a JSON parse on the game thread.
        static unsigned sSeenGen = ~0u;  // forces the first read
        const unsigned gen = dusk::guide::import_generation();
        if (gen != sSeenGen) {
            sSeenGen = gen;
            s_guideAvailable = !dusk::guide::load_index().entries.empty();
        }
        if (!s_guideAvailable) {
            static int sPoll = 0;
            if (--sPoll <= 0) {
                sPoll = 240;  // ~4s at 60fps
                // Break the deadlock. begin_import() used to be reachable only
                // from guideOpen(), which needed the left-column box, which
                // needed a non-empty index, which only an import could produce
                // — so on a clean install nothing the browser saved was ever
                // picked up, across restarts. Still a poll because a guide can
                // be saved mid-session and nothing else would notice, but the
                // scanning happens on the worker; the game thread only starts
                // it, and starting one while one runs is a no-op.
                dusk::guide::begin_import();
            }
        }
    }
    return s_guideAvailable;
}

// Left-column page: icon and label, matching the other boxes. Tapping it opens
// the reader — this replaced the battery tap, which was an invisible gesture
// on a readout that means something else.
void drawLeftGuideBox(f32 x1, f32 y0, f32 y1) {
    (void)y1;
    const f32 cx = x1 * 0.5f + 4.0f;
    // Collect slot 0 is the fish journal — a book, which reads as "guide".
    if (const ResTIMG* book = collectIconTimg(0)) {
        const f32 sz = 40.0f;
        drawTimg(book, cx - sz * 0.5f, y0 + 18.0f, sz, sz, 0xFF);
    }
    drawTextFittedCentered(cx, y0 + 80.0f, 15.0f, 9.0f, x1 - 12.0f, TEXT_MAIN,
        txt(STR_GUIDE));
}

// Deferred from touch-release, so a swipe scrolls instead of opening a row.
void guideRowTap(int row) {
    if (row < 0 || row >= (int)s_browse.size()) {
        return;
    }
    const BrowseRow r = s_browse[(std::size_t)row];
    if (r.section < 0) {
        s_expanded = s_expanded == r.guideIndex ? -1 : r.guideIndex;
        rebuildBrowseList();
        // Keep the row the finger landed on exactly where it was. This used to
        // reset the scroll to 0, which threw the list back to the first
        // chapter every time one was expanded — the further down you were, the
        // more of a jump it was.
        //
        // Not simply "leave the scroll alone" either: only one chapter is open
        // at a time, so expanding this one COLLAPSES the previous one, and if
        // that sat above this row every row above shifts up. Re-finding the
        // row and correcting by the difference handles both directions.
        for (std::size_t i = 0; i < s_browse.size(); i++) {
            if (s_browse[i].section < 0 && s_browse[i].guideIndex == r.guideIndex) {
                s_scrollGuide += ((f32)i - (f32)row) * ROW_H;
                break;
            }
        }
        if (s_scrollGuide < 0.0f) {
            s_scrollGuide = 0.0f;  // upper bound is clamped by the draw
        }
        queueSound(Z2SE_SY_CURSOR_OK, HAPTIC_LIGHT);
        return;
    }
    if (ensureDocument(r.guideId)) {
        s_guideSection = r.section;
        s_scrollGuide = 0.0f;
        s_wrappedSection = -2;
        queueSound(Z2SE_SY_CURSOR_OK, HAPTIC_LIGHT);
    }
}

// Next/previous section, walking into the neighbouring chapter at the ends so
// the pair covers the whole guide rather than stopping at each chapter.
void guideStep(int delta) {
    const dusk::guide::Index idx = dusk::guide::load_index();
    int g = -1;
    for (std::size_t i = 0; i < idx.entries.size(); i++) {
        if (idx.entries[i].id == s_docId) {
            g = (int)i;
            break;
        }
    }
    if (g < 0) {
        return;
    }
    int sec = s_guideSection + delta;
    if (sec < 1) {
        if (g == 0) {
            return;
        }
        g--;
        sec = (int)idx.entries[(std::size_t)g].sections.size() - 1;
        if (sec < 1) {
            return;
        }
    } else if (sec >= (int)idx.entries[(std::size_t)g].sections.size()) {
        if (g + 1 >= (int)idx.entries.size()) {
            return;
        }
        g++;
        sec = 1;
    }
    if (ensureDocument(idx.entries[(std::size_t)g].id)) {
        s_guideSection = sec;
        s_scrollGuide = 0.0f;
        s_wrappedSection = -2;
        s_expanded = g;
        rebuildBrowseList();
        queueSound(Z2SE_SY_CURSOR_OK, HAPTIC_LIGHT);
    }
}

bool guideIsOpen() {
    return s_guideOpen || s_guideT > 0.0f;
}

void guideOpen() {
    if (s_guideOpen) {
        return;
    }
    s_guideOpen = true;
    // Section and scroll are deliberately NOT reset: closing to glance at the
    // map and coming back should land exactly where you were reading.
    // Load lazily: the store touches the filesystem, so it must not run every
    // frame, and there is no point paying for it until the reader is asked for.
    // Unconditional rescan. This used to be inside the !s_docLoaded branch, so
    // once anything was in the catalogue nothing ever looked at the import
    // folder again — pages dropped in by hand, or saved after the first import
    // ran, stayed invisible. Kicked onto a worker, never run here: importing
    // converts pages and pulls their images over a BLOCKING http::get, which on
    // this thread froze the game for as long as the downloads took.
    dusk::guide::begin_import();
    if (!s_docLoaded) {
        loadFirstDocument();
    }
}

// One step out: section -> list -> closed. Driven by the context tab.
void guideBack() {
    if (s_guideSection >= 0) {
        s_guideSection = -1;
        s_scrollGuide = 0.0f;
        s_wrappedSection = -2;
        return;
    }
    // In the Wii U layout the reader IS a page, so there is nothing behind it
    // to close back to — the tab strip is the way out. Closing here would
    // blank the window for a frame and the page draw would reopen it on the
    // next, which reads as a flicker rather than as an action.
    if (s_page.load() == PAGE_GUIDE) {
        return;
    }
    guideClose();
}


void guideClose() {
    s_guideOpen = false;
}

namespace {

// Shared tap-target publication. The list is a fixed 12 slots (see
// companion_internal.h) and companion_touch.cpp walks the same array, so the
// cap has to live with the write, not at each call site.
constexpr int READER_RECT_MAX = 12;

void publishReaderRect(f32 rx, f32 ry, f32 rw, f32 rh, int id) {
    if (s_readerRectCount >= READER_RECT_MAX) {
        return;
    }
    s_readerRects[s_readerRectCount][0] = rx;
    s_readerRects[s_readerRectCount][1] = ry;
    s_readerRects[s_readerRectCount][2] = rx + rw;
    s_readerRects[s_readerRectCount][3] = ry + rh;
    s_readerRectIds[s_readerRectCount] = id;
    s_readerRectCount++;
}

// Header strip: prev/next plate, title, and the rule under them. Publishes the
// two nav rects and returns where the body starts.
void drawGuideHeader(f32 x0, f32 y0, f32 x1) {
    const char* title = !s_docLoaded ? txt(STR_GUIDE_EMPTY)
        : (s_guideSection >= 0 && s_guideSection < (int)s_doc.sections.size()
                  ? s_doc.sections[(std::size_t)s_guideSection].title.c_str()
                  : s_doc.title.c_str());
    // Prev / next section, TOP RIGHT, drawn with the context tab's plate so
    // they read as the same class of control. Only while reading a section.
    s_guidePrevRect[0] = 0.0f;
    s_guidePrevRect[2] = 0.0f;
    s_guideNextRect[0] = 0.0f;
    s_guideNextRect[2] = 0.0f;
    const f32 titleX = x0 + 8.0f;
    f32 titleRight = x1 - 10.0f;
    if (s_guideSection >= 0) {
        const f32 bw = 34.0f;
        const f32 bx2 = x1 - 6.0f - bw;          // next, hard against the edge
        const f32 bx = bx2 - 4.0f - bw;          // prev, just inside it
        drawTabPlate(bx, y0 + 2.0f, bw, BTN_H, true);
        drawTextFittedCentered(bx + bw * 0.5f, y0 + 21.0f, 16.0f, 9.0f, bw - 6.0f,
            TEXT_TAB_ACTIVE, "<");
        s_guidePrevRect[0] = bx;
        s_guidePrevRect[1] = y0 + 2.0f;
        s_guidePrevRect[2] = bx + bw;
        s_guidePrevRect[3] = y0 + 28.0f;
        drawTabPlate(bx2, y0 + 2.0f, bw, BTN_H, true);
        drawTextFittedCentered(bx2 + bw * 0.5f, y0 + 21.0f, 16.0f, 9.0f, bw - 6.0f,
            TEXT_TAB_ACTIVE, ">");
        s_guideNextRect[0] = bx2;
        s_guideNextRect[1] = y0 + 2.0f;
        s_guideNextRect[2] = bx2 + bw;
        s_guideNextRect[3] = y0 + 28.0f;
        titleRight = bx - 10.0f;  // title stops short of the buttons
    }

    // Smaller than a heading: it sits inline with the < > buttons, and at
    // HEAD_SIZE it crowded them. On the list there is no section to name, so
    // it is always just "Guide".
    const char* shown = s_guideSection < 0
        ? txt(STR_GUIDE)
        : (title != nullptr && title[0] != '\0' ? title : txt(STR_GUIDE));
    // Same marker as the list, so opening the section the marker pointed at
    // confirms itself rather than leaving you wondering if you picked right.
    f32 textX = titleX;
    if (s_guideSection >= 0 && title != nullptr) {
        int hereChapter = 0;
        int hereSection = 0;
        currentPosition(&hereChapter, &hereSection);
        const std::string num = dusk::guide::section_number(title);
        const std::size_t dot = num.find('.');
        if (hereChapter != 0 && hereSection != 0 && dot != std::string::npos &&
            std::atoi(num.c_str()) == hereChapter &&
            std::atoi(num.c_str() + dot + 1) == hereSection)
        {
            drawHereMarker(titleX, y0 + 14.0f, 18.0f);
            textX = titleX + 22.0f;
        }
    }
    drawTextEllipsized(textX, y0 + 19.0f, TEXT_SIZE, titleRight - textX, TEXT_ACCENT, shown);
    // No Back button here: it lives on the context tab now, which is where
    // the rest of the companion puts navigation.
    fillRect(x0 + 6.0f, y0 + HEADER_H, x1 - 6.0f, y0 + HEADER_H + 1.0f, COL_FRAME);
}

// Browse list across every saved guide: one row per chapter header, plus its
// sections while expanded. Rows publish through the shared reader-rect list,
// same as the collect page.
void drawGuideBrowseList(f32 x0, f32 y0, f32 x1, f32 y1, f32 bodyX, f32 bodyW, f32 viewH) {
    const f32 bodyY0 = y0 + HEADER_H + 6.0f;
    int hereChapter = 0;
    int hereSection = 0;
    currentPosition(&hereChapter, &hereSection);
    s_readerRectCount = 0;
    // Clamped BEFORE the rows are placed. It used to run at the end of this
    // function, so a drag that pushed the scroll past an end had its overscroll
    // drawn for one frame and corrected on the next — every frame the finger
    // kept pulling, which read as the list shaking in place at the top.
    const f32 contentH = (f32)s_browse.size() * ROW_H;
    const f32 maxScroll = clampListScroll(&s_scrollGuide, contentH, viewH);
    f32 ry = bodyY0 - s_scrollGuide;
    // Inset by the border: clipping to y1 exactly let rows paint over the
    // panel's own bottom rule as they scrolled past it.
    setWinScissor(x0 + 2.0f, bodyY0, x1 - 2.0f, y1 - 3.0f);
    for (std::size_t i = 0; i < s_browse.size(); i++) {
        if (ry + ROW_H > bodyY0 - ROW_H && ry < y1) {
            const bool header = s_browse[i].section < 0;
            const bool open = header && s_browse[i].guideIndex == s_expanded;
            const f32 rx = header ? bodyX : bodyX + 18.0f;
            const f32 rw = header ? bodyW : bodyW - 18.0f;
            drawTabPlate(rx, ry, rw, ROW_H - 4.0f, open);
            if (header) {
                // Disclosure marker, so a chapter reads as openable.
                drawText(rx + 8.0f, ry + 19.0f, TEXT_SIZE,
                    open ? TEXT_TAB_ACTIVE : TEXT_ACCENT, "%s", open ? "-" : "+");
            }
            // An expanded header sits on the light parchment plate, where
            // gold is nearly unreadable — use the dark ink the selected
            // tab and context tab already use.
            // "You are here". A chapter row is marked whenever the player is
            // anywhere in that chapter; a section row only when the dungeon
            // ordinal picks it out. Drawn on the right so it never shifts the
            // label, and skipped entirely when the position is unknown.
            const bool hereRow = hereChapter != 0 && s_browse[i].chapterNo == hereChapter &&
                (header ? true : (hereSection != 0 && s_browse[i].sectionNo == hereSection));
            const f32 markerW = hereRow ? 22.0f : 0.0f;
            drawTextEllipsized(rx + (header ? 24.0f : 8.0f), ry + 19.0f, TEXT_SIZE,
                rw - (header ? 32.0f : 16.0f) - markerW,
                open ? TEXT_TAB_ACTIVE : (header ? TEXT_ACCENT : TEXT_MAIN),
                s_browse[i].label.c_str());
            if (hereRow) {
                drawHereMarker(rx + rw - 24.0f, ry + (ROW_H - 4.0f) * 0.5f, 18.0f);
            }
            publishReaderRect(rx, ry, rw, ROW_H - 4.0f, (int)i);
        }
        ry += ROW_H;
    }
    applyWinClip();
    drawListScrollHint(x1, bodyY0, y1, s_scrollGuide, maxScroll, viewH, contentH);
}

// The section itself: wrapped prose and inline images, scrolled as one flow.
void drawGuideSectionBody(f32 x0, f32 y0, f32 x1, f32 y1, f32 bodyX, f32 bodyW, f32 viewH) {
    const f32 bodyY0 = y0 + HEADER_H + 6.0f;
    rewrapIfNeeded(bodyW);
    const f32 contentH = s_contentH;
    const f32 maxScroll = clampListScroll(&s_scrollGuide, contentH, viewH);

    // Inset by the border: clipping to y1 exactly let rows paint over the
    // panel's own bottom rule as they scrolled past it.
    setWinScissor(x0 + 2.0f, bodyY0, x1 - 2.0f, y1 - 3.0f);
    // ONE context for the whole page, not one per line: the latch inside it is
    // what skips the font's per-glyph texture rebind, so its value is exactly
    // proportional to how many draws it spans.
    FontDrawContext fontCtx;
    f32 y = bodyY0 + TEXT_SIZE - s_scrollGuide;
    for (int i = 0; i < s_lineCount; i++) {
        const f32 adv = lineAdvance(i);
        // Cull off-screen lines. This does NOT reduce the on-screen glyph cost
        // (the measured 1660 is what is visible), but it keeps a long section
        // from paying for lines nobody can see.
        if (y > bodyY0 - adv && y < y1 + adv) {
            drawBodyLine(i, bodyX, y, &fontCtx);
        }
        y += adv;
        if (y > y1 + 40.0f) {
            break;
        }
    }
    applyWinClip();
    drawListScrollHint(x1, bodyY0, y1, s_scrollGuide, maxScroll, viewH, contentH);

    // Wii U only. There the reader is a PAGE and drawGuideOverlay paints over
    // the whole content window — including drawCinematicContextTab, which is
    // drawn earlier and therefore sits UNDER the panel. Its Back was never
    // visible, so the list was unreachable once a section was open. Functional
    // keeps using the context tab, which is where that layout puts navigation.
    //
    // Square plate, not the chamfered bed the panel itself uses: it is a
    // control sitting on the surface, matching the < > buttons above.
    s_guideListRect[0] = 0.0f;
    s_guideListRect[2] = 0.0f;
    if (!dusk::dualscreen::mainHudRestored()) {
        const f32 bw = 62.0f;
        const f32 bx = x1 - 6.0f - bw;
        const f32 by = y1 - 6.0f - BTN_H;
        drawTabPlate(bx, by, bw, BTN_H, true);
        drawTextFittedCentered(bx + bw * 0.5f, by + 18.0f, TEXT_SIZE, 9.0f, bw - 8.0f,
            TEXT_TAB_ACTIVE, txt(STR_GUIDE_LIST));
        s_guideListRect[0] = bx;
        s_guideListRect[1] = by;
        s_guideListRect[2] = bx + bw;
        s_guideListRect[3] = by + BTN_H;
    }
}

}  // namespace

void drawGuideOverlay(f32 x0, f32 y0, f32 x1, f32 y1) {
    // Ramp first so a close still animates out after the flag clears.
    if (s_guideOpen) {
        s_guideT += (1.0f - s_guideT) * ANIM_RATE_FAST;
        if (s_guideT > ANIM_DONE) {
            s_guideT = 1.0f;
        }
    } else {
        s_guideT *= ANIM_DECAY_FAST;
        if (s_guideT < ANIM_ZERO) {
            s_guideT = 0.0f;
        }
    }
    if (s_guideT <= 0.0f) {
        return;
    }

    s_decodeBudget = 1;
    // Cheap poll; the actual work happened off-thread. Tracked per-consumer
    // because the settings pane watches the same signal (see fetch.hpp).
    static unsigned s_seenImportGen = 0;
    const unsigned gen = dusk::guide::import_generation();
    if (gen != s_seenImportGen) {
        s_seenImportGen = gen;
        // Only when the import actually brought something in. Every reader
        // open now kicks a scan (that is how pages saved mid-session are
        // noticed), and a scan that finds nothing still completes and still
        // bumps the generation — so clearing unconditionally threw away a
        // populated image cache every single time the guide was opened.
        if (dusk::guide::last_import_count() > 0) {
            // Negative cache entries (image not on disk yet) must not outlive
            // the import that fills them in; see imageFor.
            s_imgCache.clear();
            s_imgCacheBytes = 0;
            loadFirstDocument();
        } else if (!s_docLoaded) {
            loadFirstDocument();
        }
    }

    const f32 prevA = s_drawAlpha;
    s_drawAlpha = prevA * s_guideT;

    // Its own panel, not a flat wash over the page. The page underneath keeps
    // drawing (it owns the tab strip's grow animation) so this must be opaque,
    // but a chamfered bed plus the window rule makes it read as a separate
    // surface sitting on top rather than as the map with text painted on it.
    fillChamferRect(x0, y0, x1, y1, 10.0f, COL_WINDOW, 1 | 2 | 4 | 8);
    drawChamferFrame(x0, y0, x1, y1, 10.0f, 1.5f, COL_FRAME, 1 | 2 | 4 | 8);

    drawGuideHeader(x0, y0, x1);

    const f32 bodyY0 = y0 + HEADER_H + 6.0f;
    const f32 viewH = y1 - bodyY0 - 8.0f;
    const f32 bodyX = x0 + 10.0f;
    const f32 bodyW = x1 - x0 - BODY_INSET;

    if (!s_docLoaded) {
        drawTextCentered((x0 + x1) * 0.5f, (y0 + y1) * 0.5f, TEXT_SIZE, TEXT_DIM,
            dusk::guide::import_in_progress() ? txt(STR_GUIDE_IMPORTING)
                                              : txt(STR_GUIDE_HOWTO));
    } else if (s_guideSection < 0) {
        drawGuideBrowseList(x0, y0, x1, y1, bodyX, bodyW, viewH);
    } else {
        drawGuideSectionBody(x0, y0, x1, y1, bodyX, bodyW, viewH);
    }
    s_drawAlpha = prevA;
}

bool handleGuideTouch(f32 tx, f32 ty) {
    if (!s_guideOpen) {
        return false;
    }
    auto hit = [&](const f32* r) {
        return r[2] > r[0] && tx >= r[0] && tx <= r[2] && ty >= r[1] && ty <= r[3];
    };
    if (hit(s_guideListRect)) {
        guideBack();
        return true;
    }
    if (hit(s_guidePrevRect)) {
        guideStep(-1);
        return true;
    }
    if (hit(s_guideNextRect)) {
        guideStep(1);
        return true;
    }
    if (s_guideSection < 0) {
        // Rows are CANDIDATES, not actions. The press is remembered and only
        // fires from touch-release (guideRowTap), so a drag that starts on a
        // row scrolls the list instead of opening it. Scanned backwards
        // because rows are painted top-down and the last drawn wins.
        for (int i = s_readerRectCount - 1; i >= 0; i--) {
            if (tx >= s_readerRects[i][0] && tx <= s_readerRects[i][2] &&
                ty >= s_readerRects[i][1] && ty <= s_readerRects[i][3])
            {
                // Only the id is needed; s_readerTapRect drives the collect
                // page's zoom-open, which the guide does not use.
                s_readerTapCand = s_readerRectIds[i];
                break;
            }
        }
    }
    return true;  // modal while open: taps inside the window never fall through
}

}  // namespace dusk::companion
