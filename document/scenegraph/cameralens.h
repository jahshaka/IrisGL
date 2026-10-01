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

/// THE WIDE-ASPECT FRAMING HOLD, document side: the VERTICAL angle that keeps
/// this camera's HORIZONTAL extent at the one it has on a frame of
/// `framingAspect`, when the real frame is WIDER than that.
///
/// THE SAME POLICY THE ENGINE APPLIES WHEN IT DRAWS
/// (jahshaka::engine::verticalFovForFramingAspect, engine/Types.h — the two are
/// deliberately NOT shared, because a document header may not include an engine
/// one; the arithmetic below is the same pair of identities this file already
/// exports, so there is no second formula, only a second call site):
///
///     h  = 2 * atan( tan(v/2) * framingAspect )   [horizontalFovDeg]
///     v' = 2 * atan( tan(h/2) / aspect )          [verticalFovDegFromHorizontal]
///
/// WHY THE DOCUMENT NEEDS IT AT ALL (owner report 2026-09-07, defect: clicking
/// a sphere in the Grand Showroom on a wide window selects a column behind it).
/// The engine narrows a FREE camera's vertical angle on a wide target, so the
/// image is not what the stored angle alone would produce — and every pick ray
/// is unprojected through the DOCUMENT camera's projection matrix
/// (ScenePicker::screenSegment). Two different frusta means the ray misses by
/// more the further the click is from the centre: measured on the Showroom's
/// 75-degree camera at 3184x536, a sphere DRAWN at x = 2829 picked a column
/// eleven units behind it. So the hold is a property of the camera
/// (CameraNode::framingAspect), and this is the one function both the
/// projection and any other document-side consumer go through.
///
/// WHY AN ASPECT AND NOT A DEGREE CAP (owner-blocking defect, 2026-09-08). The
/// first version of this function capped the HORIZONTAL angle at a fixed 95
/// degrees, which a 75-degree lens exceeds at 1.42:1 — so the Grand Showroom
/// rendered zoomed in (63 degrees vertical, not 75) on every monitor wider
/// than 3:2 and "imported assets have the wrong scale" was this. A degree cap
/// cannot say "leave ordinary windows alone" because ordinary depends on the
/// lens; an aspect can, and the identity below it is EXACT.
///
/// The hold only ever NARROWS, and only STRICTLY ABOVE the framing aspect: at
/// or below it the angle is returned BIT-IDENTICALLY, with no arithmetic at
/// all, which is what keeps every existing 16:9 projection (and every pixel
/// assertion built on one) exactly as it was. A non-positive framing aspect,
/// aspect or angle is likewise the identity — off is free.
float verticalFovDegForFramingAspect(float verticalFovDeg, float aspect, float framingAspect);

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

// ---- EXPOSURE (CAMERA_LENS_SPEC §4; EXPOSURE-1, 2026-09-17) ---------------
//
// THIS IS THE ONE EXPOSURE MODEL IN THE PROGRAM. The document stores STOPS,
// everywhere — the world's grade and a camera's override are the same unit on
// the same axis — and exactly one conversion (`toChain`, below) turns a
// resolved exposure into what the post chain takes. Before this lane there were
// five spellings of the quantity spread over four files and two units (RENDER
// AUDIT A3); two of them are deleted and the rest are stated here.
//
// THE CHAIN'S AXIS. Ogre's HDR material takes `(1024 * e^(E-2), 7.5 - max,
// 7.5 - min)` and its shader computes
//
//     multiplier = 1024 * e^(E-2) / e^( clamp( meanLogLuma, 7.5-max, 7.5-min ) )
//     history    = mix( multiplier, history, 0.25 ^ dt )        // ~75%/s
//
// (DownScale03_SumLumEnd_ps.glsl, verified in the pin). So `E` lives on a
// NATURAL-LOG axis — one photographic stop is ln 2 of it. That axis is an
// ENGINE unit: nothing in the document holds it any more.
//
// AUTO vs MANUAL, and what each one IS in the renderer.
//   * AUTO is that shader: a meter (a mean of log luminance over the frame)
//     drives the multiplier, bounded by the window.
//   * MANUAL replaces the whole measurement with a constant — the chain's
//     FIXED-EXPOSURE form (PostFxDesc::tonemapFixed), which clears the 1x1
//     exposure texture to `e^(E-2) / 0.18` and draws the identical tonemap
//     node. It is exact from the first frame, it is five quads and four
//     textures cheaper, and it is BY CONSTRUCTION the same grade a thumbnail
//     or a screenshot of the same world gets, because those already use that
//     form. The old recipe — pin the shader's clamp by setting min == max —
//     is DELETED as a way of SPELLING manual exposure: it reached the same
//     number through a temporal filter that took about a second to arrive. Its
//     constant survives under its real name, `meterGreyCardChain()`, because
//     the WINDOW still lives on the meter's axis and has to be converted
//     through it.
//
// THE ANCHOR. `stops = 0` means "the default world grade", and that grade is
// DERIVED FROM PHYSICS rather than tuned by hand (it was +0.6 in chain units,
// a number fitted by eye in 2026-09). See `defaultExposureChain()`.
/// THE GREY CARD, twice over, because the two 0.18s in this file are different
/// quantities that happen to share a number:
///   * `kGreyCardReflectance` is the 18 % REFLECTANCE a photographic meter is
///     calibrated against — a property of a piece of card;
///   * `kGreyCardDisplay` is the 18 % of maximum a correctly exposed grey card
///     should make the DISPLAY emit — a property of the picture.
/// (The chain's own `e^(E-2)/0.18` carries a third one, the reflectance again.)
constexpr float kGreyCardReflectance = 0.18f;
constexpr float kGreyCardDisplay     = 0.18f;

/// THE FILM CURVE (IMAGE-1): Unreal Engine's filmic tonemapper, the ACES-based
/// curve of Unreal 4.15 (TonemapCommon.ush, FilmToneMap), as the fork's
/// `Samples/Media/2.0/scripts/materials/HDR/GLSL/FinalToneMapping_ps.glsl`
/// runs it: five parameters, Unreal's defaults below. For a NEUTRAL input (a
/// grey) the colour terms of the shader — the RRT's glow and red modifier, the
/// pre and post desaturation and the AP0/AP1 round trip — are all the identity,
/// so this scalar is the curve a grey card sees. The tonemap suite holds the
/// GPU to it at nine exposures.
struct FilmParams {
    float slope = 0.88f;
    float toe = 0.55f;
    float shoulder = 0.26f;
    float blackClip = 0.0f;
    float whiteClip = 0.04f;
};

/// The curve: linear film input (post-exposure, scene-referred) to linear
/// display output, before the sRGB encode. The curve is BUILT so that
/// filmCurve(0.18) == 0.18 for any parameters (Unreal solves the toe through
/// that point), so an 18 % card exposed to 0.18 displays as code 118.
float filmCurve(float x, const FilmParams &params = FilmParams());

/// THE TONEMAPPER INPUT THAT DISPLAYS AS AN 18 % GREY CARD — the curve
/// inverted at `kGreyCardDisplay`. 0.18 by the curve's construction; computed
/// (a bisection on the log axis, cached) rather than written so that the
/// statement is "the inverse of the curve", not a number that happens to agree.
///
/// The window and every offscreen target are plain UNORM and the shader
/// applies the exact sRGB encode itself (SRGB-ENCODE-1), so "the display emits
/// 18 %" is the curve's output 0.18, code 118 on screen and in a screenshot.
float greyCardFilmInput();

/// THE KEY IRRADIANCE of a scene's lights, in the renderer's own units.
///
/// HlmsPbs shades a Lambertian surface as `L = lightDiffuse * NdotL * albedo/PI`
/// (kD is albedo/PI — 200.BRDFs_piece_ps.any: "already included in kD"), and
/// this engine writes `lightDiffuse = colour * intensity * PI`
/// (OgreScene.cpp setPowerScale). So `lightDiffuse` IS the irradiance a surface
/// facing the light receives, and a light of intensity `i` delivers `i * PI`.
///
/// The SKY LIGHT is an integral rather than a beam: a uniform sky of radiance
/// `L_sky` puts `PI * L_sky` on an unshadowed upward-facing surface, scaled by
/// the Sky Light's own intensity.
///
///     E_key = PI * ( sunIntensity + skyLightIntensity * skyRadiance )
///
/// `skyRadiance` is the sky's own linear radiance (a colour sky is its colour
/// decoded sRGB->linear; the analytic sky is its integral). NdotL is taken as 1
/// — the key is what a surface FACING the key light gets, which is the
/// photographic definition and not an average over the frame.
float keyIrradiance(float sunIntensity, float skyLightIntensity, float skyRadiance);

/// THE EXPOSURE THAT DEVELOPS THAT ILLUMINATION CORRECTLY, on the chain's axis.
///
/// An 18 % grey card under `E_key` has radiance `L = 0.18 * E_key / PI`. Manual
/// exposure multiplies by `m = e^(E-2) / 0.18` (the chain's own constant), and
/// a correctly exposed grey card lands on `greyCardFilmInput()`:
///
///     L * m = x*
///     e^(E-2) = 0.18 * x* / L = x* * PI / E_key          (the two 0.18s cancel)
///     E       = 2 + ln( x* * PI / E_key )
///
/// A doubling of the light is exactly one stop down, which is the property that
/// makes this a meter and not a fudge factor.
float exposureForKeyIrradiance(float keyIrradianceValue);

/// THE DEFAULT WORLD GRADE, derived: the formula above for the lights a NEW
/// SCENE is born with (MainWindow::createDefaultScene; SKY-DEFAULTS-1) — the
/// sun at intensity 1 and 50 degrees of elevation, the Sky Light at 1 over the
/// REALISTIC sky (haze 10, brightness 1), on the upward-facing floor:
///
///     E_key = 1.653 (the sun) + 0.383 (the Sky Light)   = 2.036
///     x*    = 0.18                     (Unreal's film curve, inverted at 0.18)
///     E     = 2 + ln(0.18 * PI / 2.036)   = 0.71894
///
/// (both terms MEASURED through the renderer on an 18 % card — cameralens.cpp
/// defaultKeyIrradiance has the numbers; IMAGE-1 re-measured them.) So an
/// 18 % card on a new scene's floor develops to the film's 0.18: code 118.
float defaultExposureChain();
/// THE NEW LAMP'S INTENSITY (IMAGE-1): a point or spot light added to a scene
/// puts an 18 % card 3 m away, facing it, on the film's grey (code 118) at the
/// default exposure, by itself. MEASURED through the renderer
/// (spikes/image-1/scripts/lamp.js): a lamp of intensity 1, 3 m above an 18 %
/// card with the sun and the sky off, gives the card 0.2523 of the default key
/// irradiance's units (the renderer's own response at normal incidence), so
/// 2.036 / 0.2523 = 8.07 -> 8. (The units: a lamp of intensity I lights a
/// surface 1 m away, facing it, exactly as a sun of intensity I does.)
constexpr float kDefaultLampIntensity = 8.0f;
/// The chain exposure zero stops corresponds to — `defaultExposureChain()`.
float exposureAnchorChain();

/// stops -> the chain's `E`, and back. Exact inverses.
float exposureStopsToChain(float stops);
float exposureChainToStops(float chain);

/// The multiplier the tonemapper applies for a chain exposure `E` in the FIXED
/// (manual) form: `e^(E-2) / 0.18`. This is the one number manual exposure is:
/// the chain's clear colour, the thumbnail grade's constant, and the value the
/// auto path converges to on a scene whose measured geometric-mean luminance is
/// the 0.18 grey card. Also used to re-seed the adaptation history on a cut.
float exposureMultiplier(float chainExposure);

/// THE WINDOW IS NOT AN EXPOSURE, and this is the number that says so.
///
/// `chain::setExposure(E, min, max)` pushes `(1024*e^(E-2), 7.5-max, 7.5-min)`
/// and DownScale03 clamps the MEASURED log-luminance `ln(1024 * Y)` into that
/// pair. So the window lives on the MEASUREMENT's axis, not the exposure's, and
/// the two axes have different zeros. A grey-card scene measures
/// `ln(1024 * 0.18)`, and the window value that corresponds to it is
///
///     kMeterGreyCardChain = 7.5 - ln(1024 * 0.18) = 2.283327
///
/// At that value the clamp is exactly where an 18 % grey card would put the
/// meter, so the automatic multiplier equals the MANUAL grade at the same `E`:
/// `1024*e^(E-2) / e^(7.5-w)` with `w = kMeterGreyCardChain` is `e^(E-2)/0.18`.
/// Away from it the auto grade differs from Manual by `e^(w - kMeterGreyCardChain)`,
/// i.e. by exactly `(w - kMeterGreyCardChain) / ln 2` stops.
///
/// SO THE DOCUMENT'S WINDOW IS IN STOPS *FROM THE MANUAL GRADE*: a window of
/// [-3.5, +3.5] means "the meter may land up to three and a half stops either
/// side of the exposure you typed", and [0, 0] means "exactly the exposure you
/// typed", which is the same picture Manual renders. That is the only reading
/// of the pair a user can act on, and it is what makes the three numbers ONE
/// dial rather than a number and two engine internals.
///
/// (This constant is the one EXPOSURE-1 deleted as `manualExposureClamp()`. It
/// was deleted as the product's way of SPELLING manual exposure — Manual is the
/// chain's fixed form now — and it comes back named for what it always was: the
/// place on the meter's axis where the meter agrees with the grey card. The
/// first cut of this lane converted the window with the EXPOSURE's anchor and
/// was 2.43 stops out; the suites pinned only the window's WIDTH, so nothing
/// caught it — they pin the OFFSET now.)
float meterGreyCardChain();

}   // namespace lens

// ---- THE RESOLVED EXPOSURE (EXPOSURE-1) -----------------------------------

/// Manual (a number) or Auto (a measurement). The WORLD's mode; a camera adds
/// `Inherit` on top of these two (iris::CameraExposureMode).
enum class ExposureMode {
	Manual,
	Auto
};

const char *exposureModeName(ExposureMode m);              ///< "manual" | "auto"
ExposureMode exposureModeFromName(const char *name, bool *ok = nullptr);

/// HOW THE AUTOMATIC EXPOSURE'S METER LOOKS AT THE FRAME (EXPOSURE-2) — a
/// camera's METERING PATTERN, by its own names. Read only in Auto: Manual
/// measures nothing at all.
///
/// It is a CHOICE, not a number, and it is the answer to the one thing a
/// percentile clip cannot fix: a white surface filling most of the frame is
/// inside any sane percentile band, so the clips do not bound it — WHERE the
/// meter looks does. The geometry of each pattern (the falloff, the spot's
/// area) is physics and lives with the renderer that applies it
/// (jahshaka::engine::meter); the document holds only the choice.
enum class ExposureMetering {
	/// The whole frame, equally. What the engine's meter always did.
	Average,
	/// The classic camera default, and ours: a Gaussian on the distance from
	/// the frame centre with a small pedestal, so most of the sensitivity is on
	/// the subject and the edges of the frame still count for something.
	CentreWeighted,
	/// A small centre disc — a couple of percent of the frame. Expose for the
	/// thing you are pointing at and let the rest fall where it falls.
	Spot
};

const char *exposureMeteringName(ExposureMetering m);     ///< "average" | "centreWeighted" | "spot"
ExposureMetering exposureMeteringFromName(const char *name, bool *ok = nullptr);

/// ONE EXPOSURE STATEMENT, in the document's unit, after the world and the
/// driving camera have been combined. This is what crosses the mirror.
struct ExposureDesc {
	ExposureMode mode = ExposureMode::Manual;
	/// The exposure in STOPS. In Manual it IS the exposure; in Auto it is the
	/// midpoint the adaptation works around.
	float stops = 0.0f;
	/// The window Auto may adapt within, in stops. Ignored in Manual (which
	/// measures nothing at all). Kept ordered: min <= max.
	float minStops = -3.5f;
	float maxStops = 3.5f;
	/// THE METER ITSELF (EXPOSURE-2), and only the meter: how it looks at the
	/// frame, and which slice of what it sees it believes. Ignored in Manual.
	ExposureMetering metering = ExposureMetering::CentreWeighted;
	/// The PERCENTILES of the metered weight, darkest first, the meter averages
	/// between — 10 and 90 by default, so the darkest and brightest tenth of
	/// what the pattern sees are thrown away. Unreal's MinPercent/MaxPercent.
	/// Kept ordered; a degenerate pair means "the whole frame".
	float lowPercent = 10.0f;
	float highPercent = 90.0f;
};

namespace lens
{

/// THE ONE CONVERSION into the post chain's units.
///
/// TWO AXES, and they are not the same one: the EXPOSURE converts through
/// `exposureStopsToChain` (anchored at the derived default grade) and the
/// WINDOW through `meterGreyCardChain` (anchored where the meter agrees with a
/// grey card), because the chain clamps a MEASUREMENT with the window and
/// multiplies by the exposure. Both are STOPS in the document and both are
/// stops the user can act on — the window's are stops away from the exposure
/// they typed — which is why the pair is one dial and not two internals.
///
/// `fixed` out is the chain's FIXED-EXPOSURE form (PostFxDesc::tonemapFixed):
/// true for Manual. In that form `chainMin`/`chainMax` are not read by anything
/// — the clear colour is the whole grade — and they are returned at the
/// resolved exposure so a description is never carrying a stale window.
void toChain(const ExposureDesc &d, float &chainExposure, float &chainMin, float &chainMax,
             bool &fixed);

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
    const char *name = nullptr;
    float sensorWidth = 0.0f;    ///< mm
    float sensorHeight = 0.0f;   ///< mm
    float squeeze = 1.0f;        ///< 1.0 except for anamorphic entries
};

struct LensPreset {
    const char *name = nullptr;
    float focalMm = 0.0f;
    float minFStop = 0.0f;       ///< the widest aperture this lens is offered at
    const char *note = nullptr;  ///< a human hint ("ultra wide", "portrait")
};

/// The tables and their sizes. Stable order — the verbs report them as-is.
const FilmbackPreset *filmbackPresets(int &count);
const LensPreset     *lensPresets(int &count);

// ---- THE IMAGE BLOCK (IMAGE-1) ---------------------------------------------
//
// How a camera develops the picture after the exposure: contrast, saturation,
// shadows/highlights, white balance, vignette and the film curve. The WORLD
// holds the defaults (Scene::image — the editor's view and every camera that
// does not say otherwise); a camera overrides any field, tri-state, through
// its post-override map under the same id. ONE TABLE, read by the scene's
// constructor, the reader and the writer, the camera's override keys, the
// world.postFx / camera.postFx verbs and both panels, so an id, a default or
// a range can never mean two things. The engine's twin is
// jahshaka::engine::ImageGrade (Types.h), which carries the meaning of each.
enum ImageParam {
    ImageContrast = 0,
    ImageSaturation,
    ImageShadows,
    ImageHighlights,
    ImageWhiteTemperature,
    ImageWhiteTint,
    ImageVignette,
    ImageFilmSlope,
    ImageFilmToe,
    ImageFilmShoulder,
    ImageFilmBlackClip,
    ImageFilmWhiteClip,
    ImageParamCount
};

struct ImageParamDef {
    const char *id = nullptr;       ///< the verb key, the file key, the override key
    const char *label = nullptr;    ///< the panel's
    float defaultValue = 0.0f;
    float minValue = 0.0f, maxValue = 1.0f;
    double perPixelStep = 0.01;
    int decimals = 2;
    bool advanced = false;          ///< the film's five: the "Film (advanced)" rows
    const char *doc = nullptr;      ///< the row tooltip and the verb's documentation
};

/// The table, in panel order, indexed by ImageParam.
const ImageParamDef *imageParams();
/// The entry with this id, or null.
const ImageParamDef *imageParam(const char *id);

}   // namespace lens
}   // namespace iris

#endif // CAMERALENS_H
