#pragma once

// In-app browser for grabbing a guide page (Android only).
//
// The point is the 403: sites behind bot protection refuse plain HTTP clients
// outright, so the only honest way to read them is with an actual browser.
// The WebView saves the rendered page into guides/import, where the normal
// importer picks it up.

namespace dusk::guide {

// True when this build can show the in-app browser (Android).
bool browser_available();

// Opens it at `url`. Returns false if it could not be launched.
bool open_browser(const char* url);

// Where the "Get Guide" button starts. Chosen because it is the site this was
// built for and the one that refuses ordinary downloads.
constexpr const char* kDefaultGuideUrl =
    "https://www.zeldadungeon.net/twilight-princess-walkthrough/";

}  // namespace dusk::guide
