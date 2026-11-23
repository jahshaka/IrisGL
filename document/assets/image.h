/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef IRIS_IMAGE_H
#define IRIS_IMAGE_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace iris
{

/// THE DOCUMENT's PIXELS (NOQT-DOCUMENT-1 ruling 4): the pixels the document HOLDS - a cube sky's six faces, a live
/// texture's frame - never a file it reads: the document decodes nothing. A file becomes an Image at Studio's edge
/// (src/services/imagedecode.h); the engine's mirror uploads it as it is.
/// Always 8-bit RGBA, rows top to bottom, tightly packed: `rgba` holds width * height * 4 bytes.
struct Image
{
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;

    /// No pixels, or a buffer that does not match its size.
    bool isNull() const
    {
        return width <= 0 || height <= 0 || rgba.size() != std::size_t(width) * std::size_t(height) * 4u;
    }

    /// One colour everywhere (a live texture is born opaque black).
    static Image filled(int w, int h, std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a)
    {
        Image out;
        if (w <= 0 || h <= 0) return out;
        out.width = w;
        out.height = h;
        out.rgba.resize(std::size_t(w) * std::size_t(h) * 4u);
        for (std::size_t i = 0; i < out.rgba.size(); i += 4) {
            out.rgba[i] = r; out.rgba[i + 1] = g; out.rgba[i + 2] = b; out.rgba[i + 3] = a;
        }
        return out;
    }
};

}

#endif // IRIS_IMAGE_H
