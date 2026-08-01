#pragma once

// Guide document model + HTML importer.
//
// A guide is converted ONCE at import and stored in the compact form below;
// nothing here runs on the draw path. That split is deliberate: the companion
// renders with the game's own 2D primitives at one draw call per glyph, so the
// reader can afford no per-frame parsing, and the render side wants a flat list
// of already-transcoded, already-classified blocks.
//
// The model is deliberately six node kinds, not a DOM. RmlUi ships an XML
// parser but real walkthrough pages are tag soup, not well-formed XML, and a
// tree would buy nothing a flat block list does not already give us.

#include <cstdint>
#include <string>
#include <vector>

namespace dusk::guide {

enum class NodeKind : std::uint8_t {
    Heading,    // level = 1..6
    Paragraph,
    ListItem,   // level = nesting depth, 1-based
    Image,      // ref = stored image file, text = alt text
    Rule,
};

struct Node {
    NodeKind kind = NodeKind::Paragraph;
    std::uint8_t level = 0;
    std::string text;  // LATIN-1, entities resolved, whitespace collapsed
    std::string ref;   // image filename; empty otherwise
    // Pixel size of an Image node, recorded at import. The reader needs the
    // aspect ratio to reserve flow height, and without this it had to DECODE
    // every image in a section just to measure it — 20 JPEG decodes inside one
    // draw call, which then evicted its own cache. 0 = unknown.
    int imgW = 0;
    int imgH = 0;
};

// A section is the unit the reader scrolls and the unit game progress maps
// onto: one dungeon, one chapter. Split at h1/h2 boundaries.
struct Section {
    std::string id;     // slug, stable across re-imports — progress keys off this
    std::string title;
    std::vector<Node> nodes;
};

struct Document {
    std::string title;
    std::string sourceUrl;
    std::vector<Section> sections;
};

// --- Conversion -------------------------------------------------------------

// Converts a raw HTML page into the model above. Tolerant by design: unknown
// tags are ignored, unbalanced markup is survivable, and script/style/nav/
// footer subtrees are dropped wholesale. Never throws; a page it cannot make
// sense of yields a Document with no sections rather than an error.
Document convert_html(const std::string& html, const std::string& sourceUrl);

// UTF-8 -> LATIN-1. Mandatory, not cosmetic: the companion font is single-byte
// LATIN-1 and every width measurement and wrap decision in companion_gfx.cpp is
// per-byte, so a stray multi-byte sequence both draws as garbage boxes AND
// mis-measures the line. Codepoints with no LATIN-1 equivalent are folded to a
// readable ASCII stand-in (curly quotes to straight, dashes to '-', arrows to
// '->'); anything left over is dropped rather than drawn as a box.
std::string utf8_to_latin1(const std::string& in);

// Resolves HTML entities (&amp;, &#8217;, &#x2014;, and the named ones that
// actually turn up in prose). Runs before transcoding, so its output is UTF-8.
std::string decode_entities(const std::string& in);

// --- Serialisation ----------------------------------------------------------
//
// A line-oriented text format rather than JSON: the reader parses it at section
// load, so it wants a format with no allocation-heavy DOM, and being greppable
// makes converter bugs obvious by eye. Text fields have newlines escaped, so
// one node is always exactly one line.
//
//   !title <text>        document title
//   !url <text>          source url
//   #<id>|<title>        section start
//   H<level>|<text>      heading
//   P|<text>             paragraph
//   L<depth>|<text>      list item
//   I|<ref>|<w>|<h>|<alt>  image
//   -                    rule
std::string serialize(const Document& doc);
Document deserialize(const std::string& text);

// Resolves an <img src> against the page it came from. Handles the four forms
// that actually occur: absolute ("https://.."), protocol-relative ("//host/.."),
// root-relative ("/path/..") and document-relative ("img/foo.png"). Returns
// empty for anything unusable (data: URIs, empty refs).
std::string resolve_url(const std::string& base, const std::string& ref);

// Filename an image ref is stored under: the last path component, stripped of
// any query string and sanitised to the safe set. Both the acquire step and
// the reader derive it the same way, so they always agree.
std::string image_filename(const std::string& ref);

// Lowercase, alphanumeric-and-dashes slug used as a Section id and as the
// progress-mapping key. Stable for the same title across re-imports.
std::string slugify(const std::string& title);

}  // namespace dusk::guide
