// THE INJECTION's BOXES, FOLDED (PHOTON-7's walk hitch). Pure integer geometry - no Ogre - so a CPU test reads the law
// (tests/gi/test_inject_fold.cpp, gi.inject_fold). A box is { lo x, lo y, lo z, hi x, hi y, hi z } in a cascade's cells.
#ifndef JAHSHAKA_ENGINE_PHOTON_INJECTBOXES_H
#define JAHSHAKA_ENGINE_PHOTON_INJECTBOXES_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <vector>

namespace jahshaka {
namespace engine {
namespace detail {

// THE INJECTION's BOXES, FOLDED TO ITS COUNT (PHOTON-7's walk hitch: the union of every region, the fold that stood here,
// covered the whole cascade once a commit's regions lay at both ends of two axes - Bistro Interior's four lamps and its sun:
// 100 % of 128^3 for a 1.4 % slab). Greedily merges the PAIR whose union adds the fewest cells until `maxBoxes` remain: a
// merged box holds both, so the set still covers every cell it held (a bound), and opposite faces stay apart.
inline void foldInjectBoxes(std::vector<std::array<int, 6>> &boxes, size_t maxBoxes) {
    const auto volume = [](const std::array<int, 6> &b) {
        return (long long)(b[3] - b[0]) * (long long)(b[4] - b[1]) * (long long)(b[5] - b[2]);
    };
    const auto merged = [](const std::array<int, 6> &x, const std::array<int, 6> &y) {
        std::array<int, 6> u{};
        for (size_t a = 0; a < 3u; ++a) {
            u[a] = std::min(x[a], y[a]);
            u[a + 3u] = std::max(x[a + 3u], y[a + 3u]);
        }
        return u;
    };
    while (boxes.size() > std::max<size_t>(maxBoxes, 1u)) {
        size_t bi = 0, bj = 1;
        long long best = std::numeric_limits<long long>::max();
        for (size_t i = 0; i < boxes.size(); ++i)
            for (size_t k = i + 1; k < boxes.size(); ++k) {
                const long long add = volume(merged(boxes[i], boxes[k])) - volume(boxes[i]) - volume(boxes[k]);
                if (add < best) { best = add; bi = i; bj = k; }
            }
        boxes[bi] = merged(boxes[bi], boxes[bj]);
        boxes.erase(boxes.begin() + std::ptrdiff_t(bj));
    }
}

}  // namespace detail
}  // namespace engine
}  // namespace jahshaka

#endif
