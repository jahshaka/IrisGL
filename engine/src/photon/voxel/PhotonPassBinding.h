// THE PHOTON VOLUMES IN A PBS PASS (OWN-GI-1) — how the voxel cascades and the
// irradiance field reach every PBS-family shader without HlmsPbs holding them.
//
// The volumes are OURS (photon/voxel/: PhotonVoxelLighting, PhotonIrradianceField —
// an owned copy, the way an application owns Terra), so HlmsPbs keeps
// `mVctLighting == nullptr` and `mIrradianceField == nullptr` for ever. A derived
// class could not have carried them: upstream's VctLighting/IrradianceField declare
// no virtuals and HlmsPbs calls their sizing, filling and texture getters through a
// base pointer. What does carry them is the HlmsListener route Ogre gives every
// application for exactly this (OgreHlmsListener.h) — the same route the sky's
// environment slot, the gather and the cloud field already ride. Ogre allows ONE
// listener per Hlms, so these are the Photon half of FogHlmsListener's hooks, called
// from each hook FIRST (OgreFog.cpp):
//
//   preparePassHash            the pass's OWN scene's volumes (sceneGiBindingOf —
//                              the per-scene record every pass resolves), the pass
//                              properties upstream's HlmsPbs::preparePassHash set for
//                              a bound VctLighting/IrradianceField, word for word, but
//                              the two that would make HlmsPbs number registers in the
//                              MIDDLE of set 0 (`vct_num_probes`, `irradiance_field`)
//                              ride under our names (`jah_vct_cascades`, `jah_ifd`)
//   numExtraPassTextures       the volumes' textures, claimed at the END of set 0
//   propertiesMerged           our two names become upstream's, and HlmsPbs's own
//                              reaction to them (the TBN, the registers) is made —
//                              upstream's PBS pieces (the PCC/VCT blend in
//                              800.PixelShader keys on `vct_num_probes`) stay live
//   setupRootLayout            the arrays past upstream's four (HlmsPbs adds those)
//   passBufferSize/prepare...  the cascades' and the field's blocks, FIRST in the
//                              listener's extension: upstream wrote them last in its
//                              own buffer, immediately before the extension, so the
//                              layout the shader declares is the one it always was
//   bind                       the textures, at the claimed slots
//   analyzeBarriers            what HlmsPbs's analyzeBarriers did for a bound volume
//                              (called by ScenePbs and HlmsAtom, ours)
//
// Render thread only, like the rest of the listener's per-pass state.
#pragma once

#include <OgreHlmsCommon.h>
#include <OgreResourceTransition.h>

#include <cstddef>

namespace Ogre {
class CompositorManager2;
class CompositorPassDef;
class BarrierSolver;
class CommandBuffer;
class Hlms;
class HlmsDatablock;
class RootLayout;
class SceneManager;
}   // namespace Ogre

namespace jahshaka { namespace engine { namespace detail {

struct PhotonPassBinding {
    /// The colour pass `hlms` is preparing: resolve `sceneManager`'s volumes, set the
    /// pass properties, remember what this host binds. A caster pass binds nothing.
    static void preparePassHash(bool casterPass, Ogre::SceneManager *sceneManager, Ogre::Hlms *hlms);
    /// How many set-0 texture slots the volumes claim — a pure function of the
    /// property set (the hook's contract: a cached shader must be reproducible).
    static Ogre::uint16 numExtraPassTextures(const Ogre::HlmsPropertyVec &properties, bool casterPass);
    /// Per renderable, after HlmsPbs numbered its own registers: upstream's names,
    /// upstream's reaction to them, and the registers — the volumes' slots END at
    /// `endSlot` (the listener's own extras follow them to set0_texture_slot_end).
    static void propertiesMerged(Ogre::Hlms *hlms, size_t tid, Ogre::int32 endSlot);
    /// The volume arrays HlmsPbs::setupRootLayout does not add itself.
    static void setupRootLayout(Ogre::RootLayout &rootLayout, const Ogre::HlmsPropertyVec &properties);
    /// The cascades' block + the field's block for this pass (bytes).
    static Ogre::uint32 passBufferSize(bool casterPass, Ogre::SceneManager *sceneManager);
    static float *preparePassBuffer(bool casterPass, Ogre::SceneManager *sceneManager, float *passBufferPtr);
    /// Binds the host's claimed textures from `texUnit`; returns the next free unit.
    static size_t bind(bool casterPass, Ogre::CommandBuffer *commandBuffer,
                       const Ogre::HlmsDatablock *datablock, size_t texUnit);
    /// The volumes the pass's scene binds are READ by the pixel stage.
    static void analyzeBarriers(Ogre::BarrierSolver &barrierSolver,
                                Ogre::ResourceTransitionArray &resourceTransitions,
                                const Ogre::SceneManager *sceneManager, bool casterPass);
    /// A volume is about to be deleted: no host's pass state may still name it.
    static void forget(const void *volume);
    /// The field's samplerblock reference, given back before Root (~OgreEngine).
    static void releaseSamplers();

    /// THE GI READERS (ENGINE-UPGRADE-ASYNC-3 stage 2). A pass READS THE GI when its definition says
    /// so (CompositorPassDef::mJahReadsGi, true unless the chain cleared it on a pass whose shaders
    /// sample none: the G-buffer prepasses, the card capture, the overlays, the Atom id passes) and
    /// it is a scene pass that is not a shadow caster pass, or a custom pass (the ray tier's photon
    /// trace / resolve, photon_update). Quads, clears, copies and the shadow nodes' passes read none.
    static bool passReadsGi(const Ogre::CompositorPassDef *def);
    /// Is the scene pass being prepared a GI reader? The pass the scene manager is rendering, else
    /// the scene pass the always-on listener saw start (analyzeBarriers runs before Ogre names the
    /// current pass), else YES (an unknown pass is a reader). Every binding call below asks it: the
    /// GI's properties, pass-buffer blocks, textures and barriers exist ONLY on reader passes.
    static bool scenePassReadsGi(const Ogre::SceneManager *sceneManager);
    /// The always-on listener (CompositorManager2::addPassListener: every workspace's passes, the
    /// probe captures' included), installed with the engine's compositor and removed before it dies.
    static void installPassListener(Ogre::CompositorManager2 *cm);
    static void uninstallPassListener(Ogre::CompositorManager2 *cm);
    /// photonBindingStatus's counts (PhotonStatus.h).
    static unsigned long long readerPrepares();
    static unsigned long long nonReaderPrepares();
};

}}}   // namespace jahshaka::engine::detail
