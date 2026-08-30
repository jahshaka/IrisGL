// Linear distance fog: the HlmsPbs pass-buffer listener and the per-scene table
// it reads. See EnginePrivate.h for the layout rationale.
#include "EnginePrivate.h"

namespace jahshaka { namespace engine { namespace detail {

std::map<const Ogre::SceneManager *, FogParams> FogHlmsListener::sFogParams;   // render thread only

FogHlmsListener gFogListener;

void FogHlmsListener::registerScene(const Ogre::SceneManager *sm, const FogParams &p) {
    sFogParams[sm] = p;
}

void FogHlmsListener::unregisterScene(const Ogre::SceneManager *sm) {
    sFogParams.erase(sm);
}

FogParams FogHlmsListener::lookup(const Ogre::SceneManager *sm) {
    FogParams p;
    const auto it = sFogParams.find(sm);
    if (it != sFogParams.end()) p = it->second;
    return p;
}

Ogre::uint32 FogHlmsListener::getPassBufferSize(const Ogre::CompositorShadowNode *, bool /*casterPass*/,
                                                bool, Ogre::SceneManager *) const {
    // Caster passes get the bytes too (constant layout); their shaders never
    // declare the members, which is legal — the block may be smaller than the buffer.
    return 8u * sizeof(float);
}

float *FogHlmsListener::preparePassBuffer(const Ogre::CompositorShadowNode *, bool, bool,
                                          Ogre::SceneManager *sceneManager, float *passBufferPtr) {
    FogParams p = lookup(sceneManager);
    *passBufferPtr++ = p.r;
    *passBufferPtr++ = p.g;
    *passBufferPtr++ = p.b;
    *passBufferPtr++ = p.enabled ? 1.0f : 0.0f;
    *passBufferPtr++ = p.start;
    *passBufferPtr++ = p.end;
    *passBufferPtr++ = 1.0f / std::max(p.end - p.start, 1e-4f);
    *passBufferPtr++ = 0.0f;
    return passBufferPtr;
}

void OgreScene::setFog(bool enabled, const Colour &colour, float start, float end) {
    FogParams p;
    p.r = colour.r; p.g = colour.g; p.b = colour.b;
    p.start = start; p.end = end;
    p.enabled = enabled;
    FogHlmsListener::registerScene(mSceneMgr, p);   // read by FogHlmsListener::preparePassBuffer
}

}}}  // namespace jahshaka::engine::detail
