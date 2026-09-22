// THE ONE VOXEL READER'S PARITY HARNESS (PHOTON-READER-1; engine.voxel_reader_parity).
//
// WHAT IT PROVES. The cone march and the voxel read exist ONCE, as two plain-GLSL
// files (src/rayquery/include/jah_voxel_{sample,march}.glsl), and four consumers
// run that text: the pixel shader's cones (a FRAGMENT stage), the bounce job and
// the irradiance field's generation job (COMPUTE stages) and the ray hit. One
// source is necessary for "the field and the cones read one radiance field the
// same way"; it is not sufficient - two stages compiled separately may still
// disagree (a different contraction, a different binding, a different parameter
// source). This harness marches the same cones through the same bound chain in
// both stages and hands both answers back raw, so a suite can compare them BIT
// FOR BIT:
//
//   * the COMPUTE half is the Hlms job `Jahshaka/VoxelReaderParity`
//     (JahVoxelReaderParity_cs.glsl), which inserts the pieces JahVoxelSample,
//     JahVoxelMarch and JahVoxelParity exactly as the generation job inserts the
//     first two, over the chain bound the way that job binds it;
//   * the FRAGMENT half is a full-screen quad over a 256 x 1 RGBA32F target with
//     the material `Jahshaka/VoxelReaderParity`, whose program is GENERATED at
//     media staging from the SAME files (irisgl/engine/CMakeLists.txt).
//
// Both read the chain's parameters from VctLighting::getCascadeChainParams - the
// one definition the pixel pass buffer and the generation job take them from.
//
// A MEASUREMENT: renders one quad, dispatches one job, flushes and stalls on two
// readbacks. Never in a frame.
#include "EnginePrivate.h"

#include <OgreHlmsCompute.h>
#include <OgreHlmsComputeJob.h>
#include <OgreHlmsManager.h>
#include <OgreHlmsSamplerblock.h>
#include <OgreRenderSystem.h>
#include <OgreResourceTransition.h>
#include <OgreRoot.h>
#include <OgreCamera.h>
#include <OgreSceneManager.h>
#include <OgreTextureGpu.h>
#include <OgreTextureGpuManager.h>
#include <OgreAsyncTextureTicket.h>
#include <OgreMaterialManager.h>
#include <OgreMaterial.h>
#include <OgreTechnique.h>
#include <OgrePass.h>
#include <OgreTextureUnitState.h>
#include <OgreGpuProgramParams.h>
#include <Compositor/OgreCompositorManager2.h>
#include <Compositor/OgreCompositorNodeDef.h>
#include <Compositor/OgreCompositorWorkspace.h>
#include <Compositor/OgreCompositorWorkspaceDef.h>
#include <Compositor/Pass/PassQuad/OgreCompositorPassQuadDef.h>
#include <Vao/OgreAsyncTicket.h>
#include <Vao/OgreUavBufferPacked.h>
#include <Vao/OgreVaoManager.h>
#include <Vct/OgreVctLighting.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace jahshaka { namespace engine {
namespace detail {

namespace {

constexpr unsigned kMaxCones = 64u;
constexpr unsigned kMaxCascades = 4u;   ///< the fragment program declares four per kind
constexpr unsigned kTexels = 4u * kMaxCones;
const char *const kJobName = "Jahshaka/VoxelReaderParity";
const char *const kMaterialName = "Jahshaka/VoxelReaderParity";
const char *const kNodeName = "Jahshaka/VoxelReaderParity/Node";
const char *const kWorkspaceName = "Jahshaka/VoxelReaderParity/Workspace";

/// The inputs, in the layout both halves declare (the compute half's UAV struct
/// and the fragment half's uniform block).
struct ParityParams {
    float chainInvRes[8][4];
    float chainFromPrev[14][4];
    float cones[kTexels][4];
    float counts[4];
};

void toAnswers(const float *texels, size_t numCones, std::vector<VoxelReaderAnswer> &out) {
    out.resize(numCones);
    for (size_t i = 0; i < numCones; ++i) {
        const float *t = texels + i * 16u;
        std::memcpy(out[i].march, t + 0, 4 * sizeof(float));
        std::memcpy(out[i].escape, t + 4, 4 * sizeof(float));
        std::memcpy(out[i].hitRead, t + 8, 4 * sizeof(float));
        std::memcpy(out[i].marchRead, t + 12, 4 * sizeof(float));
    }
}

Ogre::DescriptorSetUav::BufferSlot uavSlot(Ogre::UavBufferPacked *buffer,
                                           Ogre::ResourceAccess::ResourceAccess access) {
    Ogre::DescriptorSetUav::BufferSlot slot = Ogre::DescriptorSetUav::BufferSlot::makeEmpty();
    slot.buffer = buffer;
    slot.offset = 0;
    slot.sizeBytes = 0;
    slot.access = access;
    return slot;
}

}   // namespace

bool OgreEngine::voxelReaderParity(Scene *scene, const std::vector<VoxelReaderCone> &cones,
                                   std::vector<VoxelReaderAnswer> &fragment,
                                   std::vector<VoxelReaderAnswer> &compute) {
    fragment.clear();
    compute.clear();
    if (!mRoot) return false;
    OgreScene *s = static_cast<OgreScene *>(scene);
    Ogre::VctLighting *vct = s ? s->voxelLighting() : nullptr;
    if (!vct) {
        mLastError = "voxelReaderParity: the scene has no voxel lighting built";
        return false;
    }
    if (cones.empty() || cones.size() > kMaxCones) {
        mLastError = "voxelReaderParity: between 1 and 64 cones";
        return false;
    }

    Ogre::RenderSystem *rs = mRoot->getRenderSystem();
    Ogre::HlmsCompute *hc = mRoot->getHlmsManager()->getComputeHlms();
    Ogre::HlmsComputeJob *job = hc ? hc->findComputeJobNoThrow(kJobName) : nullptr;
    Ogre::MaterialPtr material = Ogre::MaterialManager::getSingleton().getByName(
        kMaterialName, Ogre::ResourceGroupManager::AUTODETECT_RESOURCE_GROUP_NAME);
    if (!job || !material) {
        mLastError = "voxelReaderParity: the harness media is missing (the job or the material)";
        return false;
    }

    Ogre::UavBufferPacked *inBuf = nullptr;
    Ogre::UavBufferPacked *outBuf = nullptr;
    Ogre::TextureGpu *target = nullptr;
    Ogre::CompositorWorkspace *workspace = nullptr;
    Ogre::Camera *camera = nullptr;
    Ogre::SceneManager *sm = s->sceneManager();
    Ogre::VaoManager *vao = rs->getVaoManager();
    Ogre::TextureGpuManager *tm = rs->getTextureGpuManager();
    Ogre::CompositorManager2 *cm = mRoot->getCompositorManager2();
    Ogre::Pass *pass = nullptr;

    const auto release = [&]() {
        if (job) {
            const Ogre::DescriptorSetUav::BufferSlot empty =
                Ogre::DescriptorSetUav::BufferSlot::makeEmpty();
            job->_setUavBuffer(0u, empty);
            job->_setUavBuffer(1u, empty);
            job->setNumTexUnits(0u);
        }
        if (pass) {
            for (unsigned short u = 0; u < pass->getNumTextureUnitStates(); ++u)
                pass->getTextureUnitState(u)->setBlank();
        }
        if (workspace) { cm->removeWorkspace(workspace); workspace = nullptr; }
        if (camera) { sm->destroyCamera(camera); camera = nullptr; }
        if (target) { tm->destroyTexture(target); target = nullptr; }
        if (inBuf) { vao->destroyUavBuffer(inBuf); inBuf = nullptr; }
        if (outBuf) { vao->destroyUavBuffer(outBuf); outBuf = nullptr; }
    };

    JAH_TRY {
        // ---- THE INPUTS, ONE DEFINITION -----------------------------------
        const size_t chainLen = vct->getNumCascades();
        const unsigned numCascades = unsigned(std::min<size_t>(chainLen, kMaxCascades));
        const bool aniso = vct->isAnisotropic();
        std::vector<float> invRes(4u * chainLen, 0.0f);
        std::vector<float> fromPrev(8u * std::max<size_t>(chainLen, 1u), 0.0f);
        vct->getCascadeChainParams(invRes.data(), fromPrev.data());

        ParityParams params;
        std::memset(&params, 0, sizeof(params));
        for (unsigned c = 0; c < numCascades; ++c)
            std::memcpy(params.chainInvRes[c], &invRes[4u * c], 4 * sizeof(float));
        for (unsigned h = 0; h + 1u < numCascades; ++h)
            std::memcpy(params.chainFromPrev[2u * h], &fromPrev[8u * h], 8 * sizeof(float));
        for (size_t i = 0; i < cones.size(); ++i) {
            const VoxelReaderCone &k = cones[i];
            float *c = params.cones[4u * i];
            c[0] = k.posLS.x; c[1] = k.posLS.y; c[2] = k.posLS.z; c[3] = k.tanHalfAngle;
            c[4] = k.dirLS.x; c[5] = k.dirLS.y; c[6] = k.dirLS.z; c[7] = float(k.flags);
            c[8] = k.biasDirLS.x; c[9] = k.biasDirLS.y; c[10] = k.biasDirLS.z;
            c[11] = float(std::min(k.cascade, numCascades - 1u));
            c[12] = k.lod;
        }
        params.counts[0] = float(cones.size());
        params.counts[1] = float(numCascades);
        params.counts[2] = aniso ? 1.0f : 0.0f;

        // The volumes, per kind then per cascade (the generation job's order).
        const unsigned kinds = aniso ? 4u : 1u;
        auto volume = [&](unsigned kind, unsigned c) -> Ogre::TextureGpu * {
            return vct->getLightVoxelTextures(c)[kind];
        };
        const Ogre::HlmsSamplerblock *samplerblock = vct->getBindTrilinearSamplerblock();

        // ---- THE COMPUTE HALF ---------------------------------------------
        job->setProperty("hlms_num_vct_cascades", Ogre::int32(numCascades));
        job->setProperty("vct_anisotropic", aniso ? 1 : 0);
        job->setNumTexUnits(Ogre::uint8(kinds * numCascades));
        {
            Ogre::DescriptorSetTexture2::TextureSlot slot(
                Ogre::DescriptorSetTexture2::TextureSlot::makeEmpty());
            Ogre::uint8 unit = 0u;
            for (unsigned kind = 0; kind < kinds; ++kind)
                for (unsigned c = 0; c < numCascades; ++c, ++unit) {
                    slot.texture = volume(kind, c);
                    if (unit == 0u)
                        job->setTexture(unit, slot, samplerblock);
                    else
                        job->setTexture(unit, slot, 0, false);
                }
        }
        inBuf = vao->createUavBuffer(sizeof(ParityParams) / 16u, 16u, 0, &params, false);
        std::vector<float> zeros(size_t(kTexels) * 4u, 0.0f);
        outBuf = vao->createUavBuffer(kTexels, 16u, 0, zeros.data(), false);
        job->_setUavBuffer(0u, uavSlot(inBuf, Ogre::ResourceAccess::Read));
        job->_setUavBuffer(1u, uavSlot(outBuf, Ogre::ResourceAccess::Write));
        job->setThreadsPerGroup(64u, 1u, 1u);
        job->setNumThreadGroups(kTexels / 64u, 1u, 1u);
        {
            Ogre::ResourceTransitionArray &rt =
                rs->getBarrierSolver().getNewResourceTransitionsArrayTmp();
            job->analyzeBarriers(rt);
            rs->executeResourceTransition(rt);
            hc->dispatch(job, 0, 0);
        }
        std::vector<float> computeTexels(size_t(kTexels) * 4u, 0.0f);
        {
            Ogre::AsyncTicketPtr ticket = outBuf->readRequest(0, kTexels);
            std::memcpy(computeTexels.data(), ticket->map(), size_t(kTexels) * 16u);
            ticket->unmap();
        }

        // ---- THE FRAGMENT HALF --------------------------------------------
        material->load();
        pass = material->getTechnique(0)->getPass(0);
        for (unsigned short u = 0; u < pass->getNumTextureUnitStates() && u < 16u; ++u) {
            // Unit = kind * 4 + cascade. Cascades past the chain repeat its last
            // one and the anisotropic kinds repeat the isotropic volume on a Low
            // chain: the program never reads them (its count and its anisotropic
            // switch say so), but a descriptor must not be empty.
            const unsigned kind = u / 4u;
            const unsigned c = std::min(unsigned(u % 4u), numCascades - 1u);
            Ogre::TextureUnitState *tus = pass->getTextureUnitState(u);
            tus->setTexture(volume(kind < kinds ? kind : 0u, c));
            tus->setSamplerblock(*samplerblock);
        }
        Ogre::GpuProgramParametersSharedPtr fp = pass->getFragmentProgramParameters();
        fp->setNamedConstant("chainInvRes", &params.chainInvRes[0][0], 8u, 4u);
        fp->setNamedConstant("chainFromPrev", &params.chainFromPrev[0][0], 14u, 4u);
        fp->setNamedConstant("cones", &params.cones[0][0], kTexels, 4u);
        fp->setNamedConstant("counts", &params.counts[0], 1u, 4u);

        target = tm->createTexture("Jahshaka/VoxelReaderParity/Target",
                                   Ogre::GpuPageOutStrategy::Discard,
                                   Ogre::TextureFlags::RenderToTexture, Ogre::TextureTypes::Type2D);
        target->setResolution(kTexels, 1u);
        target->setPixelFormat(Ogre::PFG_RGBA32_FLOAT);
        target->scheduleTransitionTo(Ogre::GpuResidency::Resident);

        if (!cm->hasNodeDefinition(kNodeName)) {
            Ogre::CompositorNodeDef *node = cm->addNodeDefinition(kNodeName);
            node->addTextureSourceName("rt", 0u, Ogre::TextureDefinitionBase::TEXTURE_INPUT);
            node->setNumTargetPass(1u);
            Ogre::CompositorTargetDef *tdef = node->addTargetPass("rt");
            tdef->setNumPasses(1u);
            Ogre::CompositorPassQuadDef *quad =
                static_cast<Ogre::CompositorPassQuadDef *>(tdef->addPass(Ogre::PASS_QUAD));
            quad->mMaterialName = kMaterialName;
            quad->setAllLoadActions(Ogre::LoadAction::DontCare);
            Ogre::CompositorWorkspaceDef *ws = cm->addWorkspaceDefinition(kWorkspaceName);
            ws->connectExternal(0u, kNodeName, 0u);
        }
        camera = sm->createCamera("Jahshaka/VoxelReaderParity/Camera");
        Ogre::CompositorChannelVec channels;
        channels.push_back(target);
        workspace = cm->addWorkspace(sm, channels, camera, kWorkspaceName, false);
        workspace->_beginUpdate(false);
        workspace->_update();
        workspace->_endUpdate(false);

        // THE FLUSH IS LOAD-BEARING: the quad is only RECORDED until something
        // submits it, and a ticket reads whatever is in VRAM.
        rs->flushCommands();
        std::vector<float> fragmentTexels(size_t(kTexels) * 4u, 0.0f);
        {
            Ogre::AsyncTextureTicket *ticket = tm->createAsyncTextureTicket(
                kTexels, 1u, 1u, Ogre::TextureTypes::Type2D, Ogre::PFG_RGBA32_FLOAT);
            ticket->download(target, 0u, true);
            const Ogre::TextureBox box = ticket->map(0);
            std::memcpy(fragmentTexels.data(), box.at(0, 0, 0), size_t(kTexels) * 16u);
            ticket->unmap();
            tm->destroyAsyncTextureTicket(ticket);
        }

        toAnswers(fragmentTexels.data(), cones.size(), fragment);
        toAnswers(computeTexels.data(), cones.size(), compute);
        release();
        return true;
    }
    catch (Ogre::Exception &e) {
        mLastError = "voxelReaderParity: " + e.getFullDescription();
        release();
        return false;
    }
}

}  // namespace detail
}}  // namespace jahshaka::engine
