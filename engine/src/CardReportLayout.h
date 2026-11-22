#pragma once
// THE REPORT OF JahCardIndirect_cs.glsl (its caReport buffer, ogre_U3): ONE layout for its two users - the surface cache's
// card listings (OgreSurfaceCache.cpp, mCardReport) and the irradiance field's probe marks (PhotonIrradianceField.cpp,
// mMarkReport, the shader's jah_field_mark variant). Word 0 the count, then `capacity` listings, then the FRAME WORDS the
// shader writes past the capacity (caMode.z): the frame's largest reported move in codes and a non-finite move seen -
// mode 1 clears both, mode 4 folds the relight's into them.
// GATHER-HANG-1 stage 2: the field sized its report 1 + probes, two words short of the shader's layout, so mode 1's
// clear stored two words past it into the next block of Ogre's pool (GPU-assisted validation: 32,772 bytes bound, a
// write at byte 32,779, at 8,192 probes). source.card_report_layout holds the shader's highest frame word against
// kCardReportFrameWords and both creates on cardReportWords.
#include <cstdint>

namespace jahshaka::engine::detail {

/// the words the shader writes past the capacity: caReport[1 + capacity] and caReport[2 + capacity]
constexpr uint32_t kCardReportFrameWords = 2u;

/// the report's words for `capacity` listings: the count, the listings, the frame words
constexpr uint32_t cardReportWords(uint32_t capacity) { return 1u + capacity + kCardReportFrameWords; }

}   // namespace jahshaka::engine::detail
