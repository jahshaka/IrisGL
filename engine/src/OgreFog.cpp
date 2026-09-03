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

FogHlmsListener gFogListener;

void FogHlmsListener::registerScene(const Ogre::SceneManager *sm, const FogState &p) {
    sFogState[sm] = p;
}

void FogHlmsListener::unregisterScene(const Ogre::SceneManager *sm) {
    sFogState.erase(sm);
}

FogState FogHlmsListener::lookup(const Ogre::SceneManager *sm) {
    FogState p;
    const auto it = sFogState.find(sm);
    if (it != sFogState.end()) p = it->second;
    return p;
}

Ogre::uint32 FogHlmsListener::getPassBufferSize(const Ogre::CompositorShadowNode *, bool /*casterPass*/,
                                                bool, Ogre::SceneManager *) const {
    // Constant, fog on or off, caster or not: the shader's struct may be SHORTER
    // than the buffer (it is, whenever fog is off), never longer.
    return 8u * sizeof(float);
}

float *FogHlmsListener::preparePassBuffer(const Ogre::CompositorShadowNode *, bool, bool,
                                          Ogre::SceneManager *sceneManager, float *passBufferPtr) {
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
        FogHlmsListener::unregisterScene(mSceneMgr);
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
