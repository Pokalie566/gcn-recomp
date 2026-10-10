// The memory card for the benchmark, which has none: the game's CARD calls (hle_card.cpp)
// find no card.
#include "recomp.h"

namespace {
constexpr int32_t kNoCard = -3;   // CARD_RESULT_NOCARD
}

#define NO_CARD(name) extern "C" void hle_##name(CPU* c) { c->r[3] = (uint32_t)kNoCard; }
NO_CARD(CARDProbeEx) NO_CARD(CARDMount) NO_CARD(CARDMountAsync) NO_CARD(CARDUnmount) NO_CARD(CARDCheck)
NO_CARD(CARDCheckExAsync) NO_CARD(CARDFormat) NO_CARD(CARDFormatAsync) NO_CARD(CARDFreeBlocks) NO_CARD(CARDOpen)
NO_CARD(CARDClose) NO_CARD(CARDCreate) NO_CARD(CARDCreateAsync) NO_CARD(CARDRead) NO_CARD(CARDReadAsync)
NO_CARD(CARDWrite) NO_CARD(CARDWriteAsync) NO_CARD(CARDGetStatus) NO_CARD(CARDSetStatus) NO_CARD(CARDSetStatusAsync)
extern "C" void hle_CARDInit(CPU*) {}
extern "C" void hle_CARDProbe(CPU* c) { c->r[3] = 0; }
