#pragma once
// THE ENGINE's PICTURES TYPES - engine-neutral values, one subject (ENGINE-HEADERS-1: split from Types.h by
// declaration name). NOTHING here may reference Ogre, Qt or GL; a new subject is a new header, never an append here.
#include <cstddef>
#include <string>
#include <vector>
#include "jahshaka/engine/Math.h"

namespace jahshaka { namespace engine {


/// A CPU-side RGBA8 image, used to read back an offscreen View.
struct Image {
    unsigned width = 0, height = 0;
    std::vector<unsigned char> rgba;   // width*height*4, row-major, top-left origin
    /// Pixel accessor; returns {0,0,0,0} if out of range.
    Colour at(unsigned x, unsigned y) const {
        if (x >= width || y >= height) return Colour(0, 0, 0, 0);
        const size_t i = (static_cast<size_t>(y) * width + x) * 4u;
        return Colour(rgba[i] / 255.0f, rgba[i+1] / 255.0f, rgba[i+2] / 255.0f, rgba[i+3] / 255.0f);
    }
};

/// ONE VIDEO FRAME, as an encoder takes it (VIDEO-REC-1, View::setVideoReadback).
///
/// NV12: the luma plane (`width` x `height` bytes, row-major, top-left origin)
/// followed by ONE interleaved chroma plane (`height / 2` rows of `width` bytes:
/// Cb, Cr, Cb, Cr ... — one pair per 2x2 block of pixels). 1.5 bytes a pixel.
///
/// THE COLOUR, stated once: ITU-R BT.709 coefficients (Kr 0.2126, Kb 0.0722) on
/// the view's DISPLAY-ENCODED picture — the very bytes readPixels returns, the
/// sRGB-encoded result of the view's chain — in LIMITED ("video") range: Y in
/// [16, 235], Cb/Cr in [16, 240] about 128. The chroma of a block is the mean of
/// its four pixels' chroma (centre-sited). The picture is encoded ONCE (by the
/// chain); this is a matrix on those codes, never a second transfer curve.
///
/// `tag` is the host's own stamp, handed to armVideoFrame for the frame this
/// picture is of (the recorder's simulated step).
struct VideoFrameNv12 {
    unsigned width = 0, height = 0;
    unsigned long long tag = 0;
    std::vector<unsigned char> nv12;   // width*height*3/2
    const unsigned char *luma() const { return nv12.data(); }
    const unsigned char *chroma() const { return nv12.data() + size_t(width) * height; }
};

/// What a view's video readback is doing (View::videoReadbackStatus).
struct VideoReadbackStatus {
    bool on = false;
    unsigned width = 0, height = 0;
    /// Ring slots holding a frame not yet taken (recorded or ready); of `ringSize`.
    unsigned pending = 0, ringSize = 0;
    /// Frames converted and copied (an armed frame the view rendered); frames
    /// handed back through takeVideoFrame; and armed frames that found every
    /// slot still pending and were therefore NOT read back (the taker is behind).
    unsigned long long recorded = 0, delivered = 0, dropped = 0;
    std::string error;
};

/// A view's SCENE RADIANCE, read back in float (HDR-READBACK-1, PHOTON P3).
///
/// `Image` is the DISPLAY: eight bits a channel, clipped at 1.0, after whatever
/// grade the view's chain applies. A suite that compares a pixel against a
/// closed form in physical units (a mirror showing an emitter of radiance 3.0,
/// a floor lit by a rectangle, a card's texel) cannot use it — every value above
/// 1.0 reads 1.0, and every value below carries half a code of quantisation
/// (1.75-2 % at the card bars' magnitudes). This is the same pixels one step
/// earlier: the linear, scene-referred value the scene passes wrote, BEFORE the
/// tonemap, the exposure, the bloom composite and the 8-bit store. Filled by
/// `View::readPixelsHdr` on a view that asked for it (PostFxDesc::hdrReadback).
struct ImageF {
    unsigned width = 0, height = 0;
    std::vector<float> rgba;   // width*height*4, row-major, top-left origin, linear radiance
    /// Pixel accessor; returns {0,0,0,0} if out of range.
    Colour at(unsigned x, unsigned y) const {
        if (x >= width || y >= height) return Colour(0, 0, 0, 0);
        const size_t i = (static_cast<size_t>(y) * width + x) * 4u;
        return Colour(rgba[i], rgba[i+1], rgba[i+2], rgba[i+3]);
    }
};

}}  // namespace jahshaka::engine
