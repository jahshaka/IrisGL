// SURFACE-CACHE phase 2 — THE CAPTURE COMPONENT (SURFACE-CACHE-1b, 2026-09-21).
//
// WHAT THIS IS. `jahshaka::engine::SurfaceCache` is an Ogre-Next COMPONENT in
// the shape `PhotonVoxelLighting`, `IrradianceField` and `ParallaxCorrectedCubemap` are
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
//   * RESIDENCY BY THE READERS' DEMAND (PHOTON-7 stage 5, 5-2): a card is
//     resident because a ray's hit read it, and a face because a hit there
//     found no card; the least recently read card is evicted first when an
//     arrival needs its room (never by distance: the sphere is deleted).
//   * THE CAPTURE, in THE VIEW'S OWN SceneManager (below).
//   * INVALIDATION on the three signatures the voxel side already keeps — the
//     transform epoch, the material generation, the light write serial.
//   * A PER-FRAME TEXEL BUDGET with Lumen's priority, `lastRead - lastUpdated`.
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
// mask is that bit alone. The shadow term is not the capture's at all
// (ATOM-S3-CARDCAP): it is TRACED, one ray per card texel towards the sun
// against the still world's casters (the ray tier's TLAS, kRayMaskStillCaster),
// so the whole still world occludes the card while exactly one object is shaded
// into it — occlusion by other objects, and `gi.card_shadow` holds it to a
// reference trace from the same texel. (It was the card shadow node's PSSM, one
// fit and three caster passes per card: 77 % of a capture's cost, deleted.)
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
#include <Math/Simple/OgreAabb.h>
#include <OgreQuaternion.h>
#include <OgreVector3.h>

#include <chrono>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include <OgrePixelFormatGpu.h>
#include <OgreSharedPtr.h>
#include <Vao/OgreAsyncTicket.h>

namespace Ogre {
class Camera;
class CompositorWorkspace;
class UavBufferPacked;
class Item;
class Light;
class HlmsComputeJob;
class PhotonVoxelLighting;
class CompositorPassSceneDef;
class Node;
class SceneManager;
class TextureGpu;
}   // namespace Ogre

namespace jahshaka {
namespace engine {

/// THE SIX CAPTURED LAYERS, and the order every table here is in. (The lit
/// layers — `Radiance`, `Indirect`, `Flat` — are not captured: the
/// `Jahshaka/CardLight` job writes them from these six and the scene's lights;
/// they are their own members.)
enum class CardLayer : unsigned {
    Albedo = 0,       ///< RGBA8_UNORM — kD, the datablock's diffuse ALREADY divided by pi
    Normal = 1,       ///< RGBA8_UNORM — the shading normal in the card's view space, *0.5+0.5
    Depth = 2,        ///< R16_FLOAT  — world units from the card's near plane
    Emissive = 3,     ///< RGB9E5 (or R11G11B10F — see emissiveFormatName) — radiance
    ShadowRough = 4,  ///< RG16_UNORM — x = the shadow term, y = the GGX alpha (16 bits: the read's lobe, PHOTON-3)
    /// RGBA8_UNORM — THE SPECULAR LOBE (ENGINE-POLISH-PHOTON-1 stage 3): the voxel
    /// store's encoding (Voxelizer_piece_cs.any), rgb = ( F0 - 0.04 ) kS / 0.96 and
    /// a = kS, so F0 kS = 0.04 a + 0.96 rgb and a dielectric's 0.04 is exact; zero
    /// for the specular workflows, as in the store.
    Specular = 5,
    Count = 6
};
constexpr unsigned kCardLayers = 6u;

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


/// A footprint in the plane normal to the sun: the projected rectangle, and the top along the sun (OgreSurfaceCache.cpp,
/// "The movers' shadow"). Here rather than in the TU so the trace's scratch can be the cache's own (U4 stage 3).
struct CardSunFootprint {
    float a0 = 0.0f, a1 = 0.0f, b0 = 0.0f, b1 = 0.0f, top = 0.0f;
};

/// THE CARDS' SUN-SPACE GRID (ENGINE-UPGRADE-U4 stage 2; OgreSurfaceCache.cpp, "The cards' sun-space grid"): the
/// landed cards bucketed by their footprint's rectangle in the plane normal to the sun, built per traceSun call, so a
/// footprint (a moved caster's, a mover's) visits only the cards whose cells it covers. Scratch kept between calls.
struct CardSunGrid {
    float a0 = 0.0f, b0 = 0.0f, inv = 0.0f;   ///< the grid's origin in the sun frame and 1 / its cell side
    float a1 = 0.0f, b1 = 0.0f;               ///< the filed cards' far bounds (a query beyond them meets no cell)
    /// EVERY card's rectangle the grid was given (filed and wide), bounded — false where one is not finite: a search
    /// outside it meets no card at all.
    bool allBounded = false;
    float ua0 = 0.0f, ua1 = 0.0f, ub0 = 0.0f, ub1 = 0.0f;
    int nx = 0, ny = 0;
    std::vector<unsigned> start;               ///< per cell, its first entry in `items` (nx * ny + 1)
    std::vector<unsigned> items;               ///< card indices, cell by cell
    std::vector<unsigned> wide;                ///< cards no cell list holds (a footprint wider than the cells' bound,
                                               ///< or not finite): every query visits them
    std::vector<unsigned> stamp;               ///< per card, the last query that visited it
    unsigned query = 0u;
    std::vector<float> scratch;
    bool empty() const { return items.empty() && wide.empty(); }
};

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
    /// ITS PLACE IN ITS INSTANCE'S LAYOUT (every card the mesh's list cuts, resident or not: PHOTON-7 stage 5, 5-2 -
    /// an instance holds the faces its readers read) and the FACE BUCKET its outward axis falls in (the axis of its
    /// largest component and its sign, jah_rq_card.glsl's jahCardBucket) - the key a hit that found no card marks.
    unsigned short index = 0u;
    unsigned char bucket = 0u;
    /// Lumen's priority pair. `lastRead` is THE READERS' (the newest frame a ray's hit read the card, from the GPU's
    /// demand stamps; its admission's frame until then) - `readBy` the same per reader (0 the reflection, 1 the gather:
    /// each counts its window on its own clock); `lastUpdated` is the frame its capture landed. `since` is the frame it
    /// took its atlas cell: a stamp older than that is the cell's previous card's.
    unsigned long long lastRead = 0ull;
    unsigned long long readBy[2] = { 0ull, 0ull };
    /// THE CARD'S LAMP TABLE (PHOTON-7 stage 5, 5-3): the world ids (SceneLightList::documentId) of the lamps its last
    /// relight shaded, up to 16 - in the scene's one world order, the 16 with the largest share of the card's direct
    /// when more reach it - each texel's lamp bit j the traced visibility of lamp j (the SunVis layer's green and
    /// alpha); carried in the card's GPU record so every reader maps its own lamps to the bits by id.
    unsigned lampIds[16] = {};
    unsigned lampCount = 0u;
    /// ...and the lamps its last relight took through the FRACTIONAL byte (the SunVis layer's blue: the 5-3 fix): the
    /// area lights that reach it (never in the table) and the lamps past the table.
    unsigned areaLampCount = 0u, fractionalCount = 0u;
    /// Under the measuring arm cards.demandCount: the hits that read its cell, cumulative (the cell's, a row's rate).
    unsigned long long hits = 0ull;
    unsigned long long lastUpdated = 0ull;
    unsigned long long since = 0ull;
    bool queued = false;         ///< waiting for a capture
    /// THE LIT CARD: its radiance is stale (captured since it was relit, or a
    /// light's radiance signature moved), and the frame it was last relit.
    bool relight = false;
    unsigned long long lastRelit = 0ull;
    /// ...and its INDIRECT half: stale (captured since, or the chain
    /// re-injected), present at all in the cached layer, and when last marched.
    bool relightIndirect = false;
    bool indirectValid = false;
    unsigned long long lastIndirect = 0ull;
    /// THE MOVERS' TERM (PHOTON-CARDS-4): the card's rect in the mover-visibility
    /// layer holds a trace the relight must multiply in (`moverTraced`), and the
    /// card waits for a trace past the frame's budget (`moverPending`, since the
    /// frame `moverPendingSince` — the age the trace order serves oldest first).
    bool moverTraced = false;
    bool moverPending = false;
    unsigned long long moverPendingSince = 0ull;
    /// THE STILL WORLD'S SUN TERM (ATOM-S3-CARDCAP): stale and waiting for a
    /// trace past the frame's budget (since `stillPendingSince`), and the lift
    /// its last trace started at (CardSample::sunLift).
    bool stillPending = false;
    unsigned long long stillPendingSince = 0ull;
    float sunLift = 0.0f;
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
    /// THE SCENE'S ITEM SLOTS (the TLAS's custom indices): the instance table covers every one, so a hit on an item
    /// that holds no card can mark the face it wanted (PHOTON-7 stage 5, 5-2).
    unsigned itemSlots = 0u;
    /// THE MEASURING ARM "cards.demandCount": the readers count their carded and uncarded hits.
    bool demandCount = false;
    /// THE DEMAND'S CHANGE (PHOTON-7 stage 5, 5-2): what the readers' rays can hit changed this frame - an eye moved
    /// a card texel or turned a pixel's angle from the pose at the last change, geometry changed (an Edit, a Mover, a
    /// change with no box) or the cache was built (PhotonComponent::updateSurfaceCacheFrame). The demand stays OPEN
    /// for `demandSettleFrames` READING frames after it (GiQualityFacts::cardDemandSettleFrames: the readers' stamps
    /// the cache has harvested since the change, so a reader that has not run yet keeps it open); closed, the resident
    /// set holds (a moved instance's own cards excepted): at a still pose over a still world the demand was sampled for
    /// the settle and a later grazing hit is that sampling's noise, never new demand - every capture breaks the
    /// reflection's rest view-wide (reflectionReadsKey), and a face first grazed 500 frames into a still pose broke
    /// photon-glass's (gi.walk).
    bool demandChanged = false;
    unsigned demandSettleFrames = 10u;
    /// THE RAY TIER'S FOOTPRINT PER METRE of distance from the viewer (the ray
    /// rule's `sampleFootprintPerspective` at one metre, OgreScene::updateRayLevels):
    /// the near copy a sun ray hits may lie one footprint off the captured level,
    /// so the still trace's lift is sized by it (kSunLiftFootprints).
    float rayFootprintPerMetre = 0.0f;
    /// THE STALE SET FROM THE CHANGED-REGION LIST (PHOTON-7 stage 5, 5-1; L2: change drives work, never a serial): the
    /// frame's LAMP regions (a lamp's old and new spheres: a card whose box meets one has a stale DIRECT half - the
    /// lamp's light is exact by its range, JahCardLight_cs.glsl), its SLAB regions (a commit's entering and leaving
    /// slabs: a card in one reads another cascade now, its INDIRECT half is stale - a box sliding over a card leaves the
    /// store bit-identical, so no brick says so), and the overflow (the sun or the cloud sheet: every card's direct; the
    /// sky, a change with no box or a cascade injected whole: every card's indirect). The indirect's other changes are
    /// the GPU's accumulation (accumulateIndirect). `frame` is the engine's (the report's lag is counted in it).
    std::vector<Ogre::Aabb> directBoxes, indirectBoxes;
    bool allDirect = false, allIndirect = false;
    unsigned long long frame = 0ull;
    /// The relight budget, texels a frame (GiQualityFacts::cardLightTexels).
    unsigned lightBudgetTexels = 0u;
    /// An area light's strata a side on the cards' lamp trace (GiQualityFacts::cardAreaStrata).
    unsigned areaStrata = 8u;
    /// THE INDIRECT HALF: the chain the march reads (the scene's cascade-0
    /// PhotonVoxelLighting — the same object the pixel's pass buffer is filled from;
    /// null when GI is not the voxel arm, and the indirect is then zero), and its own budget.
    Ogre::PhotonVoxelLighting *vct = nullptr;
    /// THE CLOUD LAYER'S SHADOW (CLOUDS-2D-2): the field and its mapping the
    /// pixel's direct sun is darkened by (JahCloudShadow's cloudMap / cloudSun),
    /// null when no layer shades the sun. Its change is the list's sun overflow.
    Ogre::TextureGpu *cloudField = nullptr;
    float cloudMap[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    float cloudSun[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    unsigned indirectBudgetTexels = 0u;
    /// THE AMBIENT AT GI OFF (PHOTON-CARDS-5): the scene's own SH, 27 floats in
    /// world axes (Scene::setAmbientSh's order: the sky's SH x the Sky Light's
    /// gain, exactly what the engine pushes to HlmsPbs). With no chain the
    /// relight's environment half is this at the texel's normal; with one, the
    /// chain hands the same coefficients over and this is not read.
    const float *ambientSh = nullptr;

    /// ONE CANDIDATE — an item that may hold cards. The
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
        /// ITS WORLD BOX, measured by the scene this frame (the recomputing read:
        /// the cache plans before the frame's scene-graph update). The transform
        /// signature and the arrivals' order read it here.
        Ogre::Aabb box;
    };
    /// THE ITEMS THE CACHE ASKED FOR (SurfaceCache::takeDemand: its resident instances and the items whose faces the
    /// readers marked within the window) that CAN hold cards - never the whole scene (ATOM-CPU-WALKS-1).
    std::vector<Candidate> candidates;
};

/// THE SUN ON THE CARDS (PHOTON-CARDS-4; ATOM-S3-CARDCAP) — what the scene tells the
/// cache INSIDE the frame (after the scene graph and the GPU scene's update, so
/// every box and the moved set are this frame's), and the trace the ray tier
/// records for it. A card's sun visibility is the STILL world's term times the
/// MOVERS' term, both traced one ray per texel; see OgreSurfaceCache.cpp, "The sun
/// on the cards".
struct CardMoverBox {
    NodeId node = 0;
    Ogre::Vector3 min, max;      ///< the world AABB
};
struct CardCasterMove {
    NodeId node = 0;
    Ogre::Vector3 oldMin, oldMax;   ///< the world AABB at the previous transform
    Ogre::Vector3 newMin, newMax;
};
struct CardMoverFrame {
    /// Every traced SHADOW-CASTING mover of the scene (the TLAS's
    /// kRayMaskMoverCaster set), and whether the list changed this frame (a
    /// mover moved, arrived, left, or changed its class).
    std::vector<CardMoverBox> movers;
    bool moversChanged = false;
    /// The movers in the frame's moved set (a transform write — a walk without
    /// a pose change moves the node and counts — a flags change, a birth).
    std::vector<NodeId> moved;
    /// The STILL casters whose shadow this frame changed: a transform,
    /// visibility, caster-bit or class change, or a deletion (which names its
    /// one box as both old and new).
    std::vector<CardCasterMove> casterMoves;
};
struct CardMoverTrace {
    /// `count` records of 20 floats, the relight's own layout (the still mode's
    /// lift in float 3).
    const float *records = nullptr;
    unsigned count = 0u;
    /// `vis` = the R8 mover layer, `shadowRough` = the RG16 ShadowRough layer: both
    /// bound in either mode; `still` picks the one written (rq_card_movers.comp).
    Ogre::TextureGpu *depth = nullptr, *normal = nullptr, *vis = nullptr, *shadowRough = nullptr;
    /// ...the relight's light list and the SunVis layer (RG8: the sun's in r, the lamps' in g), bound in every mode;
    /// `lamps` (PHOTON-7 stage 5, 5-3) writes the lamps' traced visibility (the records' float 3 the lift, as the
    /// still mode's).
    Ogre::UavBufferPacked *lights = nullptr;
    Ogre::TextureGpu *sunVis = nullptr;
    Ogre::Vector3 toSun;
    float range = 0.0f;
    bool still = false;
    bool lamps = false;
    /// ...the lamps mode's area light strata a side (n: n^2 rays a texel an area light).
    unsigned areaStrata = 8u;
};
struct CardMoverHooks {
    std::function<bool(CardMoverFrame &)> frame;
    std::function<bool(const CardMoverTrace &)> trace;
    /// The ray tier's two traces' last GPU milliseconds — the movers' and the
    /// still world's monitor rows ("cards.movers" / "cards.still"; -1 outside a
    /// capture). The relight's own row ("cards.relight") is this cache's.
    std::function<void(float &, float &)> readTimes;
};

/// ONE CARD AS THE PHOTON VIEW DRAWS IT (PHOTON-VIEW-1, PhotonView::Cards): the
/// card's world rectangle as it was cut (the centre of the box face the capture
/// looks through, the two HALF-extent vectors along its u and v, the outward
/// normal), whether it HOLDS a capture, and how
/// many cache frames ago that capture landed.
struct PhotonCardQuad {
    Ogre::Vector3 centre, halfU, halfV, normal;
    bool held = false;
    unsigned long long age = 0ull;
};

class SurfaceCache final : public Ogre::CompositorWorkspaceListener {
public:
    /// THE CACHE NEVER WAITS FOR A SHADER WHILE ITS SCENE'S VIEWS DO NOT (ASYNC-SHADERS-1;
    /// View::setAsyncShaders, pushed by the scene every frame). On: the capture workspace
    /// compiles in the background and a batch that drew a placeholder or skipped a draw is
    /// DISCARDED, its cards left queued (a card is captured the moment its object appears,
    /// and a blocking capture waited for a brand-new material's compile on the UI thread —
    /// measured); the card relight skips while its permutation builds, the cards keeping
    /// their radiance and staying owed. Off (every offscreen and test scene): as before.
    void setAsyncShaders(bool on);
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
    /// THE READERS' DEMAND, TAKEN (PHOTON-7 stage 5, 5-2), before the frame's candidates are asked for: the read-backs
    /// that landed (a fixed lag of the cache's frames) folded into each card's `lastRead` and the face marks, and the
    /// item slots the cache wants offered - its resident instances' and every slot a reader marked within the window.
    void takeDemand(std::vector<uint32_t> &slots);
    /// THE GI FREEZE (PHOTON-2 round 2, M1's measuring arm): this frame captures, relights and traces nothing.
    void freezeFrame();
    /// THE CHANGED-REGION LIST'S CARD PRODUCER (PHOTON-7 stage 3): the world box of every instance this frame's plan
    /// relights a card of, once per instance (planRelights; empty before the frame's update or after a freeze).
    const std::vector<Ogre::Aabb> &relitInstanceBoxes() const { return mRelitBoxes; }
    /// ...and the instances whose cards the frame's own sun and mover trace added to the relight AFTER the plan (inside
    /// photon_update, after the list closed): taken by the next frame's plan, which lists them (the frame their light
    /// is first read in).
    void takeLateRelitBoxes(std::vector<Ogre::Aabb> &out) { out.swap(mLateRelitBoxes); mLateRelitBoxes.clear(); }

    /// THE INDIRECT HALF'S ACCUMULATION (PHOTON-7 stage 5, 5-1; JahCardIndirect_cs.glsl): one cascade's moves this
    /// frame - its passed bricks' (state word 4, the passed bit) and its injections' (word 7) - into every resident card's
    /// would-move, each weighted by a bound of the share of the card's gather it can fill, the card's sensitivity its
    /// darkest lit texel's code; a card at a code is listed. Called per cascade that moved (after its pass, before its
    /// stop and resume rules clear the passed bits), `finer` the next finer cascade's box (its bricks are skipped inside
    /// it); the frame's first call clears the list. endIndirectFrame reads the list back (a ring, a fixed lag of host
    /// frames); the cards it names relight their indirect half (planRelights).
    void accumulateIndirect(Ogre::UavBufferPacked *brickState, const unsigned dims[3], const Ogre::Vector3 &corner,
                            float brickSide, const Ogre::Aabb *finer, unsigned long long frame);
    void endIndirectFrame(unsigned long long frame);

    /// Everything a host, a monitor or a suite can see.
    void fillStatus(CardCacheStatus &out) const;
    /// THE PHOTON VIEW'S TABLE (PHOTON-VIEW-1): every allocated card of every
    /// resident instance, held (a capture landed) or waiting for its first.
    void photonQuads(std::vector<PhotonCardQuad> &out) const;
    /// ...and ITS GENERATION: moves whenever what photonQuads answers can change —
    /// a card allocated or released (an arrival, a departure, a moved instance's
    /// re-cut, a rebuild) or a capture landing (its age restarts). The view
    /// re-uploads its quads on a move (and on the age ramp's own clock, frame()).
    unsigned long long photonGeneration() const { return mPhotonGeneration; }
    /// The cache's frame counter (a card's age is counted in it).
    unsigned long long frame() const { return mFrame; }
    /// TEST AND TOOL: one card texel, all five layers, through an
    /// AsyncTextureTicket (flushCommands first).
    bool readTexel(NodeId node, unsigned card, float u, float v, CardSample &out) const;
    /// TEST AND TOOL: every resident card's lit radiance, whole (Scene::readCardRadiance): one download of the Radiance
    /// layer (flushCommands first), sliced per card.
    bool readRadiance(std::vector<CardRadiance> &out, unsigned long long &cacheFrame) const;
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
    /// The scene's in-frame answers and the ray tier's trace (PHOTON-CARDS-4,
    /// ATOM-S3-CARDCAP); set once by the scene that owns the cache, which exists
    /// only where rays run (OgreScene::updateSurfaceCache).
    void setMoverHooks(const CardMoverHooks &hooks) { mMoverHooks = hooks; }

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
    /// THE VIEW TERM'S FIVE (PHOTON-CARDS-5, jah_card_view.glsl): what the read
    /// needs besides the Radiance to restore the diffuse lobe's view term at a
    /// ray's own direction — the Indirect layer (the cached irradiance, the
    /// diffuse indirect half kD pi Dd-bar times it) and the FLAT layer (what is the
    /// same from every direction: the emissive and the specular lobe's
    /// re-emission, ENGINE-POLISH-PHOTON-1 stage 3 — outside the diffuse view
    /// restore), the ShadowRough layer (the roughness), and the Albedo and
    /// Normal layers (the stored normal, and in their alpha the texel's mean
    /// light direction the relight writes), and the SPECULAR layer (F0 and kS:
    /// the movers' gate cuts the lobe's indirect share with it, which the Flat
    /// term holds, and the lights' lobe reads it), and the SUNVIS layer (the sun's visibility
    /// the read's specular lobe of the sun takes, PHOTON-3). In the order
    /// jah_rq_card_bindings.glsl declares them from JAH_CARD_VIEW_BINDING_BASE.
    static constexpr unsigned kViewLayers = 7u;
    void viewLayers(Ogre::TextureGpu *out[kViewLayers]) const {
        out[0] = mIndirect;
        out[1] = mFlat;
        out[2] = mAtlas[unsigned(CardLayer::ShadowRough)];
        out[3] = mAtlas[unsigned(CardLayer::Albedo)];
        out[4] = mAtlas[unsigned(CardLayer::Normal)];
        out[5] = mAtlas[unsigned(CardLayer::Specular)];
        out[6] = mSunVis;
    }
    /// The item slot of a node's instance, or -1 when it holds no cards.
    long itemSlotOf(NodeId node) const;
    /// THE SUN THE CARDS STORE THE VISIBILITY OF (cardSun: the first visible shadow-casting
    /// directional light): the ray tier flags it in its light table, so the card read's specular
    /// lobe of the sun takes the texel's SunVis term (PHOTON-3).
    const Ogre::Light *readersSun() const { return cardSun(); }
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
    /// PHOTON_UPDATE (photon/PhotonComponent.h): the frame's card work after the capture
    /// workspace — the batch's movers term and the still sun trace (traceSun), the
    /// batch's close, the relight and the table — recorded by the component's photon_update
    /// pass (the GI workspace that follows the capture's). Nothing on a frame the capture
    /// workspace did not finish (a capture that threw).
    void photonUpdate();
    bool photonUpdateOwed() const { return mPosUpdated; }
    /// THE SUN TRACE'S CPU (ENGINE-UPGRADE-U4 stage 1, premise P1; inspect::SceneEcho::traceSun): the last traceSun
    /// call's whole time on the render thread, the still casters' moves it took, the cards it held, the frame it ran
    /// in and how many calls ran.
    struct TraceSunCost {
        double micros = 0.0;
        unsigned casterMoves = 0u, cards = 0u;
        unsigned long long frame = 0ull, calls = 0ull;
        /// THE STILL CASTERS' HIT SET of the call (step 1): the cards a moved still caster's old or new footprint
        /// shades, as a count and a digest of the per-card mask (FNV-1a over the card indices in order) — what a
        /// faster search must reproduce exactly.
        unsigned casterHits = 0u;
        unsigned long long casterHitDigest = 0ull;
        /// THE GRID'S GUARD (the arm "cards.sunGridParity", gi.trace_sun_cost): calls checked against the exhaustive
        /// search of every footprint against every card, and the cards whose result differed (0 is the law).
        unsigned long long parityChecks = 0ull, parityMisses = 0ull;
        /// ...of `micros`: the scene's moves frame (the hook: PhotonComponent::cardMoverFrame, the producer of the
        /// moves and the movers) and step 1 (the still casters' search, their footprints included).
        double hookMicros = 0.0, stepOneMicros = 0.0;
        /// ...and the movers' searches: step 3 (the changed footprints' cards, with the grid they build) and step 3b
        /// (the pending cards a current footprint reaches); the rest is the step-2 bookkeeping, the trace's records and
        /// the relight hand-off.
        double step3Micros = 0.0, step3bMicros = 0.0;
        /// ...and the rest, named (U4 decision 12.3): step 1b (the still trace of the stale cards the hits made pending
        /// — the per-hit term), step 2 (the movers' changed boxes), step 4 (the trace list's order and the budget's pick)
        /// and the movers' trace job (recorded, with the relight hand-off).
        double stillMicros = 0.0, step2Micros = 0.0, step4Micros = 0.0, traceJobMicros = 0.0;
        /// The cards the call's grids held apart (`wide`: wider than the cells' bound, or not finite) — the most of
        /// its grids, so a row can say its wide path ran.
        unsigned wideCards = 0u;
    };
    const TraceSunCost &traceSunCost() const { return mTraceSunCost; }

private:
    struct InstanceRec {
        NodeId node = 0;
        size_t itemSlot = size_t(-1);
        Ogre::Item *item = nullptr;
        MaterialId material = 0;    ///< what it WEARS, for the material hook
        bool seen = false;          ///< present in this frame's candidate list
        unsigned firstCard = 0u, cardCount = 0u;
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
    /// THE INSTANCE'S LAYOUT: every card its mesh's list cuts, in WORLD space at the candidate's transform (`index` its
    /// place, `bucket` its face), none allocated.
    void layoutCards(const CardSceneView::Candidate &cand, std::vector<CardRec> &out) const;
    /// THE ATLAS MADE TO FIT (5-2's compaction): the least-used sub-cut page whose cards the other pages cutting its
    /// size can hold is emptied into them (each moved card re-captured where it lands), so a set whose page demand fits
    /// fits. False when no page can be freed that way.
    bool compactOnePage(std::vector<std::vector<CardRec>> &cards);
    /// The demand read-back: issued every kDemandReadEvery frames, taken a fixed lag later; and the head (the frame's
    /// stamp, the count arm) uploaded every frame.
    void issueDemandRead();
    void uploadDemandHead();
    /// Aims batch slot `slot`'s camera at `card` (the pass bound to it runs
    /// later this frame, inside Ogre's own workspace update) and fits the
    /// pass's viewport to the card's texels.
    void aimCamera(const CardRec &card, unsigned slot);
    /// THE LIT CARD: plans this frame's relight list under the light budget
    /// (update), and records the job over it (workspacePosUpdate, after the
    /// capture's copies, with the frame's lights).
    void planRelights(const CardSceneView &view);
    void relightCards();
    /// THE SUN ON THE CARDS, in the frame after the capture's copies and before the
    /// relight: the still world's term (the batch just captured, and the cards a
    /// sun change or a moved still caster made stale) and the movers' (the cards
    /// their sun-projected footprints reach) — records both traces and hands the
    /// traced and the retired cards to the relight.
    void traceSun();
    /// The still trace's lift for `card` (kSunLiftFootprints; CardSample::sunLift).
    float sunLiftOf(const CardRec &card) const;
    /// The first visible shadow-casting directional light of the scene's one light
    /// list (SceneLightList.h; the relight's sun, the traces' direction), or null.
    const Ogre::Light *cardSun() const;
    /// Rebuilds the two GPU tables from `mCards` / `mInstances` and uploads
    /// them. Called only when the ALLOCATION changed — never per capture.
    void syncBuffers();

    Ogre::SceneManager *mSceneMgr = nullptr;
    bool mBuilt = false;

    Ogre::TextureGpu *mAtlas[kCardLayers] = {};
    Ogre::TextureGpu *mScratch[kCardLayers] = {};
    Ogre::TextureGpu *mScratchDepth = nullptr;
    /// THE RADIANCE: written by the `Jahshaka/CardLight` job (a UAV,
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
    std::vector<Ogre::Aabb> mRelitBoxes;     ///< relitInstanceBoxes(): the plan's instances, one box each
    std::vector<unsigned> mRelitInstances;   ///< ...their instance indices (the late relights' filter)
    std::vector<Ogre::Aabb> mLateRelitBoxes; ///< takeLateRelitBoxes()
    /// ...and each entry's mode (JahCardLight_cs.glsl): 1 march the indirect,
    /// 0 read it back, 2 none yet.
    std::vector<unsigned> mRelightMode;
    /// THE CACHED INDIRECT HALF (a UAV, R11G11B10F like the radiance), per channel:
    /// the diffuse half where the channel has a diffuse albedo, the specular lobe's
    /// indirect share where it has none (kD = 0: a metal) - so a relight recovers the
    /// irradiance and re-emits both lobes from it (ENGINE-POLISH-PHOTON-1 stage 3);
    /// the chain's parameter block, and this frame's chain.
    Ogre::TextureGpu *mIndirect = nullptr;
    /// THE FLAT TERM (a UAV, the radiance's format, ENGINE-POLISH-PHOTON-1 stage 3):
    /// the emissive plus the specular lobe's re-emission of the GATHERED light (its
    /// hemispherical integral) - what a texel sends the same way to every direction. The
    /// lights' lobe is evaluated at the read, toward each ray (PHOTON-3). The read adds
    /// it outside the diffuse view restore; the Radiance holds it too. +16 MB at 2048 square.
    Ogre::TextureGpu *mFlat = nullptr;
    /// THE SUN'S VISIBILITY AT THE TEXEL (PHOTON-3 stage 2; a UAV, R8): the still world's traced
    /// term times the movers' and the cloud sheet's, as the relight shades the texel - the weight
    /// the read's specular lobe of the sun takes (every light's lobe is evaluated at the read,
    /// toward its own ray: jah_rq_card_lights.glsl). +4 MB at 2048 square.
    Ogre::TextureGpu *mSunVis = nullptr;
    /// THE MOVERS' VISIBILITY (PHOTON-CARDS-4): R8 over the atlas, written by the
    /// ray tier's trace (rq_card_movers.comp), read by the relight where the card
    /// is `moverTraced`. +4 MB at 2048 square.
    Ogre::TextureGpu *mMoverVis = nullptr;
    CardMoverHooks mMoverHooks;
    /// Each mover's footprint box as last traced (its OLD footprint when it
    /// moves), by node.
    std::unordered_map<NodeId, std::pair<Ogre::Vector3, Ogre::Vector3>> mMoverLast;
    std::vector<CardMoverBox> mMovers;
    Ogre::Vector3 mMoverSun = Ogre::Vector3::ZERO;
    Ogre::Vector3 mViewerPos = Ogre::Vector3::ZERO;
    std::vector<float> mMoverCpu;
    /// This frame's relight additions from the movers (card index, mode).
    unsigned mMoverTracedLastFrame = 0u, mMoverTexelsLastFrame = 0u, mMoverPending = 0u,
             mMoverPendingAge = 0u;
    unsigned long long mMoverTraces = 0ull, mMoverRetired = 0ull;
    /// THE STILL TERM (ATOM-S3-CARDCAP): the sun it was last traced towards (ZERO =
    /// no sun), whether the sun moved last frame (`invalidSun` counts gestures),
    /// the records, and the counters CardCacheStatus names.
    Ogre::Vector3 mStillSun = Ogre::Vector3::ZERO;
    bool mStillSunKnown = false;
    bool mSunMovingLastFrame = false;
    std::vector<float> mStillCpu;
    float mRayFootprintPerMetre = 0.0f;
    float mStillCpuMs = 0.0f;   ///< the still trace's CPU this frame (in captureMs with a batch)
    TraceSunCost mTraceSunCost;
    CardSunGrid mSunGrid;
    /// THE TRACE'S SCRATCH (U4 stage 3, decision 11 e): every per-call list traceSun builds, kept between calls and
    /// cleared, never re-allocated (the grid's own storage already is).
    struct TraceScratch {
        std::vector<CardSunFootprint> cardFp, reach;
        std::vector<char> cardFpHave, hit, inside, reachedBy;
        std::vector<unsigned> landedCards, filedCards;
    } mTrace;
    /// The arms "cards.sunGridParity" and "cards.nullMove" as the frame latched them (the owner sets them before each
    /// trace).
    bool mSunGridParity = false, mNullMove = false;
public:
    void setSunGridParity(bool on) { mSunGridParity = on; }
    void setNullMove(bool on) { mNullMove = on; }
private:
    unsigned mStillTracedLastFrame = 0u, mStillTexelsLastFrame = 0u, mStillPending = 0u;
    unsigned long long mStillTraces = 0ull, mInvalidSun = 0ull, mCasterRetraces = 0ull;
    Ogre::UavBufferPacked *mGiBuffer = nullptr;
    std::vector<float> mGiCpu;
    Ogre::PhotonVoxelLighting *mVct = nullptr;
    /// This frame's cloud shadow (CardSceneView's), for the relight.
    Ogre::TextureGpu *mCloudField = nullptr;
    float mCloudMap[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    float mCloudSun[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    /// The scene's SH for the GI-off environment half (CardSceneView::ambientSh).
    float mAmbientSh[27] = {};
    bool mIndirectMovingLastFrame = false;
    unsigned mIndirectBudget = 0u;
    unsigned mIndirectLastFrame = 0u, mIndirectTexelsLastFrame = 0u;
    unsigned long long mIndirectRelights = 0ull;
    unsigned long long mInvalidIndirect = 0ull;
    bool mIndirectOnLastRelight = false;
    /// Lights past kMaxCardLights last relight, and whether the cache has said so.
    unsigned mLightsInRange = 0u;
    unsigned mLightsDropped = 0u;
    bool mLightsDroppedLogged = false;
    std::vector<float> mRelightCpu, mLightCpu;
    bool mRadianceMovingLastFrame = false;
    /// THE INDIRECT HALF'S STALE SET (accumulateIndirect): the resident cards' boxes and slots (two vec4 a record, synced
    /// with the table), the per-slot state (4 words a 16-texel cell of the atlas), the report (its count, then slots),
    /// its read-backs (a ring at a fixed lag of host frames), the two jobs, the frame the report was cleared in, and the
    /// slot each card holds (the report's map back).
    Ogre::UavBufferPacked *mCardBoxBuffer = nullptr;
    Ogre::UavBufferPacked *mCardSlotState = nullptr;
    Ogre::UavBufferPacked *mCardReport = nullptr;
    Ogre::UavBufferPacked *mCardCluster = nullptr;   ///< a cascade's clusters' largest moves (the reduce's output)
    std::vector<float> mCardBoxCpu;
    struct ReportTicket {
        Ogre::AsyncTicketPtr ticket;
        unsigned long long frame = 0ull;
    };
    static constexpr unsigned kReportSlots = 8u;
    ReportTicket mReportReadings[kReportSlots];
    unsigned mReportNextIssue = 0u, mReportNextTake = 0u;
    Ogre::HlmsComputeJob *mAccumJob = nullptr;
    Ogre::HlmsComputeJob *mSensJob = nullptr;
    unsigned long long mAccumClearFrame = ~0ull;
    bool mAccumRan = false;
    std::vector<int> mSlotCard;
    unsigned long long mIndirectReported = 0ull, mIndirectByRegion = 0ull, mIndirectOverflows = 0ull;
    bool ensureIndirectBuffers();
    /// The relight's light list, made (empty) before a trace binds it (rq_card_movers.comp binds it in every mode).
    Ogre::UavBufferPacked *ensureLightBuffer();
    /// THE LAMPS' TRACE (PHOTON-7 stage 5, 5-3): its records (the relight's, the lift in float 3), and its counts.
    std::vector<float> mLampCpu;
    unsigned long long mLampTraces = 0ull;
    unsigned mLampTracedLastFrame = 0u;
    /// The most lamps that reached one relit card, and the largest share of a card's direct its table could not hold
    /// (past its 16), since the cache was built.
    unsigned mCardLampsMax = 0u;
    float mLampOverflowShareMax = 0.0f;
    unsigned mCardAreaLampsMax = 0u;
    unsigned long long mLampRaysLastTrace = 0ull;
    unsigned mAreaStrata = 8u;
    void harvestIndirectReports(unsigned long long frame);
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
    /// THE GI FREEZE's frame (freezeFrame): photonUpdate does nothing.
    bool mFrozen = false;
    /// The compositor frame the batch was planned for — a batch never runs in
    /// any other frame.
    size_t mBatchFrame = size_t(-1);
    std::chrono::steady_clock::time_point mBatchStart;
    /// The view's placeholder draws + pending skips when this batch began (ASYNC-SHADERS-1):
    /// a batch that moved them drew a placeholder or a hole and is discarded.
    /// See setAsyncShaders.
    bool mAsyncShaders = false;
    unsigned long long mBatchPlaceholderAt = 0u;
    unsigned long long mBatchSkipsAt = 0u;
    /// Batches discarded because a shader was still being built (ASYNC-SHADERS-1).
    unsigned long long mCapturesDeferredForShaders = 0u;
    /// The subject's flags and LOD as they were before its pass.
    Ogre::uint32 mSubjectFlags = 0u;
    unsigned char mSubjectLod = 0u;
    Ogre::CompositorWorkspace *mWs = nullptr;
    std::string mNodeDef, mWsDef;
    /// The capture workspace finished this frame (its workspacePosUpdate ran), and
    /// copied a batch photon_update closes.
    bool mPosUpdated = false;
    bool mBatchCopied = false;

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

    unsigned long long mFrame = 0ull;
    unsigned long long mPhotonGeneration = 0ull;   ///< photonGeneration()
    unsigned long long mCaptures = 0ull;
    unsigned mCapturesLastFrame = 0u;
    unsigned mTexelsLastFrame = 0u;
    float    mCaptureMs = 0.0f;
    float    mWsMs = 0.0f, mCopyMs = 0.0f;
    unsigned mBudget = 0u;
    unsigned long long mInvalidTransform = 0ull;
    unsigned long long mInvalidMaterial = 0ull;
    /// THE READERS' DEMAND (PHOTON-7 stage 5, 5-2; jah_rq_card_bindings.glsl's layout after the instance table): the
    /// scene's item slots (the table covers them all), the count arm, the read-backs in flight (the cache frame each
    /// was issued in and the table's slot count then), the face marks (per item slot, 8 words: the newest frame a hit
    /// there found no card, per face bucket), and the counters.
    unsigned mItemSlots = 0u;
    bool mDemandCount = false;
    struct DemandTicket {
        Ogre::AsyncTicketPtr ticket;
        unsigned long long frame = 0ull;
        unsigned slots = 0u;
    };
    static constexpr unsigned kDemandSlots = 4u;
    DemandTicket mDemandReadings[kDemandSlots];
    unsigned mDemandNextIssue = 0u, mDemandNextTake = 0u;
    std::vector<unsigned long long> mFaceMarks;   ///< 16 a slot: 8 per reader, the 6 buckets
    std::vector<unsigned long long> mFacePrev;    ///< ...and each one's mark before (a demand that persists: two)
    /// THE READERS' CLOCKS: per reader, the newest stamp a read-back held (its window counts in it: a reader at rest
    /// reads nothing, and nothing it read goes stale while it rests); and the cache frame of the last change in what
    /// the rays can hit (CardSceneView::demandChanged) - the demand is open until a reader's harvested stamp passes it
    /// by the settle.
    unsigned long long mDemandNewest[2] = { 0ull, 0ull };
    unsigned long long mDemandChangeFrame = 0ull;
    unsigned long long mAdmitted = 0ull, mEvicted = 0ull, mCompactionMoves = 0ull;
    unsigned long long mDemandHits[4] = { 0ull, 0ull, 0ull, 0ull };
};

}   // namespace engine
}   // namespace jahshaka
