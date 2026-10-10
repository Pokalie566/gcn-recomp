// Custom textures, in Dolphin's format, so that a pack made for Dolphin works as it is.
//
// A pack is a directory of PNGs named after the texture they replace,
//   tex1_<w>x<h>[_m]_<texture hash>[_<palette hash>]_<format>.png
// in any subdirectory of textures/<game id> (or textures/<first three letters of it>) in the
// working directory, or in g_save_dir where a frontend sets one -- Dolphin's
// Load/Textures/<game id>, copied over. GCN_TEXTURES=<dir> points elsewhere.
// --dump-textures (GCN_TEXTURE_DUMP=1) writes every texture the game uses, once, to
// textures_dump/<game id>/ under those names: what a pack is made from.
//
// The name is Dolphin's (VideoCommon/TextureInfo.cpp): XXH64 of the top level's bytes as the
// game stored them, and for an indexed texture XXH64 of the palette entries between the
// lowest and the highest the texels use. A pack may replace "$" for either hash, to match
// any. A replacement may be any size: the texture keeps its size for the game's purposes
// (texel-based offsets), only its storage grows. Where the game uses mipmaps, the
// replacement's are generated.
//
// PNG or DDS: uncompressed 32-bit, DXT1, DXT3 and DXT5 (BC1 to BC3, in a DX10 header too);
// a PNG wins over a DDS of the same name. Not supported: BC7 DDS, which is reported and left
// out, and a pack's own mipmap files (_mip1..., and the levels inside a DDS), which are
// ignored in favour of generated ones; "_arb" textures lose their arbitrary mipmaps the
// same way.
#pragma once
#include "render.h"
#include <cstdint>
#include <string>
#include <vector>

namespace gx {

uint64_t xxh64(const void* data, size_t len, uint64_t seed);

struct TexName {
    std::string base, tex, tlut, fmt;   // tex1_64x64_m, 16 hex digits, "" or _ + 16, "14"
    std::string full() const { return base + "_" + tex + tlut + "_" + fmt; }
};

// Dolphin's name for a texture: `data` its top level as stored, `tlut` the palette memory it
// is drawn with and `tlut_avail` how many bytes of it there are.
TexName texture_name(const uint8_t* data, uint32_t fmt, uint32_t w, uint32_t h, bool mipmapped,
                     const uint8_t* tlut, uint32_t tlut_avail);

// Whether a pack was found or dumping is on; nothing below needs calling otherwise.
bool texture_pack_active();

// Replaces `td`'s levels with the pack's texture of that name, if there is one, mipmapped to
// `game_levels` > 1 like the original. Otherwise, when dumping, marks it to be dumped.
void texture_pack_apply(const TexName& name, TexData& td, uint32_t game_levels);

// --dump-textures: called by the frontend before the game starts.
void texture_pack_enable_dump();

// Whether the pack replaces textures (it does by default). Textures already decoded keep
// what they have until texture_cache_clear.
void texture_pack_set_enabled(bool on);

// How many textures the pack has.
size_t texture_pack_count();

// Forgets the pack, to be looked for again at its next use (for a test that changes it).
void texture_pack_reset();

// A DDS file's top level as RGBA8. False, with the reason, for a format it does not read.
bool dds_decode(const uint8_t* data, size_t size, int* w, int* h, std::vector<uint32_t>* rgba, std::string* err);

// For a renderer that writes the dumps: the directory, or null when not dumping.
const char* texture_dump_dir();

}  // namespace gx
