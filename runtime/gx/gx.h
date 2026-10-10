// GX (Flipper graphics): the command stream's entry point and the registers it sets. The
// stream is parsed and translated for Aurora by gx/aurora_bridge.cpp.
#pragma once
#include <cstdint>

namespace gx {

// The BP and CP registers as the game last wrote them (XF goes to Aurora).
struct State {
    uint32_t bp[256];
    uint32_t cp[256];
    uint32_t bp_mask = 0xFFFFFF;
};

extern State g_state;

// Byte-size of one vertex for the given VAT index, based on current VCD/VAT.
uint32_t vertex_size(int vat);

// Process a buffer of FIFO commands. Returns bytes consumed (may stop early on a
// partial command when `partial_ok`).
uint32_t process(const uint8_t* data, uint32_t len, bool partial_ok);

}  // namespace gx
