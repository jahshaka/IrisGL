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

void decodeReflectHistory(const uint32_t *src, unsigned w, unsigned h, unsigned layers,
                          std::vector<unsigned short> &out) {
    const size_t layerTexels = size_t(w) * h;
    const size_t pairW = size_t(w) * layers;
    out.assign(pairW * h, 0u);
    for (unsigned k = 0; k < layers; ++k)
        for (unsigned y = 0; y < h; ++y) {
            const uint32_t *row = src + (size_t(k) * layerTexels + size_t(y) * w) * 2u;
            unsigned short *dst = out.data() + size_t(y) * pairW + size_t(k) * w;
            for (unsigned x = 0; x < w; ++x, row += 2) {
                // the surface distance's half: positive and non-zero (no sign, an exponent or mantissa bit set)
                const unsigned dist = row[0] & 0xFFFFu;
                if (!dist || (dist & 0x8000u)) continue;
                // N in 5.3 fixed (bits 0-7), v the rest's 16 bits (8-23): jah_reflect_state.glsl's final layout
                const unsigned n8 = row[1] & 0xFFu, v = (row[1] >> 8u) & 0xFFFFu;
                dst[x] = (unsigned short)std::min(n8 / 8u + v, 65535u);
            }
        }
}

}}}
