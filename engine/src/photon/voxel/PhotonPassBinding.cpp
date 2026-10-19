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
#include <OgreLogManager.h>
#include <OgreRenderSystem.h>
#include <OgreRoot.h>
#include <OgreRootLayout.h>
#include <OgreSceneManager.h>

#include <OgreHlmsSamplerblock.h>
#include <OgrePixelFormatGpuUtils.h>
#include <OgreStagingTexture.h>
#include <OgreTextureBox.h>
#include <OgreTextureGpuManager.h>

#include <cstring>
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
    Ogre::int32 standInCascades = 0;   ///< the configured arm's stand-in (GiPassShape), when `vct` is not built
    bool standInField = false;         ///< ...and the field's, when `ifd` is not
};

// THE STAND-INS (GiPassShape): a zero 1x1x1 volume for every volume slot of every cascade, a zero 1x1 texel for the
// field's two (irradiance alpha 0: the cage adds nothing), the volumes' trilinear clamp sampler. Render thread only.
Ogre::TextureGpuManager *sStandInMgr = nullptr;
Ogre::TextureGpu *sStandInVolume = nullptr;
Ogre::TextureGpu *sStandInField = nullptr;
Ogre::HlmsManager *sStandInSamplerMgr = nullptr;
const Ogre::HlmsSamplerblock *sStandInSampler = nullptr;

/// What the pass binds of `b`: the built arm and field where they exist, else the configured shape's stand-ins.
struct PassGi {
    Ogre::PhotonVoxelLighting *vct = nullptr;
    Ogre::PhotonIrradianceField *ifd = nullptr;
    Ogre::int32 cascades = 0;   ///< the cascades the pass declares (the built arm's or the shape's)
    bool field = false;
};
PassGi passGiOf(const SceneGiBinding *b) {
    PassGi g;
    if (!b) return g;
    const bool standIns = sStandInVolume && sStandInField && sStandInSampler;
    if (b->vct) {
        g.vct = b->vct;
        g.cascades = Ogre::int32(b->vct->getNumCascades());
    } else if (standIns) {
        g.cascades = b->shape.cascades;
    }
    if (b->ifd) {
        g.ifd = b->ifd;
        g.field = true;
    } else if (standIns && b->shape.field) {
        g.field = true;
    }
    return g;
}
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
    const PassGi g = passGiOf(b);
    if (!scenePassReadsGi(sceneManager)) {   // no GI on a pass that samples none
        if (g.cascades > 0 || g.field) ++sNonReaderPrepares;
        return;
    }
    if (g.cascades > 0 || g.field) ++sReaderPrepares;
    // THE BUILT ARM AGREES WITH THE SHAPE IT WAS CONFIGURED WITH, or its arrival is a pass change (said once).
    if (b->vct && b->shape.cascades > 0 &&
        (Ogre::int32(b->vct->getNumCascades()) != b->shape.cascades ||
         b->vct->shouldEnableSpecularSdfQuality() != b->shape.sdfQuality)) {
        static bool said = false;
        if (!said) {
            said = true;
            Ogre::LogManager::getSingleton().logMessage(
                "Jahshaka GI: the built arm's pass shape differs from the configured one (" +
                    std::to_string(b->vct->getNumCascades()) + " cascades against " +
                    std::to_string(b->shape.cascades) + ") - its arrival changed the pass's permutations",
                Ogre::LML_CRITICAL);
        }
    }
    {   // THE PREPARED SHAPE (GiStatus::passShape*): what this reader pass declares, and whether it moved
        GiPassShape now;
        now.cascades = g.cascades;
        now.field = g.field;
        now.sdfQuality = g.cascades > 0 && (g.vct ? g.vct->shouldEnableSpecularSdfQuality() : b->shape.sdfQuality);
        if (b->preparedOnce && now != b->prepared) ++b->preparedChanges;
        b->prepared = now;
        b->preparedOnce = true;
        b->preparedStandIn = (g.cascades > 0 && !g.vct) || (g.field && !g.ifd);
    }
    const size_t tid = Ogre::Hlms::kNoTid;
    if (g.cascades > 0) {
        hb.vct = g.vct;
        hb.standInCascades = g.vct ? 0 : g.cascades;
        hlms->_setProperty(tid, kJahVctCascades, g.cascades);
        // ONE LIGHT STORE AT EVERY TIER (PHOTON-MARCH-1): the directional volumes are always
        // bound. The property stays for upstream's HlmsPbs, whose root layout declares the
        // three axis arrays beside `vctProbes` from it (OgreHlmsPbs.cpp setupRootLayout);
        // none of our pieces reads it.
        hlms->_setProperty(tid, kVctAnisotropic, 1);
        hlms->_setProperty(tid, kVctSdfQuality,
                           g.vct ? g.vct->shouldEnableSpecularSdfQuality() : b->shape.sdfQuality);
        // 'Static' reflections on cubemaps look horrible (upstream's own words).
        if (b->pcc && b->pcc->isRendering()) hlms->_setProperty(tid, kVctDisableSpecular, 1);
    }
    if (g.field) {
        // The bind needs the field's sampler; a registered Hlms always has a manager,
        // and the manager always hands back a block.
        fieldSampler(hlms->getHlmsManager());
        hb.ifd = g.ifd;
        hb.standInField = !g.ifd;
        hlms->_setProperty(tid, kJahIfd, 1);
        hlms->_setProperty(tid, kVctDisableDiffuse, 1);
    }
}

Ogre::uint16 PhotonPassBinding::numExtraPassTextures(const Ogre::HlmsPropertyVec &properties,
                                                     bool casterPass) {
    if (casterPass) return 0u;
    const Ogre::int32 cascades = Ogre::Hlms::getProperty(properties, kJahVctCascades);
    const bool ifd = Ogre::Hlms::getProperty(properties, kJahIfd) != 0;
    return Ogre::uint16(cascades * kVolumesPerCascade + (ifd ? kFieldTextures : 0));
}

void PhotonPassBinding::propertiesMerged(Ogre::Hlms *hlms, size_t tid, Ogre::int32 endSlot) {
    if (hlms->_getProperty(tid, kShadowCaster)) return;
    const Ogre::int32 cascades = hlms->_getProperty(tid, kJahVctCascades);
    const bool ifd = hlms->_getProperty(tid, kJahIfd) != 0;
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
        // The volumes' registers, in the order the voxel lighting lists them: the
        // total (level 0), the three directional axes, the coverage per half-axis,
        // the surface position per half, then level 0's back side and the
        // voxeliser's normal.
        static const char *const names[] = { "vctProbes",    "vctProbeX",    "vctProbeY",
                                             "vctProbeZ",    "vctProbeCovP", "vctProbeCovN",
                                             "vctProbePosP", "vctProbePosN", "vctProbeBack",
                                             "vctProbeNrm" };
        static_assert(sizeof(names) / sizeof(names[0]) == size_t(kVolumesPerCascade),
                      "one register name per light volume the voxel lighting lists");
        for (Ogre::int32 i = 0; i < kVolumesPerCascade; ++i) {
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
    // HlmsPbs::setupRootLayout adds the total's array and the three axes (upstream's
    // own volumes, vct_anisotropic always 1) whenever `vct_num_probes` > 1; the rest
    // are ours.
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
    const PassGi g = passGiOf(bindingOf(sceneManager));
    size_t bytes = 0u;
    if (g.cascades > 0)
        bytes += g.vct ? g.vct->getConstBufferSize() : Ogre::PhotonVoxelLighting::constBufferSizeFor(size_t(g.cascades));
    if (g.field) bytes += g.ifd ? g.ifd->getConstBufferSize() : Ogre::PhotonIrradianceField::constBufferSizeOf();
    return Ogre::uint32(bytes);
}

float *PhotonPassBinding::preparePassBuffer(bool casterPass, Ogre::SceneManager *sceneManager,
                                            float *passBufferPtr) {
    if (casterPass || !scenePassReadsGi(sceneManager)) return passBufferPtr;
    const PassGi g = passGiOf(bindingOf(sceneManager));
    if (g.cascades <= 0 && !g.field) return passBufferPtr;
    // The view matrix HlmsPbs::preparePassHash fills its own blocks with.
    const Ogre::Camera *cam = sceneManager->getCamerasInProgress().renderingCamera;
    const Ogre::Matrix4 viewMatrix = cam ? cam->getVrViewMatrix(0) : Ogre::Matrix4::IDENTITY;
    if (g.cascades > 0) {
        if (g.vct) {
            g.vct->fillConstBufferData(viewMatrix, passBufferPtr);
            passBufferPtr += g.vct->getConstBufferSize() >> 2u;
        } else {
            Ogre::PhotonVoxelLighting::fillEmptyConstBufferData(size_t(g.cascades), viewMatrix, passBufferPtr);
            passBufferPtr += Ogre::PhotonVoxelLighting::constBufferSizeFor(size_t(g.cascades)) >> 2u;
        }
    }
    if (g.field) {
        if (g.ifd) {
            g.ifd->fillConstBufferData(viewMatrix, passBufferPtr);
            passBufferPtr += g.ifd->getConstBufferSize() >> 2u;
        } else {
            Ogre::PhotonIrradianceField::fillEmptyConstBufferData(viewMatrix, passBufferPtr);
            passBufferPtr += Ogre::PhotonIrradianceField::constBufferSizeOf() >> 2u;
        }
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
    else if (hb.standInCascades > 0) {
        const size_t slots = size_t(hb.standInCascades) * size_t(kVolumesPerCascade);
        for (size_t i = 0u; i < slots; ++i)
            *commandBuffer->addCommand<Ogre::CbTexture>() =
                Ogre::CbTexture(Ogre::uint16(texUnit++), sStandInVolume, sStandInSampler);
    }
    if (hb.ifd) {
        *commandBuffer->addCommand<Ogre::CbTexture>() =
            Ogre::CbTexture(Ogre::uint16(texUnit++), hb.ifd->getIrradianceTex(), sFieldSampler);
        *commandBuffer->addCommand<Ogre::CbTexture>() =
            Ogre::CbTexture(Ogre::uint16(texUnit++), hb.ifd->getDepthVarianceTex(), sFieldSampler);
    } else if (hb.standInField) {
        for (int i = 0; i < 2; ++i)
            *commandBuffer->addCommand<Ogre::CbTexture>() =
                Ogre::CbTexture(Ogre::uint16(texUnit++), sStandInField, sFieldSampler);
    }
    return texUnit;
}

void PhotonPassBinding::analyzeBarriers(Ogre::BarrierSolver &barrierSolver,
                                        Ogre::ResourceTransitionArray &resourceTransitions,
                                        const Ogre::SceneManager *sceneManager, bool casterPass) {
    if (casterPass || !scenePassReadsGi(sceneManager)) return;
    const SceneGiBinding *b = bindingOf(sceneManager);
    if (!b) return;
    const PassGi g = passGiOf(b);
    if ((g.cascades > 0 && !g.vct) || (g.field && !g.ifd))   // the stand-ins: written once, read here
        for (Ogre::TextureGpu *t : { sStandInVolume, sStandInField })
            barrierSolver.resolveTransition(resourceTransitions, t, Ogre::ResourceLayout::Texture,
                                            Ogre::ResourceAccess::Read, 1u << Ogre::PixelShader);
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
    }
}

void PhotonPassBinding::ensureStandIns(Ogre::Root *root) {
    Ogre::RenderSystem *rs = root ? root->getRenderSystem() : nullptr;
    Ogre::TextureGpuManager *tm = rs ? rs->getTextureGpuManager() : nullptr;
    if (!tm || !root->getHlmsManager()) return;
    if (sStandInMgr && sStandInMgr != tm) {   // a new device: the old one gave its textures back at its teardown
        sStandInVolume = sStandInField = nullptr;
        sStandInSampler = nullptr;
        sStandInSamplerMgr = nullptr;
    }
    sStandInMgr = tm;
    // MADE AND FILLED NOW (between frames, from the drain), never inside a pass: a ManualTexture made resident at
    // once, one staging upload of zeros, and NO notifyDataIsReady (the residency transition is the readiness).
    const auto make = [&](const char *name, Ogre::TextureTypes::TextureTypes type) {
        Ogre::TextureGpu *t = tm->createTexture(name, Ogre::GpuPageOutStrategy::Discard,
                                                Ogre::TextureFlags::ManualTexture, type);
        t->setResolution(1u, 1u, 1u);
        t->setNumMipmaps(1u);
        t->setPixelFormat(Ogre::PFG_RGBA16_FLOAT);
        t->_transitionTo(Ogre::GpuResidency::Resident, nullptr);
        Ogre::StagingTexture *staging = tm->getStagingTexture(1u, 1u, 1u, 1u, Ogre::PFG_RGBA16_FLOAT);
        staging->startMapRegion();
        Ogre::TextureBox box = staging->mapRegion(1u, 1u, 1u, 1u, Ogre::PFG_RGBA16_FLOAT);
        std::memset(box.at(0, 0, 0), 0, 8u);
        staging->stopMapRegion();
        staging->upload(box, t, 0u, nullptr, nullptr, true);
        tm->removeStagingTexture(staging);
        return t;
    };
    if (!sStandInVolume) sStandInVolume = make("JahGiStandInVolume", Ogre::TextureTypes::Type3D);
    if (!sStandInField) sStandInField = make("JahGiStandInField", Ogre::TextureTypes::Type2D);
    if (!sStandInSampler) {
        // The voxel lighting's own bind block (trilinear, clamp), from the same manager every pass uses.
        Ogre::HlmsSamplerblock ref;
        ref.mMinFilter = Ogre::FO_LINEAR;
        ref.mMagFilter = Ogre::FO_LINEAR;
        ref.mMipFilter = Ogre::FO_LINEAR;
        ref.mU = Ogre::TAM_CLAMP;
        ref.mV = Ogre::TAM_CLAMP;
        ref.mW = Ogre::TAM_CLAMP;
        sStandInSamplerMgr = root->getHlmsManager();
        sStandInSampler = sStandInSamplerMgr->getSamplerblock(ref);
    }
}

void PhotonPassBinding::releaseStandIns() {
    for (HostBinds &hb : sHost) { hb.standInCascades = 0; hb.standInField = false; }
    if (sStandInMgr) {
        if (sStandInVolume) sStandInMgr->destroyTexture(sStandInVolume);
        if (sStandInField) sStandInMgr->destroyTexture(sStandInField);
    }
    if (sStandInSamplerMgr && sStandInSampler) sStandInSamplerMgr->destroySamplerblock(sStandInSampler);
    sStandInVolume = sStandInField = nullptr;
    sStandInSampler = nullptr;
    sStandInSamplerMgr = nullptr;
    sStandInMgr = nullptr;
}

void PhotonPassBinding::releaseSamplers() {
    if (sSamplerMgr && sFieldSampler) sSamplerMgr->destroySamplerblock(sFieldSampler);
    sSamplerMgr = nullptr;
    sFieldSampler = nullptr;
    for (HostBinds &hb : sHost) hb = HostBinds();
}

}}}   // namespace jahshaka::engine::detail
