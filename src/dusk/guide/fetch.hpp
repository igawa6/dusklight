#pragma once

// Background download of a guide page into the store.
//
// dusk::http::get is SYNCHRONOUS and blocks until the response completes or
// times out, so it must never be called from the game loop. Everything here
// runs on a worker thread and is polled once per frame, mirroring
// UpdateCheckTask / DiscVerificationTask in src/dusk/ui/prelaunch.cpp.
//
// Requests carry an honest User-Agent identifying Dusklight; nothing here
// impersonates a browser. Sites that refuse non-browser clients (Cloudflare
// commonly answers 403, even for robots.txt) are therefore unreachable from
// this path, which is why the WebView in browser.hpp is the primary route and
// the only thing left here is image acquisition for pages it saved.

#include "dusk/guide/store.hpp"
#include <string>

namespace dusk::guide {


// --- Import, off the game thread ---
//
// scan_import_folder() converts pages AND pulls their images over HTTP, and
// http::get BLOCKS. Calling it from guideOpen() froze the game solid once a
// multi-page guide was saved: ~23 pages x N images x up to a 10s timeout, all
// on the thread that draws. It runs on a worker now and the reader polls.
void begin_import();
bool import_in_progress();
// A finished import is observed by BOTH the reader and the settings pane, on
// different screens and different frames. This used to be a one-shot take:
// whichever polled first consumed it and the other never learned the catalogue
// had changed, so the reader could sit on an empty list until a restart.
// It is a monotonic generation instead, and each consumer remembers the last
// value it acted on. Bumped once per completed import; reaps the worker.
unsigned import_generation();
// Pages imported by the most recently completed import.
int last_import_count();

}  // namespace dusk::guide
