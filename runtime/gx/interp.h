#pragma once
#include "render.h"

namespace gx {

// A game that runs its logic at 30 fps can be shown at 60 by drawing, between two of its
// frames, one more halfway between them. Vertices arrive already transformed -- to view
// space, to a light's space for a shadow pass, to the 2D frame -- and with their texture
// coordinates and colours already generated, so "halfway" is the midpoint of each of those
// between the previous frame and this one. That follows the camera, the objects and
// whatever is projected onto them alike, and keeps a render-to-texture pass and the draws
// that sample its result in step.
//
// That needs each vertex of this frame matched with the same vertex of the previous one.
// A draw is matched with a draw of the previous frame that has the same shape: primitive,
// vertex count, first texture, and the position-array indices of its first and last vertex
// (GpuVertex::src_idx); among several equals -- a model drawn more than once a frame, for
// its shadow or its silhouette -- the one whose vertices are nearest. Index k of one names
// the same vertex as index k of the other. A strip or a fan names most of its vertices
// more than once, and each must be moved once: moved again, it creeps further towards the
// previous frame than its neighbours and the mesh cracks. What has no match -- a particle
// that was just born, a model that just appeared -- is drawn where it is now.
//
// A matched draw is blended whole or not at all: blending some of a model's draws and not
// others tears it apart. The only draws left alone are those that jumped further than their
// distance from the viewer, before or after, whichever is nearer -- a teleport. When fewer than half of the draws match, or
// more than half of the matched ones moved more than a quarter of their distance, the
// frame is a cut and no in-between frame is drawn: blending two different shots is worse
// than showing one of them for two refreshes.
//
// render_execute_interpolated (render_gl.h) draws the result.
struct InterpStats { uint32_t draws, matched, far; };

// Builds `mid` from `cur`, each matched draw halfway back towards `prev`. False if the frame
// is a cut, in which case `mid` is not to be drawn. `stats`, if given, says why.
bool interp_build_midframe(const Batch& cur, const Batch& prev, Batch& mid, InterpStats* stats = nullptr);

}  // namespace gx
