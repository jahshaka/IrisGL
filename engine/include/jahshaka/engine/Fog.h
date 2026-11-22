#pragma once
// THE ENGINE's FOG TYPES - engine-neutral values, one subject (ENGINE-HEADERS-1: split from Types.h by
// declaration name). NOTHING here may reference Ogre, Qt or GL; a new subject is a new header, never an append here.
#include "jahshaka/engine/Math.h"

namespace jahshaka { namespace engine {

// ---- Fog (scene-level) ------------------------------------------------------
/// EXPONENTIAL distance fog, plus an optional height-varying layer. Both layers
/// absorb, so their transmittances multiply:
///
///     transmittance = 2^( -distance * density ) * 2^( -heightOpticalDepth )
///     pixel         = lerp( fogColour, surface, transmittance )
///
/// `density` is therefore "how much is lost per world unit" in exp2 units: a
/// surface 1/density units away keeps half its own colour, and 4.32/density is
/// where only 5% of it survives. (The document maps the legacy linear start/end
/// pair onto it by matching the half-fogged distance — iris::Scene.)
///
/// THE COLOUR DEPENDS ON THE SKY, NOT ON A SWITCH. Under the planet's
/// atmosphere (SkyMode::Atmosphere) the air itself is a medium: every lit pixel
/// is seen through the atmosphere's own AERIAL PERSPECTIVE whether or not this
/// fog is enabled (AtmosphereSky::aerialScale; SKY-ATMOSPHERE-1), and this fog
/// composes ON TOP of it — `density` and the height layer fog towards the sky's
/// own radiance just above the horizon at the direction each surface is seen
/// from, so a fully fogged world reads as the horizon sky behind it. `colour`
/// is used only under the other skies (a colour, a gradient, an image), which
/// have no atmosphere to ask.
///
/// Only lit (PBR) surfaces are fogged; unlit overlays (gizmos, wires,
/// billboards) and the sky never are.
struct FogDesc {
    bool   enabled = false;
    Colour colour;                ///< linear fog colour — the non-analytic skies only
    float  density = 0.024f;      ///< homogeneous density per world unit (exp2), on top of the air

    /// Height layer: a second exponential medium whose density falls off with
    /// world Y — density(y) = heightDensity * 2^( -(y - heightLevel) * heightFalloff )
    /// — integrated along the view ray. heightDensity = 0 disables it exactly
    /// (the shader branch is skipped, not multiplied by one).
    float  heightDensity = 0.0f;  ///< density at heightLevel, per world unit (exp2)
    float  heightFalloff = 0.1f;  ///< per world unit; larger = thins out faster with altitude
    float  heightLevel   = 0.0f;  ///< world Y where heightDensity applies

    // (the brightness "breakthrough" - bright pixels resisting the fog by their own luminance - is DELETED, PHOTON-7
    // 10-4 R11: a medium's transmittance is its optical depth alone; its in-scatter is lit by what reaches it)

    bool operator==(const FogDesc &o) const {
        return enabled == o.enabled && colour == o.colour && density == o.density &&
               heightDensity == o.heightDensity && heightFalloff == o.heightFalloff &&
               heightLevel == o.heightLevel;
    }
    bool operator!=(const FogDesc &o) const { return !(*this == o); }
};

// ---- Height fog (scene-level; SKY-DEFAULTS-1) ------------------------------
/// THE EXPONENTIAL HEIGHT FOG — Unreal's Exponential Height Fog, the
/// non-volumetric core (its density, height falloff, height and start
/// distance), and a medium of the WORLD rather than the scene: nothing nearer
/// than `startDistance` is fogged, so an authored set stays exactly itself while
/// the far world, the horizon and the sky under it take the fog. It is a
/// SEPARATE medium from FogDesc (the World fog), which is untouched by it.
///
/// THE MEDIUM: density(y) = density * 2^( -heightFalloff * (y - baseHeight) ),
/// integrated along every view ray from `startDistance` to the pixel —
/// Unreal's own line integral, verbatim (HeightFogCommon: RayOriginTerms x
/// (1 - 2^-F) / F x length, F = heightFalloff x the segment's rise), including
/// its ln 2 at a level ray, so an Unreal density means the same picture here:
///
///     transmittance = 2^( -density_at_start * length * (1 - 2^-F) / F )
///
/// SKY PIXELS ARE FOGGED TOO, at kHeightFogSkyDistance: a ray that climbs
/// slowly (just above the horizon) crosses a long stretch of the layer and takes
/// the fog colour, a steep one crosses little and stays the sky, and a ray under
/// the horizon is fogged completely — so the horizon blends smoothly above and
/// below and the planet under it reads as the fog.
///
/// THE COLOUR IS THE SKY'S IN-SCATTER, not a pick: the mean radiance the sky
/// dome delivers to an upward-facing droplet (the environment capture's SH
/// evaluated at +Y — irradiance / pi, the Sky Light's own integral), in force
/// with the environment it was integrated from. No sky, no colour: black.
///
/// UNITS ARE METRES. `density` and `heightFalloff` are per metre (exp2);
/// Unreal's dials are these x 10 (its component divides its dials by 1000 per
/// centimetre, SceneCore.cpp FExponentialHeightFogSceneInfo), which is the
/// conversion the document's dials make.
struct HeightFogDesc {
    bool  enabled = false;
    float density = 0.002f;         ///< per metre at baseHeight (Unreal 0.02)
    float heightFalloff = 0.02f;    ///< per metre (Unreal 0.2)
    float baseHeight = 0.0f;        ///< world Y where `density` applies
    float startDistance = 0.0f;     ///< metres from the eye before any fog
    bool operator==(const HeightFogDesc &o) const {
        if (enabled != o.enabled) return false;
        if (!enabled) return true;
        return density == o.density && heightFalloff == o.heightFalloff &&
               baseHeight == o.baseHeight && startDistance == o.startDistance;
    }
    bool operator!=(const HeightFogDesc &o) const { return !(*this == o); }
};
/// The distance a SKY pixel is fogged at (metres): Unreal fogs the sky at its
/// far depth; 100 km is the aerial volume's reach and the cloud sheet's edge.
constexpr float kHeightFogSkyDistance = 100000.0f;
/// What the renderer is doing with the height fog (Scene::heightFogStatus).
struct HeightFogStatus {
    bool  on = false;          ///< registered, drawn on the sky and the PBS passes
    float colour[3] = { 0.0f, 0.0f, 0.0f };   ///< the in-scatter in force (linear)
    bool  colourFromSky = false;              ///< false = no sky captured yet (black)
};

}}  // namespace jahshaka::engine
