// OgreScene core: lifetime, node hierarchy, transforms, visibility, lights and
// the teardown helpers. Meshes, materials, sky, GI and particles live in their
// own translation units.
#include "EnginePrivate.h"

#include <cstdlib>
#include "HlmsAtom.h"

#include <cmath>
// SURFACE-CACHE phase 2: the cache is a unique_ptr member and the per-frame
// pass lives here, so this TU needs the Component's complete type.
#include "SurfaceCache.h"
#include <OgreHlmsDatablock.h>
#include <OgreItem.h>
#include <OgreSubItem.h>

namespace jahshaka { namespace engine { namespace detail {

OgreScene::OgreScene(Ogre::Root *root, Ogre::SceneManager *sm, const std::string &name,
                     std::string &errorSink)
    : mRoot(root), mSceneMgr(sm), mName(name), mError(errorSink) {
    // WHAT THIS SCENE'S PASSES BIND (SceneGiBinding, OgreGi.cpp): nothing yet.
    registerSceneGiBinding(mSceneMgr, &mGiBinding);
    // THE VISIBILITY BUFFER'S MEASUREMENT SWITCH (never a mode): the whole process
    // draws through PBS — the cost table's reference arm in the app, and the A/B
    // that attributes a moved picture to the split.
    if (std::getenv("JAHSHAKA_ATOM_DRAW_OFF")) mAtomDrawEnabled = false;
}

OgreScene::~OgreScene() { destroy(); }

const std::string &OgreScene::name() const { return mName; }

// THE PER-SCENE SHADOW REQUEST (ENGINEERING_DEBT_SPEC.md item 4; ShadowDesc
// carries the model). The backend has one filter and one shadow atlas for the
// whole process, so "per scene" is a SHAPE, not a new capability: the scene's
// resolved answer is applied to the global state, and a request for what is
// already in force does nothing. That comparison is the whole point — it is
// the read-before-write guard every host used to write by hand around two
// global Engine setters, and getting it wrong means rebuilding the shadow node
// and every workspace that references it, every frame.
void OgreScene::setShadowSettings(const ShadowDesc &desc) {
    mShadowDesc = desc;
    if (!mEngine) return;
    if (desc.hasFilter && mEngine->shadowFilter() != desc.filter)
        mEngine->setShadowFilter(desc.filter);
    if (desc.resolution) {
        // Clamp before comparing: the engine clamps inside its setter, so an
        // out-of-range request would otherwise never equal what it produced and
        // would ask for a rebuild on every frame.
        const unsigned want = std::min(8192u, std::max(256u, desc.resolution));
        if (mEngine->shadowResolution() != want) mEngine->setShadowResolution(want);
    }
}

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
    // AND THAT IS THE ENVIRONMENT EVERY READER SEES (PHOTON-ENV-1): a scene with
    // no sky cube has no environment but these coefficients, and every escape —
    // the voxel cones', the bounce's, the field's fallback — reads the SH in its
    // own direction (jah_environment.glsl). One ambient, one convention: the
    // same numbers the SH arm renders outside a volume (ledger 177 defect A was
    // a second, unscaled copy of this pair handed to the VCT arm).
    setAmbientSh(sh);
}

// THE ENVIRONMENT, INTO EVERY CASCADE'S BOUNCE (PHOTON-ENV-1). The pixel shader
// reads the environment from its own pass slot (FogHlmsListener, `jah_env`); the
// one other reader is each VctLighting's BOUNCE job, which runs inside
// VctLighting::update and sees the sky where a bounce cone escapes. It gets the
// cube the pass gets (none while the Sky Light is out — the sky goes dark by not
// being bound), the per-channel gain, and the SH coefficients in WORLD axes.
//
// It REPLACES the hemisphere pair this function used to push (upstream's
// VctLighting::setAmbient, `ambientUpperHemi/LowerHemi` in the probe pass
// buffer, and the `vct_ambient_hemisphere` variant its epsilon existed to pin):
// the pair was a two-band pole approximation of the SH that every escape was
// filled with. Called before every VctLighting::update, and whenever the
// environment's cube, gain or coefficients change.
void OgreScene::applyVctEnvironment() {
    if (!mVctLighting) return;
    applyCascadeEnvironment(mVctLighting);
    // EVERY CASCADE, not just the head (PHOTON_SPEC P0 rule 4): each cascade's
    // bounce job reads its own copy. Cascade 0 is mVctLighting and was just done.
    for (size_t i = 1; i < mVctCascades.size(); ++i)
        applyCascadeEnvironment(mVctCascades[i].lighting);
}

void OgreScene::applyCascadeEnvironment(Ogre::VctLighting *lighting) {
    if (!lighting) return;
    // THE CLOUD LAYER'S SHADOW ON THE INJECTION (CLOUDS-2D-2), bound or cleared
    // on the shared job for THIS volume, beside its environment.
    bindCloudInjection(lighting);
    JAH_TRY {
        Ogre::TextureGpu *cube = (mReflectionTex && mEnvLightScale > 0.0f) ? mReflectionTex : nullptr;
        lighting->setEnvironment(cube,
                                 Ogre::ColourValue(mEnvLightGain.r, mEnvLightGain.g, mEnvLightGain.b, 1.0f),
                                 mLastAmbientSh);
    } JAH_CATCH(mError, );
}

OgreScene::RayEnvironment OgreScene::rayEnvironment() const {
    RayEnvironment env;
    if (mReflectionTex && mEnvLightScale > 0.0f) {
        env.cube = mReflectionTex;
        env.colour[0] = mEnvLightGain.r;
        env.colour[1] = mEnvLightGain.g;
        env.colour[2] = mEnvLightGain.b;
    } else {
        // The constant band IS the mean radiance in this basis (irradianceSH's
        // first term): what a direction-free environment answers.
        for (int i = 0; i < 3; ++i) env.colour[i] = std::max(0.0f, mLastAmbientSh[i]);
    }
    return env;
}

// THE ENVIRONMENT CHANGED UNDER THE VOXELS (PHOTON-ENV-1). The bounce injection
// reads it (an escaping bounce cone sees the sky), so a change is what a light
// write is to an injection: the serial every injection and every incremental
// settle trusts moves, the in-motion tick stops skipping cascades injected
// before it, and an owed settle restarts over the new environment. A chain that
// is bouncing (more than one bounce) is owed a settle outright — nothing else
// would re-inject a still scene whose only change was its sky. And the
// irradiance field re-integrates: its probe rays read the environment where
// they escape, so its atlas holds the sky (progressively, over the converged
// data — the field never flashes).
void OgreScene::noteEnvironmentChanged() {
    ++mGiLightWriteSerial;
    applyVctEnvironment();
    // A BOUNCING chain is owed a settle (the bounce job reads the environment
    // where its cones escape); with one bounce the voxels hold the direct light
    // only and the pixel reads the environment itself.
    if (mGi.numBounces > 1) oweChainSettle();
    if (mIfd) reintegrateFieldAfterInjection();
}

void OgreScene::setAmbientSh(const float sh[27]) {
    mSkyAmbientOwned = false;   // the host lights this scene itself
    applyAmbientSh(sh, GiStaleReason::Ambient);
}

void OgreScene::applyAmbientSh(const float sh[27], GiStaleReason why) {
    // THE PROBE CACHE'S AMBIENT INPUT (ENGINE_CACHE_POLICY_SPEC P7): a probe
    // capture is lit by it. Compared by value, because the host re-pushes the
    // ambient every time a page takes the screen back and that must cost no
    // re-capture. (setAmbient funnels through here, so both are covered.)
    // GUARDED AT THE SOURCE (PHOTON-FIELD-ROTATE-1, F5): a non-finite coefficient,
    // or a negative constant band (the mean radiance - the higher bands are signed
    // by nature), is refused with one log line and the previous SH kept: every probe
    // ray's escape reads these, and the field's mean would carry a bad value.
    for (int i = 0; i < 27; ++i) {
        if (!std::isfinite(sh[i]) || (i < 3 && sh[i] < 0.0f)) {
            Ogre::LogManager::getSingleton().logMessage(
                "Jahshaka GI: an ambient SH with a non-finite or negative-mean coefficient (" +
                std::to_string(i) + " = " + std::to_string(sh[i]) +
                ") was refused; the previous SH stands");
            return;
        }
    }
    if (!mAmbientShKnown || std::memcmp(sh, mLastAmbientSh, sizeof mLastAmbientSh) != 0) {
        std::memcpy(mLastAmbientSh, sh, sizeof mLastAmbientSh);
        mAmbientShKnown = true;
        staleProbeGrid(why);                             // a no-op before a grid exists
        // ...and the bounce injection reads the environment (PHOTON-ENV-1).
        noteEnvironmentChanged();
        // (The irradiance field owes nothing here: a VOXEL-fed probe cone-traces
        // the volume live, so the new ambient reaches it on its next integration
        // without a re-arm. The re-arm that used to stand here existed for the
        // RASTERISED probe source, whose probes RENDER the scene and therefore
        // baked the old ambient into their captured faces; that source was
        // deleted 2026-09-17, lane FIELD-RASTER-CRUD.)
    }
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
        mSceneMgr->setAmbientLight(flat, flat, Ogre::Vector3::UNIT_Y, envmapScaleForPass(), 0u);
    } JAH_CATCH(mError, );
}

// THE ENVIRONMENT'S GAIN AS A SPECULAR LIGHT (SMOKE-ENGINE-1 item 2, the
// owner's "with all lights off the GPU sky still lights the scene").
//
// The DIFFUSE half of the sky has been gated on the Sky Light since SKY-GPU:
// the mirror scales the sky's own integral by the light's intensity and tint
// and pushes the product through setAmbientSh, so with no Sky Light it pushes
// 27 zeros and a matte surface goes black. The SPECULAR half had no gate at
// all — reflectionTexFor handed every material the sky's captured cube
// whatever the scene's lights were doing, and a mirror sphere reflected the sky
// BYTE-IDENTICALLY with every light hidden, the Sky Light included.
//
// One number closes it, because the pin already has the right one:
// `SceneManager::setAmbientLight`'s fourth argument is HlmsPbs' envmapScale, it
// rides `ambientUpperHemi.w`, and the pixel shader multiplies every env-probe
// sample by it (`envS.xyz *= midf3_c( passBuf.ambientUpperHemi.w )`, plus the
// `ApplyEnvMapScale` piece on the clear-coat and diffuse-GI paths). So the sky
// reflects at the Sky Light's gain and vanishes with it, while the sky itself
// stays VISIBLE: it is a picture until a Sky Light makes it a light.
//
// AT EXACTLY 1.0 NOTHING MOVES, and that is not a hope: HlmsPbs sets the
// `envmap_scale` property only `if( envMapScale != 1.0f )`, so a default scene
// (Sky Light intensity 1, white) generates the identical shader it always did
// and renders the identical pixels.
void OgreScene::setEnvironmentLight(const Colour &gain) {
    // THE FIELD'S PER-CHANNEL INPUTS ARE GUARDED AT THE SOURCE (PHOTON-FIELD-ROTATE-1,
    // F5): a non-finite gain would reach every probe ray's escape and the field's
    // running mean would carry it until the next change. Refused, the previous gain
    // kept, one log line. (A negative channel is clamped to 0 below, as before.)
    if (!std::isfinite(gain.r) || !std::isfinite(gain.g) || !std::isfinite(gain.b)) {
        Ogre::LogManager::getSingleton().logMessage(
            "Jahshaka GI: a non-finite environment-light gain was refused; the previous gain stands");
        return;
    }
    const Colour c(std::max(gain.r, 0.0f), std::max(gain.g, 0.0f), std::max(gain.b, 0.0f), 1.0f);
    // Rec.709 luminance; the weights sum to exactly 1.0f in float, so a white
    // gain of 1 is exactly 1.0f and HlmsPbs sets no envmap_scale property.
    const float g = 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
    // THE AMBIENT IS SH x THIS GAIN, AND THE ENGINE FORMS IT (PHOTON-SKY-TRANSIENT-1):
    // this push hands the ambient to the engine and re-asserts it even when the
    // gain did not move (a host's first push, a page return) — what replaces the
    // host's own SH push.
    mSkyAmbientOwned = true;
    if (c.r == mEnvLightGain.r && c.g == mEnvLightGain.g && c.b == mEnvLightGain.b) {
        applySkyAmbient(GiStaleReason::Ambient);
        return;
    }
    const bool wasLit = mEnvLightScale > 0.0f;
    mEnvLightGain = c;
    mEnvLightScale = g;
    applySkyAmbient(GiStaleReason::Ambient);
    // Every escape reads the environment at this gain, the bounce injection
    // included: what the voxels hold changes with it.
    noteEnvironmentChanged();
    // CROSSING ZERO UNBINDS THE SKY CUBE, it does not merely scale it by zero
    // (reflectionTexFor carries that gate). Rebinding is what makes "no Sky
    // Light, no sky reflection" EXACT even in a pass whose scale has to stay at
    // 1.0 for somebody else's sake — see envmapScaleForPass. Only on the edge:
    // a slider drag between two lit values costs one float.
    if (wasLit != (g > 0.0f)) applyReflectionToAll();
    else                      refreshEnvmapScale();
}

// THE SCALE IS A SNAPSHOT AND THE EXEMPTION IS NOT (round-1 self-review, caught
// by gi.budget and gi.probe_inputs going red). envmapScaleForPass() asks
// whether PCC owns the env-probe slot, and that answer changes when the hybrid
// arm is built or torn down — long after the Sky Light was last pushed. The
// scale lives in the ambient pass data, which is written once per push, so a
// scene that acquired its probes after its ambient kept the sky-cube answer and
// dimmed its PROBE reflections with the sky's gain: a closed room whose Sky
// Light is 0 (gi.probe_inputs) or which has none at all (gi.budget) stopped
// reflecting its own walls. So the binding change re-pushes, through the one
// funnel that already runs on every PCC transition.
void OgreScene::refreshEnvmapScale() {
    // THE SKY'S PASS-LEVEL SLOT RIDES THE SAME FUNNEL (lane SKY-FALLBACK-1).
    // This function is the one place every edge that can move the sky's
    // environment passes through: a sky rebuild and a PCC transition reach it
    // via applyReflectionToAllImpl, and a Sky Light gain change between two
    // non-zero values reaches it directly (setEnvironmentLight's else
    // branch). The listener holds the cube for the passes where the env-probe
    // slot is the probe array's — see FogHlmsListener::SkyEnvState.
    {
        FogHlmsListener::SkyEnvState sky;
        if (mReflectionTex && mEnvLightScale > 0.0f) {
            sky.cube = mReflectionTex;
            sky.gain[0] = mEnvLightGain.r;
            sky.gain[1] = mEnvLightGain.g;
            sky.gain[2] = mEnvLightGain.b;
            sky.numMipmaps = float(mReflectionTex->getNumMipmaps());
        }
        FogHlmsListener::setSkyEnv(mSceneMgr, sky);
        // ...and the same cube to every cascade's bounce (the cube's identity is
        // what changes on this funnel's edges; its contents move with the SH,
        // which notes the change itself).
        if (sky.cube != mEnvCubeSeen) {
            mEnvCubeSeen = sky.cube;
            noteEnvironmentChanged();
        }
    }
    // Re-push what we already hold rather than waiting for the next ambient
    // edit. setAmbientSh's own change guard compares the COEFFICIENTS, which
    // have not moved, so this costs no probe re-capture and no raster-field
    // re-integration — exactly right, because a probe capture is not lit by
    // envmapScale.
    JAH_TRY {
        const float *sh = mLastAmbientSh;
        const Ogre::ColourValue flat(sh[0], sh[1], sh[2], 1.0f);
        mSceneMgr->setAmbientLight(flat, flat, Ogre::Vector3::UNIT_Y, envmapScaleForPass(), 0u);
    } JAH_CATCH(mError, );
}

// WHY PCC IS EXEMPT. Under automatic parallax-corrected cubemaps the env-probe
// slot holds a cube ARRAY of probe captures, not the sky — reflectionTexFor
// returns nullptr for the sky cube there, deliberately (OgreSky.cpp's note on
// the slot having one occupant). A probe capture is a photograph of the scene's
// real radiance, most of it GEOMETRY lit by the scene's own lamps, and
// upstream's shader scales those samples by the same `ambientUpperHemi.w`
// (ForwardPlus_DecalsCubemaps_piece_ps.any:372). Applying the sky's gain there
// would put a lamp-lit room out because its skylight was turned down, which is
// a worse error than the one this gate fixes. The residual is recorded: a probe
// PHOTOGRAPHS the sky as drawn, so at a tier that places probes a sky with no
// Sky Light can still reach a rough metal through one. Closing that means
// drawing the sky into probe captures at the Sky Light's gain, which is a probe
// capture change with its own cache-invalidation contract.
// ...AND AN AUTHORED REFLECTION MAP IS EXEMPT FOR THE SAME REASON (round-2
// review item 2, the lead's decision). `ambientUpperHemi.w` is a PASS value and
// HlmsPbs has no per-datablock env scale, so it scales EVERY PBSM_REFLECTION
// texture in the pass — including a cubemap the author assigned to one
// material. That map is not the sky: it is the material's own environment, and
// the sky's gain has no business touching it. A scene with no Sky Light was
// zeroing it.
//
// THE PASS IS THE UNIT AND MATERIALS MIX IN IT, so this cannot be decided per
// material through the scale. It is decided in two places instead:
//
//   * the SCALE stays at 1.0 as soon as ANY live material carries an authored
//     map, because 1.0 is the only value that is right for that material and
//     the alternative is silently wrong for it; and
//   * the SKY CUBE is UNBOUND when the gain is zero (reflectionTexFor), so "no
//     Sky Light, no sky reflection" stays exact in that mixed pass too — it is
//     done by not binding rather than by scaling, and binding IS per material.
//
// THE DOCUMENTED LIMITATION, because there is one: in a scene that mixes an
// authored reflection map with sky-lit materials, the sky's reflection no
// longer follows Sky Light INTENSITY between 0 and 1 — it is full or it is off.
// Zero is exact, one is exact, and the in-between is not. Closing that needs a
// per-datablock env scale, which this pin does not have; the alternatives are a
// second pass for the authored-map materials (a real cost for a rare scene) or
// pre-scaling the sky cube at convolution time (a reconvolution per slider
// frame). Neither is worth it until a scene asks.
bool OgreScene::hasAuthoredReflectionMap() const {
    for (const auto &kv : mMaterials) {
        if (kv.second.unlit) continue;
        if (kv.second.boundTextures[size_t(PbrTextureSlot::Reflection)]) return true;
    }
    return false;
}

float OgreScene::envmapScaleForPass() const {
    // This scene's grid, for the same reason reflectionTexFor asks that way: the
    // scale multiplies whatever the env slot holds, and while this scene's
    // passes bind a PCC that is a probe array.
    if (probeGridBound()) return 1.0f;
    if (hasAuthoredReflectionMap()) return 1.0f;
    return mEnvLightScale;
}

bool OgreScene::removeNode(NodeId id) {
    auto it = mNodes.find(id);
    if (it == mNodes.end()) return false;
    JAH_TRY {
        const bool hadDecal = it->second.decal != nullptr;
        // The reverse index, by the id recorded at track() time — an adopted
        // node may already be gone, so it cannot be asked for its id now. Only
        // if it still points HERE: a re-adoption of the same Ogre node owns it.
        auto idx = mNodeByOgreId.find(it->second.ogreId);
        if (idx != mNodeByOgreId.end() && idx->second == id) mNodeByOgreId.erase(idx);
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
        Node *prec = parent ? record(parent) : nullptr;          // one lookup (F9)
        Ogre::SceneNode *p = prec ? prec->node : nullptr;
        if (parent && !p) { mError = "createNode: unknown parent"; return 0; }
        if (!p) p = mSceneMgr->getRootSceneNode(Ogre::SCENE_DYNAMIC);
        // AN ENGINE-OWNED CHILD OF A REGISTERED NODE: the one case
        // applyShownSubtree still has to descend into (a gizmo slot, a
        // selection wire, a bone-overlay bone), recorded so that walk does not
        // have to ask the registry about every child to find out.
        if (prec) ++prec->ownedChildren;
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
        Node *rec = record(id);                                  // one lookup each (F9)
        Ogre::SceneNode *n = rec ? rec->node : nullptr;
        if (!n) { mError = "setNodeParent: unknown node"; return false; }
        // An adopted node's place in the tree is the DOCUMENT's (one tree).
        if (!rec->owned) { mError = "setNodeParent: node is adopted"; return false; }
        Node *prec = parent ? record(parent) : nullptr;
        Ogre::SceneNode *p = parent ? (prec ? prec->node : nullptr)
                                    : mSceneMgr->getRootSceneNode(Ogre::SCENE_DYNAMIC);
        if (!p) { mError = "setNodeParent: unknown parent"; return false; }
        if (n->getParent() == p) return true;
        if (n->getParent()) n->getParent()->removeChild(n);
        p->addChild(n);
        // ...and the same record as createNode's (it is a COUNT that only ever
        // grows: a node that once had an engine-owned child keeps walking its
        // children, which costs a lookup per child on a node that has had one
        // and is never wrong).
        if (prec) ++prec->ownedChildren;
        // A move under a hidden parent hides the subtree, a move out from
        // under one shows it again (as far as each node's own flag allows).
        bool giChanged = false;
        applyShownSubtree(n, inheritedShown(n), giChanged);
        // A REPARENT IS A VISIBILITY EDGE HERE TOO (DRAG-1): the node moved
        // under or out from under a hidden parent. Nothing died.
        if (giChanged) invalidateGiCachesForVisibility(nullptr);
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
            // OUR HALF OF THE MOVEMENT EPOCH (ensureGiWalk): the host's counter
            // sees the document's writes into the shared graph, not ours.
            //
            // ...BUT ONLY FOR SOMETHING A SCAN CAN READ (lane ENGINE-7 item 1).
            // Most writes through this entry point are EDITOR FURNITURE — the
            // gizmo, the bone overlay, wires, the grid — and the gizmo is
            // screen-scaled, so it writes four transforms on every frame the
            // camera moves. Counting those re-ran the GI movement scan, the
            // caster walk and both GI signatures on every frame of an orbit:
            // measured on the 8,404-node lattice, the mirror's GI push cost
            // 9.7 ms a frame while flying and 0.02 ms still, and the four
            // gizmo pushes were the whole of it once the document's camera
            // stopped counting (nodegraph.h's epoch).
            if (it == mNodes.end() || writeIsSceneMovement(it->second))
                noteSceneTransformWrite();
            // A MOVABLE LAMP MOVES NOTHING A SCAN CAN SEE (DRAG-1 round 2, F5).
            // It is not GI geometry, so `mGiMovedBoxes` never mentions it and
            // the probe grid is never staled for it — but a light injection
            // reads its pose, so the in-motion tick must not skip a cascade
            // that was injected before it moved.
            if (it != mNodes.end() && it->second.light) ++mGiLightWriteSerial;
        }
    } JAH_CATCH(mError, );
}

// CAN A TRANSFORM WRITE ON THIS NODE CHANGE WHAT ANY SCAN READS?
// (lane ENGINE-7 item 1 — the movement epoch's precision.)
//
// The epoch exists so a still frame skips four O(scene) walks: the GI movement
// scan, the shadow-caster walk and the two GI signatures the host reads every
// frame. Each of them reads ITEMS through a filter (walkItems), and this is
// the same filter asked the other way round — "is there anything here that a
// filter lets through?" A NO means the write cannot have changed a single
// answer, so counting it would buy nothing but the walks.
//
// The three things a scan can read, and nothing else:
//   * VOXELISED geometry (kGiGeometryBit) — the GI scan and both signatures;
//   * PROBE-ONLY geometry (P7: unlit, visible, below the probe-face queue) —
//     the probe grid's half of the GI scan;
//   * a SHADOW CASTER (casts, in a caster CHANNEL, and below the overlay
//     queue, which writes no depth) — the caster walk.
//
// THE CASTER CHANNEL IS PART OF THE TEST, AND LEAVING IT OUT WAS A 90 Hz BILL
// (lane VR-SCAN-1, 2026-09-18; the Fable read of VR-INPUT-1E-FIX found it).
// `Item::getCastShadows()` is `mVisibilityFlags & LAYER_SHADOW_CASTER`, and
// Ogre sets that bit on EVERY MovableObject at birth — so "it casts shadows"
// is true of everything nobody has switched off, editor furniture included,
// and this test's last line used to pass for every helper in the scene. The
// caster WALK does not work that way (walkItems: `flags & channelsAll` first),
// so a write on a node in no caster channel could not change a single answer
// it gives — the filter asked the other way round has to ask the same
// question. The comment that used to sit here said the channel was "left out
// deliberately: a caster no lamp can see still counts" — which conflated a
// lamp's light mask (not read here, and rightly so) with the RENDER channel
// that decides whether a shadow pass draws the item at all.
//
// WHAT IT COST: since VR-4-FIX moved the controller proxies and the pointing
// ray into the session's own frame, those helpers' poses are written EVERY
// frame a hand is located — so every VR frame with a hand or a ray in it bumped
// the movement epoch and re-ran the full item scan (392 / 1,961 / 4,328 us at
// 1k / 5k / 10k nodes, lane R2's numbers), at 90 Hz, for furniture no GI
// consumer and no shadow map can see.
// ...plus three structural yeses that are not items at all: a node with
// CHILDREN moves them, a DECAL is a probe input (mDecalNodes), and a LIGHT is
// an input to everything (its position is not read here, but nothing is gained
// by being clever about the handful of lights in a scene).
//
// Everything else is EDITOR FURNITURE: the gizmo (four screen-scaled parts
// re-pushed on every frame the camera moves), the bone overlay, the selection
// shell, the light wires, the grid, the horizon and the GI volume boxes.
bool OgreScene::writeIsSceneMovement(const Node &n) const {
    if (n.light || n.decal) return true;
    if (n.node && n.node->numChildren()) return true;
    const Ogre::Item *item = n.item;
    if (!item) return false;              // an empty node with nothing under it
    const Ogre::uint32 flags = item->getVisibilityFlags();
    if (flags & kGiGeometryBit) return true;
    const Ogre::uint8 rq = item->getRenderQueueGroup();
    if (probeSeesItem(n)) return true;
    return (flags & allShadowCasterChannels()) && item->getCastShadows() && n.shown &&
           rq < kOverlayRenderQueue;
}

// THE bit-scheme application point (REFLECTIONS_ADOPTION_SPEC.md P1b).
// Helpers carry kHelperBit INSTEAD OF kVisibleBit — an include channel, because
// Ogre's any-bit test cannot express an exclude bit (EnginePrivate.h's block).
// Only lit (PBR) surfaces get kGiGeometryBit: unlit overlays, wires and line
// meshes must neither bounce nor occlude GI rays.
Ogre::uint32 OgreScene::itemVisibilityFlags(Node &n, bool unlit, bool distortion) {
    n.materialUnlit = unlit;          // remembered for applyNodeVisibilityFlags
    n.materialDistortion = distortion;
    // DISTORTION WINS OVER EVERYTHING, including the helper flag: the item
    // writes a screen-space displacement field, and the only pass in this
    // engine that may draw it is the distortion pass (kDistortionBit's note).
    if (distortion) return kDistortionBit;
    // A BACKDROP IS A HELPER THE PICTURE KEEPS (kBackdropBit's note): the same
    // exclusion from every capture, a channel a view can NOT mask out as
    // furniture.
    // ...AND A HELPER MAY BE IN BOTH HELPER CHANNELS (kVrHelperBit's two-bit
    // rule): the controller proxies (and phase 4b's ray and hit marker) are the
    // WEARER's furniture, drawn in every eye and at the desk, so their bit is
    // added BESIDE the desktop one rather than replacing it. A backdrop is
    // never in the VR channel — it is part of the picture and every view
    // already draws it.
    if (n.helper)
        return n.backdrop ? kBackdropBit
                          : (kHelperBit | (n.vrHelper ? kVrHelperBit : 0u));
    // MOVING THINGS ARE THEIR OWN CHANNEL (REALTIME_REFLECTIONS_SPEC §3.3.4,
    // kMovableBit's note): a movable item carries kMovableBit INSTEAD OF
    // kVisibleBit, which takes it out of every capture pass that asks for
    // kVisibleBit — the reflection-probe faces, the raster-DDGI faces and the
    // probe-kind shadow node — while the view, the planar mirror, SSR and the
    // view/reflect shadow nodes (which ask for both channels) go on drawing it
    // every frame.
    // ...AND A DRAGGED STILL IS A MOVER FOR THE LENGTH OF ITS GESTURE
    // (MOVER-1, Node::dragMover). One OR, here and in the billboard/particle
    // branch below, is the whole of the channel half: everything the mobility
    // design already does for a Movable object — out of the probe captures, out
    // of the voxel bounce, lit by the field and the cones at its live pose —
    // is what a dragged object wants while it is being dragged, and the
    // document's own answer is not touched.
    const bool moving = n.movable || n.dragMover;
    const Ogre::uint32 channel = moving ? kMovableBit : kVisibleBit;
    if (unlit) return channel;
    // A HIDDEN NODE MUST NOT BOUNCE LIGHT (SMOKE_FIX S12). Ogre's own hide —
    // SceneNode::setVisible — toggles the LAYER_VISIBILITY bit, which
    // MovableObject::getVisibilityFlags() masks off before returning
    // (OgreMovableObject.inl), so every GI gather's `flags & kGiGeometryBit`
    // test was blind to it: a hidden cube stayed voxelised, stayed in the
    // Instant-Radiosity trace, and went on defining the automatic lit volume
    // (measured: hiding a cube at (0,20,40) left the volume at 4.0 x 22.0 x
    // 63.2 and its bounce on the floor). Dropping the bit while hidden takes
    // the item out of all six gathers AND out of giItemBounds with one write,
    // because they all key on exactly this bit; setNodeVisible invalidates the
    // caches so the next solve is the one without it.
    //
    // EFFECTIVE, not own (RENDER_PIPELINE_AUDIT 1.1): `shown` is false for an
    // item under a hidden ANCESTOR too. Keyed on the node's own flag, hiding
    // an imported model by its root — the common case — left every part of
    // it voxelised and bouncing light.
    //
    // ...AND A MOVING THING IS NOT GI GEOMETRY. It is LIT by the room (it reads
    // the probes, the voxel cone and the irradiance field like everything else)
    // but it does not voxelize and does not bounce: keeping it would put a
    // moving object into the lit volume, the escape and geometry signatures,
    // the movement scan and the voxel arm — which is the entire cost this lane
    // removes. The price is stated rather than hidden: movers cast no indirect
    // light and do not darken the room's bounce (spec §5.3, the same limit
    // Unreal has without Lumen). Contact darkening is SSAO's job.
    return (n.shown && !moving) ? (channel | kGiGeometryBit) : channel;
}

void OgreScene::applyNodeVisibilityFlags(Node &n) {
    // A STRUCTURAL INPUT to the GI scans and the two signatures: which channel
    // an object is in decides whether they read it at all (clean-2 lane).
    noteSceneTransformWrite();
    // The material's unlit-ness was recorded when the geometry attached, so a
    // lit mesh that is marked helper and then unmarked gets its kGiGeometryBit
    // back. Reading it off the item's CURRENT flags could not do that: a helper
    // carries kHelperBit alone.
    if (n.item) n.item->setVisibilityFlags(
                    itemVisibilityFlags(n, n.materialUnlit, n.materialDistortion));
    if (n.item) markGpuSlotDirty(n);   // the table's flags word follows the channels
    // (A CASCADE CHAIN NO LONGER HOLDS ITS OWN COPY OF THAT DECISION, audit D2:
    // the gather reads the flags word the line above re-stages, at every build,
    // so a hide or a helper flag leaves every cascade's next rebuild by itself.)
    // THE TWO-BIT RULE REACHES BILLBOARDS TOO (VR-4-FIX finding 9): a helper
    // billboard set — an icon, and a phase-4b hit marker if it is built as one
    // — carries kVrHelperBit beside kHelperBit exactly as an Item does, or the
    // wearer's own furniture would be desk-only whenever it is not a mesh.
    const Ogre::uint32 on = n.helper
                                ? (n.backdrop ? kBackdropBit
                                              : (kHelperBit | (n.vrHelper ? kVrHelperBit : 0u)))
                                : ((n.movable || n.dragMover) ? kMovableBit : kVisibleBit);
    if (n.billboards) n.billboards->setVisibilityFlags(n.shown ? on : 0u);
    if (n.particleDef) n.particleDef->setVisibilityFlags(n.shown ? particleVisibilityBits(n) : 0u);
}

OgreScene::Node *OgreScene::registryNode(const Ogre::Node *sn) {
    if (!sn) return nullptr;
    auto idx = mNodeByOgreId.find(sn->getId());
    if (idx == mNodeByOgreId.end()) return nullptr;
    auto it = mNodes.find(idx->second);
    return it == mNodes.end() ? nullptr : &it->second;
}

bool OgreScene::inheritedShown(const Ogre::Node *sn) {
    // The nearest REGISTERED ancestor's effective state already folds in every
    // ancestor above it, so the walk stops at the first one. Unregistered
    // nodes pass through: the document root, a node the host has not adopted
    // yet, and a socket rider's TagPoint — whose chain ends there (a tag's
    // parent is a bone, not a node), which is why a host with riders pushes
    // their effective state itself.
    for (const Ogre::Node *p = sn ? sn->getParent() : nullptr; p; p = p->getParent())
        if (const Node *rec = registryNode(p)) return rec->shown;
    return true;
}

void OgreScene::applyShownSubtree(Ogre::SceneNode *sn, bool inherited, bool &giChanged) {
    applyShownSubtree(sn, registryNode(sn), inherited, giChanged);
}

/// ...and the same walk when the caller ALREADY HAS the record (the push site
/// looked it up to write `visible` into it a line earlier). `rec` may be null:
/// that is an unregistered helper child — a light's -Y adapter, a decal's
/// projector box — and the walk exists for exactly those.
void OgreScene::applyShownSubtree(Ogre::SceneNode *sn, Node *rec, bool inherited, bool &giChanged) {
    const bool shown = rec ? (inherited && rec->visible) : inherited;
    // OGRE'S HALF: LAYER_VISIBILITY on everything attached HERE — the Item, a
    // PFX2 instance — and, one level down through the unregistered helper
    // children, a light on its -Y adapter and a decal on its projector box.
    // This is SceneNode::setVisible(v, true) with one difference, the whole
    // point: a registered descendant takes ITS OWN flag into account, so
    // showing a parent no longer shows what the user hid underneath it.
    const size_t numObjects = sn->numAttachedObjects();
    for (size_t i = 0; i < numObjects; ++i) {
        Ogre::MovableObject *obj = sn->getAttachedObject(i);
        // A LIGHT switching on or off changes what every reflection probe
        // would capture (ENGINE_CACHE_POLICY_SPEC P7). It is not geometry, so
        // nothing below would notice; the light rides its -Y adapter, one
        // unregistered level down, and is reached by this same loop there.
        //
        // ...AND SO DOES A DECAL (clean-2 lane review, F2). A decal paints the
        // surface the probes capture, and hiding or re-showing one changes
        // nothing the movement scan can see — its projector box does not move,
        // so the scan reports nothing and the probes keep the wall with (or
        // without) the decal on it for ever. Same shape as the light: the
        // decal rides the projector-box child, one unregistered level down,
        // and is reached by this same loop there.
        if (obj->getVisible() != shown) {
            if (dynamic_cast<Ogre::Light *>(obj))      { ++mGiLightWriteSerial;
                                                          staleProbeGrid(GiStaleReason::Light); }
            else if (dynamic_cast<Ogre::Decal *>(obj)) staleProbeGrid(GiStaleReason::Moved);
        }
        obj->setVisible(shown);
    }
    if (rec) {
        // OURS: the Item's kGiGeometryBit and the billboard / PFX2 flags,
        // which no Ogre cascade reaches (applyNodeVisibilityFlags).
        const bool giBefore = rec->item && (rec->item->getVisibilityFlags() & kGiGeometryBit) != 0u;
        const bool probeBefore = probeSeesItem(*rec);
        rec->shown = shown;
        applyNodeVisibilityFlags(*rec);
        const bool giAfter = rec->item && (rec->item->getVisibilityFlags() & kGiGeometryBit) != 0u;
        if (giBefore != giAfter) giChanged = true;
        // ...and what the PROBES see, which is more than what GI sees: an
        // UNLIT item is captured (kVisibleBit) but never voxelized, so the GI
        // edge above misses it. A probe-only stale, no GI invalidation.
        if (probeSeesItem(*rec) != probeBefore) staleProbeGrid(GiStaleReason::Moved);
    }
    // THE WALK NO LONGER DESCENDS INTO THE DOCUMENT (L12, ledger 153). What
    // hangs under a registered node is, almost always, the DOCUMENT's own
    // subtree — the engine ADOPTED the document's tree (SCENEGRAPH_SPEC D2) —
    // and its host pushes every one of those nodes' EFFECTIVE visibility
    // itself, parent-first, on change (SceneMirror::visit:
    // `shown = parentShown && node->visible`, pushed through setNodeVisible,
    // which lands right back here for that node). Recursing into them
    // re-applied, per push, what N more pushes were about to apply anyway:
    // N parent-first pushes walked Sum(subtree sizes), not N nodes. MEASURED
    // on `bench_scenegraph --scales 10000 --quick` (10,502 document nodes):
    // 10,703 setNodeVisible calls drove 67,176 visits — 6.3 per node — and
    // cost +21.6 ms of e.first_sync (158.5 -> 180.1 ms, interleaved A/B).
    //
    // SO THE DESCENT IS A WHITELIST, NOT A BLACKLIST. "Skip the children that
    // are registered as adopted" is NOT enough and was measured not to be: on
    // a FIRST sync the host adopts parent-first, so at the moment it pushes
    // node K's visibility, K's children are document nodes it has not adopted
    // yet — unregistered, indistinguishable from a helper by the registry, and
    // the walk went straight on down the rest of the document (measured: the
    // visit count did not move at all). What this function is for is ONE
    // closed set of children, and they are all ours:
    //
    //   * an ENGINE-OWNED registered child (createNode — a gizmo slot, a
    //     selection wire, a bone-overlay bone). Nothing pushes visibility for
    //     those from a document walk; setNodeVisible / setNodeParent on their
    //     engine-owned parent is the only thing that reaches them, which is the
    //     contract tests/shadow and tests/engine drive directly.
    //   * the TWO unregistered helper children this engine creates under a
    //     registered node and no push can reach: a light's -Y adapter
    //     (setLight) and a decal's projector box (OgreDecals). They are the
    //     reason the walk still exists at all — a hidden light must stop
    //     lighting and stale the probe grid, a hidden decal must stop painting
    //     the wall the probes capture.
    //
    // Everything else below a registered node belongs to the document, and the
    // document's host owns its visibility.
    // AND THE LOOKUP PER CHILD GOES WITH IT (ledger 179). The whitelist above is
    // a closed set: engine-owned registered children (createNode / setNodeParent
    // put them there, and `ownedChildren` records that they did — sticky, never
    // cleared, so it can only over-approximate) and the two unregistered helper
    // children this engine makes, both of which the record names by pointer. A
    // node with neither has nothing below it this walk may touch, and asking the
    // registry about each of its children — 10,503 lookups on a first sync of
    // the 10 k bench, every one of them answering "a document node, skip it" —
    // is the dead work this removes. The mirror pushes those children itself,
    // parent-first, which is why the descent stopped at them in the first place.
    if (rec && !rec->ownedChildren) {
        if (rec->lightNode) applyShownSubtree(rec->lightNode, nullptr, shown, giChanged);
        if (rec->decalNode) applyShownSubtree(rec->decalNode, nullptr, shown, giChanged);
        return;
    }
    const size_t numChildren = sn->numChildren();
    for (size_t i = 0; i < numChildren; ++i) {
        Ogre::SceneNode *child = static_cast<Ogre::SceneNode *>(sn->getChild(i));
        if (const Node *crec = registryNode(child)) {
            if (crec->owned) applyShownSubtree(child, shown, giChanged);
            continue;                     // adopted: its host pushes it, parent-first
        }
        if (rec && (child == rec->lightNode || child == rec->decalNode))
            applyShownSubtree(child, shown, giChanged);
    }
}

Ogre::uint32 OgreScene::particleVisibilityBits(const Node &n) {
    // A distortion emitter is invisible to every pass but the distortion pass,
    // helper or not (there is no helper distortion; the icon queue draws colour).
    if (n.particleDistortion) return kDistortionBit;
    // A particle system is time-varying content by definition, so the document
    // resolves every emitter movable (spec §3.6, owner decision O4): the probes
    // freeze whatever they last captured of it and SSR and the mirrors show it
    // live. The channel is what implements that here.
    // Helper emitters follow the two-bit rule like every other helper
    // (VR-4-FIX finding 9): the wearer's channel is ADDITIVE here too.
    return n.helper ? (n.backdrop ? kBackdropBit
                                  : (kHelperBit | (n.vrHelper ? kVrHelperBit : 0u)))
                    : (n.movable ? kMovableBit : kVisibleBit);
}

void OgreScene::setNodeHelper(NodeId id, bool helper) {
    auto it = mNodes.find(id);
    if (it == mNodes.end()) return;
    if (it->second.helper == helper) return;
    const bool probeBefore = probeSeesItem(it->second);
    it->second.helper = helper;
    if (!helper) it->second.backdrop = false;   // one flag pair, one meaning
    applyNodeVisibilityFlags(it->second);
    // A helper is exactly "the probes must not capture this" (kHelperBit
    // instead of kVisibleBit), so the flag flipping is a probe input (P7).
    if (probeSeesItem(it->second) != probeBefore) staleProbeGrid(GiStaleReason::Moved);
}

bool OgreScene::nodeHelper(NodeId id) const {
    auto it = mNodes.find(id);
    return it != mNodes.end() && it->second.helper;
}

// THE SECOND HELPER CHANNEL (kVrHelperBit's two-bit rule). ADDITIVE, so it
// changes nothing about what a capture sees — the node keeps carrying
// kHelperBit instead of kVisibleBit, which is what every probe, shadow node and
// GI gather keys on — and therefore needs no probe-visibility edge and no
// staleness of any kind. All it does is put the item in a second INCLUDE
// channel that exactly one view in the process asks for.
void OgreScene::setNodeVrHelper(NodeId id, bool vrHelper) {
    auto it = mNodes.find(id);
    if (it == mNodes.end()) return;
    if (it->second.vrHelper == vrHelper) return;
    it->second.vrHelper = vrHelper;
    applyNodeVisibilityFlags(it->second);
}

bool OgreScene::nodeVrHelper(NodeId id) const {
    auto it = mNodes.find(id);
    return it != mNodes.end() && it->second.vrHelper;
}

// THE WEARER'S HANDS ARE PLACED INSIDE THE FRAME (Scene::setVrProxyNodes,
// VR-4-FIX finding 4). Two ids and nothing else: the nodes, their line meshes,
// their materials and their visibility stay the host's — a scene that never
// wears a headset carries two zeroes.
void OgreScene::setVrProxyNodes(NodeId left, NodeId right) {
    mVrProxyNode[0] = left;
    mVrProxyNode[1] = right;
}

void OgreScene::vrProxyNodes(NodeId out[2]) const {
    out[0] = mVrProxyNode[0];
    out[1] = mVrProxyNode[1];
}

// ...AND THE RAY THE WEARER POINTS WITH (Scene::setVrRayNodes,
// VR_INPUT_SPEC §3). Two more ids and nothing else: the line and the hit
// marker are the mirror's geometry, and the session scales and stands them up
// inside the frame from the state the host pushed.
void OgreScene::setVrRayNodes(NodeId line, NodeId marker) {
    mVrRayNode[0] = line;
    mVrRayNode[1] = marker;
}

void OgreScene::vrRayNodes(NodeId out[2]) const {
    out[0] = mVrRayNode[0];
    out[1] = mVrRayNode[1];
}

// ...AND THE WEARER'S OWN HANDS, BONE BY BONE (Scene::setVrHandBoneNodes,
// VR_INPUT_SPEC §7, stage 3). Twenty-four ids per hand and nothing else: the
// segments are ONE unit line mesh the mirror made, shared by every bone of both
// hands, and the running session stands each one between two joints inside the
// frame that draws it (vrBoneTransform). A scene that never sees a tracked hand
// carries two zero counts.
void OgreScene::setVrHandBoneNodes(unsigned hand, const NodeId *nodes, unsigned count) {
    if (hand >= 2u) return;
    const unsigned n = (!nodes || count == 0u)
                           ? 0u
                           : (count < kVrHandBoneCount ? count : unsigned(kVrHandBoneCount));
    for (unsigned b = 0; b < kVrHandBoneCount; ++b)
        mVrHandBoneNode[hand][b] = b < n ? nodes[b] : 0;
    mVrHandBones[hand] = n;
}

unsigned OgreScene::vrHandBoneNodes(unsigned hand, NodeId *out, unsigned count) const {
    if (hand >= 2u) return 0u;
    const unsigned n = mVrHandBones[hand];
    if (out)
        for (unsigned b = 0; b < count && b < n; ++b) out[b] = mVrHandBoneNode[hand][b];
    return n;
}

// WHERE A NODE ACTUALLY IS. `_getDerived*Updated` walks up to whatever parent
// chain the node hangs from and brings the derived transform up to date first,
// which is the whole reason this is not `node->getPosition()`: the caller is
// asking about the picture, and the scene manager's own walk happens later in
// the frame.
bool OgreScene::nodeWorldPose(NodeId id, Vec3 &position, Quat &rotation) const {
    Ogre::SceneNode *n = node(id);
    if (!n) return false;
    const Ogre::Vector3 p = n->_getDerivedPositionUpdated();
    const Ogre::Quaternion q = n->_getDerivedOrientationUpdated();
    position = Vec3(p.x, p.y, p.z);
    rotation = Quat(q.x, q.y, q.z, q.w);
    return true;
}

// A BACKDROP IS A HELPER (kBackdropBit's note), so this goes through the same
// door: the probe-visibility edge, the item re-flag and the cascade staleness
// are all setNodeHelper's, and the only thing that differs is WHICH channel the
// item lands in. Clearing the helper flag clears this one — one flag pair, one
// meaning, so `setNodeHelper(id,false)` cannot leave an object claiming to be a
// backdrop that no capture excludes.
void OgreScene::setNodeBackdrop(NodeId id, bool backdrop) {
    auto it = mNodes.find(id);
    if (it == mNodes.end()) return;
    if (it->second.backdrop == backdrop && (!backdrop || it->second.helper)) return;
    it->second.backdrop = backdrop;
    if (backdrop && !it->second.helper) { setNodeHelper(id, true); return; }
    applyNodeVisibilityFlags(it->second);
}

bool OgreScene::nodeBackdrop(NodeId id) const {
    auto it = mNodes.find(id);
    return it != mNodes.end() && it->second.backdrop && it->second.helper;
}

// ---- MOBILITY (REALTIME_REFLECTIONS_SPEC §3.3, lane R2: spent) ------------
//
// THE ONE PLACE the document's answer to "does this move?" becomes render
// state. Three things follow from one bool, and the order they are applied in
// is what makes the cheap cases cheap:
//
//  1. THE CHANNEL. kMovableBit instead of kVisibleBit (and the GI bit dropped)
//     — one setVisibilityFlags per attached object, no cache touched. This is
//     the whole of the play-time soft promotion.
//  2. THE PROBES. Whether a reflection probe would CAPTURE this item changes
//     across the flip (that is the point), so the probes that hold its picture
//     owe a re-capture: a stale with its own reason, spread at the budget like
//     every other input, ONCE per flip rather than once per frame of movement.
//  3. THE VOXELS, and only for an AUTHORING change. An object entering or
//     leaving the GI geometry set is a GI edge — the voxel arm, the lit volume
//     and Instant Radiosity's trace were all built without it (or with it) —
//     so the caches go and the next solve is the honest one. That is one
//     from-scratch rebuild, counted in MobilityStatus::mobilityRebuilds so the
//     cost is visible rather than mysterious.
//
//     MobilityChange::Soft refuses step 3 deliberately (owner decision O3): a
//     script pushing a prop mid-play must not buy the author a half-second
//     freeze, so the voxels keep the bounce light the object had where it
//     started — a ghost — until play stops and the object is classified for
//     real. Nothing else about it differs.
//
// A classification that arrives BEFORE the geometry (every load, every newly
// created node: the host resolves mobility in the same walk that creates the
// node) reaches none of the three — there is no item to re-flag and no probe
// that ever saw it — which is why a scene full of movers costs nothing to open.
void OgreScene::setNodeMovable(NodeId id, bool movable, MobilityChange change) {
    auto it = mNodes.find(id);
    if (it == mNodes.end()) return;
    Node &n = it->second;
    if (n.movable == movable) return;
    const bool probeBefore = probeSeesItem(n);
    const bool giBefore = n.item && (n.item->getVisibilityFlags() & kGiGeometryBit) != 0u;
    // THE GHOST HAS TO HEAL (code review 2026-09-12, item 5). A soft promotion
    // leaves the object's bounce light in the voxels where it stood — that is
    // the deal O3 makes. But if the voxels are rebuilt from scratch while it is
    // promoted, they are rebuilt WITHOUT it, and the ghost turns into a hole
    // that a free clearing push would leave for ever. So the clearing push
    // invalidates exactly when that has happened, and the counter says so.
    const bool healing = !movable && n.mobilitySoft && n.mobilitySoftRebuilds != mGiRebuilds;
    n.mobilitySoft = movable && change == MobilityChange::Soft;
    n.mobilitySoftRebuilds = mGiRebuilds;
    n.movable = movable;
    applyNodeVisibilityFlags(n);
    const bool giAfter = n.item && (n.item->getVisibilityFlags() & kGiGeometryBit) != 0u;
    if (healing && giBefore != giAfter) {
        invalidateGiCaches();
        ++mMobilityRebuilds;
    } else if (giBefore != giAfter && change == MobilityChange::Authoring) {
        // COUNT WHAT IT ACTUALLY COSTS. A scene with no GI arm built yet — the
        // load path, and every scene that never turns GI on — invalidates
        // nothing, and reporting a rebuild there would make the counter useless
        // for the question it exists to answer ("is my classification costing
        // me rebuilds?"). `mGiCachesDirty` already true means one is owed for
        // another reason and this change rides it.
        const bool wouldRebuild =
            !mGiCachesDirty && (mPcc || mVctVoxelizer || mIfd);
        invalidateGiCaches();
        if (wouldRebuild) ++mMobilityRebuilds;
    }
    if (probeSeesItem(n) != probeBefore) staleProbeGrid(GiStaleReason::Mobility);
}

bool OgreScene::nodeMovable(NodeId id) const {
    auto it = mNodes.find(id);
    return it != mNodes.end() && it->second.movable;
}

MobilityStatus OgreScene::mobilityStatus() const {
    MobilityStatus out;
    out.mobilityRebuilds = mMobilityRebuilds;
    for (const auto &kv : mNodes) {
        const Node &n = kv.second;
        if (!n.movable) continue;
        ++out.movableNodes;
        if (n.item || n.particleSystem || n.billboards) ++out.movableItems;
        if (n.light) ++out.movableLights;
    }
    return out;
}

void OgreScene::setNodeLightMask(NodeId id, unsigned mask) {
    auto it = mNodes.find(id);
    if (it == mNodes.end()) return;
    // Remembered even when nothing is attached yet: attachMesh applies it to
    // the Item it creates, which is also what makes the mask survive the Item
    // rebuild a material swap performs.
    it->second.lightMask = Ogre::uint32(mask);
    if (it->second.item) {
        it->second.item->setLightMask(Ogre::uint32(mask));
        markGpuSlotDirty(it->second);   // the table carries the light mask (ids.z)
    }
    // Deliberately NOT pushed to billboards or particle systems: a PFX2
    // definition is pooled and shared between nodes (mParticleDefPool), so a
    // per-node mask on one would silently mask every node recycling it.
}

unsigned OgreScene::nodeLightMask(NodeId id) const {
    auto it = mNodes.find(id);
    return it == mNodes.end() ? 0xFFFFFFFFu : unsigned(it->second.lightMask);
}

// PER-OBJECT SHADOW CASTING (Engine.h's contract). The document has carried
// SceneNode::castShadow for years — serialized, reflected, and set to false by
// the default floor — and nothing ever pushed it anywhere: this is the other
// end of that wire.
//
// Ogre keeps it as the LAYER_SHADOW_CASTER bit of the visibility flags, which
// MovableObject::setVisibilityFlags does NOT overwrite (it merges the reserved
// layer bits), so this coexists with itemVisibilityFlags' channel scheme.
void OgreScene::setNodeCastShadow(NodeId id, bool on) {
    auto it = mNodes.find(id);
    if (it == mNodes.end()) return;
    const bool changed = it->second.castShadow != on;
    it->second.castShadow = on;
    if (it->second.item) {
        it->second.item->setCastShadows(on);
        markGpuSlotDirty(it->second);   // the caster bit of the flags word
    }
    // A caster that just appeared or vanished is exactly what the lamp-map
    // cache exists to notice.
    if (changed && it->second.item) markShadowShapeDirty(it->second);
}

bool OgreScene::nodeCastShadow(NodeId id) const {
    auto it = mNodes.find(id);
    return it == mNodes.end() ? true : it->second.castShadow;
}

void OgreScene::setNodeVisible(NodeId id, bool visible) {
    setNodeVisibleImpl(id, visible, nullptr);
}

// THE SAME PUSH FROM A PARENT-FIRST HOST (L12 follow-up, ledger 179). The
// mirror computes `shown = parentShown && node->visible` for every node it
// walks and then pushed it through setNodeVisible, which derived the very same
// parent state again by walking up to the nearest registered ancestor: on a
// bulk first sync that is one registryNode (two hash lookups) per adopted node
// for an answer the caller had in a local variable. Measured at 10,503 nodes:
// 21,006 inheritedShown calls, of which half were this one.
void OgreScene::setNodeVisibleUnder(NodeId id, bool visible, bool parentShown) {
    setNodeVisibleImpl(id, visible, &parentShown);
}

void OgreScene::setNodeVisibleImpl(NodeId id, bool visible, const bool *parentShown) {
    JAH_TRY {
        auto it = mNodes.find(id);
        if (it == mNodes.end()) return;
        Node &n = it->second;
        // A WRITE THAT CHANGES NOTHING DIRTIES NOTHING (lane VR-SCAN-1,
        // 2026-09-18). Visibility is PUSHED, not diffed, by more than one
        // writer — the mirror's per-frame sync, and inside a VR frame
        // `VrSession::placeProxies`, which hides an unlocated hand on EVERY
        // frame it is not located (its own note says the show side is
        // deliberately unconditional). Each of those pushes ran the whole
        // subtree walk below, re-derived the item's channel bits and asked the
        // probe-visibility question again, for a state that was already
        // exactly that. So: the node's own flag AND the state it resolves to
        // both unchanged means the subtree below is already the function of
        // those two inputs it would be recomputed into — nothing to do.
        //
        // WHY THIS IS EXACT rather than a hopeful memo: the effective state of
        // every registered descendant is a pure function of its own flag and
        // the chain above it (applyShownSubtree computes precisely that), and
        // the chain above THIS node is what `parentShown` / `inheritedShown`
        // reports. With this node's flag and its effective state both equal to
        // what they already are, no descendant's inputs have moved either.
        // (One parent-chain walk, at worst, against a whole subtree.)
        const bool parentEff = parentShown ? *parentShown : inheritedShown(n.node);
        if (n.visible == visible && n.shown == (visible && parentEff)) return;
        n.visible = visible;
        // THE WHOLE SUBTREE, EFFECTIVELY (RENDER_PIPELINE_AUDIT 1.1/1.2). This
        // was Ogre's setVisible(visible, cascade = true) plus the GI bit of
        // THIS node alone: hiding a model's root hid its parts on screen but
        // left every one of them voxelised, in the Instant-Radiosity trace and
        // defining the automatic lit volume (measured: bounds unchanged); and
        // showing it again set every descendant visible, re-revealing parts
        // the user had hidden themselves. applyShownSubtree recomputes each
        // registered descendant from its OWN flag and the chain above it, and
        // carries the GI bit, the billboard and the PFX2 halves with it.
        bool giChanged = false;
        if (n.node) {
            applyShownSubtree(n.node, &n, parentEff, giChanged);
        } else {
            const bool giBefore = n.item && (n.item->getVisibilityFlags() & kGiGeometryBit) != 0u;
            const bool probeBefore = probeSeesItem(n);
            n.shown = visible;
            applyNodeVisibilityFlags(n);
            giChanged = giBefore != (n.item && (n.item->getVisibilityFlags() & kGiGeometryBit) != 0u);
            if (probeSeesItem(n) != probeBefore) staleProbeGrid(GiStaleReason::Moved);
        }
        // THE GI HALF (SMOKE_FIX S12). Hiding or showing lit geometry changes
        // what the next solve sees, exactly like detaching it does (detachItem's
        // note) — so the caches go, ONCE for the whole subtree and on the EDGE
        // only: the mirror pushes visibility on change, and a push that moves
        // no GI bit (an empty node, an unlit helper, a subtree already hidden
        // by an ancestor) costs no re-solve.
        // ...AND THE BOX IT HAPPENED IN (G1), so that under a cascade chain only
        // the cascades that can SEE the item owe a re-voxelisation for its being
        // hidden or shown. The box is this node's own item's, which is right for
        // the overwhelmingly common case (one item hidden) and an UNDERSTATEMENT
        // for a subtree whose descendants reach further — so a subtree edge is
        // reported as "somewhere" and marks the chain, which is the safe
        // direction. (Round-2 F2: the first cut keyed this on `!n.node`, which is
        // never true — every node in `mNodes` carries one — so no hide or show
        // ever carried a box at all.)
        if (giChanged) {
            // ...THROUGH THE VISIBILITY DOOR (DRAG-1, RENDER_AUDIT I-2), which
            // is the same invalidation minus the destruction generation:
            // hiding a thing destroys nothing, and charging it as a death made
            // a hide the most expensive single event in the pipeline — more
            // than deleting the same object.
            const bool subtree = n.node && n.node->numChildren() > 0;
            if (!subtree && n.item) {
                const Ogre::Aabb box = n.item->getWorldAabb();
                invalidateGiCachesForVisibility(&box);
            } else {
                invalidateGiCachesForVisibility(nullptr);
            }
        }
    } JAH_CATCH(mError, );
}

// ---- Lights ----
bool OgreScene::setLight(NodeId id, const LightDesc &d) {
    auto it = mNodes.find(id);
    if (it == mNodes.end()) { mError = "setLight: unknown node"; return false; }
    JAH_TRY {
        // A LIGHT INJECTION WOULD READ THIS (DRAG-1 round 2, F5). The serial is
        // what tells the in-motion light tick that a cascade the scheduler
        // rebuilt no longer holds the scene's lights — see
        // VctCascade::injectedAtLightSerial. Bumped for the whole push rather
        // than for a measured change: this entry point is idempotent and the
        // mirror pushes it per frame, but only for lights the mirror believes
        // changed, and a spurious bump costs one extra cascade injection on the
        // next tick, while a missed one costs a stale bounce for a whole drag.
        ++mGiLightWriteSerial;
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
            // Born HIDDEN on a hidden node (or under a hidden ancestor): the
            // visibility walk only reaches objects attached at the time, and
            // a host pushes visibility before it pushes the light.
            if (!n.shown) n.light->setVisible(false);
            if (std::find(mLightNodes.begin(), mLightNodes.end(), id) == mLightNodes.end())
                mLightNodes.push_back(id);   // the light index (see EnginePrivate.h)
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
        //
        // AND ONLY THE SUN CASTS AMONG DIRECTIONALS. The node declares a single
        // directional slot (three PSSM splits at slot 0; every focused slot
        // accepts spot/point only — OgreShadow.cpp), and Ogre fills it with the
        // first CASTING directional in its own castShadows-then-light-id sort
        // (OgreSceneManager.cpp) — engine creation order, which can flip across
        // a reload or a mirror re-attach. Clearing the flag on every directional
        // the host did not name primary leaves that sort exactly one candidate,
        // so the sun takes the slot on every frame, in every process.
        const bool secondaryDirectional =
            d.type == LightType::Directional && !d.primaryDirectional;
        L->setCastShadows(d.type == LightType::Area || secondaryDirectional ? false
                                                                            : d.castShadows);
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
            // NO FALLOFF ON A DIRECTIONAL LIGHT. Ogre's default attenuation
            // (const 0.5, quad 0.5) is never used to SHADE a directional light,
            // so this is inert for the picture — it is set because anything that
            // integrates a directional light along a RAY reads it, and a
            // quadratic term over the tens of units such a ray travels crushes
            // the result to black. (Instant Radiosity was the first such
            // consumer and is gone; the voxel light injection's ray march and
            // the ray-query tier are the live ones.)
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
        // THE LAMP-MAP CACHE'S PARAMETER INPUT (ENGINE_CACHE_POLICY_SPEC P3 item
        // 4). What a point/spot shadow map depends on, from this description:
        // the type (a point map is six faces, a spot one), the reach (the
        // shadow camera's far plane is the range) and the cone (a spot camera's
        // FOV is 1.2x the outer angle), and whether it casts at all. NOT the
        // colour, the intensity, the softness or the falloff — a shadow map is
        // depth, so a dimmer lamp casts the same shadow and a colour slider
        // re-renders nothing. The cache compares this key (with the light's
        // pose folded in) once a frame, so a direct caller pushing the same
        // description twice costs nothing either.
        {
            unsigned long long k = 1469598103934665603ull;      // FNV-1a
            const auto fold = [&k](const void *p, size_t bytes) {
                const unsigned char *b = static_cast<const unsigned char *>(p);
                for (size_t i = 0; i < bytes; ++i) { k ^= b[i]; k *= 1099511628211ull; }
            };
            const int type = int(d.type);
            const float spot = d.type == LightType::Spot ? d.spotAngleDegrees : 0.0f;
            fold(&type, sizeof type); fold(&d.range, sizeof d.range);
            fold(&spot, sizeof spot); fold(&d.castShadows, sizeof d.castShadows);
            n.lightShadowKey = k;
        }
        // A CACHED MAP MUST NOT DEPEND ON THE CAMERA THAT RENDERED IT: pin the
        // shadow camera's near plane for point and spot lights
        // (kShadowLampNearClip has the why). A directional light goes back to
        // Ogre's camera-following default — PSSM derives its splits from the
        // viewer and is never cached.
        L->setShadowNearClipDistance(d.type == LightType::Point || d.type == LightType::Spot
                                         ? kShadowLampNearClip : Ogre::Real(-1));
        // THE PROBE CACHE'S LIGHT INPUT (ENGINE_CACHE_POLICY_SPEC P7). A probe
        // capture is a lit render, so a light's colour, intensity, reach, cone,
        // shape, shadowing and channels all change what every probe would hold
        // — and a NEW light is the same statement. Keyed on exactly those
        // fields (not on the call: a direct caller may push the same
        // description twice). The
        // VOXEL half of the same edit is the host's: its GI signature hashes
        // these parameters too, so the voxels re-inject on the drag cadence and
        // re-solve once on settle.
        {
            unsigned long long k = 1469598103934665603ull;      // FNV-1a
            const auto foldBits = [&k](const void *p, size_t n) {
                const unsigned char *b = static_cast<const unsigned char *>(p);
                for (size_t i = 0; i < n; ++i) { k ^= b[i]; k *= 1099511628211ull; }
            };
            const auto fold = [&](const auto &v) { foldBits(&v, sizeof v); };
            fold(int(d.type)); fold(d.colour.r); fold(d.colour.g); fold(d.colour.b);
            fold(d.intensity); fold(d.range); fold(d.spotAngleDegrees); fold(d.spotSoftness);
            fold(d.spotFalloff); fold(d.castShadows); fold(d.rectWidth); fold(d.rectHeight);
            fold(d.doubleSided); fold(d.accurate); fold(d.lightMask);
            foldBits(d.iesProfilePath.data(), d.iesProfilePath.size());
            foldBits(d.texturePath.data(), d.texturePath.size());
            if (k == 0ull) k = 1ull;                             // 0 means "never pushed"
            if (k != n.lightProbeKey) {
                n.lightProbeKey = k;
                staleProbeGrid(GiStaleReason::Light);
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
        // Untied from every cached shadow map first: a fixed light is
        // dereferenced by its node on every update (releaseShadowLamp).
        if (mEngine) mEngine->releaseShadowLamp(this, it->second.light);
        it->second.light->detachFromParent();
        mSceneMgr->destroyLight(it->second.light);
        it->second.light = nullptr;
        mLightNodes.erase(std::remove(mLightNodes.begin(), mLightNodes.end(), id),
                          mLightNodes.end());
        // A recreated light starts with no profile and no mask: forget what the
        // dead one carried, or the next setLight would skip re-assigning it.
        it->second.lightProfilePath.clear();
        it->second.lightMaskPath.clear();
        if (it->second.lightNode) { mSceneMgr->destroySceneNode(it->second.lightNode); it->second.lightNode = nullptr; }
        // A VANISHED LIGHT MUST STOP BOUNCING — and that is a re-INJECTION over
        // the voxels that are already there, never a re-voxelisation: not one
        // voxel's albedo changed (G1). The `false` says so, and under a cascade
        // chain it really does cost zero rebuilds: nothing marks a cascade, the
        // dirty path finds nothing marked, and it re-injects every cascade at
        // the full bounce count instead. Instant Radiosity's by-pointer caches
        // and the single arm's reuse rule are unaffected.
        invalidateGiCaches(nullptr, false, true);
        // Its cached maps need nothing: the lamp leaves the cache's light list,
        // so the next frame releases its slot in every shadow-node instance.
        it->second.lightShadowKey = 0;
        return true;
    } JAH_CATCH(mError, false);
}

Ogre::SceneManager *OgreScene::sceneManager() const { return mSceneMgr; }

// The light behind a node id, and the id behind a light. The reverse lookup is
// a linear walk on purpose: it runs once per shadow map per status readback (at
// most 19 entries), never per frame, and a second index would be one more thing
// releaseNode has to keep honest.
Ogre::Light *OgreScene::ogreLight(NodeId node) const {
    auto it = mNodes.find(node);
    return it == mNodes.end() ? nullptr : it->second.light;
}

NodeId OgreScene::nodeOfLight(const Ogre::Light *light) const {
    if (!light) return 0;
    for (const auto &entry : mNodes)
        if (entry.second.light == light) return entry.first;
    return 0;
}

void OgreScene::destroy() {
    if (!mSceneMgr) return;
    mDestroying = true;
    JAH_TRY {
        // THE SURFACE CACHE BEFORE EVERYTHING, and the order is not tidiness.
        // It holds a CAMERA in this SceneManager, a workspace over this
        // manager and five resident atlases; the manager and every camera in it
        // die further down this function, and a workspace whose SceneManager
        // has gone is a dangling update. The spike this replaced had a stronger
        // version of the same rule (its scratch Item LINKED this scene's
        // datablocks, so a late teardown took `~HlmsDatablock`'s linked-
        // renderable assert) and the regression case that proved it — the exit
        // code after a passing suite — still guards the order.
        mSurfaceCache.reset();
        // THE RAY TIER'S STRUCTURES FOR THIS SCENE, FIRST. They are keyed by
        // this object's ADDRESS and they hold MeshPtrs, so leaving them behind
        // would pin this scene's geometry for the process's life and let the
        // next scene allocated at the same address inherit a TLAS built for
        // someone else's items. Defined in OgreRayQuery.cpp — like every other
        // line of the tier — so no TU without Vulkan ever sees it.
        forgetRayQuery();
        // THE GPU SCENE'S TABLES, with the ray tier and for the same two
        // reasons: they are `UavBufferPacked`s, which must be destroyed while
        // this tree's VaoManager is alive, and the mesh table HOLDS MeshPtrs —
        // and a MeshPtr outliving Root throws in VaoManager (trap 1).
        mGpuScene.destroy();
        // FIRST, before anything else in this scene goes: the overlay system's
        // render-queue listener is registered on THIS SceneManager, and the
        // teardown order the component needs is
        // removeRenderQueueListener -> destroy scenes -> delete OverlaySystem
        // -> delete Root (OgreOverlayHud.cpp's header).
        hud::detach(mSceneMgr);
        // THE HIT DECODE'S DRAWS (PHOTON-HIT-SHADE-1): HlmsAtom-owned objects in
        // this SceneManager's memory, so they die before it does.
        forgetSceneDecodes(mSceneMgr);
        teardownGi();   // VPL lights die while the SceneManager is still alive
        // The atmosphere destroys its Rectangle2D THROUGH the SceneManager, so it
        // has to go while that is still alive (teardown law: components, then the
        // manager).
        destroyAtmosphere();
        // Before the nodes: PlanarReflections holds raw Renderable pointers, and
        // it destroys its own cameras through the SceneManager. It also has to
        // leave this scene's binding and every HlmsPbs host, which its destructor
        // (like VctLighting's) does not do.
        teardownPlanar();
        destroySky();   // also unbinds + destroys the reflection cubemap
        destroyGrid();
        for (auto &kv : mNodes) releaseNode(kv.first, kv.second);
        mNodes.clear();
        mNodeByOgreId.clear();
        // The helper overlay queue's depth anchor: an entity in this
        // SceneManager's memory manager, so it dies before the manager does.
        releaseQueueDepthAnchor();
        for (auto &kv : mMaterials) {
            Ogre::Hlms *hlms = hlmsFor(kv.second);
            if (Ogre::HlmsDatablock *db = hlms->getDatablock(Ogre::IdString(kv.second.datablockName))) {
                forgetDecodeTwinOf(db);   // its decode twin dies first (HlmsAtom.h)
                hlms->destroyDatablock(Ogre::IdString(kv.second.datablockName));
            }
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
        mMeshIdByOgreMesh.clear();
        mCardsByMesh.clear();
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
    unregisterSceneGiBinding(mSceneMgr);
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
    // AND the riders on THIS node's bones (AVATAR_RIG_PERF_SPEC §4): a TagPoint
    // points into a Bone of the Item's SkeletonInstance, which is about to be
    // destroyed. They land under the scene root at the pose they last rendered
    // with; the host re-arms them on its next sync, exactly as it re-arms a
    // share.
    if (n.item && !n.boneRiders.empty()) releaseBoneRiders(id, n);
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
        // ...WITH THE BOX IT IS LEAVING (G1): under a cascade chain only the
        // cascades whose own box intersects this one owe a re-voxelisation for
        // its going — a prop deleted at the far end of a scene costs the near
        // cascades nothing (`markDirtyCascadesPending`).
        if (n.item->getVisibilityFlags() & kGiGeometryBit) {
            const Ogre::Aabb gone = n.item->getWorldAabb();
            invalidateGiCaches(&gone);
        }
        // An UNLIT item the probes capture (P7): leaving the scene is a probe
        // input and nothing else — no voxel ever held it.
        else if (probeSeesItem(n)) staleProbeGrid(GiStaleReason::Moved);
        unindexItemNode(n);   // the item walk: a caster leaving is a change
        // THE MESH TABLE'S REFERENCE, before the Item that held it dies.
        if (n.gpuMeshSlot != 0xFFFFFFFFu) {
            releaseGpuMesh(n.item->getMesh().get());
            n.gpuMeshSlot = 0xFFFFFFFFu;
        }
        n.item->detachFromParent(); mSceneMgr->destroyItem(n.item); n.item = nullptr;
        // AND THE CLIPS (S16, SMOKE_FIX_SPEC_2026_09_11 §1.1). The
        // SkeletonInstance belongs to the Item and has just died with it, while
        // every ClipRec this node holds is a set of raw float*s into that
        // instance's per-animation weight arrays plus an index into its
        // animation list. The host's very next act after a re-attach is a state
        // push (the mirror disables everything before re-attaching), which used
        // to write through all of those and then subscript an animation list
        // with nothing in it: a use-after-free write and a SIGABRT, from
        // nothing worse than a material swap on a rigged node. The clips come
        // back the way a share does — the host re-attaches them, and
        // attachClips' idempotency set is empty again, so they really do.
        ++n.rigGeneration;
        dropNodeClips(id);
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
    if (n.mesh || (n.item && (n.item->getVisibilityFlags() & kGiGeometryBit))) {
        // The box it occupied, where there is one (G1) — see detachItem.
        if (n.item && (n.item->getVisibilityFlags() & kGiGeometryBit)) {
            const Ogre::Aabb gone = n.item->getWorldAabb();
            invalidateGiCaches(&gone);
        } else {
            invalidateGiCaches();
        }
    }
    else if (probeSeesItem(n))
        staleProbeGrid(GiStaleReason::Moved);   // an unlit item the probes captured (P7)
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
    // The node's own tag, and anything riding its bones (detachItem above has
    // usually done the second half; a node with no Item never had one).
    releaseBoneTag(id, n, 0);
    releaseBoneRiders(id, n);
    unindexItemNode(n);   // always: the node is going away, and its pointer with it
    if (n.item)  { n.item->detachFromParent();  mSceneMgr->destroyItem(n.item);   n.item = nullptr; }
    n.meshRef = 0; n.materialRef = 0;
    // The internal light child must go before the reparent loop below would leak it to root.
    if (n.light) {
        if (mEngine) mEngine->releaseShadowLamp(this, n.light);   // see removeLight
        n.light->detachFromParent(); mSceneMgr->destroyLight(n.light); n.light = nullptr;
        mLightNodes.erase(std::remove(mLightNodes.begin(), mLightNodes.end(), id), mLightNodes.end());
    }
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
        if (Ogre::HlmsDatablock *db = hlmsPbs->getDatablock(Ogre::IdString(n.datablockName))) {
            forgetDecodeTwinOf(db);   // its decode twin dies first (HlmsAtom.h)
            hlmsPbs->destroyDatablock(Ogre::IdString(n.datablockName));
        }
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

NodeId OgreScene::track(const Node &n) {
    const NodeId id = ++mNextId;
    Node &rec = mNodes[id] = n;
    rec.selfId = id;
    if (rec.node) {
        rec.ogreId = rec.node->getId();
        mNodeByOgreId[rec.ogreId] = id;
        // Born with the EFFECTIVE state of wherever it hangs: a node created
        // (or adopted) under a hidden parent is hidden until that parent shows.
        rec.shown = rec.visible && inheritedShown(rec.node);
    }
    return id;
}

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

// ---------------------------------------------------------------------------
// HARDWARE RAY TRACING, RESOLVED (owner, 2026-09-15; ledger §425)
// ---------------------------------------------------------------------------
// The scene says what it was authored for and the machine says what it can do;
// this is the AND of the two, and it is the only question any ray-consuming
// stage asks. Deliberately here, beside the scene's other render state, and not
// in the ray tier's own TU: the tier is one consumer of this answer, not its
// owner.
//
// `Engine::rayTracing()` is the PROCESS latch (`--no-ray-query` /
// JAHSHAKA_NO_RAY_QUERY): a diagnostic that makes this box render the picture a
// machine without ray hardware renders, so the fallback is proved on every push
// instead of assumed. `rayQueryAvailable()` is the DEVICE's own answer and
// nothing in the document can move it — which is the whole point of the row:
// On does not force hardware, it asks the editor to SAY when there is none.
bool OgreScene::rayTracingResolved() const {
    if (mRayTracing == RayTracingMode::Off) return false;
    if (!mEngine) return false;
    return mEngine->rayTracing() && mEngine->rayQueryAvailable();
}

void OgreScene::setRayTracing(RayTracingMode mode) {
    const bool wasOn = mRayTracing != RayTracingMode::Off;
    const bool gridWas = probeGridWanted();
    mRayTracing = mode;
    // OFF is a COST guarantee, not only a picture: the ray structures this
    // scene holds are released now, and updateRayQuery skips it from here.
    if (wasOn && mode == RayTracingMode::Off) forgetRayQuery();
    // THE ROW MOVES THE PROBE GRID'S RULE (PHOTON-F12-PCC): at a ray tier the
    // grid is not built, so the row turning the rays on there takes the grid
    // down and turning them off builds it — the two things a technique change
    // already does, and nothing else. Down is `dropProbeGridByRays` (the
    // binding lets go, the datablocks take their sky cube back, the probe
    // record goes with it); up is the from-scratch `rebuildVct`, owed through the
    // flush: the cheap paths' belt refuses a hybrid that wants a grid and has
    // none (refreshCascadesFast / refreshVctFast), so the flush takes the
    // rebuild, and a rebuild that has to wait (a camera, a texture) stays armed.
    const bool gridNow = probeGridWanted();
    if (gridWas == gridNow) return;
    if (!gridNow) dropProbeGridByRays();
    else mGiCachesDirty = true;
}


// ---------------------------------------------------------------------------
// SURFACE-CACHE phase 2 — the cache's frame, and its two readbacks
// ---------------------------------------------------------------------------
//
// WHERE THIS RUNS AND WHY. Once per DRAWN scene from `renderOneFrame`, right
// after `applyPendingGi` — so a material edit or a light write has already
// bumped the signatures the cache compares — and before Root's frame. It
// PLANS (residency, invalidation, this frame's batch); the capture executes
// inside Root's frame, after `updateSceneGraph`, as the first workspace in the
// manager's list (OgreSurfaceCache.cpp, makeWorkspace — the shadow fix).
void OgreScene::updateSurfaceCache() {
    // AUTO FOLLOWS THE RAYS (PHOTON-CARDS-2): the reader of a card is the
    // reflection trace's hit (rq_reflect.comp, jah_rq_card.glsl), so the cache
    // is on exactly where that trace runs — the World row resolved against the
    // machine (`rayReflectionsWanted`) — and costs nothing where it cannot be
    // read. On forces it (a suite, the monitor); Off refuses it.
    const bool want = mGi.cards == GiToggle::On ||
                      (mGi.cards == GiToggle::Auto && rayReflectionsWanted());
    if (!want) {
        if (mSurfaceCache) mSurfaceCache.reset();
        return;
    }
    if (!mSceneMgr) return;
    if (!mSurfaceCache) {
        mSurfaceCache.reset(new SurfaceCache());
        std::string err;
        if (!mSurfaceCache->build(mSceneMgr, err)) {
            // A cache that cannot be built is a reported failure and not a
            // crash: the row stays on, the cache stays null, and the status
            // says `built` false.
            if (!err.empty()) mError = err;
            mSurfaceCache.reset();
            return;
        }
        // THE MOVERS' SHADOW (PHOTON-CARDS-4): the scene's in-frame answers and
        // the ray tier's trace; a scene without rays answers "no trace" and the
        // cards keep the still world's sun term alone.
        CardMoverHooks hooks;
        hooks.frame = [this](CardMoverFrame &f) { return cardMoverFrame(f); };
        hooks.trace = [this](const CardMoverTrace &t) { return traceCardMovers(t); };
        hooks.timeRelight = [this](bool begin) { timeCardRelight(begin); };
        hooks.readTimes = [this](float &a, float &b) { cardMoverTimes(a, b); };
        mSurfaceCache->setMoverHooks(hooks);
    }
    // A CAMERA-RELATIVE CACHE NEEDS A CAMERA, exactly as the cascade chain
    // does, and waits for one the same way: `mGiCamPos` is the authoritative
    // view's last tracked position and is the ORIGIN until a frame has tracked
    // one. Capturing around the origin and then re-capturing everything on the
    // first tracked frame is the defect the chain already learned (audit B3).
    if (!mGiCamPosKnown) return;

    const GiQualityFacts facts =
        giQualityFacts(mGi.quality, mGiDriverStereo ? GiViewProfile::Vr : GiViewProfile::Desktop);
    CardSceneView view;
    view.sceneMgr = mSceneMgr;
    view.viewerPos = mGiCamPos;
    view.budgetTexels = mGi.cardBudgetTexels > 0 ? unsigned(mGi.cardBudgetTexels)
                                                 : facts.cardBudgetTexels;
    view.radius = mGi.cardResidencyRadius > 0.0f ? mGi.cardResidencyRadius
                                                 : facts.cardResidencyRadius;
    // THE LIGHT SIGNATURE THE CACHE KEYS ON IS NOT `mGiLightWriteSerial`, and
    // the difference is a slider drag. That serial bumps on EVERY `setLight`
    // push and every light pose write, colour and intensity included — but the
    // only thing a capture stores from a light is the SHADOW TERM, which a
    // colour or an intensity cannot move. So the signature folded here is what
    // a SHADOW depends on: `Node::lightShadowKey` (the LightDesc fields the
    // shadow map depends on — type, range, spot cone, castShadows; colour and
    // intensity deliberately absent, the same rule the lamp-map cache keeps),
    // the light's derived POSE, and whether it is shown. A colour slider then
    // costs the cache nothing at all, and a lamp that moves costs it exactly
    // the cards whose shadows it could have changed.
    //
    // Over `mLightNodes`, which is the engine's own light index (a hint that is
    // a superset), so this is a handful of quantised folds and not a walk of
    // the node map.
    unsigned long long lightSig = 1469598103934665603ull;
    const auto fold = [&lightSig](unsigned long long v) {
        lightSig ^= v;
        lightSig *= 1099511628211ull;
    };
    const auto foldF = [&fold](float f) {
        // Quantised to a millimetre / a thousandth: float noise below the
        // tolerance the whole pipeline works to must not re-capture a card.
        fold((unsigned long long)(long long)std::lround(double(f) * 1000.0));
    };
    for (NodeId lid : mLightNodes) {
        auto lit = mNodes.find(lid);
        if (lit == mNodes.end() || !lit->second.light) continue;
        const Node &ln = lit->second;
        fold(ln.lightShadowKey);
        fold(ln.shown ? 1ull : 0ull);
        if (ln.node) {
            const Ogre::Vector3 p = ln.node->_getDerivedPosition();
            const Ogre::Quaternion q = ln.node->_getDerivedOrientation();
            foldF(p.x); foldF(p.y); foldF(p.z);
            foldF(q.x); foldF(q.y); foldF(q.z); foldF(q.w);
        }
    }
    view.lightSerial = lightSig;
    // THE RADIANCE SIGNATURE: the shadow signature above plus everything a
    // card's LIT radiance depends on and its capture does not — the colour,
    // the power, the reach and the cone. A colour slider costs the cache a
    // relight of the resident set (the `Jahshaka/CardLight` job, under its own
    // budget) and not one capture. The lights themselves are handed over for
    // the job's light list (below).
    unsigned long long radianceSig = lightSig;
    for (NodeId lid : mLightNodes) {
        auto lit = mNodes.find(lid);
        if (lit == mNodes.end() || !lit->second.light) continue;
        const Ogre::Light *l = lit->second.light;
        const Ogre::ColourValue c = l->getDiffuseColour() * l->getPowerScale();
        const auto foldR = [&radianceSig](float f) {
            radianceSig ^= (unsigned long long)(long long)std::lround(double(f) * 1000.0);
            radianceSig *= 1099511628211ull;
        };
        foldR(c.r); foldR(c.g); foldR(c.b);
        foldR(l->getAttenuationRange()); foldR(l->getAttenuationLinear());
        foldR(l->getAttenuationQuadric());
        foldR(l->getSpotlightInnerAngle().valueRadians());
        foldR(l->getSpotlightOuterAngle().valueRadians());
        foldR(l->getSpotlightFalloff());
        // ...and the light itself, for the relight job: EVERY light node,
        // world space, unculled — the frame's global list is culled against
        // the frame's cameras, and a card lights surfaces off screen.
        view.lights.push_back(lit->second.light);
    }
    // THE CLOUD SHADOW (CLOUDS-2D-2): the snapshot the voxels are injected
    // with, and its serial in the radiance signature — a layer change or a
    // scroll capture relights the resident cards (and recaptures none).
    radianceSig ^= mCloudGiSerial;
    radianceSig *= 1099511628211ull;
    if (mCloudGiState.field) {
        const FogHlmsListener::CloudShadowState &cs = mCloudGiState;
        view.cloudField = cs.field;
        view.cloudMap[0] = cs.invTile;  view.cloudMap[1] = cs.strength;
        view.cloudMap[2] = cs.scroll[0]; view.cloudMap[3] = cs.scroll[1];
        view.cloudSun[0] = cs.sunThrow[0]; view.cloudSun[1] = cs.sunThrow[1];
        view.cloudSun[2] = cs.altitude;    view.cloudSun[3] = cs.invMuSun;
    }
    view.radianceSerial = radianceSig;
    view.lightBudgetTexels = facts.cardLightTexels;
    // THE INDIRECT HALF: the chain the pixel's cones march (the cascade-0
    // VctLighting the pass buffer is filled from), and THE RE-INJECTION
    // SIGNATURE — folded ONLY from what moves when an injection LANDS, never
    // from the write-time serials (a light write or a material generation
    // bumps at the WRITE, and a dragged light re-marched the whole resident set
    // against voxels that had not moved, every frame): the chain's settles, each
    // cascade's rebuilds and lattice cell, the VctLighting objects themselves,
    // the single volume's own landed-injection count (OgreGi.cpp), and the
    // environment the escapes read (below).
    view.vct = mVctLighting;
    view.indirectBudgetTexels = facts.cardIndirectTexels;
    // ...and THE AMBIENT AT GI OFF (PHOTON-CARDS-5): the scene's SH, the engine's
    // own SH x gain (applySkyAmbient), in the indirect signature below.
    view.ambientSh = mLastAmbientSh;
    {
        unsigned long long sig = 1469598103934665603ull;
        const auto foldI = [&sig](unsigned long long v) {
            sig ^= v;
            sig *= 1099511628211ull;
        };
        foldI((unsigned long long)mGiChainSettles);
        foldI(mGiMonoInjections);
        // ...AND THE ENVIRONMENT THE MARCH'S ESCAPES READ, which is not an
        // injection at all: noteEnvironmentChanged hands the new sky to every
        // VctLighting at once (applyVctEnvironment) and the pixel reads it the
        // same frame, so the card's escape must too. The values
        // applyCascadeEnvironment hands over, quantised.
        {
            const Ogre::TextureGpu *cube =
                (mReflectionTex && mEnvLightScale > 0.0f) ? mReflectionTex : nullptr;
            foldI((unsigned long long)(uintptr_t)cube);
            const float gain[3] = { mEnvLightGain.r, mEnvLightGain.g, mEnvLightGain.b };
            for (float g : gain) foldI((unsigned long long)(long long)std::lround(double(g) * 1e4));
            for (int k = 0; k < 27; ++k)
                foldI((unsigned long long)(long long)std::lround(double(mLastAmbientSh[k]) * 1e4));
        }
        foldI((unsigned long long)(uintptr_t)mVctLighting);
        for (const VctCascade &c : mVctCascades) {
            foldI((unsigned long long)(uintptr_t)c.lighting);
            foldI(c.rebuilds);
            foldI((unsigned long long)c.latticeX);
            foldI((unsigned long long)c.latticeY);
            foldI((unsigned long long)c.latticeZ);
        }
        view.indirectSerial = sig;
    }

    // THE CANDIDATE LIST — THE SCENE'S OWN WALK, handed over rather than
    // reached for. The predicate is the same one the voxel side uses, and each
    // clause is the same statement it makes there:
    //   * `kGiGeometryBit` — a surface that BOUNCES light. It excludes the sky,
    //     unlit overlays, line meshes, billboards, helpers and backdrops for
    //     free, and it excludes a MOVER, which is the point: a card set is
    //     stored lighting, and a mover is the engine's word for "not part of
    //     the room's stored lighting".
    //   * `shown` — a hidden object photographs nothing.
    //   * a baked card list — every skinned mesh, every line mesh and every
    //     model opened without a bake has none, and gets none here.
    //   * OPAQUE (PHOTON-GATHER-1d fix round) — a card records a surface's
    //     albedo, normal and depth as the capture's prepass writes them, and a
    //     blended (Fade/Blend/Glass) sub-item is not a surface the prepass
    //     holds: the gather's F2 discard keeps it out, and the capture's own
    //     piece fills the same hook. An item with ANY blended sub-item gets no
    //     cards; its bounce is the voxels'.
    // The radius itself is the cache's; this walk hands over everything that
    // COULD be resident and lets the Component decide who is.
    view.candidates.reserve(mItemNodes.size());
    for (Node *n : mItemNodes) {
        if (!n || !n->item || !n->node || !n->shown) continue;
        if (!(n->item->getVisibilityFlags() & kGiGeometryBit)) continue;
        bool blended = false;
        for (size_t si = 0; si < n->item->getNumSubItems() && !blended; ++si) {
            const Ogre::HlmsDatablock *db = n->item->getSubItem(si)->getDatablock();
            blended = db && db->getBlendblock(false)->isAutoTransparent();
        }
        if (blended) continue;
        const std::vector<MeshCardDesc> *cards = meshCardsFor(n->item->getMesh().get());
        if (!cards || cards->empty()) continue;
        CardSceneView::Candidate c;
        c.node = n->selfId;
        c.itemSlot = n->itemSlot;
        c.item = n->item;
        c.sceneNode = n->node;
        c.material = n->materialRef;
        c.cards = cards;
        c.lodBounds = lodBoundsFor(n->item->getMesh().get());
        view.candidates.push_back(c);
    }
    mSurfaceCache->update(view);
}

bool OgreScene::readCardTexel(NodeId node, unsigned card, float u, float v, CardSample &out) {
    out = CardSample();
    if (!mSurfaceCache) return false;
    return mSurfaceCache->readTexel(node, card, u, v, out);
}

bool OgreScene::readCardAt(const Vec3 &world, const Vec3 &normal, CardSample &out,
                           NodeId onlyNode) {
    out = CardSample();
    if (!mSurfaceCache) return false;
    return mSurfaceCache->readAt(toOgre(world), toOgre(normal), out, onlyNode);
}

bool OgreScene::dumpCardAtlas(const std::string &prefix, std::string &err) {
    if (!mSurfaceCache) { err = "the surface cache is not built"; return false; }
    return mSurfaceCache->dump(prefix, err);
}

}}}  // namespace jahshaka::engine::detail
