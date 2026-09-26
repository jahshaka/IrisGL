// HLMS ATOM — the visibility buffer's material decode, a DERIVED HlmsPbs on Ogre's
// own Terra pattern (SPECS/atom/D1_HLMS_ATOM_AND_VISBUF_DESIGN.md §0/§1; the
// stage-0 proof: spikes/atom-stage0/FINDINGS.md §2).
//
// WHAT IT IS. A second Hlms whose pixel shader INSERTS PBS's own pieces, so the
// lighting exists ONCE: every piece PBS includes — Ogre's and Photon's (the voxel
// cones, the irradiance field, the gather's read, the environment) — is a library
// of this Hlms too, and a lighting term that lands later as a piece reaches both
// hosts by construction. What it replaces is EXACTLY ONE upstream piece,
// `LoadMaterial` (the material index comes from the id buffer, not from a per-draw
// flat interpolant), plus `DeclareObjLightMask` under the fine-light-mask property
// (the same fact for the light mask, FINDINGS Q2). Upstream's `DefaultBodyPS` runs
// unchanged over a LOCAL `inPs` the decode prologue fills.
//
// WHAT A DECODE DRAW IS. One full-screen triangle (`AtomDecodeRenderable`) whose
// datablock is the DECODE TWIN of a BUCKET (`decodeTwinForBucket`): every PBS
// datablock that generates the same shader permutation, binds the same texture
// set and lives in the same const-buffer pool (`BucketKey`) is served by ONE twin,
// a clone of the first of them — so a decode costs one draw per bucket, never one
// per material (S3-DRAW; world.atomStatus reports materials against buckets).
// The twin carries the bucket's textures
// and flags, and HlmsAtom binds the bucket's PBS CONST-BUFFER POOL in place of the
// twin's own — so the id buffer's material word (`GpuInstance::raster[0]`,
// {pool:16 | slot:16} of the PBS datablock) addresses each member's own constants
// directly. The bucket table maps EVERY member's word to the twin's slot + 1, and a
// pixel whose word maps elsewhere is discarded.
//
// WHAT IT IS TOLD. Everything PBS is told, through `tellEveryHlms` (OgreEngine.cpp)
// — never a setter of its own that could disagree with PBS's — except the scene's
// GI arms, which it binds per pass from the pass's own scene (bindSceneGi), exactly
// as PBS does.
//
// ITS TWO PRODUCT CONSUMERS. The SCREEN (ATOM S3-DRAW, OgreAtomDraw.cpp): the id
// pass's image, shaded by one full-screen decode draw per bucket as the first draws
// of the view's prepass and opaque pass (syncScreenDecodes / showScreenDecodes).
// The RAY HITS (PHOTON-HIT-SHADE-1, SPECS/atom/D2_HIT_SHADING_DESIGN.md): in HIT
// MODE the same decode shades the ray jobs' compacted hit list — one fragment per
// record — in the chain's "Jahshaka hit decode" pass (syncSceneDecodes).
//
// Ogre-private: included only by the engine's Ogre TUs and by tests that reach
// past the public API (tests/atom).
#ifndef JAHSHAKA_ENGINE_HLMSATOM_H
#define JAHSHAKA_ENGINE_HLMSATOM_H

#include <OgreHlmsPbs.h>
#include <OgreMovableObject.h>
#include <OgreRenderable.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace Ogre {
class HlmsManager;
class HlmsPbsDatablock;
class HlmsSamplerblock;
class ReadOnlyBufferPacked;
class SceneNode;
class TextureGpu;
class UavBufferPacked;
}  // namespace Ogre

namespace jahshaka {
namespace engine {
namespace detail {

/// THE ONE FUNCTION every PBS-family host is told through (D1 §1; OgreEngine.cpp).
/// PBS is the source of truth — every engine site tells HlmsPbs — and this copies
/// what PBS holds onto every other PBS-family host: at registration (`force`) and
/// once per frame before a host's first read (HlmsAtom's analyzeBarriers /
/// preparePassHash). Nothing is left unrelayed at this pin but the scene's GI arms,
/// which are bound per pass (bindSceneGi, below).
void tellEveryHlms(Ogre::HlmsManager *manager, bool force = false);

/// THE SCENE BEING DRAWN IS THE SCENE THE SHADER READS (PHOTON-SCENE-SWITCH-1;
/// OgreGi.cpp, SceneGiBinding in EnginePrivate.h): points `host` at the VctLighting,
/// IrradianceField, PCC (+ its two distances), planar mirrors and IBL chain length
/// of the pass's own SceneManager — null arms (and a 1-level chain) for one that
/// registered none. Every PBS-family host calls it at the
/// head of its analyzeBarriers and preparePassHash, so it is NOT relayed by
/// tellEveryHlms.
void bindSceneGi(Ogre::HlmsPbs *host, const Ogre::SceneManager *sm);

/// The registered HlmsAtom's forgetDecodeTwinOf — a no-op before registration, after
/// Root, or for a datablock that is not PBS's. The one call every PBS-datablock
/// destruction site makes (OgreMaterials.cpp, OgreScene.cpp).
void forgetDecodeTwinOf(const Ogre::HlmsDatablock *pbs);

/// The registered HlmsAtom's forgetSceneManager — the scene's decode draws die
/// before its SceneManager does (OgreScene's and the engine's teardown).
void forgetSceneDecodes(Ogre::SceneManager *sm);

/// The render queue the product's decode draws live in (the parity suite's too):
/// every scene pass of the chain covers it, so the draws are shown ONLY for the
/// hit decode pass (HlmsAtom::showSceneDecodes, from its listener).
constexpr Ogre::uint8 kHitDecodeRenderQueue = 99u;
/// The queue the SCREEN decode draws live in (ATOM S3-DRAW): the FIRST draws of the
/// view's prepass and opaque pass. A decode neither tests nor writes depth, so it
/// must come before the PBS items (10) that may stand in front of an Atom item and
/// depth-test against the id pass's depth; after the sky (0), which the id depth
/// already rejects wherever an Atom item stands. Shown only while a pass that skips
/// the Atom queue runs (the view's listener).
constexpr Ogre::uint8 kScreenDecodeRenderQueue = 2u;

/// The id image's two words (R32G32_UINT), the contract between whatever WRITES the
/// id buffer (the hand-made one of engine.atom_parity today, S3-DRAW's id pass
/// tomorrow) and the decode:
///   x = the GPU scene's ITEM SLOT (bits 0-23) | the mesh LEVEL (bits 24-26);
///       0xFFFFFFFF = nothing covers this pixel
///   y = the TRIANGLE, counted from the first index of (that level, submesh 0)'s
///       range (`GpuMeshLevel::firstIndex`)
/// Submesh 0 only, like the voxel gather: every mesh the engine bakes has one.
struct AtomId {
    static constexpr uint32_t kEmpty = 0xFFFFFFFFu;
    static constexpr uint32_t kSlotBits = 24u;
    static constexpr uint32_t kSlotMask = (1u << kSlotBits) - 1u;
    static constexpr uint32_t kLevelMask = 0x7u;
    static uint32_t pack(uint32_t slot, uint32_t level) {
        return (slot & kSlotMask) | ((level & kLevelMask) << kSlotBits);
    }
};

class AtomDecodeRenderable;
class AtomKeyProbe;

class HlmsAtom final : public Ogre::HlmsPbs {
public:
    static constexpr Ogre::HlmsTypes kType = Ogre::HLMS_USER0;
    static const char *const kTypeName;  ///< "Atom"

    HlmsAtom(Ogre::Archive *dataFolder, Ogre::ArchiveVec *libraryFolders);
    ~HlmsAtom() override;

    /// PBS's library list, then the engine's own pieces (Hlms/Jahshaka, at the SAME
    /// position the engine gives them on PBS), then Hlms/Atom/Any; the data folder is
    /// Hlms/Atom/<syntax>. Terra's getDefaultPaths, with our folder last.
    static void getDefaultPaths(Ogre::String &outDataFolderPath,
                                Ogre::StringVector &outLibraryFoldersPaths);

    /// What the decode reads, set by whoever records the decode pass, immediately
    /// before it (the GPU scene's tables are RE-CREATED when they grow — never cache
    /// them across an attach, DOCS/traps/ENGINE.md). Null members decode nothing.
    struct DecodeSource {
        Ogre::TextureGpu *ids = nullptr;              ///< R32G32_UINT, the AtomId words
        Ogre::UavBufferPacked *instances = nullptr;   ///< GpuScene::instanceBuffer()
        Ogre::UavBufferPacked *levels = nullptr;      ///< GpuScene::levelBuffer()
        Ogre::UavBufferPacked *geomRows = nullptr;    ///< GpuScene::geomBuffer()
        /// HIT MODE (PHOTON-HIT-SHADE-1): `ids` is the hit list's RGBA32UI record
        /// image and `hitBuf` the ray jobs' buffer (word 0 = records appended; two
        /// words a record from word 4: the sun's visibility and the footprint as
        /// halves, the write-back weight — jah_rq_hit_record.glsl). The pass
        /// property atom_hit_mode selects the decode's hit branches.
        bool hitMode = false;
        Ogre::UavBufferPacked *hitBuf = nullptr;
    };
    void setDecodeSource(const DecodeSource &src);
    /// THE SHADER WARM-UP'S ARMING (OgreView::warmUpShaders): the empty stand-ins as
    /// the source (a warm-up pass compiles, it never draws) and the scene's screen
    /// decodes shown - including the ones the warm-up frame itself creates - so the
    /// warm-up compiles the decode twins' permutations for every scene pass it clones.
    void armForWarmUp(Ogre::SceneManager *sm, bool on);
    const DecodeSource &decodeSource() const { return mSource; }
    /// Decode draws recorded with NO source set (every member null or some) — each
    /// such draw binds the host's own EMPTY stand-ins (an id image that names nothing,
    /// zero-length tables), so it shades no pixel instead of dereferencing whatever a
    /// previous pass left in those slots. Counted, and logged once with the camera.
    unsigned long long sourcelessDraws() const { return mSourcelessDraws; }

    /// THE DECODE TWIN of a PBS datablock's BUCKET (bucketKeyOf): the bucket's one
    /// twin, created with the bucket's first member (a JSON round trip through Ogre's
    /// own serialiser, so every permutation-relevant field — textures, samplers,
    /// workflow, BRDF, maps, uv sets — is Ogre's copy, not a list of ours), its
    /// macroblock replaced by the decode's (no depth test or write, CULL_NONE: a
    /// full-screen triangle's winding is not the scene's — FINDINGS §2.4 (3)); a
    /// later datablock of the same bucket JOINS it (its word enters the bucket
    /// table). Returns null when `pbs` is not a PBS datablock, carries a
    /// per-datablock custom piece (the JSON cannot carry it; such a material stays
    /// on stock HlmsPbs) or the round trip fails (`err` says why). Idempotent.
    Ogre::HlmsPbsDatablock *decodeTwinForBucket(Ogre::HlmsPbsDatablock *pbs, std::string &err);
    /// Destroys every twin (and the bucket table). Called before the PBS datablocks
    /// they point at can die.
    void destroyDecodeTwins();
    /// A PBS DATABLOCK IS DYING OR CHANGED ITS PERMUTATION: it leaves its bucket
    /// BEFORE it dies — a twin keeps a member's pointer (fillBuffersForV2 binds its
    /// pool) and the maps are keyed by it, so a recycled address would find a stale
    /// bucket. The twin dies with its LAST member (and the epoch moves). Every
    /// engine site that destroys a PBS datablock calls `forgetDecodeTwinOf` first.
    void forgetDecodeTwinOf(const Ogre::HlmsDatablock *pbs);
    /// THE WITNESSES' FORGET (the screen split's and the ray tier's, one frame
    /// apart in the same frame): a datablock edited in place leaves its twin only
    /// when its BUCKET moved — its key now differs from the twin's, or it has none
    /// (pending, refused). A hash that moved inside the same bucket (PBS hashes a
    /// datablock whose textures just landed) keeps the twin, so the second witness
    /// to see the edit cannot destroy the draws the first one re-derived. True when
    /// it forgot.
    bool forgetDecodeTwinIfMoved(const Ogre::HlmsDatablock *pbs);
    /// Twins = buckets held (every scene), and the PBS datablocks they serve.
    size_t decodeTwinCount() const { return mTwins.size(); }
    size_t decodeMemberCount() const { return mTwinOfPbs.size(); }
    /// THE BUCKET a PBS datablock is shaded by (Twin::bucketId: never 0, never
    /// reused), 0 when no twin serves it. The Atom view's Buckets table (OgreScene::
    /// syncAtomViewTable) colours by this.
    uint32_t bucketIdOf(const Ogre::HlmsDatablock *pbs) const;

    /// THE PRODUCT'S DECODE DRAWS (PHOTON-HIT-SHADE-1): for a SceneManager whose
    /// scene runs the ray tier, the bucket twin of every PBS datablock its items
    /// wear (`materialWords`: GpuInstance::raster x, HlmsAtom::materialWordOf) and
    /// ONE AtomDecodeRenderable per BUCKET, hidden, at kHitDecodeRenderQueue. Draws
    /// of buckets no longer worn are destroyed. Called outside the compositor (the
    /// ray tier's per-scene update) whenever the scene's set or twinEpoch() moved.
    void syncSceneDecodes(Ogre::SceneManager *sm, const std::vector<uint32_t> &materialWords);
    /// Shows (for the hit decode pass only) or hides a SceneManager's decode draws.
    void showSceneDecodes(Ogre::SceneManager *sm, bool on);
    /// THE SCREEN DECODE'S DRAWS (ATOM S3-DRAW): the same buckets for the words the
    /// scene's ATOM items wear (the render-queue split), one draw per bucket at
    /// kScreenDecodeRenderQueue, hidden except while a view's pass that skips the
    /// Atom queue runs (showScreenDecodes, from that view's listener). Called
    /// outside the compositor (OgreScene::updateAtomDraw).
    void syncScreenDecodes(Ogre::SceneManager *sm, const std::vector<uint32_t> &materialWords);
    void showScreenDecodes(Ogre::SceneManager *sm, bool on);
    size_t screenDecodeCount(const Ogre::SceneManager *sm) const;
    /// Destroys a SceneManager's decode draws (before the manager dies).
    void forgetSceneManager(Ogre::SceneManager *sm);
    /// Moves whenever a twin dies — a scene's synced set may then name a word a
    /// NEW datablock now holds.
    unsigned long long twinEpoch() const { return mTwinEpoch; }
    size_t sceneDecodeCount(const Ogre::SceneManager *sm) const;

    /// THE MATERIAL WORD of a PBS datablock: {pool index : 16 | slot : 16} in HlmsPbs's
    /// const-buffer pool — what `GpuInstance::raster[0]` carries and what the decode's
    /// LoadMaterial indexes. 0xFFFFFFFF for anything that is not a PBS datablock.
    static uint32_t materialWordOf(const Ogre::HlmsDatablock *pbs);
    static constexpr uint32_t kNoMaterialWord = 0xFFFFFFFFu;
    /// THE TEXTURE SET of a PBS datablock as one key: every slot's texture and
    /// samplerblock (the public getters; the baked descriptor sets are Ogre's
    /// protected state). A same-slot texture swap keeps the Hlms hash — the
    /// property vector does not change — and a decode twin is a CLONE that
    /// resolved the OLD texture by name, so the twin's staleness witness carries
    /// this beside the hash (PHOTON-HIT-SHADE-1 audit F6). 0 for anything that
    /// is not a PBS datablock.
    static uint64_t textureSetKeyOf(const Ogre::HlmsDatablock *pbs);

    /// THE DECODE BUCKET (S3-DRAW, SPECS/atom/D3_S3_DRAW_DESIGN.md §2.2): what one
    /// decode draw can serve. A draw is one twin = one shader permutation, one texture
    /// descriptor set and one bound const-buffer pool (fillBuffersForV2 binds the
    /// twin's PBS pool at const slot 1, and the id's material word names a slot IN
    /// that pool), so two PBS datablocks share a draw exactly when all three agree:
    ///   permutation  the property set (and pieces) THIS Hlms generates for the
    ///                datablock over the full-screen triangle's vertex layout —
    ///                Ogre's own calculateHashFor, read from this Hlms's renderable
    ///                cache — minus the blend/macro state the twin replaces with its
    ///                own (a twin is opaque, depth-less and CULL_NONE whatever it
    ///                clones);
    ///   textures     textureSetKeyOf: every slot's texture and samplerblock, which
    ///                is what the baked descriptor sets are made of;
    ///   pool         the PBS pool index (materialWord >> 16).
    struct BucketKey {
        uint64_t permutation = 0u;
        uint64_t textures = 0u;
        uint32_t pool = 0u;
        bool operator==(const BucketKey &o) const {
            return permutation == o.permutation && textures == o.textures && pool == o.pool;
        }
    };
    struct BucketKeyHash {
        size_t operator()(const BucketKey &k) const {
            return size_t(k.permutation ^ (k.textures * 1099511628211ull) ^ (uint64_t(k.pool) << 48u));
        }
    };
    /// The bucket of a PBS datablock; false (with `err`) for anything the decode
    /// cannot serve — not a PBS datablock, no const-buffer slot, a per-datablock
    /// custom piece (the JSON twin cannot carry one). Computes a property set (no
    /// shader is compiled); main thread, outside a pass.
    bool bucketKeyOf(const Ogre::HlmsPbsDatablock *pbs, BucketKey &out, std::string &err);
    /// The datablock's textures are still being baked (its descriptor sets are
    /// dirty): PBS delays its own hash then, so its real bucket is not known yet —
    /// bucketKeyOf gives it a bucket of its own, and the screen split routes it to
    /// PBS (atomRouteFor: pending).
    static bool isBucketPending(const Ogre::HlmsDatablock *pbs);

    Ogre::uint32 fillBuffersForV2(const Ogre::HlmsCache *cache,
                                  const Ogre::QueuedRenderable &queuedRenderable, bool casterPass,
                                  Ogre::uint32 lastCacheHash,
                                  Ogre::CommandBuffer *commandBuffer) override;
    void analyzeBarriers(Ogre::BarrierSolver &barrierSolver,
                         Ogre::ResourceTransitionArray &resourceTransitions,
                         Ogre::Camera *renderingCamera, const bool bCasterPass) override;
    Ogre::HlmsCache preparePassHash(const Ogre::CompositorShadowNode *shadowNode, bool casterPass,
                                    bool dualParaboloid, Ogre::SceneManager *sceneManager) override;
    /// PBS's half reads pass state only its preparePassHash sets (the prepass MSAA
    /// depth texture, which HlmsPbs's constructor leaves UNINITIALISED — measured: a
    /// crash under the validation layer's heap once the pass was skipped). A pass this
    /// host skipped runs only the buffer manager's half.
    void postCommandBufferExecution(Ogre::CommandBuffer *commandBuffer) override;

    /// Registers of the reserved read-only buffers (mReservedTexBufferSlots): PBS's
    /// worldMatBuf is slot 0, then ours.
    static constexpr Ogre::uint8 kInstanceBufSlot = 1u;
    static constexpr Ogre::uint8 kLevelBufSlot = 2u;
    static constexpr Ogre::uint8 kGeomRowBufSlot = 3u;
    static constexpr Ogre::uint8 kBucketBufSlot = 4u;
    /// Hit mode's list buffer: the vertex stage covers only the used rows (its
    /// count), the pixel stage reads each record's sun and footprint. A BUFFER, not
    /// a second image: the pin's Vulkan table of PASS textures holds 32 slots
    /// (NUM_BIND_TEXTURES, its bounds assert compiled out), and with a second image
    /// the hit mode's last pass texture sat at slot 31, its last entry (measured
    /// from the generated shaders). The read-only buffer table is separate.
    static constexpr Ogre::uint8 kHitBufSlot = 5u;
    static constexpr Ogre::uint8 kReservedBufSlots = 6u;

protected:
    Ogre::Hlms::PropertiesMergeStatus notifyPropertiesMergedPreGenerationStep(
        size_t tid, Ogre::PiecesMap *inOutPieces) override;
    void setupRootLayout(Ogre::RootLayout &rootLayout, size_t tid) override;

private:
    void uploadBucketTable();

    void ensureStandIns();

    DecodeSource mSource;
    /// The renderable bucketKeyOf hands to calculateHashFor: the full-screen
    /// triangle's vertex layout wearing (unlinked) the datablock asked about.
    AtomKeyProbe *mKeyProbe = nullptr;
    const Ogre::HlmsSamplerblock *mPointSampler = nullptr;
    /// The stand-ins a sourceless draw binds (see sourcelessDraws).
    Ogre::TextureGpu *mEmptyIds = nullptr;
    Ogre::ReadOnlyBufferPacked *mEmptyBuf = nullptr;
    unsigned long long mSourcelessDraws = 0ull;
    /// The last pass's preparePassHash was skipped (no twins): PBS's pass state is
    /// not this pass's.
    bool mPassSkipped = true;

    /// ONE BUCKET: its twin, the member whose pool the draw binds (any member: the
    /// key holds the pool), and every member with its material word.
    struct Twin {
        Ogre::HlmsPbsDatablock *pbs = nullptr;
        Ogre::HlmsPbsDatablock *twin = nullptr;
        BucketKey key;
        /// THE BUCKET'S ID (never 0, never reused while the process lives): what the
        /// table holds and what the twin's draws carry in their per-draw word's .w
        /// (fillBuffersForV2). NOT the twin's slot: slots repeat across the twins'
        /// const-buffer pools (512 each), and two buckets must never claim a pixel.
        uint32_t bucketId = 0u;
        std::vector<std::pair<const Ogre::HlmsDatablock *, uint32_t>> members;
    };
    /// Keyed by the TWIN (what a draw carries).
    std::unordered_map<const Ogre::HlmsDatablock *, Twin> mTwins;
    std::unordered_map<const Ogre::HlmsDatablock *, Ogre::HlmsPbsDatablock *> mTwinOfPbs;
    std::unordered_map<BucketKey, Ogre::HlmsPbsDatablock *, BucketKeyHash> mTwinOfKey;
    /// THE BUCKET TABLE: for each PBS material word (pool * slotsPerPool + slot),
    /// the id of its bucket (Twin::bucketId), 0 = no bucket. The decode discards a
    /// pixel whose entry is not its own draw's bucket id (worldMaterialIdx.w).
    Ogre::ReadOnlyBufferPacked *mBucketBuf = nullptr;
    std::vector<uint32_t> mBucketMirror;
    bool mBucketDirty = true;
    uint32_t mTwinSerial = 0u;
    uint32_t mBucketSerial = 0u;
    unsigned long long mTwinEpoch = 0ull;

    /// The product's decode draws, per SceneManager: the hit decode's
    /// (syncSceneDecodes, kHitDecodeRenderQueue) and the screen decode's
    /// (syncScreenDecodes, kScreenDecodeRenderQueue), one draw per bucket each.
    struct SceneDecodes {
        Ogre::SceneNode *node = nullptr;
        struct Set {
            std::unordered_map<const Ogre::HlmsDatablock *, AtomDecodeRenderable *> draws;
            bool shown = false;
        };
        Set hit, screen;
    };
    std::unordered_map<Ogre::SceneManager *, SceneDecodes> mSceneDecodes;
    void destroySceneDraw(SceneDecodes &sd, const Ogre::HlmsDatablock *twin);
    void syncDraws(Ogre::SceneManager *sm, SceneDecodes &sd, SceneDecodes::Set &set,
                   const std::vector<uint32_t> &words, Ogre::uint8 renderQueue);
};

/// ONE FULL-SCREEN TRIANGLE drawn through Ogre's own RenderQueue (Ogre's
/// Samples/2.0/ApiUsage/CustomRenderable is the template), so `preparePassHash` has
/// bound the pass buffer, the lights, the shadow atlas and the Forward Clustered grid
/// for it by the time it draws. Its streams exist so HlmsAtom derives the same vertex
/// properties (hlms_normal, hlms_tangent4, hlms_uv_count) the geometry it decodes has;
/// the vertex shader consumes every one of them.
class AtomDecodeRenderable final : public Ogre::MovableObject, public Ogre::Renderable {
public:
    AtomDecodeRenderable(Ogre::IdType id, Ogre::ObjectMemoryManager *objectMemoryManager,
                         Ogre::SceneManager *manager, Ogre::uint8 renderQueueId);
    ~AtomDecodeRenderable() override;

    const Ogre::String &getMovableType() const override;
    const Ogre::LightList &getLights() const override;
    void getRenderOperation(Ogre::v1::RenderOperation &op, bool casterPass) override;
    void getWorldTransforms(Ogre::Matrix4 *xform) const override;
    bool getCastsShadows() const override;

private:
    Ogre::VertexArrayObject *mVao = nullptr;
};

}  // namespace detail
}  // namespace engine
}  // namespace jahshaka

#endif  // JAHSHAKA_ENGINE_HLMSATOM_H
