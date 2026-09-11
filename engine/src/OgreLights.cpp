// Photometric (IES) light profiles and area-light mask textures.
//
// One more Ogre-private translation unit behind EnginePrivate.h. Both features
// are process-wide singletons (see the header comment for WHY), armed lazily on
// first use and torn down by OgreEngine's destructor before `delete Root`.
#include "EnginePrivate.h"

#include <LightProfiles/OgreLightProfiles.h>

#include <chrono>
#include <map>
#include <set>

namespace jahshaka { namespace engine { namespace detail {
namespace lightextras {

namespace {

// ---- IES profiles ---------------------------------------------------------
Ogre::LightProfiles *gProfiles = nullptr;
/// Resource-location directories already registered in the "Jahshaka" group.
std::set<std::string> gProfileDirs;
/// Absolute path -> the resource NAME (the bare file name) it was loaded under.
/// LightProfiles keys its own map by that name alone, so two different files
/// that happen to share a name would silently resolve to the first one loaded.
/// CAS object files are named by content hash, so this cannot happen in the
/// product; we detect it and refuse rather than render the wrong lobe.
std::map<std::string, std::string> gProfilePaths;
/// The reverse index, for exactly that collision check.
std::map<std::string, std::string> gProfileNames;

// ---- Area-light masks -----------------------------------------------------
Ogre::TextureGpu *gMaskPool = nullptr;
/// Absolute path -> the pooled texture holding it. Textures live for the
/// process: masks are small, the pool is fixed, and a light may be re-assigned
/// the same mask any frame.
std::map<std::string, Ogre::TextureGpu *> gMasks;
bool gAreaBudgetsRaised = false;
/// Has loadLtcMatrix run against the LIVE Root? Reset by shutdown() — see
/// armLtcMatrix's header comment for why this cannot be a function-local static.
bool gLtcLoaded = false;

Ogre::HlmsPbs *pbsOf(Ogre::Root *root) {
    if (!root || !root->getHlmsManager()) return nullptr;
    return static_cast<Ogre::HlmsPbs *>(root->getHlmsManager()->getHlms(Ogre::HLMS_PBS));
}

void splitPath(const std::string &path, std::string &dir, std::string &file) {
    const size_t slash = path.find_last_of("/\\");
    dir  = slash == std::string::npos ? std::string(".") : path.substr(0, slash);
    file = slash == std::string::npos ? path : path.substr(slash + 1);
}

const char *kGroup = "Jahshaka";

Ogre::ResourceGroupManager &rgm() { return Ogre::ResourceGroupManager::getSingleton(); }

}  // namespace

// ---------------------------------------------------------------------------
bool assignProfile(Ogre::Root *root, Ogre::Light *light, const std::string &path,
                   std::string &error) {
    if (!light) { error = "assignProfile: no light"; return false; }
    if (path.empty()) {
        // Nothing was ever armed => every light already reads atlas row 0 (white).
        if (!gProfiles) return true;
        JAH_TRY { gProfiles->assignProfile(Ogre::IdString(), light); return true; }
        JAH_CATCH(error, false);
    }

    Ogre::HlmsPbs *pbs = pbsOf(root);
    if (!pbs) { error = "assignProfile: HlmsPbs is not registered yet"; return false; }

    JAH_TRY {
        std::string dir, file;
        splitPath(path, dir, file);

        auto known = gProfilePaths.find(path);
        if (known == gProfilePaths.end()) {
            auto clash = gProfileNames.find(file);
            if (clash != gProfileNames.end() && clash->second != path) {
                error = "light profile '" + file + "' is already loaded from a different file (" +
                        clash->second + "); Ogre's profile registry is keyed by file NAME, so "
                        "loading both would silently apply the first one's lobe";
                return false;
            }

            if (!rgm().resourceGroupExists(kGroup)) rgm().createResourceGroup(kGroup, false);
            if (!gProfileDirs.count(dir)) {
                rgm().addResourceLocation(dir, "FileSystem", kGroup, false);
                gProfileDirs.insert(dir);
            }
            if (!rgm().resourceExists(kGroup, file)) {
                error = "light profile not found: " + path;
                return false;
            }

            // Arm on FIRST profile only: the atlas is bound in every pass.
            if (!gProfiles) {
                gProfiles = new Ogre::LightProfiles(
                    pbs, root->getRenderSystem()->getTextureGpuManager());
            }
            gProfiles->loadIesProfile(file, kGroup, /*throwOnDuplicate*/ false);
            // build() recreates and re-uploads the atlas texture. It runs ONLY
            // here — on the frame a genuinely new profile first appears — never
            // on the per-frame setLight path.
            gProfiles->build();
            gProfilePaths[path] = file;
            gProfileNames[file] = path;
        }

        gProfiles->assignProfile(Ogre::IdString(gProfilePaths[path]), light);
        return true;
    } JAH_CATCH(error, false);
}

// ---------------------------------------------------------------------------
void armAreaLightBudgets(Ogre::Root *root) {
    if (gAreaBudgetsRaised) return;
    Ogre::HlmsPbs *pbs = pbsOf(root);
    if (!pbs) return;
    gAreaBudgetsRaised = true;
    try {
        pbs->setAreaLightForwardSettings(kAreaApproxLimit, kAreaLtcLimit);
    } catch (...) {}
}

// ---------------------------------------------------------------------------
void armLtcMatrix(Ogre::Root *root) {
    if (gLtcLoaded) return;
    Ogre::HlmsPbs *pbs = pbsOf(root);
    if (!pbs) return;
    gLtcLoaded = true;   // even a failed attempt: don't retry every frame
    try {
        pbs->loadLtcMatrix();
    } catch (...) {}
}

// ---------------------------------------------------------------------------
bool assignAreaMask(Ogre::Root *root, Ogre::Light *light, const std::string &path,
                    std::string &error) {
    if (!light) { error = "assignAreaMask: no light"; return false; }
    if (path.empty()) {
        if (light->getTexture()) light->setTexture(nullptr);
        return true;
    }

    Ogre::HlmsPbs *pbs = pbsOf(root);
    if (!pbs) { error = "assignAreaMask: HlmsPbs is not registered yet"; return false; }

    JAH_TRY {
        auto cached = gMasks.find(path);
        if (cached == gMasks.end()) {
            Ogre::TextureGpuManager *tm = root->getRenderSystem()->getTextureGpuManager();

            std::string dir, file;
            splitPath(path, dir, file);
            if (!rgm().resourceGroupExists(kGroup)) rgm().createResourceGroup(kGroup, false);
            if (!gProfileDirs.count(dir)) {
                rgm().addResourceLocation(dir, "FileSystem", kGroup, false);
                gProfileDirs.insert(dir);
            }
            if (!rgm().resourceExists(kGroup, file)) {
                error = "area light mask not found: " + path;
                return false;
            }

            // Arm on the FIRST mask only (a bound array costs a slot per pass).
            if (!gMaskPool) {
                const Ogre::uint8 mips = Ogre::PixelFormatGpuUtils::getMaxMipmapCount(
                    kMaskResolution, kMaskResolution);
                gMaskPool = tm->reservePoolId(kMaskPoolId, kMaskResolution, kMaskResolution,
                                              kMaskSlices, mips, kMaskFormat);
                pbs->setAreaLightMasks(gMaskPool);
                armAreaLightBudgets(root);
            }
            if (gMasks.size() >= kMaskSlices) {
                error = "area light mask pool is full (" + std::to_string(kMaskSlices) +
                        " distinct masks per process)";
                return false;
            }

            // Decode on the CPU (load2 sniffs content when the extension lies —
            // the same tolerance loadTexture needs), then force the image into
            // the pool's EXACT shape. A mismatched image would land in a
            // different pool and the light would sample the wrong texture.
            Ogre::Image2 *img = new Ogre::Image2();
            {
                Ogre::DataStreamPtr stream = rgm().openResource(file, kGroup);
                img->load2(stream, file);
            }
            if (img->getWidth() != kMaskResolution || img->getHeight() != kMaskResolution)
                img->resize(kMaskResolution, kMaskResolution, Ogre::Image2::FILTER_BILINEAR);
            // Pool-wide RGBA_sRGB: convert whatever came in (grayscale gobos are
            // common) so the slice's texels mean what the shader thinks.
            if (img->getPixelFormat() != kMaskFormat) {
                Ogre::Image2 *rgba = new Ogre::Image2();
                rgba->createEmptyImage(kMaskResolution, kMaskResolution, 1u,
                                       Ogre::TextureTypes::Type2D, kMaskFormat, 1u);
                const bool oneChannel =
                    Ogre::PixelFormatGpuUtils::getNumberOfComponents(img->getPixelFormat()) == 1;
                for (Ogre::uint32 y = 0; y < kMaskResolution; ++y)
                    for (Ogre::uint32 x = 0; x < kMaskResolution; ++x) {
                        Ogre::ColourValue c = img->getColourAt(x, y, 0);
                        if (oneChannel) { c.g = c.b = c.r; }
                        c.a = 1.0f;
                        rgba->setColourAt(c, x, y, 0);
                    }
                delete img;
                img = rgba;
            }
            // A FULL mip chain is mandatory, not an optimisation: the diffuse
            // term samples near the smallest mip (mTexLightMaskDiffuseMipStart
            // defaults to 0.95 of the chain) and the specular term samples by
            // roughness. Without mips the mask reads hard and wrong.
            img->generateMipmaps(/*gammaCorrected*/ true, Ogre::Image2::FILTER_BILINEAR);

            Ogre::TextureGpu *tex = tm->createTexture(
                processUniqueName("areamask"), Ogre::GpuPageOutStrategy::Discard,
                Ogre::TextureFlags::AutomaticBatching, Ogre::TextureTypes::Type2D,
                Ogre::BLANKSTRING, 0, kMaskPoolId);
            tex->setResolution(kMaskResolution, kMaskResolution);
            tex->setPixelFormat(kMaskFormat);
            tex->setNumMipmaps(img->getNumMipmaps());
            tex->scheduleTransitionTo(Ogre::GpuResidency::Resident, img, true);   // deletes img
            tex->waitForData();

            cached = gMasks.emplace(path, tex).first;
        }

        // setTexture reads getInternalSliceStart(), so the texture must already
        // be resident (it is: waitForData above). Slice bookkeeping afterwards
        // is automatic through Light::notifyTextureChanged.
        light->setTexture(cached->second);
        return true;
    } JAH_CATCH(error, false);
}

// ---------------------------------------------------------------------------
void shutdown() {
    // Order matters: the profiles object unbinds itself from HlmsPbs as it
    // destroys its atlas, and every texture must die before its manager.
    delete gProfiles;
    gProfiles = nullptr;
    gProfileDirs.clear();
    gProfilePaths.clear();
    gProfileNames.clear();

    if (gMaskPool) {
        try {
            Ogre::HlmsPbs *pbs = nullptr;
            if (Ogre::Root::getSingletonPtr() && Ogre::Root::getSingleton().getHlmsManager())
                pbs = static_cast<Ogre::HlmsPbs *>(
                    Ogre::Root::getSingleton().getHlmsManager()->getHlms(Ogre::HLMS_PBS));
            if (pbs) pbs->setAreaLightMasks(nullptr);
            Ogre::TextureGpuManager *tm = gMaskPool->getTextureManager();
            for (auto &kv : gMasks) tm->destroyTexture(kv.second);
            tm->destroyTexture(gMaskPool);
        } catch (...) {}
    }
    gMasks.clear();
    gMaskPool = nullptr;
    gAreaBudgetsRaised = false;
    // The LTC matrix belongs to the Root's HlmsPbs, which is about to die: the
    // next Engine must load it again or its area lights render unlit.
    gLtcLoaded = false;
}

}  // namespace lightextras

// ---------------------------------------------------------------------------
// The shadow-atlas demand (SHADOW_TOOLING_SPEC.md §4.1)
// ---------------------------------------------------------------------------
// Counted from the BACKEND lights rather than from a mirrored description, so
// it says what the renderer will actually be asked for. Two exclusions, both
// structural: a directional light rides the PSSM block (it never competes for a
// focused map) and an area light can never cast at all.
//
// One trap, recorded rather than worked around: while a shadow node sorts its
// lights it temporarily flips `setCastShadows(false)` on every light fixed to a
// static map and restores it afterwards (OgreCompositorShadowNode.cpp:512-519).
// Reading getCastShadows() between those two lines would under-count. This runs
// from renderOneFrame BEFORE Root::renderOneFrame, which is outside that window.
unsigned OgreScene::countLocalShadowCasters(std::vector<NodeId> *out) const {
    unsigned n = 0;
    // OVER THE LIGHT INDEX, NOT OVER mNodes: this runs once per drawn scene per
    // FRAME, and walking every node of a big scene to find its handful of
    // lights is exactly the kind of per-frame cost this engine keeps measuring
    // and removing.
    for (NodeId id : mLightNodes) {
        auto it = mNodes.find(id);
        if (it == mNodes.end()) continue;
        const Ogre::Light *l = it->second.light;
        if (!l || !l->getCastShadows()) continue;
        const Ogre::Light::LightTypes t = l->getType();
        if (t != Ogre::Light::LT_POINT && t != Ogre::Light::LT_SPOTLIGHT) continue;
        ++n;
        if (out) out->push_back(id);
    }
    return n;
}

// ---------------------------------------------------------------------------
// THE LAMP-MAP CACHE, detection half (ENGINE_CACHE_POLICY_SPEC P2/P3)
// ---------------------------------------------------------------------------
// What this replaced, and why it could not stay: a light's map was either
// DYNAMIC (re-rendered every frame — six cube faces plus a copy per point lamp,
// in the view, in the planar mirror and on every face of every probe capture)
// or opt-in STATIC, dirtied by the host whenever ANY transform in the document
// changed (the mirror watched a process-wide transform-write counter, so the
// editor camera moving re-rendered every "static" map). Now every point/spot
// map is cached and the engine decides, per light, from what it can see
// itself — after the scene graph has updated, so in the frame it matters.

namespace {

/// A light's REACH as a world box: what its shadow camera can contain. A
/// point light's map is six 90-degree faces out to its range; a spot's is one
/// perspective camera at 1.2x the outer cone (OgreShadowCameraSetup.cpp's
/// DefaultShadowCameraSetup, capped at 175 degrees), also out to its range —
/// the far plane is the light's range because we never set a shadow far clip
/// (Light::_deriveShadowFarClipDistance). Conservative for spots: the box of
/// the apex and the far cap, clipped to the range sphere's box.
Ogre::Aabb lampReach(const Ogre::Light *l) {
    const Ogre::Vector3 p = l->getParentNode()->_getDerivedPosition();
    const Ogre::Real r = std::max(l->getAttenuationRange(), Ogre::Real(1e-3));
    const Ogre::Aabb sphere(p, Ogre::Vector3(r, r, r));
    if (l->getType() != Ogre::Light::LT_SPOTLIGHT) return sphere;
    const Ogre::Vector3 d = l->getDerivedDirection().normalisedCopy();
    const Ogre::Real halfFov =
        std::min(l->getSpotlightOuterAngle().valueRadians() * Ogre::Real(0.6),
                 Ogre::Degree(87.5f).valueRadians());
    const Ogre::Real capR = std::min(r * std::tan(halfFov), r * Ogre::Real(64));
    const Ogre::Vector3 c = p + d * r;
    // The half extents of a disc of radius capR with normal d.
    const Ogre::Vector3 e(capR * std::sqrt(std::max(Ogre::Real(0), 1 - d.x * d.x)),
                          capR * std::sqrt(std::max(Ogre::Real(0), 1 - d.y * d.y)),
                          capR * std::sqrt(std::max(Ogre::Real(0), 1 - d.z * d.z)));
    Ogre::Vector3 mn = p, mx = p;
    mn.makeFloor(c - e); mx.makeCeil(c + e);
    mn.makeCeil(p - Ogre::Vector3(r, r, r)); mx.makeFloor(p + Ogre::Vector3(r, r, r));
    return Ogre::Aabb::newFromExtents(mn, mx);
}

bool boxesTouch(const Ogre::Aabb &a, const Ogre::Aabb &b) {
    const Ogre::Vector3 d = a.mCenter - b.mCenter;
    const Ogre::Vector3 h = a.mHalfSize + b.mHalfSize;
    return std::abs(d.x) <= h.x && std::abs(d.y) <= h.y && std::abs(d.z) <= h.z;
}

/// For a POINT lamp the sphere is the exact reach; the box test above only
/// pre-filters.
bool sphereTouchesBox(const Ogre::Vector3 &c, Ogre::Real r, const Ogre::Aabb &b) {
    const Ogre::Vector3 d = c - b.mCenter;
    const Ogre::Vector3 q(std::max(std::abs(d.x) - b.mHalfSize.x, Ogre::Real(0)),
                          std::max(std::abs(d.y) - b.mHalfSize.y, Ogre::Real(0)),
                          std::max(std::abs(d.z) - b.mHalfSize.z, Ogre::Real(0)));
    return q.squaredLength() <= r * r;
}

/// The key of everything a lamp's map depends on that setLight does not see:
/// its world pose (a point map does not depend on orientation — its six faces
/// are world-aligned, OgreCompositorShadowNode.cpp:577-591 — a spot's does).
unsigned long long lampPoseKey(const Ogre::Light *l, unsigned long long paramKey) {
    const Ogre::Node *n = l->getParentNode();
    const Ogre::Vector3 p = n->_getDerivedPosition();
    const Ogre::Quaternion q = l->getType() == Ogre::Light::LT_SPOTLIGHT
                                   ? n->_getDerivedOrientation() : Ogre::Quaternion::IDENTITY;
    unsigned long long h = paramKey ^ 1469598103934665603ull;
    const float f[7] = { p.x, p.y, p.z, q.x, q.y, q.z, q.w };
    for (float v : f) {
        const long long k = (long long)std::llround(double(v) * 65536.0);
        h ^= (unsigned long long)k;
        h *= 1099511628211ull;
    }
    return h;
}

bool cacheableLamp(const Ogre::Light *l) {
    // getVisible(), the LAYER_VISIBILITY flag our hide writes — NOT isVisible(),
    // which also tests Root's *current* scene manager's combined visibility
    // mask, i.e. whatever happened to render last (a probe capture, another
    // scene's view): measured, it dropped every lamp of a GI scene.
    if (!l || !l->getCastShadows() || !l->getVisible()) return false;
    const Ogre::Light::LightTypes t = l->getType();
    return t == Ogre::Light::LT_POINT || t == Ogre::Light::LT_SPOTLIGHT;
}

}   // namespace

void OgreScene::shadowWorkspaces(ShadowNodeKind kind,
                                 std::vector<Ogre::CompositorWorkspace *> &out) const {
    if (kind == ShadowNodeKind::Reflect) {
        // Only while the reflect pass names the node at all (rebuildPlanar).
        if (!mPlanar || !mPlanarParams.shadows) return;
        for (size_t i = 0; i < mPlanar->slotCount(); ++i)
            if (Ogre::CompositorWorkspace *ws = mPlanar->slotWorkspace(i)) out.push_back(ws);
    } else if (kind == ShadowNodeKind::Probe) {
        if (!mPcc || !mPccShadowed) return;
        for (const Ogre::CubemapProbe *p : mPcc->getProbes())
            if (Ogre::CompositorWorkspace *ws = p->getWorkspace()) out.push_back(ws);
    }
}

bool OgreScene::hasCacheableShadowLights() const {
    for (NodeId id : mLightNodes) {          // the light index, not every node
        auto it = mNodes.find(id);
        if (it != mNodes.end() && cacheableLamp(it->second.light)) return true;
    }
    return false;
}

void OgreScene::noteShadowShapeChanged(MaterialId mat) {
    for (auto &kv : mNodes)
        if (kv.second.materialRef == mat && kv.second.item) kv.second.shadowShapeDirty = true;
}

void OgreScene::noteNodePosed(NodeId id) {
    auto it = mNodes.find(id);
    if (it != mNodes.end()) ++it->second.poseEpoch;
}

void OgreScene::collectShadowCacheFrame(ShadowCacheFrame &out) {
    const auto t0 = std::chrono::steady_clock::now();
    struct Timer {
        std::chrono::steady_clock::time_point t0; double &dst;
        ~Timer() { dst = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count(); }
    } timer{ t0, mShadowScanMicros };
    out = ShadowCacheFrame();

    // ---- 1. The lamps, in slot order, and each lamp's own inputs ----------
    std::vector<std::pair<NodeId, Ogre::Light *>> points, spots;
    for (NodeId id : mLightNodes) {
        auto it = mNodes.find(id);
        if (it == mNodes.end() || !cacheableLamp(it->second.light)) continue;
        (it->second.light->getType() == Ogre::Light::LT_POINT ? points : spots)
            .emplace_back(id, it->second.light);
    }
    const auto byId = [](const std::pair<NodeId, Ogre::Light *> &a,
                         const std::pair<NodeId, Ogre::Light *> &b) { return a.first < b.first; };
    std::sort(points.begin(), points.end(), byId);
    std::sort(spots.begin(), spots.end(), byId);
    for (const auto &pl : points) out.lights.push_back({ pl.first, pl.second });
    for (const auto &sl : spots)  out.lights.push_back({ sl.first, sl.second });

    if (mShadowDirtyAll) { out.dirtyAll = true; mShadowDirtyAll = false; }
    if (out.lights.empty()) {
        // Nothing cached here. When a lamp arrives, its fresh slot assignment
        // renders it, and the next walk re-primes the caster records silently.
        mShadowLightKeys.clear();
        mShadowScanPrimed = false;
        mShadowVanished.clear();
        return;
    }

    // Dedupe per kind with a flag per lamp (the lists are single digits).
    std::vector<unsigned char> marked(out.lights.size() * kShadowNodeKinds, 0u);
    const auto dirtyLamp = [&](size_t i, unsigned kindMask) {
        for (unsigned k = 0; k < kShadowNodeKinds; ++k) {
            if (!(kindMask & (1u << k)) || marked[i * kShadowNodeKinds + k]) continue;
            if (!shadowLampCachedFor(ShadowNodeKind(k), out.lights[i].light)) continue;
            marked[i * kShadowNodeKinds + k] = 1u;
            out.dirty[k].push_back(out.lights[i].light);
        }
    };
    const unsigned allKinds = (1u << kShadowNodeKinds) - 1u;

    std::vector<Ogre::Aabb> reach(out.lights.size());
    std::unordered_map<NodeId, unsigned long long> keys;
    keys.reserve(out.lights.size());
    for (size_t i = 0; i < out.lights.size(); ++i) {
        const Ogre::Light *l = out.lights[i].light;
        reach[i] = lampReach(l);
        const auto nit = mNodes.find(out.lights[i].id);
        const unsigned long long key = lampPoseKey(l, nit->second.lightShadowKey);
        keys.emplace(out.lights[i].id, key);
        auto old = mShadowLightKeys.find(out.lights[i].id);
        // A lamp seen for the first time needs nothing from here: its slot
        // assignment is new, and setLightFixedToShadowMap marks it dirty.
        if (old != mShadowLightKeys.end() && old->second != key) {
            dirtyLamp(i, allKinds);
            ++out.lightChanges;
        }
    }
    mShadowLightKeys.swap(keys);

    // ---- 2. The casters ---------------------------------------------------
    // The frame's item walk (runItemWalk, just before this) has produced them;
    // a caller that drives this directly without one gets its own walk. A
    // change is the box that must be re-rendered, tested against every lamp's
    // reach, per the kinds whose channels the caster renders into.
    if (!mShadowWalked) walkItems(false, true, false);
    mShadowWalked = false;              // consumed: the next frame walks again
    const std::vector<ShadowChange> &changes = mShadowChanges;
    out.casterChanges = unsigned(changes.size());
    for (const ShadowChange &c : changes) {
        unsigned kinds = 0u;
        for (unsigned k = 0; k < kShadowNodeKinds; ++k)
            if (c.channels & shadowCasterChannels(ShadowNodeKind(k))) kinds |= 1u << k;
        if (!kinds) continue;
        for (size_t i = 0; i < out.lights.size(); ++i) {
            if (!boxesTouch(reach[i], c.box)) continue;
            const Ogre::Light *l = out.lights[i].light;
            if (l->getType() == Ogre::Light::LT_POINT &&
                !sphereTouchesBox(l->getParentNode()->_getDerivedPosition(),
                                  l->getAttenuationRange(), c.box))
                continue;
            dirtyLamp(i, kinds);
        }
    }
}

}}}  // namespace jahshaka::engine::detail
