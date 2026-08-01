#include <algorithm>
#include "dusk/guide/store.hpp"

#include <SDL3/SDL_system.h>

#include "dusk/data.hpp"
#include "dusk/logging.h"
#include "dusk/guide/image.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>
#include <string_view>
#include <system_error>

// Declared rather than #including "dusk/main.h": that header uses the game's
// s8/s16 typedefs without pulling them in, so including it here would drag the
// whole Dolphin type system into a module that is otherwise plain std. One
// symbol is all the store needs.
namespace dusk {
extern std::filesystem::path ConfigPath;
}  // namespace dusk

namespace dusk::guide {
namespace {

using json = nlohmann::json;

// Minimal base64 decoder for inline data: images. The in-app browser embeds
// them because zeldadungeon answers 403 to a plain client for IMAGES too, not
// just pages — so the only thing that can fetch them is the browser already
// rendering them.
std::string base64_decode(std::string_view in) {
    static constexpr signed char kT[] = {
        62, -1, -1, -1, 63, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1, -1, -2, -1, -1, -1,
        0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21,
        22, 23, 24, 25, -1, -1, -1, -1, -1, -1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37,
        38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51};
    std::string out;
    out.reserve(in.size() * 3 / 4);
    int val = 0;
    int bits = -8;
    for (const unsigned char c : in) {
        if (c < 43 || c > 122) {
            continue;
        }
        const signed char d = kT[c - 43];
        if (d < 0) {
            continue;
        }
        val = (val << 6) + d;
        bits += 6;
        if (bits >= 0) {
            out.push_back((char)((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

constexpr auto kIndexFile = "index.json";
constexpr int kIndexVersion = 1;

// Bumped whenever the CONVERTER changes in a way that alters output. Sources
// are archived to import/done once converted and never revisited, so without
// this a converter fix is invisible to anyone who already imported — their
// .guide files keep whatever the old code produced. On a mismatch every
// archived source is converted again.
constexpr int kConverterVersion = 8;  // section-prefixed image names

// Read a whole file. Returns nullopt rather than throwing: every caller here
// treats "missing or unreadable" as "not present", never as a hard error.
std::optional<std::string> read_file(const std::filesystem::path& p) {
    std::error_code ec;
    if (!std::filesystem::exists(p, ec) || ec) {
        return std::nullopt;
    }
    std::ifstream f(p, std::ios::binary);
    if (!f) {
        return std::nullopt;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Temp-then-rename, mirroring config.cpp: a crash or a full disk mid-write
// leaves the previous file intact instead of a truncated one that would fail
// to parse on next launch.
bool write_atomic(const std::filesystem::path& p, const std::string& data) {
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::filesystem::path tmp = p;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            return false;
        }
        f.write(data.data(), (std::streamsize)data.size());
        if (!f.good()) {
            // Drop the partial temp rather than leaving it in the guides
            // directory, matching store_image_file. A failed write is usually
            // a full disk, and keeping the fragment only makes that worse.
            f.close();
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
    std::filesystem::rename(tmp, p, ec);
    if (ec) {
        // Cross-device or a Windows-style locked target: fall back to
        // remove+rename, and drop the temp if even that fails.
        std::filesystem::remove(p, ec);
        std::filesystem::rename(tmp, p, ec);
        if (ec) {
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
    return true;
}

// Ids become filenames, so anything that could escape the guides directory is
// rejected outright rather than sanitised — a silently-renamed id would break
// the index's reference to it.
bool id_is_safe(const std::string& id) {
    if (id.empty() || id.size() > 96) {
        return false;
    }
    for (const char c : id) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

}  // namespace

#if defined(__ANDROID__)
// Where the store lives on Android when the data folder is left at its
// default. SDL's pref path is INTERNAL storage (/data/data/<pkg>/files), which
// no file manager can reach on an unrooted device — and a browsable import
// folder is the whole point of the feature for sites that refuse downloads.
// App-specific external storage is browsable, needs no permission, and is
// where the screenshot dump already writes for the same reason.
std::filesystem::path android_default_guides_root() {
    if (const char* ext = SDL_GetAndroidExternalStoragePath()) {
        return std::filesystem::path(ext) / "guides";
    }
    return {};
}
#endif


std::filesystem::path guides_root() {
#if defined(__ANDROID__)
    // Only when the data folder is at its default. Once the user has pointed
    // Dusklight somewhere specific, guides belong there with the saves and
    // textures rather than staying behind in app storage — that folder is one
    // they chose and can reach, so the browsability argument above no longer
    // applies. Deliberately not cached: a data-path change needs a restart
    // (is_data_path_restart_pending), but the tests re-point ConfigPath.
    if (dusk::data::is_default_data_path()) {
        const std::filesystem::path ext = android_default_guides_root();
        if (!ext.empty()) {
            return ext;
        }
    }
#endif
    return dusk::ConfigPath / "guides";
}

bool migrate_store_dir(const std::filesystem::path& from, const std::filesystem::path& to) {
    // Platform-independent on purpose: the policy of WHEN to move is Android's
    // (see migrate_store_if_needed), but the move itself is the part that can
    // lose a user's whole library, so it is kept testable everywhere.
    std::error_code ec;
    if (from.empty() || to.empty() || from == to) {
        return false;
    }
    if (!std::filesystem::exists(from / kIndexFile, ec)) {
        return false;  // nothing to bring
    }
    if (std::filesystem::exists(to / kIndexFile, ec)) {
        return false;  // destination already has a store; leave both alone
    }
    std::filesystem::create_directories(to.parent_path(), ec);
    ec.clear();
    std::filesystem::rename(from, to, ec);
    if (!ec) {
        DuskLog.info("guide store moved to {}", to.string());
        return true;
    }
    // Different mount points — app storage and a card are the real case.
    // Copy first, and only drop the original once the copy is known good:
    // a half-finished copy that deleted its source is a destroyed library.
    ec.clear();
    std::filesystem::copy(from, to,
        std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing,
        ec);
    if (ec || !std::filesystem::exists(to / kIndexFile, ec)) {
        // The library still exists at `from`; nothing was deleted. Say so,
        // because from the user's side this looks like every guide vanished.
        DuskLog.warn("guide store could not be moved to {} ({}); it is still at {}",
            to.string(), ec.message(), from.string());
        return false;
    }
    std::error_code rec;
    std::filesystem::remove_all(from, rec);
    DuskLog.info("guide store copied to {}", to.string());
    return true;
}

void migrate_store_if_needed() {
#if defined(__ANDROID__)
    // Switching off the default data folder leaves an existing store behind in
    // app storage. Move it once, rather than silently presenting an empty
    // catalogue and making the user re-save every guide.
    if (dusk::data::is_default_data_path()) {
        return;
    }
    migrate_store_dir(android_default_guides_root(), dusk::ConfigPath / "guides");
#endif
}

std::filesystem::path import_dir() {
    return guides_root() / "import";
}

std::filesystem::path images_dir(const std::string& id) {
    return guides_root() / "images" / id;
}

bool ensure_dirs() {
    std::error_code ec;
    std::filesystem::create_directories(import_dir() / "done", ec);
    return !ec;
}

Index load_index() {
    Index out;
    const auto text = read_file(guides_root() / kIndexFile);
    if (!text) {
        return out;
    }
    try {
        const json j = json::parse(*text);
        if (!j.is_object() || !j.contains("guides") || !j["guides"].is_array()) {
            return out;
        }
        for (const auto& g : j["guides"]) {
            if (!g.is_object() || !g.contains("id")) {
                continue;
            }
            IndexEntry e;
            e.id = g.value("id", std::string());
            if (!id_is_safe(e.id)) {
                continue;
            }
            e.title = g.value("title", std::string());
            e.sourceUrl = g.value("url", std::string());
            if (g.contains("sections") && g["sections"].is_array()) {
                for (const auto& s : g["sections"]) {
                    if (!s.is_object()) {
                        continue;
                    }
                    e.sections.push_back(
                        {s.value("id", std::string()), s.value("title", std::string())});
                }
            }
            out.entries.push_back(std::move(e));
        }
        out.converter = j.value("converter", 0);
    } catch (const std::exception&) {
        // A corrupt index must not stop the reader from opening; the user can
        // re-import, and a rewrite will replace it.
        return Index{};
    }
    return out;
}

bool save_index(const Index& index) {
    json j = json::object();
    j["version"] = kIndexVersion;
    // The caller's value, NOT kConverterVersion: silently stamping "current"
    // on every write would mark guides as freshly converted the moment
    // anything touched the index, and the re-convert would never trigger.
    j["converter"] = index.converter;
    json arr = json::array();
    for (const IndexEntry& e : index.entries) {
        json g = json::object();
        g["id"] = e.id;
        g["title"] = e.title;
        g["url"] = e.sourceUrl;
        json secs = json::array();
        for (const IndexSection& s : e.sections) {
            secs.push_back(json{{"id", s.id}, {"title", s.title}});
        }
        g["sections"] = std::move(secs);
        arr.push_back(std::move(g));
    }
    j["guides"] = std::move(arr);
    return write_atomic(guides_root() / kIndexFile, j.dump(2));
}

bool save_document(const std::string& id, const Document& doc) {
    if (!id_is_safe(id)) {
        return false;
    }
    return write_atomic(guides_root() / (id + ".guide"), serialize(doc));
}

std::optional<Document> load_document(const std::string& id) {
    if (!id_is_safe(id)) {
        return std::nullopt;
    }
    const auto text = read_file(guides_root() / (id + ".guide"));
    if (!text) {
        return std::nullopt;
    }
    return deserialize(*text);
}


std::string import_html_file(const std::filesystem::path& file, const std::string& sourceUrl,
    const ImageSource& netFallback) {
    const auto html = read_file(file);
    if (!html) {
        return {};
    }
    // The in-app browser writes the page address as a leading comment, since a
    // rendered DOM carries no other record of where it came from and relative
    // image links are meaningless without it.
    std::string url = sourceUrl;
    if (url.empty()) {
        const std::string& h = *html;
        const std::string tag = "<!-- dusk-source: ";
        if (h.compare(0, tag.size(), tag) == 0) {
            const std::size_t end = h.find(" -->", tag.size());
            if (end != std::string::npos) {
                url = h.substr(tag.size(), end - tag.size());
            }
        }
    }
    // Browsers save "page.html" alongside a "page_files/" directory holding the
    // images, so a locally-saved page already carries everything it needs. The
    // ref is looked up both as written (document-relative) and by basename in
    // that folder, because browsers rewrite some names on save.
    const std::filesystem::path dir = file.parent_path();
    const std::filesystem::path assets = dir / (file.stem().string() + "_files");
    ImageSource local = [dir, assets, netFallback](const std::string& url, std::string& out) {
        std::error_code ec;
        // `url` has been through resolve_url, so recover the trailing path.
        std::string rel = url;
        const std::size_t schemeEnd = rel.find("://");
        if (schemeEnd != std::string::npos) {
            const std::size_t hostEnd = rel.find('/', schemeEnd + 3);
            rel = hostEnd == std::string::npos ? std::string() : rel.substr(hostEnd + 1);
        }
        const std::string base = image_filename(url);
        const std::filesystem::path candidates[] = {
            assets / base,
            dir / base,
            rel.empty() ? std::filesystem::path() : dir / rel,
        };
        for (const auto& c : candidates) {
            if (c.empty() || !std::filesystem::exists(c, ec)) {
                continue;
            }
            if (const auto bytes = read_file(c)) {
                out = *bytes;
                return true;
            }
        }
        // Not saved alongside the page: go and get it.
        return netFallback ? netFallback(url, out) : false;
    };
    return import_html_content(*html, url, file.stem().string(), local);
}

std::string import_html_content(const std::string& html, const std::string& sourceUrl,
    const std::string& fallbackName, const ImageSource& images) {
    Document doc = convert_html(html, sourceUrl);
    // Sections alone are not content. The site's chapter-index page survives
    // conversion as a single section holding its <h1> and nothing else, once
    // its "Chapter N - ..." headings are dropped as navigation — so counting
    // sections filed it as a guide with 22 empty entries. Require an actual
    // body node somewhere.
    std::size_t nodes = 0;
    for (const auto& sec : doc.sections) {
        nodes += sec.nodes.size();
    }
    if (doc.sections.empty() || nodes == 0) {
        return {};  // nothing recognisable — do not file an empty guide
    }
    // The site's walkthrough index page: a hub of links to the chapters. It
    // survives everything above — its "Chapter N" links are dropped as
    // navigation, but the rest of the page is not, so ~90 nodes remain.
    //
    // Content ratios do NOT separate it from a chapter (it is 69 images out of
    // 91 nodes, which looks exactly like one). What does: it has no subheadings
    // at all, so it converts to a single section named after the page, and its
    // body is a link list rather than prose. Every one of the 22 saved chapters
    // produces between 2 and 10 sections.
    //
    // Deliberately requires BOTH conditions. A genuine one-page walkthrough is
    // also a single title-named section, and rejecting those outright would be
    // wrong — so the link-list test is what actually discriminates.
    if (doc.sections.size() == 1 && doc.sections.front().title == doc.title) {
        std::size_t items = 0;
        std::size_t paras = 0;
        for (const auto& n : doc.sections.front().nodes) {
            if (n.kind == NodeKind::ListItem) {
                items++;
            } else if (n.kind == NodeKind::Paragraph) {
                paras++;
            }
        }
        if (items > paras) {
            return {};  // link index, not a walkthrough
        }
    }
    // Prefer the document's own title for the id; fall back to the filename so
    // a titleless page is still importable.
    std::string id = slugify(doc.title.empty() ? fallbackName : doc.title);
    if (!id_is_safe(id)) {
        id = "guide";
    }
    // Acquire images. Best-effort by design: a page whose pictures cannot be
    // had is still a perfectly usable guide, so every failure here is silent
    // and the reader shows alt text instead.
    {
        std::error_code iec;
        const std::filesystem::path imgDir = images_dir(id);
        std::filesystem::create_directories(imgDir, iec);
        int got = 0;
        // Sites behind bot protection refuse IMAGES as well as pages, and a
        // chapter references ~190 of them. Trying every one costs a network
        // round trip each — ~30 minutes across a guide, which reads as a hang.
        // A short run of failures is enough to conclude the host is not
        // serving us, and the browser has saved them locally anyway.
        int consecutiveFails = 0;
        constexpr int kGiveUpAfter = 5;
        // NON-const: every ref is rewritten to the filename it is stored under.
        // Two bugs collapse into this. (1) The reader derived the filename its
        // own way (bare basename, no query strip, no data: handling) and
        // disagreed with the store, so anything but a crawler-rewritten src
        // missed forever. (2) An inlined data: URI was serialised VERBATIM into
        // the .guide, duplicating every image as base64 and re-parsing it on
        // every document load.
        for (Section& sec : doc.sections) {
            // Images are prefixed with their section: "13.1-img20.jpg". The
            // site names them per PAGE (img1..img190), so a chapter folder was
            // a flat run of numbers with nothing tying a file to the part of
            // the walkthrough it illustrates. The prefix makes the folder
            // browsable by hand, which is the whole reason it lives in external
            // storage.
            const std::string prefix = section_number(sec.title);
            for (Node& n : sec.nodes) {
                if (n.kind != NodeKind::Image || n.ref.empty()) {
                    continue;
                }
                const std::string bare = image_filename(n.ref);

                if (bare.empty()) {
                    continue;
                }
                const std::string file = prefix.empty() ? bare : prefix + "-" + bare;
                const std::filesystem::path dest = imgDir / file;
                // Upgrade in place. An existing library is already on disk
                // under the unprefixed name, and re-fetching it is not an
                // option — the site refuses image requests from anything that
                // is not a browser, so the alternative is losing every picture
                // until the user re-saves all 22 chapters by hand.
                if (!prefix.empty()) {
                    std::error_code mec;
                    if (!std::filesystem::exists(dest, mec) &&
                        std::filesystem::exists(imgDir / bare, mec))
                    {
                        std::filesystem::rename(imgDir / bare, dest, mec);
                        if (mec) {
                            // Silently this reads as "the picture disappeared":
                            // the ref now points at a name that is not there and
                            // the reader falls back to alt text.
                            DuskLog.warn("guide image {} could not be renamed to {}: {}",
                                bare, file, mec.message());
                        }
                    }
                }
                // Rewritten whether or not the bytes arrive: the reader then
                // looks up exactly what the store wrote, and the .guide never
                // carries a payload.
                const std::string origRef = n.ref;
                n.ref = file;
                if (std::filesystem::exists(dest, iec)) {
                    // Have the bytes already, but still need the size: this
                    // branch used to `continue` outright, so a re-import of a
                    // page whose images were on disk recorded 0x0 for every
                    // one of them and the reader reserved a single line of
                    // alt-text height where a 400px screenshot was about to
                    // draw. Header parse only, no decode.
                    dusk::guide::probe_image_size(dest, &n.imgW, &n.imgH);
                    continue;
                }
                if (!images) {
                    continue;
                }
                std::string bytes;
                const bool inlineData = origRef.compare(0, 5, "data:") == 0;
                if (!inlineData && consecutiveFails >= kGiveUpAfter) {
                    continue;  // host is refusing; stop asking
                }
                if (inlineData) {
                    // Inlined by the browser: the bytes are right here.
                    const std::size_t comma = origRef.find(',');
                    if (comma == std::string::npos) {
                        continue;
                    }
                    bytes = base64_decode(std::string_view(origRef).substr(comma + 1));
                } else {
                    const std::string url = resolve_url(sourceUrl, origRef);
                    if (url.empty() || !images) {
                        continue;
                    }
                    if (!images(url, bytes)) {
                        consecutiveFails++;
                        continue;
                    }
                    consecutiveFails = 0;
                }
                if (bytes.empty()) {
                    continue;
                }
                // Undecodable formats (SVG and friends) are refused here and
                // the reader shows alt text instead.
                if (dusk::guide::store_image_file(bytes, dest, &n.imgW, &n.imgH)) {
                    got++;
                }
            }
        }
        (void)got;
    }

    if (!save_document(id, doc)) {
        return {};
    }

    IndexEntry entry;
    entry.id = id;
    entry.title = doc.title.empty() ? id : doc.title;
    entry.sourceUrl = sourceUrl;
    for (const Section& s : doc.sections) {
        entry.sections.push_back({s.id, s.title});
    }
    Index idx = load_index();
    bool replaced = false;
    for (IndexEntry& e : idx.entries) {
        if (e.id == id) {
            e = entry;  // re-importing the same page updates in place
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        idx.entries.push_back(std::move(entry));
    }
    if (!save_index(idx)) {
        return {};
    }
    return id;
}

std::string section_number(const std::string& title) {
    std::size_t i = 0;
    while (i < title.size() && title[i] >= '0' && title[i] <= '9') {
        i++;
    }
    if (i == 0 || i >= title.size() || title[i] != '.') {
        return {};
    }
    const std::size_t dot = i++;
    while (i < title.size() && title[i] >= '0' && title[i] <= '9') {
        i++;
    }
    if (i == dot + 1) {
        return {};  // "13." with no minor number
    }
    return title.substr(0, i);
}

bool index_needs_reconvert() {
    return load_index().converter < kConverterVersion;
}

int scan_import_folder(const ImageSource& netFallback) {
    ensure_dirs();
    std::error_code ec;
    const auto dir = import_dir();
    if (!std::filesystem::exists(dir, ec)) {
        return 0;
    }
    int count = 0;
    std::vector<std::filesystem::path> pending;

    // Both scans below use this. They used to disagree -- the live folder
    // required .html/.htm while the archive accepted any regular file, so a
    // stray .tmp or a note dropped into done/ was handed to the converter on
    // every re-convert.
    auto is_page = [](const std::filesystem::directory_entry& de) {
        if (!de.is_regular_file()) {
            return false;
        }
        std::string ext = de.path().extension().string();
        for (char& c : ext) {
            c = (char)std::tolower((unsigned char)c);
        }
        return ext == ".html" || ext == ".htm";
    };

    // Converter moved on: bring the archived sources back through it. Cheap to
    // detect, and it means a parser fix actually reaches existing guides
    // instead of only new ones.
    // Before anything reads the catalogue: if the data folder moved, the store
    // has to come with it or this scan sees an empty one and re-imports from
    // scratch (or, worse, finds nothing to import and reports no guides).
    migrate_store_if_needed();

    const bool reconverting = load_index().converter < kConverterVersion;
    if (reconverting) {
        for (const auto& de : std::filesystem::directory_iterator(dir / "done", ec)) {
            if (ec) {
                break;
            }
            if (is_page(de)) {
                pending.push_back(de.path());
            }
        }
    }
    for (const auto& de : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) {
            break;
        }
        if (is_page(de)) {
            pending.push_back(de.path());
        }
    }
    // Collected first, then processed: converting while iterating the same
    // directory we move files out of is undefined.
    std::vector<std::string> produced;
    int skipped = 0;
    for (const auto& p : pending) {
        const std::string id = import_html_file(p, std::string(), netFallback);
        if (id.empty()) {
            skipped++;
            continue;
        }
        produced.push_back(id);
        // Already archived (a re-convert): leave it where it is.
        if (p.parent_path().filename() != "done") {
            std::filesystem::path dest = dir / "done" / p.filename();
            std::filesystem::remove(dest, ec);
            std::filesystem::rename(p, dest, ec);
        }
        count++;
    }
    {
        Index idx = load_index();
        // A parse failure also reads back as an empty Index. Stamping that
        // would write a catalogue with zero guides AND the current converter —
        // the .guide files survive but nothing references them, and because the
        // stamp now matches, the re-convert that would rebuild it never fires
        // again. Refuse to stamp when the file exists but read back empty.
        std::error_code sec;
        const bool suspect = idx.entries.empty() &&
            std::filesystem::exists(guides_root() / kIndexFile, sec);
        // A re-convert regenerates every archived source, so anything left in
        // the catalogue that this pass did NOT produce is stale: its page no
        // longer converts to anything (the site's chapter-index page is the
        // real case — it is pure navigation and now yields no sections), or
        // its source is gone. Without this, a converter fix could stop
        // creating a bad guide but could never remove one already stored.
        //
        // Gated on having produced something, so a pass that failed wholesale
        // — unreadable directory, no sources archived — never empties the
        // catalogue on the strength of its own failure.
        if (!suspect && reconverting && !produced.empty()) {
            std::vector<IndexEntry> kept;
            for (auto& e : idx.entries) {
                if (std::find(produced.begin(), produced.end(), e.id) != produced.end()) {
                    kept.push_back(std::move(e));
                } else {
                    // Worth a line: a guide vanishing from the list is exactly
                    // the kind of thing that gets reported as data loss.
                    DuskLog.info("dropping guide '{}': its page no longer converts", e.id);
                    std::error_code rec;
                    std::filesystem::remove(guides_root() / (e.id + ".guide"), rec);
                    std::filesystem::remove_all(images_dir(e.id), rec);
                }
            }
            // Assigned UNCONDITIONALLY. Guarding this on "did anything
            // change" was wrong and self-inflicted: the loop above moves every
            // surviving entry into `kept`, so skipping the assignment left
            // idx.entries holding moved-from husks with empty ids — which then
            // got saved, wiping the catalogue.
            idx.entries = std::move(kept);
        }
        if (!suspect && (count > 0 || idx.converter < kConverterVersion)) {
            idx.converter = kConverterVersion;
            save_index(idx);
        }
    }
    if (!pending.empty()) {
        DuskLog.info("guide import: {} page(s) converted, {} skipped, {} in catalogue",
            count, skipped, (int)load_index().entries.size());
    }
    return count;
}

}  // namespace dusk::guide
