// The player's saves. See saves.h.
#include "saves.h"
#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace fs = std::filesystem;

static uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// A fixed-size field of the card: up to its first NUL, Latin-1 as the card's ANSI encoding
// is, as UTF-8.
static std::string text(const uint8_t* p, size_t n) {
    std::string s;
    for (size_t i = 0; i < n && p[i]; i++) {
        if (p[i] < 0x80) s += (char)p[i];
        else { s += (char)(0xC0 | p[i] >> 6); s += (char)(0x80 | (p[i] & 0x3F)); }
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

// The .gci files of a folder, by name, with their bytes.
static std::vector<std::pair<std::string, std::vector<uint8_t>>> gci_files(const std::string& folder) {
    std::vector<std::pair<std::string, std::vector<uint8_t>>> out;
    std::error_code ec;
    for (auto it = fs::directory_iterator(folder, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec) && it->path().extension() == ".gci")
            out.push_back({it->path().filename().string(), read_file(it->path().string())});
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<CardFile> card_files(const std::string& card) {
    std::vector<CardFile> out;
    for (const auto& [name, d] : gci_files(card)) {
        if (d.size() < 0x40) continue;
        const uint8_t* e = d.data();   // the directory entry
        CardFile f;
        f.game = text(e, 6);
        f.name = text(e + 0x08, 32);
        f.modified = be32(e + 0x28);
        f.blocks = be16(e + 0x38);
        f.file = name;
        const size_t comment = 0x40 + (size_t)be32(e + 0x3C);
        if (comment + 64 <= d.size()) {
            const std::string a = text(&d[comment], 32), b = text(&d[comment + 32], 32);
            f.comment = b.empty() ? a : a + " - " + b;
        }
        out.push_back(f);
    }
    return out;
}

std::vector<CardBackup> card_backups(const std::string& dir) {
    std::vector<CardBackup> out;
    std::error_code ec;
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        const std::string n = it->path().filename().string();   // card-20261010-031200
        if (n.size() != 20 || n.compare(0, 5, "card-") || !it->is_directory(ec)) continue;
        const std::string when = n.substr(5, 4) + "-" + n.substr(9, 2) + "-" + n.substr(11, 2) + " " +
                                 n.substr(14, 2) + ":" + n.substr(16, 2);
        out.push_back({it->path().string(), when});
    }
    std::sort(out.begin(), out.end(), [](const CardBackup& a, const CardBackup& b) { return a.path > b.path; });
    return out;
}

// `saves` as the .gci files of `to`, made or emptied of its own first.
static bool write_saves(const std::vector<std::pair<std::string, std::vector<uint8_t>>>& saves, const std::string& to) {
    std::error_code ec;
    fs::create_directories(to, ec);
    for (const auto& [name, d] : gci_files(to)) fs::remove(fs::path(to) / name, ec);
    for (const auto& [name, d] : saves) {
        std::ofstream out(fs::path(to) / name, std::ios::binary);
        out.write((const char*)d.data(), (std::streamsize)d.size());
        if (!out) return false;
    }
    return true;
}

bool card_backup(const std::string& card, const std::string& dir, int keep) {
    const auto saves = gci_files(card);
    if (saves.empty()) return false;
    std::vector<CardBackup> have = card_backups(dir);
    if (!have.empty() && gci_files(have[0].path) == saves) return false;
    char name[32];
    const time_t now = time(nullptr);
    strftime(name, sizeof(name), "card-%Y%m%d-%H%M%S", localtime(&now));
    if (!write_saves(saves, (fs::path(dir) / name).string())) return false;
    have = card_backups(dir);
    std::error_code ec;
    for (size_t i = (size_t)std::max(keep, 1); i < have.size(); i++) fs::remove_all(have[i].path, ec);
    return true;
}

bool card_restore(const std::string& backup, const std::string& card, const std::string& dir, int keep) {
    // Read first: the copy of the card made next may push this one, the oldest, out.
    const auto saves = gci_files(backup);
    if (saves.empty()) return false;
    card_backup(card, dir, keep);
    return write_saves(saves, card);
}

std::string card_import(const std::string& gci, const std::string& game, const std::string& card,
                        const std::string& dir, int keep) {
    const std::vector<uint8_t> d = read_file(gci);
    if (d.size() < 0x40 || d.size() != 0x40 + (size_t)be16(&d[0x38]) * 0x2000) return "not a save";
    const std::string its_game = text(d.data(), 6), its_name = text(&d[0x08], 32);
    if (!game.empty() && its_game != game) return "another game";
    card_backup(card, dir, keep);
    std::error_code ec;
    fs::create_directories(card, ec);
    for (const CardFile& f : card_files(card))
        if (f.game == its_game && f.name == its_name) fs::remove(fs::path(card) / f.file, ec);
    std::ofstream out(fs::path(card) / fs::path(gci).filename(), std::ios::binary);
    out.write((const char*)d.data(), (std::streamsize)d.size());
    return out ? "" : "not written";
}

bool card_delete(const std::string& file, const std::string& card, const std::string& dir, int keep) {
    if (file.empty() || file.find_first_of("/\\") != std::string::npos) return false;
    card_backup(card, dir, keep);
    std::error_code ec;
    return fs::remove(fs::path(card) / file, ec);
}
