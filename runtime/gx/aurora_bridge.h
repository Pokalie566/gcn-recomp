// The game's GX command stream, translated for Aurora's command processor (aurora_bridge.cpp)
// and handed from the guest's thread to whichever thread drives the GPU (aurora_gpu.h; a
// --headless run just discards it).
//
// The stream comes in chunks. A chunk ends where the game waits on the GPU -- a draw-done or
// a token -- and where it shows a frame (a copy to the external framebuffer); in between,
// every 256 KB. The guest does not go past a wait until the GPU side has processed the chunk
// that leads to it (done()), as the hardware would not let it; and it stays at most one frame
// ahead of the GPU side, as the hardware's FIFO would hold it.
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace gx {

// Where in a chunk an XF load or a draw starts, for the in-between frames (aurora_interp.h).
struct Mark {
    uint32_t off;
    uint32_t sig;   // 0: an XF load; else a draw, and what it draws as far as the stream says
};

struct Chunk {
    std::vector<uint8_t> bytes;                     // Aurora's FIFO, big-endian like the hardware's
    std::vector<std::unique_ptr<uint8_t[]>> keep;   // what `bytes` points at besides guest RAM (palettes)
    std::vector<Mark> marks;                        // its XF loads and draws, in order
    uint64_t seq = 0;
    bool frame_end = false;                         // the last chunk of a frame: present it
    // Points at nothing the guest may change (vertex arrays copied too): the GPU side may
    // hold it, and say it is done before processing it. See g_hold_frames.
    bool held = false;
};

// GPU side. The next chunk, waiting up to `timeout_ms`; false if none came.
bool take_chunk(Chunk& out, int timeout_ms);
// Everything up to chunk `seq` has been processed: the guest may go on past it.
void chunk_done(uint64_t seq);

// Frames the game has finished, the input scripts' time base.
extern std::atomic<uint32_t> g_frames_submitted;

// Set by the GPU side for frames it will hold back (the in-between frames of
// aurora_interp.h): from the next frame on, the chunks are `held`.
extern std::atomic<bool> g_hold_frames;

}  // namespace gx
