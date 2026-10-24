// THE NO-RAY ROUTE'S SCREEN INTEGRATE - the host half (PHOTON-7 stage 7, 7-6; ChainDesc::screenIntegrate).
//
// On a chain that does not gather, the compute pass "Jahshaka screen integrate" (Jahshaka/ScreenIntegrate,
// JahScreenIntegrate_cs.glsl) writes the screen products the opaque passes read in place of the world caches. Its
// texture sources (the prepass's depth, normals and roughness) and its three targets are the chain's (OgreChain.cpp);
// everything else is bound here, by the view's ScreenGiListener, in the pass's EARLY pre-execute - before the pass sets
// its own resources and asks the job for its barriers, so the world caches it reads are transitioned with them:
//   the parameters (the camera's image, the chain and the environment as every voxel reader takes them - the card
//   job's construction, JahCardLight_cs.glsl - the irradiance field for an identity view, the fog law's eight), one
//   UAV buffer a listener (a view);
//   the voxel cascades' volumes, the environment cube, the field's two atlases and the air's volume;
// and around every PrePassUse pass of the view (the screen decode, the opaque pass) the three targets are REGISTERED
// with the Hlms listener as the gather's are - pass-scoped: taken away when the pass ends, so the planar mirrors
// (rendered in the opaque pass's early pre-execute) and every other pass of the scene never see them.
// Platform-neutral (no ray query): the job is an Hlms compute job, so the route runs where the ray tier is compiled out.
#include "EnginePrivate.h"

#include "photon/voxel/PhotonIrradianceField.h"
#include "photon/voxel/PhotonVoxelLighting.h"
#include "photon/voxel/PhotonVoxelizer.h"

#include <Compositor/OgreCompositorNode.h>
#include <Compositor/Pass/OgreCompositorPass.h>
#include <Compositor/Pass/PassCompute/OgreCompositorPassCompute.h>
#include <Compositor/Pass/PassScene/OgreCompositorPassSceneDef.h>
#include <OgreCamera.h>
#include <OgreHlmsCompute.h>
#include <OgreHlmsComputeJob.h>
#include <OgreHlmsManager.h>
#include <OgreRenderSystem.h>
#include <OgreRoot.h>
#include <OgreSceneManager.h>
#include <OgreTextureGpu.h>
#include <OgreTextureGpuManager.h>
#include <Vao/OgreUavBufferPacked.h>
#include <Vao/OgreVaoManager.h>

#include <algorithm>
#include <cstring>

namespace jahshaka { namespace engine { namespace detail {

namespace {
void put3(float out[4], const Ogre::Vector3 &v, float w) {
    out[0] = float(v.x);
    out[1] = float(v.y);
    out[2] = float(v.z);
    out[3] = w;
}
/// JahScreenIntegrate_cs.glsl's ScreenGiParams, in vec4s: the camera 10, the chain 22 + 4, the environment's SH 9, the
/// field 7, the fog 8.
constexpr size_t kScreenGiVec4 = 10u + 8u + 14u + 4u + 9u + 7u + 8u;
constexpr size_t kScreenGiFloats = kScreenGiVec4 * 4u;
/// The most cascades the job binds (the longest shipped tier chain).
constexpr unsigned kScreenGiMaxCascades = unsigned(kGiTierMaxCascades);
}   // namespace

CameraImageF cameraImage(bool ortho, const Ogre::Vector3 &pos, const Ogre::Quaternion &rot, float el, float er,
                         float et, float eb) {
    CameraImageF e;
    const Ogre::Vector3 f = rot * Ogre::Vector3::NEGATIVE_UNIT_Z;
    const Ogre::Vector3 r = rot * Ogre::Vector3::UNIT_X;
    const Ogre::Vector3 u = rot * Ogre::Vector3::UNIT_Y;
    put3(e.camPos, pos, ortho ? 0.0f : 1.0f);
    put3(e.rayTL, r * el + u * et + (ortho ? Ogre::Vector3::ZERO : f), 0.0f);
    put3(e.rayRight, r * (er - el), 0.0f);
    put3(e.rayDown, u * (eb - et), 0.0f);
    put3(e.fwd, f, 0.0f);
    return e;
}

// A target uv t is the shot's (t - x0) / w, hence rayTL' = rayTL - rayRight x0/w
// - rayDown y0/h, rayRight' = rayRight / w, rayDown' = rayDown / h. The bars
// hold cleared depth and every job declines them before any ray.
void expandCameraImageToTarget(CameraImageF &e, const ChainDesc &cd, unsigned fullW, unsigned fullH) {
    if (!cd.letterbox || !fullH) return;
    float shot[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
    chain::letterboxRect(cd.letterboxAspect, float(fullW) / float(fullH), shot);
    for (int k = 0; k < 3; ++k) {
        e.rayTL[k] -= e.rayRight[k] * shot[0] / shot[2] + e.rayDown[k] * shot[1] / shot[3];
        e.rayRight[k] /= shot[2];
        e.rayDown[k] /= shot[3];
    }
}

ScreenGiListener::~ScreenGiListener() {
    // THE SHARED JOBS FORGET THIS LISTENER'S BUFFERS (Ogre's job drops a deleted TEXTURE from its slots - a
    // TextureGpuListener - and keeps a deleted buffer's pointer): the parameters' and the chain's slots emptied, so no
    // job holds a pointer this destructor frees; the next pass of any view sets every slot again before it dispatches
    if (Ogre::HlmsManager *hm = Ogre::Root::getSingletonPtr() ? Ogre::Root::getSingleton().getHlmsManager() : nullptr)
        if (Ogre::HlmsCompute *hc = hm->getComputeHlms()) {
            const Ogre::DescriptorSetUav::BufferSlot none(Ogre::DescriptorSetUav::BufferSlot::makeEmpty());
            const auto forget = [&](const char *name, std::initializer_list<Ogre::uint8> slots) {
                Ogre::HlmsComputeJob *job = hc->findComputeJobNoThrow(Ogre::IdString(name));
                for (Ogre::uint8 u : slots)
                    if (job && u < job->getNumUavUnits()) job->_setUavBuffer(u, none);
            };
            forget(kScreenIntegrateJob, { 3u });
            forget(kScreenAreaVisJob, { 5u, 6u });
            forget(kScreenAreaVisResolveJob, { 1u });
        }
    dropAreaVis();
    Ogre::RenderSystem *rs = Ogre::Root::getSingleton().getRenderSystem();
    Ogre::VaoManager *vao = rs ? rs->getVaoManager() : nullptr;
    Ogre::TextureGpuManager *tm = rs ? rs->getTextureGpuManager() : nullptr;
    for (Ogre::TextureGpu *&t : mAreaStandIn) {
        if (t && tm) tm->destroyTexture(t);
        t = nullptr;
    }
    for (Ogre::UavBufferPacked **b : { &mParams, &mParamsIdle, &mAreaParams, &mAreaChain, &mAreaIdle }) {
        if (*b && vao) vao->destroyUavBuffer(*b);
        *b = nullptr;
    }
}

void ScreenGiListener::passEarlyPreExecute(Ogre::CompositorPass *pass) {
    if (!pass || !mView || pass->getType() != Ogre::PASS_COMPUTE) return;
    const Ogre::uint32 id = pass->getDefinition()->mIdentifier;
    if (id == kScreenIntegratePassIdentifier)
        bindJob(static_cast<Ogre::CompositorPassCompute *>(pass));
    else if (id == kScreenAreaVisPassIdentifier)
        bindAreaVis(static_cast<Ogre::CompositorPassCompute *>(pass));
    else if (id == kScreenAreaVisResolvePassIdentifier)
        bindAreaVisResolve(static_cast<Ogre::CompositorPassCompute *>(pass));
}

void ScreenGiListener::passPreExecute(Ogre::CompositorPass *pass) {
    if (!pass || !mView) return;
    if (pass->getType() != Ogre::PASS_SCENE) return;
    const auto *def = static_cast<const Ogre::CompositorPassSceneDef *>(pass->getDefinition());
    if (!def || def->mPrePassMode != Ogre::PrePassUse) return;
    OgreScene *scene = mView->ogreScene();
    const Ogre::CompositorNode *node = pass->getParentNode();
    if (!scene || !node) return;
    FogHlmsListener::ProbeGatherBind bind;
    bind.irradiance = node->getDefinedTexture(Ogre::IdString(kScreenGiIrradiance));
    bind.roughSpec = node->getDefinedTexture(Ogre::IdString(kScreenGiSpecular));
    bind.coneHit = node->getDefinedTexture(Ogre::IdString(kScreenGiConeHit));
    // (the view's own passes, and the frame capture's - the same frame, camera and size: it takes the view's product,
    // never a history of its own; a product of another size is no answer for these pixels)
    const Ogre::TextureGpu *drawn = pass->getAnyTargetTexture();
    const bool area = mAreaRan && mAreaVis && drawn && drawn->getWidth() == mAreaFullW && drawn->getHeight() == mAreaFullH;
    // THE PRODUCTS' LAYOUTS, through Ogre's own solver, before the pass reads them (the gather's and the ray tier's
    // rule: the compute passes left them written as UAVs; the pass samples them from its fragment stage - the listener's
    // registration is invisible to the compositor's own barrier analysis)
    {
        Ogre::RenderSystem *rs = Ogre::Root::getSingleton().getRenderSystem();
        Ogre::BarrierSolver &solver = rs->getBarrierSolver();
        Ogre::ResourceTransitionArray trans;
        for (Ogre::TextureGpu *t : { bind.irradiance, bind.roughSpec, bind.coneHit, area ? mAreaVis : nullptr })
            if (t)
                solver.resolveTransition(trans, t, Ogre::ResourceLayout::Texture, Ogre::ResourceAccess::Read,
                                         1u << Ogre::GPT_FRAGMENT_PROGRAM);
        rs->executeResourceTransition(trans);
    }
    if (bind.irradiance && bind.roughSpec && bind.coneHit)
        FogHlmsListener::setProbeGather(scene->sceneManager(), bind);
    // ...and THE AREA LIGHTS' VISIBILITY (OgreAreaVis.cpp), while it traces or holds
    if (area) FogHlmsListener::setAreaVis(scene->sceneManager(), mAreaVis);
}

void ScreenGiListener::passPosExecute(Ogre::CompositorPass *pass) {
    if (!pass || !mView) return;
    if (pass->getType() == Ogre::PASS_COMPUTE) {
        if (pass->getDefinition()->mIdentifier == kScreenAreaVisResolvePassIdentifier) afterAreaVisResolve(pass);
        return;
    }
    if (pass->getType() != Ogre::PASS_SCENE) return;
    const auto *def = static_cast<const Ogre::CompositorPassSceneDef *>(pass->getDefinition());
    if (!def || def->mPrePassMode != Ogre::PrePassUse) return;
    if (OgreScene *scene = mView->ogreScene()) {
        FogHlmsListener::setProbeGather(scene->sceneManager(), FogHlmsListener::ProbeGatherBind());
        if (mAreaRan && mAreaVis) FogHlmsListener::setAreaVis(scene->sceneManager(), nullptr);
    }
}

bool ScreenGiListener::ownPass(const Ogre::CompositorPass *pass) const {
    const Ogre::CompositorNode *node = pass ? pass->getParentNode() : nullptr;
    return node && mView && node->getWorkspace() == mView->workspace();
}

bool ScreenGiListener::bindMissing() const {
    return mView && mView->mEngine && mView->mEngine->armRegistry().on(ArmId::GiScreenBindMissing);
}

// THE STAND-DOWN (PHOTON-7 stage 7, the lead's fix): the job is the SHARED one, so a bind that returned early left the
// last view's bindings in it - that view's volumes and parameters, which it may have destroyed or resized since - and
// the pass dispatched over them across its whole target (gi.screen_bind (1), measured before the fix: Ogre's barrier
// solver threw on another view's texture, "from Undefined to a read-only layout", and the frame was lost for every
// view). Now every slot this listener binds holds a stand-in - the permutation with no chain, no environment, no field
// and no fog (the pass's own three textures and targets are the pass's), the parameters an idle block - and the pass
// runs ONE group, which reads the block's 0 x 0 target and returns (the fork's HlmsCompute::dispatch refuses a
// dispatch of no group, BUGS-6's guard). The products stay the last ones written.
void ScreenGiListener::standDownIntegrate(Ogre::HlmsComputeJob *job) {
    const auto prop = [job](const char *name, Ogre::int32 v) {
        if (job->getProperty(name) != v) job->setProperty(name, v);
    };
    for (const char *p : { "hlms_num_vct_cascades", "jah_env", "jah_si_field", "jah_si_fog", "jah_si_air", "jah_si_sdf" })
        prop(p, 0);
    if (job->getNumTexUnits() != 3u) job->setNumTexUnits(3u);
    Ogre::RenderSystem *rs = Ogre::Root::getSingleton().getRenderSystem();
    Ogre::VaoManager *vao = rs ? rs->getVaoManager() : nullptr;
    if (!mParamsIdle && vao) {
        const float zero[kScreenGiFloats] = {};
        mParamsIdle = vao->createUavBuffer(kScreenGiVec4, 16u, 0, const_cast<float *>(zero), false);
    }
    if (mParamsIdle) {
        Ogre::DescriptorSetUav::BufferSlot buf(Ogre::DescriptorSetUav::BufferSlot::makeEmpty());
        buf.buffer = mParamsIdle;
        buf.offset = 0;
        buf.sizeBytes = 0;
        buf.access = Ogre::ResourceAccess::Read;
        job->_setUavBuffer(3u, buf);
    }
    job->setNumThreadGroupsBasedOn(Ogre::HlmsComputeJob::ThreadGroupsBasedOnNothing, 0u, 1u, 1u, 1u);
    job->setNumThreadGroups(1u, 1u, 1u);
}

void ScreenGiListener::bindJob(Ogre::CompositorPassCompute *pass) {
    // THE PASS'S JOB IS THE SHARED ONE (CompositorPassCompute finds it by name, uncloned): every binding is set here,
    // in front of its dispatch
    Ogre::HlmsCompute *hc = Ogre::Root::getSingleton().getHlmsManager()->getComputeHlms();
    Ogre::HlmsComputeJob *job = hc ? hc->findComputeJobNoThrow(Ogre::IdString(kScreenIntegrateJob)) : nullptr;
    OgreScene *scene = mView->ogreScene();
    Ogre::Camera *cam = mView->camera();
    const Ogre::CompositorNode *node = pass ? pass->getParentNode() : nullptr;
    Ogre::TextureGpu *target = node ? node->getDefinedTexture(Ogre::IdString(kScreenGiIrradiance)) : nullptr;
    Ogre::RenderSystem *rs = Ogre::Root::getSingleton().getRenderSystem();
    Ogre::VaoManager *vao = rs ? rs->getVaoManager() : nullptr;
    if (!job) return;
    if (bindMissing() || !scene || !cam || !target || !vao) {
        standDownIntegrate(job);
        return;
    }
    // (a pass that stood down ran one group: this one's follow its first target again)
    job->setNumThreadGroupsBasedOn(Ogre::HlmsComputeJob::ThreadGroupsBasedOnUav, 0u, 1u, 1u, 1u);
    float g[kScreenGiFloats] = {};
    float *at = g;
    const auto vec4 = [&at](const float v[4]) {
        std::memcpy(at, v, 4u * sizeof(float));
        at += 4;
    };

    // ---- THE CAMERA'S IMAGE (the ray jobs' five numbers), its projection, its axes, the target ----
    const bool ortho = cam->getProjectionType() == Ogre::PT_ORTHOGRAPHIC;
    const Ogre::Quaternion q = cam->getDerivedOrientation();
    Ogre::Real fl = 0, fr = 0, ft = 0, fb = 0;
    cam->getFrustumExtents(fl, fr, ft, fb, ortho ? Ogre::FET_PROJ_PLANE_POS : Ogre::FET_TAN_HALF_ANGLES);
    CameraImageF eye = cameraImage(ortho, cam->getDerivedPosition(), q, float(fl), float(fr), float(ft), float(fb));
    const unsigned fullW = target->getWidth(), fullH = target->getHeight();
    expandCameraImageToTarget(eye, mView->chainDesc(), fullW, fullH);
    vec4(eye.camPos);
    vec4(eye.rayTL);
    vec4(eye.rayRight);
    vec4(eye.rayDown);
    vec4(eye.fwd);
    {
        const Ogre::Vector2 ab = cam->getProjectionParamsAB();
        const float proj[4] = { float(ab.x), float(ab.y), float(cam->getFarClipDistance()), 0.0f };
        vec4(proj);
        float axis[4];
        put3(axis, q * Ogre::Vector3::UNIT_X, 0.0f);
        vec4(axis);
        put3(axis, q * Ogre::Vector3::UNIT_Y, 0.0f);
        vec4(axis);
        put3(axis, q * Ogre::Vector3::UNIT_Z, 0.0f);
        vec4(axis);
        const float res[4] = { float(fullW), float(fullH), 0.0f, 0.0f };
        vec4(res);
    }

    // ---- THE CHAIN AND THE ENVIRONMENT (the card job's block, member for member) ----
    Ogre::PhotonVoxelLighting *vct = scene->photon().voxelLighting();
    unsigned numCascades = 0u;
    Ogre::TextureGpu *envCube = nullptr;
    float *chain = at;   // chainInvRes[8], chainFromPrev[14], volumeOrigin, volumeInvSize, counts, envGainMips, envSh[9]
    at += 4u * (8u + 14u + 4u + 9u);
    if (vct && vct->getLightVoxelTextures() && vct->getLightVoxelTextures()[0] && vct->getVoxelizer()) {
        const size_t chainLen = vct->getNumCascades();
        numCascades = unsigned(std::min<size_t>(chainLen, kScreenGiMaxCascades));
        std::vector<float> invRes(4u * chainLen, 0.0f);
        std::vector<float> fromPrev(8u * std::max<size_t>(chainLen, 1u), 0.0f);
        vct->getCascadeChainParams(invRes.data(), fromPrev.data());
        std::memcpy(chain, invRes.data(), 4u * numCascades * sizeof(float));
        std::memcpy(chain + 32u, fromPrev.data(), 8u * (numCascades ? numCascades - 1u : 0u) * sizeof(float));
        const Ogre::Vector3 origin = vct->getVoxelizer()->getVoxelOrigin();
        const Ogre::Vector3 size = vct->getVoxelizer()->getVoxelSize();
        chain[88] = float(origin.x); chain[89] = float(origin.y); chain[90] = float(origin.z);
        chain[92] = 1.0f / float(size.x); chain[93] = 1.0f / float(size.y); chain[94] = 1.0f / float(size.z);
        chain[96] = float(numCascades);
        {
            // the specular SDF skip's two numbers (PhotonVoxelLighting::fillConstBufferData's, after the chain's rows)
            std::vector<float> pb(vct->getConstBufferSize() / sizeof(float) + 4u, 0.0f);
            vct->fillConstBufferData(Ogre::Matrix4::IDENTITY, pb.data());
            const size_t skip = 4u * chainLen + 8u * (chainLen - 1u);
            chain[97] = pb[skip];
            chain[98] = pb[skip + 1u];
        }
        envCube = vct->getEnvironmentCube();
        const float *gain = vct->getEnvironmentGain();
        chain[100] = gain[0]; chain[101] = gain[1]; chain[102] = gain[2];
        chain[103] = envCube ? float(envCube->getNumMipmaps()) : 1.0f;
        const float *sh = vct->getEnvironmentSh();
        for (unsigned k = 0; k < 9u; ++k) {
            chain[104u + 4u * k] = sh[3u * k];
            chain[105u + 4u * k] = sh[3u * k + 1u];
            chain[106u + 4u * k] = sh[3u * k + 2u];
        }
    } else {
        // NO VOLUME YET: the scene's SH alone (world axes), the diffuse's whole answer until the chain is built
        const float *sh = scene->sceneManager()->getSphericalHarmonics();
        static const float kSign[9] = { 1.0f, 1.0f, -1.0f, -1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f };
        for (unsigned k = 0; k < 9u; ++k)
            for (unsigned c = 0; c < 3u; ++c) chain[104u + 4u * k + c] = sh ? kSign[k] * sh[3u * k + c] : 0.0f;
    }

    // ---- THE IRRADIANCE FIELD, for an identity view (the gather's integrate reads it the same way) ----
    Ogre::PhotonIrradianceField *ifd = nullptr;
    {
        const SceneGiBinding *gb = sceneGiBindingOf(scene->sceneManager());
        ifd = gb ? gb->ifd : nullptr;
        if (!(ifd && ifd->getIrradianceTex() && ifd->getDepthVarianceTex())) ifd = nullptr;
    }
    if (ifd) {
        ifd->fillConstBufferData(Ogre::Matrix4::IDENTITY, at);   // 24 floats: three rows, counts, two atlases
        const Ogre::uint32 *o = ifd->getWindowOffset();
        at[24] = float(o[0] + 128u * o[1] + 16384u * o[2]);
        at[25] = float(ifd->getSettings().mNumProbes[1]);
        at[26] = float(ifd->getSettings().mNumProbes[2]);
        at[27] = 1.0f;
    }
    at += 4u * 7u;

    // ---- THE FOG LAW (OgreScene::fogAlong: the media along a reflection) ----
    Ogre::TextureGpu *aerial = nullptr;
    {
        float fog[8][4] = {};
        scene->fogAlong(fog, aerial);
        std::memcpy(at, fog, sizeof(fog));
        at += 32;
    }
    const bool fogOn = g[(10u + 8u + 14u + 4u + 9u + 7u + 3u) * 4u + 3u] > 0.5f;

    // ---- THE PARAMETERS' BUFFER (one a view: the job is shared, every binding is set here) ----
    if (!mParams)
        mParams = vao->createUavBuffer(kScreenGiVec4, 16u, 0, g, false);
    else
        mParams->upload(g, 0, kScreenGiVec4);

    // ---- THE PERMUTATION ----
    const auto prop = [job](const char *name, Ogre::int32 v) {
        if (job->getProperty(name) != v) job->setProperty(name, v);
    };
    prop("hlms_num_vct_cascades", Ogre::int32(numCascades));
    prop("jah_env", envCube ? 1 : 0);
    prop("jah_si_field", ifd ? 1 : 0);
    prop("jah_si_fog", fogOn ? 1 : 0);
    prop("jah_si_air", fogOn && aerial ? 1 : 0);
    prop("jah_si_sdf", numCascades && vct->shouldEnableSpecularSdfQuality() ? 1 : 0);

    // ---- THE TEXTURES after the pass's own three (0 depth, 1 normals, 2 roughness) ----
    constexpr unsigned kOwn = 3u;
    const unsigned kinds = numCascades ? unsigned(vct->getNumVoxelTextures()) : 0u;
    const unsigned units = kOwn + kinds * numCascades + (envCube ? 1u : 0u) + (ifd ? 2u : 0u) +
                           (fogOn && aerial ? 1u : 0u);
    job->setNumTexUnits(Ogre::uint8(units));
    Ogre::uint8 unit = Ogre::uint8(kOwn);
    const auto bindTex = [&](Ogre::TextureGpu *tex, const Ogre::HlmsSamplerblock *sampler) {
        Ogre::DescriptorSetTexture2::TextureSlot slot(Ogre::DescriptorSetTexture2::TextureSlot::makeEmpty());
        slot.texture = tex;
        if (sampler)
            job->setTexture(unit, slot, sampler);
        else
            job->setTexture(unit, slot, nullptr, false);
        ++unit;
    };
    Ogre::HlmsManager *hm = Ogre::Root::getSingleton().getHlmsManager();
    const Ogre::HlmsSamplerblock *linear = FogHlmsListener::acquireSampler(hm, true);
    for (unsigned kind = 0; kind < kinds; ++kind)
        for (unsigned c = 0; c < numCascades; ++c)
            bindTex(vct->getLightVoxelTextures(c)[kind],
                    unit == kOwn ? vct->getBindTrilinearSamplerblock() : nullptr);
    if (envCube) bindTex(envCube, linear);
    if (ifd) {
        bindTex(ifd->getIrradianceTex(), linear);
        bindTex(ifd->getDepthVarianceTex(), nullptr);
    }
    if (fogOn && aerial) bindTex(aerial, linear);

    Ogre::DescriptorSetUav::BufferSlot buf(Ogre::DescriptorSetUav::BufferSlot::makeEmpty());
    buf.buffer = mParams;
    buf.offset = 0;
    buf.sizeBytes = 0;
    buf.access = Ogre::ResourceAccess::Read;
    job->_setUavBuffer(3u, buf);
}

}}}   // namespace jahshaka::engine::detail
