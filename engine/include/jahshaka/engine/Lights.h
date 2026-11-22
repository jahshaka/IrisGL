#pragma once
// THE ENGINE's LIGHTS TYPES - engine-neutral values, one subject (ENGINE-HEADERS-1: split from Types.h by
// declaration name). NOTHING here may reference Ogre, Qt or GL; a new subject is a new header, never an append here.
#include <string>
#include "jahshaka/engine/Ids.h"
#include "jahshaka/engine/Math.h"

namespace jahshaka { namespace engine {

enum class LightType { Directional, Point, Spot, Area };

/// Shadow-map filter quality. GLOBAL to the engine, not per light — the backend's
/// material system has exactly one filter for every shadowed light (see
/// Engine::setShadowFilter). Ordered from cheapest/sharpest to softest.
enum class ShadowFilter { Hard, Soft, VerySoft };

/// A SCENE'S SHADOW REQUEST (ENGINEERING_DEBT_SPEC.md item 4, §9's reading of
/// it: the globalness is real, so the API HIDES it rather than pretending to
/// remove it).
///
/// The backend has ONE shadow filter and ONE shadow atlas for every scene it
/// draws — Engine::setShadowFilter and Engine::setShadowResolution carry the
/// detail and are still the low-level truth. What hosts actually have is a
/// per-scene answer they derived themselves (the softest filter and the
/// largest map any shadow-casting light in THIS scene asked for, or a World
/// panel override that pins both), and before this shape they pushed it
/// through the two global setters guarded by hand-written read-before-write
/// tests — three of them, in the mirror, one per knob per policy branch.
///
/// Scene::setShadowSettings takes the resolved per-scene answer and applies it
/// to the global state, dropping a push that asks for what is already in
/// force. The last scene to push a value owns it, exactly as before.
struct ShadowDesc {
    /// false = "this scene has no opinion about the filter": whatever is in
    /// force stays. That is a scene with no shadow-casting light and no pinned
    /// quality — it must not drag the filter back to a default and undo
    /// another scene's request.
    bool         hasFilter = false;
    ShadowFilter filter = ShadowFilter::Soft;
    /// 0 = "no opinion about the map size" (same reasoning as hasFilter).
    /// Otherwise a request in pixels; the backend clamps to [256, 8192].
    /// NOT cheap when it CHANGES (the shadow node and every workspace that
    /// references it are rebuilt) — which is why the backend compares first.
    unsigned     resolution = 0;

    bool operator==(const ShadowDesc &o) const {
        return hasFilter == o.hasFilter && (!hasFilter || filter == o.filter) &&
               resolution == o.resolution;
    }
    bool operator!=(const ShadowDesc &o) const { return !(*this == o); }
};

/// A light attached to a node. Direction comes from the node's orientation
/// (lights shine down the node's -Z), position from the node's transform.
struct LightDesc {
    LightType type = LightType::Point;
    Colour    colour = Colour(1.0f, 1.0f, 1.0f);
    float     intensity = 1.0f;        // radiometric scale (Jahshaka's "intensity")
    /// THE RANGE (point/spot): where the light's window reaches zero. The light
    /// follows the inverse square law, I / d^2, windowed by
    /// saturate(1 - (d/range)^4)^2 (Karis 2013) — the one falloff every consumer
    /// shares (the fork's JahBrdf piece, `jahLightAttenuation`; the suites' closed
    /// form is tests/support/lightfalloff.h). An intensity of
    /// I at 1 m lights a facing surface exactly as a sun of intensity I does.
    float     range = 10.0f;
    /// THE SOURCE RADIUS, metres (point/spot; IMAGE-1): the size of the emitter.
    /// Inside it the light cannot get brighter: the falloff is
    /// 1 / max(d^2, sourceRadius^2). It changes the falloff only — Ogre's
    /// shadow maps have no source size. Area lights ignore it: their source
    /// is their rectangle.
    float     sourceRadius = 0.1f;
    /// The spot cone's HALF angle, in degrees — the angle between the light's
    /// axis and the cone's edge, which is what the document has always stored
    /// and what the editor's cone wire is drawn from
    /// (`radius = range * tan(spotCutOff)`, scenemirror.cpp).
    ///
    /// It is NOT what Ogre's `Light::setSpotlightRange` takes: that one wants
    /// the FULL apex angle. Passing the half angle straight through (which this
    /// boundary did until LIGHTING_FIX fix 5) rendered every spot at half the
    /// cone the editor drew — the classic "the light does not fill its wire"
    /// report. The doubling lives in OgreScene::setLight, once.
    float     spotAngleDegrees = 30.0f;
    /// 0..1. The bright core as a fraction pulled off the outer cone:
    /// innerFull = outerFull * (1 - softness). 0 = hard edge, 0.99 = almost all
    /// penumbra. Values outside the range are clamped, not honoured.
    float     spotSoftness = 0.1f;
    /// The penumbra's exponent, Ogre's `falloff` argument. 1 = linear across the
    /// penumbra (the only value this engine used before it was authorable);
    /// higher concentrates the light towards the core.
    float     spotFalloff = 1.0f;
    bool      castShadows = true;          // ignored for Area (backend cannot shadow them)
    // Area lights only: a rectangle spanning the node's local X (width) and
    // Z (height), emitting down -Y like every other light type here. Every
    // area light is LTC on every path (the pixel, the voxels, the cards, the
    // rays): the fast approximation and its mask are deleted (PHOTON-7 7-0).
    float     rectWidth = 1.0f;
    float     rectHeight = 1.0f;
    bool      doubleSided = false;         // emit from both faces

    /// Absolute path to an IES photometric profile (.ies); empty = none.
    ///
    /// The profile is a 1-D candela lobe around the light's own direction; the
    /// backend samples it as an extra attenuation term. Three hard limits come
    /// from the renderer, not from us, and the UI must say so:
    ///   * SPOT lights always honour it (shadow-casting or not).
    ///   * POINT lights honour it ONLY while they cast no shadows — a
    ///     shadow-casting point light moves from the clustered light list into
    ///     the pass buffer, whose point-light loop has no profile term.
    ///   * DIRECTIONAL and AREA lights never honour it.
    /// The profile's own candela scale is NOT normalized here: `intensity`
    /// arrives already divided by the profile's peak (the host does that from
    /// import-time metadata) so assigning a profile changes the SHAPE of the
    /// falloff and not the brightness.
    std::string iesProfilePath;

    /// LIGHTING CHANNELS, light side (Scene::setNodeLightMask is the object
    /// side). This light lights an object when `lightMask & object mask` is
    /// non-zero; both default to all-ones, so out of the box every light lights
    /// every object. All 32 bits are the host's — the engine reserves none.
    ///
    /// It filters DIRECT light only, from every path (directional, shadow
    /// casting, Forward+ clustered, area LTC). It does NOT
    /// filter shadow CASTING (a masked-off object still renders into this
    /// light's shadow map) and it does NOT filter this light's GI bounce.
    /// Scene::setNodeLightMask carries the full contract, and
    /// Scene::setNodeCastShadow is the escape hatch for the caster side.
    unsigned  lightMask = 0xFFFFFFFFu;

    /// FORWARD SHADING PRIORITY — DIRECTIONAL LIGHTS ONLY. 0 is the sun.
    /// Informational at the backend: the host resolves which directional is
    /// the sun (one resolver, iris::Scene::sunLight) and says so in
    /// `primaryDirectional`; this number is carried so the renderer can report
    /// it back and so the two cannot silently disagree.
    int       forwardShadingPriority = 0;

    /// Is this the scene's SUN — the directional light that takes the shadow
    /// node's PSSM splits (slot 0)? Every further casting directional takes a
    /// focused map. setLight gives the primary the higher shadow sort priority,
    /// so Ogre's directional sort puts it first on every frame and reload
    /// (creation order would not). Meaningless on point/spot/area lights.
    bool      primaryDirectional = true;

    /// "Is this the same light state I last pushed?" — the guard a host with a
    /// per-frame push loop needs (setLight is ~20 backend setters, including an
    /// attenuation solve that rewrites the light's local AABB). Exact
    /// comparison, like PbrParams::operator==.
    ///
    /// EVERY FIELD setLight READS IS HERE: add a field to this struct and this
    /// must grow with it, or the new field silently stops reaching the backend
    /// after the first push. (It lives beside the fields for that reason — the
    /// mirror carried a hand-written `sameLight()` for it, one file away from
    /// the struct it had to track.)
    bool operator==(const LightDesc &o) const {
        return type == o.type && colour == o.colour && intensity == o.intensity &&
               range == o.range && sourceRadius == o.sourceRadius &&
               spotAngleDegrees == o.spotAngleDegrees &&
               spotSoftness == o.spotSoftness && spotFalloff == o.spotFalloff &&
               castShadows == o.castShadows &&
               rectWidth == o.rectWidth && rectHeight == o.rectHeight &&
               doubleSided == o.doubleSided &&
               iesProfilePath == o.iesProfilePath &&
               lightMask == o.lightMask &&
               forwardShadingPriority == o.forwardShadingPriority &&
               primaryDirectional == o.primaryDirectional;
    }
    bool operator!=(const LightDesc &o) const { return !(*this == o); }
};

/// A projected-texture decal attached to a node (DECALS_SPEC.md §5.2).
///
/// The decal is an ORIENTED BOX that overwrites base colour, roughness and
/// metalness on every surface inside it. Two conventions, both fixed by the
/// backend's shader and neither cheap to change:
///
///  - it projects down the node's LOCAL -Y (identical to LightDesc's
///    direction convention), and only affects surfaces whose normal points
///    back at it;
///  - the image's U axis is local X and its V axis is local Z, so `width`
///    is the local-X extent and `height` the local-Z extent. `depth` is the
///    local-Y thickness of the projector box.
///
/// `diffuse` MUST come from Scene::loadDecalTexture(): decal images live in a
/// dedicated fixed-geometry texture pool and a plain loadTexture() id is either
/// non-batched (the backend asserts) or in the wrong pool (it would silently
/// sample another decal's image).
///
/// THERE IS NO PER-DECAL OPACITY OR COLOUR TINT. The backend packs exactly four
/// floats per decal (3 rows of the inverse world matrix + one float4 of
/// indices/metalness/roughness); adding either would mean forking the shader
/// template, which this project does not do.
struct DecalDesc {
    TextureId diffuse  = 0;   ///< base colour + alpha mask; from loadDecalTexture()
    TextureId normal   = 0;   ///< optional; from loadDecalTexture(kind Normal)
    TextureId emissive = 0;   ///< optional; from loadDecalTexture(kind Emissive)
    float width  = 1.0f;      ///< local X extent
    float height = 1.0f;      ///< local Z extent
    float depth  = 0.5f;      ///< local Y extent (projection thickness)
    float metalness = 0.0f;
    float roughness = 1.0f;
    /// Diffuse alpha masks the base colour only, not the normal/emissive maps.
    bool  ignoreAlphaDiffuse = false;
};

/// Which pooled decal atlas a decal image is loaded into. The three atlases
/// have different pixel formats and filters (a normal map is neither sRGB nor
/// the same channel layout), so the caller must say which one it wants.
enum class DecalMap { Diffuse, Normal, Emissive };

}}  // namespace jahshaka::engine
