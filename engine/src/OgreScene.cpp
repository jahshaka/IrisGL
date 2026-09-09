// OgreScene core: lifetime, node hierarchy, transforms, visibility, lights and
// the teardown helpers. Meshes, materials, sky, GI and particles live in their
// own translation units.
#include "EnginePrivate.h"

namespace jahshaka { namespace engine { namespace detail {

OgreScene::OgreScene(Ogre::Root *root, Ogre::SceneManager *sm, const std::string &name,
                     std::string &errorSink)
    : mRoot(root), mSceneMgr(sm), mName(name), mError(errorSink) {}

OgreScene::~OgreScene() { destroy(); }

const std::string &OgreScene::name() const { return mName; }

void OgreScene::setAmbient(const Colour &upper, const Colour &lower) {
    // The hemisphere pair, expressed EXACTLY in the SH basis the backend now
    // runs on: f(n) = lerp(lower, upper, n.y * 0.5 + 0.5)
    //              = (upper + lower)/2  +  (upper - lower)/2 * n.y,
    // i.e. the constant band and the y term, nothing else. No approximation.
    //
    // The 1/pi on the EQUAL-colour case, and only there. HlmsPbs' two ambient
    // paths do not agree with each other by a factor of pi, and which one a
    // caller got used to depend on exactly this test:
    //   * upper != lower selected AmbientHemisphere, whose colours feed
    //     envColourD and are multiplied by pi against a kD that already carries
    //     1/pi (200.BRDFs_piece_ps.any:305) — i.e. a mean RADIANCE;
    //   * upper == lower selected AmbientFixed, which does
    //     `finalColour += ambient * kD` with no pi — i.e. pi times darker for the
    //     same numbers.
    // (OgreHlmsPbs.cpp:1698, the AmbientAutoNormal branch.) Every caller in the
    // tree was written against whichever of the two it happened to hit, so the
    // conversion reproduces the split rather than picking a side: a flat ambient
    // keeps its old (dark) meaning, a hemisphere pair keeps its old (radiance)
    // one. Sky-driven ambient never comes through here — the mirror pushes
    // radiance-unit SH straight to setAmbientSh.
    // FINDING for the lead: that pi cliff at upper == lower is Ogre's, not ours,
    // and it is now the only reason this scale factor exists.
    const bool flat = upper.r == lower.r && upper.g == lower.g && upper.b == lower.b;
    const float kFlat = flat ? 1.0f / 3.14159265358979f : 1.0f;
    const float c0[3] = { (upper.r + lower.r) * 0.5f * kFlat,
                          (upper.g + lower.g) * 0.5f * kFlat,
                          (upper.b + lower.b) * 0.5f * kFlat };
    const float c1[3] = { (upper.r - lower.r) * 0.5f * kFlat,
                          (upper.g - lower.g) * 0.5f * kFlat,
                          (upper.b - lower.b) * 0.5f * kFlat };
    float sh[27] = { 0 };
    for (int c = 0; c < 3; ++c) { sh[c] = c0[c]; sh[3 + c] = c1[c]; }
    setAmbientSh(sh);
    // ...but VCT gets the RADIANCE pair, not the 1/pi one (LIGHTING_FIX fix 3).
    // The scale factor above exists to reproduce HlmsPbs' own pi discrepancy
    // between its AmbientFixed and AmbientHemisphere paths; VctLighting has no
    // such split — its ambient is added to the cone-trace result as plain
    // radiance (`light.xyz += ambient * light.w`, Vct_piece_ps.any) — so pushing
    // the darkened value there would make a VCT scene's ambient pi times darker
    // than the same scene without VCT. setAmbientSh has already recorded the
    // (possibly scaled) SH-derived pair; overwrite it with the true one.
    mAmbientRadiance[0] = upper;
    mAmbientRadiance[1] = lower;
    applyVctAmbient();
}

// THE VCT AMBIENT (LIGHTING_FIX fix 3 / F-V1). Ogre's VctLighting is born with
// both hemispheres at BLACK (OgreVctLighting.cpp:106-107) and nothing in this
// engine ever set them, so every surface inside a VCT volume lost the scene's
// ambient entirely: the PBS ambient pieces are wrapped in
// `if( vctSpecular.w == 0 )` — "only use ambient lighting if the object is
// outside any VCT probe" (AmbientLighting_piece_ps.any) — and the replacement
// inside the volume is VctLighting's own pair. Zero in, zero out. It is also
// why probe captures came out brighter than the world they sampled (F-V3): the
// capture pass and the main pass disagreed about the ambient term.
//
// ALWAYS A GENUINE PAIR. `VctLighting::needsAmbientHemisphere()` is a memcmp of
// the two colours (OgreVctLighting.cpp:1010-1013) and its result becomes the
// `vct_ambient_sphere` shader property (OgreHlmsPbs.cpp:1777) — so an ambient
// whose upper and lower happen to be equal for one frame of a colour drag
// compiles a DIFFERENT shader for that frame and back again on the next. The
// epsilon below costs nothing visually (it is 1e-6 of radiance) and pins the
// variant, which is worth far more than the exactness it gives up.
void OgreScene::applyVctAmbient() {
    if (!mVctLighting) return;
    JAH_TRY {
        static const float kHemiEpsilon = 1e-6f;
        const Colour &u = mAmbientRadiance[0], &l = mAmbientRadiance[1];
        mVctLighting->setAmbient(
            Ogre::ColourValue(u.r, u.g, u.b + kHemiEpsilon, 1.0f),
            Ogre::ColourValue(l.r, l.g, l.b, 1.0f));
        if (std::getenv("JAHSHAKA_GI_DEBUG"))
            Ogre::LogManager::getSingleton().logMessage(
                "Jahshaka GI: vct ambient upper " + std::to_string(u.r) + "," + std::to_string(u.g) +
                "," + std::to_string(u.b) + " lower " + std::to_string(l.r) + "," +
                std::to_string(l.g) + "," + std::to_string(l.b) + " hemi=" +
                (mVctLighting->needsAmbientHemisphere() ? "1" : "0"));
    } JAH_CATCH(mError, );
}

void OgreScene::setAmbientSh(const float sh[27]) {
    JAH_TRY {
        // HlmsPbs does NOT evaluate the SH basis on the world normal. It uses
        //     wsNormal = mul( passBuf.invViewMatCubemap, normal ); wsNormal.x = -wsNormal.x;
        // (AmbientLighting_piece_ps.any) — the left-handed cubemap frame with X
        // flipped on top, which works out to the world frame rotated 180 degrees
        // about Y: (x, y, z) -> (-x, y, -z). Under that rotation the basis terms
        // {1, y, z, x, xy, yz, 3z^2-1, zx, x^2-y^2} pick up the signs below, so
        // the coefficients a caller gives in WORLD axes are multiplied by them to
        // land where the caller meant. VERIFIED by ambient_sh_lights_world_axes
        // (tests/engine): each band lights the face of a cube it names.
        static const float kAxisSign[9] = { 1, 1, -1, -1, -1, -1, 1, 1, 1 };
        Ogre::Vector3 coeffs[9];
        for (int i = 0; i < 9; ++i)
            coeffs[i] = Ogre::Vector3(sh[i * 3 + 0], sh[i * 3 + 1], sh[i * 3 + 2]) * kAxisSign[i];
        mSceneMgr->setSphericalHarmonics(coeffs);
        // The two-colour ambient still drives envmapScale (it rides
        // ambientUpperHemi.w) and is what a non-SH Hlms would read; keep it at
        // the SH constant band so nothing reads stale colours.
        //
        // envFeatures = 0, NOT the 0xffffffff default: that default turns on
        // EnvFeatures_DiffuseGiFromReflectionProbe, which adds the reflection
        // cubemap's roughest mip to envColourD as an approximate diffuse GI. We
        // now have the real thing in SH, and the reflection cube is a genuine
        // GGX convolution rather than the face-local box mips it used to be — so
        // leaving the flag on both DOUBLE-COUNTS the ambient and undoes the
        // hemisphere split (a sky lit only below its horizon was lighting
        // upward-facing surfaces at 0.69 instead of 0.04). Ogre's own doc says
        // exactly this: "do not set this flag ... because the diffuse GI is
        // already gathered from another source of information".
        const Ogre::ColourValue flat(sh[0], sh[1], sh[2], 1.0f);
        mSceneMgr->setAmbientLight(flat, flat, Ogre::Vector3::UNIT_Y, 1.0f, 0u);
        // The VCT arm's own copy of the same ambient, in RADIANCE units — which
        // for an SH push (the sky path) is what the coefficients already are.
        // f(n) = c0 + c1 * n.y, so the poles are c0 +- c1. setAmbient overwrites
        // this afterwards with its unscaled pair; see applyVctAmbient.
        mAmbientRadiance[0] = Colour(sh[0] + sh[3], sh[1] + sh[4], sh[2] + sh[5], 1.0f);
        mAmbientRadiance[1] = Colour(sh[0] - sh[3], sh[1] - sh[4], sh[2] - sh[5], 1.0f);
        applyVctAmbient();
    } JAH_CATCH(mError, );
}

bool OgreScene::removeNode(NodeId id) {
    auto it = mNodes.find(id);
    if (it == mNodes.end()) return false;
    JAH_TRY {
        const bool hadDecal = it->second.decal != nullptr;
        releaseNode(it->first, it->second);
        mNodes.erase(it);
        // The last decal leaving must clear the SceneManager's atlas bindings,
        // which is what drops the decal code back out of every PBS shader.
        if (hadDecal) refreshDecalBindings();
        return true;
    } JAH_CATCH(mError, false);
}

// ---- Hierarchy and transforms ----
NodeId OgreScene::createNode(NodeId parent) {
    JAH_TRY {
        Ogre::SceneNode *p = parent ? node(parent) : nullptr;
        if (parent && !p) { mError = "createNode: unknown parent"; return 0; }
        if (!p) p = mSceneMgr->getRootSceneNode(Ogre::SCENE_DYNAMIC);
        Node rec; rec.node = p->createChildSceneNode(Ogre::SCENE_DYNAMIC);
        return track(rec);
    } JAH_CATCH(mError, 0);
}

NodeId OgreScene::adoptNode(void *nativeSceneNode) {
    JAH_TRY {
        Ogre::SceneNode *n = static_cast<Ogre::SceneNode *>(nativeSceneNode);
        if (!n) { mError = "adoptNode: null node"; return 0; }
        if (n->getCreator() != mSceneMgr) {
            mError = "adoptNode: the node belongs to another scene manager";
            return 0;
        }
        Node rec;
        rec.node = n;
        rec.owned = false;
        return track(rec);
    } JAH_CATCH(mError, 0);
}

void *OgreScene::nativeSceneManager() const { return mSceneMgr; }

bool OgreScene::setNodeParent(NodeId id, NodeId parent) {
    JAH_TRY {
        Ogre::SceneNode *n = node(id);
        if (!n) { mError = "setNodeParent: unknown node"; return false; }
        // An adopted node's place in the tree is the DOCUMENT's (one tree).
        if (!mNodes[id].owned) { mError = "setNodeParent: node is adopted"; return false; }
        Ogre::SceneNode *p = parent ? node(parent) : mSceneMgr->getRootSceneNode(Ogre::SCENE_DYNAMIC);
        if (!p) { mError = "setNodeParent: unknown parent"; return false; }
        if (n->getParent() == p) return true;
        if (n->getParent()) n->getParent()->removeChild(n);
        p->addChild(n);
        return true;
    } JAH_CATCH(mError, false);
}

void OgreScene::setNodeTransform(NodeId id, const Vec3 &pos, const Quat &rot, const Vec3 &scale) {
    JAH_TRY {
        auto it = mNodes.find(id);
        if (it != mNodes.end() && !it->second.owned) {
            // The document owns an adopted node's transform. Silently ignoring
            // the write would be worse than saying so: a caller that still
            // pushes transforms at the engine has not been converted.
            mError = "setNodeTransform: node is adopted; the document owns its transform";
            return;
        }
        if (auto *n = node(id)) {
            n->setPosition(toOgre(pos));
            n->setOrientation(Ogre::Quaternion(rot.w, rot.x, rot.y, rot.z));
            n->setScale(toOgre(scale));
        }
    } JAH_CATCH(mError, );
}

// THE bit-scheme application point (REFLECTIONS_ADOPTION_SPEC.md P1b).
// Helpers carry kHelperBit INSTEAD OF kVisibleBit — an include channel, because
// Ogre's any-bit test cannot express an exclude bit (EnginePrivate.h's block).
// Only lit (PBR) surfaces get kGiGeometryBit: unlit overlays, wires and line
// meshes must neither bounce nor occlude GI rays.
Ogre::uint32 OgreScene::itemVisibilityFlags(Node &n, bool unlit) {
    n.materialUnlit = unlit;          // remembered for applyNodeVisibilityFlags
    if (n.helper) return kHelperBit;
    return unlit ? kVisibleBit : (kVisibleBit | kGiGeometryBit);
}

void OgreScene::applyNodeVisibilityFlags(Node &n) {
    // The material's unlit-ness was recorded when the geometry attached, so a
    // lit mesh that is marked helper and then unmarked gets its kGiGeometryBit
    // back. Reading it off the item's CURRENT flags could not do that: a helper
    // carries kHelperBit alone.
    if (n.item) n.item->setVisibilityFlags(itemVisibilityFlags(n, n.materialUnlit));
    const Ogre::uint32 on = n.helper ? kHelperBit : kVisibleBit;
    if (n.billboards) n.billboards->setVisibilityFlags(n.visible ? on : 0u);
    if (n.particleDef) n.particleDef->setVisibilityFlags(n.visible ? on : 0u);
}

void OgreScene::setNodeHelper(NodeId id, bool helper) {
    auto it = mNodes.find(id);
    if (it == mNodes.end()) return;
    if (it->second.helper == helper) return;
    it->second.helper = helper;
    applyNodeVisibilityFlags(it->second);
}

bool OgreScene::nodeHelper(NodeId id) const {
    auto it = mNodes.find(id);
    return it != mNodes.end() && it->second.helper;
}

void OgreScene::setNodeLightMask(NodeId id, unsigned mask) {
    auto it = mNodes.find(id);
    if (it == mNodes.end()) return;
    // Remembered even when nothing is attached yet: attachMesh applies it to
    // the Item it creates, which is also what makes the mask survive the Item
    // rebuild a material swap performs.
    it->second.lightMask = Ogre::uint32(mask);
    if (it->second.item) it->second.item->setLightMask(Ogre::uint32(mask));
    // Deliberately NOT pushed to billboards or particle systems: a PFX2
    // definition is pooled and shared between nodes (mParticleDefPool), so a
    // per-node mask on one would silently mask every node recycling it.
}

unsigned OgreScene::nodeLightMask(NodeId id) const {
    auto it = mNodes.find(id);
    return it == mNodes.end() ? 0xFFFFFFFFu : unsigned(it->second.lightMask);
}

void OgreScene::setNodeVisible(NodeId id, bool visible) {
    JAH_TRY {
        auto it = mNodes.find(id);
        if (it == mNodes.end()) return;
        it->second.visible = visible;
        if (it->second.node) it->second.node->setVisible(visible, true);
        // The billboard set hangs off the STATIC root (world-space positions),
        // not off this node, so the cascade above never reaches it. And
        // setVisible() is USELESS for PFX2 objects: ParticleSystemManager2::
        // _addToRenderQueue tests getVisibilityFlags(), which strips the
        // LAYER_VISIBILITY bit setVisible toggles. Toggle the user flags.
        // A HELPER's billboards carry kHelperBit, not kVisibleBit (P1b): the
        // light icons are the biggest single thing probe captures used to eat.
        const Ogre::uint32 on = it->second.helper ? kHelperBit : kVisibleBit;
        if (it->second.billboards) it->second.billboards->setVisibilityFlags(visible ? on : 0u);
        // Same trap, same fix, for a simulated particle system — except the flag
        // lives on the DEFINITION, not on the instance: _addToRenderQueue tests
        // the def (OgreParticleSystemManager2.cpp:762-765). Hiding the def is
        // also what makes already-emitted particles disappear at once instead
        // of finishing their lives on screen.
        if (it->second.particleDef)
            it->second.particleDef->setVisibilityFlags(visible ? on : 0u);
    } JAH_CATCH(mError, );
}

// ---- Lights ----
bool OgreScene::setLight(NodeId id, const LightDesc &d) {
    auto it = mNodes.find(id);
    if (it == mNodes.end()) { mError = "setLight: unknown node"; return false; }
    JAH_TRY {
        Node &n = it->second;
        if (!n.light) {
            // The document's convention (IrisGL LightNode::getLightDir): lights shine
            // down their node's -Y. Ogre lights shine down -Z, so the light rides an
            // internal child node pitched -90 about X. Getting this wrong leaves every
            // scene lit near-horizontally: dark viewports and shadows nobody can see.
            n.lightNode = n.node->createChildSceneNode();
            n.lightNode->setOrientation(Ogre::Quaternion(Ogre::Radian(-Ogre::Math::HALF_PI), Ogre::Vector3::UNIT_X));
            n.light = mSceneMgr->createLight();
            n.lightNode->attachObject(n.light);
        }
        Ogre::Light *L = n.light;
        switch (d.type) {
        case LightType::Directional: L->setType(Ogre::Light::LT_DIRECTIONAL); break;
        case LightType::Point:       L->setType(Ogre::Light::LT_POINT); break;
        case LightType::Spot:        L->setType(Ogre::Light::LT_SPOTLIGHT); break;
        // Area lights emit down the light's -Z, exactly like spot/directional,
        // so the -Y child-node convention below already orients them.
        case LightType::Area:
            L->setType(d.accurate ? Ogre::Light::LT_AREA_LTC : Ogre::Light::LT_AREA_APPROX);
            break;
        }
        L->setDiffuseColour(toOgre(d.colour));
        L->setSpecularColour(toOgre(d.colour));
        // LIGHTING CHANNELS, light side. Free at the default (all ones), and
        // read by nothing at all unless the engine was built with
        // OGRE_CONFIG_ENABLE_FINE_LIGHT_MASK_GRANULARITY=ON — which
        // build-ogre.sh passes and guards on the installed OgreBuildSettings.h,
        // because a stale install turns this line into a silent no-op.
        L->setLightMask(Ogre::uint32(d.lightMask));
        // Ogre-Next cannot render shadows for area lights (and our shadow node
        // only lists directional/point/spot); never mark them casters.
        L->setCastShadows(d.type == LightType::Area ? false : d.castShadows);
        if (d.type == LightType::Area) {
            L->setRectSize(Ogre::Vector2(std::max(d.rectWidth, 0.01f), std::max(d.rectHeight, 0.01f)));
            L->setDoubleSided(d.doubleSided);
            // HlmsPbs only pays for area lights in scenes that contain one
            // (the LightsAreaApprox/Ltc shader properties are gated on the
            // live light list), but the LTC/BRDF lookup textures must be
            // resident before an area light is drawn. Loading them reserves
            // a texture slot in every pass, so do it lazily, once, here —
            // never for scenes without area lights. The .dds files ship with
            // the staged Common material scripts (registerCommonResources).
            // The "once" flag lives in lightextras beside the area-light
            // budgets and is reset by its shutdown(): as a function-local
            // static it survived Engine destruction and the SECOND Engine's
            // area lights then rendered with no LTC matrix loaded.
            lightextras::armLtcMatrix(mRoot);
            // Ogre budgets ONE forward area light of each kind and silently
            // drops the rest — a scene's second area light renders nothing
            // until this runs. Same lazy arm point, same reasoning.
            lightextras::armAreaLightBudgets(mRoot);
        }
        // HlmsPbs divides diffuse by pi (Lambert BRDF); IrisGL's default shader does
        // not, so matching legacy exposure needs powerScale = intensity * pi. (An
        // earlier 'calibration' removed this while the light DIRECTION mapping was
        // broken — the overexposure it fixed was side-lit faces, not the scale.)
        L->setPowerScale(d.intensity * Ogre::Math::PI);
        if (d.type != LightType::Directional) {
            // THE AUTHORED RANGE IS THE RANGE (LIGHTING_FIX fix 4 / F-A1..A4).
            //
            // This used to be `setAttenuationBasedOnRadius(range, 0.01f)`, which
            // takes the range as the radius of the falloff CURVE and then solves
            // for the distance at which the light dims to 1% — and that distance
            // is 14.1 times the number the user typed (OgreLight.cpp:194-217:
            // q = 0.5/r^2, threshold 0.01 => mRange = sqrt(199) * r). So a light
            // authored at range 5 lit out to 70 units, the Forward+ cut-off sat
            // 14x too far away, and the range wire the editor draws at 5 was a
            // decoration rather than a statement about the picture.
            //
            // Same curve, range authored: keep Ogre's own constants (the shader
            // hardcodes the 0.5 numerator, so the curve must keep its 0.5
            // constant term) and set mRange to R directly.
            //
            // KNOWN AND ACCEPTED CONSEQUENCE: the Forward+ fade now ramps across
            // [0, R] instead of [0, 14.1R], so mid-range brightness drops
            // measurably — the fade at d = R/2 goes from ~0.96 to 0.5. Any
            // scene authored against the old 14x reach is dimmer and must be
            // re-lit. MEASURED on the 203-suite gate: no shipped pixel suite
            // moved, because every one of them lights with a DIRECTIONAL light,
            // whose branch this does not touch. lights.falloff is the suite
            // that pins the new curve.
            const float r = std::max(d.range, 0.01f);
            L->setAttenuation(r, 0.5f, 0.0f, 0.5f / (r * r));
        } else {
            // Ogre's default attenuation (const 0.5, quad 0.5) is never used to
            // SHADE a directional light, but Instant Radiosity attenuates its
            // rays with it — over the tens of units a ray travels from outside
            // the scene that quadratic term crushed every bounce to black.
            // The InstantRadiosity sample sets exactly this: no falloff.
            L->setAttenuation(std::numeric_limits<Ogre::Real>::max(), 1.0f, 0.0f, 0.0f);
        }
        if (d.type == LightType::Spot) {
            // HALF ANGLE IN, FULL ANGLE OUT (LIGHTING_FIX fix 5 / F-S1).
            // `spotAngleDegrees` is the half angle — the document's meaning
            // since forever, and the one the editor's cone wire is built from —
            // while `setSpotlightRange` wants the full apex angle. Doubling here
            // is the whole of the fix: without it every spot rendered a cone
            // half the width of the wire drawn around it.
            //
            // Clamped to [1, 85] BEFORE doubling, i.e. a full cone in [2, 170].
            // 85 rather than 89: past ~90 of half angle
            // `getSpotlightTanHalfAngle()` runs away and Forward+ falls back to
            // its conservative OBB test for the light (OgreForwardClustered.cpp:
            // 632), and a >170 degree "spot" is a point light with extra cost.
            const float halfDeg = std::min(std::max(d.spotAngleDegrees, 1.0f), 85.0f);
            const float outerFull = 2.0f * halfDeg;
            const float innerFull =
                outerFull * (1.0f - std::min(std::max(d.spotSoftness, 0.0f), 0.99f));
            L->setSpotlightRange(Ogre::Degree(innerFull), Ogre::Degree(outerFull),
                                 std::max(d.spotFalloff, 0.0f));
        }

        // IES profile and area-light mask: assigned ONLY when the requested path
        // actually changed. This function runs for every light on every mirror
        // sync (60 Hz); LightProfiles::build() recreates and re-uploads a GPU
        // texture, and a mask costs a decode + resize + mip chain + upload.
        //
        // Honesty about what the renderer does with these — the host UI says the
        // same thing, because there is no engine signal for either:
        //   * a profile shapes SPOT lights always, POINT lights only while they
        //     cast no shadows (a shadow-casting point light is shaded from the
        //     pass buffer, whose point loop has no profile term), and never
        //     directional or area lights;
        //   * a mask applies to the area-light APPROXIMATION only — LTC
        //     ("accurate") ignores it — so accurate mode drops it here rather
        //     than leaving a stale slice bound.
        {
            const bool profileApplies =
                d.type == LightType::Spot ||
                (d.type == LightType::Point && !d.castShadows);
            const std::string wantProfile = profileApplies ? d.iesProfilePath : std::string();
            if (wantProfile != n.lightProfilePath) {
                std::string err;
                if (lightextras::assignProfile(mRoot, L, wantProfile, err)) {
                    n.lightProfilePath = wantProfile;
                } else {
                    mError = err;
                    // Remember the REQUEST anyway: a failing path must not be
                    // retried (and re-logged) every single frame.
                    n.lightProfilePath = wantProfile;
                }
            }

            const bool maskApplies = d.type == LightType::Area && !d.accurate;
            const std::string wantMask = maskApplies ? d.texturePath : std::string();
            if (wantMask != n.lightMaskPath) {
                std::string err;
                if (!lightextras::assignAreaMask(mRoot, L, wantMask, err)) mError = err;
                n.lightMaskPath = wantMask;
            }
        }
        // Lights shine down their node's -Y once attached (document convention).
        return true;
    } JAH_CATCH(mError, false);
}

bool OgreScene::removeLight(NodeId id) {
    auto it = mNodes.find(id);
    if (it == mNodes.end() || !it->second.light) return false;
    JAH_TRY {
        it->second.light->detachFromParent();
        mSceneMgr->destroyLight(it->second.light);
        it->second.light = nullptr;
        // A recreated light starts with no profile and no mask: forget what the
        // dead one carried, or the next setLight would skip re-assigning it.
        it->second.lightProfilePath.clear();
        it->second.lightMaskPath.clear();
        if (it->second.lightNode) { mSceneMgr->destroySceneNode(it->second.lightNode); it->second.lightNode = nullptr; }
        invalidateGiCaches();   // a vanished light must stop bouncing (VCT re-injects)
        return true;
    } JAH_CATCH(mError, false);
}

Ogre::SceneManager *OgreScene::sceneManager() const { return mSceneMgr; }

void OgreScene::destroy() {
    if (!mSceneMgr) return;
    JAH_TRY {
        // FIRST, before anything else in this scene goes: the overlay system's
        // render-queue listener is registered on THIS SceneManager, and the
        // teardown order the component needs is
        // removeRenderQueueListener -> destroy scenes -> delete OverlaySystem
        // -> delete Root (OgreOverlayHud.cpp's header).
        hud::detach(mSceneMgr);
        teardownGi();   // VPL lights die while the SceneManager is still alive
        // The atmosphere destroys its Rectangle2D THROUGH the SceneManager, so it
        // has to go while that is still alive (teardown law: components, then the
        // manager).
        destroyAtmosphere();
        // Before the nodes: PlanarReflections holds raw Renderable pointers, and
        // it destroys its own cameras through the SceneManager. It also has to
        // unbind itself from the process-wide HlmsPbs, which its destructor
        // (like VctLighting's) does not do.
        teardownPlanar();
        destroySky();   // also unbinds + destroys the reflection cubemap
        for (auto &kv : mNodes) releaseNode(kv.first, kv.second);
        mNodes.clear();
        // The helper overlay queue's depth anchor: an entity in this
        // SceneManager's memory manager, so it dies before the manager does.
        releaseQueueDepthAnchor();
        for (auto &kv : mMaterials) {
            Ogre::Hlms *hlms = hlmsFor(kv.second);
            if (hlms->getDatablock(Ogre::IdString(kv.second.datablockName)))
                hlms->destroyDatablock(Ogre::IdString(kv.second.datablockName));
        }
        mMaterials.clear();
        for (auto &kv : mTextures) releaseTextureRec(kv.second);
        mTextures.clear();
        mTextureIndex.clear();
        Ogre::MeshManager &mm = Ogre::MeshManager::getSingleton();
        for (auto &kv : mMeshes) {
            kv.second.mesh.reset();
            if (mm.resourceExists(kv.second.name)) mm.remove(kv.second.name);
        }
        mMeshes.clear();
        mRoot->destroySceneManager(mSceneMgr);
        // AFTER the SceneManager, deliberately. Particle definitions are freed
        // only by ~ParticleSystemManager2 (there is no destroyParticleSystemDef),
        // and a definition is a Renderable linked to its datablock —
        // ~HlmsDatablock asserts on any renderable still holding it. So the
        // definitions must die first; then these datablocks, which belong to the
        // process-wide HlmsManager and would otherwise outlive the scene
        // forever. mParticleDatablocks covers every def this scene created,
        // whether it ended on a node or in the recycling pool.
        {
            auto *hlmsUnlit = mRoot->getHlmsManager()->getHlms(Ogre::HLMS_UNLIT);
            for (auto &kv : mParticleDatablocks) {
                if (hlmsUnlit->getDatablock(Ogre::IdString(kv.second)))
                    hlmsUnlit->destroyDatablock(Ogre::IdString(kv.second));
            }
            mParticleDatablocks.clear();
            mParticleDefPool.clear();
        }
    } JAH_CATCH(mError, );
    FogHlmsListener::unregisterScene(mSceneMgr);
    mSceneMgr = nullptr;
}

void OgreScene::detachItem(NodeId id, Node &n) {
    // BEFORE the Item dies: PlanarReflections keeps a raw Renderable* and its
    // header says "You must call removeRenderable before destroying the
    // Renderable". The reflector FLAG survives in mReflectors, so a node that is
    // given a new mesh re-arms in attachMesh.
    if (n.item) disarmReflector(id, n);
    // AND BEFORE IT LEAVES ITS NODE: skeleton sharing (AVATAR_RIG_PERF_SPEC
    // §3.3). `detachFromParent()` below ends in
    // `mSkeletonInstance->setParentNode(nullptr)` (OgreMovableObject.cpp:155),
    // and on a SLAVE that instance is the MASTER's — the whole character would
    // render at the origin from the next frame. So a slave stops sharing first
    // (getting its own instance, posed and re-parented), and a master hands
    // every slave back its own instance before its Item goes anywhere. The
    // pairing is NOT kept: the host re-arms sharing on its next sync, which is
    // the same shape the mirror already has for every other engine-side fact.
    if (n.item && (n.shareSource || !n.shareFollowers.empty())) {
        releaseShareFollowers(id, n);
        if (n.shareSource) {
            auto sit = mNodes.find(n.shareSource);
            unshareFollower(id, n, sit == mNodes.end() ? nullptr : &sit->second);
        }
    }
    // ANY Item, not just one with a mesh reference. It used to be
    // `n.item && n.meshRef`, so an Item whose bookkeeping had been lost — a
    // throw between createItem and the meshRef assignment is enough — was
    // ORPHANED: still attached to the scene node, still drawing, with the next
    // attach overwriting the only pointer to it. An engine that cannot destroy
    // a renderable it created has no way back to one-item-per-node.
    if (n.item) {
        // Only GI-participating (lit) geometry invalidates — detaching a selection
        // outline or wire overlay must not trigger a re-voxelize. BEFORE the
        // destroy: the voxelizer/IR hold raw pointers into the dying geometry.
        if (n.item->getVisibilityFlags() & kGiGeometryBit) invalidateGiCaches();
        n.item->detachFromParent(); mSceneMgr->destroyItem(n.item); n.item = nullptr;
    }
    n.meshRef = 0; n.materialRef = 0;
}

// Pose FOLLOWING (followSkeleton) is bookkeeping plus one copy per frame. The
// pairing is INTENT and survives the Items: an editor re-attaches geometry all
// the time — a material change, a mesh swap — and a silhouette that follows a
// character must come back posed, not frozen at bind. Nothing has to be undone
// when an Item dies (no shared Ogre state), so only node destruction drops it.
void OgreScene::dropSkeletonFollowers(NodeId id, Node &n) {
    for (NodeId followerId : n.skeletonFollowers) {
        auto fit = mNodes.find(followerId);
        if (fit != mNodes.end() && fit->second.skeletonSource == id)
            fit->second.skeletonSource = 0;
    }
    n.skeletonFollowers.clear();
    if (n.skeletonSource) {
        auto sit = mNodes.find(n.skeletonSource);
        if (sit != mNodes.end()) {
            auto &list = sit->second.skeletonFollowers;
            list.erase(std::remove(list.begin(), list.end(), id), list.end());
        }
        n.skeletonSource = 0;
    }
}

size_t OgreScene::itemCount(NodeId id) const {
    auto it = mNodes.find(id);
    if (it == mNodes.end() || !it->second.node) return 0;
    size_t n = 0;
    Ogre::SceneNode *sn = it->second.node;
    for (size_t i = 0; i < sn->numAttachedObjects(); ++i)
        if (dynamic_cast<Ogre::Item *>(sn->getAttachedObject(i))) ++n;
    return n;
}

void OgreScene::releaseNode(NodeId id, Node &n) {
    // An ADOPTED node belongs to the document, which may already have destroyed
    // it (a delete, or a migration into another scene manager). Drop the pointer
    // FIRST: everything below either destroys it or walks its children, and both
    // are read-after-destroy on a node we do not own. Its engine-owned children — a
    // light's -Y adapter, a decal's projector box — were re-homed under the
    // scene root by iris::graph precisely so that they are still destroyable
    // here.
    if (!n.owned) n.node = nullptr;
    // Order: renderable off the node -> item (drops the datablock link and one
    // mesh ref) -> datablock -> node -> our mesh ref -> the mesh itself.
    releaseBillboards(n);
    // The particle INSTANCE dies with the node; the definition cannot be
    // destroyed at all (no such API) and returns to the recycling pool, hidden.
    releaseParticleSystem(n);
    // A destroyed node stops being a reflector for good (unlike detachItem,
    // which only swaps the mesh) — drop the actor AND the flag, before the Item
    // the component tracks by raw pointer dies.
    disarmReflector(id, n);
    mReflectors.erase(id);
    // Invalidate BEFORE anything dies (IR frees its by-pointer caches inside):
    // VCT holds the raw Item*, IR caches the mesh's VAO and any node-owned mesh.
    if (n.mesh || (n.item && (n.item->getVisibilityFlags() & kGiGeometryBit)))
        invalidateGiCaches();
    // The node is going away for good, so its pose-following pairings go with
    // it (detachItem does NOT: an Item swap keeps them, so a re-attached
    // character still drags its silhouette along).
    dropSkeletonFollowers(id, n);
    // ...and the SHARING pairings, which unlike the pose-following ones are
    // live Ogre state: a master's node dying under its slaves would leave them
    // reading a recycled SoA slot through Bone::_setNodeParent's raw pointer.
    // (detachItem above has usually done this already; a node with no Item at
    // all still has to have its bookkeeping dropped.)
    dropShareFollowers(id, n);
    if (n.item)  { n.item->detachFromParent();  mSceneMgr->destroyItem(n.item);   n.item = nullptr; }
    n.meshRef = 0; n.materialRef = 0;
    // The internal light child must go before the reparent loop below would leak it to root.
    if (n.light) { n.light->detachFromParent(); mSceneMgr->destroyLight(n.light); n.light = nullptr; }
    if (n.lightNode) { mSceneMgr->destroySceneNode(n.lightNode); n.lightNode = nullptr; }
    // Same for the decal's internal child. Note releaseDecal only tears the
    // objects down; the SceneManager's atlas bindings are refreshed by the
    // caller (removeNode) once, after the node is gone.
    releaseDecal(n);
    if (n.node) {   // children survive: re-parent them to the root
        Ogre::SceneNode *root = mSceneMgr->getRootSceneNode(Ogre::SCENE_DYNAMIC);
        while (n.node->numChildren() > 0) {
            Ogre::Node *c = n.node->getChild(0);
            n.node->removeChild(c); root->addChild(c);
        }
    }
    if (!n.datablockName.empty()) {
        auto *hlmsPbs = mRoot->getHlmsManager()->getHlms(Ogre::HLMS_PBS);
        if (hlmsPbs->getDatablock(Ogre::IdString(n.datablockName)))
            hlmsPbs->destroyDatablock(Ogre::IdString(n.datablockName));
        n.datablockName.clear();
    }
    if (n.node) { mSceneMgr->destroySceneNode(n.node); n.node = nullptr; }
    n.mesh.reset();
    if (!n.meshName.empty()) {
        Ogre::MeshManager &mm = Ogre::MeshManager::getSingleton();
        if (mm.resourceExists(n.meshName)) mm.remove(n.meshName);
        n.meshName.clear();
    }
}

Ogre::SceneNode *OgreScene::node(NodeId id) const {
    auto it = mNodes.find(id);
    return it == mNodes.end() ? nullptr : it->second.node;
}

NodeId OgreScene::track(const Node &n) { mNodes[++mNextId] = n; return mNextId; }

void OgreScene::addObjectCounts(ObjectCounts &out) const {
    // Registry sizes, not Ogre object counts: these are the ids this boundary
    // has handed out and still honours. That is deliberately the leak-relevant
    // number — a record kept after its document node died holds the Ogre
    // object alive too, and a record freed while the Ogre object leaked would
    // be a different (and louder) bug.
    out.nodes     += unsigned(mNodes.size());
    out.meshes    += unsigned(mMeshes.size());
    out.materials += unsigned(mMaterials.size());
    out.textures  += unsigned(mTextures.size());
}

}}}  // namespace jahshaka::engine::detail
