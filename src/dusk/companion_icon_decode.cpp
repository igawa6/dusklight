#include "dusk/companion_icon_decode.h"

#include "dusk/dualscreen.h"

#if DUSK_PHONE_SPIKE_STATE

#include "dusk/companion_internal.h"

#include "JSystem/J2DGraph/J2DPicture.h"
#include "JSystem/JUtility/JUTTexture.h"
#include "d/d_com_inf_game.h"
#include "d/d_meter2_info.h"

#include <aurora/lib/gfx/texture_convert.hpp>

#include <array>
#include <cstring>

namespace dusk::companion {

namespace {

// Item icons are composited from up to four archive layers.
constexpr size_t kItemTextureBufferSize = 0xC00;
using ItemTextureBuffer = std::array<uint8_t, kItemTextureBufferSize>;

struct LayerColors {
    JUtility::TColor black;
    JUtility::TColor white;
    std::array<JUtility::TColor, 4> corner;
};

// aurora's convert_texture()/convert_texture_palette() end their format
// switch in DEFAULT_FATAL, which is [[noreturn]] — an unexpected format
// would take the whole process down rather than fail one icon. Everything
// reaching here comes off a disc archive and should be one of these, but
// "should be" is not a reason to let a malformed resource be fatal.
bool formatSupported(uint8_t format, uint8_t indexed, uint8_t tlutFormat) {
    if (indexed != 0) {
        return (format == GX_TF_C4 || format == GX_TF_C8 || format == GX_TF_C14X2) &&
            tlutFormat <= GX_TL_RGB5A3;
    }
    switch (format) {
    case GX_TF_I4:
    case GX_TF_I8:
    case GX_TF_IA4:
    case GX_TF_IA8:
    case GX_TF_RGB565:
    case GX_TF_RGB5A3:
    case GX_TF_RGBA8:
    case GX_TF_CMPR:
        return true;
    default:
        return false;
    }
}

// Mirrors icon_provider.cpp's decode_timg(). Deliberately duplicated rather
// than shared: that one lives in an anonymous namespace inside a file gated
// on the RmlUI build flag, and hoisting it would mean touching a working UI
// path to serve this one. Worth unifying once something needs it a third
// time.
//
// The payload is NOT byte-swapped here. The header fields are big-endian and
// swap on read via BE<T>, but aurora's decoders handle the pixel data's
// endianness internally — pre-swapping it produces plausible-looking but
// wrong colours, which is a far worse failure than not decoding at all.
aurora::gfx::ConvertedTexture decodeTimg(const ResTIMG* image) {
    if (image == nullptr || image->width.host() == 0 || image->height.host() == 0) {
        return {};
    }
    if (!formatSupported(image->format, image->indexTexture, image->colorFormat)) {
        return {};
    }
    const auto* base = reinterpret_cast<const uint8_t*>(image);
    const auto width = image->width.host();
    const auto height = image->height.host();
    // mips=1 / GX_FALSE: level 0 is first in the blob and is all we want, so
    // this both sizes the read correctly and avoids decoding levels nothing
    // will ever sample.
    const uint32_t textureSize = GXGetTexBufferSize(width, height, image->format, GX_FALSE, 0);
    const auto* textureData = base + static_cast<int32_t>(image->imageOffset);

    if (image->indexTexture != 0) {
        const auto* paletteData = base + static_cast<int32_t>(image->paletteOffset);
        // TLUT entries are 16-bit, so the palette size is NOT a
        // GXGetTexBufferSize question.
        return aurora::gfx::convert_texture_palette(image->format, width, height, 1,
            aurora::ArrayRef{textureData, textureSize},
            static_cast<GXTlutFmt>(image->colorFormat), image->numColors,
            aurora::ArrayRef{paletteData, static_cast<size_t>(image->numColors) * 2});
    }
    return aurora::gfx::convert_texture(
        image->format, width, height, 1, aurora::ArrayRef{textureData, textureSize});
}

uint8_t lerpU8(uint8_t a, uint8_t b, uint32_t t) {
    return static_cast<uint8_t>((static_cast<uint32_t>(a) * (255u - t) + static_cast<uint32_t>(b) * t) / 255u);
}

JUtility::TColor lerpColor(const JUtility::TColor& a, const JUtility::TColor& b, uint32_t t) {
    return {lerpU8(a.r, b.r, t), lerpU8(a.g, b.g, t), lerpU8(a.b, b.b, t), lerpU8(a.a, b.a, t)};
}

JUtility::TColor bilerpCorner(
    const LayerColors& colors, uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    const uint32_t u = width > 1 ? (x * 255u) / (width - 1u) : 0u;
    const uint32_t v = height > 1 ? (y * 255u) / (height - 1u) : 0u;
    const JUtility::TColor top = lerpColor(colors.corner[0], colors.corner[1], u);
    const JUtility::TColor bottom = lerpColor(colors.corner[2], colors.corner[3], u);
    return lerpColor(top, bottom, v);
}

LayerColors layerColors(const J2DPicture& picture) {
    return {
        .black = picture.getBlack(),
        .white = picture.getWhite(),
        .corner = {picture.corner(0), picture.corner(1), picture.corner(2), picture.corner(3)},
    };
}

// The per-layer tint the game applies through TEV. Skipping it is not
// cosmetic: every rupee would come out the same grey and potions would lose
// their colour entirely, because those are one shared base texture tinted
// per variant.
std::array<uint8_t, 4> applyLayerColors(const uint8_t* src, const LayerColors& colors, uint32_t x,
    uint32_t y, uint32_t width, uint32_t height) {
    std::array<uint8_t, 4> out{
        lerpU8(colors.black.r, colors.white.r, src[0]),
        lerpU8(colors.black.g, colors.white.g, src[1]),
        lerpU8(colors.black.b, colors.white.b, src[2]),
        src[3],
    };
    const JUtility::TColor corner = bilerpCorner(colors, x, y, width, height);
    out[0] = static_cast<uint8_t>((static_cast<uint32_t>(out[0]) * corner.r) / 255u);
    out[1] = static_cast<uint8_t>((static_cast<uint32_t>(out[1]) * corner.g) / 255u);
    out[2] = static_cast<uint8_t>((static_cast<uint32_t>(out[2]) * corner.b) / 255u);
    out[3] = static_cast<uint8_t>((static_cast<uint32_t>(out[3]) * corner.a) / 255u);
    return out;
}

// Straight-alpha source-over. NOT the premultiplied blend icon_provider.cpp
// uses — that one is premultiplied because SDL/RmlUI wants it that way, and
// copying it here would give every icon a dark fringe once encoded to PNG,
// which expects straight alpha.
void blendOver(uint8_t* dst, const std::array<uint8_t, 4>& src) {
    const uint32_t srcAlpha = src[3];
    if (srcAlpha == 0) {
        return;
    }
    const uint32_t invAlpha = 255u - srcAlpha;
    for (int c = 0; c < 3; c++) {
        dst[c] = static_cast<uint8_t>(
            (static_cast<uint32_t>(src[c]) * srcAlpha + static_cast<uint32_t>(dst[c]) * invAlpha) /
            255u);
    }
    dst[3] = static_cast<uint8_t>(
        srcAlpha + (static_cast<uint32_t>(dst[3]) * invAlpha) / 255u);
}

}  // namespace

bool decodeItemIconRgba(
    uint8_t itemNo, std::vector<uint8_t>& out, uint32_t& outWidth, uint32_t& outHeight) {
    if (itemNo == dItemNo_NONE_e || dComIfGp_getItemIconArchive() == NULL) {
        return false;
    }

    std::array<ItemTextureBuffer, 4> buffers{};
    std::array<J2DPicture, 4> pictures{};
    // iconTextureOverride() matters: two items (Ordon Sword, Wooden Shield)
    // resolve through the item table to leftover unused art, and the
    // companion's own icon path already corrects them. Passing -1 here would
    // silently show the wrong picture for exactly those two.
    const int textureCount = dMeter2Info_readItemTexture(itemNo, buffers[0].data(), &pictures[0],
        buffers[1].data(), &pictures[1], buffers[2].data(), &pictures[2], buffers[3].data(),
        &pictures[3], iconTextureOverride(itemNo));
    if (textureCount <= 0) {
        return false;
    }

    // Layer 0 defines the output size; later layers are sampled into it with
    // nearest neighbour, which is what the existing CPU icon path does and is
    // indistinguishable at icon scale.
    aurora::gfx::ConvertedTexture base = decodeTimg(reinterpret_cast<const ResTIMG*>(buffers[0].data()));
    if (base.data.empty() || base.width == 0 || base.height == 0) {
        return false;
    }
    const uint32_t width = base.width;
    const uint32_t height = base.height;

    // Start from the dashboard background rather than transparent — see the
    // header's note on why the result is deliberately opaque.
    out.assign(static_cast<size_t>(width) * height * 4, 0);
    for (size_t i = 0; i < out.size(); i += 4) {
        out[i + 0] = COL_BG.r;
        out[i + 1] = COL_BG.g;
        out[i + 2] = COL_BG.b;
        out[i + 3] = 255;
    }

    for (int layer = 0; layer < textureCount && layer < 4; layer++) {
        aurora::gfx::ConvertedTexture decoded =
            layer == 0 ? std::move(base)
                       : decodeTimg(reinterpret_cast<const ResTIMG*>(buffers[layer].data()));
        if (decoded.data.empty() || decoded.width == 0 || decoded.height == 0) {
            continue;
        }
        const LayerColors colors = layerColors(pictures[layer]);
        const uint8_t* srcBytes = static_cast<const uint8_t*>(decoded.data.data());
        for (uint32_t y = 0; y < height; y++) {
            const uint32_t sy = decoded.height == height ? y : (y * decoded.height) / height;
            for (uint32_t x = 0; x < width; x++) {
                const uint32_t sx = decoded.width == width ? x : (x * decoded.width) / width;
                const uint8_t* src = srcBytes + (static_cast<size_t>(sy) * decoded.width + sx) * 4;
                const std::array<uint8_t, 4> tinted =
                    applyLayerColors(src, colors, x, y, width, height);
                blendOver(out.data() + (static_cast<size_t>(y) * width + x) * 4, tinted);
            }
        }
    }

    // Force opaque: the background seeded above is opaque, but a layer with
    // partial alpha leaves intermediate values, and the wire format this
    // replaces was fully opaque.
    for (size_t i = 3; i < out.size(); i += 4) {
        out[i] = 255;
    }
    outWidth = width;
    outHeight = height;
    return true;
}

bool decodeMapIconRgba(
    uint8_t iconKind, std::vector<uint8_t>& out, uint32_t& outWidth, uint32_t& outHeight) {
    const ResTIMG* timg = dmapIconTimg(iconKind);
    if (timg == NULL) {
        return false;
    }
    aurora::gfx::ConvertedTexture decoded = decodeTimg(timg);
    if (decoded.data.empty() || decoded.width == 0 || decoded.height == 0) {
        return false;
    }
    const uint32_t width = decoded.width;
    const uint32_t height = decoded.height;
    out.assign(static_cast<size_t>(width) * height * 4, 0);
    const uint8_t* src = static_cast<const uint8_t*>(decoded.data.data());
    for (size_t i = 0; i < out.size(); i += 4) {
        // Straight over the dashboard background, then opaque — same
        // reasoning as the item path.
        uint8_t* dst = out.data() + i;
        dst[0] = COL_BG.r;
        dst[1] = COL_BG.g;
        dst[2] = COL_BG.b;
        dst[3] = 255;
        const std::array<uint8_t, 4> px{src[i + 0], src[i + 1], src[i + 2], src[i + 3]};
        blendOver(dst, px);
        dst[3] = 255;
    }
    outWidth = width;
    outHeight = height;
    return true;
}

}  // namespace dusk::companion

#endif  // DUSK_PHONE_SPIKE_STATE
