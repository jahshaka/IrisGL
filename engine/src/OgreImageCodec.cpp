// THE ENGINE's IMAGE CODEC (MCP-ON-HOST-1 stage 3): jahshaka/engine/ImageCodec.h says the laws. The downscale is plain
// C++; the PNG is Ogre's image codec (registered by Root's constructor, OgreMain/src/OgreRoot.cpp: the NULL engine's too).
#include "jahshaka/engine/ImageCodec.h"
#include "jahshaka/engine/Pictures.h"

#include <OgreDataStream.h>
#include <OgreException.h>
#include <OgreImage2.h>

#include <algorithm>
#include <exception>

namespace jahshaka { namespace engine {

Image boxDownscale(const Image &in, unsigned maxEdge)
{
    const unsigned edge = std::max(maxEdge, 1u);
    const unsigned longest = std::max(in.width, in.height);
    const unsigned s = longest > edge ? (longest + edge - 1) / edge : 1u;
    if (s == 1) return in;   // it fits: an exact copy, never an upscale
    Image out;
    out.width = (in.width + s - 1) / s;
    out.height = (in.height + s - 1) / s;
    out.rgba.resize(std::size_t(out.width) * out.height * 4);
    for (unsigned oy = 0; oy < out.height; ++oy)
        for (unsigned ox = 0; ox < out.width; ++ox) {
            // the block's source pixels that exist (an edge block counts only its own)
            const unsigned x0 = ox * s, y0 = oy * s, x1 = std::min(x0 + s, in.width), y1 = std::min(y0 + s, in.height);
            const std::uint64_t n = std::uint64_t(x1 - x0) * (y1 - y0);
            for (unsigned c = 0; c < 4; ++c) {
                std::uint64_t sum = 0;
                for (unsigned y = y0; y < y1; ++y)
                    for (unsigned x = x0; x < x1; ++x) sum += in.rgba[(std::size_t(y) * in.width + x) * 4 + c];
                out.rgba[(std::size_t(oy) * out.width + ox) * 4 + c] = (unsigned char)((sum + n / 2) / n);   // half up
            }
        }
    return out;
}

bool encodePng(const Image &image, std::vector<std::uint8_t> &out, std::string &why)
{
    out.clear();
    if (image.width == 0 || image.height == 0) {
        why = "an empty picture (" + std::to_string(image.width) + " x " + std::to_string(image.height) + ") has no PNG";
        return false;
    }
    // Ogre's door takes a mutable pointer: the rows are copied (top-left origin, RGBA8: the codec flips them for its own
    // bottom-up bitmap and orders the channels itself)
    std::vector<std::uint8_t> rows(image.rgba.begin(), image.rgba.end());
    try {
        Ogre::Image2 img;
        img.loadDynamicImage(rows.data(), image.width, image.height, 1u, Ogre::TextureTypes::Type2D,
                             Ogre::PFG_RGBA8_UNORM, false, 1u);
        Ogre::DataStreamPtr png = img.encode("png", 0, 1);
        out.resize(png ? png->size() : 0);
        if (!out.empty()) out.resize(png->read(out.data(), out.size()));
    } catch (const Ogre::Exception &e) {
        out.clear();
        why = "no PNG codec on this engine: " + e.getDescription();
        return false;
    } catch (const std::exception &e) {
        out.clear();
        why = std::string("no PNG codec on this engine: ") + e.what();
        return false;
    }
    // OGRE's DEFECT (DOCS/OGRE_UPSTREAM_ISSUES.md): FreeImageCodec2::encode ignores FreeImage_SaveToMemory's answer, so a
    // failed save is an EMPTY stream, never an exception - the bytes are checked for the PNG signature
    static const std::uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (out.size() < 8 || !std::equal(kSignature, kSignature + 8, out.begin())) {
        why = "the codec wrote no PNG (" + std::to_string(out.size()) + " bytes without the PNG signature)";
        out.clear();
        return false;
    }
    return true;
}

}}  // namespace jahshaka::engine
