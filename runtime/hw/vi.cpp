// Video Interface: retrace interrupts and framebuffer registers.
#include "../runtime.h"
#include <atomic>

static uint16_t g_vi[0x80];
static uint64_t g_frame_start;
// A field as the game set the display (DCR's format, bits 8-9): PAL's 50 Hz and 313 lines,
// else NTSC's (and MPAL's) 59.94 Hz and 263. A PAL disc in its 50 Hz mode runs at its speed.
static bool pal() { return ((g_vi[0x02 >> 1] >> 8) & 3) == 1; }
static uint64_t field_ticks() { return (uint64_t)(TB_FREQ / (pal() ? 50.0 : 59.94)); }
static int lines_per_field() { return pal() ? 313 : 263; }
std::atomic<uint32_t> g_vi_retrace_count{0};

static void vi_update_irq() {
    bool any = false;
    for (int i = 0; i < 4; i++) {
        uint16_t hi = g_vi[(0x30 + 4 * i) >> 1];
        if ((hi & 0x8000) && (hi & 0x1000)) any = true;
    }
    pi_set_interrupt(INT_VI, any);
}

static void vi_retrace() {
    g_frame_start = now_ticks();
    g_vi_retrace_count++;
    // Raise display interrupts 0 and 1 if enabled.
    for (int i = 0; i < 2; i++) {
        uint16_t& hi = g_vi[(0x30 + 4 * i) >> 1];
        if (hi & 0x1000) hi |= 0x8000;
    }
    vi_update_irq();
    event_schedule(g_frame_start + field_ticks(), vi_retrace);
}

void vi_init() {
    event_schedule_in(field_ticks(), vi_retrace);
}

uint16_t vi_read16(uint32_t off) {
    off &= 0xFF;
    switch (off) {
    case 0x2C: {  // DPV: current vertical position (1-based)
        uint64_t t = now_ticks() - g_frame_start;
        uint32_t line = (uint32_t)(t * lines_per_field() / field_ticks()) % lines_per_field();
        return (uint16_t)(line + 1);
    }
    case 0x2E: return 1;
    case 0x6E: return 0;  // DTV status: no component cable
    default: return g_vi[off >> 1];
    }
}

void vi_write16(uint32_t off, uint16_t v) {
    off &= 0xFF;
    if (getenv("GCN_VILOG") && off < 0x40 && g_vi[off >> 1] != v) fprintf(stderr, "[vi] reg %02X = %04X\n", off, v);
    g_vi[off >> 1] = v;
    if (off >= 0x30 && off < 0x40) vi_update_irq();
}
