// The player's saves: what is on the memory card, and copies of the card kept beside it.
//
// The card is a folder of .gci files, one per save (card.h): each starts with its 0x40-byte
// directory entry, then its blocks. Copies are whole folders, card-<date>-<time>/, so a
// restore is a copy of files, and a copy's files import in Dolphin as they are.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct CardFile {
    std::string game;       // game code and maker: "GLME01"
    std::string name;       // the file's name on the card
    std::string comment;    // the game's two comment lines, joined: what a GameCube menu shows
    uint16_t blocks = 0;
    uint32_t modified = 0;  // seconds since 2000-01-01, as the card stores it
};

// The saves in a card folder. Empty if there is no such folder.
std::vector<CardFile> card_files(const std::string& card);

struct CardBackup {
    std::string path;
    std::string when;       // "2026-10-10 03:12", from the name
};

// The copies in `dir`, newest first.
std::vector<CardBackup> card_backups(const std::string& dir);

// Copies the folder `card` into `dir`, unless it is empty or the newest copy there is the
// same, keeping the `keep` newest. True if a copy was made.
bool card_backup(const std::string& card, const std::string& dir, int keep);

// Puts a copy's saves in the card's place, after copying the card itself (so a restore can be
// undone). The card must be remounted after (card_remount).
bool card_restore(const std::string& backup, const std::string& card, const std::string& dir, int keep);
