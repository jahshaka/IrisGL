#pragma once
// THE ENGINE's SKY TYPES - engine-neutral values, one subject (ENGINE-HEADERS-1: split from Types.h by
// declaration name). NOTHING here may reference Ogre, Qt or GL; a new subject is a new header, never an append here.
#include <string>
#include "jahshaka/engine/Ids.h"
#include "jahshaka/engine/Math.h"

namespace jahshaka { namespace engine {
enum class SkyMode { NoSky, Equirectangular, Cubemap, Atmosphere };   // 'None' collides with X11's macro

/// THE PLANET'S ATMOSPHERE, drawn by the engine itself (SKY-ATMOSPHERE-1).
///
/// A physical model of the air around a planet (Hillaire, "A Scalable and
/// Production Ready Sky and Atmosphere Rendering Technique", EGSR 2020 — what
/// Unreal's Sky Atmosphere is): a planet of radius `planetRadiusKm` inside a
/// shell of air `atmosphereHeightKm` deep, Rayleigh and Mie scattering with
/// exponential density profiles and an ozone layer that only absorbs. ONE model
/// answers everything the air does to light in the scene: the sky's pixels
/// (with the planet under the horizon — a sharp horizon over a darker ground
/// band), the sun's colour (Scene::atmosphereSunTint: the transmittance towards
/// the sun), the environment the Sky Light captures, the cloud sheet's air and
/// the AERIAL PERSPECTIVE on every lit pixel (the light the air scatters into
/// the view ray and the transmittance along it, by distance). The engine holds
/// it in four look-up tables it rebuilds only when a dial or the sun changes —
/// never per frame (Scene::atmosphereStatus counts the rebuilds).
///
/// THE SUN IS THE HOST'S LIGHT. `sunDir` and `sunIlluminance` are pushed from
/// the scene's sun light; the sky is LIT by it, so a brighter sun makes a
/// brighter sky and a sun of zero illuminance a black one (the mirror pushes
/// zero for a scene with no sun light: the night). A DIRECT engine caller that
/// says nothing gets the defaults: a white sun of intensity 1 at the zenith.
/// The sun disc is SunDisc's (one mechanism over every sky type).
///
/// THE DIALS MEAN PHYSICAL THINGS, and 1 is the paper's clear Earth for each
/// scale (Rayleigh scattering (5.802, 13.558, 33.1)e-6 per metre over an 8 km
/// scale height; Mie scattering 3.996e-6, extinction 4.44e-6 per metre over
/// 1.2 km with g 0.8; ozone absorption (0.650, 1.881, 0.085)e-6 per metre in a
/// tent 30 km wide centred at 25 km). They MUST stay equal to
/// `iris::SkyRealistic::defaults()` in the document (the boundary forbids the
/// include, so they are written twice).
struct AtmosphereSky {
    /// THE HAZE: the aerosol (Mie) density as a multiple of the paper's clear
    /// air. 1 = the reference's very clear air (an aerosol optical depth of
    /// 0.005 over the whole column), 0 = no aerosol at all (a purely molecular
    /// sky), 10 = THE DEFAULT, a clean ordinary clear day (optical depth 0.053:
    /// 4.44e-3 per km x 1.2 km scale height x 10), ~50 = hazy (0.27), 100 =
    /// thick haze (0.53). The aerosol is grey: it dims and whitens, the
    /// molecules redden. It is the SKY's haze (owner 2026-09-30: "the haze
    /// should mainly be the horizon, not the scene; fog is for the scene"): the
    /// sky's pixels, the horizon's whiteness, the glow round the sun, the sun's
    /// colour and the Sky Light's capture. It reaches a scene surface only
    /// through `aerialScale`, which is off by default.
    float sunHaze   = 10.0f;
    /// The molecular (Rayleigh) density as a multiple of Earth's: the sky's
    /// blue. 1 = Earth.
    float rayleighScale = 1.0f;
    /// The ozone layer's absorption on (Earth) or off. Its visible effect is
    /// the blue of the sky with the sun low (Chappuis band).
    bool  ozone     = true;
    /// The planet's surface albedo (Lambert, grey), seen under the horizon and
    /// part of the light the sky scatters back down.
    float groundAlbedo = 0.3f;
    /// THE AERIAL PERSPECTIVE ON THE SCENE, a distance scale (Unreal's
    /// "aerial perspective view distance scale"): 0 = THE DEFAULT, no air
    /// between a surface and the camera (scene haze is the World fog's job,
    /// owner 2026-09-30), 1 = the real air of this atmosphere, in between = the
    /// air a surface that much closer would be seen through. Changes no sky
    /// pixel, so it is ABSENT from the comparison below and applied on its own
    /// (Scene::setSky).
    float aerialScale = 0.0f;
    /// THE SKY'S BRIGHTNESS (Unreal's Sky Atmosphere "sky luminance factor"):
    /// scales the light the air SCATTERS — the sky's pixels, the environment
    /// the Sky Light and every reflection capture, and the aerial perspective's
    /// in-scatter — and NOT the sun's direct light, its colour or its
    /// transmittance. 1 = the physical sky (the default), 0..10.
    float skyBrightness = 1.0f;
    /// The planet, fixed at Earth (not dials yet): the bottom radius and the
    /// depth of the air above it.
    float planetRadiusKm = 6360.0f;
    float atmosphereHeightKm = 100.0f;
    /// Unit vector FROM the scene TOWARDS the sun, in world space — the scene's
    /// sun light's direction, reversed, pushed by the host. With `hasSun` false
    /// the sun stands at the zenith.
    float sunDir[3] = { 0.0f, 1.0f, 0.0f };
    bool  hasSun    = false;
    /// THE SUN'S ILLUMINANCE in the renderer's units — the sun light's linear
    /// colour x its intensity x pi (HlmsPbs lights with intensity x pi and
    /// divides the diffuse by pi, so this is what a white Lambert card facing
    /// the sun at NOON receives), untinted by the air: the engine divides by the
    /// zenith transmittance to get the light at the top of the atmosphere, so
    /// the picked colour stays the noon colour (atmosphereSunTint's contract).
    /// Zero: the sky is black. The default is a white sun of intensity 1 (pi).
    Colour sunIlluminance { 3.14159265f, 3.14159265f, 3.14159265f, 1.0f };

    /// "Is this the same SKY?" — every field that moves a sky pixel (a change
    /// tears nothing down any more, but it re-captures the environment the Sky
    /// Light and every reflection read).
    bool operator==(const AtmosphereSky &o) const {
        return sunHaze == o.sunHaze && rayleighScale == o.rayleighScale && ozone == o.ozone &&
               groundAlbedo == o.groundAlbedo && planetRadiusKm == o.planetRadiusKm &&
               atmosphereHeightKm == o.atmosphereHeightKm && skyBrightness == o.skyBrightness &&
               hasSun == o.hasSun &&
               sunDir[0] == o.sunDir[0] && sunDir[1] == o.sunDir[1] && sunDir[2] == o.sunDir[2] &&
               sunIlluminance.r == o.sunIlluminance.r && sunIlluminance.g == o.sunIlluminance.g &&
               sunIlluminance.b == o.sunIlluminance.b;
    }
    bool operator!=(const AtmosphereSky &o) const { return !(*this == o); }
};

/// THE ATMOSPHERE'S LOOK-UP TABLES, counted (Scene::atmosphereStatus). The
/// transmittance and multiple-scattering tables depend on the model's dials
/// alone; the sky view and the aerial-perspective volume on the dials and the
/// sun. None of them is rebuilt on a still frame.
struct AtmosphereStatus {
    /// The analytic atmosphere is the scene's sky.
    bool     on = false;
    unsigned transmittanceBuilds = 0;
    unsigned multiScatterBuilds = 0;
    unsigned skyViewBuilds = 0;
    unsigned aerialBuilds = 0;
    /// Table rebuilds DEFERRED (SHADER-SYSTEM-1 7a): an asynchronous scene's table job met its permutation still
    /// building in the background; the rebuild was retried the next frame and the sky's capture waited for it.
    unsigned deferredUpdates = 0;
    /// The sun at the top of the air, in the renderer's units (what the LUTs'
    /// unit-illuminance radiance is multiplied by, times skyBrightness).
    float    topIlluminance[3] = { 0.0f, 0.0f, 0.0f };
    /// The observer the view-dependent tables were built for (metres over
    /// y = 0, the scene's driving camera, banded) and how many band changes
    /// have rebuilt them.
    float    observerAltitudeM = 2.0f;
    unsigned observerRebuilds = 0;
    /// The altitude the ENVIRONMENT (the sky capture the Sky Light reads) is
    /// photographed from (metres): the ground's 2 m under 71 m, else a point of the
    /// octave lattice 50 m x 2^n. At rest both altitudes are functions of the
    /// camera's (REOPEN-SKY-1).
    float    environmentObserverM = 2.0f;
    /// The aerial-perspective volume is bound to the scene's colour passes
    /// (aerialScale > 0 or the World fog on): one pass texture.
    bool     aerialBound = false;
};

/// A SCENE'S WHOLE SKY, as one value (ENGINEERING_DEBT_SPEC.md item 4).
///
/// It replaces the three entry points this boundary used to have —
/// `setSky(SkyMode, TextureId)`, `setSkyCubemap(faces[6])` and
/// `setSkyReflection(faces[6])` — which between them made the host write the
/// dispatch, the ordering ("sky first, then reflections") and the
/// already-pushed bookkeeping that the backend is in a better position to own.
/// Scene::setSky(const SkyDesc &) takes the whole description and is
/// IDEMPOTENT: pushing a description equal to the live one does nothing at all
/// — no re-upload, no cube rebuild, no IBL reconvolution — so a host may push
/// it every frame and let `operator==` be the change guard.
///
/// WHAT IS NOT HERE. The flat "single colour sky" is not a sky at all in the
/// backend: it is the VIEW's background (View::setBackground), and several
/// views of one scene legitimately clear to different colours (an opaque
/// editor, a transparent thumbnail). It stays a view property on purpose.
/// The gradient and Preetham "realistic" skies are not here either: both are
/// CPU bakes of DOCUMENT parameters into an equirectangular image, and the
/// image is what this boundary consumes — see SceneMirror::buildSky, which
/// owns those bakes because they need an image decoder and this layer has
/// none (no Qt, no image formats — Types.h's first line).
/// THE SUN DISC (SKY_LIGHT_SPEC.md §3) — the bright disc drawn in the sky where
/// the scene's sun light points, as part of the sky description.
///
/// It is the SUN'S, not the sky's, and there is exactly ONE of it. It used to
/// be a term baked into the analytic sky's texels, which made it a property of
/// one sky type, at the bake's resolution, in a place no capture mask could
/// exclude it from. Here it is a quad the backend draws over WHATEVER sky is
/// bound — a colour, a gradient, a Preetham bake, an equirect photograph — with
/// its own visibility channel, so "not in the reflection probes" is expressible.
///
/// `colour` is the disc's RADIANCE, already multiplied by whatever the host
/// wants (the sun light's colour, its intensity, an overdrive constant); the
/// backend writes it additively and does not scale it. The disc is meant to
/// CLIP in LDR and to bloom under the HDR chain — it is the sun.
struct SunDisc {
    bool  enabled = false;
    /// Unit vector FROM the scene TOWARDS the sun, in world space.
    float dir[3] = { 0.0f, 1.0f, 0.0f };
    /// The disc's angular DIAMETER in degrees (the real sun is 0.53).
    float angularDiameterDeg = 0.53f;
    Colour colour { 1.0f, 1.0f, 1.0f, 1.0f };
    /// Do reflection-probe captures contain it? Off by default: the sun's
    /// energy already reaches glossy surfaces through the directional light's
    /// own specular highlight, so a captured disc is a second sun.
    bool  inProbes = false;

    bool operator==(const SunDisc &o) const {
        if (enabled != o.enabled) return false;
        if (!enabled) return true;   // a disabled disc has no other state
        return dir[0] == o.dir[0] && dir[1] == o.dir[1] && dir[2] == o.dir[2] &&
               angularDiameterDeg == o.angularDiameterDeg &&
               colour.r == o.colour.r && colour.g == o.colour.g &&
               colour.b == o.colour.b && inProbes == o.inProbes;
    }
    bool operator!=(const SunDisc &o) const { return !(*this == o); }
};

/// THE 2D CLOUD LAYER (CLOUDS-2D-1; SPECS/CLOUDS_ASSESSMENT.md option C0 —
/// HDRP's Cloud Layer as the model). One sheet of cloud at `altitude` metres,
/// drawn as a screen quad at render queue 0 OVER the bound sky (the sun disc's
/// shape) and therefore inside the sky capture's range: the ambient SH, the
/// reflection cube and every Photon estimator that reads the one environment
/// see it with no further work. A top-down TRANSMITTANCE map of the same field
/// shades the first directional light's term in HlmsPbs (the listener's extra
/// pass texture — Terra's shadow pattern).
///
/// A DISABLED LAYER IS NO LAYER: nothing is created, no pass property is set,
/// no Hlms shader changes — every picture of a scene without one is the picture
/// it was before the layer existed (the 0048 pattern).
///
/// WHAT IT COSTS, MEASURED (spikes/clouds-2d-1/, 2026-09-23, Debug, Xvfb, one
/// process, paired arms, a six-primitive scene whose still frame is ~0.6 ms GPU —
/// so carry the ABSOLUTE numbers, not a percentage of that frame):
///   * the layer's own draw (the quad, the clouded disc, the ground-shadow
///     lookup), coverage 0.5 against the layer off, clocks locked 2100-2550 MHz:
///     +0.06 / +0.15 / +0.07 ms GPU per frame over three pairs (median frames);
///     the UI-thread CPU difference was inside its noise (±0.4 ms);
///   * ONE environment re-capture, downstream included: 26.4 ms GPU and 45.7 ms
///     UI-thread CPU in total — the capture is small; the new SH re-stales the
///     whole probe grid (re-captured at the budget's one probe per frame) and
///     re-sweeps the irradiance field. On a scene with more probes it is more.
///     The scroll therefore re-captures every 600 drawn frames (OgreSky.cpp).
///
/// A HAZARD, BY DESIGN: the sheet is SUN-LIT, so the sun is in its look — with a
/// layer on, a COLOUR or GRADIENT sky (which never depended on the sun) re-captures
/// the environment and re-stales the probe grid on every frame the sun moves (a
/// drag, a keyframed sun), a cost class those skies never paid; the realistic sky
/// always did. The probe budget caps the probe half at one probe per frame.
struct CloudLayerDesc {
    bool      enabled = false;
    /// 0 (clear) .. 1 (overcast): the fraction of the field that is cloud.
    float     coverage = 0.5f;
    /// Optical thickness multiplier, 0 .. 4.
    float     density = 1.0f;
    /// The layer's altitude in metres (the curved-earth projection and the
    /// sideways throw of the ground shadow under a low sun).
    float     altitude = 2000.0f;
    /// 0 .. 1: how much of the layer's transmittance reaches the sun's light
    /// on the ground (1 = all of it).
    float     shadow = 1.0f;
    /// Wind, world metres per second of SCENE time along x and z. The scroll is
    /// the scene's own time (its document's steps, ONE-FRAME-1), so a paused
    /// scene holds still and a scripted frame is reproducible.
    float     wind[2] = { 0.0f, 0.0f };
    /// An optional weather map (its red channel scales the coverage over one
    /// tile of the layer); 0 = none. Sampled as data, never sRGB-decoded.
    TextureId weatherMap = 0;
    /// THE SUN THAT LIGHTS THE LAYER: the direction TOWARDS it and its
    /// irradiance in the renderer's units (the first directional light's
    /// colour x tint x intensity x pi — what a white Lambert plate facing it
    /// reflects is that over pi: the light's power is intensity x pi and
    /// HlmsPbs divides the diffuse by pi, so the plate shows `intensity`;
    /// cloud_2d.energy E0 measures it). No sun = lit by the sky alone.
    bool      hasSun = false;
    float     sunDir[3] = { 0.0f, 1.0f, 0.0f };
    Colour    sunIrradiance { 0.0f, 0.0f, 0.0f, 1.0f };

    /// The inputs of the baked density / transmittance field. A change here
    /// re-bakes it; anything else is a uniform write.
    bool sameField(const CloudLayerDesc &o) const {
        if (enabled != o.enabled) return false;
        if (!enabled) return true;
        return coverage == o.coverage && density == o.density && weatherMap == o.weatherMap;
    }
    /// Everything the CAPTURED sky depends on (the field, the look and the
    /// light) — a change re-captures the environment. The wind and the shadow
    /// strength are not in it: neither moves a sky pixel at a given time.
    bool sameLook(const CloudLayerDesc &o) const {
        if (!sameField(o)) return false;
        if (!enabled) return true;
        return altitude == o.altitude && hasSun == o.hasSun &&
               sunDir[0] == o.sunDir[0] && sunDir[1] == o.sunDir[1] && sunDir[2] == o.sunDir[2] &&
               sunIrradiance.r == o.sunIrradiance.r && sunIrradiance.g == o.sunIrradiance.g &&
               sunIrradiance.b == o.sunIrradiance.b;
    }
    bool operator==(const CloudLayerDesc &o) const {
        if (!sameLook(o)) return false;
        if (!enabled) return true;
        return shadow == o.shadow && wind[0] == o.wind[0] && wind[1] == o.wind[1];
    }
    bool operator!=(const CloudLayerDesc &o) const { return !(*this == o); }
};

/// What the cloud layer is DOING in a scene (world.clouds().live).
/// THE CLOUD LAYER'S TEXTURE BUDGET (CLOUD-BAKE-MEMORY-1), the same at every tier: one
/// field over the 64 km tile, 2048^2 R16F (~31 m a texel) with its full mip chain —
/// 2 B x (4^12 - 1) / 3 = 11,184,810 B (10.67 MiB). The layer DRAWS the field (its
/// optical depth is the deck's look overhead, read with the pixel's own gradients), so a
/// coarser far field is a coarser sky, not a cheaper one: the mips are the far field. The
/// bake's three 512^2 intermediates live only inside the workspace that makes them.
/// OgreSky.cpp builds the field from this number (a static_assert ties its size to it);
/// cloud_2d.budget holds the live textures to it.
constexpr unsigned long long kCloudFieldBytes = 11184810ull;

struct CloudStatus {
    /// The layer quad exists and is visible (enabled, over a sky it may draw on).
    bool     drawn = false;
    /// Why not, when not: "off", "noSky", "media". (The engine never hears of a
    /// layer the mirror keeps off an image sky; world.clouds composes "imageSky"
    /// from the document for that case.)
    std::string reason;
    /// Captures of the sky environment the layer has asked for: on a change
    /// (`changeCaptures`) and while it scrolls (`scrollCaptures`), over the
    /// scene's life.
    unsigned changeCaptures = 0;
    unsigned scrollCaptures = 0;
    /// Frames the layer's clock advanced (one per drawn frame with wind and a
    /// non-zero scene time, never more).
    unsigned clockTicks = 0;
    /// Frames between two scroll captures (0 = no scroll captures: still wind).
    unsigned capturePeriodFrames = 0;
    /// How many times the density / transmittance field has been baked.
    unsigned fieldBakes = 0;
    /// The layer's scroll offset in metres (x, z) at the last frame drawn.
    float    scroll[2] = { 0.0f, 0.0f };
};

/// THE EDITOR'S GRID, DRAWN BY A SHADER (GRID-2, owner review R5: "the grid is a
/// LINE mesh — MSAA/SMAA treat 1 px lines poorly and Vulkan line width is fixed
/// at 1; the correct fix is a shader-drawn grid on the ground plane").
///
/// One screen quad per scene (the sun disc's mechanism): its fragment program
/// intersects the camera ray with the grid PLANE through the world origin,
/// draws the lines ANALYTICALLY from the plane point — anti-aliased through the
/// pixel footprint, `thicknessPx` wide on screen wherever it stands — and writes
/// the plane point's own depth, so the scene's geometry covers it exactly where
/// it stands on the plane and a floor AT the plane never z-fights it (the depth
/// is analytic; a one-part-in-ten-thousand bias toward the camera settles the
/// coplanar case, which Vulkan honours here because this is a polygon, not a
/// line). It is INFINITE: there is no extent and no line count; the minor
/// lines fade out where a cell is narrower than a few pixels and everything
/// fades to nothing by `fadeDistance` metres along the ray. A HELPER: the
/// probes, the shadow node, the planar reflectors, the Player and the Scene
/// grade never draw it.
///
/// IDEMPOTENT like setSky: a description equal to the live one does nothing.
struct GridDesc {
    bool  enabled = false;
    /// The plane through the world origin: Floor is y = 0 (the ground), FrontXY
    /// is z = 0, SideYZ is x = 0 — what the three axis views ask for.
    enum class Plane : int { Floor = 0, FrontXY = 1, SideYZ = 2 };
    Plane plane = Plane::Floor;
    /// The cell, in world units (the translate snap size doubles as it).
    float spacing = 1.0f;
    /// Every Nth line is a major line, in `majorColour`.
    int   majorEvery = 10;
    /// Alpha in each colour is the line's own opacity at full strength.
    Colour minorColour { 0.46f, 0.48f, 0.52f, 0.28f };
    Colour majorColour { 0.62f, 0.64f, 0.68f, 0.50f };
    /// Line width on screen, in pixels, at any distance.
    float thicknessPx = 1.0f;
    /// Where the whole grid has faded to nothing, in metres along the ray.
    float fadeDistance = 100.0f;

    bool operator==(const GridDesc &o) const {
        if (enabled != o.enabled) return false;
        if (!enabled) return true;
        return plane == o.plane && spacing == o.spacing && majorEvery == o.majorEvery &&
               minorColour.r == o.minorColour.r && minorColour.g == o.minorColour.g &&
               minorColour.b == o.minorColour.b && minorColour.a == o.minorColour.a &&
               majorColour.r == o.majorColour.r && majorColour.g == o.majorColour.g &&
               majorColour.b == o.majorColour.b && majorColour.a == o.majorColour.a &&
               thicknessPx == o.thicknessPx && fadeDistance == o.fadeDistance;
    }
    bool operator!=(const GridDesc &o) const { return !(*this == o); }
};

struct SkyDesc {
    /// NoSky removes the sky (the View's background shows through).
    SkyMode   mode = SkyMode::NoSky;
    /// SkyMode::Equirectangular: the lat-long image. Ignored otherwise.
    TextureId equirect = 0;
    /// SkyMode::Cubemap: six face textures, in the order +X, -X, +Y, -Y, +Z,
    /// -Z, each face seen from INSIDE the cube looking down that WORLD axis
    /// (the backend converts to whatever handedness its cubemaps use). Ignored
    /// in every other mode.
    TextureId faces[6] = { 0, 0, 0, 0, 0, 0 };

    /// SkyMode::Atmosphere: the planet's atmosphere. Ignored otherwise.
    AtmosphereSky atmosphere;

    /// THE ENVIRONMENT (IBL and the ambient) is not described here: it is the
    /// CAPTURE of this sky as drawn, for every mode (PHOTON-7 10-3 R5 - the sky
    /// emits what it shows). A host never states an environment the sky does not
    /// show; the explicit reflection faces that once could are deleted.

    /// THE SUN DISC, drawn over this sky (SunDisc above). A THIRD independent
    /// half: it changes every time the sun light is rotated, and re-uploading
    /// the sky or rebuilding the IBL cubemap for that would be absurd.
    SunDisc   sun;
    /// THE CLOUD LAYER (CloudLayerDesc above), a FOURTH independent half: drawn
    /// as part of the sky and captured with it, but a cloud edit must not tear
    /// the sky down, and a wind change must not even re-capture it.
    CloudLayerDesc clouds;

    /// Exact equality, like every other change-guard on this boundary. Texture
    /// ids are monotonic per scene and never recycled, so equal ids really are
    /// the same pixels.
    bool operator==(const SkyDesc &o) const {
        return sameSky(o) && sun == o.sun && clouds == o.clouds;
    }
    bool operator!=(const SkyDesc &o) const { return !(*this == o); }

    /// The sky half alone: a description that changes only its sun disc or its
    /// cloud layer must not tear the sky down and back up.
    bool sameSky(const SkyDesc &o) const {
        if (mode != o.mode) return false;
        if (mode == SkyMode::Equirectangular) return equirect == o.equirect;
        if (mode == SkyMode::Cubemap) {
            for (int i = 0; i < 6; ++i) if (faces[i] != o.faces[i]) return false;
        }
        if (mode == SkyMode::Atmosphere) return atmosphere == o.atmosphere;
        return true;
    }
};

/// THE ENVIRONMENT'S SOURCE (PHOTON-7 10-3 R5b: the two author tools every renderer has on top of R5's one-radiance law -
/// Unreal's Sky Light Source Type and the film renderers' dome light / backdrop split). What the environment capture
/// photographs - the ambient SH, the convolved reflection cube and every escape that reads them - at the scene's Sky
/// Light gain (Scene::setEnvironmentLight); the camera sees the scene's sky as drawn (SkyDesc) whatever this says.
///   * Sky (the default, R5): the sky as drawn - it emits what it shows;
///   * Cubemap: an INVISIBLE DOME LIGHT - six world-axis faces (SkyDesc::faces' order and frame), photographed instead
///     of the drawn sky, at the faces' own size (a studio HDRI lighting a product against a black World sky);
///   * Black: an UNLIT BACKDROP - nothing is photographed: no sky light at all, the sky still drawn.
enum class EnvironmentSource : unsigned char { Sky = 0, Cubemap, Black };
struct EnvironmentDesc {
    EnvironmentSource source = EnvironmentSource::Sky;
    TextureId faces[6] = { 0, 0, 0, 0, 0, 0 };   ///< Cubemap only; the faces are copied, the caller may destroy them
    bool operator==(const EnvironmentDesc &o) const {
        if (source != o.source) return false;
        if (source == EnvironmentSource::Cubemap)
            for (int i = 0; i < 6; ++i) if (faces[i] != o.faces[i]) return false;
        return true;
    }
    bool operator!=(const EnvironmentDesc &o) const { return !(*this == o); }
};

}}  // namespace jahshaka::engine
