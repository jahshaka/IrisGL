/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef IRIS_QT_COLOUR_H
#define IRIS_QT_COLOUR_H

// THE COLOUR's QT EDGE (NOQT-DOCUMENT-1 stage 3): iris::SrgbColor <-> QColor, bit for bit for a valid colour (the
// 16-bit channels pass through as they are); every INVALID colour crosses as THE invalid colour (QColor() /
// SrgbColor(), alpha 1 - an out-of-range QColor(-1, ...) keeps an alpha of 0 that is not carried). The editor's
// widgets, its property rows and its file codecs speak
// QColor; the document stores SrgbColor. A QColor outside 0..1 (Qt's ExtendedRgb) arrives through QColor::rgba64(),
// Qt's own clamp. This folder is the document's Qt adapter: it goes at NOQT-DOCUMENT-1's last stage.

#include <QColor>
#include <QRgba64>

#include "document/srgbcolor.h"

namespace iris
{

inline QColor toQColor(const SrgbColor &c)
{
    if (!c.isValid()) return QColor();
    return QColor::fromRgba64(c.red16(), c.green16(), c.blue16(), c.alpha16());
}

inline SrgbColor toSrgbColor(const QColor &c)
{
    if (!c.isValid()) return SrgbColor();
    const QRgba64 v = c.rgba64();
    return SrgbColor::fromRgba64(v.red(), v.green(), v.blue(), v.alpha());
}

}

#endif // IRIS_QT_COLOUR_H
