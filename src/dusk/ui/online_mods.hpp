#pragma once

#include <string>
#include <unordered_set>

namespace dusk::ui {
class Document;
class Pane;

void build_online_mods(
    Pane& pane, Document& document, std::unordered_set<std::string>& expandedChangelogs);
}  // namespace dusk::ui
