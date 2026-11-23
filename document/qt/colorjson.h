/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef IRIS_CORE_COLORJSON_H
#define IRIS_CORE_COLORJSON_H

// THE ONE COLOUR CODEC (audit D10). A colour in any file this platform writes
// — a scene, a sky definition, a material graph, a baked material's values —
// is `{r, g, b, a}` with each channel a FLOAT in 0..1, the engine's own
// encoding. There used to be two under the same keys: SceneWriter::jsonColor
// wrote 0..255 INTEGERS and graphbaker's colorToJson 0..1 floats, so a reader
// that guessed the other form read black (or white) and said nothing.
// Forward-only: there is no reader of the integer form.

#include <QColor>
#include <QJsonObject>
#include <QtGlobal>

namespace iris
{

/// `c` as {r, g, b, a}, each 0..1. An invalid colour writes as transparent
/// black, which is what QColor reports for it.
inline QJsonObject colorToJson(const QColor &c)
{
    QJsonObject o;
    o.insert(QStringLiteral("r"), c.redF());
    o.insert(QStringLiteral("g"), c.greenF());
    o.insert(QStringLiteral("b"), c.blueF());
    o.insert(QStringLiteral("a"), c.alphaF());
    return o;
}

/// The colour an {r, g, b, a} object holds (channels clamped to 0..1; `a`
/// absent = opaque). An EMPTY object is `absent` — an invalid QColor unless the
/// caller names a default, so a reader can tell "not written" from black.
inline QColor colorFromJson(const QJsonObject &o, const QColor &absent = QColor())
{
    if (o.isEmpty()) return absent;
    const auto channel = [&o](const char *key, double fallback) {
        return qBound(0.0, o.value(QLatin1String(key)).toDouble(fallback), 1.0);
    };
    return QColor::fromRgbF(float(channel("r", 0.0)), float(channel("g", 0.0)),
                            float(channel("b", 0.0)), float(channel("a", 1.0)));
}

}  // namespace iris

#endif  // IRIS_CORE_COLORJSON_H
