#pragma once
// THE ENGINE's PLANAR TYPES - engine-neutral values, one subject (ENGINE-HEADERS-1: split from Types.h by
// declaration name). NOTHING here may reference Ogre, Qt or GL; a new subject is a new header, never an append here.
#include "jahshaka/engine/Math.h"

namespace jahshaka { namespace engine {

// ---- Planar reflections (scene-level, PLANAR_REFLECTIONS_SPEC.md) ----
/// Mirrors and glossy floors. A node marked a *reflector* (Scene::setNodePlanarReflector)
/// contributes a world-space reflection PLANE derived from its own flat geometry;
/// surfaces lying on such a plane, and within 20 degrees of its normal, sample a
/// re-render of the scene from the mirrored camera.
///
/// THE COST IS A WHOLE EXTRA SCENE RENDER PER ACTIVE PLANE, every frame — plus a
/// private shadow atlas render when `shadows` is on. A scene may hold any number of
/// reflectors; only `budget` of them (the ones on screen, nearest first) render.
/// budget == 0 disables the feature completely and costs nothing at all.
/// THE REFLECTION CUTOFF'S FEATHER, and the only copy of it (DRAG-1 round 2,
/// F13; moved here from EnginePrivate.h).
///
/// A scene carries a reflection ROUGHNESS CUTOFF — where a traced or marched
/// reflection stops being worth its cost — and both arms ramp their confidence
/// to zero across a band of this half-width around it: the traced half from full
/// at `cutoff - kRayReflectFeather` to zero at `cutoff + kRayReflectFeather`,
/// the marched half from full at `cutoff - kRayReflectFeather` to zero AT the
/// cutoff. What one gives up the other's fallback takes through the same
/// composite, so a floor whose roughness varies across it crossfades instead of
/// stepping.
///
/// A CONSTANT and deliberately not a second dial: the cutoff says WHERE the
/// technique stops being worth it (content), the feather only says that it stops
/// smoothly (renderer). 0.1 is about two and a half times the +-0.04 that a
/// roughness map's 8-bit quantisation can move a neighbouring pixel by, so the
/// ramp is always wider than the noise it hides.
///
/// IT IS IN THE PUBLIC HEADER so that anything reasoning about the band reads
/// the shipped number instead of copying it. It was private while only the
/// renderer used it, and document.material_defaults — which asserts that the
/// unauthored material's roughness CLEARS the band — had to carry a second 0.1
/// that nothing would have updated.
constexpr float kRayReflectFeather = 0.1f;

struct PlanarReflectionParams {
    /// Active reflection planes, 0..8 (0 = off). CHANGING THIS RECOMPILES SHADERS:
    /// the count is baked into the PBS shader as a property, not passed as a
    /// uniform. Pushing the same value again is free.
    int      budget = 0;
    /// Edge of each plane's square render target, 256..2048 (rounded to a power
    /// of two). Memory is budget x resolution^2 x 4 bytes x 4/3 (the mip chain),
    /// allocated whether or not the planes are visible.
    unsigned resolution = 512;
    /// Mip chain on the reflection targets. Mips ARE how glossiness works — the
    /// shader samples at roughness * numMips. Without them a rough floor
    /// reflects as sharply as a mirror. Free to leave on.
    bool     mipmaps = true;
    /// Shadows inside the reflections. Costs a private shadow atlas per plane,
    /// at HALF the scene's shadow resolution, allocated up front.
    bool     shadows = false;
    /// THE MIRROR'S TARGET IS THE CHAIN'S TARGET (DRAG-1, RENDER_AUDIT ON-3).
    /// A planar reflection is a SCENE RENDER with no tonemapper of its own, so
    /// its render target has to be able to hold what the scene emits: with the
    /// main chain at RGBA16F (`PostFxDesc::hdr`) and the mirror at 8-bit sRGB,
    /// every radiance above 1.0 inside a mirror was CLIPPED and every dark
    /// reflection carried 8-bit steps the same surface does not show outside
    /// the mirror — banding by construction, and the reason a bright window or
    /// a lamp reflected flat white. It follows the chain rather than being its
    /// own dial for the same reason the probe captures' HDR follows the quality
    /// row: two switches for one picture is one switch too many. Costs 2x the
    /// reflection RTT's memory (a 1024-square slot: 4 MB against 2).
    bool     hdr = false;
    /// Full lighting update for each reflection camera. Off is faster and rarely
    /// visibly different (Ogre's own words); on is what "maximum realness" means.
    bool     accurateLighting = true;
    /// World-space distance over which a surface's reflection fades out as it
    /// leaves the plane, and the radius within which a surface may be matched to
    /// a plane at all. Small values keep a floor's reflection on the floor.
    float    maxDistance = 2.0f;
    /// Clear colour of the reflection render (what shows where the scene has no
    /// geometry and no sky). Normally the view's background.
    Colour   background = Colour(0.0f, 0.0f, 0.0f, 1.0f);

    bool operator==(const PlanarReflectionParams &o) const {
        return budget == o.budget && resolution == o.resolution && mipmaps == o.mipmaps &&
               shadows == o.shadows && hdr == o.hdr && accurateLighting == o.accurateLighting &&
               maxDistance == o.maxDistance && background == o.background;
    }
    bool operator!=(const PlanarReflectionParams &o) const { return !(*this == o); }
};

}}  // namespace jahshaka::engine
