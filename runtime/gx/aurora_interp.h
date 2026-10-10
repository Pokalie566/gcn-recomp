// 60 fps from a 30 fps game on Aurora: an in-between frame, the game's frame replayed with
// its transforms halfway between the previous frame's and its own.
//
// Aurora transforms vertices on the GPU, so what moves an object between two frames is the
// matrices the game loads into XF: position and normal matrices, texture matrices, the
// projection, light positions. The in-between frame is the new frame's stream with each of
// those loads set to the midpoint of its value and the matching load of the previous frame.
// Loads are matched through the draws they lead to (Mark::sig: primitive, size, first and
// last indices): the loads before a run of draws form a group, the k-th group drawing
// something in one frame matches the k-th drawing the same in the other, and within it the
// loads match by address, size and rank. Everything else is the new frame's as it is.
//
// A load whose value jumped further than half its own size (a teleport, a camera cut, a
// texture scroll wrapping round) keeps the new value; a frame where under half of the loads
// find their match, or over half of the matched ones jumped, is a cut: no in-between.
//
// Free of Aurora and of the GPU: tests/aurora_interp_test.cpp drives it with made-up streams.
#pragma once
#include "aurora_bridge.h"
#include <cstdint>
#include <vector>

namespace gx {

struct InterpStats {
    uint32_t loads = 0;     // XF loads of something that moves
    uint32_t matched = 0;   // ...with a match in the previous frame
    uint32_t far = 0;       // ...of which jumped too far to be moved
    bool cut = false;       // no in-between for this frame
};

// The bytes of `cur`'s chunks, with its XF loads moved to `t` of the way from `prev`'s
// (0: the previous frame's values, 1: its own). False, `out` left empty, if it is a cut.
bool interpolate_frame(const std::vector<Chunk>& prev, const std::vector<Chunk>& cur, float t,
                       std::vector<std::vector<uint8_t>>& out, InterpStats* stats = nullptr);

}  // namespace gx
