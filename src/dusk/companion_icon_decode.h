#pragma once

// CPU-side item icon decoding for the phone companion.
//
// The phone fetches icon art on demand, and until now the only way to
// produce those pixels was to DRAW the icon into the aux render surface and
// read it back off the GPU — the same single capture slot the video frame
// stream uses. Every art fetch therefore stole a frame from the stream,
// which is why the frame rate visibly dipped whenever icons loaded.
//
// That was justified by a comment claiming no CPU-side texture decoder
// existed anywhere in the codebase. That claim was false, and it had already
// been false for some time: src/dusk/ui/icon_provider.cpp decodes GX
// textures on the CPU via aurora::gfx::convert_texture(), and the item
// icons' source bytes are copied into caller-owned buffers by
// JKRReadIdxResource, so they are plainly readable without the GPU at all.
//
// This path touches no GPU state and no capture slot.
//
// NOT everything can move here. The dungeon map's base image genuinely
// must keep the capture path: on PC its ResTIMG's image allocation is a
// 32-byte stub (d_menu_dmap_map.cpp's setTexture) because the real pixels
// live only in a GPU copy-texture, so following imageOffset there would read
// garbage and then run off the heap. Heart containers could move here in
// principle — their panes' textures are ordinary archive bytes in CPU RAM —
// but they need each pane's transform and alpha applied rather than two
// same-size layers stacked, so they are left on the existing path for now.
//
// Gated by DUSK_PHONE_SPIKE_STATE (dualscreen.h) and off by default.

#include <cstdint>
#include <vector>

namespace dusk::companion {

// Decodes one item's icon to RGBA8 (tightly packed, width*4 bytes per row,
// top-to-bottom) ready to hand straight to miniz's PNG encoder.
//
// The result is fully OPAQUE: layers are composited with straight alpha and
// the result is then flattened over the dashboard background colour with
// alpha forced to 255. That is deliberate — the icons the phone receives
// today are opaque for the same reason (the draw path starts with a
// full-canvas background fill and the capture stamps alpha), so keeping it
// that way makes this a drop-in replacement rather than a wire-format
// change the client would have to be updated for. Emitting real
// transparency is a reasonable future change, but it is a change.
//
// Returns false (leaving `out` untouched) if the item has no icon, the icon
// archive isn't loaded yet, or the texture is in a format this refuses to
// decode. Must be called on the GAME THREAD: obtaining the source bytes runs
// game-side logic. The decoded bytes it returns are owned, so they can then
// be encoded and sent from any thread.
bool decodeItemIconRgba(uint8_t itemNo, std::vector<uint8_t>& out, uint32_t& outWidth,
    uint32_t& outHeight);

// Same, for one dungeon-map overlay icon (ICON_*_e). These are single-layer
// CI8 textures already copied into owned static buffers, so unlike item
// icons there are no archive layers to composite and no per-layer tint to
// apply — just a decode. Same opaque-over-background treatment, for the same
// wire-compatibility reason.
bool decodeMapIconRgba(uint8_t iconKind, std::vector<uint8_t>& out, uint32_t& outWidth,
    uint32_t& outHeight);

}  // namespace dusk::companion
