/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include "document/scenegraph/cameralens.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace iris
{
namespace lens
{

namespace {

constexpr double kPi = 3.14159265358979323846;
/// One photographic stop on the post chain's natural-log exposure axis.
constexpr double kLn2 = 0.69314718055994530942;

inline double deg2rad(double d) { return d * kPi / 180.0; }
inline double rad2deg(double r) { return r * 180.0 / kPi; }

/// The angle range a projection can express. 0 and 180 are both degenerate
/// (tan blows up at 90 degrees), so every conversion clamps into the open
/// interval rather than returning an infinity somebody has to check for.
inline double clampFovDeg(double d) { return std::min(std::max(d, 0.0001), 179.9); }

}   // namespace

FitAxis fitAxis(CameraSensorFit fit, float aspect)
{
    switch (fit) {
    case CameraSensorFit::Horizontal: return FitAxis::Horizontal;
    case CameraSensorFit::Vertical:   return FitAxis::Vertical;
    case CameraSensorFit::Auto: break;
    }
    // Blender's rule: the sensor WIDTH covers the larger image axis. A square
    // frame is landscape by convention (aspect == 1 takes the horizontal
    // branch) so the two sides of the comparison never disagree at the seam.
    return (aspect >= 1.0f) ? FitAxis::Horizontal : FitAxis::Vertical;
}

float bindingSensorMm(const Filmback &fb)
{
    const float squeeze = fb.squeeze > 0.0f ? fb.squeeze : 1.0f;
    const FitAxis axis = fitAxis(fb.fit, fb.aspect);
    if (axis == FitAxis::Horizontal)
        return std::max(0.0001f, fb.sensorWidth * squeeze);
    // Vertical. AUTO on a portrait frame binds the sensor WIDTH through the
    // vertical axis (Blender), and takes NO squeeze there: an anamorphic
    // element compresses the horizontal axis, so applying it to a vertical
    // binding would be inventing physics.
    if (fb.fit == CameraSensorFit::Auto)
        return std::max(0.0001f, fb.sensorWidth);
    return std::max(0.0001f, fb.sensorHeight);
}

float verticalFovDegFromFocal(const Filmback &fb, float focalMm)
{
    if (focalMm <= 0.0f) return 0.0f;
    const double sensor = bindingSensorMm(fb);
    const double axisFov = 2.0 * std::atan(sensor / (2.0 * double(focalMm)));
    if (fitAxis(fb.fit, fb.aspect) == FitAxis::Vertical)
        return float(rad2deg(axisFov));
    // Horizontal binding: cross to the vertical angle through the aspect.
    return verticalFovDegFromHorizontal(float(rad2deg(axisFov)), fb.aspect);
}

float focalFromVerticalFovDeg(const Filmback &fb, float verticalFovDeg)
{
    if (verticalFovDeg <= 0.0f || verticalFovDeg >= 180.0f) return 0.0f;
    const double sensor = bindingSensorMm(fb);
    double axisFovDeg = verticalFovDeg;
    if (fitAxis(fb.fit, fb.aspect) == FitAxis::Horizontal)
        axisFovDeg = horizontalFovDeg(verticalFovDeg, fb.aspect);
    const double t = std::tan(deg2rad(clampFovDeg(axisFovDeg)) * 0.5);
    if (t <= 0.0) return 0.0f;
    return float(sensor / (2.0 * t));
}

float horizontalFovDeg(float verticalFovDeg, float aspect)
{
    const double a = aspect > 0.0f ? double(aspect) : 1.0;
    const double t = std::tan(deg2rad(clampFovDeg(verticalFovDeg)) * 0.5) * a;
    return float(rad2deg(2.0 * std::atan(t)));
}

float verticalFovDegFromHorizontal(float horizontalFov, float aspect)
{
    const double a = aspect > 0.0f ? double(aspect) : 1.0;
    const double t = std::tan(deg2rad(clampFovDeg(horizontalFov)) * 0.5) / a;
    return float(rad2deg(2.0 * std::atan(t)));
}

float verticalFovDegForFramingAspect(float verticalFovDeg, float aspect, float framingAspect)
{
    // Every degenerate input is the identity — "no hold" must cost nothing and
    // must not perturb a single bit of the projection it does not apply to.
    if (!(framingAspect > 0.0f) || !(aspect > 0.0f) || !(verticalFovDeg > 0.0f))
        return verticalFovDeg;
    // AT OR BELOW the framing aspect nothing happens, and the test is an exact
    // float comparison of two aspects rather than a round trip through degrees:
    // the common case must be the identity by CONSTRUCTION, not by tolerance.
    if (aspect <= framingAspect)
        return verticalFovDeg;      // inside the hold: untouched, bit for bit
    // Hold the horizontal extent the shot has at `framingAspect`, and solve for
    // the vertical angle that produces it on this wider frame.
    return std::max(1.0f, verticalFovDegFromHorizontal(
                              horizontalFovDeg(verticalFovDeg, framingAspect), aspect));
}

float diagonalFovDeg(float verticalFovDeg, float aspect)
{
    const double a = aspect > 0.0f ? double(aspect) : 1.0;
    const double tv = std::tan(deg2rad(clampFovDeg(verticalFovDeg)) * 0.5);
    const double th = tv * a;
    return float(rad2deg(2.0 * std::atan(std::sqrt(tv * tv + th * th))));
}

float sensorDiagonalMm(float sensorWidth, float sensorHeight)
{
    const double w = std::max(0.0f, sensorWidth), h = std::max(0.0f, sensorHeight);
    return float(std::sqrt(w * w + h * h));
}

float circleOfConfusionMm(float sensorWidth, float sensorHeight)
{
    // The Zeiss criterion: the diagonal over 1500. Full frame (36x24) gives
    // 43.267 / 1500 = 0.02884 mm, which is the value the published 35 mm
    // depth-of-field tables are computed from — so our numbers can be checked
    // against a photographer's chart and not only against our own arithmetic.
    const float d = sensorDiagonalMm(sensorWidth, sensorHeight);
    return d > 0.0f ? d / 1500.0f : 0.0f;
}

FocusInfo focusInfo(float focalMm, float fStop, float focusDistanceMetres, float cocMm)
{
    FocusInfo info;
    const double f = double(focalMm);
    const double N = double(fStop);
    const double c = double(cocMm);
    const double s = double(focusDistanceMetres) * 1000.0;   // mm

    info.focusDistance = double(focusDistanceMetres);
    info.cocLimit = c;

    if (f <= 0.0 || N <= 0.0 || c <= 0.0) {
        // A lens with no length, no aperture or no blur criterion has no depth
        // of field to speak of: report zeroes rather than dividing by them.
        return info;
    }

    const double H = (f * f) / (N * c) + f;    // mm
    info.hyperfocal = H / 1000.0;

    if (s <= 0.0) return info;   // no subject to focus on

    const double denomNear = H + s - 2.0 * f;
    info.nearLimit = denomNear > 0.0 ? (s * (H - f) / denomNear) / 1000.0 : 0.0;

    const double denomFar = H - s;
    // At (and past) the hyperfocal distance the far limit is infinity — with
    // THIS pair of formulas that is exactly true rather than nearly true, which
    // is the whole reason to use them: focusing at H gives [H/2, infinity), the
    // textbook result, and a test can assert it as an identity.
    info.farLimit = denomFar > 0.0 ? (s * (H - f) / denomFar) / 1000.0
                                   : std::numeric_limits<double>::infinity();
    return info;
}

// ---- lens shift -----------------------------------------------------------

float halfExtentAtNear(float fovDegOnAxis, float nearDist)
{
    return float(std::tan(deg2rad(clampFovDeg(fovDegOnAxis)) * 0.5) * double(nearDist));
}

float nearOffsetFromShift(float shiftFraction, float halfExtent)
{
    // The frame spans 2 * halfExtent, so a shift of 1.0 slides it a whole frame.
    return shiftFraction * 2.0f * halfExtent;
}

float shiftFromNearOffset(float nearOffset, float halfExtent)
{
    const float span = 2.0f * halfExtent;
    return span != 0.0f ? nearOffset / span : 0.0f;
}

float ogreFrustumOffset(float shiftFraction, float halfExtent, float nearDist,
                        float stereoFocalLength)
{
    // Ogre computes nearOffset = frustumOffset * (near / stereoFocalLength)
    // (OgreFrustum.cpp:370-371), so invert exactly that. The default stereo
    // focal length is 1.0, which makes `near` cancel — the multiplication is
    // still written out because the cancellation is a property of the default,
    // and a future stereo rig setting it would silently break a simplified form.
    const float focal = stereoFocalLength > 0.0f ? stereoFocalLength : 1.0f;
    const float scale = nearDist > 0.0f ? (nearDist / focal) : 1.0f;
    return nearOffsetFromShift(shiftFraction, halfExtent) / scale;
}

float shiftFromOgreFrustumOffset(float frustumOffset, float halfExtent, float nearDist,
                                 float stereoFocalLength)
{
    const float focal = stereoFocalLength > 0.0f ? stereoFocalLength : 1.0f;
    const float scale = nearDist > 0.0f ? (nearDist / focal) : 1.0f;
    return shiftFromNearOffset(frustumOffset * scale, halfExtent);
}

// ---- exposure (CAMERA_LENS_SPEC §4) ---------------------------------------

float exposureStopsToChain(float stops)
{
    // One stop is a doubling, and the chain's axis is natural-log, so a stop is
    // ln 2 of it. Anchored so that zero stops is the default world grade.
    return kExposureAnchorChain + stops * float(kLn2);
}

float exposureChainToStops(float chain)
{
    return (chain - kExposureAnchorChain) / float(kLn2);
}

float manualExposureClamp()
{
    // 7.5 - ln(1024 * 0.18). Derived, not typed: see the header for why this
    // exact number is what makes manual exposure agree with the deterministic
    // tonemap constant e^(E-2)/0.18.
    return 7.5f - float(std::log(1024.0 * 0.18));
}

float exposureMultiplier(float chainExposure)
{
    return float(std::exp(double(chainExposure) - 2.0) / 0.18);
}

float smoothTowards(float current, float target, float speed, float dt)
{
    if (speed <= 0.0f || dt <= 0.0f) return target;
    const double k = std::exp(-double(speed) * double(dt));
    return float(double(target) + (double(current) - double(target)) * k);
}

// ---- the tables -----------------------------------------------------------

namespace {

// FILMBACKS. Name, width, height, squeeze. Sorted small to large so a UI list
// reads like a sensor-size chart.
const FilmbackPreset kFilmbacks[] = {
    { "Super 16",              12.52f,  7.41f, 1.0f },
    { "Micro Four Thirds",     17.30f, 13.00f, 1.0f },
    { "Super 35",              24.89f, 18.66f, 1.0f },
    { "16:9 Digital Film",     23.76f, 13.365f, 1.0f },
    { "APS-C",                 23.60f, 15.70f, 1.0f },
    { "Full Frame (35mm)",     36.00f, 24.00f, 1.0f },
    { "16:9 DSLR",             36.00f, 20.25f, 1.0f },
    { "Anamorphic 2x Super 35", 24.89f, 18.66f, 2.0f },
    { "65mm ALEXA",            54.12f, 25.58f, 1.0f },
    { "IMAX 70mm",             70.41f, 52.63f, 1.0f },
};

// PRIMES. The union of the two lens sets this program was briefed with
// (CAMERA_LENS_SPEC §3 lists 12/16/24/35/50/85/100; the build brief lists
// 12/24/35/50/85/105/200) — every focal length either names is here, because
// the table is data and dropping one would be an arbitrary choice a user pays
// for. minFStop is the widest aperture the lens is conventionally offered at.
const LensPreset kLenses[] = {
    {  "12mm",  12.0f, 2.8f, "ultra wide — rectilinear, not a fisheye" },
    {  "16mm",  16.0f, 2.8f, "ultra wide" },
    {  "24mm",  24.0f, 1.4f, "wide" },
    {  "35mm",  35.0f, 1.4f, "wide normal — the reportage lens" },
    {  "50mm",  50.0f, 1.2f, "normal" },
    {  "85mm",  85.0f, 1.4f, "portrait" },
    { "100mm", 100.0f, 2.8f, "macro / short tele" },
    { "105mm", 105.0f, 1.4f, "portrait tele" },
    { "200mm", 200.0f, 2.0f, "tele" },
};

}   // namespace

const FilmbackPreset *filmbackPresets(int &count)
{
    count = int(sizeof(kFilmbacks) / sizeof(kFilmbacks[0]));
    return kFilmbacks;
}

const LensPreset *lensPresets(int &count)
{
    count = int(sizeof(kLenses) / sizeof(kLenses[0]));
    return kLenses;
}

}   // namespace lens
}   // namespace iris
