/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include "document/scenegraph/skybake.h"

#include <QtGlobal>
#include <algorithm>

namespace iris
{

QImage bakeGradientSky(const QColor &top, const QColor &mid, const QColor &bottom,
                       float offset, int width, int height)
{
    const int W = std::max(1, width);
    const int H = std::max(2, height);
    const float middle = qBound(0.01f, offset, 0.99f);

    QImage strip(W, H, QImage::Format_RGBA8888);
    if (strip.isNull()) return strip;

    for (int r = 0; r < H; ++r) {
        // 1 at the top (zenith) row, 0 at the bottom — the ramp's own axis.
        const float pos = 1.0f - float(r) / float(H - 1);
        float t;
        const QColor *c0, *c1;
        if (pos <= middle) { t = pos / middle;                      c0 = &bottom; c1 = &mid; }
        else               { t = (pos - middle) / (1.0f - middle);  c0 = &mid;    c1 = &top; }

        const auto lerp8 = [t](float a, float b) {
            return (unsigned char)qBound(0.0f, (a + (b - a) * t) * 255.0f, 255.0f);
        };
        const unsigned char rr = lerp8(float(c0->redF()),   float(c1->redF()));
        const unsigned char gg = lerp8(float(c0->greenF()), float(c1->greenF()));
        const unsigned char bb = lerp8(float(c0->blueF()),  float(c1->blueF()));

        unsigned char *line = strip.scanLine(r);
        for (int x = 0; x < W; ++x) {
            unsigned char *p = line + size_t(x) * 4u;
            p[0] = rr; p[1] = gg; p[2] = bb; p[3] = 255;
        }
    }
    return strip;
}

}
