// Input logging and replay. See input_log.h.
#include "input_log.h"
#include <filesystem>
#include "runtime.h"
#include "platform.h"
#include <cstdio>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <mutex>
#include <vector>

uint32_t gx_frames_submitted();

namespace {

struct Entry {
    uint32_t key;  // poll sequence number (v2) or presented frame (v1)
    uint8_t chan;
    PadState s;
};

uint32_t g_poll_seq;  // pad polls so far, all channels

// ---- recording ----
FILE* g_out;
PadState g_last[4];
bool g_have_last[4];
std::mutex g_mutex;

bool copy_file(const std::string& from, const std::string& to) {
    FILE* in = fopen(from.c_str(), "rb");
    if (!in) return false;
    FILE* out = fopen(to.c_str(), "wb");
    if (!out) { fclose(in); return false; }
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) fwrite(buf, 1, n, out);
    fclose(in);
    fclose(out);
    return true;
}

bool same(const PadState& a, const PadState& b) {
    return a.connected == b.connected && a.buttons == b.buttons && a.stick_x == b.stick_x &&
           a.stick_y == b.stick_y && a.cstick_x == b.cstick_x && a.cstick_y == b.cstick_y &&
           a.trig_l == b.trig_l && a.trig_r == b.trig_r;
}

// ---- replay ----
std::vector<Entry> g_replay;     // sorted by key, as logged
size_t g_replay_pos;             // first entry not yet applied
PadState g_replay_state[4];
bool g_replay_loaded;
bool g_replay_by_frame;          // a v1 log
uint32_t g_replay_last_key;      // the last change, or the end marker when the log has one

uint32_t replay_key() { return g_replay_by_frame ? gx_frames_submitted() : g_poll_seq; }

void record(int chan, const PadState& s) {
    if (!g_out) return;
    if (g_have_last[chan] && same(g_last[chan], s)) return;
    g_last[chan] = s;
    g_have_last[chan] = true;
    fprintf(g_out, "%u %u %d %d %04X %02X %02X %02X %02X %02X %02X\n", g_poll_seq, gx_frames_submitted(), chan,
            s.connected ? 1 : 0, s.buttons, s.stick_x, s.stick_y, s.cstick_x, s.cstick_y, s.trig_l, s.trig_r);
    // Flushed per line: the point of the log is to survive whatever happens next.
    fflush(g_out);
}

}  // namespace

// A card folder (card.h), copied whole. False if it could not be.
static bool copy_dir(const std::string& from, const std::string& to) {
    std::error_code ec;
    std::filesystem::remove_all(to, ec);
    std::filesystem::create_directories(to, ec);
    std::filesystem::copy(from, to, std::filesystem::copy_options::recursive, ec);
    return !ec;
}

bool input_log_start(const std::string& dir, const std::string& card_base) {
    plat_make_dirs(dir);
    const std::string path = dir + "/inputs.txt";
    g_out = fopen(path.c_str(), "wb");
    if (!g_out) {
        fprintf(stderr, "[input] cannot write %s; not logging input\n", path.c_str());
        return false;
    }
    char stamp[64] = "";
    time_t now = time(nullptr);
    strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S", localtime(&now));
    rtc_set_base(now);
    fprintf(g_out, "# gcn-recomp input log v2\n# game %s  started %s\n# epoch %lld\n", g_disc_id, stamp, (long long)now);
    fprintf(g_out, "# poll frame chan connected buttons stick_x stick_y cstick_x cstick_y trig_l trig_r\n");
    fprintf(g_out, "# (hex; each line is that channel's state from that pad poll on)\n");
    fflush(g_out);
    std::error_code ec;
    if (std::filesystem::is_directory(card_base, ec) && !copy_dir(card_base, dir + "/card"))
        fprintf(stderr, "[input] could not snapshot %s into the log\n", card_base.c_str());
    fprintf(stderr, "[input] logging to %s\n", path.c_str());
    return true;
}


void input_log_end() {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!g_out) return;
    fprintf(g_out, "# end %u %u\n", g_poll_seq, gx_frames_submitted());
    fflush(g_out);
}

bool input_replay_load(const std::string& dir, std::string& card_out) {
    const std::string path = dir + "/inputs.txt";
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        fprintf(stderr, "[input] no %s to replay\n", path.c_str());
        return false;
    }
    char line[256];
    int version = 1;
    uint32_t end_key = 0;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#') {
            long long epoch;
            unsigned end_poll, end_frame;
            if (sscanf(line, "# gcn-recomp input log v%d", &version) == 1) continue;
            if (sscanf(line, "# epoch %lld", &epoch) == 1) rtc_set_base((time_t)epoch);
            if (sscanf(line, "# end %u %u", &end_poll, &end_frame) == 2) end_key = version >= 2 ? end_poll : end_frame;
            continue;
        }
        if (line[0] == '\n') continue;
        unsigned key, frame, chan, conn, buttons, sx, sy, cx, cy, l, r;
        if (version >= 2) {
            if (sscanf(line, "%u %u %u %u %x %x %x %x %x %x %x", &key, &frame, &chan, &conn, &buttons, &sx, &sy, &cx, &cy, &l, &r) != 11) continue;
        } else {
            if (sscanf(line, "%u %u %u %x %x %x %x %x %x %x", &key, &chan, &conn, &buttons, &sx, &sy, &cx, &cy, &l, &r) != 10) continue;
        }
        if (chan > 3) continue;
        Entry e;
        e.key = key;
        e.chan = (uint8_t)chan;
        e.s.connected = conn != 0;
        e.s.buttons = (uint16_t)buttons;
        e.s.stick_x = (uint8_t)sx; e.s.stick_y = (uint8_t)sy;
        e.s.cstick_x = (uint8_t)cx; e.s.cstick_y = (uint8_t)cy;
        e.s.trig_l = (uint8_t)l; e.s.trig_r = (uint8_t)r;
        g_replay.push_back(e);
    }
    fclose(f);
    if (g_replay.empty()) {
        fprintf(stderr, "[input] %s holds no input\n", path.c_str());
        return false;
    }
    g_replay_last_key = std::max(g_replay.back().key, end_key);
    g_replay_by_frame = version < 2;
    for (auto& s : g_replay_state) { s = PadState(); s.connected = true; }
    g_replay_loaded = true;
    // The card as the run found it, in a scratch copy: a folder, or (an older log) a raw
    // image, which card_init takes over from there.
    card_out.clear();
    std::error_code ec;
    const std::string snap = dir + "/card", raw = dir + "/memcard_a.raw", scratch = dir + "/card_replay";
    bool ok = true;
    if (std::filesystem::is_directory(snap, ec)) ok = copy_dir(snap, scratch), card_out = scratch;
    else if (plat_readable(raw.c_str())) {
        std::filesystem::remove_all(scratch, ec);
        std::filesystem::create_directories(scratch, ec);
        ok = copy_file(raw, scratch + "/memcard_a.raw");
        card_out = scratch;
    }
    if (!ok) {
        fprintf(stderr, "[input] could not copy the card snapshot; replaying with the live card\n");
        card_out.clear();
    }
    fprintf(stderr, "[input] replaying %zu changes over %u %s from %s%s%s\n", g_replay.size(),
            g_replay_last_key, g_replay_by_frame ? "frames" : "polls", path.c_str(),
            card_out.empty() ? "" : " (with its card snapshot)",
            g_replay_by_frame ? " (v1 log: keyed by frame, so only approximate)" : "");
    return true;
}

bool input_pad_poll(int chan, PadState& s) {
    if (chan < 0 || chan > 3) return false;
    std::lock_guard<std::mutex> lk(g_mutex);
    bool replayed = false;
    if (g_replay_loaded) {
        const uint32_t key = replay_key();
        while (g_replay_pos < g_replay.size() && g_replay[g_replay_pos].key <= key) {
            const Entry& e = g_replay[g_replay_pos++];
            g_replay_state[e.chan] = e.s;
        }
        if (key > g_replay_last_key) {
            static bool said;
            if (!said) {
                said = true;
                fprintf(stderr, "[input] replay finished at poll %u (frame %u); live input from here\n", g_poll_seq, gx_frames_submitted());
            }
        } else {
            s = g_replay_state[chan];
            replayed = true;
        }
    }
    record(chan, s);
    g_poll_seq++;
    return replayed;
}
