#pragma once
// Internal header of the Ogre-Next 4.0 backend.
//
// THE ONE-DIRECTORY RULE: engine/src/ is the only directory that includes Ogre.
// This header is private to it — it is never installed and must never be included
// from outside irisgl/engine/src. If Ogre appears anywhere else, the boundary has
// been breached.
//
// Verified behaviours this backend depends on (spikes/qt-ogre-next/FINDINGS.md and
// spikes/headless-vulkan/README.md):
//   * Vulkan supports MULTIPLE on-screen windows; GL3Plus does not (single mGlobalVao).
//   * v1 meshes render NOTHING on Vulkan — geometry is built as v2 buffers directly.
//   * Hlms shader templates are required at runtime, not optional sample data.
//   * A render target must exist BEFORE Hlms registration or any SceneManager;
//     for a purely offscreen engine a surfaceless "null" window satisfies this.
//   * Teardown order is load-bearing: workspaces -> scenes -> our MeshPtrs ->
//     MeshManager::removeAll -> Root. A MeshPtr outliving Root hits a dead VaoManager.
//   * No Ogre exception may escape: every virtual is wrapped and translated.
#include "jahshaka/engine/Engine.h"

#include <OgreRoot.h>
#include <OgreAbiUtils.h>
#include <OgreWindow.h>
#include <OgreCamera.h>
#include <OgreSceneManager.h>
#include <OgreItem.h>
#include <OgreMesh2.h>
#include <OgreMeshManager2.h>
#include <OgreSubMesh2.h>
#include <OgreArchiveManager.h>
#include <OgreHlmsManager.h>
#include <OgreHlmsListener.h>
#include <OgreHlmsPbs.h>
#include <OgreHlmsUnlit.h>
#include <OgreHlmsPbsDatablock.h>
#include <OgreHlmsUnlitDatablock.h>
#include <OgreTextureGpuManager.h>
#include <OgreTextureFilters.h>
#include <OgreStagingTexture.h>
#include <OgrePixelFormatGpuUtils.h>
#include <OgreImage2.h>
#include <OgreTextureGpu.h>
#include <OgreAsyncTextureTicket.h>
#include <OgreLogManager.h>
#include <OgreResourceGroupManager.h>
// v1 skeletons are a BUILD-TIME scaffold only (SkeletonDef has exactly one
// constructor and it takes a v1::Skeleton) — the prerequisites header is what
// lets this file name v1::SkeletonPtr / v1::OldBone without pulling v1 in.
#include <OgreControllerManager.h>
#include <OgrePrerequisites.h>
#include <OgreHlmsSamplerblock.h>
#include <OgreRectangle2D2.h>
#include <set>
#include <Compositor/OgreCompositorManager2.h>
#include <Compositor/OgreCompositorWorkspace.h>
#include <Compositor/OgreCompositorNodeDef.h>
#include <Compositor/OgreCompositorShadowNode.h>
#include <Compositor/OgreCompositorShadowNodeDef.h>
#include <OgreRenderSystemCapabilities.h>
#include <Vao/OgreVaoManager.h>
#include <Vao/OgreVertexArrayObject.h>
#include <ParticleSystem/OgreBillboardSet2.h>
#include <ParticleSystem/OgreParticleSystemManager2.h>
#include <ParticleSystem/OgreParticleSystem2.h>
#include <ParticleSystem/OgreEmitter2.h>
#include <ParticleSystem/OgreParticleAffector2.h>
#include <OgreParticleEmitter.h>
#include <OgreForwardPlusBase.h>
#include <OgreDecal.h>
#include <InstantRadiosity/OgreInstantRadiosity.h>
#include <Vct/OgreVctVoxelizer.h>
#include <Vct/OgreVctLighting.h>
#include <Cubemaps/OgreParallaxCorrectedCubemapAuto.h>
#include <Cubemaps/OgrePccPerPixelGridPlacement.h>
// Fog rides Ogre's Atmosphere component: we take its exponential fog + brightness
// breakthrough and leave its sky and its sun/ambient coupling alone (OgreFog.cpp).
#include <Atmosphere/OgreAtmosphereNpr.h>
#include <OgrePlanarReflections.h>
#include <Compositor/OgreCompositorWorkspaceListener.h>

// X11 is the only on-screen window path today (Ogre Vulkan/XCB). Other
// platforms build headless-only until they grow a native window backend
// (macOS: CAMetalLayer + VK_EXT_metal_surface — see DOCS/HANDOFF.md §7).
#ifdef __linux__
#    include <X11/Xlib.h>
#endif
#include <atomic>
#include <functional>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace jahshaka { namespace engine {
// The backend's own namespace: these types and helpers are shared between the
// TUs under engine/src and by nothing else (they used to live in one anonymous
// namespace, when the backend was a single translation unit).
namespace detail {

inline Ogre::Vector3     toOgre(const Vec3 &v)   { return Ogre::Vector3(v.x, v.y, v.z); }
inline Ogre::ColourValue toOgre(const Colour &c) { return Ogre::ColourValue(c.r, c.g, c.b, c.a); }

/// Names handed to Ogre must be unique for the life of the process (a destroyed
/// scene may be recreated under the same name while stale resources linger).
inline std::string processUniqueName(const char *prefix) {
    static std::atomic<unsigned> counter{0};
    return std::string(prefix) + "_" + std::to_string(++counter);
}

// Every backend virtual is wrapped: `JAH_TRY { ... } JAH_CATCH(errSink, failValue)`.
// Ogre throws Ogre::Exception; its own allocations may throw std::bad_alloc.
#define JAH_TRY try
#define JAH_CATCH(sink, ret)                                                          \
    catch (Ogre::Exception &e) { (sink) = e.getFullDescription(); return ret; }       \
    catch (std::exception &e)  { (sink) = std::string("engine: ") + e.what(); return ret; }

class OgreEngine;

// ---------------------------------------------------------------------------
// Visibility-flag bits (user bits; Ogre reserves the top two for layer state).
// Every object keeps kVisibleBit so default cameras/compositor masks (all ones)
// draw it. kGiGeometryBit marks items whose surfaces bounce light for GI: PBR
// items only — never the sky, unlit overlays, line meshes or billboards, which
// would otherwise occlude rays or raycast as garbage triangles (a non-indexed
// line VAO reads as a vertex triangle list). kGiLightBit marks exactly the one
// light Instant Radiosity traces from (InstantRadiosity::mLightMask).
// kNoReflectBit marks objects that must NOT appear inside a planar reflection —
// today exactly the reflector planes themselves (a mirror containing itself is
// a feedback artefact, and Ogre's own sample excludes them the same way).
//
// THE TRAP, and why this bit is INVERTED relative to the other three: Ogre's
// PlanarReflections sample uses `visibility_mask 0xfffffffe` and tags mirrors
// `setVisibilityFlags(1u)` — i.e. in the sample, bit 0 means "not in
// reflections". Bit 0 here is kVisibleBit and EVERY object carries it, so
// copying the sample's mask renders a perfectly empty reflection. Ours is a new
// bit with the opposite polarity: the reflective pass masks with
// ~kNoReflectBit, so an object is in reflections unless it says otherwise.
constexpr Ogre::uint32 kVisibleBit     = 1u;
constexpr Ogre::uint32 kGiGeometryBit  = 1u << 1;
constexpr Ogre::uint32 kGiLightBit     = 1u << 2;
constexpr Ogre::uint32 kNoReflectBit   = 1u << 3;

// Forward+ clustered decal budget PER CELL (DECALS_SPEC D5). Not a scene-wide
// cap: decals beyond this in one cluster cell are dropped farthest-first.
constexpr Ogre::uint32 kDecalsPerCell = 8u;

// ---------------------------------------------------------------------------
// Particles (PARTICLES_FX2_SPEC.md). The HARD per-definition quota ceiling for
// every scene: setHighestPossibleQuota is called with this at scene creation,
// before any definition or billboard set can initialise, and setParticleSystem
// clamps every request to it. The backend's own limit is 65535/4 = 16383
// (setHighestPossibleQuota, OgreParticleSystemManager2.cpp:812-817); 16000
// stays under it with room and costs 768 KiB of shared index buffer per scene,
// allocated lazily on the first particle draw.
constexpr Ogre::uint16 kMaxParticleQuota = 16000u;
// Quota BUCKETS. A definition's quota is frozen at init() — changing it needs a
// whole new definition, and definitions can never be destroyed — so requests
// round UP to one of these. Two emitters whose quotas land in the same bucket
// can trade definitions through the recycling pool.
constexpr unsigned kParticleQuotaBuckets[] = { 256u, 1024u, 4096u, 16000u };

// ---------------------------------------------------------------------------
// Render-queue policy (POST_CHAIN_SPEC.md §6). Ogre fixes the queue MODES in
// RenderQueue's constructor: [0,100) and [200,225) are v2 FAST, [100,200) and
// [225,256) are V1_FAST, 15 is PARTICLE_SYSTEM. Our v2 items can therefore only
// live in 0-99 and 200-224.
//   0     sky rectangle          (OgreSky)
//   10    normal items           (Ogre's default)
//   15    PFX2 billboards
//   200   refractive items       (reserved; phase 7)
//   210   on-top overlays        (gizmos, wires, selection outlines)
// The compositor chain renders [0, kRefractiveRenderQueue) in the opaque pass
// and [kOverlayRenderQueue, 255) in a second pass, so overlays stay out of the
// G-buffers, luminance averages and edge-detection passes the later phases add.
// (Overlays used to sit at 200; the move is pixel-neutral — both values are
// inside the single range the old one-pass workspace drew.)
constexpr Ogre::uint8 kRefractiveRenderQueue = 200;
constexpr Ogre::uint8 kOverlayRenderQueue    = 210;

// ---------------------------------------------------------------------------
// What shape of compositor chain a view wants. Phase 1 carries only what every
// view has always had; the effect switches (HDR + tonemap, bloom, SSAO, SMAA,
// SSR, refraction) become extra fields here and extra nodes in OgreChain.cpp,
// and nothing outside those two places has to learn about them.
struct ChainDesc {
    Colour   background;
    bool     shadows = false;   ///< instantiate the process-wide shadow node
    unsigned samples = 1u;      ///< achieved MSAA count of the view's target

    // ---- Effects (phases 3-7). Every one of them is OFF on offscreen views by
    //      construction: OgreView::chainDesc() clears them (POST_CHAIN_SPEC §7.3).
    bool  hdr = false;              ///< RGBA16F scene target + filmic tonemap
    float exposure = 0.0f;          ///< stops; the auto-exposure midpoint
    float exposureMin = -2.5f;
    float exposureMax = 2.5f;
    bool  bloom = false;            ///< rides the HDR node at ~zero marginal cost
    float bloomThreshold = 5.0f;    ///< bright-pass start, in the sample's units
    bool  ssao = false;
    float ssaoScale = 1.0f;         ///< AO buffer resolution factor (0.5 or 1.0)
    float ssaoPower = 1.5f;
    float ssaoRadius = 2.0f;
    int   smaaPreset = -1;          ///< -1 off, 0 Low, 1 Medium, 2 High, 3 Ultra
    int   ssr = 0;                  ///< 0 off, 1 half-res rays, 2 HQ
    bool  refractions = false;

    /// Does this description need anything beyond the passthrough graph?
    bool anyEffect() const;
    /// Do these two describe the same GRAPH? Parameters (exposure, AO power)
    /// are uniforms — changing one must never rebuild a workspace.
    static bool sameShape(const ChainDesc &a, const ChainDesc &b);
};

class OgreView;

/// The chain builder (OgreChain.cpp). Not a class: it has no state — the state
/// is the ChainDesc the view owns and the definition names it hands back.
namespace chain {
/// Creates the node definitions and the workspace definition `desc` describes,
/// under `workspaceDef`. EVERY node definition created is appended to
/// `nodeDefsOut`, in creation order: a multi-node chain whose owner cleans up
/// only one definition leaks the rest across view recreation.
void build(Ogre::CompositorManager2 *cm, const std::string &workspaceDef,
           const ChainDesc &desc, std::vector<std::string> &nodeDefsOut);
/// Removes the workspace definition and every node definition in `nodeDefs`
/// (which is cleared). Safe when nothing was built.
void destroy(Ogre::CompositorManager2 *cm, const std::string &workspaceDef,
             std::vector<std::string> &nodeDefs);
/// The name of the scene node definition for a workspace — the anchor later
/// phases (and the planar-reflection lane) need to find the main scene pass.
std::string sceneNodeDefName(const std::string &workspaceDef);

/// PSO precache for a scene (SHADER_CACHE_SPEC.md §5): builds a throwaway
/// warm-up workspace that mirrors `refNodeDef`'s scene passes, renders it once
/// into a 4x4 target, and tears it down. Every shader `sm` needs through
/// `camera` — including shadow casters — is generated and compiled by the time
/// this returns.
///
/// The pass type is Ogre's own PASS_WARM_UP and the node is built by
/// Ogre::WarmUpHelper::createFrom, which copies the reference node's scene and
/// shadow passes (their RQ ranges, visibility masks and shadow nodes) into
/// warm-up passes and marks the last one CollectAndTrigger. So this warms
/// exactly the passes the real view runs, and it stays correct automatically
/// when the chain shape changes — there is no second description of the chain
/// to keep in step.
///
/// Nothing is presented. Returns false if the reference node is missing or the
/// workspace could not be built; failure is never fatal (the shaders compile
/// later, exactly as they did before this existed).
bool warmUp(Ogre::Root *root, Ogre::SceneManager *sm, Ogre::Camera *camera,
            const std::string &refNodeDef, const std::string &baseName);

// ---- Effect parameters -----------------------------------------------------
// EVERY ONE OF THESE IS PROCESS-GLOBAL (POST_CHAIN_SPEC.md §7.4): Ogre's HDR,
// SSAO and SMAA helpers all write MaterialManager singletons. The rule the
// backend follows is "the primary on-screen view owns the globals" —
// OgreEngine::renderOneFrame pushes them from the first enabled on-screen view
// whose chain has effects, and every other view lives with that.
void initHdrMsaa(unsigned samples);
void setExposure(float exposure, float minAutoExposure, float maxAutoExposure);
void setBloomThreshold(float minThreshold, float fullColourThreshold);
void initSsao(Ogre::Root *root);
void destroySsao(Ogre::Root *root);
void updateSsao(Ogre::Camera *camera, unsigned aoWidth, unsigned aoHeight,
                float kernelRadius, float powerScale);
void initSmaa(Ogre::Root *root, int preset);
/// One call per frame from the primary view: recompiles what changed and pushes
/// every per-frame uniform the enabled effects need.
void applyGlobals(Ogre::Root *root, Ogre::Camera *camera, const ChainDesc &desc,
                  unsigned viewWidth, unsigned viewHeight);
}   // namespace chain

// ---------------------------------------------------------------------------
// The persistent shader cache (SHADER_CACHE_SPEC.md; impl in OgreShaderCache.cpp).
//
// Three layers Ogre already implements, behind ONE container we fingerprint and
// checksum ourselves:
//
//   pipeline.cache    RenderSystem::load/savePipelineCache  — the VkPipelineCache
//                     blob (driver ISA). Ogre validates it thoroughly on its own
//                     (vendor/device/driverVersion/pipelineCacheUUID + a payload
//                     hash), so this layer is the one we could almost trust.
//   microcode.cache   GpuProgramManager::load/saveMicrocodeCache — SPIR-V, keyed
//                     by a hash of the GENERATED shader source, so a stale hit is
//                     structurally impossible. But the file format has NO magic,
//                     NO version, NO checksum and NO bounds checking
//                     (OgreGpuProgramManager.cpp:368-398 reads a count and trusts
//                     it), and its bytes go straight to vkCreateShaderModule.
//                     Everything below about checksums exists for this sentence.
//   hlms.<n>.bin      HlmsDiskCache::saveTo/loadFrom — the Hlms-preprocessed
//                     shader source per permutation. Ogre version-stamps and
//                     template-hashes it correctly. NOTE it does NOT replay PSOs
//                     at our pin (the replay lines are commented out upstream,
//                     OgreHlmsDiskCache.cpp:424-455): this layer removes template
//                     parsing, not pipeline creation.
//
// THE RULES, in order of how much they cost to get wrong:
//   1. Never load stale. Any doubt -> delete the directory and start cold. A
//      cold start costs seconds; a bad SPIR-V blob costs a GPU hang.
//   2. WE checksum every file, because layer 2 does not.
//   3. Every write is atomic: *.tmp in the same directory, flushed, renamed.
//   4. One writer. A second process gets a READ-ONLY cache, never a failed run.
//   5. Load order is upstream's, not ours: pipeline cache -> setSaveMicrocodes
//      ToCache(true) then microcode -> Hlms disk caches. Placed inside
//      ensureHlms() after both registerHlms calls and before any shader can
//      compile, mirroring Samples/2.0/Common/src/GraphicsSystem.cpp:626-692.
//   6. Size cap with a generation reset — the microcode map never evicts.
//   7. A load failure is never fatal. Log, clear, continue cold.
class ShaderCache {
public:
    /// Resolves the directory and computes the fingerprint. No I/O beyond
    /// hashing the staged Hlms media tree. An empty `dir` leaves the cache off
    /// (but the compile COUNTERS still run — the startup progress display and
    /// the tests need them whether or not anything is persisted).
    void configure(const std::string &dir, const std::string &appBuildId,
                   const std::string &mediaDir);

    /// Starts counting compiles. Call as soon as the log exists (i.e. right
    /// after Root), long before any shader is built.
    void attachCounters();
    /// Removes the log listener. MUST run before Root is deleted.
    void detachCounters();

    /// The whole load, in upstream's mandated order. Call from ensureHlms()
    /// after registerHlms and before anything can compile a shader.
    void load(Ogre::Root *root);
    /// Writes every dirty layer. False = the write failed and the previous
    /// cache (if any) is untouched.
    bool save(Ogre::Root *root);
    /// True when something has been compiled since the last save — the
    /// burst-settle timer's condition, and what makes save() a cheap no-op.
    bool dirty(Ogre::Root *root) const;
    /// Deletes every file we wrote. The running process is unaffected.
    bool clear();

    ShaderCacheStats stats(Ogre::Root *root) const;
    void progress(unsigned &compiled, unsigned &fromCache, unsigned &expected) const;

    /// Both out of line: Counter is only defined in OgreShaderCache.cpp, and a
    /// unique_ptr member to an incomplete type needs its owner's special
    /// members compiled where the type IS complete.
    ShaderCache();
    ~ShaderCache();

private:
    struct Entry { std::string name; unsigned long long bytes; std::string hash; };

    bool  readManifest(std::vector<Entry> &filesOut) const;
    bool  writeManifest(const std::vector<Entry> &files) const;
    /// Reads `name`, checks it against the manifest entry, and returns the bytes.
    /// Empty on any mismatch — the caller then wipes.
    bool  readVerified(const Entry &e, std::vector<char> &out) const;
    void  wipe() const;
    bool  acquireLock();
    void  releaseLock();
    std::string path(const std::string &name) const;

    std::string mDir, mFingerprint, mMediaDir, mAppBuildId;
    bool        mEnabled = false;
    bool        mWriter = false;      ///< we hold the single-writer lock
    int         mLockFd = -1;
    unsigned    mExpectedShaders = 0; ///< from the manifest of the last saved run
    long long   mLastSavedUnixMs = 0;
    bool        mPipelineLoaded = false, mMicrocodeLoaded = false;
    unsigned    mHlmsLoaded = 0;
    /// Microcode-map size right after the load — the baseline compiledThisRun
    /// would use if we had no log listener. Kept for the dirty() shortcut.
    size_t      mMicrocodeAtLoad = 0;
    /// compiled+cached at the last successful write — the "nothing new" test
    /// that stops a clean quit writing the same bytes twice.
    unsigned    mSavedAtCompileCount = 0;
    /// Set by clear(): the next save writes even though nothing new compiled.
    bool        mForceSave = false;
    class Counter;
    std::unique_ptr<Counter> mCounter;
};

// ---------------------------------------------------------------------------
// Fog. The DISTANCE term is Ogre's: an AtmosphereNpr registered on the scene's
// SceneManager (OgreScene::mAtmosphere) sets hlms_fog and binds its own const
// buffer, and the stock HlmsPbs pixel shader does the exponential mix. What the
// component cannot give us is an AUTHORED colour (it computes a procedural sky
// one) or height fog, so those ride this listener's pass-buffer extension, read
// by media/Hlms/Jahshaka/JahFog_piece_vs_piece_ps.any in BOTH shader stages.
//
// Parameters are per-scene, keyed by SceneManager: the listener is global to
// HlmsPbs, but preparePassBuffer receives the SceneManager of the pass being
// built. The two float4s are ALWAYS appended, fog on or off, so the pass-buffer
// layout never changes size; the shader members simply do not exist when fog is
// off (no hlms_fog, no piece).
struct FogState {
    float r = 0.0f, g = 0.0f, b = 0.0f;
    float heightDensity = 0.0f;     ///< 0 = no height layer (shader skips the branch)
    float heightFalloff = 0.1f;
    float heightLevel   = 0.0f;
};

class FogHlmsListener final : public Ogre::HlmsListener {
public:
    Ogre::uint32 getPassBufferSize(const Ogre::CompositorShadowNode *, bool /*casterPass*/,
                                   bool, Ogre::SceneManager *) const override;
    float *preparePassBuffer(const Ogre::CompositorShadowNode *, bool, bool,
                             Ogre::SceneManager *sceneManager, float *passBufferPtr) override;

    /// The per-scene fog table. OgreScene::setFog registers, the scene teardown
    /// unregisters, preparePassBuffer looks up.
    static void     registerScene(const Ogre::SceneManager *sm, const FogState &p);
    static void     unregisterScene(const Ogre::SceneManager *sm);
    static FogState lookup(const Ogre::SceneManager *sm);

private:
    static std::map<const Ogre::SceneManager *, FogState> sFogState;   // render thread only
};
extern FogHlmsListener gFogListener;

// ---------------------------------------------------------------------------
// Planar reflections (PLANAR_REFLECTIONS_SPEC.md; impl in OgrePlanar.cpp).
//
// SHAPE, in one paragraph, because the moving parts are spread over three
// classes: OgreScene owns ONE Ogre::PlanarReflections (Ogre allows one per
// SceneManager) plus one actor per reflector node; OgreView owns the compositor
// listener that drives it (the call must ride the MAIN workspace's per-frame
// update, and only the view knows its own workspace and camera); OgreEngine owns
// the half-resolution shadow-node DEFINITION the reflective pass may reference
// and re-syncs the view listeners once a frame.
namespace planar {

/// The reflective pass renders render queues [0, kReflectLastRQ) — the same
/// opaque range the main chain draws, deliberately stopping before the
/// on-top overlay queue. Gizmos, selection outlines and wire helpers must
/// never appear inside a mirror; excluding them by RENDER QUEUE rather than by
/// visibility bit means an overlay does not have to remember to tag itself,
/// and it composes with the chain's own RQ policy (OgreChain.cpp).
constexpr Ogre::uint8 kReflectLastRQ = 199u;

/// Builds the private workspace definition the reflection cameras render
/// through, under `workspaceDef` (node definitions appended to `nodeDefsOut`).
/// Deliberately NOT a copy of Samples/.../PlanarReflections.compositor: that
/// script hard-codes a 2048x7168 shadow atlas that would fight
/// Engine::setShadowResolution, and copying sample scripts is how the
/// patches-only law gets broken by the back door.
void buildWorkspace(Ogre::CompositorManager2 *cm, const std::string &workspaceDef,
                    const PlanarReflectionParams &p, const std::string &shadowNodeName,
                    std::vector<std::string> &nodeDefsOut);
void destroyWorkspace(Ogre::CompositorManager2 *cm, const std::string &workspaceDef,
                      std::vector<std::string> &nodeDefs);

/// A reflection plane derived from a node's own geometry.
struct Plane {
    Ogre::Vector3    centre;        ///< world space
    Ogre::Vector2    halfSize;      ///< actor-local X/Y half extents
    Ogre::Quaternion orientation;   ///< zAxis() IS the plane normal
    Ogre::Vector3    localNormal;   ///< the mesh-local unit axis the normal came from
    Ogre::Vector3    localCentre;   ///< the mesh-local AABB centre
};

/// Derives the plane from `item`'s local bounds and `node`'s world transform.
/// Returns false (with `error` filled) when the mesh is not plate-like: the
/// thinnest extent must be at most `kPlateRatio` of the next thinnest, or the
/// 20-degree matching rule makes the result look broken rather than merely
/// approximate.
constexpr float kPlateRatio = 0.1f;
bool derivePlane(Ogre::SceneNode *node, const Ogre::Item *item, Plane &out,
                 std::string &error);

/// Drives one Ogre::PlanarReflections from one view's workspace.
///
/// It must be a per-VIEW object even though PlanarReflections is per-SCENE:
/// `update()` takes the camera being rendered, and it does not merely book-keep
/// — it synchronously runs every active reflection workspace inside the
/// callback. Matching on the view's own camera pointer is also what keeps the
/// callback from firing for the reflection passes themselves. (Ogre's sample
/// discriminates with the magic pass identifier 25001; we do not adopt magic
/// numbers into our programmatic chain.)
class WorkspaceListener final : public Ogre::CompositorWorkspaceListener {
public:
    /// Both may be re-pointed at any time: the scene's arm is rebuilt on every
    /// parameter change and the view's camera is recreated on every setScene.
    Ogre::PlanarReflections *mReflections = nullptr;
    Ogre::Camera            *mCamera      = nullptr;

    void workspacePreUpdate(Ogre::CompositorWorkspace *) override;
    void passEarlyPreExecute(Ogre::CompositorPass *pass) override;
};

}   // namespace planar

// ---------------------------------------------------------------------------
// Photometric (IES) light profiles and area-light mask textures — OgreLights.cpp.
//
// Both are PROCESS-WIDE by construction, not per-scene, and that is a decision
// rather than an accident:
//   * LightProfiles::build() writes HlmsPbs::setLightProfilesTexture AND
//     Root::_setLightProfilesInvHeight — there is exactly one of each per
//     process, so a per-scene registry would have scenes fighting over the
//     binding (the sVctBindingOwner shape in OgreGi.cpp, which we do not want
//     to repeat).
//   * HlmsPbs::setAreaLightMasks binds ONE 2D-array pool. Light::setTexture
//     stores only the pool SLICE index, so a mask that landed in a different
//     pool renders the WRONG texture with no error at all. One reserved pool,
//     one fixed resolution/format/mip count, everything resampled into it.
// Both cost a texture slot in EVERY pass once armed (mTexUnitSlotStart grows),
// which is why each arms lazily on first use — the loadLtcMatrix precedent.
namespace lightextras {

/// The mask pool's fixed shape. Every area-light mask is resampled to this,
/// because a mismatched image silently lands in a DIFFERENT pool and the light
/// then samples whatever happens to occupy its slice index in ours.
constexpr Ogre::uint32 kMaskPoolId     = 0x4A414831;   // 'JAH1'
constexpr Ogre::uint32 kMaskResolution = 512u;
constexpr Ogre::uint32 kMaskSlices     = 8u;
/// Colour masks (>2 components) so a mask can TINT the cast. Pool-wide and
/// one-time: hlms_lights_area_tex_colour is derived from the pool's format.
constexpr Ogre::PixelFormatGpu kMaskFormat = Ogre::PFG_RGBA8_UNORM_SRGB;
/// Forward+ area-light budgets. Ogre defaults to ONE of each and silently
/// drops the rest; the shader property IS the limit (not the live count), so
/// raising it costs one shader variant, not one per light added.
constexpr Ogre::uint16 kAreaApproxLimit = 4u;
constexpr Ogre::uint16 kAreaLtcLimit    = 4u;

/// Assigns the IES profile at `path` to `light` (empty path = unset). Loads and
/// atlas-builds the profile the first time a path is seen, and NEVER rebuilds
/// for a path already registered — `setLight` runs every frame, and build()
/// recreates + re-uploads a GPU texture.
/// Returns false and fills `error` on failure; the light keeps what it had.
bool assignProfile(Ogre::Root *root, Ogre::Light *light, const std::string &path,
                   std::string &error);

/// Binds the area-light mask at `path` to `light` (empty path = unbind). The
/// image is decoded, resampled to the pool's resolution, given a full mip chain
/// (the diffuse term samples a very low mip — no mips is a visibly wrong mask)
/// and uploaded into a pool slice, once per path.
/// Returns false and fills `error` on failure; the light keeps what it had.
bool assignAreaMask(Ogre::Root *root, Ogre::Light *light, const std::string &path,
                    std::string &error);

/// Raises HlmsPbs's forward area-light budgets off Ogre's default of 1. Called
/// where loadLtcMatrix is armed: the second area light of a scene renders
/// nothing without it.
void armAreaLightBudgets(Ogre::Root *root);

/// Destroys the profile atlas and the mask pool. MUST run before `delete Root`
/// (a TextureGpu outliving its manager is the usual teardown crash).
void shutdown();

}  // namespace lightextras

// ---------------------------------------------------------------------------
// Decal atlases (OgreDecals.cpp). PROCESS-WIDE, like the TextureGpuManager pools
// they wrap: one image loaded by two scenes costs one slice. resetDecalAtlases()
// MUST run when Ogre::Root dies, or the next Engine in the process inherits
// dangling TextureGpu pointers (test_engine_recreate creates a second one).
struct DecalAtlas;
DecalAtlas &decalAtlas(DecalMap kind);
void        resetDecalAtlases();
unsigned    decalAtlasCapacity();
bool        releaseDecalTexture(Ogre::TextureGpuManager *tm, DecalMap kind, Ogre::TextureGpu *tex);

// ---------------------------------------------------------------------------
class OgreScene final : public Scene {
public:
    OgreScene(Ogre::Root *root, Ogre::SceneManager *sm, const std::string &name,
              std::string &errorSink);
    ~OgreScene() override;

    const std::string &name() const override;

    void setAmbient(const Colour &upper, const Colour &lower) override;
    void setAmbientSh(const float sh[27]) override;

    void setFog(const FogDesc &desc) override;
    /// Creates the scene's AtmosphereNpr (fog only — the sky quad is created and
    /// immediately hidden, the sun/ambient link is never made) or destroys it.
    /// Destroying is what makes "fog off" bit-exact: no atmosphere means no
    /// hlms_fog property, which means the fog code is not in the shader at all.
    /// destroyAtmosphere() MUST run before the SceneManager dies (the component
    /// destroys its Rectangle2D through it).
    void ensureAtmosphere();
    void destroyAtmosphere();
    Ogre::AtmosphereNpr *mAtmosphere = nullptr;

    /// Ogre's OWN sky (SceneManager::setSky): a full-screen Rectangle2D at the far
    /// plane whose camera-direction shader samples an equirect or cube texture.
    /// There is no sky geometry of ours any more — no sphere, no six quads, no
    /// per-frame follow-the-camera. The equirect method needs a texture whose
    /// INTERNAL type is Type2DArray (automatic-batching pool slices are; our
    /// pixel-uploaded ManualTextures are not) — makeSkyArrayTexture() copies when
    /// it must. And it needs ogre-patch 0009: upstream's Vulkan GLSL declares
    /// `sliceIdx` but samples slice 0, so glslang strips the uniform and
    /// SceneManager::setSky throws on setNamedConstant AFTER attaching the sky.
    bool setSky(SkyMode mode, TextureId texId) override;
    bool setSkyCubemap(const TextureId faces[6]) override;
    /// Environment reflections divorced from the sky: the host pushes six
    /// resampled faces of its equirect/baked sky image. Six zero ids clear.
    bool setSkyReflection(const TextureId faces[6]) override;
    /// Builds (replacing any previous) the GGX-prefiltered reflection cubemap by
    /// convolving `srcCube`, and binds it on every PBR datablock. `ownsSource`
    /// means the source is ours to destroy once the convolution has run (the
    /// cubemap-sky path passes false: there the source IS the sky texture).
    /// The prefilter runs on the next renderOneFrame (applyPendingIbl).
    void buildReflectionCubemapFrom(Ogre::TextureGpu *srcCube, bool ownsSource);
    /// Unbinds and destroys the reflection cubemap (no-op when there is none).
    void destroyReflection();
    /// Binds (or clears, when mReflectionTex is null) the scene's sky reflection
    /// cubemap on every PBR material's datablock. (Body in complete-class context,
    /// so it may call the private impl declared further down.)
    void applyReflectionToAll();
    /// Runs the queued ibl_specular convolution (roughness mip chain) for the
    /// reflection cubemap. Called once per frame by the engine, like applyPendingGi.
    void applyPendingIbl();
    Ogre::TextureGpu *mReflectionTex = nullptr;   // prefiltered cube on PBSM_REFLECTION
    void destroySky();
    bool removeNode(NodeId id) override;

    // ---- Hierarchy and transforms ----
    NodeId createNode(NodeId parent) override;
    bool setNodeParent(NodeId id, NodeId parent) override;
    void setNodeTransform(NodeId id, const Vec3 &pos, const Quat &rot, const Vec3 &scale) override;
    void setNodeVisible(NodeId id, bool visible) override;

    // ---- Meshes and materials ----
    MeshId createMesh(const MeshData &data) override;
    bool updateMeshVertices(MeshId id, const std::vector<float> &positions,
                            const std::vector<float> &normals) override;
    bool destroyMesh(MeshId id) override;
    MaterialId createPbrMaterial(const PbrParams &p) override;
    bool setPbrMaterial(MaterialId id, const PbrParams &p) override;
    bool destroyMaterial(MaterialId id) override;
    bool attachMesh(NodeId id, MeshId meshId, MaterialId matId) override;
    bool detachMesh(NodeId id) override;

    // ---- Rigs: GPU skinning (GPU_SKINNING_SPEC; impl in OgreSkeleton.cpp) ----
    bool attachSkinnedMesh(NodeId id, MeshId meshId, MaterialId matId,
                           const SkeletonDesc &rig) override;
    bool hasSkeleton(NodeId id) const override;
    std::vector<std::string> boneNames(NodeId id) const override;
    bool setBonePoses(NodeId id, const BonePose *poses, size_t count) override;
    bool boneMatrices(NodeId id, float *out, size_t count) const override;

    // ---- Clips (ANIMATION_ENGINE_MIGRATION_SPEC; impl in OgreClips.cpp) ----
    bool attachClips(NodeId id, const ClipDesc *clips, size_t count) override;
    std::vector<std::string> clipNames(NodeId id) const override;
    bool setClipStates(NodeId id, const ClipState *states, size_t count) override;
    bool setBoneManual(NodeId id, const std::string &bone, bool manual) override;
    bool bonePoses(NodeId id, BonePose *out, size_t count) const override;
    std::vector<float> clipBoneWeights(NodeId id, const std::string &clip) const override;

    // ---- Textures ----
    TextureId loadTexture(const std::string &path, bool srgb) override;
    TextureId createTexture(unsigned w, unsigned h, const unsigned char *rgba, bool srgb) override;
    bool destroyTexture(TextureId id) override;
    bool setPbrTexture(MaterialId mat, PbrTextureSlot slot, TextureId texId) override;

    // ---- Overlay primitives ----
    MaterialId createUnlitMaterial(const Colour &c, bool depthTest, bool wireframe) override;
    MaterialId createOutlineMaterial(const Colour &c) override;
    bool setUnlitMaterial(MaterialId id, const Colour &c) override;
    MeshId createLineMesh(const std::vector<Vec3> &points, bool strip) override;

    // ---- Particles (billboard sets) ----
    // Ogre-Next's BillboardSet2 (ParticleFX2 core, lives in OgreNextMain — no
    // plugin needed): geometry is generated in the vertex shader from a read-only
    // buffer the ParticleSystemManager2 uploads each frame. The manager attaches
    // every set to the STATIC root scene node, so positions are world-space —
    // exactly how the document simulates. Requires
    // Hlms::_setHasParticleFX2Plugin(true) before shaders are built (ensureHlms).
    bool createBillboardSet(NodeId id, TextureId texId, bool additiveBlend,
                            unsigned capacity) override;
    bool setBillboards(NodeId id, const BillboardInstance *data, size_t count) override;
    bool destroyBillboardSet(NodeId id) override;

    // ---- Particles: engine-simulated systems (PARTICLES_FX2_SPEC.md) ----
    // One ParticleSystemDef per node. The def carries quota, material and
    // visibility; the ParticleSystem2 instance rides the node's SceneNode (the
    // emitter dereferences getParentNode() with no null check, so attaching is
    // mandatory). ParticleSystemManager2 has NO API to destroy a single def —
    // defs live until the SceneManager dies — so releases park the def on
    // mParticleDefPool keyed by its topology, and a matching rebuild reuses it.
    bool     setParticleSystem(NodeId id, const ParticleSystemDesc &d) override;
    bool     removeParticleSystem(NodeId id) override;
    unsigned particleCount(NodeId id) const override;
    unsigned particleDefinitionsCreated() const override { return mParticleDefsCreated; }

    // ---- Lights ----
    bool setLight(NodeId id, const LightDesc &d) override;
    bool removeLight(NodeId id) override;

    // ---- Decals (DECALS_SPEC.md; impl in OgreDecals.cpp) ----
    bool setDecal(NodeId id, const DecalDesc &d) override;
    bool removeDecal(NodeId id) override;
    TextureId loadDecalTexture(const std::string &path, DecalMap kind) override;
    unsigned decalAtlasCapacity(DecalMap kind) const override;
    unsigned decalAtlasUsed(DecalMap kind) const override;

    // ---- Global illumination (GI_SPEC.md phases 1-3) ----
    // Instant Radiosity traces rays from ONE chosen light against the scene's
    // PBR items and plants virtual point lights (LT_VPL) where the rays bounce.
    // The VPLs live in THIS SceneManager and ride its Forward+ clustered list —
    // nothing binds to the process-wide HlmsPbs, so IR never leaks into the
    // player/asset/preview scenes. (The sample's optional IrradianceVolume WOULD
    // be such a global binding — deliberately not used.)
    // VCT voxelizes the scene's PBR items over the GI bounds and cone-traces the
    // result; the hybrid adds a parallax-corrected cubemap probe grid whose
    // reflections blend with VCT's by distance (HlmsPbs PccVctMinDistance).
    // CAVEAT (GI_SPEC.md): setVctLighting/setParallaxCorrectedCubemap bind to the
    // process-wide HlmsPbs singleton — VCT GI is effectively editor-scene-only
    // in v1; the last scene to enable a VCT mode owns the binding, and other
    // scenes' geometry outside the voxel volume samples nothing (cones exit the
    // volume and add no light), so previews/thumbnails stay sane in practice.
    bool setGlobalIllumination(const GiParams &p) override;
    void refreshGlobalIllumination() override;

    // ---- Planar reflections (PLANAR_REFLECTIONS_SPEC.md; impl OgrePlanar.cpp) ----
    bool setPlanarReflections(const PlanarReflectionParams &p) override;
    bool setNodePlanarReflector(NodeId id, bool on) override;
    bool nodePlanarReflector(NodeId id) const override;
    int  activePlanarReflectors() const override;
    /// The scene's live PlanarReflections, or null when the budget is 0. Views
    /// read this once a frame to decide whether to arm their listener.
    Ogre::PlanarReflections *planarReflections() const { return mPlanar; }
    /// Re-derives every actor's world plane from its node's CURRENT transform.
    /// Actors are world-space objects that do NOT follow a SceneNode, so this
    /// has to happen after the host has pushed transforms and before the frame
    /// renders. Called once per frame by the engine, like applyPendingGi.
    void applyPendingPlanar();
    /// Shadow-atlas rebuild support, the OgreView::dropWorkspaceForShadowRebuild
    /// shape: the reflective workspaces instantiate the half-resolution shadow
    /// node, whose DEFINITION cannot be replaced while anything references it.
    /// Returns true when the arm was actually dropped (caller re-adds).
    bool dropPlanarForShadowRebuild();
    void recreatePlanarAfterShadowRebuild();

    Ogre::SceneManager *sceneManager() const;

    /// Releases everything in dependency order. Safe to call twice. Called by
    /// Engine::destroyScene and by the Engine destructor BEFORE Root dies.
    void destroy();

private:
    /// What a node owns. mNodes used to track only the SceneNode, leaking the
    /// Item, Light, mesh and datablock on removal (audit).
    struct Node {
        Ogre::SceneNode *node  = nullptr;
        Ogre::Item      *item  = nullptr;
        Ogre::Light     *light = nullptr;
        Ogre::SceneNode *lightNode = nullptr;   // internal child: -Y (document) -> -Z (Ogre)
        // What is CURRENTLY assigned to `light`, so the per-frame setLight can
        // do nothing when nothing changed. Both assignments are expensive the
        // first time (an atlas rebuild / a decode+resize+mipgen+upload) and the
        // mirror calls setLight for every light on every sync — at 60 Hz.
        std::string      lightProfilePath;
        std::string      lightMaskPath;
        // Vestigial since the selftest-era addTestCube was pruned (nothing assigns
        // these any more); releaseNode still clears them so a future node-owned
        // mesh/datablock keeps the "dropped before Root" teardown guarantee.
        Ogre::MeshPtr    mesh;              // uniquely owned; MUST be dropped before Root
        std::string      meshName;
        std::string      datablockName;     // uniquely owned
        MeshId           meshRef     = 0;   // shared, owned by mMeshes
        MaterialId       materialRef = 0;   // shared, owned by mMaterials
        // Billboard set (particles): uniquely owned; freed by releaseBillboards
        // BEFORE the scene manager dies (its _destroy needs the live VaoManager).
        // Decal (DECALS_SPEC): the Decal rides an internal child node whose
        // scale IS the projector box (Ogre's culler reads the derived scale as
        // the box half-extents), so the document node's own scale composes with
        // the numeric width/height/depth.
        Ogre::Decal              *decal = nullptr;
        Ogre::SceneNode          *decalNode = nullptr;
        Ogre::BillboardSet       *billboards = nullptr;
        std::vector<Ogre::uint32> billboardHandles;   // live handles, dense, in order
        std::string               billboardDatablockName;
        unsigned                  billboardCapacity = 0;
        // Engine-simulated particle system (PARTICLES_FX2_SPEC): the def is NOT
        // uniquely owned — it outlives the node and returns to mParticleDefPool
        // (there is no destroyParticleSystemDef). The INSTANCE is ours to
        // destroy; the datablock belongs to the def and travels with it
        // (mParticleDatablocks), because a def binds its datablock exactly once,
        // inside init().
        Ogre::ParticleSystemDef  *particleDef = nullptr;
        Ogre::ParticleSystem2    *particleSystem = nullptr;
        std::string               particleTopology;   // the pool key this def answers to
        /// What setNodeVisible was last told. Kept because PFX2 objects do not
        /// live under the node in Ogre's graph (they hang off the STATIC root),
        /// so no visibility cascade reaches them and a system created or
        /// recycled later has to be told the node's state explicitly.
        bool                      visible = true;
    };

    /// A definition's frozen shape. Two systems can share a recycled def only if
    /// every element of this matches, because none of it can be changed after
    /// ParticleSystemDef::init(): setParticleQuota asserts !isInitialized(), and
    /// adding an emitter once an instance exists corrupts the per-instance
    /// emitter array (sized once in the ParticleSystem2 ctor, indexed by the
    /// def's emitter count in the update loop).
    struct ParticleTopology {
        unsigned quotaBucket = 0;
        int      orientation = 0;
        bool     additive = true;
        bool     alphaHash = false;
        std::vector<int> emitterShapes;   // ParticleEmitterShape per emitter, in order
        std::vector<int> affectorKinds;   // ParticleAffectorDesc::Kind per affector, in order
        std::string key() const;
    };
    struct MeshRec {
        Ogre::MeshPtr mesh; std::string name;
        // CPU-skinning support (MeshData::dynamic): the interleaved vertex array
        // (12 floats: pos3 normal3 tangent4 uv2) is kept so updateMeshVertices can
        // rewrite positions/normals while preserving tangents and uvs.
        bool dynamic = false;
        std::vector<float> interleaved;
        // GPU skinning: the mesh was built with VES_BLEND_INDICES/WEIGHTS in its
        // vertex declaration, so attachSkinnedMesh may bind a rig to it. A mesh
        // carries at most ONE rig (Ogre::Mesh holds one SkeletonDef); `rigId` is
        // the SkeletonDesc::id that was bound, empty until one is.
        bool hasSkinData = false;
        unsigned maxBlendIndex = 0;
        std::string rigId;
    };
    /// A rig, as this scene knows it. The Ogre-side SkeletonDef is cached
    /// PROCESS-wide by SkeletonManager under the same id (GPU_SKINNING_SPEC R6),
    /// which is why SkeletonDesc::id must be derived from the bone structure
    /// alone: two files of the same rig must resolve to one def.
    struct RigRec {
        std::vector<std::string> boneNames;
        /// The DESC the rig was built from, kept whole. A clip def must be
        /// built from the SAME bone structure the rig def was — Ogre's own
        /// addAnimationsFromSkeleton indexes the other def's block layout, and
        /// a mismatch is undefined behaviour rather than a no-op.
        SkeletonDesc desc;
    };
    /// One clip attached to one node.
    struct ClipRec {
        std::string id;             ///< content hash; the def-cache key
        std::string name;           ///< uniquified per node
        std::string defName;        ///< the SkeletonDef / v1 resource name
        float       length = 0.0f;  ///< seconds, after zero-length padding
        /// Bones this clip animates, and the cached weight slot for each (§4.2:
        /// per-bone renormalization is one float store per bone per frame, and
        /// only through a pointer we cached at attach time).
        std::vector<int>    coverage;
        std::vector<float*> weightPtr;   // parallel to coverage
        size_t index = 0;   ///< slot in SkeletonInstance::getAnimations()
    };
    /// A node's clip set. Absent until attachClips is called, which is also
    /// what takes the node out of manual-bone mode.
    struct NodeClips {
        std::vector<ClipRec>  clips;
        std::set<std::string> manualBones;   ///< explicit setBoneManual overrides
        /// attachSkinnedMesh marks EVERY bone manual (setBonePoses' values must
        /// survive resetToPose). The first attachClips clears that, because a
        /// manual bone is not reset to bind and a clip would ADD to the last
        /// pushed pose rather than replace it.
        bool clipModeEntered = false;
    };
    struct MaterialRec {
        std::string datablockName;
        bool unlit = false;
        bool onTop = false;
        /// PbrAlphaMode::Refractive. Refractive items must render in the chain's
        /// OWN pass (kRefractiveRenderQueue) — Ogre's words: "the compositor
        /// scene pass must be set to render refractive objects in its own pass".
        /// Left in the opaque pass they render as ordinary glass, silently.
        bool refractive = false;
    };
    struct TextureRec {
        Ogre::TextureGpu *texture = nullptr;
        std::string path;
        /// Decal-atlas slice (loadDecalTexture): SHARED process-wide and
        /// refcounted, so it must never go through the plain
        /// TextureGpuManager::destroyTexture path.
        bool     decal = false;
        DecalMap decalKind = DecalMap::Diffuse;
    };

    void applyReflectionToAllImpl();
public:
    /// Is a refraction pass present in EVERY view that draws this scene?
    ///
    /// This is not a preference, it is a SAFETY INTERLOCK. An
    /// HlmsPbsDatablock::Refractive material rendered by a pass that does not
    /// offer it refractions generates a pixel shader referencing an undeclared
    /// `refractionMap`; the compile throws out of renderOneFrame and the WHOLE
    /// FRAME is lost, not just that object. Views of one scene do not all have
    /// the same chain (a screenshot renders the editor's scene through a
    /// throwaway offscreen view), so the engine recomputes this every frame and
    /// downgrades refractive datablocks to plain glass whenever any view that
    /// draws them lacks the pass. No combination of settings can black-frame.
    void setRefractionsActive(bool active);
    bool refractionsActive() const { return mRefractionsActive; }
private:
    /// The render queue an item using this material belongs in.
    static Ogre::uint8 renderQueueFor(const MaterialRec &m);
    /// Re-files every item that uses this material after its alpha mode changed.
    void refileItems(MaterialId id, const MaterialRec &m);

    /// Releases one texture record: a pooled decal slice drops a reference (and
    /// dies with the last one), anything else is destroyed outright.
    void releaseTextureRec(const TextureRec &rec);

    Ogre::Hlms *hlmsFor(const MaterialRec &m) const;
    /// Removes the renderable from a node that references a SHARED mesh/material.
    void detachItem(NodeId id, Node &n);
    // Ogre::HlmsPbsDatablock::None is unspellable here: X11's `None` macro (this
    // file includes Xlib.h for window handles) eats the identifier.
    static constexpr auto kTransparencyNone = static_cast<Ogre::HlmsPbsDatablock::TransparencyModes>(0);
    /// `refractionsActive` false downgrades PbrAlphaMode::Refractive to plain
    /// glass — see setRefractionsActive for why that is not optional.
    static void applyPbr(Ogre::HlmsPbsDatablock *db, const PbrParams &p,
                         bool refractionsActive);
    /// Builds (or finds) the in-memory v1 skeleton `rig` translates to and hands
    /// the resulting SkeletonDef to `mesh`. v1 is a BUILD-TIME SCAFFOLD ONLY —
    /// SkeletonDef has exactly one constructor and it takes a v1::Skeleton
    /// (OgreSkeletonDef.h:145); nothing v1 reaches the render path (v1 meshes
    /// render NOTHING on Vulkan, and geometry stays in our v2 buffers).
    bool bindRigToMesh(MeshRec &meshRec, const SkeletonDesc &rig);
    /// Assembles the in-memory v1 skeleton a SkeletonDesc translates to, under
    /// `resName`. Shared by the rig def and every CLIP def, because a clip def
    /// built from anything but the node's own rig indexes the wrong blocks in
    /// addAnimationsFromSkeleton — undefined behaviour, not a no-op.
    /// `madeOut`, when given, receives the bones in DESC ORDER.
    Ogre::v1::SkeletonPtr buildV1Skeleton(const std::string &resName, const SkeletonDesc &rig,
                                          std::vector<Ogre::v1::OldBone *> *madeOut = nullptr);
    /// The rig a node's mesh is bound to, or null.
    const RigRec *rigOf(NodeId id) const;
    /// The node's live rig, or null.
    Ogre::SkeletonInstance *skeletonOf(NodeId id) const;
    /// Uploads MeshData as a v2 mesh: interleaved position/normal/tangent/uv, 16- or
    /// 32-bit indices. v1 meshes silently render nothing on Vulkan, so only this path
    /// exists. Every mesh carries tangents: HlmsPbs refuses to render a normal-mapped
    /// datablock on a mesh without them (throws, object falls back to flat grey).
    Ogre::MeshPtr buildMeshV2(const std::string &name, const MeshData &data,
                              std::vector<float> *interleavedOut = nullptr);
    /// The six WORLD-axis faces (+X,-X,+Y,-Y,+Z,-Z, seen from inside) as ONE Ogre
    /// cubemap. Ogre samples cubemaps LEFT-handed — HlmsPbs negates the view
    /// matrix' Z column ("Cubemaps are left-handed", OgreHlmsPbs.cpp:2327) and
    /// SkyCubemap_ps.glsl negates cameraDir.z — so a world direction d reads the
    /// cube at (d.x, d.y, -d.z). Mirroring in Z means the +Z and -Z faces swap and
    /// every face image is mirrored: horizontally for +X,-X,+Z,-Z (their in-face U
    /// runs along +-Z) and vertically for +Y,-Y (their in-face V does). copyTo
    /// cannot mirror, so each face is downloaded, flipped and re-uploaded — once
    /// per sky change, not per frame. Returns null (and sets mError) on failure.
    /// `extraFlags` is OR-ed into the texture flags; `mips` false = one mip.
    Ogre::TextureGpu *buildCubeFromWorldFaces(Ogre::TextureGpu *const tex[6],
                                              const std::string &namePrefix,
                                              Ogre::uint32 extraFlags, bool mips);
    /// A Type2DArray (1 slice) copy of a 2D texture: SkyEquirectangular refuses
    /// anything whose internal type is not Type2DArray.
    Ogre::TextureGpu *makeSkyArrayTexture(Ogre::TextureGpu *src);
    /// Applies our render-queue / visibility policy to Ogre's sky renderable:
    /// queue 0 (the sky is drawn FIRST, exactly where our six quads used to be, so
    /// on-top overlays at queue 200 still paint over it), and kVisibleBit only, so
    /// Instant Radiosity's visibility-masked ray casts never hit it.
    void tuneSkyRenderable();
    /// Frees a node's billboard set and its datablock, in that order (the set
    /// references the datablock until it is destroyed). Safe to call twice.
    void releaseBillboards(Node &n);
    /// Detaches and destroys the node's ParticleSystem2 instance, hides the def
    /// and parks it on mParticleDefPool (there is no destroyParticleSystemDef),
    /// and destroys the datablock the def referenced. Safe to call twice; must
    /// run before the SceneManager dies.
    void releaseParticleSystem(Node &n);
    /// Builds (or recycles) a definition matching `topo` and points `n` at it.
    /// The returned def is initialised and has its emitters/affectors in place;
    /// scalar values are pushed separately by applyParticleValues.
    bool buildParticleDef(Node &n, const ParticleSystemDesc &d,
                          const ParticleTopology &topo);
    /// Pushes every scalar of `d` onto the node's existing def. No rebuild.
    void applyParticleValues(Node &n, const ParticleSystemDesc &d);
    /// Creates or updates the Unlit datablock a particle def renders with and
    /// binds it. Returns the datablock name, empty on failure.
    std::string ensureParticleDatablock(Node &n, const ParticleSystemDesc &d);
    /// Destroys the node's decal and its internal child node, and re-points the
    /// SceneManager's decal atlases (clearing them when the last decal goes).
    /// Safe to call twice; must run before the SceneManager dies.
    void releaseDecal(Node &n);
    /// Points this SceneManager at the atlas master textures iff the scene
    /// still has at least one decal — the shader permutation is gated on a
    /// non-null SceneManager decal texture, so clearing it drops the decal
    /// code out of every PBS shader again.
    void refreshDecalBindings();
    void releaseNode(NodeId id, Node &n);

    // ---- GI internals ----
    /// Resolves the driving light — the requested node's light, else the first
    /// directional, else any light — and marks exactly that light with
    /// kGiLightBit so InstantRadiosity's light mask selects it alone. Returns
    /// null when the scene has no light at all. (IR's own LT_VPL lights are not
    /// in mNodes and are skipped by IR itself.)
    Ogre::Light *markGiLight(NodeId requested);
    /// The GI working volume: the document's explicit bounds, or (min == max)
    /// the world AABB of every GI-participating item plus a margin.
    bool computeGiBounds(Ogre::Vector3 &mn, Ogre::Vector3 &mx) const;
    /// Re-traces Instant Radiosity against the scene as it is right now. Cheap
    /// enough (a few ms at editor quality) to run on every light move.
    /// Ogre::InstantRadiosity caches mesh data by raw VertexArrayObject* and
    /// downloaded images by TextureGpu* (OgreInstantRadiosity.h:238-244). Destroying
    /// a mesh/texture while IR is live leaves those caches dangling — the owner's
    /// scene-switch crash ("double free or corruption" tearing down a scene with IR
    /// enabled while the next project's assets churned). Ogre::VctVoxelizer has the
    /// same shape twice over: addItem keeps raw Item* until removeAllItems, and
    /// VctMaterial caches conversions by raw datablock pointer across builds.
    /// So every geometry/material/texture destroy path calls this BEFORE the
    /// object actually dies — IR's caches are freed EAGERLY here, because
    /// InstantRadiosity::freeMemory dereferences its VertexArrayObject* cache
    /// keys and calling it after the mesh died is itself the heap corruption.
    /// The rebuild still happens ONCE at frame time (bursty destroys = one
    /// rebuild); for VCT the flush tears the whole arm down and re-voxelizes
    /// from the LIVE scene, so a recycled pointer can never alias.
    void invalidateGiCaches();
public:
    /// Called by Engine::renderOneFrame before rendering.
    void applyPendingGi();
    /// Called by OgreView each frame with its camera position: the PCC probe
    /// blend tracks the viewer. No-op unless the hybrid mode is live.
    void updateGiTracking(const Ogre::Vector3 &camPos);
private:
    void rebuildGi();
    /// Voxelizes the scene's PBR items over computeGiBounds at quality-mapped
    /// resolution, (re)builds VctLighting and binds it to HlmsPbs. The voxelizer
    /// and lighting are recreated from scratch every time (see invalidateGiCaches).
    /// In hybrid mode also (re)builds the PCC probe grid.
    void rebuildVct();
    /// Builds the ParallaxCorrectedCubemapAuto probe grid over the same bounds
    /// and binds it with distance-blended VCT specular (PccVctMinDistance).
    void buildPcc(const Ogre::Aabb &aabb);
    /// Unbinds from HlmsPbs (when this scene owns the binding) and deletes the
    /// PCC, VctLighting and VctVoxelizer, in that order. Safe to call twice;
    /// must run BEFORE the SceneManager dies.
    void teardownVct();
    /// Deletes the radiosity solution and its VPL lights. Safe to call twice;
    /// must run BEFORE the SceneManager dies (the dtor destroys its lights).
    void teardownIr();
    /// Deletes every GI object (IR + VCT arms). Safe to call twice; must run
    /// BEFORE the SceneManager dies (VPL lights, probe workspaces, GI camera).
    void teardownGi();

    // ---- Planar-reflection internals (OgrePlanar.cpp) ----
    /// Builds the PlanarReflections arm at mPlanarParams (private workspace
    /// definition, cameras, RTTs) and re-adds every reflector's actor. Always
    /// from scratch: the VCT lesson — Ogre caches by raw pointer inside these
    /// objects, and setMaxActiveActors can only grow, never re-specify.
    void rebuildPlanar();
    /// Unbinds from HlmsPbs (when this scene owns the binding), deletes the
    /// PlanarReflections and removes the private workspace definitions. Safe to
    /// call twice; MUST run before the SceneManager dies (it owns the
    /// reflection cameras and workspaces).
    void teardownPlanar();
    /// Registers `n` with the live arm: adds its actor, adds its item as a PBS
    /// receiver, and tags the item kNoReflectBit so the mirror stays out of its
    /// own reflection. No-op when the arm is down. Returns false + mError when
    /// the node's mesh is not plate-like.
    bool armReflector(NodeId id, Node &n);
    /// Removes `n` from the live arm and restores its visibility flags. MUST be
    /// called before the node's Item is destroyed: PlanarReflections keeps raw
    /// Renderable pointers and its own header says so in as many words.
    void disarmReflector(NodeId id, Node &n);
    /// disarmReflector for every reflector, keeping the flags — used when the
    /// arm itself is being torn down and rebuilt.
    void disarmAllReflectors();

    Ogre::SceneNode *node(NodeId id) const;
    /// Ids are monotonic per scene and never reused.
    NodeId track(const Node &n);


    Ogre::Root         *mRoot;
    Ogre::SceneManager *mSceneMgr;
    std::string         mName;
    std::string        &mError;
    std::map<NodeId, Node> mNodes;
    std::map<MeshId, MeshRec> mMeshes;
    std::map<std::string, RigRec> mRigs;
    std::map<NodeId, NodeClips> mClips;
    std::map<MaterialId, MaterialRec> mMaterials;
    std::map<TextureId, TextureRec> mTextures;
    std::set<std::string> mTextureDirs;
    /// SceneManager::getSkyMethod() never reflects the method actually set
    /// (upstream's setSky forgets to assign mSkyMethod), so remember it.
    bool              mSkyIsEquirect = false;
    /// Textures WE own for Ogre's sky renderable (the equirect Type2DArray copy /
    /// the converted cube). Null when the sky uses a host texture directly.
    Ogre::TextureGpu *mSkyOwnedTex = nullptr;
    /// The un-prefiltered cube the ibl_specular pass convolves into mReflectionTex.
    /// May alias mSkyOwnedTex (a cubemap sky is its own IBL source); mIblSourceOwned
    /// says whether it is ours to destroy.
    Ogre::TextureGpu *mIblSourceTex = nullptr;
    bool              mIblSourceOwned = false;
    bool              mIblPending = false;   // convolve on the next frame
    /// One-shot ibl_specular workspace; kept null between runs.
    Ogre::Camera *mIblCamera = nullptr;
    Ogre::InstantRadiosity *mInstantRadiosity = nullptr;   // owned; null unless IR mode
    // VCT arm (null unless a VCT mode is live). Teardown order within the arm:
    // unbind HlmsPbs -> PCC -> VctLighting -> VctVoxelizer, all before the
    // SceneManager (probe workspaces and the GI camera live in it).
    Ogre::VctVoxelizer               *mVctVoxelizer = nullptr;
    Ogre::VctLighting                *mVctLighting  = nullptr;
    Ogre::ParallaxCorrectedCubemapAuto *mPcc        = nullptr;
    Ogre::Camera                     *mGiCamera     = nullptr;   // PCC build + tracking
    bool mRefractionsActive = false;   // see setRefractionsActive
    bool mGiCachesDirty = false;   // mesh/texture/material died while GI live; flush at frame time
    GiParams         mGi;                                  // last applied GI state
    /// Live decals in THIS scene. The SceneManager-level atlas binding is
    /// driven off the count (see refreshDecalBindings).
    unsigned            mDecalCount = 0;
    // Planar-reflection arm. mPlanar is null unless mPlanarParams.budget > 0.
    // mReflectors is the DOCUMENT's set of reflector nodes and survives the arm
    // going up and down; mActors only exists while the arm is up.
    Ogre::PlanarReflections *mPlanar = nullptr;
    PlanarReflectionParams   mPlanarParams;
    std::string              mPlanarWorkspaceDef;
    std::vector<std::string> mPlanarNodeDefs;
    std::set<NodeId>         mReflectors;
    std::map<NodeId, Ogre::PlanarReflectionActor *> mActors;
    /// Abandoned particle definitions, keyed by ParticleTopology::key(). They
    /// cannot be destroyed (no such API on ParticleSystemManager2 — defs are
    /// freed only in its destructor, i.e. with the SceneManager), so a released
    /// def is hidden and parked here for the next system with the same shape.
    /// With frozen topologies (one emitter, a fixed affector set, coarse quota
    /// buckets) the pool stays a handful of entries per scene.
    std::map<std::string, std::vector<Ogre::ParticleSystemDef *>> mParticleDefPool;
    /// The Unlit datablock each def is bound to. A def binds its datablock ONCE,
    /// inside init() (OgreParticleSystem2.cpp:251-256) — setMaterialName
    /// afterwards is a no-op for PFX2 defs (mIsRendererConfigured is never true
    /// for them). So the datablock is per-DEF, mutated in place when a node's
    /// texture or blend mode changes, and destroyed only at scene teardown
    /// (datablocks belong to the process-wide HlmsManager, not to the
    /// SceneManager that frees the defs).
    std::map<Ogre::ParticleSystemDef *, std::string> mParticleDatablocks;
    /// Every def this scene ever created, for the def-accumulation measurement
    /// the particle gates print (PARTICLES_FX2_SPEC §3.2). Never shrinks.
    unsigned            mParticleDefsCreated = 0;
    TextureId           mNextTextureId = 0;
    NodeId              mNextId = 0;
    MeshId              mNextMeshId = 0;
    MaterialId          mNextMaterialId = 0;
};

// ---------------------------------------------------------------------------
class OgreView final : public View {
public:
    /// On-screen: `window` is set. Offscreen: `texture` is set. Never both.
    OgreView(Ogre::Root *root, Ogre::Window *window, Ogre::TextureGpu *texture,
             const std::string &name, unsigned w, unsigned h, const Colour &background,
             std::string &errorSink);
    ~OgreView() override;

    const std::string &name() const override;
    Scene *scene() const override;

    bool setScene(Scene *scene) override;

    /// Unbinds the scene: workspace and camera go, the scene itself survives.
    void detachScene();

    void setCamera(const CameraDesc &c) override;
    void setEnabled(bool on) override;
    Colour background() const override;
    bool shadows() const override;
    void setShadows(bool on) override;
    void setBackground(const Colour &c) override;
    /// The clear colour and the shadow node live in the chain's definitions:
    /// rebuild definitions + workspace, keeping scene, camera and enabled state.
    void rebuildWorkspaceDef();
    static constexpr const char *kShadowNodeName = "JahshakaShadowNode";
    /// The SECOND shadow node, at half the base resolution, used ONLY by the
    /// planar-reflection pass. CompositorShadowNodes are per-workspace and are
    /// constructed eagerly in CompositorPassScene's constructor, so every
    /// reflection slot allocates its own atlas the moment the arm is built:
    /// sharing kShadowNodeName would cost ~56 MB PER SLOT at the default 2048.
    /// Half resolution makes that ~14 MB, and a shadow seen in a mirror is the
    /// last place anyone measures shadow-map resolution.
    static constexpr const char *kReflectShadowNodeName = "JahshakaReflectShadowNode";

    // ---- The workspace seam (POST_CHAIN_SPEC.md; the planar-reflection lane
    //      depends on it) ---------------------------------------------------
    /// THE one place a CompositorWorkspace is created for this view. Six call
    /// sites used to do it inline (setScene, definition rebuild, shadow-atlas
    /// rebuild, RTT rebuild, window recreate, MSAA change); anything that must
    /// ride a LIVE workspace — a compositor listener, per-view effect state —
    /// would have had to be re-attached in all six. Now it is re-attached here.
    /// Returns true when a workspace exists afterwards.
    bool attachWorkspace();
    /// This view's current chain shape — what the builder is asked for.
    ChainDesc chainDesc() const;
    /// Drops the live workspace (detaching its listeners first). Safe when
    /// there is none; returns whether one was actually dropped.
    bool detachWorkspace();
    /// Compositor listeners this view re-attaches to every workspace it builds.
    /// The view does NOT own them: register at setup, unregister before the
    /// listener dies. Registering twice is a no-op.
    void addWorkspaceListener(Ogre::CompositorWorkspaceListener *l);
    void removeWorkspaceListener(Ogre::CompositorWorkspaceListener *l);
    /// Counts completed workspace attachments. Neutral introspection (no Ogre
    /// type crosses the boundary) that lets hosts and tests see that a call was
    /// or was not structurally expensive — every rebuild goes through the seam,
    /// so this is exactly the number of times it ran.
    unsigned workspaceGeneration() const override;

    unsigned long long framesPresented() const override;
    bool warmUpShaders() override;
    /// Called by OgreEngine::renderOneFrame AFTER Root::renderOneFrame: counts
    /// this frame if the view was actually part of it (enabled + workspace +
    /// scene). The one place mFramesPresented moves up.
    void notePresented();

    void setPostFx(const PostFxDesc &fx) override;
    const PostFxDesc &postFx() const override;
    /// The camera the chain's per-frame globals are computed from.
    Ogre::Camera *camera() const { return mCamera; }
    /// Shadow-atlas rebuild support (Engine::setShadowResolution): the shadow node
    /// DEFINITION cannot be replaced while any workspace instantiates it, so the
    /// engine first drops every shadowed view's workspace (true = dropped, caller
    /// re-adds), then swaps the definition, then calls the restore below.
    bool dropWorkspaceForShadowRebuild();
    void recreateWorkspaceAfterShadowRebuild();
    bool isEnabled() const override;
    unsigned width()  const override;
    unsigned height() const override;
    bool isOffscreen() const override;

    /// Set by the engine for on-screen views ON X11: creates a fresh Ogre window on the
    /// same native handle at the given size and MSAA sample count (the "FSAA" misc param —
    /// BOTH window-creation sites must pass it or MSAA silently resets on resize).
    /// EMPTY on macOS: VulkanMetalWindow resizes its own surface (see applyPendingResize).
    std::function<Ogre::Window *(unsigned, unsigned, unsigned)> mCreateWindow;
    unsigned mPendingW = 0, mPendingH = 0;
    /// Runtime MSAA change is structurally a resize (Vulkan has no runtime
    /// setFsaa): non-zero = recreate the target at mRequestedSamples next
    /// applyPendingResize, even at the same size.
    unsigned mPendingSamples = 0;
    /// What the host asked for (sanitized). The engine initialises it for
    /// on-screen views from EngineConfig::sampleCount; offscreen views start
    /// at 1 (pixel-asserted readbacks stay exact unless a test opts in).
    unsigned mRequestedSamples = 1;

    void setSampleCount(unsigned samples) override;
    unsigned sampleCount() const override;

    /// On-screen resize is applied at frame time (applyPendingResize): by then Qt has
    /// resized the native window, and doing it once per frame coalesces layout bursts.
    void resize(unsigned w, unsigned h) override;

    bool readPixels(Image &out) override;

    /// The XCB window does not reallocate its depth buffer when the host resizes it
    /// (the swapchain follows the surface, the depth texture keeps its old size, and
    /// the mismatched framebuffer faults the GPU — Vulkan validation VUID 04533/04534),
    /// so on X11 a size change recreates the render window on the same native handle
    /// (mCreateWindow). Windows that implement requestResolution properly — macOS's
    /// VulkanMetalWindow does: destroySwapchain (which transitions colour AND depth to
    /// OnStorage) → setFinalResolution → createSwapchain — are resized in place instead.
    void applyPendingResize();
    /// Feeds the camera position to the scene's particle manager: billboard uploads
    /// are depth-sorted against it (matters for alpha-blended sets).
    void updateParticles();
    /// Feeds the camera position to the scene's GI (PCC probe blending tracks
    /// the viewer in hybrid mode).
    void updateGi();
    /// Arms or disarms this view's planar-reflection compositor listener to
    /// match the bound scene's current state, and re-points it at the current
    /// camera and PlanarReflections instance. Cheap and idempotent; called once
    /// a frame, because BOTH ends move (the scene rebuilds its arm on any
    /// parameter change, the view recreates its camera on every setScene).
    /// Registration itself rides addWorkspaceListener, so the listener survives
    /// every workspace rebuild — that is the seam this feature was waiting for.
    void syncPlanarListener();
    /// Releases workspace, camera, workspace definitions and the window/texture.
    /// Safe to call twice. Called by Engine::destroyView and by the Engine
    /// destructor BEFORE Root dies.
    void destroy();

    /// `samples` > 1 asks for an implicit-resolve MSAA target: the sample
    /// description MUST be set before scheduleTransitionTo(Resident) (Ogre
    /// asserts OnStorage); the achieved count is validated at the transition.
    static Ogre::TextureGpu *createRtt(Ogre::Root *root, const std::string &name,
                                       unsigned w, unsigned h, unsigned samples = 1);
    /// Rounds down to a power of two and clamps to [1, 16] — what the backend
    /// will even ask the driver for (the driver may still clamp further).
    static unsigned sanitizeSamples(unsigned samples);

private:
    /// Offscreen only: replaces the RTT (which cannot change in place) at the
    /// given size and mRequestedSamples, re-adding the workspace. The shared
    /// tail of resize() and setSampleCount().
    void rebuildRtt(unsigned w, unsigned h);
    Ogre::TextureGpu *target() const;

    Ogre::Root                *mRoot;
    Ogre::Window              *mWindow;
    Ogre::TextureGpu          *mTexture;
    Ogre::Camera              *mCamera    = nullptr;
    Ogre::CompositorWorkspace *mWorkspace = nullptr;
    OgreScene                 *mScene     = nullptr;
    std::string                mName, mWorkspaceDef;
    /// Every node definition the chain builder made for this view, in creation
    /// order. Was a single std::string while the chain was one node — a
    /// multi-node chain that removes one definition leaks the rest across view
    /// recreation (test_engine_recreate is where that shows up).
    std::vector<std::string>   mNodeDefs;
    std::vector<Ogre::CompositorWorkspaceListener *> mWorkspaceListeners;
    /// Owned; registered through addWorkspaceListener while the bound scene has
    /// planar reflections armed. Null until the first frame that needs it.
    std::unique_ptr<planar::WorkspaceListener> mPlanarListener;
    unsigned                   mWorkspaceGeneration = 0;
    /// Frames drawn+presented since the current scene was bound (see
    /// View::framesPresented). Reset by setScene/detachScene, NOT by a
    /// workspace rebuild.
    unsigned long long         mFramesPresented = 0;
    /// What the host asked for. Offscreen views keep it and ignore it.
    PostFxDesc                 mPostFx;
    unsigned                   mWidth, mHeight;
    Colour                     mBackground;
    bool                       mEnabled = true;
    bool                       mShadows = false;
    std::string               &mError;
};

// ---------------------------------------------------------------------------
class OgreEngine final : public Engine {
public:
    bool init(const EngineConfig &cfg, std::string &error);

    Scene *createScene(const std::string &name) override;

    void destroyScene(Scene *scene) override;

    View *createView(const std::string &name,
                     NativeWindowHandle handle, unsigned width, unsigned height,
                     const Colour &background) override;

    View *createOffscreenView(const std::string &name, unsigned width, unsigned height,
                              const Colour &background) override;

    void destroyView(View *view) override;

    void renderOneFrame() override;
    const std::string &lastError() const override;

    /// PROCESS-WIDE: both ride Ogre's single frame-time controller value
    /// (ControllerManager -> FrameTimeControllerValue). Note the backend's own
    /// coupling — setTimeFactor zeroes the frame delay and setFrameDelay zeroes
    /// the time factor, so the two are mutually exclusive by construction, not
    /// by our choice.
    void  setParticleTimeScale(float scale) override;
    float particleTimeScale() const override;
    void  setFixedFrameDelta(float seconds) override;
    float fixedFrameDelta() const override;

    /// GLOBAL by construction: HlmsPbs keeps ONE ShadowFilter for every shadowed
    /// light in every scene (HlmsPbs::mShadowFilter). The filter properties are
    /// evaluated in preparePassHash each pass, so changing it at runtime takes
    /// effect next frame — no datablock or workspace rebuild.
    void setShadowFilter(ShadowFilter f) override;
    ShadowFilter shadowFilter() const override;

    /// GLOBAL like the filter, but NOT cheap: the sizes live in the shadow-node
    /// DEFINITION, which cannot change while any workspace instantiates it. So:
    /// drop every shadowed view's workspace, swap the definition, re-add them —
    /// the same teardown order the engine's destructor honours.
    void setShadowResolution(unsigned pixels) override;
    unsigned shadowResolution() const override;

    /// Ogre::Mesh::msOptimizeForShadowMapping — a plain process-wide static,
    /// read by buildMeshV2 when it decides whether to give a mesh its own
    /// position-only shadow VAOs (POST_CHAIN_SPEC.md §11).
    void setShadowMeshOptimization(bool on) override;
    bool shadowMeshOptimization() const override;

    /// The persistent shader cache (SHADER_CACHE_SPEC.md). Loaded inside the
    /// first createView() -> ensureHlms(); saved on clean teardown and whenever
    /// the host says a compile burst has settled.
    ShaderCacheStats shaderCacheStats() const override;
    bool saveShaderCache() override;
    bool clearShaderCache() override;
    void shaderBuildProgress(unsigned &compiled, unsigned &fromCache,
                             unsigned &expected) const override;

    ~OgreEngine() override;

private:
#ifdef __linux__
    struct X11Handle { Display *display; ::Window window; };
#endif

    bool viewNameTaken(const std::string &name);

    /// First render target: the VaoManager now exists, so Hlms can be registered.
    void ensureHlms();
    /// Ogre's low-level material scripts (sky quad, DPSM shadow maps, depth utils).
    /// Staged from Samples/Media/2.0/scripts/materials/Common next to the Hlms data.
    void registerCommonMaterials();
    /// One shadow node for the process: PSSM (3 splits) for the first directional
    /// light and focused maps for the next two point/spot lights, in one atlas.
    /// Mirrors Ogre's ShadowMapFromCode sample. Views opt in with setShadows(true).
    /// Also creates the half-resolution twin the planar-reflection pass uses.
    void createShadowNode();
    /// The shared body: one PSSM + two focused maps in one atlas derived from
    /// `baseResolution`, registered under `name`.
    void buildShadowNode(const char *name, unsigned baseResolution);

    /// Maps the neutral enum onto HlmsPbs. PCF only: ExponentialShadowMaps is
    /// deliberately NOT used for VerySoft — ESM needs an ESM-compatible shadow
    /// node (colour shadow-map target plus blur passes) which our fixed
    /// depth-atlas shadow node (createShadowNode) is not, and
    /// setShadowSettings(ESM) also flips the global ShadowCameraSetup ESM flag.
    void applyShadowFilter();

    Ogre::Root     *mRoot = nullptr;
    Ogre::Window   *mNullWindow = nullptr;
#ifdef __linux__
    Display        *mDisplay = nullptr;
#endif
    bool            mHlmsRegistered = false;
    /// Plugin_ParticleFX2 loaded: the emitter/affector factories exist. False
    /// leaves billboard sets working and setParticleSystem failing cleanly.
    bool            mHasParticleFX2 = false;
    ShadowFilter    mShadowFilter = ShadowFilter::Soft;
    unsigned        mShadowResolution = 2048;
    unsigned        mDefaultSamples = 1;   // EngineConfig::sampleCount, sanitized; on-screen views only
    Ogre::AbiCookie mAbiCookie{};
    std::string     mBackendName, mMediaDir, mLastError;
    ShaderCache     mShaderCache;
    std::vector<std::unique_ptr<OgreScene>> mScenes;
    std::vector<std::unique_ptr<OgreView>>  mViews;
};

}  // namespace detail
}}  // namespace jahshaka::engine
