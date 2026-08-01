#include "dusk/guide/guide_doc.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dusk::guide {
namespace {

// --- small helpers ----------------------------------------------------------

bool ieq(const std::string& a, const char* b) {
    const std::size_t n = std::strlen(b);
    if (a.size() != n) {
        return false;
    }
    for (std::size_t i = 0; i < n; i++) {
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) {
            return false;
        }
    }
    return true;
}

// Whitespace runs collapse to one space, and leading/trailing go. HTML treats
// newlines as spaces, and the wrapper downstream does its own line breaking.
std::string collapse_ws(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    bool pending = false;
    for (const char c : in) {
        const bool ws = c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
            c == '\v' || (unsigned char)c == 0xA0;
        if (ws) {
            pending = !out.empty();
            continue;
        }
        if (pending) {
            out.push_back(' ');
            pending = false;
        }
        out.push_back(c);
    }
    return out;
}

void append_utf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
        out.push_back((char)cp);
    } else if (cp < 0x800) {
        out.push_back((char)(0xC0 | (cp >> 6)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back((char)(0xE0 | (cp >> 12)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else {
        out.push_back((char)(0xF0 | (cp >> 18)));
        out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    }
}

struct NamedEntity {
    const char* name;
    std::uint32_t cp;
};

// Only the entities that actually appear in prose. An unknown entity is left
// verbatim rather than dropped, so a converter gap shows up as visible
// "&foo;" in the reader instead of silently eaten text.
constexpr NamedEntity kEntities[] = {
    {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''},
    {"nbsp", 0xA0}, {"ndash", 0x2013}, {"mdash", 0x2014}, {"lsquo", 0x2018},
    {"rsquo", 0x2019}, {"ldquo", 0x201C}, {"rdquo", 0x201D}, {"hellip", 0x2026},
    {"times", 0xD7}, {"deg", 0xB0}, {"eacute", 0xE9}, {"egrave", 0xE8},
    {"agrave", 0xE0}, {"ccedil", 0xE7}, {"uuml", 0xFC}, {"ouml", 0xF6},
    {"auml", 0xE4}, {"szlig", 0xDF}, {"ntilde", 0xF1}, {"iacute", 0xED},
    {"oacute", 0xF3}, {"aacute", 0xE1}, {"uacute", 0xFA}, {"middot", 0xB7},
    {"bull", 0x2022}, {"rarr", 0x2192}, {"larr", 0x2190}, {"copy", 0xA9},
};

}  // namespace

std::string decode_entities(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size();) {
        if (in[i] != '&') {
            out.push_back(in[i++]);
            continue;
        }
        const std::size_t semi = in.find(';', i + 1);
        // A bare '&' (or one absurdly far from its ';') is literal text.
        if (semi == std::string::npos || semi - i > 12) {
            out.push_back(in[i++]);
            continue;
        }
        const std::string body = in.substr(i + 1, semi - i - 1);
        bool done = false;
        if (!body.empty() && body[0] == '#') {
            std::uint32_t cp = 0;
            bool ok = body.size() > 1;
            if (body.size() > 2 && (body[1] == 'x' || body[1] == 'X')) {
                for (std::size_t k = 2; k < body.size() && ok; k++) {
                    const char c = body[k];
                    cp *= 16;
                    if (c >= '0' && c <= '9') {
                        cp += (std::uint32_t)(c - '0');
                    } else if ((c | 32) >= 'a' && (c | 32) <= 'f') {
                        cp += (std::uint32_t)((c | 32) - 'a' + 10);
                    } else {
                        ok = false;
                    }
                }
            } else {
                for (std::size_t k = 1; k < body.size() && ok; k++) {
                    if (body[k] < '0' || body[k] > '9') {
                        ok = false;
                    } else {
                        cp = cp * 10 + (std::uint32_t)(body[k] - '0');
                    }
                }
            }
            if (ok && cp != 0 && cp < 0x110000) {
                append_utf8(out, cp);
                done = true;
            }
        } else {
            for (const NamedEntity& e : kEntities) {
                if (ieq(body, e.name)) {
                    append_utf8(out, e.cp);
                    done = true;
                    break;
                }
            }
        }
        if (done) {
            i = semi + 1;
        } else {
            out.push_back(in[i++]);  // leave unknown entities visible
        }
    }
    return out;
}

std::string utf8_to_latin1(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size();) {
        const unsigned char c = (unsigned char)in[i];
        std::uint32_t cp = 0;
        int len = 1;
        if (c < 0x80) {
            cp = c;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < in.size()) {
            cp = ((std::uint32_t)(c & 0x1F) << 6) | (in[i + 1] & 0x3F);
            len = 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < in.size()) {
            cp = ((std::uint32_t)(c & 0x0F) << 12) |
                ((std::uint32_t)(in[i + 1] & 0x3F) << 6) | (in[i + 2] & 0x3F);
            len = 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < in.size()) {
            cp = ((std::uint32_t)(c & 0x07) << 18) |
                ((std::uint32_t)(in[i + 1] & 0x3F) << 12) |
                ((std::uint32_t)(in[i + 2] & 0x3F) << 6) | (in[i + 3] & 0x3F);
            len = 4;
        } else {
            // Invalid lead byte: treat as LATIN-1 already, which is the common
            // case for a page that lied about its charset.
            cp = c;
        }
        i += (std::size_t)len;

        if (cp == 0xA0) {
            out.push_back(' ');  // nbsp — the wrapper must be able to break here
        } else if (cp < 0x100) {
            out.push_back((char)cp);
        } else {
            // No LATIN-1 glyph. Fold to a readable stand-in rather than drawing
            // the font's default box.
            switch (cp) {
            case 0x2018:
            case 0x2019:
            case 0x201B:
                out.push_back('\'');
                break;
            case 0x201C:
            case 0x201D:
                out.push_back('"');
                break;
            case 0x2013:
            case 0x2014:
            case 0x2212:
                out.push_back('-');
                break;
            case 0x2026:
                out.append("...");
                break;
            case 0x2022:
            case 0x25CF:
                out.push_back('*');
                break;
            case 0x2192:
                out.append("->");
                break;
            case 0x2190:
                out.append("<-");
                break;
            case 0x00A0:
                out.push_back(' ');
                break;
            default:
                break;  // dropped: a box glyph is worse than nothing
            }
        }
    }
    return out;
}

std::string resolve_url(const std::string& base, const std::string& ref) {
    if (ref.empty() || ref.compare(0, 5, "data:") == 0) {
        return {};  // inline payloads are not worth carrying
    }
    if (ref.compare(0, 8, "https://") == 0 || ref.compare(0, 7, "http://") == 0) {
        return ref;
    }
    if (ref.compare(0, 2, "//") == 0) {
        return "https:" + ref;  // protocol-relative
    }
    // Everything below needs a base to hang off.
    const std::size_t schemeEnd = base.find("://");
    if (schemeEnd == std::string::npos) {
        return {};
    }
    const std::size_t hostEnd = base.find('/', schemeEnd + 3);
    const std::string origin =
        hostEnd == std::string::npos ? base : base.substr(0, hostEnd);
    if (ref[0] == '/') {
        return origin + ref;
    }
    // Document-relative: drop the base's filename, keep its directory.
    std::string dir = hostEnd == std::string::npos ? base + "/" : base;
    const std::size_t lastSlash = dir.find_last_of('/');
    if (lastSlash != std::string::npos && lastSlash > schemeEnd + 2) {
        dir = dir.substr(0, lastSlash + 1);
    }
    return dir + ref;
}

std::string image_filename(const std::string& ref) {
    // Inline data: images have no path to take a name from, so the name comes
    // from the payload itself — stable across re-imports, and identical
    // payloads collapse to one file.
    if (ref.compare(0, 5, "data:") == 0) {
        std::uint64_t h = 1469598103934665603ull;
        for (const char c : ref) {
            h ^= (unsigned char)c;
            h *= 1099511628211ull;
        }
        char buf[32];
        std::snprintf(buf, sizeof(buf), "img_%016llx.bin", (unsigned long long)h);
        return buf;
    }
    std::string name = ref;
    const std::size_t q = name.find_first_of("?#");
    if (q != std::string::npos) {
        name.erase(q);
    }
    const std::size_t slash = name.find_last_of('/');
    if (slash != std::string::npos) {
        name.erase(0, slash + 1);
    }
    // Same safe set the store enforces on ids, plus '.' and '_' so extensions
    // and typical CDN names survive.
    std::string out;
    for (const char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        out.push_back(ok ? c : '_');
    }
    if (out.size() > 80) {
        out.erase(0, out.size() - 80);
    }
    return out;
}

std::string slugify(const std::string& title) {
    std::string out;
    out.reserve(title.size());
    bool dash = false;
    for (const char c : title) {
        const unsigned char u = (unsigned char)c;
        if (std::isalnum(u)) {
            out.push_back((char)std::tolower(u));
            dash = false;
        } else if (!out.empty()) {
            dash = true;
        }
        if (dash && !out.empty() && out.back() != '-') {
            out.push_back('-');
            dash = false;
        }
    }
    while (!out.empty() && out.back() == '-') {
        out.pop_back();
    }
    return out.empty() ? std::string("section") : out;
}

// --- the scanner ------------------------------------------------------------

Document convert_html(const std::string& html, const std::string& sourceUrl) {
    Document doc;
    doc.sourceUrl = sourceUrl;

    Section cur;
    std::string text;        // accumulating inline text for the current block
    NodeKind kind = NodeKind::Paragraph;
    std::uint8_t level = 0;
    int listDepth = 0;

    // "Chapter 4 - Forest Temple", "4.1 - Dungeon Map", "Chapter 5": the site's
    // own navigation. Deliberately strict about what follows the number — a
    // real section heading like "13.1 First Poe Soul" has prose there, not a
    // dash or the end of the string, so it is kept.
    auto is_nav_label = [](const std::string& t) {
        const char* p2 = t.c_str();
        if (std::strncmp(p2, "Chapter ", 8) == 0) {
            p2 += 8;
        }
        int digits = 0;
        while (*p2 >= '0' && *p2 <= '9') {
            p2++;
            digits++;
        }
        if (digits > 0 && *p2 == '.') {
            p2++;
            while (*p2 >= '0' && *p2 <= '9') {
                p2++;
            }
        }
        while (*p2 == ' ') {
            p2++;
        }
        return digits > 0 && (*p2 == '-' || *p2 == '\0');
    };

    auto flush = [&]() {
        const std::string t = collapse_ws(utf8_to_latin1(decode_entities(text)));
        text.clear();
        if (t.empty()) {
            return;
        }
        Node n;
        n.kind = kind;
        n.level = level;
        n.text = t;
        // An h1/h2 opens a new section; deeper headings stay inside one.
        if (kind == NodeKind::Heading && level <= 2) {
            // The site's hub page lists every chapter as its own <h2>
            // ("Chapter 1 - The Twilight" ... "Chapter 22 - ..."), so importing
            // it produced a guide whose 22 "sections" were pure navigation.
            // Dropping the heading means no section opens for it; a page that
            // is nothing BUT these ends up with no sections at all and is
            // therefore never stored, which is the desired outcome.
            if (is_nav_label(t)) {
                return;
            }
            if (!cur.nodes.empty() || !cur.title.empty()) {
                doc.sections.push_back(std::move(cur));
                cur = Section{};
            }
            cur.title = t;
            cur.id = slugify(t);
            if (doc.title.empty()) {
                doc.title = t;
            }
            return;  // the section title is not also a body node
        }
        // Site table of contents, repeated verbatim on every chapter page:
        // "Chapter 4 - Forest Temple", "4.1 - Dungeon Map". It is navigation,
        // not walkthrough text, and the reader already has a section list of
        // its own, so it is dropped rather than shown twice.
        if (n.kind == NodeKind::ListItem && is_nav_label(t)) {
            return;
        }
        cur.nodes.push_back(std::move(n));
    };

    for (std::size_t i = 0; i < html.size();) {
        if (html[i] != '<') {
            // Accumulated ALWAYS, not only inside a recognised block tag.
            // Real pages put prose in <section>, <article>, bare containers and
            // inline wrappers, and gating on a known-tag whitelist silently
            // dropped all of it — a chapter came through as nothing but its
            // images. Block tags below delimit; the skip list above is what
            // keeps nav and script text out.
            text.push_back(html[i]);
            i++;
            continue;
        }
        // Comments and doctype.
        if (html.compare(i, 4, "<!--") == 0) {
            const std::size_t e = html.find("-->", i + 4);
            i = e == std::string::npos ? html.size() : e + 3;
            continue;
        }
        const std::size_t close = html.find('>', i);
        if (close == std::string::npos) {
            break;
        }
        std::string tag = html.substr(i + 1, close - i - 1);
        i = close + 1;
        if (tag.empty()) {
            continue;
        }
        if (tag[0] == '!' || tag[0] == '?') {
            continue;
        }
        const bool closing = tag[0] == '/';
        if (closing) {
            tag.erase(0, 1);
        }
        // Split name from attributes.
        std::string name;
        std::string attrs;
        {
            std::size_t k = 0;
            while (k < tag.size() && !std::isspace((unsigned char)tag[k]) && tag[k] != '/') {
                name.push_back((char)std::tolower((unsigned char)tag[k]));
                k++;
            }
            attrs = tag.substr(k);
        }

        // Subtrees that never contain guide prose. Skipping them wholesale is
        // what keeps the scanner tolerant — no need to understand their markup.
        if (!closing &&
            (name == "script" || name == "style" || name == "head" || name == "nav" ||
                name == "footer" || name == "aside" || name == "noscript" || name == "svg"))
        {
            const std::string endTag = "</" + name;
            const std::size_t e = html.find(endTag, i);
            i = e == std::string::npos ? html.size() : e;
            continue;
        }

        if (name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6') {
            flush();
            if (closing) {
                // MUST reset. Leaving kind == Heading meant any bare text
                // between </h2> and the next block tag was flushed AS A
                // HEADING — so a stray line after a heading became its own
                // section (and, if it was the first, the document title and
                // therefore the guide id). Every other block tag resets on
                // open, so headings were uniquely affected.
                kind = NodeKind::Paragraph;
                level = 0;
            } else {
                kind = NodeKind::Heading;
                level = (std::uint8_t)(name[1] - '0');
            }
            continue;
        }
        if (name == "p" || name == "div" || name == "blockquote" || name == "td" ||
            name == "section" || name == "article" || name == "main" ||
            name == "figcaption" || name == "dd" || name == "dt" || name == "th" ||
            name == "tr" || name == "pre")
        {
            flush();
            if (!closing) {
                kind = NodeKind::Paragraph;
                level = 0;
            }
            continue;
        }
        if (name == "ul" || name == "ol") {
            flush();
            listDepth += closing ? -1 : 1;
            if (listDepth < 0) {
                listDepth = 0;
            }
            continue;
        }
        if (name == "li") {
            flush();
            if (!closing) {
                kind = NodeKind::ListItem;
                level = (std::uint8_t)(listDepth < 1 ? 1 : listDepth);
            }
            continue;
        }
        if (name == "br") {
            flush();
            kind = NodeKind::Paragraph;
            level = 0;
            continue;
        }
        if (name == "hr") {
            flush();
            Node n;
            n.kind = NodeKind::Rule;
            cur.nodes.push_back(n);
            continue;
        }
        if (name == "img" && !closing) {
            // Images are their own block; alt text is kept so a skipped or
            // undecodable image still says what it was.
            auto attr = [&](const char* key) -> std::string {
                const std::string k = std::string(key) + "=";
                std::size_t p = 0;
                for (;;) {
                    p = attrs.find(k, p);
                    if (p == std::string::npos) {
                        return {};
                    }
                    const bool atStart = p == 0 || std::isspace((unsigned char)attrs[p - 1]);
                    p += k.size();
                    if (!atStart || p >= attrs.size()) {
                        continue;
                    }
                    const char q = attrs[p];
                    if (q == '"' || q == '\'') {
                        const std::size_t e = attrs.find(q, p + 1);
                        return e == std::string::npos ? std::string()
                                                      : attrs.substr(p + 1, e - p - 1);
                    }
                    std::size_t e = p;
                    while (e < attrs.size() && !std::isspace((unsigned char)attrs[e])) {
                        e++;
                    }
                    return attrs.substr(p, e - p);
                }
            };
            const std::string src = attr("src");
            if (!src.empty()) {
                flush();
                Node n;
                n.kind = NodeKind::Image;
                n.ref = src;
                n.text = collapse_ws(utf8_to_latin1(decode_entities(attr("alt"))));
                cur.nodes.push_back(std::move(n));
            }
            continue;
        }
        // Everything else (a, span, strong, em, ...) is inline: its text is
        // already being accumulated, so the tag itself is simply dropped.
    }
    flush();
    if (!cur.nodes.empty() || !cur.title.empty()) {
        doc.sections.push_back(std::move(cur));
    }
    // Content that appears before the first heading (lead paragraphs, infobox
    // prose) lands in a section with no title, and therefore no slug. An empty
    // id is not a usable filename stem or progress key and id_is_safe() would
    // reject it, so give it a stable one. Sections that are empty AND untitled
    // are page furniture, not content — drop them.
    // Content before the first heading is page furniture on a real site — nav
    // links, breadcrumbs, a stray byline — not guide text, and it cannot be
    // named or jumped to because it has no heading to slug. Dropped outright.
    for (auto it = doc.sections.begin(); it != doc.sections.end();) {
        if (it->id.empty()) {
            it = doc.sections.erase(it);
        } else {
            ++it;
        }
    }
    return doc;
}

// --- serialisation ----------------------------------------------------------

namespace {

std::string escape_line(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        if (c == '\n') {
            out.append("\\n");
        } else if (c == '\\') {
            out.append("\\\\");
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::string unescape_line(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            out.push_back(s[++i] == 'n' ? '\n' : s[i]);
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

}  // namespace

std::string serialize(const Document& doc) {
    std::string out;
    out += "!title " + escape_line(doc.title) + "\n";
    out += "!url " + escape_line(doc.sourceUrl) + "\n";
    for (const Section& s : doc.sections) {
        out += "#" + s.id + "|" + escape_line(s.title) + "\n";
        for (const Node& n : s.nodes) {
            char pre[16];
            switch (n.kind) {
            case NodeKind::Heading:
                std::snprintf(pre, sizeof(pre), "H%u|", (unsigned)n.level);
                out += pre + escape_line(n.text) + "\n";
                break;
            case NodeKind::Paragraph:
                out += "P|" + escape_line(n.text) + "\n";
                break;
            case NodeKind::ListItem:
                std::snprintf(pre, sizeof(pre), "L%u|", (unsigned)n.level);
                out += pre + escape_line(n.text) + "\n";
                break;
            case NodeKind::Image:
                std::snprintf(pre, sizeof(pre), "|%d|%d|", n.imgW, n.imgH);
                out += "I|" + escape_line(n.ref) + pre + escape_line(n.text) + "\n";
                break;
            case NodeKind::Rule:
                out += "-\n";
                break;
            }
        }
    }
    return out;
}

Document deserialize(const std::string& text) {
    Document doc;
    Section cur;
    bool have = false;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t nl = text.find('\n', pos);
        const std::string line =
            text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? text.size() + 1 : nl + 1;
        if (line.empty()) {
            continue;
        }
        if (line.compare(0, 7, "!title ") == 0) {
            doc.title = unescape_line(line.substr(7));
            continue;
        }
        if (line.compare(0, 5, "!url ") == 0) {
            doc.sourceUrl = unescape_line(line.substr(5));
            continue;
        }
        if (line[0] == '#') {
            if (have) {
                doc.sections.push_back(std::move(cur));
                cur = Section{};
            }
            have = true;
            const std::size_t bar = line.find('|');
            cur.id = line.substr(1, bar == std::string::npos ? std::string::npos : bar - 1);
            cur.title = bar == std::string::npos ? std::string() : unescape_line(line.substr(bar + 1));
            continue;
        }
        if (line == "-") {
            Node n;
            n.kind = NodeKind::Rule;
            cur.nodes.push_back(n);
            continue;
        }
        const std::size_t bar = line.find('|');
        if (bar == std::string::npos) {
            continue;
        }
        Node n;
        const char c = line[0];
        if (c == 'H' || c == 'L') {
            n.kind = c == 'H' ? NodeKind::Heading : NodeKind::ListItem;
            n.level = (std::uint8_t)(bar > 1 ? line[1] - '0' : 1);
            n.text = unescape_line(line.substr(bar + 1));
        } else if (c == 'P') {
            n.kind = NodeKind::Paragraph;
            n.text = unescape_line(line.substr(bar + 1));
        } else if (c == 'I') {
            n.kind = NodeKind::Image;
            const std::size_t b2 = line.find('|', bar + 1);
            n.ref = line.substr(bar + 1, b2 == std::string::npos ? std::string::npos : b2 - bar - 1);
            if (b2 != std::string::npos) {
                const std::size_t b3 = line.find('|', b2 + 1);
                const std::size_t b4 = b3 == std::string::npos
                    ? std::string::npos : line.find('|', b3 + 1);
                if (b4 != std::string::npos) {
                    n.imgW = std::atoi(line.substr(b2 + 1, b3 - b2 - 1).c_str());
                    n.imgH = std::atoi(line.substr(b3 + 1, b4 - b3 - 1).c_str());
                    n.text = unescape_line(line.substr(b4 + 1));
                } else {
                    n.text = unescape_line(line.substr(b2 + 1));
                }
            }
        } else {
            continue;
        }
        cur.nodes.push_back(std::move(n));
    }
    if (have) {
        doc.sections.push_back(std::move(cur));
    }
    return doc;
}

}  // namespace dusk::guide
