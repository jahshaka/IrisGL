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

#include <CommandBuffer/OgreCbTexture.h>
#include <CommandBuffer/OgreCommandBuffer.h>
#include <Cubemaps/OgreParallaxCorrectedCubemapBase.h>
#include <OgreCamera.h>
#include <OgreHlms.h>
#include <OgreHlmsManager.h>
#include <OgreHlmsPbs.h>
#include <OgreRenderSystem.h>
#include <OgreRoot.h>
#include <OgreRootLayout.h>
#include <OgreSceneManager.h>

#include <limits>

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
const Ogre::IdString kVctConeDirs("vct_cone_dirs");
const Ogre::IdString kVctAnisotropic("vct_anisotropic");
const Ogre::IdString kVctSdfQuality("vct_enable_specular_sdf_quality");
const Ogre::IdString kVctDisableSpecular("vct_disable_specular");
const Ogre::IdString kVctDisableDiffuse("vct_disable_diffuse");
const Ogre::IdString kIrradianceField("irradiance_field");
const Ogre::IdString kNormalMap("normal_map");
const Ogre::IdString kNeedsReflDir("needs_refl_dir");
const Ogre::IdString kNeedsEnvBrdf("needs_env_brdf");
const Ogre::IdString kNeedsViewDir("needs_view_dir");
const Ogre::IdString kNormal("hlms_normal");
const Ogre::IdString kTangent("hlms_tangent");
const Ogre::IdString kQTangent("hlms_qtangent");
const Ogre::IdString kShadowCaster("hlms_shadowcaster");

/// The light volumes per cascade, in the order the voxel lighting lists them
/// (PhotonVoxelLighting::getLightVoxelTextures) and the pieces declare them.
Ogre::int32 volumesPerCascade(bool anisotropic) { return anisotropic ? 10 : 5; }

/// What one host binds in the pass it last prepared. One per Hlms type, like
/// FogHlmsListener::PassBinds: RenderQueue::renderPassPrepare prepares EVERY
/// registered Hlms of a pass in turn, so a shared copy would be overwritten by
/// whichever host ran last.
struct HostBinds {
    Ogre::PhotonVoxelLighting *vct = nullptr;
    Ogre::PhotonIrradianceField *ifd = nullptr;
};
HostBinds sHost[Ogre::HLMS_MAX];   // render thread only

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
}   // namespace

void PhotonPassBinding::preparePassHash(bool casterPass, Ogre::SceneManager *sceneManager,
                                        Ogre::Hlms *hlms) {
    if (!hlms) return;
    HostBinds &hb = sHost[hlms->getType()];
    hb = HostBinds();
    if (casterPass) return;
    const SceneGiBinding *b = bindingOf(sceneManager);
    if (!b) return;
    const size_t tid = Ogre::Hlms::kNoTid;
    if (b->vct) {
        hb.vct = b->vct;
        hlms->_setProperty(tid, kJahVctCascades, Ogre::int32(b->vct->getNumCascades()));
        // The cone count is HLMS_PBS's setting (setVctFullConeCount, upstream API)
        // for every PBS-family host: PBS is where the engine tells it.
        const auto *pbs =
            dynamic_cast<const Ogre::HlmsPbs *>(hlms->getHlmsManager()->getHlms(Ogre::HLMS_PBS));
        hlms->_setProperty(tid, kVctConeDirs, (pbs && pbs->getVctFullConeCount()) ? 6 : 4);
        hlms->_setProperty(tid, kVctAnisotropic, b->vct->isAnisotropic());
        hlms->_setProperty(tid, kVctSdfQuality, b->vct->shouldEnableSpecularSdfQuality());
        // 'Static' reflections on cubemaps look horrible (upstream's own words).
        if (b->pcc && b->pcc->isRendering()) hlms->_setProperty(tid, kVctDisableSpecular, 1);
    }
    if (b->ifd) {
        const Ogre::HlmsSamplerblock *sampler = fieldSampler(hlms->getHlmsManager());
        if (sampler) {
            hb.ifd = b->ifd;
            hlms->_setProperty(tid, kJahIfd, 1);
            hlms->_setProperty(tid, kVctDisableDiffuse, 1);
        }
    }
}

Ogre::uint16 PhotonPassBinding::numExtraPassTextures(const Ogre::HlmsPropertyVec &properties,
                                                     bool casterPass) {
    if (casterPass) return 0u;
    const Ogre::int32 cascades = Ogre::Hlms::getProperty(properties, kJahVctCascades);
    const bool aniso = Ogre::Hlms::getProperty(properties, kVctAnisotropic) != 0;
    const bool ifd = Ogre::Hlms::getProperty(properties, kJahIfd) != 0;
    return Ogre::uint16(cascades * volumesPerCascade(aniso) + (ifd ? 2 : 0));
}

void PhotonPassBinding::propertiesMerged(Ogre::Hlms *hlms, size_t tid, Ogre::int32 endSlot) {
    if (hlms->_getProperty(tid, kShadowCaster)) return;
    const Ogre::int32 cascades = hlms->_getProperty(tid, kJahVctCascades);
    const bool ifd = hlms->_getProperty(tid, kJahIfd) != 0;
    if (cascades <= 0 && !ifd) return;
    // The same count numExtraPassTextures gave HlmsPbs for this property set.
    Ogre::int32 slot =
        endSlot - (cascades > 0 ? cascades * volumesPerCascade(hlms->_getProperty(tid, kVctAnisotropic) != 0)
                                : 0) -
        (ifd ? 2 : 0);
    if (cascades > 0) {
        hlms->_setProperty(tid, kVctNumProbes, cascades);
        // HlmsPbs: "If decals normals are enabled or VCT is used, we need to
        // generate the TBN matrix."
        const bool normalMapCanBeSupported =
            (hlms->_getProperty(tid, kNormal) && hlms->_getProperty(tid, kTangent)) ||
            hlms->_getProperty(tid, kQTangent);
        hlms->_setProperty(tid, kNormalMap, normalMapCanBeSupported);
        // The volumes' registers, in the order the voxel lighting lists them: the
        // isotropic total, the three anisotropic axes, the coverage per half-axis,
        // the surface position per half, then level 0's back side and the
        // voxeliser's normal on the anisotropic tiers.
        const bool aniso = hlms->_getProperty(tid, kVctAnisotropic) != 0;
        static const char *const kIso[] = { "vctProbes", "vctProbeCovP", "vctProbeCovN",
                                            "vctProbePosP", "vctProbePosN" };
        static const char *const kAniso[] = { "vctProbes",    "vctProbeX",    "vctProbeY",
                                              "vctProbeZ",    "vctProbeCovP", "vctProbeCovN",
                                              "vctProbePosP", "vctProbePosN", "vctProbeBack",
                                              "vctProbeNrm" };
        const char *const *names = aniso ? kAniso : kIso;
        for (Ogre::int32 i = 0; i < volumesPerCascade(aniso); ++i) {
            hlms->_setTextureReg(tid, Ogre::PixelShader, names[i], slot);
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
    // HlmsPbs::setupRootLayout adds the isotropic array and, anisotropic, the three
    // axes (upstream's own volumes) whenever `vct_num_probes` > 1; the rest are ours.
    const Ogre::int32 cascades = Ogre::Hlms::getProperty(properties, kVctNumProbes);
    if (cascades <= 1) return;
    const bool aniso = Ogre::Hlms::getProperty(properties, kVctAnisotropic) != 0;
    Ogre::int32 idx = Ogre::Hlms::getProperty(properties, Ogre::IdString("vctProbes"));
    idx += cascades * (aniso ? 4 : 1);
    const Ogre::int32 ours = volumesPerCascade(aniso) - (aniso ? 4 : 1);
    for (Ogre::int32 i = 0; i < ours; ++i) {
        rootLayout.addArrayBinding(Ogre::DescBindingTypes::Texture,
                                   Ogre::RootLayout::ArrayDesc(Ogre::uint16(idx), Ogre::uint16(cascades)));
        idx += cascades;
    }
}

Ogre::uint32 PhotonPassBinding::passBufferSize(bool casterPass, Ogre::SceneManager *sceneManager) {
    if (casterPass) return 0u;
    const SceneGiBinding *b = bindingOf(sceneManager);
    if (!b) return 0u;
    size_t bytes = 0u;
    if (b->vct) bytes += b->vct->getConstBufferSize();
    if (b->ifd) bytes += b->ifd->getConstBufferSize();
    return Ogre::uint32(bytes);
}

float *PhotonPassBinding::preparePassBuffer(bool casterPass, Ogre::SceneManager *sceneManager,
                                            float *passBufferPtr) {
    if (casterPass) return passBufferPtr;
    const SceneGiBinding *b = bindingOf(sceneManager);
    if (!b || (!b->vct && !b->ifd)) return passBufferPtr;
    // The view matrix HlmsPbs::preparePassHash fills its own blocks with.
    const Ogre::Camera *cam = sceneManager->getCamerasInProgress().renderingCamera;
    const Ogre::Matrix4 viewMatrix = cam ? cam->getVrViewMatrix(0) : Ogre::Matrix4::IDENTITY;
    if (b->vct) {
        b->vct->fillConstBufferData(viewMatrix, passBufferPtr);
        passBufferPtr += b->vct->getConstBufferSize() >> 2u;
    }
    if (b->ifd) {
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
    return texUnit;
}

void PhotonPassBinding::analyzeBarriers(Ogre::BarrierSolver &barrierSolver,
                                        Ogre::ResourceTransitionArray &resourceTransitions,
                                        const Ogre::SceneManager *sceneManager, bool casterPass) {
    if (casterPass) return;
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
    if (Ogre::PhotonIrradianceField *ifd = b->ifd) {
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
    }
}

void PhotonPassBinding::releaseSamplers() {
    if (sSamplerMgr && sFieldSampler) sSamplerMgr->destroySamplerblock(sFieldSampler);
    sSamplerMgr = nullptr;
    sFieldSampler = nullptr;
    for (HostBinds &hb : sHost) hb = HostBinds();
}

}}}   // namespace jahshaka::engine::detail
