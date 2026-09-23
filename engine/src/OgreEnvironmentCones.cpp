// THE ENVIRONMENT'S CONE LOOKUP, MEASURED (PHOTON-ENV-1; gi.env_cone).
//
// jahEnvCone (src/rayquery/include/jah_environment.glsl) answers "what does a
// cone of half-angle theta see of the sky" with ONE fetch: the GGX-prefiltered
// chain at the mip whose lobe matches the cone. A GGX lobe is not a box, so that
// is an approximation, and this harness is where it is measured: the Hlms job
// `Jahshaka/EnvironmentCones` (JahEnvironmentCones_cs.glsl) inserts the piece the
// build wraps the shared file into — the same text every consumer runs — and
// computes, per query, the lookup and a 64-direction integral of the cube's
// finest mip over the same cone.
//
// A MEASUREMENT: one dispatch, a flush, a stalled readback. Never in a frame.
#include "EnginePrivate.h"

#include <OgreHlmsCompute.h>
#include <OgreHlmsComputeJob.h>
#include <OgreHlmsManager.h>
#include <OgreHlmsSamplerblock.h>
#include <OgreRenderSystem.h>
#include <OgreResourceTransition.h>
#include <OgreRoot.h>
#include <OgreTextureGpu.h>
#include <Vao/OgreAsyncTicket.h>
#include <Vao/OgreUavBufferPacked.h>
#include <Vao/OgreVaoManager.h>

#include <cstring>
#include <vector>

namespace jahshaka { namespace engine {
namespace detail {

namespace {

constexpr unsigned kMaxQueries = 64u;
const char *const kJobName = "Jahshaka/EnvironmentCones";

/// The job's input, in the layout JahEnvironmentCones_cs.glsl declares.
struct ConeParams {
    float queries[kMaxQueries][4] = {};   ///< xyz = world direction, w = tan(half-angle)
    float counts[4] = {};                 ///< x = queries, y = the cube's mip count
};

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

bool OgreEngine::environmentCones(Scene *scene, const std::vector<EnvironmentConeQuery> &queries,
                                  std::vector<EnvironmentConeAnswer> &out) {
    out.clear();
    if (!mRoot) return false;
    OgreScene *s = static_cast<OgreScene *>(scene);
    Ogre::TextureGpu *cube = s ? s->mReflectionTex : nullptr;
    if (!cube) {
        mLastError = "environmentCones: the scene has no environment cube (no sky, or its "
                     "capture has not landed yet)";
        return false;
    }
    if (queries.empty() || queries.size() > kMaxQueries) {
        mLastError = "environmentCones: between 1 and 64 queries";
        return false;
    }
    Ogre::RenderSystem *rs = mRoot->getRenderSystem();
    Ogre::HlmsManager *hm = mRoot->getHlmsManager();
    Ogre::HlmsCompute *hc = hm ? hm->getComputeHlms() : nullptr;
    Ogre::HlmsComputeJob *job = hc ? hc->findComputeJobNoThrow(kJobName) : nullptr;
    if (!job) {
        mLastError = "environmentCones: the harness media is missing (the job)";
        return false;
    }
    Ogre::VaoManager *vao = rs->getVaoManager();
    Ogre::UavBufferPacked *inBuf = nullptr;
    Ogre::UavBufferPacked *outBuf = nullptr;
    const auto release = [&]() {
        const Ogre::DescriptorSetUav::BufferSlot empty =
            Ogre::DescriptorSetUav::BufferSlot::makeEmpty();
        job->_setUavBuffer(0u, empty);
        job->_setUavBuffer(1u, empty);
        job->setNumTexUnits(0u);
        if (inBuf) { vao->destroyUavBuffer(inBuf); inBuf = nullptr; }
        if (outBuf) { vao->destroyUavBuffer(outBuf); outBuf = nullptr; }
    };
    JAH_TRY {
        ConeParams params;
        for (size_t i = 0; i < queries.size(); ++i) {
            params.queries[i][0] = queries[i].dirWorld.x;
            params.queries[i][1] = queries[i].dirWorld.y;
            params.queries[i][2] = queries[i].dirWorld.z;
            params.queries[i][3] = queries[i].tanHalfAngle;
        }
        params.counts[0] = float(queries.size());
        params.counts[1] = float(cube->getNumMipmaps());

        // Trilinear, as every reader of the chain samples it. A REFERENCE block:
        // the job acquires (and later releases) its own from it.
        Ogre::HlmsSamplerblock ref;
        ref.setFiltering(Ogre::TFO_TRILINEAR);
        ref.setAddressingMode(Ogre::TAM_CLAMP);
        job->setNumTexUnits(1u);
        {
            Ogre::DescriptorSetTexture2::TextureSlot slot(
                Ogre::DescriptorSetTexture2::TextureSlot::makeEmpty());
            slot.texture = cube;
            job->setTexture(0u, slot, &ref);
        }
        inBuf = vao->createUavBuffer(sizeof(ConeParams) / 16u, 16u, 0, &params, false);
        std::vector<float> zeros(size_t(kMaxQueries) * 8u, 0.0f);
        outBuf = vao->createUavBuffer(kMaxQueries * 2u, 16u, 0, zeros.data(), false);
        job->_setUavBuffer(0u, uavSlot(inBuf, Ogre::ResourceAccess::Read));
        job->_setUavBuffer(1u, uavSlot(outBuf, Ogre::ResourceAccess::Write));
        job->setThreadsPerGroup(64u, 1u, 1u);
        job->setNumThreadGroups(1u, 1u, 1u);
        {
            Ogre::ResourceTransitionArray &rt =
                rs->getBarrierSolver().getNewResourceTransitionsArrayTmp();
            job->analyzeBarriers(rt);
            rs->executeResourceTransition(rt);
            hc->dispatch(job, 0, 0);
        }
        std::vector<float> texels(size_t(kMaxQueries) * 8u, 0.0f);
        {
            Ogre::AsyncTicketPtr ticket = outBuf->readRequest(0, kMaxQueries * 2u);
            std::memcpy(texels.data(), ticket->map(), size_t(kMaxQueries) * 32u);
            ticket->unmap();
        }
        out.resize(queries.size());
        for (size_t i = 0; i < queries.size(); ++i) {
            const float *t = texels.data() + i * 8u;
            std::memcpy(out[i].lookup, t + 0, 3 * sizeof(float));
            out[i].lod = t[3];
            std::memcpy(out[i].reference, t + 4, 3 * sizeof(float));
        }
        release();
        return true;
    }
    catch (Ogre::Exception &e) {
        mLastError = "environmentCones: " + e.getFullDescription();
        release();
        return false;
    }
}

}  // namespace detail
}}  // namespace jahshaka::engine
