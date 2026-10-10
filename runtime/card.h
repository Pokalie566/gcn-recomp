// The memory card: Aurora's (hle_card.cpp), a folder of .gci files.
#pragma once
#include <string>

// Where the card lives: Aurora's folders under it (<base>/USA/Card A, as Dolphin's). Null:
// save_path("card"). A scripted run or a replay points it at a card of its own.
extern const char* g_card_base;

// Before the game starts: the card mounted, an older raw card's saves taken over once.
void card_init();
// The folder of .gci files of slot A.
std::string card_folder();
// After the folder was changed from outside (a copy restored).
void card_remount();
