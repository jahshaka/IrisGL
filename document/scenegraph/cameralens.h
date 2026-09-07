/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef CAMERALENS_H
#define CAMERALENS_H

// THE LENS MATH (CAMERA_LENS_SPEC §3, phases P1/P2).
//
// Pure functions and two tables. Nothing here knows about a CameraNode, a
// scene, Qt containers or the engine: it is the arithmetic that binds an angle
// of view to a piece of glass in front of a piece of film, plus the standard
// depth-of-field optics. That is deliberate — every claim this program makes
// about a number ("a 12 mm lens on Super 35 is 92 degrees across") is checked
// against a hand-computed value in tests/cameras, and a function that needs a
// document to run cannot be checked that way.
//
// UNITS, once, because mixing them is the classic bug in this arithmetic:
//   * focal lengths and sensor dimensions are MILLIMETRES;
//   * angles crossing this boundary are DEGREES (the document stores degrees);
//   * scene distances (focus, hyperfocal, the DoF limits) are WORLD UNITS,
//     which Jahshaka treats as METRES. The optics below are computed in
//     millimetres internally and converted once, at the edge.
//
// WHAT "FIT" MEANS (§3, Blender's model). A camera stores ONE angle of view —
// the VERTICAL one, because that is what the renderer takes (Ogre setFOVy).
// The sensor is a PAIR, so "which sensor dimension does the focal length bind
// through" is a real choice and it is what `sensorFit` names:
//   * Vertical   — through the sensor HEIGHT. This is what Jahshaka did before
//                  this program existed, so it is the default: no scene's
//                  projection moves because these fields arrived.
//   * Horizontal — through the sensor WIDTH, converted to the vertical angle
//                  through the frame ASPECT. This is the cine convention (a
//                  lens is sold by how wide it sees) and it is what finally
//                  makes `sensorWidth` do something.
//   * Auto       — Blender's rule: the sensor WIDTH covers the LARGER image
//                  axis. Landscape frames (aspect >= 1) behave like Horizontal;
//                  portrait frames bind the width through the vertical axis.
//
// THE ASPECT IS THE DOCUMENT'S, and it can differ from what a view renders.
// A view that does not constrain the aspect renders at the target's aspect
// (Ogre's auto aspect ratio), so a horizontal-fit camera's HORIZONTAL angle is
// only exactly the authored one when the frame it is shown in has the authored
// shape (constrainAspect, an export at outputWidth x outputHeight, or a window
// that happens to match). The vertical angle is always what the document says,
// because that is the value that is stored and pushed.
//
// ANAMORPHIC SQUEEZE multiplies the EFFECTIVE sensor WIDTH: a 2x anamorphic
// lens optically compresses twice as much horizontal world onto the same piece
// of film, so the horizontal angle of a 50 mm anamorphic matches a 25 mm
// spherical. It therefore only participates when the binding axis is
// horizontal — on a vertical fit it changes nothing, which is the honest
// answer rather than a hidden fudge (we do not model the DESQUEEZED delivery
// aspect; that is a display decision, not a projection one).

namespace iris
{

/// Which sensor dimension the angle of view binds through (CAMERA_LENS_SPEC §3).
enum class CameraSensorFit {
    Auto,
    Horizontal,
    Vertical
};

namespace lens
{

/// The axis a fit resolves to for a given frame aspect (width / height).
enum class FitAxis { Horizontal, Vertical };

/// The filmback, as the math sees it. Defaults are CameraNode's own.
struct Filmback {
    float sensorWidth  = 36.0f;   ///< mm
    float sensorHeight = 24.0f;   ///< mm
    float squeeze      = 1.0f;    ///< anamorphic; multiplies the effective WIDTH
    CameraSensorFit fit = CameraSensorFit::Vertical;
    float aspect       = 1.0f;    ///< frame aspect, width / height
};

/// Auto resolves against the aspect; the other two are themselves.
FitAxis fitAxis(CameraSensorFit fit, float aspect);

/// The effective sensor dimension (mm) on the binding axis — sensorHeight for a
/// vertical fit, sensorWidth * squeeze for a horizontal one. Auto on a portrait
/// frame binds the sensor WIDTH through the vertical axis (Blender's rule), and
/// takes no squeeze there: the squeeze is a property of the horizontal axis.
float bindingSensorMm(const Filmback &fb);

/// The VERTICAL angle of view (degrees) a focal length gives on this filmback.
/// Zero or negative focal lengths have no angle and return 0.
float verticalFovDegFromFocal(const Filmback &fb, float focalMm);

/// The exact inverse: the focal length (mm) that gives this vertical angle.
/// Degenerate angles (<= 0, >= 180) return 0.
float focalFromVerticalFovDeg(const Filmback &fb, float verticalFovDeg);

/// tan(h/2) = tan(v/2) * aspect — the standard cross-axis pair (§3).
float horizontalFovDeg(float verticalFovDeg, float aspect);
float verticalFovDegFromHorizontal(float horizontalFovDeg, float aspect);
/// The corner-to-corner angle, from the vertical one and the aspect.
float diagonalFovDeg(float verticalFovDeg, float aspect);

/// sqrt(w^2 + h^2) — the physical sensor diagonal, squeeze-free.
float sensorDiagonalMm(float sensorWidth, float sensorHeight);

/// The circle-of-confusion CRITERION for this sensor, in mm: the classic
/// diagonal/1500 (Zeiss). Full frame gives 0.0288 mm, which is the number every
/// published depth-of-field table for 35 mm film is computed from. It is a
/// criterion, not a measurement — it says how much blur we agree to call sharp.
float circleOfConfusionMm(float sensorWidth, float sensorHeight);

/// What `camera.focusInfo` reports (CAMERA_LENS_SPEC §3, P2). Distances are
/// METRES; `cocLimit` is millimetres. `farLimit` is +infinity when the far
/// depth-of-field limit is unbounded (focus at or beyond the hyperfocal
/// distance) — that is the physical answer, not an error.
struct FocusInfo {
    double focusDistance = 0.0;
    double hyperfocal    = 0.0;
    double nearLimit     = 0.0;
    double farLimit      = 0.0;
    double cocLimit      = 0.0;
};

/// The standard optics, from the public formulas (Filament is the reference for
/// the derivation; nothing is vendored):
///
///     H     = f^2 / (N * c) + f
///     near  = s * (H - f) / (H + s - 2f)
///     far   = s * (H - f) / (H - s)      (unbounded when s >= H)
///
/// with f the focal length, N the f-number, c the circle of confusion and s the
/// focus distance — all in millimetres inside, metres at the boundary. This
/// PAIRING (rather than the equally common H*s/(H +- (s - f))) is chosen because
/// it makes the hyperfocal identity exact: focusing at H gives [H/2, infinity),
/// which is what the definition of a hyperfocal distance says and what the
/// suite asserts.
FocusInfo focusInfo(float focalMm, float fStop, float focusDistanceMetres, float cocMm);

// ---- lens shift -----------------------------------------------------------
//
// The document authors lens shift as a FRACTION OF THE FRAME: 0.5 slides the
// image by half a frame width (or height). Everything below converts that into
// what a projection actually needs, and the two conversions are exact inverses
// so a round trip is loss-free (asserted in tests/cameras).
//
// THE TRAP, recorded because it costs a day if it is not (CAMERA_LENS_SPEC §1):
// Ogre's Frustum::setFrustumOffset is in WORLD UNITS AT THE NEAR PLANE, scaled
// internally by mNearDist / mFocalLength (OgreFrustum.cpp:370-371) — and that
// mFocalLength is the STEREO divisor (default 1.0), NOT a camera lens. Nothing
// in this program may touch setFocalLength. Because the scaling involves the
// near distance and the half-extent involves the field of view and the aspect,
// the conversion must be re-derived whenever ANY of fov, aspect or near
// changes; it is never a value to compute once and store.

/// The near-plane half-extent on an axis: tan(angle/2) * near.
float halfExtentAtNear(float fovDegOnAxis, float nearDist);

/// The world-space near-plane offset a fraction-of-frame shift asks for.
/// A full frame spans 2 * halfExtent, so the offset is shift * 2 * halfExtent.
float nearOffsetFromShift(float shiftFraction, float halfExtent);
float shiftFromNearOffset(float nearOffset, float halfExtent);

/// The value to hand Ogre's setFrustumOffset so the near plane ends up offset
/// by `nearOffsetFromShift`. Ogre multiplies it by near / stereoFocalLength, so
/// this divides that back out. With the default stereo focal length of 1.0 the
/// near distance cancels — a coincidence of the default, not a rule, which is
/// why it is spelled out here instead of simplified away.
float ogreFrustumOffset(float shiftFraction, float halfExtent, float nearDist,
                        float stereoFocalLength);
float shiftFromOgreFrustumOffset(float frustumOffset, float halfExtent, float nearDist,
                                 float stereoFocalLength);

/// Exponential smoothing toward a target, framerate-independent:
///     out = target + (current - target) * exp(-speed * dt)
/// Speed <= 0 or a non-positive dt snaps. Used by focus tracking (P2) and kept
/// here — as a pure function of an explicit dt — so it can be asserted without
/// a clock.
float smoothTowards(float current, float target, float speed, float dt);

// ---- the preset tables ----------------------------------------------------
//
// OURS, named as Jahshaka defaults. The filmback numbers are inspired by the
// preset lists every DCC ships (the spec's §8 note: a UE-era table was the
// one-source input and we do not claim parity with it); the lens set is a
// conventional prime kit. They are data, deliberately: a table is auditable and
// a hard-coded lens choice inside a verb is not.

struct FilmbackPreset {
    const char *name;
    float sensorWidth;    ///< mm
    float sensorHeight;   ///< mm
    float squeeze;        ///< 1.0 except for anamorphic entries
};

struct LensPreset {
    const char *name;
    float focalMm;
    float minFStop;       ///< the widest aperture this lens is offered at
    const char *note;     ///< a human hint ("ultra wide", "portrait")
};

/// The tables and their sizes. Stable order — the verbs report them as-is.
const FilmbackPreset *filmbackPresets(int &count);
const LensPreset     *lensPresets(int &count);

}   // namespace lens
}   // namespace iris

#endif // CAMERALENS_H
