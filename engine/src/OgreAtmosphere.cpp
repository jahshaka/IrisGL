// THE PLANET'S ATMOSPHERE (SKY-ATMOSPHERE-1) — Atmosphere.h says what the
// component owns and why it has Ogre's AtmosphereComponent shape; Types.h
// (AtmosphereSky) states the model and its constants.
//
// THE FRAME. Nothing here runs per frame unless an input moved: `update()` is
// called once a frame per drawn scene, inside the frame and before the sky
// capture (OgreEngine::renderOneFrame), and on a still frame it compares four
// flags and returns. A dial change rebuilds all four tables (their dispatches
// are each a monitor row, named for the job); a sun change rebuilds the sky view
// and the aerial volume; the aerial scale, the fog and the sun's intensity are
// constants in a buffer and cost an upload.
#include "EnginePrivate.h"
#include "Atmosphere.h"

#include <cstring>

#include <OgreHlmsCompute.h>
#include <OgreHlmsPbs.h>
#include <Cubemaps/OgreParallaxCorrectedCubemapBase.h>
#include <OgreHlmsComputeJob.h>
#include <OgreShaderParams.h>
#include <OgreMaterialManager.h>
#include <OgreTechnique.h>
#include <OgrePass.h>
#include <OgreTextureUnitState.h>
#include <CommandBuffer/OgreCbShaderBuffer.h>
#include <CommandBuffer/OgreCommandBuffer.h>
#include <Vao/OgreConstBufferPacked.h>

#include <chrono>
#include <cmath>

namespace jahshaka { namespace engine { namespace detail {

namespace {

// THE CLEAR EARTH (Hillaire 2020, the reference implementation's defaults), per
// KILOMETRE. The Mie pair is scattering and EXTINCTION (absorption 0.444e-3 is
// their difference) — the brief's "sigma_a 4.40e-6" reads the extinction.
constexpr float kRayleighScatter[3] = { 5.802e-3f, 13.558e-3f, 33.1e-3f };
constexpr float kRayleighScaleHeightKm = 8.0f;
constexpr float kMieScatter = 3.996e-3f;
constexpr float kMieExtinction = 4.440e-3f;
constexpr float kMieScaleHeightKm = 1.2f;
constexpr float kMieG = 0.8f;
constexpr float kOzoneAbsorb[3] = { 0.650e-3f, 1.881e-3f, 0.085e-3f };

/// The const buffer HlmsPbs binds while the component is registered, in the
/// layout JahFog_piece_vs_piece_ps.any declares (`JahAtmoSettings`, instance
/// `atmoSettings` — upstream's fog block reads its first three floats by name).
struct AtmoSettingsGpu {
    float fogDensity = 0.0f;
    float fogBreakMinBrightness = 0.0f;   ///< upstream's packing: min x falloff
    float fogBreakFalloff = 0.0f;         ///< ...and -falloff
    float aerialScale = 1.0f;
    float skyE[4] = { 0.0f, 0.0f, 0.0f, 0.0f };     ///< rgb = the sun at the top of the air, w = 1 with the air on
    float sunDir[4] = { 0.0f, 1.0f, 0.0f, 0.0f };   ///< xyz = towards the sun
    float planet[4] = { 0.0f, 0.0f, 0.0f, 0.0f };   ///< bottom radius, top radius, observer radius, aerial far (km)
    /// THE HEIGHT FOG (SKY-DEFAULTS-1): density and falloff per metre (exp2),
    /// the base height and the start distance (m) — read only under the pass
    /// property jah_height_fog, which is set only while it is on.
    float heightFog[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    float heightFogColour[4] = { 0.0f, 0.0f, 0.0f, 0.0f };   ///< rgb linear, w unused
};

Ogre::TextureGpu *makeLut(Ogre::TextureGpuManager *tm, const std::string &name,
                          Ogre::TextureTypes::TextureTypes type, unsigned w, unsigned h,
                          unsigned d) {
    Ogre::TextureGpu *t = tm->createTexture(name, Ogre::GpuPageOutStrategy::Discard,
                                            Ogre::TextureFlags::Uav, type);
    t->setResolution(w, h, d);
    t->setPixelFormat(Ogre::PFG_RGBA16_FLOAT);
    t->setNumMipmaps(1u);
    // Resident for good (the probe gather's rule): these tables live as long as
    // the scene's atmosphere does.
    t->_transitionTo(Ogre::GpuResidency::Resident, nullptr);
    return t;
}

}  // namespace

JahAtmosphere::JahAtmosphere(Ogre::Root *root, Ogre::SceneManager *sm, Ogre::uint32 visibleBit)
    : mRoot(root), mSceneMgr(sm), mVisibleBit(visibleBit) {
    Ogre::VaoManager *vao = mRoot->getRenderSystem()->getVaoManager();
    mBuffer = vao->createConstBuffer(sizeof(AtmoSettingsGpu), Ogre::BT_DEFAULT, nullptr, false);
    createTextures();
    createQuad();
    createFogQuad();
}

JahAtmosphere::~JahAtmosphere() {
    // Unregistered first: a SceneManager must never hold a dead component.
    if (mSceneMgr && mSceneMgr->getAtmosphereRaw() == this) mSceneMgr->_setAtmosphere(nullptr);
    if (mQuad) { mSceneMgr->destroyRectangle2D(mQuad); mQuad = nullptr; }
    if (mQuadMaterial) {
        Ogre::MaterialManager::getSingleton().remove(mQuadMaterial);
        mQuadMaterial.reset();
    }
    if (mFogQuad) { mSceneMgr->destroyRectangle2D(mFogQuad); mFogQuad = nullptr; }
    if (mFogQuadMaterial) {
        Ogre::MaterialManager::getSingleton().remove(mFogQuadMaterial);
        mFogQuadMaterial.reset();
    }
    Ogre::RenderSystem *rs = mRoot->getRenderSystem();
    Ogre::TextureGpuManager *tm = rs->getTextureGpuManager();
    for (Ogre::TextureGpu **t : { &mTrans, &mMs, &mSkyView, &mAerial })
        if (*t) { tm->destroyTexture(*t); *t = nullptr; }
    if (mBuffer) { rs->getVaoManager()->destroyConstBuffer(mBuffer); mBuffer = nullptr; }
}

void JahAtmosphere::createTextures() {
    Ogre::TextureGpuManager *tm = mRoot->getRenderSystem()->getTextureGpuManager();
    const std::string id = Ogre::StringConverter::toString(mSceneMgr->getId());
    mTrans = makeLut(tm, "JahAtmo/Transmittance/" + id, Ogre::TextureTypes::Type2D, kTransW, kTransH, 1u);
    mMs = makeLut(tm, "JahAtmo/MultiScatter/" + id, Ogre::TextureTypes::Type2D, kMsSize, kMsSize, 1u);
    mSkyView = makeLut(tm, "JahAtmo/SkyView/" + id, Ogre::TextureTypes::Type2D, kSkyW, kSkyH, 1u);
    mAerial = makeLut(tm, "JahAtmo/Aerial/" + id, Ogre::TextureTypes::Type3D, kApW, kApH, kApD + 1u);
    // SAMPLEABLE FROM BIRTH (ASYNC-SHADERS-1, a validation defect on base): the
    // sky quad samples SkyView/Aerial in every draw of the scene, and a new
    // scene is drawn BEFORE its first frame's update() builds the tables — the
    // open/create slices' prime and shader warm-up renders. A UAV is born in
    // GENERAL, so those draws read an image in the wrong layout
    // (VUID-vkCmdDraw-None-09600 on every create-after-create). The tables are
    // handed to the samplers here once; update() rebuilds and hands them over
    // again before any frame the user sees.
    handOver();
}

// THE SKY QUAD — the sun disc's recipe and its three traps (DOCS/traps/ENGINE.md:
// update() after setGeometry, the identity flags OFF, the static AABB), the
// camera ray derived by the shared screen-ray vertex program (so the VR
// session's stereo swap recognises it by name, OgreVrSession syncStereoQuads).
void JahAtmosphere::createQuad() {
    Ogre::MaterialManager &mm = Ogre::MaterialManager::getSingleton();
    Ogre::MaterialPtr base = std::static_pointer_cast<Ogre::Material>(
        mm.load("Jahshaka/AtmosphereSky", Ogre::ResourceGroupManager::AUTODETECT_RESOURCE_GROUP_NAME));
    if (!base)
        OGRE_EXCEPT(Ogre::Exception::ERR_FILE_NOT_FOUND,
                    "the atmosphere's sky material Jahshaka/AtmosphereSky is not staged",
                    "JahAtmosphere::createQuad");
    // A per-SCENE clone: the constants below are this scene's sky.
    const Ogre::String name =
        "Jahshaka/AtmosphereSky" + Ogre::StringConverter::toString(mSceneMgr->getId());
    if (Ogre::MaterialPtr stale = mm.getByName(name)) mm.remove(stale);
    mQuadMaterial = base->clone(name);
    mQuadMaterial->load();

    mQuad = mSceneMgr->createRectangle2D(Ogre::SCENE_STATIC);
    mQuad->initialize(Ogre::BT_DEFAULT, Ogre::Rectangle2D::GeometryFlagQuad);
    mQuad->setGeometry(-Ogre::Vector2::UNIT_SCALE, Ogre::Vector2(2.0f));
    mQuad->update();
    mQuad->setUseIdentityView(false);
    mQuad->setUseIdentityProjection(false);
    // QUEUE 0, SUBGROUP 1: the sky's place (Ogre's own sky quad's, which this
    // scene then does not have) — after a VR eye's hidden-area mesh at subgroup
    // 0, before the cloud sheet at 2, inside the environment capture's range.
    mQuad->setRenderQueueGroup(0u);
    mQuad->setRenderQueueSubGroup(1u);
    mQuad->setCastShadows(false);
    mQuad->setVisibilityFlags(0u);   // hidden until the air is the sky
    mSceneMgr->getRootSceneNode(Ogre::SCENE_STATIC)->attachObject(mQuad);
    mSceneMgr->notifyStaticAabbDirty(mQuad);
    mQuad->setMaterial(mQuadMaterial);
    Ogre::Pass *pass = mQuadMaterial->getTechnique(0)->getPass(0);
    if (Ogre::TextureUnitState *tu = pass->getTextureUnitState("skyViewLut")) tu->setTexture(mSkyView);
}

// THE HEIGHT FOG'S SKY QUAD (SKY-DEFAULTS-1) — the sky quad's recipe (the same
// three traps), its own material, QUEUE 5 at subgroup 1: after the sky (0), the
// cloud sheet (0/2) and the sun disc (5/0), before any geometry (10), which
// covers it through the depth test exactly as it covers the sky and fogs
// itself in its own shader. Out of the sky capture's range (queue 0 and the
// one above it) and, on the sun disc's channel, out of every probe face.
void JahAtmosphere::createFogQuad() {
    Ogre::MaterialManager &mm = Ogre::MaterialManager::getSingleton();
    Ogre::MaterialPtr base = std::static_pointer_cast<Ogre::Material>(
        mm.load("Jahshaka/HeightFogSky", Ogre::ResourceGroupManager::AUTODETECT_RESOURCE_GROUP_NAME));
    if (!base)
        OGRE_EXCEPT(Ogre::Exception::ERR_FILE_NOT_FOUND,
                    "the height fog's sky material Jahshaka/HeightFogSky is not staged",
                    "JahAtmosphere::createFogQuad");
    const Ogre::String name =
        "Jahshaka/HeightFogSky" + Ogre::StringConverter::toString(mSceneMgr->getId());
    if (Ogre::MaterialPtr stale = mm.getByName(name)) mm.remove(stale);
    mFogQuadMaterial = base->clone(name);
    mFogQuadMaterial->load();

    mFogQuad = mSceneMgr->createRectangle2D(Ogre::SCENE_STATIC);
    mFogQuad->initialize(Ogre::BT_DEFAULT, Ogre::Rectangle2D::GeometryFlagQuad);
    mFogQuad->setGeometry(-Ogre::Vector2::UNIT_SCALE, Ogre::Vector2(2.0f));
    mFogQuad->update();
    mFogQuad->setUseIdentityView(false);
    mFogQuad->setUseIdentityProjection(false);
    mFogQuad->setRenderQueueGroup(5u);
    mFogQuad->setRenderQueueSubGroup(1u);
    mFogQuad->setCastShadows(false);
    mFogQuad->setVisibilityFlags(0u);   // hidden until the height fog is on
    mSceneMgr->getRootSceneNode(Ogre::SCENE_STATIC)->attachObject(mFogQuad);
    mSceneMgr->notifyStaticAabbDirty(mFogQuad);
    mFogQuad->setMaterial(mFogQuadMaterial);
}

void JahAtmosphere::setHeightFog(bool on, float density, float falloff, float baseHeight,
                                 float startDistance, const float rgb[3], Ogre::uint32 visibleBit) {
    const Ogre::Vector3 colour(rgb[0], rgb[1], rgb[2]);
    const float hf[4] = { std::max(0.0f, density), falloff, baseHeight, std::max(0.0f, startDistance) };
    const bool same = on == mHfOn && visibleBit == mHfBit && colour == mHfColour &&
                      hf[0] == mHf[0] && hf[1] == mHf[1] && hf[2] == mHf[2] && hf[3] == mHf[3];
    if (same) return;
    mHfOn = on;
    mHfBit = visibleBit;
    mHfColour = colour;
    for (int i = 0; i < 4; ++i) mHf[i] = hf[i];
    mDirtyBuffer = mDirtyFogQuad = true;
    if (mFogQuad) mFogQuad->setVisibilityFlags(on ? visibleBit : 0u);
}

void JahAtmosphere::pushFogQuadConstants() {
    if (!mFogQuadMaterial) return;
    Ogre::Pass *pass = mFogQuadMaterial->getTechnique(0)->getPass(0);
    Ogre::GpuProgramParametersSharedPtr ps = pass->getFragmentProgramParameters();
    ps->setNamedConstant("heightFog", Ogre::Vector4(mHf[0], mHf[1], mHf[2], mHf[3]));
    ps->setNamedConstant("heightFogColour",
                         Ogre::Vector4(mHfColour.x, mHfColour.y, mHfColour.z, kHeightFogSkyDistance));
}

// ---- the inputs -------------------------------------------------------------
void JahAtmosphere::setModel(const AtmosphereModel &m) {
    if (m == mModel) return;
    // THE ALBEDO IS NOT IN THE TRANSMITTANCE (the merge read's W3): the ground
    // is a boundary of the multiple scattering and the sky view, never of the
    // air's own optical depth.
    const bool species = m.rayleighScale != mModel.rayleighScale || m.mieScale != mModel.mieScale ||
                         m.ozone != mModel.ozone || m.planetRadiusKm != mModel.planetRadiusKm ||
                         m.atmosphereHeightKm != mModel.atmosphereHeightKm;
    mModel = m;
    ++mModelGeneration;
    if (species) mDirtyTrans = true;
    mDirtyTables = mDirtySun = mDirtyBuffer = mDirtyQuad = true;
}

void JahAtmosphere::setSun(const Ogre::Vector3 &toSun, bool hasSun) {
    // No sun direction: the zenith (AtmosphereSky's contract — whether the sky
    // is LIT is the illuminance's business, not this flag's).
    Ogre::Vector3 d = hasSun && toSun.squaredLength() > 1e-12f ? toSun.normalisedCopy()
                                                               : Ogre::Vector3::UNIT_Y;
    if (d == mToSun && hasSun == mHasSun) return;
    mToSun = d;
    mHasSun = hasSun;
    mDirtySun = mDirtyBuffer = mDirtyQuad = true;
}

void JahAtmosphere::setSunIlluminance(const Ogre::Vector3 &noon) {
    if (noon == mSunNoon) return;
    mSunNoon = noon;
    mDirtyBuffer = mDirtyQuad = true;
}

void JahAtmosphere::setAerialScale(float s) {
    s = std::max(0.0f, s);
    if (s == mAerialScale) return;
    mAerialScale = s;
    mDirtyBuffer = true;
}

bool JahAtmosphere::setObserverAltitude(float metres) {
    const float km = std::max(kMinObserverKm, metres * 0.001f);
    if (std::fabs(std::log2(km / mObserverKm)) <= kObserverBandOctaves) return false;
    mObserverKm = km;
    ++mObserverRebuilds;
    // The view-dependent tables (the transmittance and multiple-scattering
    // ones are functions of altitude already), the constants that name the
    // observer, and the sun's tint memo (the beam crosses less air from up here).
    ++mModelGeneration;
    mDirtySun = mDirtyBuffer = mDirtyQuad = true;
    return true;
}

bool JahAtmosphere::settleObserverAltitude(float metres) {
    const float km = std::max(kMinObserverKm, metres * 0.001f);
    if (km == mObserverKm) return false;
    mObserverKm = km;
    ++mObserverRebuilds;
    ++mModelGeneration;
    mDirtySun = mDirtyBuffer = mDirtyQuad = true;
    return true;
}

bool JahAtmosphere::beginEnvironmentObserver(float km) {
    km = std::max(kMinObserverKm, km);
    if (!mAirOn || km == mObserverKm) return false;
    mDrawnObserverKm = mObserverKm;
    mObserverKm = km;
    // A THROW HERE MUST NOT LEAVE THE CAPTURE'S ALTITUDE BEHIND (the caller undoes
    // only a begin that returned): the drawn observer goes back, and the tables are
    // marked for the next update to rebuild at it.
    try {
        rebuildObserverView();
    } catch (...) {
        restoreDrawnObserver();
        throw;
    }
    return true;
}

void JahAtmosphere::endEnvironmentObserver() {
    mObserverKm = mDrawnObserverKm;
    try {
        rebuildObserverView();
    } catch (...) {
        restoreDrawnObserver();
        throw;
    }
}

void JahAtmosphere::restoreDrawnObserver() {
    mObserverKm = mDrawnObserverKm;
    ++mModelGeneration;
    mDirtySun = mDirtyBuffer = mDirtyQuad = true;
}

void JahAtmosphere::rebuildObserverView() {
    runJob("Jahshaka/AtmoSkyView", mSkyView, mTrans, mMs, kSkyW / 8u, (kSkyH + 7u) / 8u);
    handOver();
    uploadSettings();
    pushQuadConstants();
}

void JahAtmosphere::setSkyBrightness(float b) {
    b = std::max(0.0f, b);
    if (b == mSkyBrightness) return;
    mSkyBrightness = b;
    mDirtyBuffer = mDirtyQuad = true;
}

void JahAtmosphere::setFogBlock(float density, float breakMin, float breakFalloff) {
    if (density == mFogDensity && breakMin == mFogBreakMin && breakFalloff == mFogBreakFalloff) return;
    mFogDensity = density;
    mFogBreakMin = breakMin;
    mFogBreakFalloff = breakFalloff;
    mDirtyBuffer = true;
}

void JahAtmosphere::setAirOn(bool on) {
    if (on == mAirOn) return;
    mAirOn = on;
    mDirtyBuffer = true;
    if (mQuad) mQuad->setVisibilityFlags(on ? mVisibleBit : 0u);
}

// ---- the CPU half of the model ------------------------------------------------
// THE TRANSMITTANCE TABLE'S INTEGRAL, on the CPU: the same medium
// (jah_atmosphere.glsl, jahAtmoMedium) along the same ray to the top of the air,
// with more steps than the table uses (the table is interpolated; this is not).
Ogre::Vector3 JahAtmosphere::transmittance(float rKm, float mu) const {
    const double Rb = mModel.planetRadiusKm, Rt = Rb + mModel.atmosphereHeightKm;
    const double r = std::max(double(rKm), Rb);
    mu = std::max(-1.0f, std::min(1.0f, mu));
    // distance to the top of the air
    const double b = r * mu;
    const double c = r * r - Rt * Rt;
    const double disc = b * b - c;
    const double tTop = disc > 0.0 ? std::max(0.0, -b + std::sqrt(disc)) : 0.0;
    const int kSteps = 256;
    const double dt = tTop / kSteps;
    const double sinT = std::sqrt(std::max(0.0, 1.0 - double(mu) * mu));
    double depth[3] = { 0.0, 0.0, 0.0 };
    for (int i = 0; i < kSteps; ++i) {
        const double t = (i + 0.5) * dt;
        const double px = sinT * t, py = r + double(mu) * t;
        const double h = std::max(0.0, std::sqrt(px * px + py * py) - Rb);
        const double dR = std::exp(-h / kRayleighScaleHeightKm);
        const double dM = std::exp(-h / kMieScaleHeightKm);
        const double dO = mModel.ozone ? std::max(0.0, 1.0 - std::fabs(h - 25.0) / 15.0) : 0.0;
        for (int k = 0; k < 3; ++k)
            depth[k] += (kRayleighScatter[k] * mModel.rayleighScale * dR +
                         kMieExtinction * mModel.mieScale * dM + kOzoneAbsorb[k] * dO) * dt;
    }
    return Ogre::Vector3(float(std::exp(-depth[0])), float(std::exp(-depth[1])),
                         float(std::exp(-depth[2])));
}

// The sun at the top of the air: the noon illuminance the host pushed, over
// the zenith transmittance at the observer, so a white sun at noon lights the
// ground exactly as white as the user's colour says (atmosphereSunTint's
// contract). Zero with a sun of zero illuminance (the host's night).
Ogre::Vector3 JahAtmosphere::topIlluminance() const {
    const Ogre::Vector3 tz = transmittance(observerRadiusKm(), 1.0f);
    return Ogre::Vector3(mSunNoon.x / std::max(tz.x, 1e-6f), mSunNoon.y / std::max(tz.y, 1e-6f),
                         mSunNoon.z / std::max(tz.z, 1e-6f));
}

// ---- the GPU half -----------------------------------------------------------
void JahAtmosphere::pushJobParams(Ogre::HlmsComputeJob *job) const {
    Ogre::ShaderParams &sp = job->getShaderParams("default");
    const float Rb = mModel.planetRadiusKm, Rt = Rb + mModel.atmosphereHeightKm;
    const auto set = [&sp](const char *name, const Ogre::Vector4 &v) {
        if (Ogre::ShaderParams::Param *p = sp.findParameter(name)) p->setManualValue(v);
    };
    set("atmoPlanet", Ogre::Vector4(Rb, Rt, Rb + mObserverKm, kApMaxKm));
    set("atmoRayleigh", Ogre::Vector4(kRayleighScatter[0] * mModel.rayleighScale,
                                      kRayleighScatter[1] * mModel.rayleighScale,
                                      kRayleighScatter[2] * mModel.rayleighScale, kRayleighScaleHeightKm));
    set("atmoMie", Ogre::Vector4(kMieScatter * mModel.mieScale, kMieExtinction * mModel.mieScale,
                                 kMieScaleHeightKm, kMieG));
    const float oz = mModel.ozone ? 1.0f : 0.0f;
    set("atmoOzone", Ogre::Vector4(kOzoneAbsorb[0] * oz, kOzoneAbsorb[1] * oz, kOzoneAbsorb[2] * oz, 0.0f));
    set("atmoGround", Ogre::Vector4(mModel.groundAlbedo, mModel.groundAlbedo, mModel.groundAlbedo, 0.0f));
    set("atmoSun", Ogre::Vector4(mToSun.x, mToSun.y, mToSun.z, 0.0f));
    sp.setDirty();
}

// One table job, the compositor's own compute discipline (bind, the job's
// barriers through Ogre's solver, dispatch), the bindings released after it
// (a job's descriptor sets hold raw pointers). Its GPU time is the monitor row
// named for the job (CacheKind::Atmosphere) — the only measure of a table's cost
// since lane TEST-1 deleted the wall-clock slope (measureAtmosphere).
bool JahAtmosphere::runJob(const char *name, Ogre::TextureGpu *target, Ogre::TextureGpu *in0,
                           Ogre::TextureGpu *in1, unsigned gx, unsigned gy) {
    Ogre::HlmsManager *hm = mRoot->getHlmsManager();
    Ogre::HlmsCompute *hc = hm ? hm->getComputeHlms() : nullptr;
    Ogre::HlmsComputeJob *job = hc ? hc->findComputeJobNoThrow(name) : nullptr;
    if (!job) {
        Ogre::LogManager::getSingleton().logMessage(
            std::string("Jahshaka atmosphere: compute job ") + name + " is not staged", Ogre::LML_CRITICAL);
        return false;
    }
    Ogre::RenderSystem *rs = mRoot->getRenderSystem();
    pushJobParams(job);
    Ogre::HlmsSamplerblock ref;
    ref.setFiltering(Ogre::TFO_BILINEAR);
    ref.setAddressingMode(Ogre::TAM_CLAMP);
    Ogre::TextureGpu *ins[2] = { in0, in1 };
    for (Ogre::uint8 i = 0; i < 2; ++i) {
        if (!ins[i]) continue;
        Ogre::DescriptorSetTexture2::TextureSlot slot(Ogre::DescriptorSetTexture2::TextureSlot::makeEmpty());
        slot.texture = ins[i];
        job->setTexture(i, slot, &ref);
    }
    Ogre::DescriptorSetUav::TextureSlot uav = Ogre::DescriptorSetUav::TextureSlot::makeEmpty();
    uav.texture = target;
    uav.access = Ogre::ResourceAccess::Write;
    job->_setUavTexture(0u, uav);
    job->setNumThreadGroups(gx, gy, 1u);
    rs->endRenderPassDescriptor();
    // THE COST, AS THE GPU SEES IT (the merge read's D1): a frame-monitor cache
    // row with its own timestamp pair (the only way a dispatch outside a
    // compositor pass reports GPU time; CacheWork::gpuMs). Nothing while the
    // monitor is off.
    monitor::CacheScope scope(CacheKind::Atmosphere, WorkReason::Sky, 0, name, rs);
    {
        Ogre::ResourceTransitionArray &rt = rs->getBarrierSolver().getNewResourceTransitionsArrayTmp();
        job->analyzeBarriers(rt);
        rs->executeResourceTransition(rt);
        hc->dispatch(job, nullptr, nullptr);
    }
    for (Ogre::uint8 i = 0; i < 2; ++i)
        if (ins[i]) job->setTexture(i, Ogre::DescriptorSetTexture2::TextureSlot::makeEmpty());
    job->_setUavTexture(0u, Ogre::DescriptorSetUav::TextureSlot::makeEmpty());
    return true;
}

// THE TABLES ARE HANDED TO THE SAMPLERS (the reflection cube's rule,
// ENVPROBE-LAYOUT-1): no compositor pass names them, so nothing else would
// move them out of the UAV layout the jobs left them in.
void JahAtmosphere::handOver() {
    Ogre::RenderSystem *rs = mRoot->getRenderSystem();
    rs->endCopyEncoder();
    Ogre::BarrierSolver &solver = rs->getBarrierSolver();
    Ogre::ResourceTransitionArray trans;
    for (Ogre::TextureGpu *t : { mSkyView, mAerial })
        solver.resolveTransition(trans, t, Ogre::ResourceLayout::Texture, Ogre::ResourceAccess::Read,
                                 Ogre::c_allGraphicStagesMask);
    rs->executeResourceTransition(trans);
}

void JahAtmosphere::uploadSettings() {
    float g[kSettingsFloats];
    settingsFloats(g);
    mBuffer->upload(g, 0u, sizeof(g));
}

// THE CONST BUFFER'S CONTENTS, ONCE: what HlmsPbs's passes read (uploadSettings)
// and what the ray jobs' fog along a reflection reads (OgreScene::fogAlong).
void JahAtmosphere::settingsFloats(float out[kSettingsFloats]) const {
    static_assert(sizeof(AtmoSettingsGpu) == kSettingsFloats * sizeof(float), "the settings block's size");
    AtmoSettingsGpu g{};
    // THE FOG BLOCK IN UPSTREAM'S PACKING (AtmosphereNpr::_update): the
    // breakthrough as min x falloff and -falloff, so the stock block's
    // arithmetic is unchanged under every sky that is not this one.
    g.fogDensity = mFogDensity;
    g.fogBreakMinBrightness = mFogBreakMin * mFogBreakFalloff;
    g.fogBreakFalloff = -mFogBreakFalloff;
    g.aerialScale = mAerialScale;
    const Ogre::Vector3 e = skyRadianceScale();   // scattered light only: the sky's brightness
    g.skyE[0] = e.x; g.skyE[1] = e.y; g.skyE[2] = e.z; g.skyE[3] = mAirOn ? 1.0f : 0.0f;
    g.sunDir[0] = mToSun.x; g.sunDir[1] = mToSun.y; g.sunDir[2] = mToSun.z; g.sunDir[3] = 0.0f;
    const float Rb = mModel.planetRadiusKm;
    g.planet[0] = Rb;
    g.planet[1] = Rb + mModel.atmosphereHeightKm;
    g.planet[2] = Rb + mObserverKm;
    g.planet[3] = kApMaxKm;
    for (int i = 0; i < 4; ++i) g.heightFog[i] = mHfOn ? mHf[i] : 0.0f;
    g.heightFogColour[0] = mHfColour.x; g.heightFogColour[1] = mHfColour.y;
    g.heightFogColour[2] = mHfColour.z; g.heightFogColour[3] = 0.0f;
    std::memcpy(out, &g, sizeof(g));
}

void JahAtmosphere::pushQuadConstants() {
    if (!mQuadMaterial) return;
    Ogre::Pass *pass = mQuadMaterial->getTechnique(0)->getPass(0);
    Ogre::GpuProgramParametersSharedPtr ps = pass->getFragmentProgramParameters();
    const float Rb = mModel.planetRadiusKm;
    ps->setNamedConstant("atmoPlanet", Ogre::Vector4(Rb, Rb + mModel.atmosphereHeightKm,
                                                     Rb + mObserverKm, kApMaxKm));
    ps->setNamedConstant("atmoSunDir", Ogre::Vector4(mToSun.x, mToSun.y, mToSun.z, 0.0f));
    const Ogre::Vector3 e = skyRadianceScale();
    ps->setNamedConstant("atmoSkyE", Ogre::Vector4(e.x, e.y, e.z, 0.0f));
}

void JahAtmosphere::update() {
    // The tables only while the air is the sky: a fog-only registration (the
    // World fog under another sky) reads none of them, and the flags wait.
    const bool trans = mDirtyTrans;
    const bool tables = mDirtyTables || trans;
    const bool sun = mDirtySun || tables;
    if (mAirOn && (tables || sun)) {
        if (trans) {
            if (runJob("Jahshaka/AtmoTransmittance", mTrans, nullptr, nullptr, kTransW / 8u, kTransH / 8u))
                ++mStatus.transmittanceBuilds;
        }
        if (tables) {
            if (runJob("Jahshaka/AtmoMultiScatter", mMs, mTrans, nullptr, kMsSize, kMsSize))
                ++mStatus.multiScatterBuilds;
        }
        if (runJob("Jahshaka/AtmoSkyView", mSkyView, mTrans, mMs, kSkyW / 8u, (kSkyH + 7u) / 8u))
            ++mStatus.skyViewBuilds;
        if (runJob("Jahshaka/AtmoAerial", mAerial, mTrans, mMs, kApW / 8u, kApH / 8u))
            ++mStatus.aerialBuilds;
        handOver();
        mDirtyTrans = mDirtyTables = mDirtySun = false;
    }
    if (mDirtyBuffer) { uploadSettings(); mDirtyBuffer = false; }
    if (mDirtyQuad) { pushQuadConstants(); mDirtyQuad = false; }
    if (mDirtyFogQuad) { pushFogQuadConstants(); mDirtyFogQuad = false; }
}

AtmosphereStatus JahAtmosphere::status() const {
    AtmosphereStatus s = mStatus;
    s.on = mAirOn;
    s.observerAltitudeM = mObserverKm * 1000.0f;
    s.observerRebuilds = mObserverRebuilds;
    const Ogre::Vector3 e = topIlluminance();
    s.topIlluminance[0] = e.x; s.topIlluminance[1] = e.y; s.topIlluminance[2] = e.z;
    return s;
}

// ---- Ogre::AtmosphereComponent -------------------------------------------------
// HlmsPbs asks for this on every colour pass while the component is registered
// (OgreHlmsPbs.cpp preparePassHash): `hlms_fog` switches upstream's fog block
// and our piece on, and the slot is where our const buffer lands. The tables
// themselves are pass textures claimed by FogHlmsListener (jah_atmo_*), which
// also decides whether the air is on for the pass.
Ogre::uint32 JahAtmosphere::preparePassHash(Ogre::Hlms *hlms, size_t constBufferSlot) {
    hlms->_setProperty(Ogre::Hlms::kNoTid, Ogre::HlmsBaseProp::Fog, 1);
    hlms->_setProperty(Ogre::Hlms::kNoTid, "jah_atmo_buf", Ogre::int32(constBufferSlot));
    // THE HEIGHT FOG'S VARIANT (SKY-DEFAULTS-1): its block is in the buffer
    // always; the code that reads it is in the shader only while it is on —
    // and NEVER in a cubemap probe's face (a PCC capture: HlmsPbs's own
    // isRendering() test, the one it withholds the probes' sampling by). The
    // probe faces photograph the sky without the fog quad (visibility 0x1), so
    // fogged geometry in them would sit against an unfogged sky, and the near
    // scene's indirect light would change with a medium that begins 100 m away.
    if (mHfOn) {
        bool probeFace = false;
        if (auto *pbs = dynamic_cast<Ogre::HlmsPbs *>(hlms))
            if (const Ogre::ParallaxCorrectedCubemapBase *pcc = pbs->getParallaxCorrectedCubemap())
                probeFace = pcc->isRendering();
        if (!probeFace) hlms->_setProperty(Ogre::Hlms::kNoTid, "jah_height_fog", 1);
    }
    return 1u;
}

Ogre::uint32 JahAtmosphere::bindConstBuffers(Ogre::CommandBuffer *commandBuffer, size_t slotIdx) {
    *commandBuffer->addCommand<Ogre::CbShaderBuffer>() = Ogre::CbShaderBuffer(
        Ogre::VertexShader, Ogre::uint16(slotIdx), mBuffer, 0, Ogre::uint32(mBuffer->getTotalSizeBytes()));
    *commandBuffer->addCommand<Ogre::CbShaderBuffer>() = Ogre::CbShaderBuffer(
        Ogre::PixelShader, Ogre::uint16(slotIdx), mBuffer, 0, Ogre::uint32(mBuffer->getTotalSizeBytes()));
    return 1u;
}

// Nothing per camera: the sky quad derives each camera's ray itself, and every
// constant here is the scene's, uploaded once per change in update().
void JahAtmosphere::_update(Ogre::SceneManager *, Ogre::Camera *) {}

}}}  // namespace jahshaka::engine::detail
