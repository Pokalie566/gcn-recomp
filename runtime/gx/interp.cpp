// Frame interpolation: the geometry of a frame halfway between two of the game's. See
// interp.h. Free of GL, so that it can be tested without a GPU.
#include "interp.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

namespace gx {
namespace {
using DrawKey = std::tuple<uint8_t, uint32_t, uint32_t, uint32_t, uint32_t>;

DrawKey draw_key(const Batch& b, const Cmd& c) {
    const uint32_t first = b.verts[b.indices[c.first]].src_idx;
    const uint32_t last = b.verts[b.indices[c.first + c.count - 1]].src_idx;
    return {c.prim, c.count, first, last, b.states[c.state].tex_id[0]};
}

// How far draw `p` of the previous frame is from draw `c` of this one, sampled at its first,
// middle and last vertex: the squared distance moved relative to the squared distance from
// the viewer -- the nearer of before and after, or a jump away from the camera would never
// look like one -- the worst of the three.
float draw_distance(const Batch& cur, const Cmd& c, const Batch& prev, const Cmd& p) {
    float worst = 0;
    for (uint32_t k : {0u, c.count / 2, c.count - 1}) {
        const float* a = prev.verts[prev.indices[p.first + k]].pos;
        const float* z = cur.verts[cur.indices[c.first + k]].pos;
        const float dx = z[0] - a[0], dy = z[1] - a[1], dz = z[2] - a[2];
        const float ref = std::min(a[0] * a[0] + a[1] * a[1] + a[2] * a[2], z[0] * z[0] + z[1] * z[1] + z[2] * z[2]);
        worst = std::max(worst, (dx * dx + dy * dy + dz * dz) / (ref + 1e-6f));
    }
    return worst;
}

// One vertex halfway between its previous state `a` and its current one, into `m`.
void blend_vertex(const GpuVertex& a, GpuVertex& m) {
    for (int i = 0; i < 3; i++) m.pos[i] = 0.5f * (m.pos[i] + a.pos[i]);
    for (int ch = 0; ch < 2; ch++)
        for (int i = 0; i < 4; i++) m.col[ch][i] = (uint8_t)((m.col[ch][i] + a.col[ch][i] + 1) / 2);
    for (int t = 0; t < 8; t++) {
        const float* ta = a.tex[t];
        float* tm = m.tex[t];
        // A coordinate that scrolls and wraps comes back by a whole texture from one frame
        // to the next; its midpoint would be the wrong half. Such a set keeps its value.
        const float span = std::max(1.0f, std::fabs(tm[2]));
        if (std::fabs(tm[0] - ta[0]) > 0.5f * span || std::fabs(tm[1] - ta[1]) > 0.5f * span) continue;
        for (int i = 0; i < 3; i++) tm[i] = 0.5f * (tm[i] + ta[i]);
    }
}

}  // namespace

bool interp_build_midframe(const Batch& cur, const Batch& prev, Batch& mid, InterpStats* stats) {
    std::map<DrawKey, std::vector<const Cmd*>> before;
    for (const Cmd& c : prev.cmds)
        if (c.type == CmdType::Draw && c.count) before[draw_key(prev, c)].push_back(&c);
    constexpr float kTeleport = 1.0f;          // squared: moved further than its distance
    constexpr float kFar = 0.25f * 0.25f;      // squared: a quarter of its distance

    mid.cmds = cur.cmds;
    mid.verts = cur.verts;
    mid.indices = cur.indices;
    mid.states = cur.states;
    mid.mtxs = cur.mtxs;
    mid.new_textures = cur.new_textures;

    std::vector<uint8_t> moved(mid.verts.size(), 0);
    uint32_t draws = 0, matched = 0, far = 0;
    for (const Cmd& c : cur.cmds) {
        if (c.type != CmdType::Draw || !c.count) continue;
        draws++;
        auto it = before.find(draw_key(cur, c));
        if (it == before.end() || it->second.empty()) continue;
        auto& candidates = it->second;
        size_t best = 0;
        float best_d = draw_distance(cur, c, prev, *candidates[0]);
        for (size_t i = 1; i < candidates.size(); i++) {
            const float d = draw_distance(cur, c, prev, *candidates[i]);
            if (d < best_d) { best = i; best_d = d; }
        }
        const Cmd& p = *candidates[best];
        candidates.erase(candidates.begin() + best);
        matched++;
        if (best_d > kFar) far++;
        if (best_d > kTeleport) continue;
        for (uint32_t k = 0; k < c.count; k++) {
            const uint32_t v = cur.indices[c.first + k];
            if (moved[v]) continue;
            moved[v] = 1;
            blend_vertex(prev.verts[prev.indices[p.first + k]], mid.verts[v]);
        }
    }
    if (stats) *stats = {draws, matched, far};
    return draws && matched * 2 >= draws && far * 2 <= matched;
}

}  // namespace gx
