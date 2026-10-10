// Which discs a game project knows. See versions.h.
#include "versions.h"
#include <fstream>
#include <regex>
#include <sstream>

#ifndef GCN_DISC_ID   // a test builds this file on its own
#define GCN_DISC_ID ""
#define GCN_DOL_SHA1 ""
#endif

static std::string hash_or_empty(const std::string& s) { return s == "-" || s[0] == '<' ? "" : s; }

std::vector<DiscVersion> versions_parse(const std::string& text) {
    std::vector<DiscVersion> out;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        line = line.substr(0, line.find('#'));
        line = std::regex_replace(line, std::regex("<[^>]*>"), "-");   // `<à remplir>` holds a space
        std::istringstream f(line);
        DiscVersion v;
        std::string dol, iso, sup;
        if (!(f >> v.id >> v.region >> dol >> iso >> sup)) continue;
        v.dol_sha1 = hash_or_empty(dol);
        v.iso_sha1 = hash_or_empty(iso);
        v.supported = sup == "yes" || sup == "oui";
        out.push_back(v);
    }
    return out;
}

std::vector<DiscVersion> versions_load(const std::string& path) {
    std::ifstream in(path);
    if (!in) return {{GCN_DISC_ID, "", GCN_DOL_SHA1, "", true}};
    std::stringstream s;
    s << in.rdbuf();
    return versions_parse(s.str());
}

DiscVerdict versions_judge(const std::vector<DiscVersion>& table, bool gamecube, const std::string& id,
                           const std::string& dol_sha1) {
    if (!gamecube) return {false, "This is not a GameCube disc image", ""};
    const DiscVersion* same_id = nullptr;
    for (const auto& v : table) {
        if (v.id != id) continue;
        if (!same_id || v.supported) same_id = &v;
        if (v.dol_sha1 != dol_sha1) continue;
        if (v.supported) return {true, "{}", v.region + ", " + id};
        return {false, "Recognized ({}), but this version is not supported yet", v.region + ", " + id};
    }
    if (!same_id) return {false, "This disc is another game ({})", id};
    // The id is known but the main.dol is not one of the listed ones: a modified or a
    // damaged dump -- or a version whose main.dol hash nobody has filled in yet.
    if (same_id->supported) return {false, "This {} disc's main.dol is not the original: a modified or incomplete dump", id};
    return {false, "Recognized ({}), but this version is not supported yet", same_id->region + ", " + id};
}

std::string DiscVerdict::text() const {
    std::string s = message;
    if (const size_t at = s.find("{}"); at != std::string::npos) s.replace(at, 2, arg);
    return s;
}

DiscVerdict versions_dump_info(const std::vector<DiscVersion>& table, const std::string& id,
                               const std::string& iso_sha1) {
    for (const auto& v : table)
        if (v.id == id && !v.iso_sha1.empty() && v.iso_sha1 == iso_sha1) return {true, "Matches Redump ({})", v.region};
    return {false, "Not a raw Redump dump (compressed, trimmed or another dump): it plays just as well", ""};
}
