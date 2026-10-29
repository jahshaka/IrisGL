// THE PIXEL'S AREA LIGHT VISIBILITY - the host's one text for both producers, and the no-ray route's half (PHOTON-7
// stage 7: 7-A built it on the ray tier, 7-6 put the second producer beside it; SPECS/PHOTON_7_SPEC.md "AREA LIGHTS,
// ONE LAW ON EVERY PATH").
//
// Ogre-Next draws an LTC area light with no shadow. The pixel takes the cards' law - the ratio over n x n strata of the
// light's rectangle, its two sums averaged over time (jah_area_vis_law.glsl, one text for both producers' shaders) -
// into the screen product `jahAreaVis` the fork's hook in DoAreaLtcLights multiplies light i's LTC terms by. THE RAY
// TIER (OgreRayQuery.cpp's recordAreaVis) sees a stratum by one ray; THE NO-RAY ROUTE (here: the two compute passes
// right after the screen integrate, kScreenAreaVisPassIdentifier and kScreenAreaVisResolvePassIdentifier) sees it
// through the voxel chain's own shadow march (LightInjection's jahInjectVisibility). What both take from here: the
// forward LTC set in HlmsPbs's order, the parameter block, the view's history and rest state and one frame's numbers.
// Platform-neutral: the no-ray route's jobs are Hlms compute jobs, so it runs where the ray tier is compiled out.
// MONO ONLY on both (a stereo view declines: stage 9's stereo item).
#include "EnginePrivate.h"

#include "photon/voxel/PhotonVoxelLighting.h"
#include "photon/voxel/PhotonVoxelizer.h"

#include <Compositor/OgreCompositorNode.h>
#include <Compositor/Pass/OgreCompositorPass.h>
#include <Compositor/Pass/PassCompute/OgreCompositorPassCompute.h>
#include <OgreCamera.h>
#include <OgreHlmsCompute.h>
#include <OgreHlmsComputeJob.h>
#include <OgreHlmsManager.h>
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
/// The no-ray route's chain block (JahScreenAreaVis_cs.glsl's AreaChainParams), in vec4s.
constexpr size_t kAreaChainVec4 = 8u + 8u + 3u;
}   // namespace

// ---------------------------------------------------------------------------
// BOTH PRODUCERS' ONE TEXT
// ---------------------------------------------------------------------------
std::vector<const Ogre::Light *> forwardLtcLights(const Ogre::SceneManager *sm) {
    std::vector<const Ogre::Light *> out;
    if (!sm) return out;
    for (const Ogre::Light *l : sm->getGlobalLightList().lights) {
        if (!l || l->getType() != Ogre::Light::LT_AREA_LTC) continue;
        out.push_back(l);
        if (out.size() >= size_t(lightextras::kAreaLtcLimit)) break;
    }
    return out;
}

bool areaVisFrame(AreaVisState &st, AreaVisParams &pp, const std::vector<const Ogre::Light *> &lights,
                  const Ogre::Camera *cam, const ChainDesc &cd, unsigned w, unsigned h, unsigned fullW, unsigned fullH,
                  unsigned strata, const PhotonComponent &photon, bool freshImages, float liftA, float liftB,
                  float mask, CameraImageF &eye) {
    // ---- THE LIGHTS, in HlmsPbs's order, and the rest's test ----------------
    std::vector<float> lightKey;
    for (size_t i = 0; i < lights.size() && i < 4u; ++i) {
        const Ogre::Light *l = lights[i];
        Ogre::Vector3 corner[4];
        lightextras::areaLightCorners(l, corner);
        const Ogre::Vector3 eu = corner[1] - corner[0], ev = corner[3] - corner[0];
        const Ogre::Vector3 emit = l->getDerivedDirection();
        put3(pp.light[i].c0, corner[0], l->getDoubleSided() ? 1.0f : 0.0f);
        put3(pp.light[i].eu, eu, l->getAttenuationRange());
        put3(pp.light[i].ev, ev, 0.0f);
        put3(pp.light[i].emit, emit, 0.0f);
        for (const float *v : { pp.light[i].c0, pp.light[i].eu, pp.light[i].ev, pp.light[i].emit })
            lightKey.insert(lightKey.end(), v, v + 4);
    }
    st.lights = unsigned(std::min<size_t>(lights.size(), 4u));
    pp.lights[0] = float(st.lights);

    // ---- THE CAMERA, and whether the view is at rest -----------------------
    const bool ortho = cam->getProjectionType() == Ogre::PT_ORTHOGRAPHIC;
    const Ogre::Quaternion q = cam->getDerivedOrientation();
    Ogre::Real fl = 0, fr = 0, ft = 0, fb = 0;
    cam->getFrustumExtents(fl, fr, ft, fb, ortho ? Ogre::FET_PROJ_PLANE_POS : Ogre::FET_TAN_HALF_ANGLES);
    eye = cameraImage(ortho, cam->getDerivedPosition(), q, float(fl), float(fr), float(ft), float(fb));
    expandCameraImageToTarget(eye, cd, fullW, fullH);
    const float *now[5] = { eye.camPos, eye.rayTL, eye.rayRight, eye.rayDown, eye.fwd };
    bool sameCamera = st.age > 0u;
    for (int k = 0; k < 5 && sameCamera; ++k) sameCamera = std::memcmp(st.prev[k], now[k], sizeof(st.prev[k])) == 0;
    // (a material edit: the casters' alpha - the light set's key, a restart of the sums)
    const unsigned long long edits = photon.materialEditSerial();
    lightKey.push_back(float(edits & 0xFFFFFFull));
    lightKey.push_back(float((edits >> 24u) & 0xFFFFFFull));
    const bool still = sameCamera && lightKey == st.lightKey;
    // THE TRACED GEOMETRY THAT CHANGED (PHOTON-7 8B, B2): the list's movers' swept boxes - every transform write, flags
    // change and birth of a traced slot is one (an EDIT is the store's geometry or a material, which no visibility ray
    // reads; a material edit restarts the light set below, its alpha being the casters') - an overflow or a restart:
    // every texel. Each resting texel whose light rays one meets rests again from the next frame (jah_area_vis_law.glsl)
    // EACH CLOSE IS READ ONCE (two frames reading one close would restart its texels twice), and a close the view missed
    // that carried an input restarts every texel (the gather's missed-draw rule, without its held exception: the
    // visibility's history is cheap to re-rest)
    const GiChangedRegions &regions = photon.changedRegions();
    const unsigned long long serial = photon.regionCloseSerial(), lastInput = photon.lastInputCloseSerial();
    const bool fresh = serial != st.listSerialSeen;
    const bool missed = st.listSerialSeen != 0ull && lastInput > st.listSerialSeen && lastInput < serial;
    st.listSerialSeen = serial;
    std::vector<const GiChangedRegion *> boxes;
    const bool all = missed || (fresh && regions.available && (regions.overflow != 0u || regions.restart));
    if (fresh && regions.available)
        for (const GiChangedRegion &r : regions.regions)
            if (r.kind == GiRegionKind::Mover) boxes.push_back(&r);
    const bool listed = all || !boxes.empty();
    if (lightKey != st.lightKey) st.age = 0u;   // a light added, removed or moved: the sums are another set's
    st.lightKey = lightKey;
    const unsigned n = std::max(strata, 1u);
    st.strata = n;
    st.restFrames = still ? std::min(st.restFrames + 1u, 1u << 20) : 0u;
    if (st.restFrames == 0u) st.restListAt = 0u;
    if (st.restFrames > 0u && listed) st.restListAt = st.restFrames;
    // THE HOLD: past n^2 rest frames the rest mean is every stratum once - nothing is dispatched
    st.held = still && !listed && st.restFrames > st.restListAt + n * n && !freshImages;
    if (st.held) return true;

    std::memcpy(pp.camPos, eye.camPos, sizeof(pp.camPos));
    std::memcpy(pp.rayTL, eye.rayTL, sizeof(pp.rayTL));
    std::memcpy(pp.rayRight, eye.rayRight, sizeof(pp.rayRight));
    std::memcpy(pp.rayDown, eye.rayDown, sizeof(pp.rayDown));
    std::memcpy(pp.fwd, eye.fwd, sizeof(pp.fwd));
    std::memcpy(pp.prevCamPos, st.age ? st.prev[0] : eye.camPos, sizeof(pp.prevCamPos));
    std::memcpy(pp.prevRayTL, st.age ? st.prev[1] : eye.rayTL, sizeof(pp.prevRayTL));
    std::memcpy(pp.prevRayRight, st.age ? st.prev[2] : eye.rayRight, sizeof(pp.prevRayRight));
    std::memcpy(pp.prevRayDown, st.age ? st.prev[3] : eye.rayDown, sizeof(pp.prevRayDown));
    std::memcpy(pp.prevFwd, st.age ? st.prev[4] : eye.fwd, sizeof(pp.prevFwd));
    const Ogre::Vector2 projAB = cam->getProjectionParamsAB();
    pp.projParams[0] = projAB.x;
    pp.projParams[1] = projAB.y;
    pp.projParams[2] = cam->getFarClipDistance();
    put3(pp.viewAxisX, q * Ogre::Vector3::UNIT_X, 0.0f);
    put3(pp.viewAxisY, q * Ogre::Vector3::UNIT_Y, 0.0f);
    put3(pp.viewAxisZ, -(q * Ogre::Vector3::NEGATIVE_UNIT_Z), 0.0f);
    pp.resolution[0] = float(w);
    pp.resolution[1] = float(h);
    pp.resolution[2] = float(fullW);
    pp.resolution[3] = float(fullH);
    // THE CLEARANCE'S TERMS, the producer's (jah_area_vis_law.glsl's JAH_AREA_LIFT): the ray tier's drawn footprints and
    // the depth-rebuilt point's sub-pixel snap (the ray origin's one law, jah_ray_origin.glsl); the no-ray route's 0 and
    // the voxel march's cell
    pp.knobs[0] = liftA;
    pp.knobs[1] = liftB;
    pp.knobs[2] = mask;
    pp.knobs[3] = float(kAreaVisDivisor);
    pp.strata[0] = float(n);
    // the stratum sequence: the view's frame, and at rest the rest frame's own index (every stratum once by n^2)
    pp.strata[1] = float(st.restFrames ? st.restFrames - 1u : (st.frame & 0xFFFFu));
    pp.strata[2] = 1.0f / float(n * n);
    pp.strata[3] = float(std::min(st.age, 65535u));
    pp.rest[0] = float(st.restFrames);
    pp.rest[1] = float(n * n);
    // (the boxes, merged to fit: box i takes every listed box j = i mod kAreaVisRegions - never one dropped)
    const unsigned nb = unsigned(std::min<size_t>(boxes.size(), kAreaVisRegions));
    for (size_t j = 0; j < boxes.size(); ++j) {
        const unsigned i = unsigned(j % kAreaVisRegions);
        const Vec3 &lo = boxes[j]->lo, &hi = boxes[j]->hi;
        const float l[3] = { lo.x, lo.y, lo.z }, u[3] = { hi.x, hi.y, hi.z };
        for (int c = 0; c < 3; ++c) {
            pp.regionBox[2 * i][c] = j < kAreaVisRegions ? l[c] : std::min(pp.regionBox[2 * i][c], l[c]);
            pp.regionBox[2 * i + 1][c] = j < kAreaVisRegions ? u[c] : std::max(pp.regionBox[2 * i + 1][c], u[c]);
        }
    }
    pp.regions[0] = float(nb);
    pp.regions[1] = all ? 1.0f : 0.0f;
    return false;
}

void areaVisAdvance(AreaVisState &st, const CameraImageF &eye) {
    const float *now[5] = { eye.camPos, eye.rayTL, eye.rayRight, eye.rayDown, eye.fwd };
    for (int k = 0; k < 5; ++k) std::memcpy(st.prev[k], now[k], sizeof(st.prev[k]));
    st.flip ^= 1u;
    ++st.age;
    ++st.frame;
}

// ---------------------------------------------------------------------------
// THE NO-RAY ROUTE'S HALF (the view's screen-GI listener)
// ---------------------------------------------------------------------------
namespace {
Ogre::TextureGpu *makeAreaImage(Ogre::TextureGpuManager *tm, unsigned w, unsigned h, Ogre::PixelFormatGpu fmt) {
    static unsigned sSerial = 0u;
    Ogre::TextureGpu *t = tm->createTexture("JahScreenAreaVis/" + std::to_string(++sSerial),
                                            Ogre::GpuPageOutStrategy::Discard, Ogre::TextureFlags::Uav,
                                            Ogre::TextureTypes::Type2D);
    t->setResolution(w, h, 1u);
    t->setPixelFormat(fmt);
    t->setNumMipmaps(1u);
    t->_transitionTo(Ogre::GpuResidency::Resident, nullptr);
    return t;
}
void bindTexture(Ogre::HlmsComputeJob *job, Ogre::uint8 unit, Ogre::TextureGpu *tex,
                 const Ogre::HlmsSamplerblock *sampler = nullptr) {
    Ogre::DescriptorSetTexture2::TextureSlot slot(Ogre::DescriptorSetTexture2::TextureSlot::makeEmpty());
    slot.texture = tex;
    if (sampler)
        job->setTexture(unit, slot, sampler);
    else
        job->setTexture(unit, slot, nullptr, false);
}
void bindUav(Ogre::HlmsComputeJob *job, Ogre::uint8 unit, Ogre::TextureGpu *tex, Ogre::ResourceAccess::ResourceAccess access) {
    Ogre::DescriptorSetUav::TextureSlot slot(Ogre::DescriptorSetUav::TextureSlot::makeEmpty());
    slot.texture = tex;
    slot.access = access;
    slot.pixelFormat = tex->getPixelFormat();
    job->_setUavTexture(unit, slot);
}
void bindBuffer(Ogre::HlmsComputeJob *job, Ogre::uint8 unit, Ogre::UavBufferPacked *buffer) {
    Ogre::DescriptorSetUav::BufferSlot buf(Ogre::DescriptorSetUav::BufferSlot::makeEmpty());
    buf.buffer = buffer;
    buf.offset = 0;
    buf.sizeBytes = 0;
    buf.access = Ogre::ResourceAccess::Read;
    job->_setUavBuffer(unit, buf);
}
}   // namespace

void ScreenGiListener::dropAreaVis() {
    Ogre::RenderSystem *rs = Ogre::Root::getSingleton().getRenderSystem();
    Ogre::TextureGpuManager *tm = rs ? rs->getTextureGpuManager() : nullptr;
    for (Ogre::AsyncTextureTicket *&t : mAreaTickets) {
        if (t && tm) tm->destroyAsyncTextureTicket(t);
        t = nullptr;
    }
    for (bool &live : mAreaTicketLive) live = false;
    Ogre::TextureGpu **images[] = { &mAreaSU[0], &mAreaSU[1], &mAreaS2U2[0], &mAreaS2U2[1], &mAreaG[0], &mAreaG[1],
                                    &mAreaRestSU, &mAreaRestS2U2, &mAreaVis };
    for (Ogre::TextureGpu **t : images) {
        if (*t && tm) tm->destroyTexture(*t);
        *t = nullptr;
    }
    mAreaW = mAreaH = mAreaFullW = mAreaFullH = 0u;
    mAreaVramBytes = 0ull;
    mAreaVisWritten = mAreaResolved = false;
    mAreaState = AreaVisState();
    mAreaVisHost.clear();
    mAreaVisHostW = mAreaVisHostH = 0u;
}

void ScreenGiListener::bindAreaVis(Ogre::CompositorPassCompute *pass) {
    Ogre::HlmsCompute *hc = Ogre::Root::getSingleton().getHlmsManager()->getComputeHlms();
    Ogre::HlmsComputeJob *job = hc ? hc->findComputeJobNoThrow(Ogre::IdString(kScreenAreaVisJob)) : nullptr;
    if (!job) return;
    // THE FRAME CAPTURE'S PASS (OgreView::ensureCaptureWorkspace: the view's chain a second time, the same frame, camera
    // and size, after the view's own workspace): it TAKES the view's product of this frame and keeps no history of its
    // own. Its trace ran on the view's history - a second rest frame on that frame (another stratum, the pair flipped
    // twice) and its resolve wrote the view's product - so the live picture after a screenshot was not the one it would
    // have been (gi.screen_bind (2)); at another size it remade the view's images, the history restarted.
    if (!ownPass(pass)) {
        standDown(job);
        return;
    }
    mAreaDispatched = false;
    mAreaMarches = 0ull;
    Ogre::RenderSystem *rs = Ogre::Root::getSingleton().getRenderSystem();
    Ogre::VaoManager *vao = rs ? rs->getVaoManager() : nullptr;
    Ogre::TextureGpuManager *tm = rs ? rs->getTextureGpuManager() : nullptr;
    OgreScene *scene = mView->ogreScene();
    Ogre::Camera *cam = mView->camera();
    const Ogre::CompositorNode *node = pass ? pass->getParentNode() : nullptr;
    Ogre::TextureGpu *depth = node ? node->getDefinedTexture(Ogre::IdString(kSceneDepthTexture)) : nullptr;
    Ogre::TextureGpu *normals = node ? node->getDefinedTexture(Ogre::IdString("jahGBufNormals")) : nullptr;
    Ogre::TextureGpu *rough = node ? node->getDefinedTexture(Ogre::IdString("jahSsrShadowRough")) : nullptr;
    if (bindMissing() || !vao || !tm || !scene || !cam || !depth || !normals || !rough) {
        // (the history untouched, and the opaque passes read the last product as the integrate's: a frame without its
        // inputs is no frame of unshadowed area lights - a flash)
        standDown(job);
        return;
    }
    mAreaRan = false;
    mAreaRead = false;
    // THE FORWARD LTC SET - none: OFF IS FREE AND CLEAN (the images given back, nothing registered)
    const std::vector<const Ogre::Light *> lights = forwardLtcLights(scene->sceneManager());
    if (lights.empty()) {
        if (mAreaVis) dropAreaVis();
        mAreaReason.clear();
        standDown(job);
        return;
    }
    // MONO ONLY (the ray tier's rule): a two-eye target is never traced as one image - its area lights unshadowed
    if (mView->stereo()) {
        mAreaReason = "a stereo view (mono only: stage 9's stereo item)";
        standDown(job);
        return;
    }
    const unsigned fullW = depth->getWidth(), fullH = depth->getHeight();
    if (!fullW || !fullH) { standDown(job); return; }
    const unsigned w = (fullW + kAreaVisDivisor - 1u) / kAreaVisDivisor, h = (fullH + kAreaVisDivisor - 1u) / kAreaVisDivisor;
    // ---- THE IMAGES, remade on a resize (a new set: the history restarts, age 0) ----
    if (!mAreaVis || mAreaW != w || mAreaH != h || mAreaFullW != fullW || mAreaFullH != fullH) {
        dropAreaVis();
        for (unsigned k = 0; k < 2u; ++k) {
            mAreaSU[k] = makeAreaImage(tm, w, h, Ogre::PFG_RGBA32_UINT);
            mAreaS2U2[k] = makeAreaImage(tm, w, h, Ogre::PFG_RGBA32_UINT);
            mAreaG[k] = makeAreaImage(tm, w, h, Ogre::PFG_RGBA32_UINT);
        }
        mAreaRestSU = makeAreaImage(tm, w, h, Ogre::PFG_RGBA32_UINT);
        mAreaRestS2U2 = makeAreaImage(tm, w, h, Ogre::PFG_RGBA32_UINT);
        mAreaVis = makeAreaImage(tm, fullW, fullH, Ogre::PFG_RG32_UINT);
        mAreaW = w;
        mAreaH = h;
        mAreaFullW = fullW;
        mAreaFullH = fullH;
        mAreaFresh = true;
        mAreaVisWritten = false;
        // the sums' pairs and the surface words' pair (6 x 16 B), the rest means (2 x 16 B), the product (8 B a pixel)
        mAreaVramBytes = (unsigned long long)w * h * (6u * 16u + 2u * 16u) + (unsigned long long)fullW * fullH * 8u;
    }
    Ogre::PhotonVoxelLighting *vct = scene->photon().voxelLighting();
    const bool chain = vct && vct->getVoxelizer() && vct->getLightVoxelTextures() && vct->getLightVoxelTextures()[0];
    const unsigned numCascades = chain ? std::min<unsigned>(vct->getChainLength(), 8u) : 0u;
    // ---- THE FRAME: the lights, the camera, the rest (both producers' one text) ----
    // THE CLEARANCE: the voxel march's own start bias - one cell of cascade 0 along the normal (the cones'
    // jahConeStart): the start is past its own surface's voxel planes, whose stored position is the cell's
    const float cell0 = chain ? float(vct->getVoxelizer()->getVoxelCellSize().x) : 0.0f;
    AreaVisParams pp{};
    const unsigned n = giQualityFacts(scene->giParams().quality).cardAreaStrata;
    // (the history as it stands: a trace whose dispatch does not run this frame gives the frame back, below)
    const AreaVisState before = mAreaState;
    const CameraImageF eyeBefore = mAreaEye;
    // (no hold before a resolve has written the product: the trace's history can be complete while the resolve's
    // permutation still builds, F2)
    const bool held = areaVisFrame(mAreaState, pp, lights, cam, mView->chainDesc(), w, h, fullW, fullH, n,
                                   scene->photon(), mAreaFresh || !mAreaVisWritten, 0.0f,
                                   std::max(cell0, 0.001f), 0.0f, mAreaEye);
    mAreaRan = true;
    mAreaRead = true;
    mAreaReason.clear();
    if (held) { standDown(job); return; }

    // ---- THE PARAMETERS and THE CHAIN the march runs through ----
    if (!mAreaParams)
        mAreaParams = vao->createUavBuffer(sizeof(AreaVisParams) / 16u, 16u, 0, &pp, false);
    else
        mAreaParams->upload(&pp, 0, sizeof(AreaVisParams) / 16u);
    {
        float c[4u * kAreaChainVec4] = {};
        if (chain) {
            float origin[4u * 8u] = {}, res[4u * 8u] = {};
            vct->chainMarchParams(origin, res);
            std::memcpy(c, origin, sizeof(origin));
            std::memcpy(c + 32u, res, sizeof(res));
            const Ogre::Vector3 corner = vct->getVoxelizer()->getVoxelOrigin();
            const Ogre::Vector3 r = vct->getVoxelizer()->getVoxelResolution();
            put3(c + 64u, corner, 0.0f);
            put3(c + 68u, Ogre::Vector3(1.0f / r.x, 1.0f / r.y, 1.0f / r.z), 0.0f);
            c[72] = float(numCascades);
        }
        if (!mAreaChain)
            mAreaChain = vao->createUavBuffer(kAreaChainVec4, 16u, 0, c, false);
        else
            mAreaChain->upload(c, 0, kAreaChainVec4);
    }
    // ---- THE PERMUTATION and THE BINDINGS ----
    job->setStoodDown(false);
    if (job->getProperty("hlms_num_vct_cascades") != Ogre::int32(numCascades))
        job->setProperty("hlms_num_vct_cascades", Ogre::int32(numCascades));
    const Ogre::uint8 units = Ogre::uint8(6u + 4u * numCascades);
    if (job->getNumTexUnits() != units) job->setNumTexUnits(units);
    coldKey(job);
    const unsigned cur = mAreaState.flip & 1u, prev = cur ^ 1u;
    bindTexture(job, 0u, depth);
    bindTexture(job, 1u, normals);
    bindTexture(job, 2u, rough);
    bindTexture(job, 3u, mAreaSU[prev]);
    bindTexture(job, 4u, mAreaS2U2[prev]);
    bindTexture(job, 5u, mAreaG[prev]);
    // the chain's coverage and surface position per half (the voxel textures' own entries, as the integrate binds them)
    for (unsigned j = 0; j < numCascades; ++j) {
        Ogre::TextureGpu *const *vox = vct->getLightVoxelTextures(j);
        for (unsigned k = 0; k < 4u; ++k) {
            const unsigned half = k & 1u;
            Ogre::TextureGpu *t = vox[k >= 2u ? vct->positionIndex(half) : vct->coverageIndex(half)];
            bindTexture(job, Ogre::uint8(6u + k * numCascades + j), t,
                        k == 0u && j == 0u ? vct->getBindTrilinearSamplerblock() : nullptr);
        }
    }
    bindUav(job, 0u, mAreaSU[cur], Ogre::ResourceAccess::Write);
    bindUav(job, 1u, mAreaS2U2[cur], Ogre::ResourceAccess::Write);
    bindUav(job, 2u, mAreaG[cur], Ogre::ResourceAccess::Write);
    bindUav(job, 3u, mAreaRestSU, Ogre::ResourceAccess::ReadWrite);
    bindUav(job, 4u, mAreaRestS2U2, Ogre::ResourceAccess::ReadWrite);
    bindBuffer(job, 5u, mAreaParams);
    bindBuffer(job, 6u, mAreaChain);
    // ---- WHETHER ITS DISPATCH RUNS THIS FRAME (the read's F2; asked of the bound job, the permutation a dispatch
    // would select): a trace deferred while its permutation builds in the background traces nothing - the history
    // neither ages nor flips for it (the frame given back: the rest count, the stratum, the camera), and the opaque
    // passes read the last product a resolve wrote (none: nothing registered). Before, the host aged the history over
    // deferred frames and could reach the hold with the product never written.
    if (!dispatchRuns(pass, job)) {
        mAreaState = before;
        mAreaEye = eyeBefore;
        mAreaRan = false;
        mAreaRead = mAreaVisWritten;
        mAreaReason = "its shaders build in the background";
        standDown(job);
        return;
    }
    mAreaDispatched = true;
    mAreaMarches = (unsigned long long)w * h * mAreaState.lights;
}

void ScreenGiListener::bindAreaVisResolve(Ogre::CompositorPassCompute *pass) {
    Ogre::HlmsCompute *hc = Ogre::Root::getSingleton().getHlmsManager()->getComputeHlms();
    Ogre::HlmsComputeJob *job = hc ? hc->findComputeJobNoThrow(Ogre::IdString(kScreenAreaVisResolveJob)) : nullptr;
    if (!job) return;
    const Ogre::CompositorNode *node = pass ? pass->getParentNode() : nullptr;
    Ogre::TextureGpu *depth = node ? node->getDefinedTexture(Ogre::IdString(kSceneDepthTexture)) : nullptr;
    Ogre::TextureGpu *normals = node ? node->getDefinedTexture(Ogre::IdString("jahGBufNormals")) : nullptr;
    mAreaResolved = false;
    // (the capture's pass, a trace that stood down, or its inputs missing: nothing to resolve)
    if (!ownPass(pass) || !mAreaDispatched || bindMissing() || !depth || !normals) {
        standDown(job);
        return;
    }
    job->setStoodDown(false);
    if (job->getNumTexUnits() != 5u) job->setNumTexUnits(5u);
    coldKey(job);
    bindTexture(job, 0u, depth);
    bindTexture(job, 1u, normals);
    const unsigned cur = mAreaState.flip & 1u;
    bindTexture(job, 2u, mAreaSU[cur]);
    bindTexture(job, 3u, mAreaS2U2[cur]);
    bindTexture(job, 4u, mAreaG[cur]);
    bindUav(job, 0u, mAreaVis, Ogre::ResourceAccess::Write);
    bindBuffer(job, 1u, mAreaParams);
    // (deferred while its permutation builds: the product not written this frame, F2)
    mAreaResolved = dispatchRuns(pass, job);
}

void ScreenGiListener::afterAreaVisResolve(const Ogre::CompositorPass *pass) {
    if (!ownPass(pass)) return;   // the capture's: it advanced nothing
    if (mAreaDispatched) {
        areaVisAdvance(mAreaState, mAreaEye);
        mAreaFresh = false;
    }
    if (mAreaResolved) mAreaVisWritten = true;
    // ---- THE READBACK (a test door, the arm area.visReadback): a ring of tickets, decoded once a transfer is done ----
    const bool readback = mView->mEngine && mView->mEngine->armRegistry().on(ArmId::AreaVisReadback);
    Ogre::RenderSystem *rs = Ogre::Root::getSingleton().getRenderSystem();
    Ogre::TextureGpuManager *tm = rs ? rs->getTextureGpuManager() : nullptr;
    if (!readback || !mAreaVis || !mAreaVisWritten || !tm) return;
    for (unsigned k = 0; k < kAreaReadbacks; ++k) {
        Ogre::AsyncTextureTicket *t = mAreaTickets[k];
        if (!t || !mAreaTicketLive[k] || !t->queryIsTransferDone()) continue;
        const Ogre::TextureBox box = t->map(0);
        mAreaVisHost.resize(size_t(box.width) * box.height * 8u);
        for (unsigned y = 0; y < box.height; ++y)
            std::memcpy(&mAreaVisHost[size_t(y) * box.width * 8u], box.at(0, y, 0), size_t(box.width) * 8u);
        t->unmap();
        mAreaVisHostW = box.width;
        mAreaVisHostH = box.height;
        mAreaTicketLive[k] = false;
    }
    const unsigned slot = mAreaTicketNext;
    if (mAreaTicketLive[slot]) return;   // every slot in flight: a later frame takes one
    Ogre::AsyncTextureTicket *&t = mAreaTickets[slot];
    if (t && (t->getWidth() != mAreaVis->getWidth() || t->getHeight() != mAreaVis->getHeight())) {
        tm->destroyAsyncTextureTicket(t);
        t = nullptr;
    }
    if (!t)
        t = tm->createAsyncTextureTicket(mAreaVis->getWidth(), mAreaVis->getHeight(), 1u, Ogre::TextureTypes::Type2D,
                                         mAreaVis->getPixelFormat());
    t->download(mAreaVis, 0, false);
    mAreaTicketLive[slot] = true;
    mAreaTicketNext = (slot + 1u) % kAreaReadbacks;
}

void ScreenGiListener::areaVisStatsInto(const OgreScene *scene, AreaVisStatus &st) const {
    if (!mView || mView->ogreScene() != scene) return;
    if (!mAreaRan) {
        if (st.reason.empty()) st.reason = mAreaReason;
        return;
    }
    st.running = true;
    st.lights = mAreaState.lights;
    st.strata = mAreaState.strata;
    st.traceW = mAreaW;
    st.traceH = mAreaH;
    st.targetW = mAreaFullW;
    st.targetH = mAreaFullH;
    st.restFrames = mAreaState.restFrames;
    st.held = mAreaState.held;
    st.rays += mAreaMarches;
    st.vramBytes += mAreaVramBytes;
    // (its GPU time is its two passes' own timestamp pairs - the monitor's FramePass rows "Jahshaka screen area
    // visibility" and "... resolve" - not a CacheWork row: traceMs / resolveMs stay unmeasured here)
    if (!mAreaVisHost.empty()) {
        st.vis = mAreaVisHost;
        st.visW = mAreaVisHostW;
        st.visH = mAreaVisHostH;
    }
    st.reason.clear();
}

// THE SCENE'S ONE STATUS (ScenePublished::areaVis): the ray tier's views, then the no-ray route's - a scene's views take
// one producer or the other (the chain's: ChainDesc::screenIntegrate on a chain that does not gather).
AreaVisStatus OgreScene::areaVisStatus() const {
    AreaVisStatus st;
    if (!mEngine) return st;
    rayTierAreaVisStatsInto(mEngine, this, st);
    for (const OgreView *v : mEngine->viewsOf(this))
        if (v->screenGiListener()) v->screenGiListener()->areaVisStatsInto(this, st);
    return st;
}

}}}   // namespace jahshaka::engine::detail
