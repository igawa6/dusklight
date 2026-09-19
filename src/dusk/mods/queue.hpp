#pragma once

#include "updates.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace dusk::mods::queue {

enum class State {
    Queued,
    Downloading,
    Paused,
    Retrying,
    Verifying,
    Handoff,
    Installed,
    InstallFailed,
    Failed,
    Canceled,
};

[[nodiscard]] constexpr bool is_failed(State state) noexcept {
    return state == State::InstallFailed || state == State::Failed;
}

[[nodiscard]] constexpr bool is_completed(State state) noexcept {
    return state == State::Installed || state == State::Canceled;
}

[[nodiscard]] constexpr bool is_terminal(State state) noexcept {
    return is_completed(state) || is_failed(state);
}

[[nodiscard]] constexpr bool is_install_result(State state) noexcept {
    return state == State::Installed || state == State::InstallFailed;
}

struct LocalFile {
    std::filesystem::path path;
};

using Source = std::variant<Download, LocalFile>;

struct Icon {
    std::string url;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct Request {
    std::string id;
    std::string name;
    std::string version;
    Source source;
    std::optional<Icon> icon;
    std::optional<UpdatePrecondition> update;
};

struct Item {
    // Queue key, independent of the mod ID.
    std::string id;
    std::string modId;
    std::string name;
    std::string version;
    std::string previousVersion;
    State state = State::Queued;
    uint64_t completed = 0;
    uint64_t total = 0;
    std::string message;
    int retrySeconds = 0;
    bool local = false;
    std::optional<Icon> icon;
};

/** Adds an install, replacing failed or canceled work for the same package ID. */
bool enqueue(Request request, std::string* key = nullptr);

void update();
void shutdown() noexcept;

[[nodiscard]] std::vector<Item> items();
[[nodiscard]] std::optional<Item> find(std::string_view key);
[[nodiscard]] std::optional<Item> find_by_mod_id(std::string_view id);
[[nodiscard]] bool has_active_items();
[[nodiscard]] size_t item_count() noexcept;
[[nodiscard]] size_t active_items_ahead(std::string_view id) noexcept;

void pause(std::string_view id);
void resume(std::string_view id);
void retry(std::string_view id);
void cancel(std::string_view id);
void clear(std::string_view id);
void remove_by_mod_id(std::string_view id);
void pause_all();

}  // namespace dusk::mods::queue
