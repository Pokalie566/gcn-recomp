// The in-between frame. See aurora_interp.h.
#include "aurora_interp.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace gx {

namespace {

uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
void put32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v; }
float word_f(const uint8_t* p) { const uint32_t u = be32(p); float f; memcpy(&f, &u, 4); return f; }

// The XF words that hold a transform, floats all: position, texture and normal matrices,
// the dual-texture matrices, a light's position and direction, the projection.
bool moves(uint32_t a) {
    return a < 0x100 || (a >= 0x400 && a < 0x460) || (a >= 0x500 && a < 0x600) ||
           (a >= 0x600 && a < 0x680 && (a & 0xF) >= 10) || (a >= 0x1020 && a < 0x1026);
}

struct Load {
    uint32_t chunk, off;   // where it is
    uint32_t addr, n;
    const uint8_t* data;   // n big-endian words
};

// The loads that lead to a run of draws, and what those draws draw.
struct Group {
    std::vector<Load> loads;
    uint32_t sig = 2166136261u;
};

bool moving(const Load& l) {
    for (uint32_t i = 0; i < l.n; i++)
        if (moves(l.addr + i)) return true;
    return false;
}

// A frame's moving loads, grouped by the draws that follow them.
std::vector<Group> groups_of(const std::vector<Chunk>& frame) {
    std::vector<Group> out(1);
    bool drawn = false;
    for (uint32_t ci = 0; ci < frame.size(); ci++)
        for (const Mark& m : frame[ci].marks) {
            const uint8_t* p = &frame[ci].bytes[m.off];
            if (m.sig) {
                out.back().sig = (out.back().sig ^ m.sig) * 16777619u;
                drawn = true;
                continue;
            }
            if (drawn) out.emplace_back(), drawn = false;
            const uint32_t h = be32(p + 1);
            const Load l{ci, m.off, h & 0xFFFF, ((h >> 16) & 0xF) + 1, p + 5};
            if (moving(l)) out.back().loads.push_back(l);
        }
    return out;
}

uint64_t key(const Load& l) { return (uint64_t)l.addr << 5 | l.n; }

}  // namespace

bool interpolate_frame(const std::vector<Chunk>& prev, const std::vector<Chunk>& cur, float t,
                       std::vector<std::vector<uint8_t>>& out, InterpStats* stats) {
    InterpStats st;
    // The previous frame's groups by what they draw; the k-th group drawing something here
    // is matched with the k-th drawing the same there.
    const std::vector<Group> before = groups_of(prev);
    std::unordered_map<uint32_t, std::vector<const Group*>> by_sig;
    for (const Group& g : before) by_sig[g.sig].push_back(&g);
    std::unordered_map<uint32_t, uint32_t> sig_rank;

    out.clear();
    for (const Chunk& c : cur) out.push_back(c.bytes);
    for (const Group& g : groups_of(cur)) {
        st.loads += (uint32_t)g.loads.size();
        const auto it = by_sig.find(g.sig);
        const uint32_t gk = sig_rank[g.sig]++;
        if (it == by_sig.end() || gk >= it->second.size()) continue;
        const Group& was = *it->second[gk];
        // Within the group, loads matched by address, size and rank.
        std::unordered_map<uint64_t, std::vector<const Load*>> then;
        for (const Load& l : was.loads) then[key(l)].push_back(&l);
        std::unordered_map<uint64_t, uint32_t> rank;
        for (const Load& l : g.loads) {
            const auto m = then.find(key(l));
            const uint32_t k = rank[key(l)]++;
            if (m == then.end() || k >= m->second.size()) continue;
            st.matched++;
            const uint8_t* a = m->second[k]->data;
            // A projection of another kind (perspective, orthographic) is not in between.
            if (l.addr <= 0x1026 && l.addr + l.n > 0x1026 && be32(a + 4 * (0x1026 - l.addr)) != be32(l.data + 4 * (0x1026 - l.addr))) {
                st.far++;
                continue;
            }
            float diff = 0, size = 0;
            bool finite = true;
            for (uint32_t i = 0; i < l.n; i++) {
                if (!moves(l.addr + i)) continue;
                const float x = word_f(a + 4 * i), y = word_f(l.data + 4 * i);
                finite = finite && std::isfinite(x) && std::isfinite(y);
                diff = std::max(diff, std::fabs(y - x));
                size = std::max(size, std::max(std::fabs(x), std::fabs(y)));
            }
            if (!finite || diff > 0.5f * size) {
                st.far++;
                continue;
            }
            uint8_t* dst = &out[l.chunk][l.off + 5];
            for (uint32_t i = 0; i < l.n; i++) {
                if (!moves(l.addr + i)) continue;
                const float x = word_f(a + 4 * i), y = word_f(l.data + 4 * i), v = x + (y - x) * t;
                uint32_t u;
                memcpy(&u, &v, 4);
                put32(dst + 4 * i, u);
            }
        }
    }
    st.cut = st.matched * 2 < st.loads || st.far * 2 > st.matched;
    if (stats) *stats = st;
    if (st.cut) out.clear();
    return !st.cut;
}

}  // namespace gx
