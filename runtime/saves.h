// The player's saves: what is on a memory card image, and copies of the card kept beside it.
//
// The card is the raw image hw/memcard.cpp keeps (block 0 the header, blocks 1 and 2 the
// directory and its backup, the newer one by its update counter, 127 entries of 0x40 bytes).
// Copies are whole cards, memcard-<date>-<time>.raw, so a restore is a file copy and a copy
// opens in Dolphin's memory card manager as it is.
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

// The files on a card image. Empty if there is no such file or it is not a card.
std::vector<CardFile> card_files(const std::string& card);

struct CardBackup {
    std::string path;
    std::string when;       // "2026-10-10 03:12", from the name
};

// The copies in `dir`, newest first.
std::vector<CardBackup> card_backups(const std::string& dir);

// Copies `card` into `dir`, unless the newest copy there is the same, keeping the `keep`
// newest. True if a copy was made.
bool card_backup(const std::string& card, const std::string& dir, int keep);

// Puts a copy in the card's place, after copying the card itself (so a restore can be undone).
bool card_restore(const std::string& backup, const std::string& card, const std::string& dir, int keep);
