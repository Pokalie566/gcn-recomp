// Texture packs, in Dolphin's format, so that a pack made for Dolphin works as it is: PNG or
// DDS files named after the texture they replace (tex1_<w>x<h>[_m]_<hash>[_<tlut hash>]_<fmt>),
// which Aurora reads and puts in place of the game's own (aurora/texture.hpp).
//
// A pack is a folder in the game's pack folder -- textures/<game id> (or textures/<its first
// three letters>) under the save directory, or GCN_TEXTURES=<dir> -- as Dolphin's are copied
// there; files right in it are a pack with no name. Each can be turned off, by name.
#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace gx {

struct TexturePack {
    std::string name;
    size_t textures;
    bool on;
};

// Where packs go, whether or not there is one yet.
std::string texture_pack_dir();
// The packs found, and whether each is on.
std::vector<TexturePack> texture_pack_list();
// How many textures the packs that are on have.
size_t texture_pack_count();
// Gives Aurora the packs that are on: all of them off with `enabled` false, and those named
// in `off` left out. Looks at the folder again, for packs copied in since.
void texture_packs_apply(bool enabled, const std::vector<std::string>& off);

}  // namespace gx
