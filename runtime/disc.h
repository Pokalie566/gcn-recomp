// The disc image, read through nod (github.com/encounter/nod): .iso, .ciso, .gcz, .rvz,
// .wia, .wbfs, .nfs, .tgc and trimmed (.nkit) images alike, as the raw disc they hold.
// hw/di.cpp emulates the drive's registers on top of it; the boot, the launcher and the
// preparation read it too.
#pragma once
#include <cstdint>
#include <functional>
#include <string>

// Opens a disc image, closing the one before: a launcher reads one disc after another.
// False, with nod's reason, if it is not a disc image nod can read.
bool disc_open(const char* path, std::string* err = nullptr);
// The last disc opened: its game code, region and maker ("GLMP01"); the build's own disc
// (GCN_DISC_ID) before one is. One recompiled game takes every version of its disc.
extern char g_disc_id[7];
// The open disc, as the raw disc it holds. False past its end, or with none open.
bool iso_read(uint64_t offset, void* dst, uint32_t len);

// Writes a copy of the open disc to `dst`, so that the player's own image is no longer
// needed: a CISO holding the blocks the game can read -- the boot area, main.dol, the file
// system and every file in it -- and nothing else (the padding between files reads as
// zero). Reads the whole disc on the way, for the SHA-1 of it: Redump's, for a full raw dump
// or a lossless RVZ. Written next to `dst` first, then moved into place.
bool disc_copy_ciso(const std::string& dst, std::string* sha1, std::string* err,
                    const std::function<void(float)>& progress);
