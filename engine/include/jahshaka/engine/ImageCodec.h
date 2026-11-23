#pragma once
// THE ENGINE's IMAGE CODEC - a held picture made small and encoded for a viewer (MCP-ON-HOST-1 stage 3). Engine-neutral:
// NOTHING here may reference Ogre, Qt or GL (the boundary law); the encoder is the engine's (Ogre's Root registers the
// image codecs, the NULL engine's included), so encodePng needs a live engine in the process.
// `Image` is FORWARD-DECLARED (Pictures.h holds it): an include here would widen Pictures.h's fan-out.
#include <cstdint>
#include <string>
#include <vector>

namespace jahshaka { namespace engine {

struct Image;

/// THE BOX DOWNSCALE: s = ceil(longest edge / maxEdge) (1 when the image already fits: an exact copy, never an upscale);
/// the output is ceil(width / s) x ceil(height / s); each output pixel, per channel, the mean of the source pixels of its
/// s x s block that exist (an edge block counts only its own), rounded half up ((sum + n / 2) / n in integers). The codes
/// are averaged as they are (display-encoded, no linearisation: a preview, never a measurement). maxEdge 0 is taken as 1.
Image boxDownscale(const Image &in, unsigned maxEdge);

/// The image as PNG bytes (8 bits a channel, RGBA, its alpha kept - an all-opaque image may be written as RGB, colour type 2:
/// its alpha is 255 by the PNG's own rule) into `out`; false with `why` when the engine has no PNG codec, the encode threw,
/// the codec wrote no complete PNG (pngComplete), or the image is empty (a zero width or height) - nothing is thrown.
bool encodePng(const Image &image, std::vector<std::uint8_t> &out, std::string &why);

/// True when `png` is framed as a whole PNG: the 8-byte signature first AND the 12-byte IEND chunk last (a failed save can
/// leave the signature and IHDR alone in the stream: Ogre's FreeImage codec ignores the save's answer).
bool pngComplete(const std::vector<std::uint8_t> &png);

}}  // namespace jahshaka::engine
