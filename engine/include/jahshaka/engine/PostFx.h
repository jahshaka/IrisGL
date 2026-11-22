#pragma once
// THE ENGINE's POSTFX TYPES - engine-neutral values, one subject (ENGINE-HEADERS-1: split from Types.h by
// declaration name). NOTHING here may reference Ogre, Qt or GL; a new subject is a new header, never an append here.
#include <cmath>
#include <vector>

namespace jahshaka { namespace engine {

/// ONE LDR image filter — a "look" (POST_LOOKS_SPEC.md §4).
///
/// The looks stage is a stack of full-screen quads that runs at the very end of
/// the chain, AFTER tonemapping and AFTER SMAA (§4.2 and D5: SMAA is LDR edge
/// detection, so a look that CREATES edges — posterize bands, sharpen halos —
/// must not be smeared by it, and a look that WARPS the image must not have its
/// warped edges anti-aliased instead of the scene's). Every look is a colour
/// transform of the finished picture: nothing here can see depth, normals or
/// the scene at all.
enum class LookKind {
    /// Luma-weighted desaturation. p[0] = amount (0 = identity, 1 = grey).
    Desaturate = 0,
    /// Refraction through rippled glass: a procedural normal field offsets the
    /// UV. p[0] = amount (0 = identity), p[1] = scale (ripple frequency).
    GlassWarp,
    /// Zoom blur about a centre. p[0] = amount, p[1] = centre x, p[2] = centre
    /// y (both 0..1 in UV), p[3] = falloff exponent.
    RadialBlur,
    /// Sepia + dirt + flicker + frame jitter, all driven by `time`.
    /// p[0] = amount, p[1] = flicker, p[2] = dirt, p[3] = jitter.
    OldMovie,
    /// Colour quantization. p[0] = amount, p[1] = levels, p[2] = gamma.
    Posterize,
    /// 3x3 unsharp mask. p[0] = amount.
    Sharpen,
    /// Not a look: the count, for range checks on the host side.
    Count
};

/// One entry of the ordered looks stack. The PARAMETERS are uniforms pushed per
/// view every frame (chain::applyViewGlobals), so scrubbing one is free; the
/// KIND SEQUENCE is compositor shape and a change to it rebuilds the workspace
/// (ChainDesc::sameShape) — the same split hdr/exposure has had since phase 3.
///
/// A kind appears AT MOST ONCE in a stack. Every look's parameters live on ONE
/// process-global material (Ogre's MaterialManager is a singleton), so a second
/// instance of the same look would be handed the first one's numbers; the host
/// registry enforces the rule and the engine simply takes the first (§7 R2).
struct LookDesc {
    LookKind kind = LookKind::Desaturate;
    /// Meaning is per kind, documented on LookKind. p[0] is ALWAYS the amount,
    /// and every look is an exact identity at p[0] == 0 — that is what makes
    /// "a look at zero is byte-identical to no look" assertable.
    float p[8] = { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };

    bool operator==(const LookDesc &o) const {
        if (kind != o.kind) return false;
        for (int i = 0; i < 8; ++i) if (p[i] != o.p[i]) return false;
        return true;
    }
    bool operator!=(const LookDesc &o) const { return !(*this == o); }
};

/// HOW THE AUTOMATIC EXPOSURE'S METER LOOKS AT THE FRAME (EXPOSURE-2) — the
/// per-pixel WEIGHT the log-luminance histogram is built with. A camera's
/// metering pattern, by its own names; ignored under `tonemapFixed`, which
/// measures nothing at all.
///
/// The geometry is a circle IN PIXELS on any window shape, and the numbers
/// below are the whole definition of each pattern — they live here, beside the
/// enum, because they are physics rather than policy, and the shader derives
/// everything else from them (JahHdrMeterBuild_cs).
enum class ExposureMeterPattern : int {
    /// The whole frame, equally. The reference the other two are judged
    /// against, and what the pin's ladder always did.
    Average = 0,
    /// The classic camera default: a Gaussian on the distance from the frame
    /// centre, with a pedestal so the corners still count for something.
    CentreWeighted = 1,
    /// A centre disc of `kSpotAreaFraction` of the frame.
    Spot = 2
};

namespace meter {
/// CENTRE-WEIGHTED: the radius, in units of HALF THE FRAME HEIGHT, at which the
/// weight has fallen to half. 0.5 = half weight halfway to the top edge, which
/// integrates (over the 16:9 rectangle, with the pedestal) to 41 % of the
/// meter's sensitivity inside the central 11 % of the picture and 83 % inside
/// the inscribed full-height circle (the infinite-plane Gaussian gives 40/81;
/// the picture has corners). "Half weight" is the Gaussian term's half — with
/// the pedestal the total weight there is 0.525 of the peak.
constexpr float kCentreWeightedHalfRadius = 0.5f;
/// CENTRE-WEIGHTED: the weight a pixel infinitely far from the centre still
/// carries, as a fraction of the peak. NOT zero on purpose: a meter that
/// ignores the edges of the frame outright cannot see a window opening behind
/// the subject.
constexpr float kCentreWeightedPedestal = 0.05f;
/// SPOT: the fraction of the FRAME AREA inside the disc's half-weight radius.
/// 2.5 % is a 35 mm spot meter (1-5 % is the range real bodies offer); on 16:9
/// it is a disc 23.8 % of the frame height across.
constexpr float kSpotAreaFraction = 0.025f;
}   // namespace meter

/// THE IMAGE BLOCK (IMAGE-1): how a camera develops the picture after the
/// exposure — the world's defaults, a camera's overrides layered over them by
/// the host. All of it is UNIFORMS on the tonemap quad (HDR/FinalToneMapping):
/// moving any field rebuilds nothing. Every default is the neutral picture.
///
///  * contrast / saturation: Unreal's colour correction, in ACEScg — saturation
///    about the pixel's luminance, contrast as a power about the grey card.
///  * shadows / highlights: GAINS IN STOPS on Unreal's two luminance masks
///    (shadows below 0.09, highlights from 0.5 to 1 of post-exposure luminance).
///    They are a lift and a gain, NOT the film curve's toe and shoulder (those
///    are the film fields below).
///  * whiteTemperature (K) / whiteTint: a Bradford chromatic adaptation from
///    the white at that temperature and tint (Unreal's WhiteBalance) to the
///    white at 6500 K and tint 0, so the default is exactly the identity.
///  * vignette: 0..1 of the cos^4 lens falloff, the frame's corner at 45 degrees.
///  * film*: Unreal's filmic tonemapper (ACES-based), its five parameters at
///    Unreal's defaults. 0.18 always leaves the curve at 0.18.
/// THE FILM CURVE ON A GREY - Unreal 4.15's FilmToneMap (TonemapCommon.ush) as the fork's FinalToneMapping_ps.glsl
/// runs it (jahFilmToneMap): on a NEUTRAL input its colour terms (the RRT's glow and red modifier, the desaturations, the
/// AP0/AP1 round trip) are the identity, so this scalar is the curve a grey sees - linear film input (post-exposure) to
/// linear display output, before the sRGB encode. THE ONE C++ TRANSCRIPTION: iris::lens::filmCurve calls it (the tonemap
/// suite holds the GPU to that), and so does the reflection rest's display slope (PHOTON-7 stage 6, the stop's code).
inline double filmToneGrey(double x, double slopeIn, double toe, double shoulder, double black, double white) {
    const double slope = slopeIn > 0.01 ? slopeIn : 0.01;
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
    const double lc = std::log10(x > 1e-10 ? x : 1e-10);
    const double straight = slope * (lc + straightMatch);
    double toeC = -black + (2.0 * toeScale) / (1.0 + std::exp((-2.0 * slope / toeScale) * (lc - toeMatch)));
    double shC = (1.0 + white) -
                 (2.0 * shoulderScale) / (1.0 + std::exp((2.0 * slope / shoulderScale) * (lc - shoulderMatch)));
    toeC = lc < toeMatch ? toeC : straight;
    shC = lc > shoulderMatch ? shC : straight;
    double t = (lc - toeMatch) / (shoulderMatch - toeMatch);
    t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
    if (shoulderMatch < toeMatch) t = 1.0 - t;
    t = (3.0 - 2.0 * t) * t * t;
    return toeC + (shC - toeC) * t;
}

struct ImageGrade {
    float contrast = 1.0f;
    float saturation = 1.0f;
    float shadows = 0.0f;
    float highlights = 0.0f;
    float whiteTemperature = 6500.0f;
    float whiteTint = 0.0f;
    float vignette = 0.0f;
    float filmSlope = 0.88f;
    float filmToe = 0.3f;     // BISTRO-FIXES-3: softer than Unreal's 0.55 (iris::lens::FilmParams)
    float filmShoulder = 0.26f;
    float filmBlackClip = 0.0f;
    float filmWhiteClip = 0.04f;

    bool operator==(const ImageGrade &o) const {
        return contrast == o.contrast && saturation == o.saturation && shadows == o.shadows &&
               highlights == o.highlights && whiteTemperature == o.whiteTemperature &&
               whiteTint == o.whiteTint && vignette == o.vignette && filmSlope == o.filmSlope &&
               filmToe == o.filmToe && filmShoulder == o.filmShoulder &&
               filmBlackClip == o.filmBlackClip && filmWhiteClip == o.filmWhiteClip;
    }
    bool operator!=(const ImageGrade &o) const { return !(*this == o); }
};

/// The post-processing chain for a View (POST_CHAIN_SPEC.md).
///
/// Everything here is OFF by default, and every field is IGNORED on an offscreen
/// View: thumbnails, material previews, asset viewers and every pixel suite go
/// through createOffscreenView, and their exact colours are the contract that
/// makes them testable. Screenshots that WANT the chain ask for an on-screen
/// view or opt in explicitly.
///
/// The enable flags are per View. The TUNING (exposure, bloom threshold, AO
/// power and radius, SMAA preset) is process-global inside the backend — Ogre's
/// HDR/SSAO/SMAA materials are MaterialManager singletons — so the engine pushes
/// the values of the primary on-screen view and every other view lives with
/// them. Documented rather than hidden: it is a property of the upstream
/// implementation, not a choice.
struct PostFxDesc {
    /// Render the scene into a floating-point target and tonemap it (Unreal's
    /// filmic curve and the image block, `image` below). The prerequisite for bloom. The exposure may be
    /// MANUAL (`tonemapFixed`, the editor's default since EXPOSURE-1) or
    /// metered; this flag only says the chain exists.
    bool  hdr = false;
    /// THE CHAIN'S OWN EXPOSURE AXIS — natural log, NOT stops: the multiplier
    /// the frame is scaled by before the tonemap is `e^(exposure - 2)` times
    /// the chain's 1024, so +0.69 is one doubling. The host converts: a
    /// document exposure is in STOPS and `iris::lens::toChain` is the one door
    /// between the two (SceneMirror::applyExposure).
    float exposure = 0.0f;
    /// THE AUTO WINDOW, AND IT IS ON THE METER'S AXIS, NOT THIS ONE
    /// (EXPOSURE-1, lead review): the meter's resolve clamps the measured log
    /// luminance — the percentile-clipped mean of its histogram since
    /// EXPOSURE-2 — to `[7.5 - exposureMax, 7.5 - exposureMin]`
    /// (JahHdrMeterResolve_cs.glsl), so these two bound WHAT THE METER
    /// IS ALLOWED TO READ, not what `exposure` above may become. They are inert
    /// under manual exposure, which has no meter to bound.
    ///
    /// (There is no "set min == max for a fixed exposure" recipe any more — the
    /// doc said so for months and it was never how the pin behaved. Manual
    /// exposure is `tonemapFixed`, an explicit flag with its own code path: the
    /// luminance ladder is replaced by a clear to one constant.)
    float exposureMin = -2.5f;
    float exposureMax = 2.5f;
    /// THE METERING PATTERN and THE PERCENTILE CLIPS — the automatic exposure's
    /// meter, and nothing else (EXPOSURE-2). Read only when `hdr` and not
    /// `tonemapFixed`; they are UNIFORMS on the meter's compute jobs, so
    /// changing one never rebuilds the chain.
    ///
    /// The clips are PERCENTILES of the metered weight, darkest first: the
    /// meter averages the log-luminance of the slice between them and throws
    /// the rest away. 10 and 90 by default. What that buys is a meter a sun
    /// disc, a blown window or a specular firefly cannot pull — they are a few
    /// percent of the weight and they are cut — where a MEAN of logs (the pin's
    /// ladder) had no resistance at all. Kept ordered; a degenerate pair falls
    /// back to the whole frame rather than to no measurement.
    ExposureMeterPattern meterPattern = ExposureMeterPattern::CentreWeighted;
    float meterLowPercent = 10.0f;
    float meterHighPercent = 90.0f;
    /// Highlight bloom: a progressive mip ladder on the scene (six half-size
    /// levels from half the view down to a sixty-fourth, 13-tap downsamples and
    /// tent upsamples; BLOOM-1), so a highlight spreads by the same number of
    /// pixels across and down at every aspect. It needs `hdr`.
    bool  bloom = false;
    /// Where the bright pass starts, in the tonemapper's units. High values read
    /// as highlight bloom; low values as a haze filter.
    float bloomThreshold = 5.0f;
    /// The WIDTH of the bright pass's ramp, above `bloomThreshold`: below the
    /// threshold nothing blooms, above threshold+knee everything does, and in
    /// between the contribution ramps up. The backend's own control is two
    /// ABSOLUTE thresholds (min, full); a width is the same control expressed
    /// so it CANNOT INVERT — the backend clamps `full <= min` back up, and a
    /// second absolute row would let the panel offer a state the renderer
    /// silently refuses. 2.0 is what the caller hard-coded before this row
    /// existed, so the default is byte-identical (ADDENDUM A-6).
    float bloomKnee = 2.0f;
    /// HOW MUCH of the bloom reaches the picture — a plain multiplier on the
    /// blurred highlight term where the tonemap quad adds it, 0 to 2, and 1 is
    /// the amount this engine has always drawn (owner review R17, lane
    /// BLOOM-AMOUNT-1). It is a UNIFORM, not graph shape: dragging it must
    /// never rebuild a workspace (ChainDesc::sameShape does not read it, and
    /// tests/hdr bloom_amount asserts the generation holds still), and it is
    /// therefore the cheap dial the threshold and the knee are not — those two
    /// decide WHICH pixels bloom, this one decides how strongly the result is
    /// mixed in.
    ///
    /// 0 IS THE BLOOM-OFF PICTURE WITH THE CHAIN STILL BUILT: the term is
    /// multiplied by zero, which is exactly what the bloom-off chain adds (it
    /// feeds the tonemapper a black bloom texture), so a person can scrub to
    /// nothing and back without a rebuild at either end. Turning `bloom` off is
    /// still the way to stop PAYING for it — that is the graph change.
    ///
    /// THE ARITHMETIC IS IN THE PIXEL, NOT IN THE LADDER, deliberately: the
    /// ladder's resolve writes the square root of the bloom and the tonemapper
    /// reads it through a SQUARE (`fromSRGB`), so a scale inside the ladder
    /// would arrive squared. At the composite it is linear in the added
    /// radiance, which is what "2x the bloom" has to mean.
    float bloomAmount = 1.0f;
    /// Screen-space ambient occlusion. Adds a normals G-buffer to the main pass.
    /// A REQUEST: the chain refuses it in a view whose scene's GI mode is not Off
    /// (giCarriesOcclusion — the GI carries the occlusion; GiStatus::ssaoSuppressed).
    bool  ssao = false;
    /// AO buffer resolution, as a factor of the view (0.5 or 1.0). The tap count
    /// is fixed at 64 by the shader and is deliberately not exposed.
    float ssaoScale = 1.0f;
    /// Contrast of the occlusion term, and how far in world units it looks.
    float ssaoPower = 1.5f;
    float ssaoRadius = 2.0f;
    /// THE DITHER'S OFF SWITCH — A DIAGNOSTIC, NOT A DIAL (lane DITHER-1).
    ///
    /// The final grade quantises a floating-point picture to 8-bit display
    /// codes, and that write is DITHERED: a deterministic, screen-space,
    /// zero-mean offset of at most half a code, so a smooth gradient reads as
    /// noise whose local mean follows it instead of as a staircase of contours
    /// (the owner's "rippling in the ground plane while flying"). It is
    /// correctness, so there is no project row for it and there will not be
    /// one.
    ///
    /// THIS EXISTS SO A TEST CAN RENDER BOTH PICTURES IN ONE PROCESS. Every
    /// arm of hdr.dither is measured against the same binary with this set,
    /// and so is the --engine-selftest hash A/B. `JAHSHAKA_NO_DITHER` in the
    /// environment forces it on process-wide (read once) for the arms that
    /// cannot reach into a description — a rig shot, a selftest run.
    ///
    /// A uniform, not a graph term: setting it rebuilds no workspace.
    bool  ditherOff = false;
    /// SMAA: -1 off, 0 Low, 1 Medium, 2 High, 3 Ultra. Runs AFTER tonemapping.
    int   smaaPreset = -1;
    /// Screen-space reflections: 0 off, 1 half-resolution rays, 2 full.
    ///
    /// The one reflection source that knows about things that MOVE. A depth +
    /// normal + roughness prepass feeds a ray march against this frame's depth
    /// buffer and the PREVIOUS frame's colour; the result is handed to HlmsPbs
    /// as the pass' `ssrTexture` and upstream's own shader lerps it into the
    /// specular environment term before planar reflections and ambient
    /// (`hlms_use_ssr`). Where the march found nothing — off screen, occluded,
    /// too rough, pointing back at the camera — the confidence is zero and the
    /// pixel keeps exactly the probe/sky answer it has today.
    ///
    /// COSTS A SECOND FULL SCENE TRAVERSAL (the prepass), which is why the
    /// World Modes table never turns it on below High.
    ///
    /// KNOWN v1 LIMITATION, and it is upstream's architecture rather than ours:
    /// while SSR is on, the main scene pass shades from the prepass G-buffer
    /// (`use_prepass`), so an ALPHA-BLENDED material — Fade or Transparent,
    /// NOT Refractive, which renders in its own later pass and is unaffected —
    /// overwrites the G-buffer normal of whatever is behind it, and that
    /// surface is then shaded with the blended material's normal. Scenes that
    /// lean on translucent panes should leave SSR off until the prepass learns
    /// a visibility split.
    ///
    /// THE REFLECTION HAS NO MODE - THE HYBRID IS THE ONE SHAPE (PHOTON-7-9A, the owner's ruling; Unreal's shape): on a
    /// MONO chain that traces rays, each reflected sample is answered by THE SCREEN FIRST (this view's previous picture
    /// through the depth it was drawn at, where the screen vouches for the hit) and THE RAYS for the rest (the world
    /// through the acceleration structure, rq_reflect.comp); a STEREO chain's rays answer alone (the screen sources are
    /// not per layer); a chain without rays marches the screen alone (until NO-RAY-CUT-1). No "Both" / "Rays" / "Screen"
    /// switch exists: on the desktop glass walk the rays alone never rested after a walk (6.5 ms of tracing a frame) and
    /// walked at 2.49 % of the glossy pixels over a code of noise against the hybrid's 1.15 % - the lobe's own variance,
    /// which the screen's converged answers carry less of (ledger 3953).
    int   ssr = 0;
    /// How far a reflection ray may travel, in world units. Beyond this the
    /// march gives up and the pixel falls back to the probe/sky reflection.
    /// Scene-scale dependent: the default suits a room, not a landscape.
    float ssrMaxDistance = 25.0f;
    /// THE MARCH'S STEP COUNT, a measurement knob (SSR-EDGE-1): 0 = the quality
    /// row's (96 at Full-Res Rays, 48 at Half-Res), otherwise 8..128. It exists
    /// so a suite can change the STEP at a FIXED range — `ssrMaxDistance` alone
    /// changes both (ssr.rings' fixed-range arm). Nothing in the document writes it.
    int   ssrSteps = 0;
    /// How thick the depth buffer's surfaces are assumed to be, in world units.
    /// A depth buffer records a surface's FRONT and nothing else, so a crossing
    /// is only accepted when the ray passed within this much of it — too small
    /// and reflections drop out behind objects, too large and they smear.
    float ssrThickness = 0.5f;
    /// A straight multiplier on the composite confidence. 1.0 is physical —
    /// the reflection replaces the probe answer where the march is confident.
    float ssrIntensity = 1.0f;
    /// WHICH SAMPLE ANSWERS THE MARCH'S TWO QUESTIONS (SSR-RINGS-1) — is this
    /// a hit, and how much does the depth buffer vouch for it. The march walks
    /// the ray in FIXED STEPS of `ssrMaxDistance / steps` (0.26 m at Full-Res
    /// rays), and today both questions are answered at the coarse sample the
    /// ray happened to land on, so both carry the step's own phase. Measured on
    /// the Grand Showroom's glossy sphere (the nested crescents of SMOKE-41
    /// item 1a): the accepted hit region covers 15.3 % of the crop at a 1.04 m
    /// step, 29.5 % at the shipped 0.26 m, 34.3 % at 0.13 m and 58.4 % when the
    /// thickness tolerance is widened instead — the crescents are that region's
    /// BOUNDARY, and the resolve draws it hard because a trusted hit replaces
    /// the probe's answer outright.
    ///
    ///   0 `checker` — the shipped march, exactly. THE DEFAULT: nothing that
    ///     ships moves until a project asks for something else.
    ///   1 `refined` — a crossing is a SIGN CHANGE (the ray was in front at the
    ///     previous sample and is behind at this one), and both the thickness
    ///     test and the trust are evaluated at the bisected crossing, which is
    ///     within 1/32 of a step of the true one. The step stops deciding
    ///     WHETHER a ray hit; it only decides how finely the ray is sampled.
    ///   2 `dither` — the SHIPPED hit rule with a 4x4 ordered phase instead of
    ///     the two-value checkerboard: the minimal candidate, which changes
    ///     what a ray SAMPLES and nothing about what counts as a hit, so the
    ///     reflection covers exactly what it covers today and only the
    ///     boundary's 2 px stipple is spread over sixteen phases for the
    ///     resolve's 3x3 gather to average. It trades an edge for a grain.
    ///
    /// It is a UNIFORM and not a graph change (ChainDesc::sameShape).
    int   ssrMarchPhase = 0;
    /// THE REFLECTION ROUGHNESS CUTOFF, in PERCEPTUAL roughness (PHOTON_SPEC
    /// §7 R5; owner, ledger §426). ONE number for BOTH sources of a per-pixel
    /// reflection (lane SSR-3): below it the reflection is MARCHED in screen
    /// space and, on a ray-capable machine, TRACED for whatever the screen
    /// cannot see; above it the reflection probes' own prefiltered photograph
    /// answers. A PER-PROJECT dial and not a renderer constant: above the cutoff
    /// a reflection is a wide lobe and the probe's photograph is a good enough
    /// integral of it, so a ray or a march per pixel buys a blurrier answer for
    /// the same cost — but WHERE that point falls is a property of the content,
    /// not of the renderer.
    ///
    /// THERE USED TO BE A SECOND ONE. `ssrRoughnessCutoff` (0.35) gated the
    /// march, nothing in the document ever wrote it, and it was compared
    /// against a G-buffer channel the march decoded with the pre-fork 16d8e29d4 (was 0043)
    /// range and never square-rooted — so the band the frame applied was
    /// perceptual 0.581, not 0.35 and not anything a user could read. It is
    /// deleted; the march reads this field (OgreChain::updateSsr).
    ///
    /// THE TRANSITION IS FEATHERED, not a step, and BOTH SOURCES FADE OVER THE
    /// SAME 0.1 (`kRayReflectFeather`, one constant in this header since
    /// lane SSR-3 so the two cannot drift). The ray's confidence runs from full
    /// at `cutoff - feather` to zero at `cutoff + feather`; the march's
    /// roughness ramp runs from full at `cutoff - feather` to zero AT `cutoff`,
    /// because the march is skipped above the cutoff and has nothing there to
    /// fade. So a surface more than a feather below the cutoff keeps its screen
    /// reflection whole, and one whose roughness varies across the cutoff — a
    /// scratched floor — hands the pixel over through the existing confidence
    /// composite with no seam. A constant, deliberately not a second dial.
    ///
    /// It is a UNIFORM, not a graph change: moving it must not rebuild a
    /// workspace (see ChainDesc::sameShape).
    float reflectionRoughnessCutoff = 0.4f;
    /// Re-render refractive materials (alphaMode Refractive) in a second pass
    /// that samples the opaque result. Costs nothing when no material is.
    bool  refractions = false;

    /// THE DISTORTION PASS (POST_LOOKS_SPEC.md §5.3). Objects whose material's
    /// shading model is Distortion render into their own RG target through
    /// their own render queue, and a quad then warps the scene image by what
    /// they wrote. Like `refractions` this is resolved by the HOST from an
    /// auto/off row against whether the scene actually holds such a material,
    /// so a scene without one pays nothing: no target, no passes, and a graph
    /// identical to one built before this existed.
    ///
    /// It runs in LINEAR HDR, before the SSR history copy, SSAO and the
    /// tonemap: heat haze is a phenomenon in front of the LENS, so bloom and
    /// exposure should see the warped radiance and the anti-aliasing should
    /// clean the warped edges.
    bool  distortion = false;
    /// A global multiplier on every distortion material's own strength. 0 is
    /// inert and produces a byte-identical frame (the warp offset is exactly
    /// zero, so the compose quad's fetch lands on the source texel).
    float distortionStrength = 1.0f;

    /// THE SECONDARY-SURFACE TONEMAP (owner report 2026-09-07, fix wave item 6).
    ///
    /// The problem: everything that is NOT the main viewport — thumbnails,
    /// material and asset previews, screenshots, the PiP inset — renders the
    /// scene's raw linear radiance straight into an 8-bit target. A world the
    /// viewport grades filmically therefore photographs BLOWN OUT: a bright
    /// light clips to flat white in the thumbnail while the viewport, which
    /// tonemaps, shows the highlight rolled off.
    ///
    /// The fix cannot simply be `hdr = true`, because Ogre's HDR node is
    /// AUTO-exposed: it reduces the frame's luminance through four downscales
    /// into a 1x1 texture and then ADAPTS towards it over frames, weighted by
    /// wall-clock time. On a surface that renders two frames and reads them
    /// back, "the exposure it happened to reach" is not a value anybody can
    /// assert, and the material parameters it rides on are PROCESS-GLOBAL.
    ///
    /// `tonemapFixed` is the deterministic form: the same filmic curve, the
    /// same node, but the whole luminance-reduction chain is REPLACED by a
    /// per-frame clear of the 1x1 exposure texture to a constant derived from
    /// `exposure` alone. Same picture for the same scene, every time, on any
    /// machine — and cheaper than the auto path (five quads and four textures
    /// fewer). Ignored unless `hdr`.
    bool  tonemapFixed = false;

    /// THE FIXED GRADE'S EXPOSURE, AS THE TONEMAPPER'S OWN MULTIPLIER, when the
    /// host already knows it (SS1, 2026-09-13). 0 — the default — means "derive
    /// it from `exposure`", which is the grey-card constant `tonemapFixed` has
    /// always used (OgreChain's fixedInverseLuminance: it substitutes a 0.18
    /// mid-grey for the measurement the auto chain would have made).
    ///
    /// WHY A SECOND WAY TO SAY IT. The grey card is right for a THUMBNAIL, which
    /// is a photograph of content nobody has measured. It is about a stop off in
    /// a bright room, and it is wrong for a SCREENSHOT OF THE EDITOR, which has
    /// a measured exposure a few centimetres away: the on-screen view converged
    /// on one over the last second. `View::measuredExposureScale()` reads that
    /// number back, and this field carries it into the one-shot offscreen view,
    /// which can never converge on its own (it lives two frames). Right AND
    /// deterministic — the same picture every time, because it is a constant.
    ///
    /// Ignored unless `hdr && tonemapFixed`. Never negative; 0 is "unknown".
    float exposureScale = 0.0f;

    /// THE LOOKS STACK (POST_LOOKS_SPEC.md §4), in FRAME ORDER: entry 0 runs
    /// first, on the tonemapped and anti-aliased image, and the last one writes
    /// the window. Empty is the default and costs exactly nothing — the stage
    /// is absent from the compositor graph, not disabled inside it, so a scene
    /// with no looks renders byte-for-byte what it rendered before this
    /// existed.
    ///
    /// The KIND SEQUENCE is shape (adding, removing or reordering rebuilds the
    /// workspace); the parameters are per-view uniforms and are free to scrub.
    /// A kind must appear at most once — see LookDesc.
    std::vector<LookDesc> looks;

    /// THE HIERARCHICAL DEPTH PYRAMID (SPECS/NANITE_SPEC.md §4.3) — PHOTON
    /// SHARED INFRASTRUCTURE, not a picture effect.
    ///
    /// Builds a closest-depth mip chain of the scene depth once per frame, right
    /// after the opaque pass: mip 0 is the depth buffer, and each level after it
    /// holds the FARTHEST (or, on request, the closest — see `hzbFarthest`) depth
    /// of its footprint in the level above, down to 1x1.
    /// A stackless screen-space trace walks it instead of stepping pixel by
    /// pixel — Epic measure the compaction that rides on it at up to a 50%
    /// tracing speedup — and nothing in this engine reads it YET.
    ///
    /// So it is OFF by default and costs exactly nothing while off: no texture,
    /// no pass, no shader. UNLIKE every flag above it, it is NOT cleared on an
    /// offscreen view — an offscreen capture is where a trace will be measured,
    /// and the pyramid changes no pixel of the picture either way.
    bool  hzb = false;
    /// WHICH DEPTH EACH LEVEL KEEPS, and the two readers of a pyramid want
    /// opposite answers (ATOM-SUBSTRATE-1, 2026-09-22):
    ///
    ///   true  (the default, and the only build anything asks for today) — the
    ///         FARTHEST depth of a footprint, which is the ONLY direction an
    ///         occlusion cull can be conservative with: "my nearest point is
    ///         behind this level" then proves every pixel under it is already
    ///         covered. With the closest depth, a texel that is half wall and
    ///         half sky reports the wall and an object seen through the sky half
    ///         is culled — geometry lost.
    ///   false — the CLOSEST depth, what a stackless screen-space trace wants so
    ///         it can skip a region it cannot have hit yet. Photon's gather asks
    ///         for it when it lands; nothing reads it today.
    bool  hzbFarthest = true;

    /// THE offscreen opt-in. Offscreen Views ignore every flag above unless this
    /// is set, because their exact colours are what thumbnails, previews and the
    /// pixel suites assert. Two callers set it, both deliberately: a screenshot
    /// that asked to look like the viewport (`screenshot({postFx:true})`), and
    /// the engine suite, which is the only way to pixel-test the chain at all.
    bool  allowOffscreen = false;

    /// THE RADIANCE READBACK (HDR-READBACK-1). The view keeps its scene result
    /// in a FLOAT target (RGBA16F) beside the 8-bit one it presents, so
    /// `View::readPixelsHdr` can return the scene-referred value a closed form
    /// is stated in. Honoured on EVERY view, offscreen ones included and
    /// whatever `allowOffscreen` says — like `refractions` it is not a post
    /// effect but a property of what the view keeps: a passthrough view that
    /// asks takes the chain's shape (the scene into the float target, the ONE
    /// composite quad into the 8-bit one) and nothing else. No second
    /// composite pass exists: the float target IS the chain's scene target,
    /// read where the tonemap would read it. A view that does not ask builds
    /// the graph it built before this flag existed. A view that DOES ask is not
    /// a byte-identical display of the one that does not: with `ssr > 0` its
    /// SSR colour history is float too, so its reflections carry the UNCLIPPED
    /// radiance (what an `hdr` chain has always reflected) where an 8-bit chain
    /// reflects the clipped one — readPixels of the two differ wherever the
    /// reflected radiance exceeds 1.
    bool  hdrReadback = false;
    /// THE IMAGE BLOCK (IMAGE-1): contrast, saturation, shadows/highlights, white
    /// balance, vignette and the film curve. Read only under `hdr` (it lives in
    /// the tonemap quad); uniforms, never graph shape.
    ImageGrade image;

    bool operator==(const PostFxDesc &o) const {
        return hdr == o.hdr && image == o.image && exposure == o.exposure && exposureMin == o.exposureMin &&
               exposureMax == o.exposureMax &&
               meterPattern == o.meterPattern &&
               meterLowPercent == o.meterLowPercent &&
               meterHighPercent == o.meterHighPercent && bloom == o.bloom &&
               bloomThreshold == o.bloomThreshold && bloomKnee == o.bloomKnee &&
               bloomAmount == o.bloomAmount &&
               ssao == o.ssao &&
               ssaoScale == o.ssaoScale && ssaoPower == o.ssaoPower &&
               ssaoRadius == o.ssaoRadius && ditherOff == o.ditherOff &&
               smaaPreset == o.smaaPreset &&
               ssr == o.ssr &&
               ssrMaxDistance == o.ssrMaxDistance && ssrSteps == o.ssrSteps &&
               ssrMarchPhase == o.ssrMarchPhase &&
               ssrThickness == o.ssrThickness &&
               reflectionRoughnessCutoff == o.reflectionRoughnessCutoff &&
               ssrIntensity == o.ssrIntensity &&
               refractions == o.refractions &&
               distortion == o.distortion &&
               distortionStrength == o.distortionStrength &&
               tonemapFixed == o.tonemapFixed &&
               exposureScale == o.exposureScale &&
               looks == o.looks && hzb == o.hzb && hzbFarthest == o.hzbFarthest &&
               allowOffscreen == o.allowOffscreen && hdrReadback == o.hdrReadback;
    }
    bool operator!=(const PostFxDesc &o) const { return !(*this == o); }
};

/// ---------------------------------------------------------------------------
/// THE VR VIEW POLICY — the project's post chain, minus what the stereo chain
/// does not run per eye yet (lane EYE-GRADE-1, 2026-09-18; LAYERED-STEREO-1).
///
/// WHAT IT IS FOR. The picture in the headset is a VIEW OF THE PROJECT'S SCENE
/// and is graded by the project, exactly like the desktop's: the exposure mode
/// and its stops, the meter's pattern and its percentile clips, the looks
/// stack, the image block, the reflection row. Until this existed the session
/// wrote its own PostFxDesc BY HAND, so the wearer got this struct's DEFAULTS
/// whatever the author had chosen (spikes/smoke-50/f5b: the whole World panel
/// was inert in the headset). `SceneMirror` pushes the project's description
/// into the session's view like into every other view of that scene, and THIS
/// is applied on top of it — once, here, and nowhere else.
///
/// THE STEREO CHAIN IS LAYERED (LAYERED-STEREO-1): each eye is its own layer of
/// a two-layer array at the same origin, every quad of the chain runs per
/// layer (multiview) and reads its screen-sized inputs at its own eye. So a
/// look, a vignette or a meter pattern measured from the frame's centre is
/// measured from EACH eye's centre, and the looks stack and the image block
/// ride whole. What is still off is what has no per-eye form in this engine:
///
///   * allowOffscreen — the eye pair is offscreen only because the runtime owns
///     the display. It is the picture the wearer is standing in, and it keeps
///     the chain (PostFxDesc::allowOffscreen).
///   * bloom — the ladder is screen-sized and its programs have the multiview
///     arm (BLOOM-1), but the tonemap quad's multiview arm still reads its bloom
///     input as one 2D texture; that read, per layer, is the work
///     (chain::build refuses the ladder in stereo until it is done).
///   * ssao, smaaPreset, hzb — their passes and media are not per layer (the
///     half-resolution SSAO targets, the SMAA lookup tables' passes, the
///     pyramid's compute), and a layered chain refuses a pass that reads a
///     layered texture into a single-layer target (chain::applyStereo).
///   * refractions, distortion — screen-space READS of a scene copy at an
///     offset, whose copy and composite passes are not per layer (and the pin's
///     refraction piece has no multiview arm, fork F4's stated limitation).
///
/// AND WHAT IS DELIBERATELY *NOT* HERE. MSAA is not a PostFxDesc field: the
/// session pins `setSampleCount(1)` itself (HDR + MSAA segfaults this driver,
/// OgreChain's own note) and the mirror never pushes a count into an offscreen
/// view. The exposure (mode, stops, window), the meter's pattern and clips, the
/// looks, the image block, the reflection row and the grade follow the project,
/// whole.
///
/// `ssrOverride` is the ONE session-scoped override: -1 means "the project's
/// row", 0/1/2 are `vr.begin({reflections:n})`'s measurement arm (VrConfig::ssr).
inline void applyVrViewPolicy(PostFxDesc &fx, int ssrOverride = -1) {
    fx.allowOffscreen = true;
    fx.bloom          = false;
    fx.ssao           = false;
    fx.smaaPreset     = -1;
    fx.hzb            = false;
    fx.hzbFarthest    = true;
    fx.refractions    = false;
    fx.distortion     = false;
    if (ssrOverride >= 0) fx.ssr = ssrOverride;
}

}}  // namespace jahshaka::engine
