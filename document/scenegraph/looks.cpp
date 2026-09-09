/**************************************************************************
This file is part of JahshakaVR, VR Authoring Toolkit
http://www.jahshaka.com
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include "document/scenegraph/looks.h"

#include <algorithm>

namespace iris
{

namespace
{

// THE TABLE. One row per look, one entry per parameter, and the parameter ORDER
// is the wire order into the engine's LookDesc::p — moving a row here moves a
// float in a shader, so the order is part of the contract and not cosmetic.
//
// Every look's first parameter is `amount`, always in 0..1, always an exact
// identity at 0 (POST_LOOKS_SPEC §4.1). The rest are the constants upstream's
// sample shaders hard-code, which is the whole reason this program authors its
// own GLSL (§3 decision D2).

const LookParamDef kDesaturateParams[] = {
    { "amount", 1.0f, 0.0f, 1.0f },
};

const LookParamDef kGlassWarpParams[] = {
    { "amount", 0.5f, 0.0f, 1.0f },
    // The ripple frequency, in cycles across the frame. Below 1 the whole image
    // leans; above ~24 the ripple is finer than the resampling can carry and it
    // reads as noise.
    { "scale",  4.0f, 0.5f, 32.0f },
};

const LookParamDef kRadialBlurParams[] = {
    { "amount",  0.5f, 0.0f, 1.0f },
    // The centre of the zoom, in UV. Outside 0..1 is legal and useful: a centre
    // off the edge of the frame streaks everything one way.
    { "centerX", 0.5f, -1.0f, 2.0f },
    { "centerY", 0.5f, -1.0f, 2.0f },
    // How fast the blur comes on with distance from the centre. 1 is linear;
    // above 2 the middle of the frame stays sharp much further out.
    { "falloff", 1.0f, 0.1f, 8.0f },
};

const LookParamDef kOldMovieParams[] = {
    { "amount",  1.0f, 0.0f, 1.0f },
    { "flicker", 0.5f, 0.0f, 1.0f },
    { "dirt",    0.5f, 0.0f, 1.0f },
    { "jitter",  0.5f, 0.0f, 1.0f },
};

const LookParamDef kPosterizeParams[] = {
    { "amount", 1.0f, 0.0f, 1.0f },
    // Upstream's eight, as a setting. Two is a hard duotone; above ~32 the
    // banding is below what an 8-bit frame can show anyway.
    { "levels", 8.0f, 2.0f, 64.0f },
    // Where the bands fall. Below 1 they crowd into the shadows (upstream's
    // 0.6, and the reason its posterize looks right rather than blocky).
    { "gamma",  0.6f, 0.1f, 3.0f },
};

const LookParamDef kSharpenParams[] = {
    { "amount", 0.5f, 0.0f, 1.0f },
};

const LookParamDef kFilmGradeParams[] = {
    { "amount",     1.0f, 0.0f, 1.0f },
    // 1 is neutral for both, which is why the grade at amount 1 with untouched
    // parameters is still (very nearly) the original picture: the look is the
    // FOUR knobs, not a preset.
    { "saturation", 1.0f, 0.0f, 2.0f },
    { "contrast",   1.0f, 0.0f, 2.0f },
    { "vignette",   0.0f, 0.0f, 1.0f },
    { "tintR",      1.0f, 0.0f, 2.0f },
    { "tintG",      1.0f, 0.0f, 2.0f },
    { "tintB",      1.0f, 0.0f, 2.0f },
};

const LookDef kCatalogue[] = {
    { "desaturate", LookKind::Desaturate, kDesaturateParams, 1 },
    { "glassWarp",  LookKind::GlassWarp,  kGlassWarpParams,  2 },
    { "radialBlur", LookKind::RadialBlur, kRadialBlurParams, 4 },
    { "oldMovie",   LookKind::OldMovie,   kOldMovieParams,   4 },
    { "posterize",  LookKind::Posterize,  kPosterizeParams,  3 },
    { "sharpen",    LookKind::Sharpen,    kSharpenParams,    1 },
    { "filmGrade",  LookKind::FilmGrade,  kFilmGradeParams,  7 },
};

constexpr int kCatalogueCount = int(sizeof(kCatalogue) / sizeof(kCatalogue[0]));

float clampParam(const LookParamDef &p, double v)
{
    if (!(v == v)) return p.defaultValue;   // NaN -> the default, never propagated
    return float(std::min(double(p.maxValue), std::max(double(p.minValue), v)));
}

}   // namespace

const LookDef *lookCatalogue(int &count)
{
    count = kCatalogueCount;
    return kCatalogue;
}

const LookDef *lookDef(const QString &id)
{
    for (const LookDef &d : kCatalogue)
        if (id == QLatin1String(d.id)) return &d;
    return nullptr;
}

void lookParamValues(const LookDef &def, const QJsonObject &entry, float out[8])
{
    for (int i = 0; i < 8; ++i) out[i] = 0.0f;
    const QJsonObject params = entry.value(QStringLiteral("params")).toObject();
    for (int i = 0; i < def.paramCount && i < 8; ++i) {
        const LookParamDef &p = def.params[i];
        const QJsonValue v = params.value(QLatin1String(p.id));
        out[i] = v.isDouble() ? clampParam(p, v.toDouble()) : p.defaultValue;
    }
}

QJsonObject makeLookEntry(const LookDef &def, const QJsonObject &params, bool enabled)
{
    QJsonObject out;
    out.insert(QStringLiteral("id"), QLatin1String(def.id));
    out.insert(QStringLiteral("enabled"), enabled);
    QJsonObject p;
    for (int i = 0; i < def.paramCount; ++i) {
        const LookParamDef &d = def.params[i];
        const QJsonValue v = params.value(QLatin1String(d.id));
        p.insert(QLatin1String(d.id), double(v.isDouble() ? clampParam(d, v.toDouble())
                                                          : d.defaultValue));
    }
    out.insert(QStringLiteral("params"), p);
    return out;
}

QJsonArray normalizeLookStack(const QJsonArray &in)
{
    QJsonArray out;
    QStringList seen;
    for (const QJsonValue &v : in) {
        if (!v.isObject()) continue;
        const QJsonObject entry = v.toObject();
        const QString id = entry.value(QStringLiteral("id")).toString();
        const LookDef *def = lookDef(id);
        if (!def) continue;              // a look this build does not implement
        if (seen.contains(id)) continue; // ONE INSTANCE PER LOOK (§7 R2)
        seen.append(id);
        out.append(makeLookEntry(*def, entry.value(QStringLiteral("params")).toObject(),
                                 entry.value(QStringLiteral("enabled")).toBool(true)));
    }
    return out;
}

}   // namespace iris
