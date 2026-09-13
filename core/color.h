/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef IRIS_CORE_COLOR_H
#define IRIS_CORE_COLOR_H

// THE ONE COLOUR-SPACE RULE (SKY_LIGHT_SPEC.md §4, owner decision §188e,
// pick 3 §193a).
//
// An 8-bit QColor a user picked in a colour dialog is an sRGB value — the same
// encoding an 8-bit albedo TEXTURE carries. Textures have always been bound
// sRGB and decoded by the sampler; flat colours went to the renderer RAW, so a
// "50% grey" material rendered 2.33x brighter than a 50% grey texture of the
// same colour, and a 72-grey colour sky radiated 0.282 while the same 72 grey
// painted into an equirect radiated 0.065. Two meanings for one picker.
//
// `linearOf` is that decode, and the rule is that EVERY colour a user picks
// passes through it on its way into the renderer: material albedo/emissive/
// specular/fresnel, the sky (colour, gradient stops), fog, the view background,
// light colours including the Sky Light's tint, and particle colours. UI
// colours — wires, gizmos, outlines, icons — are NOT radiance and never go
// through it.
//
// The curve is the IEC 61966-2-1 sRGB EOTF, identical to the one the SH sky
// integral already applies per texel (the table that used to live in
// scenemirror.cpp) — which is exactly why a painted sky and a picked sky now
// agree.
//
// 0 and 255 are fixed points (0.0 and 1.0), so black and white never move: the
// look change is entirely in the midtones, by design.

#include <QColor>
#include <cmath>

namespace iris
{

/// The sRGB -> linear table for 8-bit channel values. One shared table; the
/// `float` return is the linear radiance multiplier in [0, 1].
inline const float *srgbToLinearTable()
{
    static float t[256];
    static const bool once = [] {
        for (int i = 0; i < 256; ++i) {
            const float c = i / 255.0f;
            t[i] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }
        return true;
    }();
    (void)once;
    return t;
}

/// sRGB -> linear for one already-normalised channel (the analytic curve; used
/// where the source is not an 8-bit integer, e.g. a float colour component).
inline float linearOfChannel(float c)
{
    if (c <= 0.0f) return 0.0f;
    if (c >= 1.0f) return 1.0f;
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

/// The three linear components of a user-picked colour. Alpha is NOT a colour
/// and is never decoded — it is a coverage/blend weight and stays linear.
struct LinearColor {
    float r = 0.0f, g = 0.0f, b = 0.0f;
};

inline LinearColor linearOf(const QColor &c)
{
    const float *lut = srgbToLinearTable();
    // QColor's 8-bit accessors are the values the picker showed the user.
    LinearColor out;
    out.r = lut[qBound(0, c.red(), 255)];
    out.g = lut[qBound(0, c.green(), 255)];
    out.b = lut[qBound(0, c.blue(), 255)];
    return out;
}

/// THE INVERSE, and the reason it exists: `linearOf` above is a DECODE applied
/// to every colour on its way into the renderer, so the document's invariant is
/// "a QColor is sRGB". An IMPORTER reading a format that specifies LINEAR
/// values — glTF's baseColorFactor, specularColorFactor and emissiveFactor are
/// all linear by spec, and assimp hands COLOR_DIFFUSE through unchanged — has
/// a linear number in its hand and must ENCODE it before it becomes a QColor,
/// or the renderer decodes it a second time and every flat-coloured import
/// darkens by a gamma (round-2 review item 2).
///
/// 0 and 1 are fixed points, as they are for linearOf.
inline float srgbOfChannel(float linear)
{
    if (linear <= 0.0f) return 0.0f;
    if (linear >= 1.0f) return 1.0f;
    return linear <= 0.0031308f ? linear * 12.92f
                                : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
}

/// A linear RGB triple as the sRGB QColor the document stores. Alpha is not a
/// colour and passes through.
inline QColor srgbOf(float r, float g, float b, float a = 1.0f)
{
    return QColor::fromRgbF(qreal(srgbOfChannel(r)), qreal(srgbOfChannel(g)),
                            qreal(srgbOfChannel(b)), qreal(qBound(0.0f, a, 1.0f)));
}

}  // namespace iris

#endif  // IRIS_CORE_COLOR_H
