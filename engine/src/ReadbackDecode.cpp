// See ReadbackDecode.h. Compiled at -O2 in Debug (irisgl/engine/CMakeLists.txt).
#include "ReadbackDecode.h"

#include <algorithm>
#include <cstring>

namespace jahshaka { namespace engine { namespace detail {

void decodeGatherHistory(const uint32_t *src, unsigned w, unsigned h, unsigned layers,
                         std::vector<unsigned char> &count, std::vector<unsigned char> &cls,
                         std::vector<float> &dist) {
    const size_t layerTexels = size_t(w) * h;
    const size_t pairW = size_t(w) * layers;
    count.assign(pairW * h, 0u);
    cls.assign(pairW * h, 0u);
    dist.assign(pairW * h, 0.0f);
    for (unsigned k = 0; k < layers; ++k)
        for (unsigned y = 0; y < h; ++y) {
            const uint32_t *row = src + (size_t(k) * layerTexels + size_t(y) * w) * 2u + 1u;
            unsigned char *c = count.data() + size_t(y) * pairW + size_t(k) * w;
            unsigned char *f = cls.data() + size_t(y) * pairW + size_t(k) * w;
            float *d = dist.data() + size_t(y) * pairW + size_t(k) * w;
            for (unsigned x = 0; x < w; ++x, row += 2) {
                const uint32_t g = *row;
                c[x] = (unsigned char)((g >> 22u) & 15u);
                f[x] = (unsigned char)((g >> 21u) & 1u);
                const uint32_t bits = (g & 0x1FFFFFu) << 10u;   // packGeom's distance (jah_probe_history.glsl)
                std::memcpy(&d[x], &bits, sizeof(float));
            }
        }
}

void decodeReflectHistory(const uint16_t *src, unsigned w, unsigned h, unsigned layers,
                          std::vector<unsigned short> &out) {
    const size_t layerTexels = size_t(w) * h;
    const size_t pairW = size_t(w) * layers;
    out.assign(pairW * h, 0u);
    // THE COUNT FROM THE HALF'S BITS: a half holds every whole number up to 2048 exactly (11 significant bits) and
    // only even ones above it, so a count past 2048 reads to within one — the histogram's edges (9, 17, 33) are far
    // below that. A value under one (a subnormal, zero, a sign) or not finite is no count; else the mantissa with
    // its hidden bit, shifted.
    for (unsigned k = 0; k < layers; ++k)
        for (unsigned y = 0; y < h; ++y) {
            const uint16_t *row = src + (size_t(k) * layerTexels + size_t(y) * w) * 4u + 3u;
            unsigned short *dst = out.data() + size_t(y) * pairW + size_t(k) * w;
            for (unsigned x = 0; x < w; ++x, row += 4) {
                const unsigned hv = *row, e = (hv >> 10u) & 31u;
                dst[x] = (hv & 0x8000u) || e < 15u || e == 31u
                             ? (unsigned short)0u
                             : (unsigned short)std::min(((1024u + (hv & 1023u)) << (e - 15u)) >> 10u, 4096u);
            }
        }
}

}}}
