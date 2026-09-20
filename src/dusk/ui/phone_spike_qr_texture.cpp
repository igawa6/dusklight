#include "phone_spike_qr_texture.hpp"

#include "dusk/dualscreen.h"

#if DUSK_PHONE_SPIKE && defined(AURORA_ENABLE_RMLUI)

#include "dusk/phone_spike_pairing.h"

#include <aurora/rmlui.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dusk::ui {
namespace {

constexpr std::string_view kScheme = "duskqr";

// Namespace-scope rather than a callback-local: RuntimeTexture::rgba8 is a
// non-owning span into this, and keeping the backing storage alive past
// the callback's return avoids any doubt about upload timing.
std::vector<std::uint8_t> s_scratch;

std::optional<aurora::rmlui::RuntimeTexture> qr_texture_provider(std::string_view) {
    uint32_t width = 0;
    uint32_t height = 0;
    if (!dusk::phone_spike::current_qr_bitmap(s_scratch, width, height)) {
        return std::nullopt;
    }
    return aurora::rmlui::RuntimeTexture{
        .width = width,
        .height = height,
        .rgba8 = std::as_bytes(std::span{s_scratch}),
        .premultipliedAlpha = false,
        .generateMipmaps = false,
    };
}

}  // namespace

void register_qr_texture_provider() noexcept {
    aurora::rmlui::register_texture_provider(std::string{kScheme}, qr_texture_provider);
}

void unregister_qr_texture_provider() noexcept {
    aurora::rmlui::unregister_texture_provider(kScheme);
}

}  // namespace dusk::ui

#else

namespace dusk::ui {

void register_qr_texture_provider() noexcept {}
void unregister_qr_texture_provider() noexcept {}

}  // namespace dusk::ui

#endif
