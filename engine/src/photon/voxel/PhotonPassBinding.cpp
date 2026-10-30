// THE PHOTON VOLUMES IN A PBS PASS (OWN-GI-1) — see PhotonPassBinding.h for the route.
//
// EVERY RULE HERE IS A LINE OF UPSTREAM'S HlmsPbs MOVED, NOT A NEW RULE: the pass
// properties are OgreHlmsPbs.cpp's `if( mVctLighting )` / `if( mIrradianceField )`
// block of preparePassHash; the TBN and the register order are its
// notifyPropertiesMergedPreGenerationStep; the arrays are its setupRootLayout; the
// blocks are its preparePassHash pass-buffer tail; the bindings are its
// fillBuffersFor; the barriers are its analyzeBarriers. Each was keyed on a pointer
// HlmsPbs held; each is keyed here on the pass's own scene's record.
#include "photon/voxel/PhotonPassBinding.h"

#include "EnginePrivate.h"
#include "photon/voxel/PhotonIrradianceField.h"
#include "photon/voxel/PhotonVoxelLighting.h"

#include <Compositor/OgreCompositorManager2.h>
#include <Compositor/OgreCompositorWorkspaceListener.h>
#include <Compositor/Pass/OgreCompositorPass.h>
#include <Compositor/Pass/OgreCompositorPassDef.h>
#include <Compositor/Pass/PassScene/OgreCompositorPassScene.h>
#include <Compositor/Pass/PassScene/OgreCompositorPassSceneDef.h>
#include <Compositor/Pass/PassWarmUp/OgreCompositorPassWarmUpDef.h>
#include <CommandBuffer/OgreCbTexture.h>
#include <CommandBuffer/OgreCommandBuffer.h>
#include <Cubemaps/OgreParallaxCorrectedCubemapBase.h>
#include <OgreCamera.h>
#include <OgreHlms.h>
#include <OgreHlmsManager.h>
#include <OgreRenderSystem.h>
#include <OgreRoot.h>
#include <OgreRootLayout.h>
#include <OgreSceneManager.h>

#include <limits>
#include <mutex>

namespace jahshaka { namespace engine { namespace detail {

namespace {
// The two pass properties that stand in for upstream's `vct_num_probes` and
// `irradiance_field` until the renderable's registers are numbered. HlmsPbs reads
// those two names in notifyPropertiesMergedPreGenerationStep to number the volumes'
// registers between its own (OgreHlmsPbs.cpp, `numVctProbes > 0`): with the volumes
// not HlmsPbs's any more, those slots would be declared and never bound.
const Ogre::IdString kJahVctCascades("jah_vct_cascades");
const Ogre::IdString kJahIfd("jah_ifd");
// Upstream's names (PbsProperty::*), read by its pieces and by ours.
const Ogre::IdString kVctNumProbes("vct_num_probes");
const Ogre::IdString kVctAnisotropic("vct_anisotropic");
const Ogre::IdString kVctSdfQuality("vct_enable_specular_sdf_quality");
const Ogre::IdString kVctDisableSpecular("vct_disable_specular");
const Ogre::IdString kIrradianceField("irradiance_field");
const Ogre::IdString kNormalMap("normal_map");
const Ogre::IdString kNeedsReflDir("needs_refl_dir");
const Ogre::IdString kNeedsEnvBrdf("needs_env_brdf");
const Ogre::IdString kNeedsViewDir("needs_view_dir");
const Ogre::IdString kNormal("hlms_normal");
const Ogre::IdString kTangent("hlms_tangent");
const Ogre::IdString kQTangent("hlms_qtangent");
const Ogre::IdString kShadowCaster("hlms_shadowcaster");
const Ogre::IdString kJahLampChain("jah_lamp_chain");

/// The light volumes per cascade, in the order the voxel lighting lists them
/// (PhotonVoxelLighting::getLightVoxelTextures) and the pieces declare them — the
/// voxel lighting's own count, so the two cannot disagree.
constexpr Ogre::int32 kVolumesPerCascade = Ogre::int32(Ogre::PhotonVoxelLighting::kVolumesPerCascade);
constexpr Ogre::int32 kFieldTextures = Ogre::int32(Ogre::PhotonIrradianceField::kPassTextures);

/// THE ONE CONDITION for the field in a pass: its pass properties, its textures
/// and its pass-buffer block all follow it (they must agree, or the shader's
/// struct and the buffer the listener writes drift apart).
bool fieldBound(const SceneGiBinding *b) { return b && b->ifd; }

/// What one host binds in the pass it last prepared. One per Hlms type, like
/// FogHlmsListener::PassBinds: RenderQueue::renderPassPrepare prepares EVERY
/// registered Hlms of a pass in turn, so a shared copy would be overwritten by
/// whichever host ran last.
struct HostBinds {
    Ogre::PhotonVoxelLighting *vct = nullptr;
    Ogre::PhotonIrradianceField *ifd = nullptr;
    Ogre::PhotonVoxelLighting *lampChain = nullptr;   ///< the no-ray tier's lamp chain (lampChainOf)
};
HostBinds sHost[Ogre::HLMS_MAX];   // render thread only

/// The voxel cascades' light-volume registers, in the order the voxel lighting lists them: the total (level 0), the
/// three directional axes, the coverage per half-axis and the surface position per half. propertiesMerged numbers
/// them; the door names them.
const char *const kVolumeRegisters[] = { "vctProbes",    "vctProbeX",    "vctProbeY",    "vctProbeZ",
                                         "vctProbeCovP", "vctProbeCovN", "vctProbePosP", "vctProbePosN" };
static_assert(sizeof(kVolumeRegisters) / sizeof(kVolumeRegisters[0]) ==
                  size_t(Ogre::PhotonVoxelLighting::kVolumesPerCascade),
              "one register name per light volume the voxel lighting lists");
// THE LAMP CHAIN'S FOUR VOLUMES: the coverage and the surface position per half - kVolumeRegisters' entries 4 to 7 (the
// voxel lighting's own order: getLightVoxelTextures(c)[4 + k]) - each an array of the chain's cascades
constexpr Ogre::int32 kLampChainFirstVolume = 4;
constexpr Ogre::int32 kLampChainVolumes = 4;
const char *const kLampChainRegisters[] = { "jahLampCovP", "jahLampCovN", "jahLampPosP", "jahLampPosN" };
static_assert(sizeof(kLampChainRegisters) / sizeof(kLampChainRegisters[0]) == size_t(kLampChainVolumes),
              "one register per lamp chain volume");

/// THE OPAQUE PASSES' DOOR (7-1): one record per (pass, host), the newest preparation's.
std::mutex sPassRecordsLock;
std::vector<PhotonPassBindingRecord> sPassRecords;

/// The field's sampler: the block HlmsPbs bound it with (its mDecalsSamplerblock —
/// upstream's own `TODO_irradianceField_samplerblock`), built from the same
/// fields so HlmsManager hands back the same pooled block. Taken ONCE per manager
/// (the samplerblock reference count is a uint16 — DOCS/traps/ENGINE.md).
Ogre::HlmsManager *sSamplerMgr = nullptr;
const Ogre::HlmsSamplerblock *sFieldSampler = nullptr;

const Ogre::HlmsSamplerblock *fieldSampler(Ogre::HlmsManager *mgr) {
    if (!mgr) return nullptr;
    if (sSamplerMgr && sSamplerMgr != mgr) PhotonPassBinding::releaseSamplers();
    sSamplerMgr = mgr;
    if (!sFieldSampler) {
        // OgreHlmsPbs.cpp _changeRenderSystem: the block starts as the shadow maps'
        // (a border colour by depth convention) and is then made trilinear + clamp.
        Ogre::HlmsSamplerblock ref;
        const Ogre::RenderSystem *rs = Ogre::Root::getSingleton().getRenderSystem();
        ref.mBorderColour = (rs && rs->isReverseDepth())
                                ? Ogre::ColourValue(0, 0, 0, 0)
                                : Ogre::ColourValue(std::numeric_limits<float>::max(),
                                                    std::numeric_limits<float>::max(),
                                                    std::numeric_limits<float>::max(),
                                                    std::numeric_limits<float>::max());
        ref.mMinFilter = Ogre::FO_LINEAR;
        ref.mMagFilter = Ogre::FO_LINEAR;
        ref.mMipFilter = Ogre::FO_LINEAR;
        ref.mCompareFunction = Ogre::NUM_COMPARE_FUNCTIONS;
        ref.mU = Ogre::TAM_CLAMP;
        ref.mV = Ogre::TAM_CLAMP;
        ref.mW = Ogre::TAM_CLAMP;
        sFieldSampler = mgr->getSamplerblock(ref);
    }
    return sFieldSampler;
}

const SceneGiBinding *bindingOf(const Ogre::SceneManager *sm) {
    return sm ? sceneGiBindingOf(sm) : nullptr;
}

/// The scene pass the always-on listener saw start (render thread only): set in
/// passSceneAfterShadowMaps, which Ogre fires BEFORE the pass's analyzeBarriers, cleared at its end.
const Ogre::CompositorPassDef *sScenePass = nullptr;

struct GiReaderListener final : public Ogre::CompositorWorkspaceListener {
    // A pass that throws between its start and its end never reaches passPosExecute: every pass's
    // start forgets the last one (the stage-2 read's H1), so no definition outlives a chain rebuild.
    void passEarlyPreExecute(Ogre::CompositorPass *) override { sScenePass = nullptr; }
    void passSceneAfterShadowMaps(Ogre::CompositorPassScene *pass) override {
        sScenePass = pass ? pass->getDefinition() : nullptr;
    }
    void passPosExecute(Ogre::CompositorPass *pass) override {
        if (pass && pass->getDefinition() == sScenePass) sScenePass = nullptr;
    }
};
GiReaderListener sReaderListener;
unsigned long long sReaderPrepares = 0ull, sNonReaderPrepares = 0ull;
}   // namespace

unsigned long long PhotonPassBinding::readerPrepares() { return sReaderPrepares; }
unsigned long long PhotonPassBinding::nonReaderPrepares() { return sNonReaderPrepares; }

bool PhotonPassBinding::passReadsGi(const Ogre::CompositorPassDef *def) {
    if (!def || !def->mJahReadsGi || def->mShadowMapIdx != ~Ogre::uint32(0)) return false;
    if (def->getType() == Ogre::PASS_CUSTOM) return true;
    // THE WARM-UP PASS IS ITS SCENE PASS'S STAND-IN (the stage-2 read's D1): it compiles what that
    // pass will draw with, so it reads the GI exactly when that pass does (the fork copies the flag).
    if (def->getType() == Ogre::PASS_WARM_UP)
        return static_cast<const Ogre::CompositorPassWarmUpDef *>(def)->mShadowNodeRecalculation !=
               Ogre::SHADOW_NODE_CASTER_PASS;
    if (def->getType() != Ogre::PASS_SCENE) return false;
    return static_cast<const Ogre::CompositorPassSceneDef *>(def)->mShadowNodeRecalculation !=
           Ogre::SHADOW_NODE_CASTER_PASS;
}

bool PhotonPassBinding::scenePassReadsGi(const Ogre::SceneManager *sceneManager) {
    const Ogre::CompositorPass *current = sceneManager ? sceneManager->getCurrentCompositorPass() : nullptr;
    const Ogre::CompositorPassDef *def = current ? current->getDefinition() : sScenePass;
    return !def || passReadsGi(def);
}

Ogre::PhotonVoxelLighting *PhotonPassBinding::lampChainOf(bool casterPass, const Ogre::SceneManager *sceneManager) {
    if (casterPass || !sceneManager || scenePassReadsGi(sceneManager) || FogHlmsListener::lampRaysOn(sceneManager))
        return nullptr;
    const SceneGiBinding *b = bindingOf(sceneManager);
    if (!b || !b->vct || !b->vct->getVoxelizer()) return nullptr;
    const Ogre::CompositorPass *current = sceneManager->getCurrentCompositorPass();
    const Ogre::CompositorPassDef *def = current ? current->getDefinition() : sScenePass;
    // THE PASSES THAT SHADE THE OPAQUE PICTURE (OgreChain.cpp's L4): the opaque pass and the Atom screen decode
    if (!def || def->getType() != Ogre::PASS_SCENE || def->mShadowMapIdx != ~Ogre::uint32(0) ||
        static_cast<const Ogre::CompositorPassSceneDef *>(def)->mShadowNodeRecalculation == Ogre::SHADOW_NODE_CASTER_PASS)
        return nullptr;
    if (def->mIdentifier != planar::kPlanarUpdatePassIdentifier && def->mIdentifier != kScreenDecodePassIdentifier)
        return nullptr;
    return b->vct;
}

Ogre::PhotonVoxelLighting *PhotonPassBinding::lampVoxelsOf(bool casterPass, const Ogre::SceneManager *sceneManager) {
    if (casterPass || !sceneManager || FogHlmsListener::lampRaysOn(sceneManager)) return nullptr;
    if (Ogre::PhotonVoxelLighting *chain = lampChainOf(casterPass, sceneManager)) return chain;
    const SceneGiBinding *b = bindingOf(sceneManager);
    return b && b->vct && b->vct->getVoxelizer() && scenePassReadsGi(sceneManager) ? b->vct : nullptr;
}

void PhotonPassBinding::installPassListener(Ogre::CompositorManager2 *cm) {
    if (!cm) return;
    // ONCE PER MANAGER (the stage-2 read's wl4: the Hlms registration's retry calls this again).
    for (const Ogre::CompositorWorkspaceListener *l : cm->getPassListeners())
        if (l == &sReaderListener) return;
    cm->addPassListener(&sReaderListener);
}
void PhotonPassBinding::uninstallPassListener(Ogre::CompositorManager2 *cm) {
    if (cm) cm->removePassListener(&sReaderListener);
    sScenePass = nullptr;
}

void PhotonPassBinding::preparePassHash(bool casterPass, Ogre::SceneManager *sceneManager,
                                        Ogre::Hlms *hlms) {
    if (!hlms) return;
    HostBinds &hb = sHost[hlms->getType()];
    hb = HostBinds();
    if (casterPass) return;
    const SceneGiBinding *b = bindingOf(sceneManager);
    if (!b) return;
    if (!scenePassReadsGi(sceneManager)) {   // no GI on a pass that samples none
        if (b->vct || fieldBound(b)) ++sNonReaderPrepares;
        // ...but the opaque passes of a scene that does not trace bind the chain's GEOMETRY for the lamps (L4's one
        // exception: lampChainOf)
        if (Ogre::PhotonVoxelLighting *chain = lampChainOf(casterPass, sceneManager)) {
            hb.lampChain = chain;
            hlms->_setProperty(Ogre::Hlms::kNoTid, kJahLampChain, Ogre::int32(chain->getNumCascades()));
        }
        return;
    }
    if (b->vct || fieldBound(b)) ++sReaderPrepares;
    const size_t tid = Ogre::Hlms::kNoTid;
    if (b->vct) {
        hb.vct = b->vct;
        hlms->_setProperty(tid, kJahVctCascades, Ogre::int32(b->vct->getNumCascades()));
        // ONE LIGHT STORE AT EVERY TIER (PHOTON-MARCH-1): the directional volumes are always
        // bound. The property stays for upstream's HlmsPbs, whose root layout declares the
        // three axis arrays beside `vctProbes` from it (OgreHlmsPbs.cpp setupRootLayout);
        // none of our pieces reads it.
        hlms->_setProperty(tid, kVctAnisotropic, 1);
        hlms->_setProperty(tid, kVctSdfQuality, b->vct->shouldEnableSpecularSdfQuality());
        // 'Static' reflections on cubemaps look horrible (upstream's own words).
        if (b->pcc && b->pcc->isRendering()) hlms->_setProperty(tid, kVctDisableSpecular, 1);
    }
    if (fieldBound(b)) {
        // The bind needs the field's sampler; a registered Hlms always has a manager,
        // and the manager always hands back a block.
        fieldSampler(hlms->getHlmsManager());
        hb.ifd = b->ifd;
        hlms->_setProperty(tid, kJahIfd, 1);
        // (vct_disable_diffuse is never set: a bound field does not disable the chain's diffuse at any cascade count -
        // where its confidence fades, the chain's rays answer, never the unoccluded environment: PHOTON-7 10-3 R3)
    }
}

Ogre::uint16 PhotonPassBinding::numExtraPassTextures(const Ogre::HlmsPropertyVec &properties,
                                                     bool casterPass) {
    if (casterPass) return 0u;
    const Ogre::int32 cascades = Ogre::Hlms::getProperty(properties, kJahVctCascades);
    const bool ifd = Ogre::Hlms::getProperty(properties, kJahIfd) != 0;
    const Ogre::int32 lamp = Ogre::Hlms::getProperty(properties, kJahLampChain);
    return Ogre::uint16(cascades * kVolumesPerCascade + (ifd ? kFieldTextures : 0) + lamp * kLampChainVolumes);
}

void PhotonPassBinding::propertiesMerged(Ogre::Hlms *hlms, size_t tid, Ogre::int32 endSlot) {
    if (hlms->_getProperty(tid, kShadowCaster)) return;
    const Ogre::int32 cascades = hlms->_getProperty(tid, kJahVctCascades);
    const bool ifd = hlms->_getProperty(tid, kJahIfd) != 0;
    const Ogre::int32 lamp = hlms->_getProperty(tid, kJahLampChain);
    if (lamp > 0) {
        // THE LAMP CHAIN (lampChainOf): its four arrays last, in bind()'s order - after the volumes and the field
        Ogre::int32 lampSlot = endSlot - lamp * kLampChainVolumes;
        for (Ogre::int32 k = 0; k < kLampChainVolumes; ++k) {
            hlms->_setTextureReg(tid, Ogre::PixelShader, kLampChainRegisters[k], lampSlot);
            lampSlot += lamp;
        }
        endSlot -= lamp * kLampChainVolumes;
    }
    if (cascades <= 0 && !ifd) return;
    // The same count numExtraPassTextures gave HlmsPbs for this property set.
    Ogre::int32 slot =
        endSlot - (cascades > 0 ? cascades * kVolumesPerCascade : 0) -
        (ifd ? kFieldTextures : 0);
    if (cascades > 0) {
        hlms->_setProperty(tid, kVctNumProbes, cascades);
        // HlmsPbs: "If decals normals are enabled or VCT is used, we need to
        // generate the TBN matrix."
        const bool normalMapCanBeSupported =
            (hlms->_getProperty(tid, kNormal) && hlms->_getProperty(tid, kTangent)) ||
            hlms->_getProperty(tid, kQTangent);
        hlms->_setProperty(tid, kNormalMap, normalMapCanBeSupported);
        // The volumes' registers, in the order the voxel lighting lists them (kVolumeRegisters).
        for (Ogre::int32 i = 0; i < kVolumesPerCascade; ++i) {
            hlms->_setTextureReg(tid, Ogre::PixelShader, kVolumeRegisters[i], slot);
            slot += cascades;
        }
    }
    if (ifd) {
        hlms->_setProperty(tid, kIrradianceField, 1);
        hlms->_setTextureReg(tid, Ogre::PixelShader, "ifdColour", slot++);
        hlms->_setTextureReg(tid, Ogre::PixelShader, "ifdDepth", slot++);
    }
    // HlmsPbs raises these for a bound volume or field; the pass's spherical-harmonics
    // ambient already has (OgreEngine registers PBS with AmbientSh), so this states
    // the dependency rather than relying on it.
    hlms->_setProperty(tid, kNeedsReflDir, 1);
    hlms->_setProperty(tid, kNeedsEnvBrdf, 1);
    hlms->_setProperty(tid, kNeedsViewDir, 1);
}

void PhotonPassBinding::setupRootLayout(Ogre::RootLayout &rootLayout,
                                        const Ogre::HlmsPropertyVec &properties) {
    // HlmsPbs::setupRootLayout adds the total's array and the three axes (upstream's
    // own volumes, vct_anisotropic always 1) whenever `vct_num_probes` > 1; the rest
    // are ours.
    // THE LAMP CHAIN's four arrays (lampChainOf), one binding of its cascades each
    const Ogre::int32 lamp = Ogre::Hlms::getProperty(properties, kJahLampChain);
    if (lamp > 1) {
        Ogre::int32 at = Ogre::Hlms::getProperty(properties, Ogre::IdString(kLampChainRegisters[0]));
        for (Ogre::int32 k = 0; k < kLampChainVolumes; ++k) {
            rootLayout.addArrayBinding(Ogre::DescBindingTypes::Texture,
                                       Ogre::RootLayout::ArrayDesc(Ogre::uint16(at), Ogre::uint16(lamp)));
            at += lamp;
        }
    }
    const Ogre::int32 cascades = Ogre::Hlms::getProperty(properties, kVctNumProbes);
    if (cascades <= 1) return;
    Ogre::int32 idx = Ogre::Hlms::getProperty(properties, Ogre::IdString("vctProbes"));
    idx += cascades * 4;
    const Ogre::int32 ours = kVolumesPerCascade - 4;
    for (Ogre::int32 i = 0; i < ours; ++i) {
        rootLayout.addArrayBinding(Ogre::DescBindingTypes::Texture,
                                   Ogre::RootLayout::ArrayDesc(Ogre::uint16(idx), Ogre::uint16(cascades)));
        idx += cascades;
    }
}

Ogre::uint32 PhotonPassBinding::passBufferSize(bool casterPass, Ogre::SceneManager *sceneManager) {
    if (casterPass || !scenePassReadsGi(sceneManager)) return 0u;
    const SceneGiBinding *b = bindingOf(sceneManager);
    if (!b) return 0u;
    size_t bytes = 0u;
    if (b->vct) bytes += b->vct->getConstBufferSize();
    if (fieldBound(b)) bytes += b->ifd->getConstBufferSize();
    return Ogre::uint32(bytes);
}

float *PhotonPassBinding::preparePassBuffer(bool casterPass, Ogre::SceneManager *sceneManager,
                                            float *passBufferPtr) {
    if (casterPass || !scenePassReadsGi(sceneManager)) return passBufferPtr;
    const SceneGiBinding *b = bindingOf(sceneManager);
    if (!b || (!b->vct && !fieldBound(b))) return passBufferPtr;
    // The view matrix HlmsPbs::preparePassHash fills its own blocks with.
    const Ogre::Camera *cam = sceneManager->getCamerasInProgress().renderingCamera;
    const Ogre::Matrix4 viewMatrix = cam ? cam->getVrViewMatrix(0) : Ogre::Matrix4::IDENTITY;
    if (b->vct) {
        b->vct->fillConstBufferData(viewMatrix, passBufferPtr);
        passBufferPtr += b->vct->getConstBufferSize() >> 2u;
    }
    if (fieldBound(b)) {
        b->ifd->fillConstBufferData(viewMatrix, passBufferPtr);
        passBufferPtr += b->ifd->getConstBufferSize() >> 2u;
    }
    return passBufferPtr;
}

size_t PhotonPassBinding::bind(bool casterPass, Ogre::CommandBuffer *commandBuffer,
                               const Ogre::HlmsDatablock *datablock, size_t texUnit) {
    if (casterPass || !commandBuffer || !datablock || !datablock->getCreator()) return texUnit;
    const HostBinds &hb = sHost[datablock->getCreator()->getType()];
    if (hb.vct) {
        Ogre::PhotonVoxelLighting *vct = hb.vct;
        const size_t numCascades = vct->getNumCascades();
        const size_t numVolumes = vct->getNumVoxelTextures();
        const Ogre::HlmsSamplerblock *samplerblock = vct->getBindTrilinearSamplerblock();
        for (size_t i = 0u; i < numVolumes; ++i) {
            for (size_t c = 0u; c < numCascades; ++c) {
                Ogre::TextureGpu **volumes = vct->getLightVoxelTextures(c);
                *commandBuffer->addCommand<Ogre::CbTexture>() =
                    Ogre::CbTexture(Ogre::uint16(texUnit), volumes[i], samplerblock);
                ++texUnit;
            }
        }
    }
    if (hb.ifd) {
        *commandBuffer->addCommand<Ogre::CbTexture>() =
            Ogre::CbTexture(Ogre::uint16(texUnit++), hb.ifd->getIrradianceTex(), sFieldSampler);
        *commandBuffer->addCommand<Ogre::CbTexture>() =
            Ogre::CbTexture(Ogre::uint16(texUnit++), hb.ifd->getDepthVarianceTex(), sFieldSampler);
    }
    if (hb.lampChain) {
        // THE LAMP CHAIN (lampChainOf): its coverage and position per half, each an array of its cascades; read by
        // texelFetch (no sampler)
        Ogre::PhotonVoxelLighting *chain = hb.lampChain;
        const size_t numCascades = chain->getNumCascades();
        for (Ogre::int32 k = 0; k < kLampChainVolumes; ++k)
            for (size_t c = 0u; c < numCascades; ++c) {
                Ogre::TextureGpu **volumes = chain->getLightVoxelTextures(c);
                *commandBuffer->addCommand<Ogre::CbTexture>() =
                    Ogre::CbTexture(Ogre::uint16(texUnit++), volumes[kLampChainFirstVolume + k], nullptr);
            }
    }
    return texUnit;
}

void PhotonPassBinding::analyzeBarriers(Ogre::BarrierSolver &barrierSolver,
                                        Ogre::ResourceTransitionArray &resourceTransitions,
                                        const Ogre::SceneManager *sceneManager, bool casterPass) {
    if (casterPass) return;
    if (Ogre::PhotonVoxelLighting *chain = lampChainOf(casterPass, sceneManager)) {
        // THE LAMP CHAIN's four volumes (lampChainOf), read by the opaque pass's pixel shader
        for (size_t c = 0u; c < chain->getNumCascades(); ++c) {
            Ogre::TextureGpu **volumes = chain->getLightVoxelTextures(c);
            for (Ogre::int32 k = 0; k < kLampChainVolumes; ++k)
                barrierSolver.resolveTransition(resourceTransitions, volumes[kLampChainFirstVolume + k],
                                                Ogre::ResourceLayout::Texture, Ogre::ResourceAccess::Read,
                                                1u << Ogre::PixelShader);
        }
        return;
    }
    if (!scenePassReadsGi(sceneManager)) return;
    const SceneGiBinding *b = bindingOf(sceneManager);
    if (!b) return;
    if (Ogre::PhotonVoxelLighting *vct = b->vct) {
        const size_t numCascades = vct->getNumCascades();
        const size_t numVolumes = vct->getNumVoxelTextures();
        for (size_t c = 0u; c < numCascades; ++c) {
            Ogre::TextureGpu **volumes = vct->getLightVoxelTextures(c);
            for (size_t i = 0u; i < numVolumes; ++i)
                barrierSolver.resolveTransition(resourceTransitions, volumes[i], Ogre::ResourceLayout::Texture,
                                                Ogre::ResourceAccess::Read, 1u << Ogre::PixelShader);
        }
    }
    if (fieldBound(b)) {
        Ogre::PhotonIrradianceField *ifd = b->ifd;
        barrierSolver.resolveTransition(resourceTransitions, ifd->getIrradianceTex(),
                                        Ogre::ResourceLayout::Texture, Ogre::ResourceAccess::Read,
                                        1u << Ogre::PixelShader);
        barrierSolver.resolveTransition(resourceTransitions, ifd->getDepthVarianceTex(),
                                        Ogre::ResourceLayout::Texture, Ogre::ResourceAccess::Read,
                                        1u << Ogre::PixelShader);
    }
}

void PhotonPassBinding::forget(const void *volume) {
    if (!volume) return;
    for (HostBinds &hb : sHost) {
        if (hb.vct == volume) hb.vct = nullptr;
        if (hb.ifd == volume) hb.ifd = nullptr;
        if (hb.lampChain == volume) hb.lampChain = nullptr;
    }
}

void PhotonPassBinding::notePass(const Ogre::SceneManager *sceneManager, const Ogre::Hlms *hlms,
                                 const char *const *screen, size_t numScreen) {
    if (!sceneManager || !hlms) return;
    const Ogre::CompositorPass *current = sceneManager->getCurrentCompositorPass();
    const Ogre::CompositorPassDef *def = current ? current->getDefinition() : nullptr;
    if (!def || def->mProfilingId.empty()) return;   // only the chain's named passes
    const HostBinds &hb = sHost[hlms->getType()];
    const bool atom = hlms->getType() != Ogre::HLMS_PBS;
    std::vector<std::string> bound;
    if (hb.vct) {
        const std::string count = " x" + std::to_string(hb.vct->getNumCascades());
        for (const char *reg : kVolumeRegisters) bound.push_back(reg + count);
    }
    if (hb.ifd) {
        bound.push_back("ifdColour");
        bound.push_back("ifdDepth");
    }
    for (size_t i = 0; i < numScreen; ++i) bound.push_back(screen[i]);
    const unsigned long long frame = Ogre::Root::getSingleton().getNextFrameNumber();
    std::lock_guard<std::mutex> lock(sPassRecordsLock);
    for (PhotonPassBindingRecord &r : sPassRecords)
        if (r.pass == def->mProfilingId && (r.host == "HlmsAtom") == atom) {
            r.frame = frame;
            r.bound.swap(bound);
            return;
        }
    PhotonPassBindingRecord r;
    r.pass = def->mProfilingId;
    r.host = atom ? "HlmsAtom" : "HlmsPbs";
    r.frame = frame;
    r.bound.swap(bound);
    sPassRecords.push_back(std::move(r));
}

std::vector<PhotonPassBindingRecord> PhotonPassBinding::passRecords() {
    std::lock_guard<std::mutex> lock(sPassRecordsLock);
    return sPassRecords;
}

void PhotonPassBinding::resetPassRecords() {
    std::lock_guard<std::mutex> lock(sPassRecordsLock);
    sPassRecords.clear();
}

void PhotonPassBinding::releaseSamplers() {
    if (sSamplerMgr && sFieldSampler) sSamplerMgr->destroySamplerblock(sFieldSampler);
    sSamplerMgr = nullptr;
    sFieldSampler = nullptr;
    for (HostBinds &hb : sHost) hb = HostBinds();
}

}}}   // namespace jahshaka::engine::detail
