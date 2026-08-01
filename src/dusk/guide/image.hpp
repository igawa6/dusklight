#pragma once

// PNG -> drawable texture, done at IMPORT time so the reader never decodes on
// the draw path. See image.cpp for why GX_TF_RGBA8_PC and a single allocation.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace dusk::guide {

struct RgbaImage {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels;  // linear RGBA8, tightly packed
};

// Decodes PNG and JPEG (stb_image). Anything else fails and the reader falls
// back to the alt text.
bool decode_png_to_rgba(const std::string& bytes, RgbaImage& out);

// Wraps linear RGBA8 in a ResTIMG header. Header and pixels share one buffer
// because ResTIMG::imageOffset is a delta from the header itself.
std::vector<std::uint8_t> build_timg(const RgbaImage& img);

// Stores an image for later display.
//
// Keeps the COMPRESSED source bytes, not the decoded texture. A 512x512 RGBA8
// blob is 1 MB, and a 23-chapter guide has ~2000 images — that was 2 GB on
// disk. The same images as JPEG are ~40 KB each, so this is ~25x smaller and
// the decode moves to load time, where a bounded RAM cache already limits it.
// Reads just the dimensions of an already-stored image, in the SAME units
// store_image_file reports (i.e. after the import downscale), without decoding
// the pixels. Needed because a re-import of a page whose images are already on
// disk skips the store step entirely and would otherwise leave the recorded
// size at 0, which makes the reader reserve alt-text height for a real image.
bool probe_image_size(const std::filesystem::path& file, int* o_width, int* o_height);

bool store_image_file(const std::string& bytes, const std::filesystem::path& outFile,
    int* o_width = nullptr, int* o_height = nullptr);

// Reads a stored image and decodes it into a drawable ResTIMG blob. The
// returned bytes must outlive any J2DPicture built over them — imageOffset
// points inside this buffer. Empty on failure.
std::vector<std::uint8_t> load_timg_blob(const std::filesystem::path& file);

}  // namespace dusk::guide
