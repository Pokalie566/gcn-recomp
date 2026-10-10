// The GPU side of the game's GX stream (aurora_bridge.h): on the thread that owns the window,
// Aurora and the menus, the stream's chunks are handed to Aurora's command processor and
// every finished frame is presented, with the interface over it (Aurora draws RmlUi as it
// presents).
//
// The last finished frame is kept, so that a paused game -- or the interface over it -- can
// be shown again without the guest: gpu_present_again() replays it. A frame the guest was in
// the middle of when it stopped is replayed too when it goes on, so that a pause leaves no
// trace in what is drawn.
#pragma once
#include <cstdint>

// Before the guest starts.
void gpu_init();
// Takes what the guest has sent, up to `timeout_ms` for the first chunk; presents each frame
// it finishes. True if a frame was presented.
bool gpu_pump(int timeout_ms);
// The last finished frame again (none yet: black), with the interface over it. While the
// guest is stopped, or before it runs.
void gpu_present_again();
// --dump-dir=DIR --dump-every=N: every Nth presented frame as DIR/frame_NNNNN.png
// (GCN_DUMP_RANGE=a-b: only those). And the frame as it stands, as DIR/<name>.png.
void gpu_set_dump(const char* dir, int every);
void gpu_dump(const char* name);
uint32_t gpu_frames_presented();
