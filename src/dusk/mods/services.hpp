#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace dusk::mods {

struct ServiceImport {
    std::string id;
    uint16_t major = 0;
    uint16_t minMinor = 0;
    bool optional = false;
    bool operator==(const ServiceImport&) const = default;
};

struct ServiceExport {
    std::string id;
    uint16_t major = 0;
    uint16_t minor = 0;
    std::string providerId;
    bool operator==(const ServiceExport&) const = default;
};

inline std::string service_key(std::string_view id, uint16_t major) {
    return std::string{id} + '\x1f' + std::to_string(major);
}

}  // namespace dusk::mods
