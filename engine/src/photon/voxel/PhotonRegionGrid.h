#pragma once
// THE PASS'S GRID (PHOTON-7 stage 2; NONDET-1 H2): the texels [lo, lo + n) of a level whose texel covers `cellsPerTexel`
// volume cells - [regionLo, regionHi) of level 0 rounded OUT (every texel a changed cell reaches), or the whole level
// `levelRes` when no region is given. Pure arithmetic: PhotonVoxelLighting's setRegionGrid sets the job's origin and
// its groups from it, and engine.region_grid_law reads it on the CPU.
#include <algorithm>
#include <cstdint>

namespace jahshaka::engine::detail {

/// False when the box holds no texel of the level on some axis (an empty box, or one past the level's end: h <= l). Its
/// pass dispatches NOTHING then: a group dispatched for an empty box lit the cells [res, res + 4) past the edge, and the
/// half store's layout puts a cell at x >= W into the next half's block, in bounds.
inline bool regionGridOf(const std::uint32_t *regionLo, const std::uint32_t *regionHi, std::uint32_t cellsPerTexel,
                         const std::uint32_t levelRes[3], std::uint32_t lo[3], std::uint32_t n[3])
{
    bool texels = true;
    for (int a = 0; a < 3; ++a) {
        std::uint32_t l = 0u, h = levelRes[a];
        if (regionLo && regionHi) {
            l = std::min(regionLo[a] / cellsPerTexel, levelRes[a]);
            h = std::min((regionHi[a] + cellsPerTexel - 1u) / cellsPerTexel, levelRes[a]);
        }
        lo[a] = l;
        n[a] = h > l ? h - l : 0u;
        texels = texels && h > l;
    }
    return texels;
}

}   // namespace jahshaka::engine::detail
