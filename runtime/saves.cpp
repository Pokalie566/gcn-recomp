// The player's saves. See saves.h.
#include "saves.h"
#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace fs = std::filesystem;

static constexpr size_t kBlock = 0x2000;

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

std::vector<CardFile> card_files(const std::string& card) {
    const std::vector<uint8_t> d = read_file(card);
    std::vector<CardFile> out;
    if (d.size() < 5 * kBlock || d.size() % kBlock) return out;
    // The directory last written: the one whose update counter is ahead.
    const uint8_t* dir = &d[kBlock];
    if ((int16_t)(be16(&d[2 * kBlock + 0x1FFA]) - be16(&d[kBlock + 0x1FFA])) > 0) dir = &d[2 * kBlock];
    for (int i = 0; i < 127; i++) {
        const uint8_t* e = dir + 0x40 * i;
        if (be32(e) == 0xFFFFFFFF) continue;
        CardFile f;
        f.game = text(e, 6);
        f.name = text(e + 0x08, 32);
        f.modified = be32(e + 0x28);
        f.blocks = be16(e + 0x38);
        const size_t comment = be16(e + 0x36) * kBlock + be32(e + 0x3C);
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
        const std::string n = it->path().filename().string();   // memcard-20261010-031200.raw
        if (n.size() != 27 || n.compare(0, 8, "memcard-") || n.compare(23, 4, ".raw")) continue;
        const std::string when = n.substr(8, 4) + "-" + n.substr(12, 2) + "-" + n.substr(14, 2) + " " +
                                 n.substr(17, 2) + ":" + n.substr(19, 2);
        out.push_back({it->path().string(), when});
    }
    std::sort(out.begin(), out.end(), [](const CardBackup& a, const CardBackup& b) { return a.path > b.path; });
    return out;
}

bool card_backup(const std::string& card, const std::string& dir, int keep) {
    const std::vector<uint8_t> d = read_file(card);
    if (d.empty()) return false;
    std::vector<CardBackup> have = card_backups(dir);
    if (!have.empty() && read_file(have[0].path) == d) return false;
    char name[32];
    const time_t now = time(nullptr);
    strftime(name, sizeof(name), "memcard-%Y%m%d-%H%M%S.raw", localtime(&now));
    std::error_code ec;
    fs::create_directories(dir, ec);
    {
        std::ofstream out(fs::path(dir) / name, std::ios::binary);
        out.write((const char*)d.data(), (std::streamsize)d.size());
        if (!out) return false;
    }
    have = card_backups(dir);
    for (size_t i = (size_t)std::max(keep, 1); i < have.size(); i++) fs::remove(have[i].path, ec);
    return true;
}

bool card_restore(const std::string& backup, const std::string& card, const std::string& dir, int keep) {
    // Read first: the copy of the card made next may push this one, the oldest, out.
    const std::vector<uint8_t> d = read_file(backup);
    if (d.empty()) return false;
    card_backup(card, dir, keep);
    std::ofstream out(card, std::ios::binary);
    out.write((const char*)d.data(), (std::streamsize)d.size());
    return (bool)out;
}
