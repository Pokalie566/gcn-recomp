// Which discs a game project knows: its tables' versions.txt, one line per release,
//
//   # id    region   main.dol SHA-1   Redump SHA-1 of the whole image   supported
//   GLME01  NTSC-U   77d4...          96a7...                           yes
//
// `<...>` (e.g. `<à remplir>`) or `-` for a hash nobody has filled in yet; yes/oui or no/non.
// A project without the file supports one version: gcn_add_game()'s DISC_ID and DOL_SHA1.
//
// The main.dol decides: it is what the tables' addresses are written for, and it reads the
// same from a .iso, .ciso, .rvz or a trimmed (.nkit) image. The Redump hash is only ever
// information: anything but a full raw dump hashes differently, and plays just as well.
#pragma once
#include <string>
#include <vector>

struct DiscVersion {
    std::string id, region, dol_sha1, iso_sha1;   // a hash nobody filled in is empty
    bool supported = false;
};

std::vector<DiscVersion> versions_parse(const std::string& text);
// The tables' versions.txt, else the one version the build was configured with.
std::vector<DiscVersion> versions_load(const std::string& path);

struct DiscVerdict {
    bool playable = false;
    std::string message;   // why not, or which version it is: English with {} for `arg`, for ui_trf
    std::string arg;
    std::string text() const;   // message with arg in it
};
// `gamecube`: the image has the GameCube magic word. Messages are in English, for ui_tr.
DiscVerdict versions_judge(const std::vector<DiscVersion>& table, bool gamecube, const std::string& id,
                           const std::string& dol_sha1);
// What the whole image's SHA-1 says about the dump, once it has been read through
// (preparing does): `playable` is whether it is the Redump dump. Never a reason to refuse it.
DiscVerdict versions_dump_info(const std::vector<DiscVersion>& table, const std::string& id,
                               const std::string& iso_sha1);
