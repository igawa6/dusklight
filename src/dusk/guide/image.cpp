#include "dusk/guide/image.hpp"

// Vendored stb_image rather than SDL_LoadPNG_IO. SDL core decodes PNG and BMP
// only, so every JPEG on a real walkthrough page fell through to alt text —
// which is what "[image]" everywhere was. stb handles both formats in one
// call, and vendoring the header beats reaching into SDL's private src/video
// copy, which would break the moment SDL moves it.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
// Fed fully untrusted bytes. stb defaults to 1<<24 per axis, so a hostile
// 16384x16384 PNG allocates ~1 GiB before our downscale ever runs.
#define STBI_MAX_DIMENSIONS 8192
#define STBI_NO_FAILURE_STRINGS
#include "stb/stb_image.h"

#include <cstring>
#include <fstream>
#include <sstream>

#include "JSystem/JUtility/JUTTexture.h"
#include "dolphin/gx/GXEnum.h"

// Runtime PNG -> drawable bridge.
//
// Nothing else in the repo does this. The companion's own images are either
// baked into BTI at build time (companion_logo.inc) or fabricated over
// renderer-owned pixels (dRenderingMap_c::makeResTIMG). Guide images are
// neither: they arrive as PNG at import time and have to become something
// J2DPicture can draw.
//
// Two decisions make it cheap:
//
//  * GX_TF_RGBA8_PC takes LINEAR RGBA8 and uploads directly. The GameCube
//    RGBA8 format is 4x4-tiled and there is no linear->GX tiler anywhere in
//    this tree, so writing one would have been the bulk of the work. The PC
//    format sidesteps it entirely.
//  * The header and the pixels live in ONE allocation, because ResTIMG's
//    imageOffset is a byte delta from the header itself (see makeResTIMG in
//    d_map_path.cpp). Two allocations would make that offset meaningless.

namespace dusk::guide {
namespace {

constexpr std::size_t kHeaderSize = sizeof(ResTIMG);  // 0x20

// Guide images are decorative and the second screen is small; anything larger
// is downscaled at import so the on-disk blob and the texture upload stay
// bounded. 512x512 RGBA8 is 1 MiB, which is the ceiling per image.
constexpr int kMaxDim = 512;

// The single owner of the downscale rule. decode_png_to_rgba and the header
// probe must agree exactly or the reserved height and the drawn height differ.
void scaled_extent(int w, int h, int* o_w, int* o_h) {
    int step = 1;
    while (w / step > kMaxDim || h / step > kMaxDim) {
        step++;
    }
    *o_w = w / step;
    *o_h = h / step;
}

}  // namespace

bool probe_image_size(const std::filesystem::path& file, int* o_width, int* o_height) {
    std::ifstream f(file, std::ios::binary);
    if (!f) {
        return false;
    }
    // stbi_info parses only the header, so this stays cheap even though the
    // import worker calls it once per image on every re-convert.
    std::ostringstream ss;
    ss << f.rdbuf();
    const std::string bytes = ss.str();
    int w = 0;
    int h = 0;
    int comp = 0;
    if (!stbi_info_from_memory((const stbi_uc*)bytes.data(), (int)bytes.size(), &w, &h, &comp) ||
        w <= 0 || h <= 0) {
        return false;
    }
    int sw = 0;
    int sh = 0;
    scaled_extent(w, h, &sw, &sh);
    if (o_width != nullptr) {
        *o_width = sw;
    }
    if (o_height != nullptr) {
        *o_height = sh;
    }
    return true;
}

bool decode_png_to_rgba(const std::string& bytes, RgbaImage& out) {
    if (bytes.empty()) {
        return false;
    }
    int w = 0;
    int h = 0;
    int channels = 0;
    // Forced to 4 channels so the caller always gets tightly-packed RGBA8,
    // which is exactly what GX_TF_RGBA8_PC uploads.
    stbi_uc* pixels = stbi_load_from_memory((const stbi_uc*)bytes.data(),
        (int)bytes.size(), &w, &h, &channels, 4);
    if (pixels == nullptr || w <= 0 || h <= 0) {
        stbi_image_free(pixels);
        return false;
    }
    // Integer-factor downscale: cheap, and these are screenshots where a clean
    // 1/2 or 1/4 reads fine. Bounds the on-disk blob and the texture upload.
    scaled_extent(w, h, &out.width, &out.height);
    const int step = out.width > 0 ? w / out.width : 1;
    if (out.width <= 0 || out.height <= 0) {
        stbi_image_free(pixels);
        return false;
    }
    out.pixels.assign((std::size_t)out.width * (std::size_t)out.height * 4u, 0);
    for (int y = 0; y < out.height; y++) {
        const stbi_uc* row = pixels + (std::size_t)(y * step) * (std::size_t)w * 4u;
        auto* dst = out.pixels.data() + (std::size_t)y * (std::size_t)out.width * 4u;
        for (int x = 0; x < out.width; x++) {
            std::memcpy(dst + (std::size_t)x * 4u, row + (std::size_t)(x * step) * 4u, 4);
        }
    }
    stbi_image_free(pixels);
    return true;
}

std::vector<std::uint8_t> build_timg(const RgbaImage& img) {
    std::vector<std::uint8_t> blob;
    if (img.width <= 0 || img.height <= 0 || img.pixels.empty()) {
        return blob;
    }
    blob.resize(kHeaderSize + img.pixels.size(), 0);
    auto* h = (ResTIMG*)blob.data();
    h->format = GX_TF_RGBA8_PC;
    h->alphaEnabled = 1;
    h->width = (u16)img.width;
    h->height = (u16)img.height;
    h->wrapS = GX_CLAMP;
    h->wrapT = GX_CLAMP;
    h->indexTexture = false;
    h->colorFormat = 0;
    h->numColors = 0;
    h->paletteOffset = 0;
    h->mipmapEnabled = false;
    h->doEdgeLOD = false;
    h->biasClamp = false;
    h->maxAnisotropy = 0;
    h->minFilter = GX_LINEAR;
    h->magFilter = GX_LINEAR;
    h->minLOD = 0;
    h->maxLOD = 0;
    h->mipmapCount = 1;
    h->LODBias = 0;
    // Offset from the HEADER, which is why this is one allocation.
    h->imageOffset = (s32)kHeaderSize;
    std::memcpy(blob.data() + kHeaderSize, img.pixels.data(), img.pixels.size());
    return blob;
}

bool store_image_file(const std::string& bytes, const std::filesystem::path& outFile,
    int* o_width, int* o_height) {
    // Validated by decoding, then thrown away: storing something undecodable
    // would just fail later, in the reader, where there is nowhere to report
    // it. Only the compressed bytes are kept.
    RgbaImage probe;
    if (!decode_png_to_rgba(bytes, probe)) {
        return false;
    }
    if (o_width != nullptr) {
        *o_width = probe.width;
    }
    if (o_height != nullptr) {
        *o_height = probe.height;
    }
    // Temp + rename, like every other write in the store. The import worker
    // writes these while the game thread reads them: a read landing mid-write
    // got a truncated JPEG, stb failed, and the reader cached an EMPTY blob
    // against that ref — so a transient race became a permanent "[image]".
    std::error_code ec;
    std::filesystem::create_directories(outFile.parent_path(), ec);
    std::filesystem::path tmp = outFile;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            return false;
        }
        f.write(bytes.data(), (std::streamsize)bytes.size());
        if (!f.good()) {
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
    std::filesystem::rename(tmp, outFile, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

std::vector<std::uint8_t> load_timg_blob(const std::filesystem::path& file) {
    std::ifstream f(file, std::ios::binary);
    if (!f) {
        return {};
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    RgbaImage img;
    if (!decode_png_to_rgba(ss.str(), img)) {
        return {};
    }
    return build_timg(img);
}

}  // namespace dusk::guide
