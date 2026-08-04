// Guide HTML conversion.
//
// The converter is the only part of the guide pipeline that has to cope with
// arbitrary third-party markup, and its failures are quiet: adverts and comment
// threads become guide text, or a filter that is one token too greedy eats a
// paragraph of the walkthrough. Neither shows up as a crash, and a user running
// an ad blocker never sees the first kind at all — which is exactly how the
// original bug survived for so long.
//
// The committed fixtures are synthetic on purpose. They reproduce the shapes
// seen on real saved pages (share widgets, comment sections, nested ad
// wrappers) without vendoring a copy of someone else's walkthrough into the
// repository. Point GUIDE_TEST_PAGE at a real saved page to check against one;
// see tests/CMakeLists.txt.

#include "dusk/guide/guide_doc.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

namespace {

using dusk::guide::Document;
using dusk::guide::NodeKind;

Document Convert(const std::string& html) {
  return dusk::guide::convert_html(html, "https://example.invalid/tp/forest-temple/");
}

/// Every paragraph and heading in the document, flattened, for whole-document
/// assertions about what survived.
std::string AllText(const Document& doc) {
  std::string out;
  for (const auto& section : doc.sections) {
    out += section.title;
    out += '\n';
    for (const auto& node : section.nodes) {
      out += node.text;
      out += '\n';
    }
  }
  return out;
}

bool Contains(const Document& doc, const std::string& needle) {
  return AllText(doc).find(needle) != std::string::npos;
}

constexpr const char* kPage = R"HTML(<html><head><title>x</title>
<script>var a = "<div>not real markup</div>";</script></head>
<body>
<nav class="site-nav"><ul><li>Home</li><li>Walkthroughs</li></ul></nav>
<div id="header-ad" class="ad-container"><div class="inner">
  <div>SPONSORED CONTENT</div><p>Buy this now</p></div></div>
<ins class="adsbygoogle"><p>Advertisement text</p></ins>
<article>
<h1>Twilight Princess Walkthrough - Forest Temple</h1>
<p>Head north from the entrance and light the two torches.</p>
<div class="advert-inline"><div><div>Deep nested ad</div></div></div>
<h2>4.1 Dungeon Map</h2>
<p>The map is in the chest to the east.</p>
<div class="sharedaddy"><h3>Share this:</h3><a>Tweet</a><a>Facebook</a></div>
<p>Defeat the Deku Baba to continue.</p>
</article>
<div id="comments" class="comments-area"><h2>Comments</h2><p>First! great guide</p></div>
<aside class="widget-area"><h3>Related Posts</h3><p>Goron Mines</p></aside>
<footer class="site-footer"><p>About Us - Contact Us - Privacy Policy</p></footer>
</body></html>)HTML";

TEST(GuideConvert, KeepsTheWalkthroughProse) {
  const auto doc = Convert(kPage);
  EXPECT_EQ(doc.title, "Twilight Princess Walkthrough - Forest Temple");
  EXPECT_TRUE(Contains(doc, "Head north from the entrance"));
  EXPECT_TRUE(Contains(doc, "The map is in the chest to the east."));
  EXPECT_TRUE(Contains(doc, "Defeat the Deku Baba to continue."));
}

TEST(GuideConvert, KeepsTheChapterHeadings) {
  const auto doc = Convert(kPage);
  ASSERT_EQ(doc.sections.size(), 2u);
  EXPECT_EQ(doc.sections[1].id, "4-1-dungeon-map");
  EXPECT_EQ(doc.sections[1].title, "4.1 Dungeon Map");
}

TEST(GuideConvert, DropsAdverts) {
  const auto doc = Convert(kPage);
  EXPECT_FALSE(Contains(doc, "SPONSORED CONTENT"));
  EXPECT_FALSE(Contains(doc, "Buy this now"));
  EXPECT_FALSE(Contains(doc, "Advertisement text"));
}

// The skip used to stop at the FIRST closing tag of the matching name, so an ad
// wrapper containing any nested div ended early and leaked the rest as prose.
TEST(GuideConvert, SkipsAnAdvertContainingNestedElements) {
  EXPECT_FALSE(Contains(Convert(kPage), "Deep nested ad"));
}

TEST(GuideConvert, DropsShareWidgetsCommentsAndFurniture) {
  const auto doc = Convert(kPage);
  EXPECT_FALSE(Contains(doc, "Tweet"));
  EXPECT_FALSE(Contains(doc, "Share this"));
  EXPECT_FALSE(Contains(doc, "First! great guide"));
  EXPECT_FALSE(Contains(doc, "Related Posts"));
  EXPECT_FALSE(Contains(doc, "Privacy Policy"));
  EXPECT_FALSE(Contains(doc, "Walkthroughs"));  // the nav list
}

// A comments block becoming its own section is the visible symptom users
// reported: a chapter list with "Comments" sitting in it.
TEST(GuideConvert, DoesNotCreateSectionsFromFurnitureHeadings) {
  for (const auto& section : Convert(kPage).sections) {
    EXPECT_NE(section.title, "Comments");
    EXPECT_NE(section.title, "Related Posts");
  }
}

TEST(GuideConvert, NeverTreatsScriptContentAsProse) {
  EXPECT_FALSE(Contains(Convert(kPage), "not real markup"));
}

// The filter matches short ambiguous words only as whole class tokens, because
// "ad" is a substring of plenty of ordinary class names.
TEST(GuideConvert, DoesNotMistakeOrdinaryClassNamesForAdverts) {
  const auto doc = Convert(
      "<article><h1>T</h1>"
      "<div class=\"shadow\"><p>Shadow Crystal prose</p></div>"
      "<div class=\"loaded content-header\"><p>Loaded prose</p></div>"
      "<div class=\"read-more-body\"><p>Ready prose</p></div>"
      "</article>");
  EXPECT_TRUE(Contains(doc, "Shadow Crystal prose"));
  EXPECT_TRUE(Contains(doc, "Loaded prose"));
  EXPECT_TRUE(Contains(doc, "Ready prose"));
}

TEST(GuideConvert, SurvivesUnbalancedMarkupWithoutThrowing) {
  EXPECT_NO_THROW({
    (void)Convert("<article><h1>T</h1><p>unclosed <div><span>text");
    (void)Convert("<<<>>><p");
    (void)Convert("");
  });
}

// Optional: run against a page actually saved from the browser. Not committed —
// it is someone else's content — so this is skipped unless a path is supplied.
TEST(GuideConvert, RealSavedPage) {
#ifdef GUIDE_TEST_PAGE
  std::ifstream file(GUIDE_TEST_PAGE);
  ASSERT_TRUE(file) << "cannot open " << GUIDE_TEST_PAGE;
  std::stringstream buffer;
  buffer << file.rdbuf();
  const auto doc = Convert(buffer.str());
  EXPECT_FALSE(doc.title.empty());
  EXPECT_FALSE(doc.sections.empty());
  // Whatever the page is, none of this belongs in a walkthrough.
  EXPECT_FALSE(Contains(doc, "Tweet"));
  EXPECT_FALSE(Contains(doc, "submit to reddit"));
  for (const auto& section : doc.sections) {
    EXPECT_NE(section.title, "Comments");
  }
#else
  GTEST_SKIP() << "configure with -DGUIDE_TEST_PAGE=<saved .html> to enable";
#endif
}

}  // namespace
