// SURFACE-CACHE phase 2 — THE CAPTURE COMPONENT (SURFACE-CACHE-1b, 2026-09-21).
//
// WHAT THIS IS. `jahshaka::engine::SurfaceCache` is an Ogre-Next COMPONENT in
// the shape `VctLighting`, `IrradianceField` and `ParallaxCorrectedCubemap` are
// built in (SPECS/SURFACE_CACHE_ASSESSMENT.md §6): a C++ class using only the
// public engine API — `TextureGpuManager::createTexture`, a
// `CompositorWorkspace` of its own, `Camera`s in the scene it serves — with its
// shader half as Hlms media under `media/Hlms/Jahshaka/`. It owns:
//
//   * THE ATLAS, from day one and for the life of the cache: five `Type2D`
//     layers at 2048 square, RESIDENT, never created or destroyed around a
//     capture (the 0071 lesson — residency churn on a large target is what cost
//     this engine an Xid).
//   * THE PAGE ALLOCATOR: a 128-texel page grid over the atlas. A card wider
//     than a page is SPLIT across several; a card smaller than a page is
//     SUB-ALLOCATED inside one, with the half-texel inset that keeps a filtered
//     fetch off its neighbour's texels (Lumen's rule).
//   * RESIDENCY BY DISTANCE: an instance beyond the tier's radius holds no
//     pages at all, and gives back the pages it held.
//   * THE CAPTURE, in THE VIEW'S OWN SceneManager (below).
//   * INVALIDATION on the three signatures the voxel side already keeps — the
//     transform epoch, the material generation, the light write serial.
//   * A PER-FRAME TEXEL BUDGET with Lumen's priority, `lastUsed - lastUpdated`.
//   * THE CARD RECT TABLE and its SSBO, indexed by the item slot the TLAS
//     already carries as `instanceCustomIndex` — the key the reflection
//     trace's card read (rq_reflect.comp, jah_rq_card.glsl) looks a hit up by.
//
// WHY THE CAPTURE IS IN THE REAL SCENE MANAGER, and it is the whole reason this
// lane exists rather than an extension of the spike's shape. SURFACE-CACHE-0
// captured through a SCRATCH SceneManager holding one Item, and measured two
// facts that condemn it:
//
//   1. `Root::renderOneFrame` walks EVERY REGISTERED SceneManager's
//      `updateSceneGraph()` before any workspace runs (OgreRoot.cpp:1104-1110),
//      so a live scratch manager costs a graph walk and its barrier every frame
//      for as long as it EXISTS, capturing or not. A thousand card sets would
//      be a thousand managers walked per frame.
//   2. A one-item scene can only ever SELF-shadow, and a card's shadow term is
//      occlusion by OTHER objects. The spike's cards read shadow = 1.0
//      everywhere: the prepass's constant branch, not a measurement.
//
// So the capture runs in the scene it is caching, and bounds its cull to one
// object the only way Ogre's ANY-BIT visibility test allows — an INCLUDE
// channel: `kCardSubjectBit` is granted to the subject Item for the duration of
// its own capture passes and to nothing else, and the capture pass's visibility
// mask is that bit alone. The shadow node is a different question and its own
// mask: a shadow node draws its casters through `shadowCasterChannels(kind)`,
// which the pass mask does not touch, so the whole still world casts into the
// atlas while exactly one object is shaded out of it. That is what makes a
// card's shadow term real, and `gi.card_shadow` measures it.
//
// WHY A SCRATCH TARGET AND A COPY, rather than rendering straight into the
// atlas page. Vulkan's render-pass `renderArea` in this pin is the WHOLE
// attachment — `OgreVulkanRenderPassDescriptor.cpp:945-948` sets it from
// `mTargetWidth/mTargetHeight` and never from the viewport — so a
// `LoadAction::Clear` on a pass whose viewport is one page CLEARS THE WHOLE
// 2048-square atlas, wiping every other card. The alternatives are all worse
// than one copy: a page-sized `Type2DArray` slice per card makes the clear
// exact but forbids sub-allocation (a slice's clear wipes its co-tenants), and
// no clear at all leaves a card's empty texels holding the previous card's
// depth, which is precisely the value the read depth-tests against. So each
// card is captured into a page-sized scratch RTT that IS cleared whole, and
// `TextureGpu::copyTo` moves it into its rect of the atlas: five
// `vkCmdCopyImage` of at most 128 square per card, against a 2048-square clear
// avoided.
#pragma once

#include "jahshaka/engine/Types.h"

#include <Compositor/OgreCompositorWorkspaceListener.h>
#include <OgreQuaternion.h>
#include <OgreVector3.h>

#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>

#include <OgrePixelFormatGpu.h>

namespace Ogre {
class Camera;
class CompositorWorkspace;
class UavBufferPacked;
class Item;
class Light;
class HlmsComputeJob;
class VctLighting;
class CompositorPassSceneDef;
class Node;
class SceneManager;
class TextureGpu;
}   // namespace Ogre

namespace jahshaka {
namespace engine {

/// THE FIVE CAPTURED LAYERS, and the order every table here is in. (The
/// SIXTH, `Radiance`, is not captured — the `Jahshaka/CardLight` job writes it
/// from these five and the scene's lights; it is its own member, `mRadiance`.)
enum class CardLayer : unsigned {
    Albedo = 0,       ///< RGBA8_UNORM — kD, the datablock's diffuse ALREADY divided by pi
    Normal = 1,       ///< RGBA8_UNORM — the shading normal in the card's view space, *0.5+0.5
    Depth = 2,        ///< R16_FLOAT  — world units from the card's near plane
    Emissive = 3,     ///< RGB9E5 (or R11G11B10F — see emissiveFormatName) — radiance
    ShadowRough = 4,  ///< RG8_UNORM  — x = the shadow term, y = the GGX alpha
    Count = 5
};
constexpr unsigned kCardLayers = 5u;

/// THE PAGE, in texels a side. Lumen's number, and the one the tier budgets are
/// arithmetic on (a 128-texel card is 16,384 texels; a 327,680-texel budget is
/// twenty of them).
constexpr unsigned kCardPageSize = 128u;
/// The atlas, in texels a side. 2048 = 256 pages = about forty six-card sets at
/// full size. Phase 5 (SURFACE-CACHE-2) is where this grows a page table and a
/// feedback loop and stops being a fixed number.
constexpr unsigned kCardAtlasSize = 2048u;
/// THE SMALLEST CARD THE ALLOCATOR WILL CUT, and it is 16 because the
/// sub-allocation mask says so: a page that cuts cards of side S holds
/// (128/S)^2 slots, the free-slot mask is ONE uint64, and S = 16 is exactly 64
/// slots. At 8 it would be 256 — `1ull << s` for s >= 64 is undefined
/// behaviour, and on x86 the masked shift aliases slots 64..255 onto 0..63, so
/// three quarters of every 8-texel page would read as permanently taken while
/// every later allocation scanned all 256 pages looking for room. Any card
/// under about 12.5 cm reaches that path. A smaller floor needs a wider mask,
/// not a smaller constant.
constexpr unsigned kCardMinSize = 16u;
/// THE BATCH: how many cards ONE capture-workspace update carries — one
/// PASS_SCENE, one camera and one scratch slice each (CARD-BATCH-1). Eight
/// because the batch is gated by the workspace's execution mask, one bit a
/// pass, and Ogre's execution mask is a uint8.
constexpr unsigned kCaptureBatch = 8u;
/// The `CompositorPassDef::mIdentifier` of pass b of the batch is this + b —
/// how the per-pass listener knows which card a pass is capturing.
constexpr unsigned kCardPassIdentifier = 0x4A434300u;   // 'JCC\0'


/// ONE CARD, ALLOCATED AND (perhaps) CAPTURED.
///
/// The rectangle is in WORLD space: the instance's transform is applied when
/// the card is allocated, which is what lets phase 4's read project a world hit
/// into a card with three dot products and no per-instance matrix — and what
/// makes a MOVED instance's cards stale, which is exactly the invalidation
/// rule.
struct CardRec {
    unsigned instance = 0u;      ///< index into SurfaceCache::mInstances
    unsigned char axis = 0u;
    unsigned char lodLevel = 0u;
    Ogre::Vector3 centre;        ///< the card box's centre, world
    Ogre::Vector3 u, v, d;       ///< the world axis frame (d = outward)
    float halfU = 0.0f, halfV = 0.0f, halfDepth = 0.0f;   ///< world metres
    /// Where it lives in the atlas, in texels, and how big it is.
    unsigned atlasX = 0u, atlasY = 0u, size = 0u;
    /// Lumen's priority pair. `lastUsed` is bumped by the residency pass every
    /// frame the card is inside the radius (phase 4 will bump it from the GPU's
    /// own feedback instead); `lastUpdated` is the frame its capture landed.
    unsigned long long lastUsed = 0ull;
    unsigned long long lastUpdated = 0ull;
    bool queued = false;         ///< waiting for a capture
    /// THE LIT CARD: its radiance is stale (captured since it was relit, or a
    /// light's radiance signature moved), and the frame it was last relit.
    bool relight = false;
    unsigned long long lastRelit = 0ull;
    /// ...and its INDIRECT half: stale (captured since, or the chain
    /// re-injected), present at all in the cached layer, and when last marched.
    bool relightIndirect = false;
    /// The next capture changes the SURFACE (a new rect, a material), not only
    /// the shadow term — so it re-marches the indirect too.
    bool surfaceStale = false;
    bool indirectValid = false;
    unsigned long long lastIndirect = 0ull;
};

/// WHAT THE CACHE IS HANDED EACH FRAME, and the reason it is handed anything at
/// all. SURFACE-CACHE-0 made its card set a `friend` of `OgreScene` so it could
/// reach the item table and the SceneManager, and recorded that as debt: "a
/// Component takes what it needs as arguments". This is that argument. The
/// scene owns the walk (it already walks its items every frame for GI and for
/// the caster scan) and hands over the candidates; the Component owns the
/// atlas, the allocation, the queue and the capture, and knows nothing about
/// how a scene stores a node.
struct CardSceneView {
    Ogre::SceneManager *sceneMgr = nullptr;
    /// The authoritative view's tracked position — the same number the cascade
    /// chain centres on, for the same reason.
    Ogre::Vector3 viewerPos;
    unsigned budgetTexels = 0u;
    float    radius = 0.0f;
    /// THE LIGHT WRITE SERIAL — the one signature the cache still compares
    /// rather than being told about. A light is not owned by an instance, so
    /// there is nothing to be precise about: a light write stales every card's
    /// shadow term, and the counter is where a suite sees it.
    unsigned long long lightSerial = 0ull;
    /// THE RADIANCE SIGNATURE (PHOTON-CARDS-1): `lightSerial` plus what only a
    /// card's LIT radiance depends on (colour, power, reach, cone) — a change
    /// relights the resident set and recaptures nothing.
    unsigned long long radianceSerial = 0ull;
    /// The relight budget, texels a frame (GiQualityFacts::cardLightTexels).
    unsigned lightBudgetTexels = 0u;
    /// EVERY light of the scene, world space, no culling — the relight job's
    /// light list and its sun (a card lights surfaces no camera sees).
    std::vector<Ogre::Light *> lights;
    /// THE INDIRECT HALF: the chain the march reads (the scene's cascade-0
    /// VctLighting — the same object the pixel's pass buffer is filled from;
    /// null when GI is not the voxel arm, and the indirect is then zero), the
    /// signature that says it re-injected, and its own budget.
    Ogre::VctLighting *vct = nullptr;
    unsigned long long indirectSerial = 0ull;
    /// THE CLOUD LAYER'S SHADOW (CLOUDS-2D-2): the field and its mapping the
    /// pixel's direct sun is darkened by (JahCloudShadow's cloudMap / cloudSun),
    /// null when no layer shades the sun. Its change is in `radianceSerial`.
    Ogre::TextureGpu *cloudField = nullptr;
    float cloudMap[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    float cloudSun[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    unsigned indirectBudgetTexels = 0u;

    /// ONE CANDIDATE — an item inside the radius that may hold cards. The
    /// scene's own predicate decides membership (still-world GI geometry,
    /// shown, with a baked card list); the cache decides residency.
    struct Candidate {
        NodeId node = 0;
        /// The slot the TLAS carries as `instanceCustomIndex` — phase 4's key.
        size_t itemSlot = size_t(-1);
        Ogre::Item *item = nullptr;
        Ogre::Node *sceneNode = nullptr;
        MaterialId material = 0;
        const std::vector<MeshCardDesc> *cards = nullptr;
        /// The mesh's baked LOD BOUNDS. Carried for the cache's own diagnostics
        /// and for a future consumer; the CARD'S LEVEL is not derived from it —
        /// the bake owns that number (AT-CARDLOD, see OgreSurfaceCache.cpp).
        const std::vector<float> *lodBounds = nullptr;
    };
    std::vector<Candidate> candidates;
};

class SurfaceCache final : public Ogre::CompositorWorkspaceListener {
public:
    SurfaceCache();
    ~SurfaceCache();

    SurfaceCache(const SurfaceCache &) = delete;
    SurfaceCache &operator=(const SurfaceCache &) = delete;

    /// Allocates the atlas and the capture workspace in `sceneMgr`. Idempotent;
    /// false with `err` set when the device cannot render the formats the cache
    /// wants.
    bool build(Ogre::SceneManager *sceneMgr, std::string &err);
    /// Frees everything, in the order the pin requires (workspace, then
    /// definitions, then textures, then cameras).
    void destroyAll();
    bool built() const { return mBuilt; }

    /// THE PER-FRAME PASS, called from the scene's own frame hook while a frame
    /// is being prepared. It does four things in this order: reconciles the
    /// residency set against `view.candidates`, re-checks the three
    /// invalidation signatures, sorts the queue by Lumen's priority, and
    /// captures until the texel budget is spent.
    void update(const CardSceneView &view);

    /// Everything a host, a monitor or a suite can see.
    void fillStatus(CardCacheStatus &out) const;
    /// TEST AND TOOL: one card texel, all five layers, through an
    /// AsyncTextureTicket (flushCommands first).
    bool readTexel(NodeId node, unsigned card, float u, float v, CardSample &out) const;
    /// TEST AND TOOL: what the cache holds AT A WORLD POINT on a surface whose
    /// outward normal is `normal` — the question the ray job's read asks at a
    /// hit (jah_rq_card.glsl, the GPU port), answered here on the CPU as its
    /// reference (gi.card_read_parity).
    ///
    /// It is Lumen's own order: take the cards whose outward
    /// axis faces the normal, project the point into each with three dot
    /// products (the cards are captured in WORLD space, so there is no
    /// per-instance matrix), reject a card the point falls outside, reject one
    /// whose stored depth disagrees with the point's own distance from the card
    /// plane by more than a texel or two (this is what stops a card being read
    /// THROUGH a wall), and keep the one that faces the normal most squarely.
    /// `onlyNode` (0 = every card) restricts the pick to one instance's cards —
    /// the scope the ray job's read has (it knows the hit instance). The ray
    /// job's port is rayquery/include/jah_rq_card.glsl; this stays as its test
    /// reference (gi.card_read_parity).
    bool readAt(const Ogre::Vector3 &world, const Ogre::Vector3 &normal, CardSample &out,
                NodeId onlyNode = 0) const;
    /// TEST AND TOOL: every resident card's every layer as a PNG.
    bool dump(const std::string &prefix, std::string &err) const;

    /// THE ONE INVALIDATION DOOR the rest of the engine knocks on. It THROWS
    /// THE AFFECTED CARDS BACK ON THE QUEUE and nothing else — never a rebuild,
    /// never a reallocation, so a hover preview costs the cache the cards of
    /// the object under the mouse and not one texel more.
    ///
    /// (Everything ELSE the cache must notice arrives through the per-frame
    /// candidate list: an item that was destroyed, hidden, made a mover or
    /// re-classified simply stops being offered, and an item that MOVED fails
    /// the transform test. There is no "the node went away" door to forget to
    /// call, which is the shape a cache beside four others wants.)
    /// A material changed: the cards of every instance WEARING it go back on
    /// the queue, and nothing else does. Precise rather than generation-wide,
    /// because "a hover preview costs the object under the mouse" is the whole
    /// point of the model MATERIAL-SWAP-GI-1 built.
    void noteMaterialChanged(MaterialId material);

    Ogre::CompositorWorkspace *workspace() const { return mWs; }
    /// THE TWO BUFFERS THE RAY JOB'S CARD READ BINDS (rq_reflect.comp through
    /// jah_rq_card.glsl; see `syncBuffers`). `cardBuffer` holds one 80-byte
    /// record per allocated card, in the exact layout the shader reads;
    /// `instanceBuffer` is indexed by the ITEM SLOT the TLAS already carries as
    /// `instanceCustomIndex`, and each entry is (firstCard, cardCount, 0, 0).
    Ogre::UavBufferPacked *cardBuffer() const { return mCardBuffer; }
    Ogre::UavBufferPacked *instanceBuffer() const { return mInstanceBuffer; }
    /// How many card records the buffer currently describes, and how many
    /// instance slots the instance table holds.
    unsigned cardRecords() const { return mCardRecords; }
    unsigned instanceSlots() const { return mInstanceSlots; }
    /// ...and the two atlas layers the read samples: the captured Depth (the
    /// through-the-wall test) and the lit Radiance.
    Ogre::TextureGpu *depthLayer() const { return mAtlas[unsigned(CardLayer::Depth)]; }
    Ogre::TextureGpu *radianceLayer() const { return mRadiance; }
    /// The item slot of a node's instance, or -1 when it holds no cards.
    long itemSlotOf(NodeId node) const;
    /// Is a capture executing right now? The Hlms listener's pass property
    /// (`jah_card_capture`) is set from this and from nothing else.
    static bool capturing();

    /// THE BATCH'S HOOKS (OgreSurfaceCache.cpp, "The batch, as Ogre's frame
    /// executes it"): the frame-head flag reset, the timing, the per-pass
    /// subject grant, and the copies after the last pass.
    void allWorkspacesBeforeBeginUpdate() override;
    void workspacePreUpdate(Ogre::CompositorWorkspace *) override;
    void passPreExecute(Ogre::CompositorPass *) override;
    void passPosExecute(Ogre::CompositorPass *) override;
    void workspacePosUpdate(Ogre::CompositorWorkspace *) override;

private:
    struct InstanceRec {
        NodeId node = 0;
        size_t itemSlot = size_t(-1);
        Ogre::Item *item = nullptr;
        MaterialId material = 0;    ///< what it WEARS, for the material hook
        bool seen = false;          ///< present in this frame's candidate list
        unsigned firstCard = 0u, cardCount = 0u;
        float distance = 0.0f;
        /// The three signatures, as they were when this instance's cards were
        /// last ALLOCATED (the transform one) or CAPTURED (the other two).
        unsigned long long transformSig = 0ull;
        /// THE TRANSFORM SIGNATURE, whole: the world box AND the frame the
        /// cards were cut in. A turn leaves the box alone and moves every card.
        Ogre::Vector3 centre;
        Ogre::Vector3 halfSize;
        Ogre::Quaternion rotation;
        Ogre::Vector3 scale{ 1.0f, 1.0f, 1.0f };
    };

    // ---- the atlas + the page allocator -----------------------------------
    bool makeAtlas(std::string &err);
    bool makeWorkspace(std::string &err);
    /// One card, one (u, v) in [0, 1], five layers, through an
    /// AsyncTextureTicket. The one place a card parameter becomes an atlas
    /// texel — see its body for the v flip, which lives there and nowhere else.
    bool sampleCard(const CardRec &card, float u, float v, CardSample &out) const;
    /// Cuts `size` square texels out of the atlas. The allocator is a free list
    /// of PAGES plus a free list per sub-page size, which is the whole of
    /// Lumen's rule at this phase: a full page for anything at `kCardPageSize`,
    /// a quadtree split inside one page for anything smaller.
    bool allocRect(unsigned size, unsigned &x, unsigned &y);
    void freeRect(unsigned x, unsigned y, unsigned size);

    // ---- residency + capture ----------------------------------------------
    void refreshResidency(const CardSceneView &view);
    void releaseInstance(size_t idx);
    bool buildCardsFor(const CardSceneView::Candidate &cand);
    /// Aims batch slot `slot`'s camera at `card` (the pass bound to it runs
    /// later this frame, inside Ogre's own workspace update) and fits the
    /// pass's viewport to the card's texels.
    void aimCamera(const CardRec &card, unsigned slot);
    /// THE LIT CARD: plans this frame's relight list under the light budget
    /// (update), and records the job over it (workspacePosUpdate, after the
    /// capture's copies, with the frame's lights).
    void planRelights(const CardSceneView &view);
    void relightCards();
    /// Rebuilds the two GPU tables from `mCards` / `mInstances` and uploads
    /// them. Called only when the ALLOCATION changed — never per capture.
    void syncBuffers();

    Ogre::SceneManager *mSceneMgr = nullptr;
    bool mBuilt = false;

    Ogre::TextureGpu *mAtlas[kCardLayers] = {};
    Ogre::TextureGpu *mScratch[kCardLayers] = {};
    Ogre::TextureGpu *mScratchDepth = nullptr;
    /// THE SIXTH LAYER: radiance, written by the `Jahshaka/CardLight` job (a UAV,
    /// never a render target, never a copy destination).
    Ogre::TextureGpu *mRadiance = nullptr;
    std::string mRadianceFormatName;
    /// The relight job and its two per-frame tables: the cards to relight
    /// (80 bytes each) and the scene's lights in world space (a 16-byte count,
    /// then 80 bytes a light).
    Ogre::HlmsComputeJob *mLightJob = nullptr;
    Ogre::UavBufferPacked *mRelightBuffer = nullptr;
    Ogre::UavBufferPacked *mLightBuffer = nullptr;
    std::vector<unsigned> mRelight;          ///< this frame's relight list: indices into mCards
    /// ...and each entry's mode (JahCardLight_cs.glsl): 1 march the indirect,
    /// 0 read it back, 2 none yet.
    std::vector<unsigned> mRelightMode;
    std::vector<Ogre::Light *> mLights;      ///< this frame's scene lights (valid inside the frame)
    /// THE CACHED INDIRECT HALF (a UAV, R11G11B10F like the radiance), the
    /// chain's parameter block, and this frame's chain.
    Ogre::TextureGpu *mIndirect = nullptr;
    Ogre::UavBufferPacked *mGiBuffer = nullptr;
    std::vector<float> mGiCpu;
    Ogre::VctLighting *mVct = nullptr;
    /// This frame's cloud shadow (CardSceneView's), for the relight.
    Ogre::TextureGpu *mCloudField = nullptr;
    float mCloudMap[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    float mCloudSun[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    unsigned long long mIndirectSerial = 0ull;
    bool mIndirectMovingLastFrame = false;
    unsigned mIndirectBudget = 0u;
    unsigned mIndirectLastFrame = 0u, mIndirectTexelsLastFrame = 0u;
    unsigned long long mIndirectRelights = 0ull;
    unsigned long long mInvalidIndirect = 0ull;
    bool mIndirectOnLastRelight = false;
    /// Lights past kMaxCardLights last relight, and whether the cache has said so.
    unsigned mLightsDropped = 0u;
    bool mLightsDroppedLogged = false;
    std::vector<float> mRelightCpu, mLightCpu;
    unsigned long long mRadianceSerial = 0ull;
    bool mRadianceMovingLastFrame = false;
    unsigned mLightBudget = 0u;
    unsigned mRelitLastFrame = 0u, mRelitTexelsLastFrame = 0u;
    unsigned long long mRelights = 0ull;
    unsigned long long mInvalidRadiance = 0ull;
    float mLightMs = 0.0f;
    /// The batch's pass definitions (ours), for the per-card viewport.
    Ogre::CompositorPassSceneDef *mPassDef[kCaptureBatch] = {};
    Ogre::PixelFormatGpu mEmissiveFormat;
    std::string mEmissiveFormatName;

    Ogre::UavBufferPacked *mCardBuffer = nullptr;
    Ogre::UavBufferPacked *mInstanceBuffer = nullptr;
    unsigned mCardRecords = 0u;
    unsigned mInstanceSlots = 0u;
    /// THE TABLE, AS THE GPU WILL READ IT — kept CPU-side and uploaded when it
    /// changes, so the buffers are allocated once and never churn.
    std::vector<float> mCardBufferCpu;      ///< 24 floats a record
    std::vector<unsigned> mInstanceBufferCpu;
    bool mTableDirty = false;

    /// ONE capture camera per pass of the batch, bound for life.
    Ogre::Camera *mCam[kCaptureBatch] = {};
    /// THIS FRAME'S BATCH: indices into mCards, slot i = pass i. Planned by
    /// `update()`, executed by Ogre's frame, consumed by `workspacePosUpdate`.
    std::vector<unsigned> mBatch;
    /// The compositor frame the batch was planned for — a batch never runs in
    /// any other frame.
    size_t mBatchFrame = size_t(-1);
    std::chrono::steady_clock::time_point mBatchStart;
    /// The subject's flags and LOD as they were before its pass.
    Ogre::uint32 mSubjectFlags = 0u;
    unsigned char mSubjectLod = 0u;
    Ogre::CompositorWorkspace *mWs = nullptr;
    std::string mNodeDef, mWsDef;

    std::vector<InstanceRec> mInstances;
    std::vector<CardRec> mCards;
    std::unordered_map<NodeId, size_t> mByNode;
    /// The capture queue: indices into mCards, rebuilt and sorted each frame.
    std::vector<unsigned> mQueue;

    /// The page grid. `mPageUsed[p]` is 0 for free, kCardPageSize for a whole
    /// card, or the sub-allocation size in use inside it.
    std::vector<unsigned char> mPageOwner;   ///< 0 free, 1 whole, 2 sub-allocated
    std::vector<unsigned char> mPageSubSize; ///< the sub-card size this page cuts
    std::vector<unsigned char> mPageSubUsed; ///< how many sub-slots are taken
    std::vector<unsigned long long> mPageSubMask;  ///< which ones (64 slots exactly at kCardMinSize)
    /// Counted once a frame, not once per arrival, and published as-is.
    unsigned mPagesUsed = 0u;
    /// Raised by `allocRect` the moment it finds no room, so the arrival loop
    /// stops on the first failure instead of after the next success.
    bool mAtlasFull = false;

    unsigned long long mFrame = 0ull;
    unsigned long long mCaptures = 0ull;
    unsigned mCapturesLastFrame = 0u;
    unsigned mTexelsLastFrame = 0u;
    float    mCaptureMs = 0.0f;
    float    mWsMs = 0.0f, mCopyMs = 0.0f;
    float    mRadius = 0.0f;
    unsigned mBudget = 0u;
    unsigned long long mInvalidTransform = 0ull;
    unsigned long long mInvalidMaterial = 0ull;
    unsigned long long mInvalidLight = 0ull;
    /// The light signature as the last frame saw it, and whether it moved on
    /// that frame (so `invalidLight` counts gestures and not frames).
    unsigned long long mLightSerial = 0ull;
    bool mLightMovingLastFrame = false;
};

}   // namespace engine
}   // namespace jahshaka
