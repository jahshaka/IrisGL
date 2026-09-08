// Fog: Ogre's AtmosphereNpr adopted for its exponential fog MATH, wired to
// Jahshaka's authored colour and extended with a height layer.
//
// WHY A COMPONENT WE ONLY HALF WANT
// AtmosphereNpr bundles four things: a procedural sky quad, a fog model, a
// sun-light link and an ambient link. We want the second only. The component
// makes that separable, but the recipe is not obvious and is load-bearing:
//
//   setSky( sm, true );        // creates the sky Rectangle2D and registers us
//   setSky( sm, false );       // hides the quad (Rectangle2D honours setVisible,
//                              // unlike BillboardSet2) and UNregisters us
//   sm->_setAtmosphere( this ) // registers again — fog only, no visible sky
//
// The first call is not optional: AtmosphereNpr::_update() asserts on, and then
// dereferences, the per-SceneManager Rectangle2D. The last call is what actually
// puts hlms_fog into the pass properties, via preparePassHash.
//
// setLight() is NEVER called. syncToLight() returns immediately without a linked
// light, so the component cannot touch SceneManager::setAmbientLight — our SH
// ambient (and everything the sky/IBL lane computes) stays exactly as it was.
// This is by construction, not by luck: there is no other path from the component
// to the ambient.
//
// What the component gives the shader: hlms_fog + a const buffer with fogDensity
// and the two breakthrough terms, consumed by the stock HlmsPbs pixel shader.
// What it CANNOT give: an authored fog colour (it computes a procedural sky
// colour per vertex) and height fog. Those two ride the pass-buffer extension
// below and media/Hlms/Jahshaka/JahFog_piece_vs_piece_ps.any.
#include "EnginePrivate.h"

namespace jahshaka { namespace engine { namespace detail {

std::map<const Ogre::SceneManager *, FogState> FogHlmsListener::sFogState;   // render thread only
std::map<const Ogre::SceneManager *, float>    FogHlmsListener::sSceneTime;  // render thread only
std::map<const Ogre::SceneManager *, FogHlmsListener::IfdState> FogHlmsListener::sIfdState;  // render thread only
Ogre::HlmsPbs                                 *FogHlmsListener::sPbs = nullptr;

FogHlmsListener gFogListener;

void FogHlmsListener::registerScene(const Ogre::SceneManager *sm, const FogState &p) {
    sFogState[sm] = p;
}

void FogHlmsListener::unregisterFog(const Ogre::SceneManager *sm) {
    sFogState.erase(sm);
}

void FogHlmsListener::unregisterScene(const Ogre::SceneManager *sm) {
    sFogState.erase(sm);
    // The clock table is keyed by SceneManager POINTER, and Ogre recycles those
    // addresses — a scene destroyed and another created would otherwise inherit
    // a stale time. Cleared here rather than in setFog's disable branch: this
    // one is the scene's teardown (OgreScene.cpp), that one is "fog off".
    sSceneTime.erase(sm);
    sIfdState.erase(sm);
}

void FogHlmsListener::setSceneTime(const Ogre::SceneManager *sm, float seconds) {
    sSceneTime[sm] = seconds;
}

float FogHlmsListener::sceneTime(const Ogre::SceneManager *sm) {
    const auto it = sSceneTime.find(sm);
    return it == sSceneTime.end() ? 0.0f : it->second;
}

void FogHlmsListener::setIfdState(const Ogre::SceneManager *sm, const IfdState &state) {
    sIfdState[sm] = state;
}

FogHlmsListener::IfdState FogHlmsListener::ifdState(const Ogre::SceneManager *sm) {
    const auto it = sIfdState.find(sm);
    if (it != sIfdState.end()) return it->second;
    // Not zeros: a scene that has a field bound but never pushed state
    // (impossible today — the arm writes it before it binds) must still render
    // at the calibrated brightness rather than at upstream's raw one. GiParams
    // is the single source of both numbers. The probe counts stay 0, which
    // makes the sky-visibility threshold 0 and every depth sample "sky" — so
    // the ambient dial is what has to be trusted to be 0 in that state, and it
    // is: the arm writes the counts and the dial in the same call.
    IfdState fallback;
    const GiParams defaults;
    fallback.intensity = defaults.ddgiIntensity;
    fallback.ambient = 0.0f;
    return fallback;
}

FogState FogHlmsListener::lookup(const Ogre::SceneManager *sm) {
    FogState p;
    const auto it = sFogState.find(sm);
    if (it != sFogState.end()) p = it->second;
    return p;
}

void FogHlmsListener::setPbs(Ogre::HlmsPbs *pbs) { sPbs = pbs; }

// THE IRRADIANCE-FIELD ALIGNMENT COMPENSATION — an UPSTREAM DEFECT worked
// around from our side, and the only reason DDGI can share a pass buffer with
// this listener at all.
//
// `IrradianceField::getConstBufferSize()` returns `sizeof(float) * (4*3 + 4 + 4)`
// = 80 bytes (OgreIrradianceField.cpp:770). But the struct
// `fillConstBufferData` writes through — and the `IrradianceField` struct the
// generated shader declares (Hlms/Pbs/Any/IrradianceField_piece_ps.any's
// uniform block) — is 96 bytes: a float4x3, then THREE float4s
// (numProbesAggregated + 2 padding, the depth pair, the irradiance pair). The
// last one is missing from the size. So HlmsPbs:
//
//   * reserves 16 bytes too few for the pass buffer, and
//   * advances the write pointer by 20 floats after a 24-float write,
//
// which puts THIS listener's first float4 on top of the field's irradiance
// atlas parameters (irradBorderedRes / irradFullWidth / irradInvFullResolution).
// The symptom is not a crash: the irradiance UVs collapse to texel 0 and DDGI
// contributes an almost-black constant, which reads as "the technique is
// broken" rather than as a buffer bug. Measured exactly that way in this lane.
//
// The compensation is to RESERVE four extra floats and then SKIP them —
// deliberately not to write them. The shader declares nothing extra: its
// IrradianceField struct already occupies those four floats, and they already
// hold the values fillConstBufferData put there. Writing anything (even zeros)
// would reproduce the defect. With the skip, the CPU and the shader agree from
// the field's first float to our last, and the reservation finally covers the
// field's real write — upstream's own samples, which attach no listener at all,
// still overrun their pass-buffer map by those 16 bytes.
//
// Conditions match HlmsPbs's own exactly (OgreHlmsPbs.cpp:1784, inside
// `if(!casterPass)`): a bound field, and not a shadow-caster pass. A caster
// pass neither sets the property nor fills the block, so it must get no
// padding either.
//
// FOR UPSTREAM (reported by this lane; belongs in SPECS/OGRE_UPSTREAM_ISSUES.md
// once the lead files it): the one-line fix is `4u*3u + 4u + 4u + 4u`. We do not
// patch it — a patch here would move the engine ABI and force a build-ogre.sh
// rerun on every tree, for something a host-side four-float skip fixes
// completely.
Ogre::uint32 FogHlmsListener::ifdAlignFloats(bool casterPass) {
    if (casterPass || !sPbs) return 0u;
    return sPbs->getIrradianceField() ? 4u : 0u;
}

Ogre::uint32 FogHlmsListener::getPassBufferSize(const Ogre::CompositorShadowNode *, bool casterPass,
                                                bool, Ogre::SceneManager *) const {
    // Constant, fog on or off, caster or not: the shader's struct may be SHORTER
    // than the buffer (it is, whenever fog is off), never longer. Four for the
    // shader clock (HLMS_ADOPTION P5) — declared only by materials that carry a
    // generated piece — and four for the DDGI block (GI_UNIFIED P1 and the
    // ambient fix), declared only while an IrradianceField is bound. Both
    // written always, because this hook cannot know which materials the pass
    // will draw, and sixteen unconditional bytes are cheaper than a size that
    // varies per pass.
    // Plus the irradiance-field alignment pad, which is the one thing here that
    // MUST vary per pass (see ifdAlignFloats above).
    return (16u + ifdAlignFloats(casterPass)) * sizeof(float);
}

float *FogHlmsListener::preparePassBuffer(const Ogre::CompositorShadowNode *, bool casterPass, bool,
                                          Ogre::SceneManager *sceneManager, float *passBufferPtr) {
    // SKIP the four floats upstream's IrradianceField block wrote but did not
    // count — SKIP, never write: `fillConstBufferData` already put the field's
    // irradiance-atlas parameters there, and zeroing them collapses every
    // irradiance UV onto texel 0 (measured: DDGI goes to an almost-black
    // constant). The shader declares nothing for them; its IrradianceField
    // struct already occupies them, which is exactly the discrepancy.
    passBufferPtr += ifdAlignFloats(casterPass);
    const FogState p = lookup(sceneManager);
    // The height layer integrates from the CAMERA's altitude, so the shader needs
    // it; this hook runs inside HlmsPbs::preparePassBuffer, where the camera of
    // the pass being built is current.
    float cameraY = 0.0f;
    if (const Ogre::Camera *cam = sceneManager->getCamerasInProgress().renderingCamera)
        cameraY = cam->getDerivedPosition().y;
    *passBufferPtr++ = p.r;
    *passBufferPtr++ = p.g;
    *passBufferPtr++ = p.b;
    *passBufferPtr++ = p.heightDensity;
    *passBufferPtr++ = p.heightFalloff;
    *passBufferPtr++ = p.heightLevel;
    *passBufferPtr++ = cameraY;
    *passBufferPtr++ = 0.0f;
    // The clock. Four floats so the struct stays 16-byte aligned like every
    // other member of a std140 buffer; x is seconds, yzw are reserved for the
    // things a generated piece will want next (frame index, delta, a seed).
    *passBufferPtr++ = sceneTime(sceneManager);
    *passBufferPtr++ = 0.0f;
    *passBufferPtr++ = 0.0f;
    *passBufferPtr++ = 0.0f;
    // The DDGI block, same four-float alignment rule. Read by
    // media/Hlms/Jahshaka/JahIfd_piece_ps.any, which only exists in the
    // generated shader while an IrradianceField is bound: x scales the field's
    // irradiance, y scales the ambient sky-visibility term (0 removes it
    // through a uniform branch), zw are the field's Y and Z probe counts, which
    // upstream's own IrradianceField block does not carry and the visibility
    // threshold needs.
    const IfdState ifd = ifdState(sceneManager);
    *passBufferPtr++ = ifd.intensity;
    *passBufferPtr++ = ifd.ambient;
    *passBufferPtr++ = ifd.numProbesY;
    *passBufferPtr++ = ifd.numProbesZ;
    return passBufferPtr;
}

void OgreScene::ensureAtmosphere() {
    if (mAtmosphere) return;
    Ogre::VaoManager *vao = mRoot->getRenderSystem()->getVaoManager();
    if (!vao) return;
    // The constructor loads the "Ogre/Atmo/NprSky" material and THROWS when the
    // Atmosphere media is missing, so it runs inside the guard like every other
    // Ogre call here: a scene with no fog is the failure mode, never an exception
    // crossing the boundary. It can only run after registerCommonMaterials() (and
    // therefore after the first render window and Hlms registration), which every
    // caller satisfies — scenes exist only after Engine::createView().
    JAH_TRY {
        mAtmosphere = new Ogre::AtmosphereNpr(vao);
        mAtmosphere->setSky(mSceneMgr, true);      // creates the sky quad, registers
        mAtmosphere->setSky(mSceneMgr, false);     // hides the quad, unregisters
        mSceneMgr->_setAtmosphere(mAtmosphere);    // fog only, no sky
    } JAH_CATCH(mError, );
}

void OgreScene::destroyAtmosphere() {
    if (!mAtmosphere) return;
    // ~AtmosphereNpr un-registers itself from every SceneManager it knows and
    // destroys their Rectangle2Ds — which is why this must precede the manager.
    JAH_TRY {
        delete mAtmosphere;
    } JAH_CATCH(mError, );
    mAtmosphere = nullptr;
}

void OgreScene::setFog(const FogDesc &desc) {
    if (!desc.enabled) {
        // Bit-exact off: no atmosphere means no hlms_fog property, which means the
        // fog code is not compiled into the shader at all. Every offscreen pixel
        // suite depends on this.
        destroyAtmosphere();
        FogHlmsListener::unregisterFog(mSceneMgr);
        return;
    }
    ensureAtmosphere();
    if (!mAtmosphere) return;   // media missing: the scene renders unfogged, mError says why

    JAH_TRY {
        Ogre::AtmosphereNpr::Preset preset = mAtmosphere->getPreset();
        preset.fogDensity            = std::max(desc.density, 0.0f);
        preset.fogBreakMinBrightness = std::max(desc.breakMinBrightness, 0.0f);
        preset.fogBreakFalloff       = std::max(desc.breakFalloff, 0.0f);
        // Everything else in the preset drives the sky and the (unlinked) sun; the
        // hidden quad and the absent light make those values unobservable.
        mAtmosphere->setPreset(preset);

        FogState s;
        s.r = desc.colour.r; s.g = desc.colour.g; s.b = desc.colour.b;
        s.heightDensity = std::max(desc.heightDensity, 0.0f);
        s.heightFalloff = desc.heightFalloff;
        s.heightLevel   = desc.heightLevel;
        FogHlmsListener::registerScene(mSceneMgr, s);   // read by preparePassBuffer
    } JAH_CATCH(mError, );
}

}}}  // namespace jahshaka::engine::detail
