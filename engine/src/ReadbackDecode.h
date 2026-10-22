// THE INSTRUMENTS' READBACK DECODES (PHOTON-2 round 2, the hitch after the stop): the per-pixel loops that turn a
// retired GPU copy into the host vectors the stillness door reads - the gather's history count and class, the
// reflection's history count. They run on the render thread inside a frame, and at -O0 a 1080p decode alone took
// ~15 ms: Bistro's frame 3 after a stop ran 41 ms wall on 26 ms of GPU (the door had asked for the stop frame's
// history count; its copy retires three frames later). ReadbackDecode.cpp is compiled at -O2 in Debug (the
// stillness door's own precedent): integer work no optimisation level changes the answer of.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace jahshaka { namespace engine { namespace detail {

/// The gather's packed history (rg32ui, `layers` eye layers of w x h, layer after layer): per texel the second word's
/// count (bits 22-25) and class (bit 21), the eye layers side by side (w x layers wide).
void decodeGatherHistory(const uint32_t *src, unsigned w, unsigned h, unsigned layers,
                         std::vector<unsigned char> &count, std::vector<unsigned char> &cls);
/// The reflection's history (rgba16f): per texel its w channel as a whole count (0 = no count; <= 4096), the eye
/// layers side by side.
void decodeReflectHistory(const uint16_t *src, unsigned w, unsigned h, unsigned layers,
                          std::vector<unsigned short> &out);

}}}
