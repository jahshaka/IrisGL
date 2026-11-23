/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef IRIS_SRGBCOLOR_H
#define IRIS_SRGBCOLOR_H

#include <cstdint>

#include "core/color.h"

namespace iris
{

/// A COLOUR A USER PICKED, as the document stores it: sRGB-encoded (core/color.h says why), with an INVALID state ("not
/// set": an outline colour that follows the theme, an importer's absent factor) and an alpha that is coverage, not
/// colour. Qt-free (NOQT-DOCUMENT-1 ruling 3); the editor converts at its edge (document/qt/colour.h).
/// ITS 16-BIT CHANNELS AND ITS ROUNDING ARE QColor's (Rgb spec), BIT FOR BIT, BECAUSE THE STORED BYTES MUST NOT MOVE: it
/// is the stored format's colour until the saved scene's format is rewritten (plan 0.16) - never a design to copy.
/// core.colour_qt_parity proves the equality over every channel value and 100,000 seeded floats.
class SrgbColor
{
public:
    /// Invalid ("not set"): alpha reads 1, the colour 0 - what a default QColor reads.
    constexpr SrgbColor() = default;

    /// 8-bit channels, as a picker shows them. A channel outside 0..255 gives an INVALID colour (QColor's rule).
    constexpr SrgbColor(int r, int g, int b, int a = 255)
        : mValid(unsigned(r) <= 255u && unsigned(g) <= 255u && unsigned(b) <= 255u && unsigned(a) <= 255u),
          mA(mValid ? std::uint16_t(a * 0x0101) : std::uint16_t(0)),
          mR(mValid ? std::uint16_t(r * 0x0101) : std::uint16_t(0)),
          mG(mValid ? std::uint16_t(g * 0x0101) : std::uint16_t(0)),
          mB(mValid ? std::uint16_t(b * 0x0101) : std::uint16_t(0)) {}

    /// 0..1 channels, rounded to 16 bits as QColor::fromRgbF rounds (x * 65535 + 0.5, in float). A channel outside
    /// 0..1 is CLAMPED (every producer in the document clamps first; Qt would keep it as an extended colour); an alpha
    /// outside 0..1 gives an INVALID colour (Qt's rule).
    static SrgbColor fromRgbF(float r, float g, float b, float a = 1.0f)
    {
        if (!(a >= 0.0f && a <= 1.0f)) return SrgbColor();
        return fromRgba64(round16(r), round16(g), round16(b), round16(a));
    }

    /// The 16-bit channels as they are (the editor's edge: QColor::rgba64()).
    static constexpr SrgbColor fromRgba64(std::uint16_t r, std::uint16_t g, std::uint16_t b, std::uint16_t a)
    {
        SrgbColor c;
        c.mValid = true;
        c.mR = r; c.mG = g; c.mB = b; c.mA = a;
        return c;
    }

    constexpr bool isValid() const { return mValid; }

    /// The 8-bit channels a picker shows: QColor's reduction of a 16-bit channel (div 257, rounded).
    constexpr int red() const   { return div257(mR); }
    constexpr int green() const { return div257(mG); }
    constexpr int blue() const  { return div257(mB); }
    constexpr int alpha() const { return div257(mA); }

    /// 0..1 channels (16-bit / 65535, in float: QColor's).
    constexpr float redF() const   { return mR / 65535.0f; }
    constexpr float greenF() const { return mG / 65535.0f; }
    constexpr float blueF() const  { return mB / 65535.0f; }
    constexpr float alphaF() const { return mA / 65535.0f; }

    constexpr std::uint16_t red16() const   { return mR; }
    constexpr std::uint16_t green16() const { return mG; }
    constexpr std::uint16_t blue16() const  { return mB; }
    constexpr std::uint16_t alpha16() const { return mA; }

    friend constexpr bool operator==(const SrgbColor &x, const SrgbColor &y)
    {
        return x.mValid == y.mValid && x.mA == y.mA && x.mR == y.mR && x.mG == y.mG && x.mB == y.mB;
    }
    friend constexpr bool operator!=(const SrgbColor &x, const SrgbColor &y) { return !(x == y); }

private:
    static constexpr int div257(int x) { return (x + 0x80 - ((x + 0x80) >> 8)) >> 8; }
    static std::uint16_t round16(float x)
    {
        x = x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
        return std::uint16_t(int(x * 65535.0f + 0.5f));
    }

    bool mValid = false;
    std::uint16_t mA = 0xffff;
    std::uint16_t mR = 0;
    std::uint16_t mG = 0;
    std::uint16_t mB = 0;
};

/// The three linear components of a picked colour (core/color.h's one rule: the sRGB EOTF on the 8-bit value the
/// picker showed). Alpha is not a colour and is never decoded.
inline LinearColor linearOf(const SrgbColor &c)
{
    const float *lut = srgbToLinearTable();
    LinearColor out;
    out.r = lut[c.red()];
    out.g = lut[c.green()];
    out.b = lut[c.blue()];
    return out;
}

/// A LINEAR RGB triple (an importer's glTF factor, an assimp colour) as the sRGB colour the document stores
/// (core/color.h's srgbOfChannel on each channel, then fromRgbF's 16-bit store). Alpha is not a colour: clamped to
/// 0..1 and passed through.
inline SrgbColor srgbOf(float r, float g, float b, float a = 1.0f)
{
    const float upper = 1.0f < a ? 1.0f : a;            // qBound(0, a, 1)'s exact form: a NaN reads 0
    const float alpha = 0.0f < upper ? upper : 0.0f;
    return SrgbColor::fromRgbF(srgbOfChannel(r), srgbOfChannel(g), srgbOfChannel(b), alpha);
}

}

#endif // IRIS_SRGBCOLOR_H
