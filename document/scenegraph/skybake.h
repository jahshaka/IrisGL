/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef SKYBAKE_H
#define SKYBAKE_H

#include <QColor>
#include <QImage>

namespace iris
{

/// The legacy gradientsky.frag ramp — a pure vertical 3-stop gradient — baked
/// into a narrow equirect strip. Row 0 is the ZENITH and the last row the
/// nadir, which is the convention every other sky image in the codebase uses
/// (SceneMirror::bakeRealisticSky writes it, integrateSkyAmbientSh reads it).
/// `offset` is where the middle stop sits, 0..1 from the bottom; it is clamped
/// to [0.01, 0.99] so the two segments can never collapse. The result is
/// Format_RGBA8888 (alpha 255), so `constBits()` is a straight RGBA upload.
///
/// THE one implementation (VISUAL_PARITY re-audit F10). The engine mirror's
/// sky path and the glTF/web exporter both need this ramp and each carried its
/// own copy of the interpolation until this function existed — two places to
/// fix when the ramp changes, and nothing enforcing that the exported sky and
/// the sky in the viewport agreed in the first place. It lives in the IrisGL
/// library rather than on SceneMirror because the exporter links the document
/// but not the mirror (which drags in the engine).
QImage bakeGradientSky(const QColor &top, const QColor &mid, const QColor &bottom,
                       float offset, int width = 4, int height = 256);

}

#endif // SKYBAKE_H
