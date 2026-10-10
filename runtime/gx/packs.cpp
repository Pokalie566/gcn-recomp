// Texture packs. See packs.h.
#include "packs.h"
#include "../runtime.h"
#include <dolphin/gx.h>   // the GX types aurora/texture.hpp names
#include <aurora/texture.hpp>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>

namespace fs = std::filesystem;

namespace gx {

namespace {

std::vector<aurora::texture::ReplacementGroup> g_groups;   // what Aurora was given
std::vector<std::string> g_off;
bool g_enabled = true;

bool is_texture(const fs::path& p) {
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)tolower(c); });
    return p.filename().string().compare(0, 5, "tex1_") == 0 && (ext == ".png" || ext == ".dds");
}

// The pack a file belongs to: the first folder under the game's, or "" for a file right in it.
std::string pack_of(const fs::path& dir, const fs::path& file) {
    const fs::path rel = file.lexically_relative(dir);
    return std::distance(rel.begin(), rel.end()) > 1 ? rel.begin()->string() : std::string();
}

// Aurora asks for a loose file by its name; userData is the folder it is in.
std::string g_loose_dir;
bool read_file(void* dir, const char* name, std::vector<uint8_t>& out) {
    std::ifstream in(fs::path(*(const std::string*)dir) / name, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

}  // namespace

std::string texture_pack_dir() {
    const char* env = getenv("GCN_TEXTURES");
    const fs::path root = env ? fs::path(env) : fs::path(save_path("textures"));
    std::error_code ec;
    if (!fs::is_directory(root / GCN_DISC_ID, ec) && fs::is_directory(root / std::string(GCN_DISC_ID).substr(0, 3), ec))
        return (root / std::string(GCN_DISC_ID).substr(0, 3)).string();
    return (root / GCN_DISC_ID).string();
}

std::vector<TexturePack> texture_pack_list() {
    const fs::path dir = texture_pack_dir();
    std::map<std::string, size_t> count;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec) && is_texture(it->path())) count[pack_of(dir, it->path())]++;
    std::vector<TexturePack> out;
    for (const auto& [pack, n] : count) out.push_back({pack, n, std::find(g_off.begin(), g_off.end(), pack) == g_off.end()});
    return out;
}

size_t texture_pack_count() {
    if (!g_enabled) return 0;
    size_t n = 0;
    for (const TexturePack& p : texture_pack_list()) n += p.on ? p.textures : 0;
    return n;
}

void texture_packs_apply(bool enabled, const std::vector<std::string>& off) {
    g_enabled = enabled;
    g_off = off;
    for (const auto& g : g_groups) aurora::texture::unregister_replacements(g);
    g_groups.clear();
    if (!enabled) return;
    const fs::path dir = texture_pack_dir();
    std::error_code ec;
    aurora::texture::ReplacementGroup loose;
    g_loose_dir = dir.string();
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        const fs::path& p = it->path();
        if (it->is_directory(ec)) {
            if (std::find(off.begin(), off.end(), p.filename().string()) == off.end())
                g_groups.push_back(aurora::texture::load_replacement_directory(p));
        } else if (is_texture(p) && std::find(off.begin(), off.end(), std::string()) == off.end()) {
            const auto r = aurora::texture::register_virtual_replacement(p.filename().string(), {read_file, &g_loose_dir});
            if (r.id) loose.registrations.push_back(r);
        }
    }
    if (!loose.registrations.empty()) g_groups.push_back(std::move(loose));
}

}  // namespace gx
