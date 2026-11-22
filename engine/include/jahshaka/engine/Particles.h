#pragma once
// THE ENGINE's PARTICLES TYPES - engine-neutral values, one subject (ENGINE-HEADERS-1: split from Types.h by
// declaration name). NOTHING here may reference Ogre, Qt or GL; a new subject is a new header, never an append here.
#include <string>
#include <vector>
#include "jahshaka/engine/Ids.h"
#include "jahshaka/engine/Math.h"
#include "jahshaka/engine/Fwd.h"

namespace jahshaka { namespace engine {

/// One camera-facing textured quad in a node's billboard set (Scene::setBillboards).
/// Positions are WORLD-space: the document simulates particles in world space and
/// the engine draws them as-is.
struct BillboardInstance {
    Vec3   position;                       ///< world-space centre of the quad
    float  size = 1.0f;                    ///< quad edge length in world units
    float  rotationRadians = 0.0f;         ///< spin around the view axis
    Colour colour = Colour(1.0f, 1.0f, 1.0f, 1.0f);   ///< multiplies the texture
};

/// WHICH LAYER a billboard set belongs to — scene content, or editor helper.
///
/// `Scene` is what a particle emitter is: geometry in the world, depth-tested
/// against it, graded by every post effect the view runs (tonemap, bloom, SSAO,
/// SMAA) exactly like the meshes around it.
///
/// `Overlay` is what a light ICON is: a helper the user must be able to READ.
/// It draws in the same pass as the gizmo — after the whole post chain, with no
/// depth test — so a white glyph stays white instead of being tonemapped to
/// grey, bloomed into its neighbours and smeared by edge detection (the
/// 2026-09-08 owner report: "the light icons are grey and blurred"). Pair it
/// with Scene::setNodeHelper so the icon also stays out of reflections and
/// probe captures.
enum class BillboardLayer { Scene, Overlay };

// ---- Particles (PARTICLES_FX2_SPEC.md): natively simulated particle systems ----
// The host describes WHAT it wants; the engine owns every Ogre object behind it.
// One authored node = one particle-system definition = one quota, one material,
// one visibility flag (the definition is what the render queue tests).

/// The shape particles spawn inside. Point ignores `extents`.
enum class ParticleEmitterShape { Point, Box, Cylinder, Ellipsoid, HollowEllipsoid, Ring };

/// How a particle's quad is oriented. `Point` is the camera-facing billboard
/// everything used before this; `OrientedSelf` streaks the quad along the
/// particle's own velocity (sparks, rain).
enum class ParticleOrientation { Point, OrientedCommon, OrientedSelf,
                                 PerpendicularCommon, PerpendicularSelf };

/// One emitter on a system. Position/direction are LOCAL to the node: the engine
/// applies the node's derived position and orientation (but NOT its scale — an
/// emitter's spawn volume is numeric, see `extents`).
struct ParticleEmitterDesc {
    ParticleEmitterShape shape = ParticleEmitterShape::Point;
    Vec3  position{0, 0, 0};       ///< offset from the node's origin
    Vec3  direction{0, 1, 0};      ///< emission axis; the document's +Y convention
    float angleDegrees = 0.0f;     ///< emission cone half-angle around `direction`
    float rate = 24.0f;            ///< particles per second
    float velocityMin = 1.0f, velocityMax = 1.0f;   ///< initial speed range (m/s)
    float ttlMin = 1.0f, ttlMax = 1.0f;             ///< time-to-live range (seconds)
    float sizeWidth = 1.0f, sizeHeight = 1.0f;      ///< initial quad dimensions
    Colour colourStart{1, 1, 1, 1}, colourEnd{1, 1, 1, 1};   ///< per-particle emission colour range
    Vec3  extents{1, 1, 1};        ///< Box: w/h/d. Cylinder/Ellipsoid/Ring: radii. Point: ignored
    Vec3  innerExtents{0, 0, 0};   ///< HollowEllipsoid / Ring only: the hole
    float duration = 0.0f;         ///< 0 = emit forever; >0 = burst of this many seconds
    float repeatDelay = 0.0f;      ///< pause between bursts
    float startTime = 0.0f;        ///< delay before the first emission
};

/// One affector on a system. `kind` selects which fields matter; the rest are
/// ignored. Affectors run per particle, per frame, SIMD, on worker threads.
struct ParticleAffectorDesc {
    enum class Kind {
        ColourKeys,     ///< colour over life, up to 6 keys (ColourInterpolator)
        ScaleKeys,      ///< size multiplier over life, up to 6 keys (ScaleInterpolator)
        Rotator,        ///< random start angle + spin speed
        LinearForce,    ///< a constant acceleration: gravity, buoyancy, wind
        Turbulence,     ///< random velocity perturbation (DirectionRandomiser)
        DeflectorPlane, ///< bounce off an infinite plane
        // ---- ADDENDUM A-4: the four the plugin registers and we never used.
        // (The plugin has TEN affector factories; we mapped six, and the audit's
        // "6/9" counted the two colour faders as one.)
        /// Per-second colour DELTAS, in two stages: `colourAdjust1` until a
        /// particle has `colourSwitchAt` seconds of life left, `colourAdjust2`
        /// after. Covers the plugin's plain ColourFader as well — it IS this
        /// affector with adjust2 == adjust1 — so there is one kind, not two.
        /// Different from ColourKeys: keys REPLACE the colour at authored life
        /// fractions, a fade ADDS a rate to whatever the colour currently is.
        ColourFade,
        /// Colour over life sampled from row 0 of an IMAGE — the classic fire
        /// ramp. NOT a TextureId: the affector loads by NAME through
        /// ResourceGroupManager::AUTODETECT, so the engine registers the file's
        /// directory as a resource location first (the idiom OgreLights and
        /// OgreDecals already use for the same reason).
        ColourRamp,
        /// A size RATE: additive units per second, or multiplicative
        /// `rate^dt` when `scaleMultiply` is set. Different from ScaleKeys,
        /// which authors absolute sizes at life fractions.
        ScaleRate
    };
    Kind kind = Kind::LinearForce;

    /// ColourKeys / ScaleKeys. `keyCount` entries are used, in ascending time.
    /// Times are life fractions in [0,1]. Colour components may exceed 1 — the
    /// GPU encoding carries [-4, 120], which is what makes HDR fire bloom.
    unsigned keyCount = 0;
    Colour colourKeys[6];
    float  colourKeyTimes[6] = {0, 0, 0, 0, 0, 0};
    float  scaleKeys[6]      = {1, 1, 1, 1, 1, 1};
    float  scaleKeyTimes[6]  = {0, 0, 0, 0, 0, 0};

    /// Rotator: degrees. Start angle is picked per particle in [rotStart, rotEnd],
    /// spin speed per particle in [rotSpeedMin, rotSpeedMax] degrees/second.
    float rotSpeedMin = 0.0f, rotSpeedMax = 0.0f;
    float rotStart = 0.0f, rotEnd = 0.0f;

    /// LinearForce: world-space acceleration. `forceAverage` averages the force
    /// into the velocity instead of adding to it.
    Vec3 force{0, 0, 0};
    bool forceAverage = false;

    /// Turbulence: how much random direction is injected, and to what fraction
    /// of the particles (`scope` in [0,1]).
    float randomness = 0.0f, scope = 1.0f;
    bool  keepVelocity = false;

    /// DeflectorPlane.
    Vec3  planePoint{0, 0, 0}, planeNormal{0, 1, 0};
    float bounce = 1.0f;

    // ---- ADDENDUM A-4 ----------------------------------------------------
    /// ColourFade: per-second deltas, stage 1 then stage 2, plus the clamps.
    /// Both stages default to ZERO, which is the neutral value — an affector
    /// with no authored fade does nothing at all.
    Colour colourAdjust1{0, 0, 0, 0};
    Colour colourAdjust2{0, 0, 0, 0};
    /// Switch to stage 2 when the particle has this much life LEFT, in
    /// seconds. 0 = never switch (stage 1 for the whole life).
    float  colourSwitchAt = 0.0f;
    Colour colourMin{0, 0, 0, 0};
    Colour colourMax{1, 1, 1, 1};

    /// ColourRamp: an absolute path to a 1-D ramp image. Row 0 is sampled
    /// across the particle's life.
    std::string colourRampPath;

    /// ScaleRate: units per second (additive) or the per-second factor
    /// (multiplicative). 1.0 with `scaleMultiply` and 0.0 without are both
    /// neutral.
    float scaleRate = 0.0f;
    bool  scaleMultiply = false;
};

/// A complete particle system for one node. Changing a scalar (rate, colour keys,
/// force...) is applied in place; changing the TOPOLOGY — the emitter shapes, the
/// affector kinds, the quota, the orientation — rebuilds the underlying definition,
/// which is why the engine keeps the affector set fixed and neutral at defaults.
struct ParticleSystemDesc {
    unsigned  quota = 1024;        ///< hard cap on live particles; rounded up to a bucket
    TextureId texture = 0;         ///< 0 = untextured white
    bool      additive = true;     ///< (src-alpha, one); false = alpha blending
    bool      alphaHash = true;    ///< order-independent transparency for alpha blending
                                   ///< (ignored when `additive`, which needs no sorting)
    /// A DISTORTION emitter (POST_LOOKS_SPEC §5.3 4b): the particles draw no
    /// colour at all — they are rendered into the view's distortion field and
    /// warp whatever is behind them (heat haze, shock rings, a jet's exhaust).
    /// `texture` is then read as the DISPLACEMENT map, exactly as a
    /// ShadingModel::Distortion material reads its normal-map slot (R/G
    /// remapped to [-1,1]), and each particle's colour ALPHA is its strength
    /// (the emitter's colour range and any colour ramp fade it over life).
    /// `additive` and `alphaHash` are ignored: the field is always alpha
    /// blended. A distortion emitter is invisible to every pass but the
    /// distortion pass, so with no distortion in the view's post chain the
    /// frame is byte-identical to the emitter not existing. It is a TOPOLOGY
    /// property: flipping it rebuilds the definition.
    bool      distortion = false;
    ParticleOrientation orientation = ParticleOrientation::Point;
    Vec3      commonDirection{0, 0, 1}, commonUp{0, 1, 0};   ///< *Common orientations only
    std::vector<ParticleEmitterDesc>  emitters;
    std::vector<ParticleAffectorDesc> affectors;
};

}}  // namespace jahshaka::engine
