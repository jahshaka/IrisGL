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
#include <string>

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

// ---- exposure (CAMERA_LENS_SPEC §4; EXPOSURE-1) ----------------------------
//
// THE DERIVATION. The header carries the physics; these are the numbers.

namespace {
/// Unreal Engine 4.15+, TonemapCommon.ush FilmToneMap's curve on ONE channel of
/// the AP1 working colour. Every line maps one-to-one onto
/// FinalToneMapping_ps.glsl's jahFilmToneMap.
double filmChannel(double x, const FilmParams &p)
{
    const double slope = std::max(double(p.slope), 0.01);
    const double toe = p.toe, shoulder = p.shoulder;
    const double black = p.blackClip, white = p.whiteClip;
    const double toeScale = 1.0 + black - toe;
    const double shoulderScale = 1.0 + white - shoulder;
    const double inMatch = 0.18, outMatch = 0.18;
    double toeMatch;
    if (toe > 0.8) {
        toeMatch = (1.0 - toe - outMatch) / slope + std::log10(inMatch);
    } else {
        const double bt = (outMatch + black) / toeScale - 1.0;
        toeMatch = std::log10(inMatch) - 0.5 * std::log((1.0 + bt) / (1.0 - bt)) * (toeScale / slope);
    }
    const double straightMatch = (1.0 - toe) / slope - toeMatch;
    const double shoulderMatch = shoulder / slope - straightMatch;
    const double lc = std::log10(std::max(x, 1e-10));
    const double straight = slope * (lc + straightMatch);
    double toeC = -black + (2.0 * toeScale) / (1.0 + std::exp((-2.0 * slope / toeScale) * (lc - toeMatch)));
    double shC = (1.0 + white) -
                 (2.0 * shoulderScale) / (1.0 + std::exp((2.0 * slope / shoulderScale) * (lc - shoulderMatch)));
    toeC = lc < toeMatch ? toeC : straight;
    shC = lc > shoulderMatch ? shC : straight;
    double t = (lc - toeMatch) / (shoulderMatch - toeMatch);
    t = std::min(std::max(t, 0.0), 1.0);
    if (shoulderMatch < toeMatch) t = 1.0 - t;
    t = (3.0 - 2.0 * t) * t * t;
    return toeC + (shC - toeC) * t;
}

void mul3(const double m[9], const double v[3], double o[3])
{
    for (int r = 0; r < 3; ++r) o[r] = m[r * 3] * v[0] + m[r * 3 + 1] * v[1] + m[r * 3 + 2] * v[2];
}
}   // namespace

void filmRGB(const double in[3], double out[3], const FilmParams &p)
{
    // The shader's matrices, rows: sRGB -> ACEScg (Bradford D65 -> D60, rows
    // normalised so white maps to white), AP1 <-> AP0, ACEScg -> sRGB.
    static const double S2A[9] = { 0.6131486203, 0.3394883591, 0.0473630206, 0.0702074147, 0.9163424763,
                                   0.0134501090, 0.0206231422, 0.1095899890, 0.8697868689 };
    static const double A2S[9] = { 1.7050473375, -0.6217891459, -0.0832581917, -0.1302575067, 1.1408060644,
                                   -0.0105485577, -0.0240032831, -0.1289688126, 1.1529720957 };
    static const double A1T0[9] = { 0.6954522414, 0.1406786965, 0.1638690622, 0.0447945634, 0.8596711185,
                                    0.0955343182, -0.0055258826, 0.0040252103, 1.0015006723 };
    static const double A0T1[9] = { 1.4514393161, -0.2365107469, -0.2149285693, -0.0765537734, 1.1762296998,
                                    -0.0996759264, 0.0083161484, -0.0060324498, 0.9977163014 };
    static const double Y[3] = { 0.2722287168, 0.6740817658, 0.0536895174 };
    double ap1[3], c0[3];
    mul3(S2A, in, ap1);
    mul3(A1T0, ap1, c0);
    // The RRT's glow module.
    const double mi = std::min({ c0[0], c0[1], c0[2] }), ma = std::max({ c0[0], c0[1], c0[2] });
    const double sat = (std::max(ma, 1e-10) - std::max(mi, 1e-10)) / std::max(ma, 1e-2);
    const double chroma = std::sqrt(std::max(0.0, c0[2] * (c0[2] - c0[1]) + c0[1] * (c0[1] - c0[0]) +
                                                     c0[0] * (c0[0] - c0[2])));
    const double yc = (c0[0] + c0[1] + c0[2] + 1.75 * chroma) / 3.0;
    const double sx = (sat - 0.4) / 0.2;
    const double tt = std::max(1.0 - std::fabs(0.5 * sx), 0.0);
    const double s = 0.5 * (1.0 + (sx > 0 ? 1.0 : sx < 0 ? -1.0 : 0.0) * (1.0 - tt * tt));
    const double gIn = 0.05 * s, mid = 0.08;
    const double glow = yc <= 2.0 / 3.0 * mid ? gIn : yc >= 2.0 * mid ? 0.0 : gIn * (mid / yc - 0.5);
    for (double &v : c0) v *= 1.0 + glow;
    // The RRT's red modifier (scale 0.82, pivot 0.03, hue 0, width 135).
    double hue = 0.0;
    if (!(c0[0] == c0[1] && c0[1] == c0[2])) {
        hue = 57.2957795131 * std::atan2(1.7320508076 * (c0[1] - c0[2]), 2.0 * c0[0] - c0[1] - c0[2]);
        if (hue < 0.0) hue += 360.0;
    }
    const double ch = hue > 180.0 ? hue - 360.0 : hue;
    double hw = std::min(std::max(1.0 - std::fabs(2.0 * ch / 135.0), 0.0), 1.0);
    hw = hw * hw * (3.0 - 2.0 * hw);
    hw *= hw;
    c0[0] += hw * sat * (0.03 - c0[0]) * (1.0 - 0.82);
    // ACEScg, pre-desaturated; the curve per channel; post-desaturated; back to sRGB.
    double w[3];
    mul3(A0T1, c0, w);
    for (double &v : w) v = std::max(v, 0.0);
    double l = w[0] * Y[0] + w[1] * Y[1] + w[2] * Y[2];
    for (double &v : w) v = l + (v - l) * 0.96;
    double t[3];
    for (int k = 0; k < 3; ++k) t[k] = filmChannel(w[k], p);
    l = t[0] * Y[0] + t[1] * Y[1] + t[2] * Y[2];
    for (double &v : t) v = std::max(l + (v - l) * 0.93, 0.0);
    mul3(A2S, t, out);
    for (int k = 0; k < 3; ++k) out[k] = std::max(out[k], 0.0);
}

float filmCurve(float xIn, const FilmParams &p)
{
    // On a grey the colour terms are the identity: the curve on one channel.
    return float(std::max(0.0, filmChannel(double(xIn), p)));
}

float greyCardFilmInput()
{
    // The curve is monotonic: bisect its input on the log axis for the output
    // kGreyCardDisplay. Cached — the anchor below calls it on every conversion.
    static const float cached = [] {
        double lo = -6.0, hi = 3.0;   // log10 of the input
        for (int i = 0; i < 80; ++i) {
            const double mid = 0.5 * (lo + hi);
            if (double(filmCurve(float(std::pow(10.0, mid)))) < double(kGreyCardDisplay)) lo = mid;
            else hi = mid;
        }
        return float(std::pow(10.0, 0.5 * (lo + hi)));
    }();
    return cached;
}

float keyIrradiance(float sunIntensity, float skyLightIntensity, float skyRadiance)
{
    const double sun = std::max(0.0, double(sunIntensity));
    const double sky = std::max(0.0, double(skyLightIntensity)) * std::max(0.0, double(skyRadiance));
    return float(kPi * (sun + sky));
}

/// The key irradiance of the DEFAULT TEMPLATE (MainWindow::createDefaultScene),
/// RE-DERIVED FROM THE PHYSICAL SKY (SKY-DEFAULTS-1): the floor of a new Basic
/// scene, i.e. an UPWARD-facing Lambert card, under
///
///   the sun   intensity 1 at 50 degrees of elevation (scenetemplate::
///             kSunElevationDegrees), coloured by the default atmosphere
///             (Sun Follows Atmosphere: its noon colour, x 0.94 at 50 degrees)
///   the Sky Light  intensity 1 over the REALISTIC sky at the default haze 10
///             and brightness 1 — the environment's SH at +Y is a mean incident
///             radiance of 0.1325 (luminance), pi x that = 0.416 by the formula
///
/// MEASURED THROUGH THE RENDERER, not assumed (re-measured by IMAGE-1,
/// 2026-10-01, spikes/image-1/scripts/anchor.js): an 18 % card (#767676, linear
/// 0.1812) on the floor of a new Basic scene read 0.0953 of plain radiance
/// under the sun alone and 0.1174 under both, so the floor receives
///     sun   pi x 0.0953 / 0.1812 = 1.653
///     sky   pi x 0.0221 / 0.1812 = 0.383
/// (the sun's number is sin 50 x its atmosphere colour x pi x the renderer's own
/// Lambert response at that angle — which is the whole reason it is measured
/// rather than written as pi sin 50: the chain must put the CARD, as drawn, on
/// the grey card). The previous anchor (1.987 + 0.406) was measured before the
/// diffuse lobe and the sky moved, and left the card 0.22 stops dark (code 108
/// where the film's grey is 118; spikes/bright-1).
/// Keyed through keyIrradiance's own form: pi x (sun + skyLight x sky).
static float defaultKeyIrradiance()
{
    constexpr double kSunAtFloor = 1.653;   // renderer units, measured (above)
    constexpr double kSkyAtFloor = 0.383;   // renderer units, measured (above)
    return keyIrradiance(float(kSunAtFloor / kPi), 1.0f /*sky light*/, float(kSkyAtFloor / kPi));
}

float exposureForKeyIrradiance(float keyIrradianceValue)
{
    // A scene with no light at all has no correct exposure. The DEFAULT
    // TEMPLATE's illumination stands in — and it is the INPUT that is
    // substituted, not the result, so this stays the one place the formula
    // lives and there is no way for the two to disagree (or to recurse).
    const double e = keyIrradianceValue > 0.0f ? double(keyIrradianceValue)
                                               : double(defaultKeyIrradiance());
    return float(2.0 + std::log(double(greyCardFilmInput()) * kPi / e));
}

float defaultExposureChain()
{
    // ONE SPELLING: the default template's own lights, through the same rule
    // every other scene goes through. "The default" is an evaluation of the
    // general formula, never a second copy of it.
    //
    // CACHED, because this is the ANCHOR: exposureStopsToChain calls it on
    // every conversion and it is a pow and three logs deep. The inputs are
    // compile-time constants, so once is right.
    static const float cached = exposureForKeyIrradiance(defaultKeyIrradiance());
    return cached;
}

float exposureAnchorChain() { return defaultExposureChain(); }

float exposureStopsToChain(float stops)
{
    // One stop is a doubling, and the chain's axis is natural-log, so a stop is
    // ln 2 of it. Anchored so that zero stops is the default world grade.
    return exposureAnchorChain() + stops * float(kLn2);
}

float exposureChainToStops(float chain)
{
    return (chain - exposureAnchorChain()) / float(kLn2);
}

float exposureMultiplier(float chainExposure)
{
    return float(std::exp(double(chainExposure) - 2.0) / double(kGreyCardReflectance));
}

float meterGreyCardChain()
{
    // 7.5 - ln(1024 * 0.18). DERIVED, not typed: 1024 is the chain's own scale
    // (DownScale01 measures ln(1024 * Y)), 0.18 is the grey card's reflectance
    // and 7.5 is the offset setExposure applies. The header says what it means.
    return 7.5f - float(std::log(1024.0 * double(kGreyCardReflectance)));
}

}   // namespace lens

const char *exposureModeName(ExposureMode m)
{
    return m == ExposureMode::Auto ? "auto" : "manual";
}

ExposureMode exposureModeFromName(const char *name, bool *ok)
{
    const std::string n = name ? name : "";
    if (ok) *ok = true;
    if (n == "auto") return ExposureMode::Auto;
    if (n == "manual") return ExposureMode::Manual;
    if (ok) *ok = false;
    return ExposureMode::Manual;
}

const char *exposureMeteringName(ExposureMetering m)
{
    return m == ExposureMetering::Average        ? "average"
         : m == ExposureMetering::Spot           ? "spot"
                                                 : "centreWeighted";
}

ExposureMetering exposureMeteringFromName(const char *name, bool *ok)
{
    const std::string n = name ? name : "";
    if (ok) *ok = true;
    if (n == "average") return ExposureMetering::Average;
    if (n == "centreWeighted") return ExposureMetering::CentreWeighted;
    if (n == "spot") return ExposureMetering::Spot;
    if (ok) *ok = false;
    return ExposureMetering::CentreWeighted;
}

namespace lens
{

void toChain(const ExposureDesc &d, float &chainExposure, float &chainMin, float &chainMax,
             bool &fixed)
{
    chainExposure = exposureStopsToChain(d.stops);
    fixed = d.mode == ExposureMode::Manual;
    if (fixed) {
        // Nothing reads the window in the fixed form (the exposure IS a clear
        // colour). Returning the exposure itself keeps a description from
        // carrying a window that means nothing, so two descriptions that grade
        // identically compare equal and the chain is not rebuilt for nothing.
        chainMin = chainMax = chainExposure;
        return;
    }
    // THE WINDOW IS ON THE METER'S AXIS, NOT THE EXPOSURE'S (the header's
    // meterGreyCardChain says why, with the arithmetic). Zero stops here means
    // "where the meter agrees with a grey card", which is the picture Manual
    // renders at the same exposure — so a window of [0, 0] IS the manual grade
    // and [-3.5, +3.5] is symmetric about it. Converting these through
    // exposureStopsToChain instead put the shipped window at [-5.93, +1.07]
    // around Manual and a pinned [0, 0] 2.43 stops dark.
    const float base = meterGreyCardChain();
    chainMin = base + std::min(d.minStops, d.maxStops) * float(kLn2);
    chainMax = base + std::max(d.minStops, d.maxStops) * float(kLn2);
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


// ---- the image block (IMAGE-1) -----------------------------------------------

namespace {
const ImageParamDef kImageParams[ImageParamCount] = {
    { "contrast", "Contrast", 1.0f, 0.0f, 2.0f, 0.005, 3, false,
      "How far the picture's tones spread from the grey card: a power about 0.18 on the "
      "scene's light (Unreal's colour correction), so the grey card stays where the exposure put "
      "it while darker tones get darker and brighter ones brighter. 1 is neutral." },
    { "saturation", "Saturation", 1.0f, 0.0f, 2.0f, 0.005, 3, false,
      "How coloured the picture is, about each pixel's own luminance: 0 is black and white, 1 "
      "neutral, 2 twice as saturated." },
    { "shadows", "Shadows", 0.0f, -4.0f, 4.0f, 0.01, 2, false,
      "A GAIN IN STOPS on the dark tones only (luminance below 0.09 after the exposure, "
      "Unreal's shadow range): +1 doubles the light in the shadows and leaves the mid tones "
      "and highlights alone. A lift, not the film curve's toe (that is Film Toe below)." },
    { "highlights", "Highlights", 0.0f, -4.0f, 4.0f, 0.01, 2, false,
      "A GAIN IN STOPS on the bright tones only (luminance from 0.5 to 1 after the exposure, "
      "Unreal's highlight range): -1 halves the light in the highlights. Not the film curve's "
      "shoulder (that is Film Shoulder below)." },
    { "whiteTemperature", "White Balance", 6500.0f, 1500.0f, 15000.0f, 10.0, 0, false,
      "The colour temperature, in kelvin, of the light the camera is balanced for: the picture "
      "is corrected from that white to neutral (a Bradford chromatic adaptation, Unreal's white "
      "balance). 6500 is neutral; a lower number cools the picture (it takes out a warm light), "
      "a higher one warms it." },
    { "whiteTint", "Tint", 0.0f, -1.0f, 1.0f, 0.005, 3, false,
      "The white balance's green-magenta axis, perpendicular to the temperature: positive takes "
      "out a green cast, negative a magenta one. 0 is neutral." },
    { "vignette", "Vignette", 0.0f, 0.0f, 1.0f, 0.005, 3, false,
      "How much of a lens's natural falloff the frame gets: the cos^4 law with the frame's "
      "corner at 45 degrees off axis, so 1 puts a quarter of the light in the corners. 0 is none." },
    { "filmSlope", "Film Slope", 0.88f, 0.2f, 1.5f, 0.005, 3, true,
      "The film curve's mid-tone steepness (Unreal's filmic tonemapper, Slope). Higher is more "
      "contrasty film. The grey card always develops to the same grey." },
    { "filmToe", "Film Toe", 0.55f, 0.0f, 1.0f, 0.005, 3, true,
      "How the film curve rolls into black (Unreal's Toe): higher holds more detail in the "
      "darks before they crush." },
    { "filmShoulder", "Film Shoulder", 0.26f, 0.0f, 1.0f, 0.005, 3, true,
      "How the film curve rolls into white (Unreal's Shoulder): higher compresses the "
      "highlights sooner, so less of the picture reaches pure white." },
    { "filmBlackClip", "Film Black Clip", 0.0f, 0.0f, 1.0f, 0.005, 3, true,
      "Where the film's toe crosses black (Unreal's Black clip): above 0 the darkest tones "
      "clip to black." },
    { "filmWhiteClip", "Film White Clip", 0.04f, 0.0f, 1.0f, 0.005, 3, true,
      "Where the film's shoulder crosses white (Unreal's White clip): above 0 the brightest "
      "tones reach pure white; 0 never quite gets there." },
};
}   // namespace

const ImageParamDef *imageParams() { return kImageParams; }

const ImageParamDef *imageParam(const char *id)
{
    if (!id) return nullptr;
    const std::string want = id;
    for (const ImageParamDef &d : kImageParams)
        if (want == d.id) return &d;
    return nullptr;
}

}   // namespace lens
}   // namespace iris
