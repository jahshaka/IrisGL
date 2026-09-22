// THE GPU SCENE — the tables, the grow, the one write path (GpuScene.h carries
// the design's rationale; SPECS/atom/A3_GPU_SCENE_SLICE_DESIGN.md §1).
//
// NOTHING IN HERE TOUCHES VULKAN. The tables are Ogre `UavBufferPacked`s in
// device-local memory and every write goes through ONE staging buffer per frame
// whose `unmap` carries a Destination per contiguous run of slots — which is
// Ogre's own spelling of "one vkCmdCopyBuffer per run, recorded, never waited
// on" (OgreBufferInterface.cpp's non-shared path). That keeps the facility alive
// on a platform with no ray queries and lets both an `HlmsComputeJob` (Atom P3's
// cull) and a raw descriptor set (the ray tier's instance job) bind the same
// buffer.
//
// A GROW IS A RE-CREATE WITH THE MIRROR AS THE INITIAL DATA, and that is a copy
// of every entry rather than a loss of them: the CPU mirror is authoritative, so
// the new buffer is born holding exactly what the old one held. It happens by
// DOUBLING, so a scene that grows to 8,001 items pays it eight times in its
// life and never per frame (the 0071 lesson). The old buffer goes to Ogre's
// delayed destruction, which holds it for the frames still in flight — so a
// TLAS build or a compute job recorded last frame keeps reading valid memory.
#include "EnginePrivate.h"
#include "GpuScene.h"

#include <OgreLogManager.h>
#include <OgreMesh2.h>
#include <Vao/OgreAsyncTicket.h>
#include <Vao/OgreStagingBuffer.h>
#include <Vao/OgreUavBufferPacked.h>
#include <Vao/OgreVaoManager.h>

#include <algorithm>
#include <chrono>
#include <cstring>

namespace jahshaka {
namespace engine {
namespace detail {

namespace {
/// The first capacity. Small enough that a thumbnail scene of three nodes does
/// not reserve a megabyte, large enough that the default editor scene never
/// grows at all.
const uint32_t kInitialSlots = 64u;
const uint32_t kInitialMeshes = 32u;

uint32_t roundUpPow2(uint32_t v, uint32_t floorValue) {
    uint32_t n = floorValue;
    while (n < v) n *= 2u;
    return n;
}
}  // namespace

GpuScene::~GpuScene() { destroy(); }

bool GpuScene::create(Ogre::VaoManager *vao, std::string &err) {
    if (mInstanceBuffer) return true;
    if (!vao) {
        // THE HEADLESS FALLBACK, stated rather than hidden: the NULL render
        // system has no VaoManager, so there are no tables and every consumer
        // keeps the CPU path it already has.
        err = "gpuscene: no VaoManager (the NULL render system) — the tables are not created";
        return false;
    }
    mVao = vao;
    mSlotCapacity = 0;
    mMeshCapacity = 0;
    growTo(kInitialSlots);
    growMeshTable(kInitialMeshes);
    return mInstanceBuffer != nullptr;
}

void GpuScene::destroy() {
    if (mVao) {
        if (mInstanceBuffer) mVao->destroyUavBuffer(mInstanceBuffer);
        if (mMeshBuffer) mVao->destroyUavBuffer(mMeshBuffer);
        if (mLevelBuffer) mVao->destroyUavBuffer(mLevelBuffer);
    }
    mInstanceBuffer = mMeshBuffer = mLevelBuffer = nullptr;
    mVao = nullptr;
    mMirror.clear();
    mBorn.clear();
    mMeshMirror.clear();
    mLevelMirror.clear();
    mMeshEntries.clear();
    mFreeMeshSlots.clear();
    mMeshIndex.clear();
    mMovedLastFrame.clear();
    mCopySet.clear();
    mSlotCapacity = mSlotCount = mMeshCapacity = 0u;
}

void GpuScene::growTo(uint32_t capacity) {
    if (!mVao || capacity <= mSlotCapacity) return;
    const uint32_t want = roundUpPow2(capacity, std::max(kInitialSlots, mSlotCapacity ? mSlotCapacity : kInitialSlots));
    mMirror.resize(want);
    mBorn.resize(want, 0u);
    if (mInstanceBuffer) {
        mVao->destroyUavBuffer(mInstanceBuffer);
        mInstanceBuffer = nullptr;
        ++mGrows;
        ++mGeneration;
    }
    mInstanceBuffer = mVao->createUavBuffer(want, sizeof(GpuInstance), 0, mMirror.data(), false);
    mSlotCapacity = want;
}

void GpuScene::growMeshTable(uint32_t capacity) {
    if (!mVao || capacity <= mMeshCapacity) return;
    const uint32_t want = roundUpPow2(capacity, std::max(kInitialMeshes, mMeshCapacity ? mMeshCapacity : kInitialMeshes));
    mMeshMirror.resize(want);
    mLevelMirror.resize(size_t(want) * kLevelsPerMesh);
    if (mMeshBuffer) {
        mVao->destroyUavBuffer(mMeshBuffer);
        mMeshBuffer = nullptr;
        ++mGeneration;
    }
    if (mLevelBuffer) {
        mVao->destroyUavBuffer(mLevelBuffer);
        mLevelBuffer = nullptr;
    }
    mMeshBuffer = mVao->createUavBuffer(want, sizeof(GpuMesh), 0, mMeshMirror.data(), false);
    mLevelBuffer = mVao->createUavBuffer(size_t(want) * kLevelsPerMesh, sizeof(GpuMeshLevel), 0,
                                         mLevelMirror.data(), false);
    mMeshCapacity = want;
}

void GpuScene::ensureSlots(uint32_t count) {
    if (count > mSlotCapacity) growTo(count);
}

void GpuScene::setSlotCount(uint32_t n) {
    ensureSlots(n);
    mSlotCount = n;
}

void GpuScene::stage(uint32_t slot, const GpuInstance &in) {
    if (slot >= mSlotCapacity) ensureSlots(slot + 1u);
    if (slot >= mSlotCapacity) return;
    GpuInstance &dst = mMirror[slot];
    // THE PREVIOUS WORLD IS THE MIRROR'S OWN CURRENT WORLD, which is what makes
    // it exactly the last frame's and never a frame older. A slot seen for the
    // FIRST time has no previous pose — reprojecting from a pose it never
    // occupied would be a motion vector out of nowhere — so it is born with
    // prevWorld = world (the "stills snap" rule, ENGINE_V2_SPEC §4.2).
    const bool born = mBorn[slot] != 0u;
    float prev[12];
    std::memcpy(prev, born ? dst.world : in.world, sizeof(prev));
    dst = in;
    std::memcpy(dst.prevWorld, prev, sizeof(prev));
    mBorn[slot] = 1u;
    ++mWrites;
}

void GpuScene::onSlotMoved(uint32_t from, uint32_t to) {
    if (from >= mSlotCapacity || to >= mSlotCapacity) return;
    // THE SWAP-REMOVE, IN ONE PLACE. The mover keeps its poses (it did not
    // move in the world, only in the index), so `prevWorld` travels with it —
    // a reprojection of the object that was renumbered must not read the
    // previous pose of the object that DIED in that slot.
    mMirror[to] = mMirror[from];
    mBorn[to] = mBorn[from];
    mBorn[from] = 0u;
    std::memset(&mMirror[from], 0, sizeof(GpuInstance));
}

void GpuScene::update(const std::vector<uint32_t> &dirtySlots, unsigned long long epoch) {
    mEpoch = epoch;
    mLastDirtyCount = unsigned(dirtySlots.size());
    if (!live()) return;

    // THE MESH TABLE FIRST: an instance's mesh index must mean something on the
    // device before the instance referencing it arrives.
    if (mMeshDirty && !mMeshMirror.empty()) {
        mMeshBuffer->upload(mMeshMirror.data(), 0, mMeshMirror.size());
        mMeshDirty = false;
        ++mCopies;
    }
    if (mLevelDirty && !mLevelMirror.empty()) {
        mLevelBuffer->upload(mLevelMirror.data(), 0, mLevelMirror.size());
        mLevelDirty = false;
        ++mCopies;
    }

    // LAST FRAME'S MOVERS THAT DID NOT MOVE AGAIN. Their previous pose is now
    // their current one; without this a mover that stops carries a stale
    // `prevWorld` for one frame and every reprojection reads one frame of
    // motion that did not happen. It costs one entry per stopped mover, once.
    mCopySet.clear();
    mCopySet.insert(mCopySet.end(), dirtySlots.begin(), dirtySlots.end());
    for (uint32_t slot : mMovedLastFrame) {
        if (slot >= mSlotCapacity || !mBorn[slot]) continue;
        if (std::find(dirtySlots.begin(), dirtySlots.end(), slot) != dirtySlots.end()) continue;
        GpuInstance &e = mMirror[slot];
        if (std::memcmp(e.prevWorld, e.world, sizeof(e.world)) == 0) continue;
        std::memcpy(e.prevWorld, e.world, sizeof(e.world));
        mCopySet.push_back(slot);
    }
    mMovedLastFrame.assign(dirtySlots.begin(), dirtySlots.end());

    if (mCopySet.empty()) return;   // A STILL FRAME COPIES NOTHING AT ALL.

    const auto t0 = std::chrono::steady_clock::now();
    std::sort(mCopySet.begin(), mCopySet.end());
    mCopySet.erase(std::unique(mCopySet.begin(), mCopySet.end()), mCopySet.end());
    while (!mCopySet.empty() && mCopySet.back() >= mSlotCapacity) mCopySet.pop_back();
    if (mCopySet.empty()) return;

    // THE RUN COALESCER. Sorted slots collapse into contiguous runs, and a run
    // is one copy — a scene whose whole item list moved is ONE copy, a scene
    // with one mover is one copy of 160 bytes.
    struct Run {
        uint32_t first = 0, count = 0;
    };
    std::vector<Run> runs;
    runs.reserve(mCopySet.size());
    for (uint32_t slot : mCopySet) {
        if (!runs.empty() && runs.back().first + runs.back().count == slot)
            ++runs.back().count;
        else
            runs.push_back(Run{ slot, 1u });
    }

    size_t bytes = 0;
    for (const Run &r : runs) bytes += size_t(r.count) * sizeof(GpuInstance);
    Ogre::StagingBuffer *sb = mVao->getStagingBuffer(bytes, true);
    unsigned char *dst = static_cast<unsigned char *>(sb->map(bytes));
    Ogre::StagingBuffer::DestinationVec dests;
    dests.reserve(runs.size());
    size_t srcOffset = 0;
    for (const Run &r : runs) {
        const size_t len = size_t(r.count) * sizeof(GpuInstance);
        std::memcpy(dst + srcOffset, &mMirror[r.first], len);
        dests.push_back(Ogre::StagingBuffer::Destination(
            mInstanceBuffer, size_t(r.first) * sizeof(GpuInstance), srcOffset, len));
        srcOffset += len;
    }
    sb->unmap(dests);
    sb->removeReferenceCount();
    mCopies += runs.size();
    ++mUpdates;
    mLastCopyMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// ---------------------------------------------------------------------------
// THE MESH TABLE. Reference-counted by mesh pointer: an entry is written the
// first time an Item wears the mesh and freed with the last one, so the table
// holds what the scene HAS rather than what it has ever had.
uint32_t GpuScene::meshIndex(const Ogre::Mesh *mesh) const {
    auto it = mMeshIndex.find(mesh);
    return it == mMeshIndex.end() ? kNoMesh : it->second;
}

uint32_t GpuScene::acquireMesh(const Ogre::MeshPtr &meshPtr, const GpuMesh &desc,
                               const GpuMeshLevel *levels, uint32_t levelCount) {
    const Ogre::Mesh *mesh = meshPtr.get();
    if (!mesh || !live()) return kNoMesh;
    auto it = mMeshIndex.find(mesh);
    if (it != mMeshIndex.end()) {
        ++mMeshEntries[it->second].refs;
        return it->second;
    }
    uint32_t index;
    if (!mFreeMeshSlots.empty()) {
        index = mFreeMeshSlots.back();
        mFreeMeshSlots.pop_back();
    } else {
        index = uint32_t(mMeshEntries.size());
        mMeshEntries.push_back(MeshEntry());
        if (index + 1u > mMeshCapacity) growMeshTable(index + 1u);
    }
    mMeshEntries[index].mesh = meshPtr;
    mMeshEntries[index].refs = 1u;
    mMeshIndex[mesh] = index;
    mMeshMirror[index] = desc;
    const uint32_t take = std::min(levelCount, kLevelsPerMesh);
    if (levelCount > kLevelsPerMesh && !mWarnedLevels) {
        mWarnedLevels = true;
        Ogre::LogManager::getSingleton().logMessage(
            "Jahshaka gpuscene: a mesh carries more LOD levels than the range table's stride "
            "(" + std::to_string(levelCount) + " > " + std::to_string(kLevelsPerMesh) +
            "); the coarsest levels are not in the table");
    }
    for (uint32_t l = 0; l < kLevelsPerMesh; ++l)
        mLevelMirror[size_t(index) * kLevelsPerMesh + l] =
            l < take ? levels[l] : GpuMeshLevel();
    mMeshMirror[index].counts[2] = take;
    mMeshDirty = true;
    mLevelDirty = true;
    return index;
}

void GpuScene::releaseMesh(const Ogre::Mesh *mesh) {
    auto it = mMeshIndex.find(mesh);
    if (it == mMeshIndex.end()) return;
    const uint32_t index = it->second;
    if (mMeshEntries[index].refs > 1u) {
        --mMeshEntries[index].refs;
        return;
    }
    mMeshEntries[index] = MeshEntry();
    mMeshIndex.erase(it);
    mFreeMeshSlots.push_back(index);
    // THE ENTRY IS ZEROED, not left behind: a slot recycled to a different mesh
    // must never be readable as the dead one's geometry (the VctMaterial
    // by-pointer aliasing lesson, DOCS/traps/ENGINE.md).
    mMeshMirror[index] = GpuMesh();
    for (uint32_t l = 0; l < kLevelsPerMesh; ++l)
        mLevelMirror[size_t(index) * kLevelsPerMesh + l] = GpuMeshLevel();
    mMeshDirty = true;
    mLevelDirty = true;
}

const Ogre::MeshPtr &GpuScene::meshAt(uint32_t index) const {
    static const Ogre::MeshPtr kNone;
    return index < mMeshEntries.size() ? mMeshEntries[index].mesh : kNone;
}

uint32_t GpuScene::meshRefCount(uint32_t index) const {
    return index < mMeshEntries.size() ? mMeshEntries[index].refs : 0u;
}

void GpuScene::setMeshAddresses(uint32_t index, uint64_t positionAddress, uint64_t indexAddress) {
    if (index >= mMeshMirror.size()) return;
    GpuMesh &m = mMeshMirror[index];
    m.positionAddress[0] = uint32_t(positionAddress & 0xFFFFFFFFull);
    m.positionAddress[1] = uint32_t(positionAddress >> 32);
    m.indexAddress[0] = uint32_t(indexAddress & 0xFFFFFFFFull);
    m.indexAddress[1] = uint32_t(indexAddress >> 32);
    mMeshDirty = true;
}

// ===========================================================================
// THE SCENE'S HALF: who is dirty, and what one entry says.
// ===========================================================================

static_assert(sizeof(Ogre::Real) == sizeof(float),
              "the GPU scene copies Ogre's Matrix4 rows verbatim; a double-precision build "
              "would need a conversion here");

/// THE ONE PLACE THE PREDICATES ARE COMPUTED (design §3: every duplicate "is
/// this item ray-visible" test outside this word goes).
///
/// The TRACED SET is the conjunction `gatherRayInstances` used to walk for, and
/// each exclusion is load-bearing for the same reasons it always was: editor
/// furniture and the backdrop carry their own channel instead of kVisibleBit;
/// the overlay queues are unlit and depth-test-off; a SKINNED item's buffers
/// hold the bind pose, so tracing it would reflect a T-pose (audit C-5); an
/// ALPHA-TESTED datablock has no any-hit shader to cut it out, so a leaf would
/// intersect as a solid quad (audit C-16).
Ogre::uint32 OgreScene::gpuFlagsFor(const Node &n) const {
    Ogre::Item *item = n.item;
    if (!item) return 0u;
    const Ogre::uint32 vis = item->getVisibilityFlags();
    Ogre::uint32 f = 0u;
    if (n.shown && (vis & (kVisibleBit | kMovableBit))) f |= kGpuVisible;
    if (vis & kMovableBit) f |= kGpuMover;
    if (vis & kGiGeometryBit) f |= kGpuGiVisible;
    if (item->getCastShadows() && (vis & allShadowCasterChannels())) f |= kGpuCaster;
    if (item->getSkeletonInstance()) f |= kGpuSkinned;
    if (item->getRenderQueueGroup() >= kOverlayRenderQueue) f |= kGpuOverlay;
    for (size_t i = 0, e = item->getNumSubItems(); i < e; ++i) {
        const Ogre::HlmsDatablock *db = item->getSubItem(i)->getDatablock();
        if (db && db->getAlphaTest() != Ogre::CMPF_ALWAYS_PASS) {
            f |= kGpuAlphaTested;
            break;
        }
    }
    if (n.dragMover && n.shown) f |= kGpuDragMover;
    if (n.giBoundsExcluded) f |= kGpuGiExcluded;
    // ...and it must be IN the graph: an Item with no parent node draws nothing
    // and has no world transform to trace (the old walk skipped it outright).
    if ((f & kGpuVisible) && !(f & (kGpuOverlay | kGpuSkinned | kGpuAlphaTested)) &&
        item->getMesh() && item->getParentNode())
        f |= kGpuRayTraced;
    return f;
}

/// ONE ENTRY, from the node. The only expensive line is the world AABB (a
/// parent-chain walk that recomputes a whole SIMD block), which is exactly why
/// this runs for CHANGED slots only.
void OgreScene::composeGpuInstance(const Node &n, detail::GpuInstance &out) const {
    out = detail::GpuInstance();
    Ogre::Item *item = n.item;
    if (!item) return;
    if (Ogre::Node *pn = item->getParentNode()) {
        const Ogre::Matrix4 &m = pn->_getFullTransformUpdated();
        std::memcpy(out.world, &m[0][0], sizeof(out.world));
    }
    const Ogre::Aabb a = item->getWorldAabbUpdated();
    const Ogre::Vector3 mn = a.getMinimum(), mx = a.getMaximum();
    for (int i = 0; i < 3; ++i) {
        out.boundsMin[i] = mn[i];
        out.boundsMax[i] = mx[i];
    }
    const uint32_t meshIndex = uint32_t(n.gpuMeshSlot);
    const Ogre::uint32 flags = gpuFlagsFor(n);
    std::memcpy(&out.boundsMin[3], &meshIndex, sizeof(uint32_t));
    std::memcpy(&out.boundsMax[3], &flags, sizeof(uint32_t));
    out.ids[0] = uint32_t(n.selfId);
    out.ids[2] = uint32_t(n.lightMask);
}

/// A SEAM THAT CHANGED WHAT THE TABLE SAYS WITHOUT MOVING ANYTHING. The
/// movement epoch cannot see these — a visibility write on editor furniture
/// deliberately is NOT scene movement (VR-SCAN-1) — so the seams that write an
/// Item's flags say so by name.
void OgreScene::markGpuSlotDirty(const Node &n) {
    if (n.itemSlot == size_t(-1)) return;
    mGpuForced.push_back(uint32_t(n.itemSlot));
}

/// The mesh table entry for an attached mesh, acquired once per attach.
uint32_t OgreScene::acquireGpuMesh(const MeshRec &rec) {
    ensureGpuTables();
    if (!mGpuScene.live() || !rec.mesh) return detail::GpuScene::kNoMesh;
    const Ogre::Mesh *mesh = rec.mesh.get();
    if (const uint32_t have = mGpuScene.meshIndex(mesh); have != detail::GpuScene::kNoMesh)
        return mGpuScene.acquireMesh(rec.mesh, detail::GpuMesh(), nullptr, 0u);

    detail::GpuMesh desc;
    const Ogre::Aabb local = rec.mesh->getAabb();
    const Ogre::Vector3 mn = local.getMinimum(), mx = local.getMaximum();
    for (int i = 0; i < 3; ++i) {
        desc.localBoundsMin[i] = mn[i];
        desc.localBoundsMax[i] = mx[i];
    }
    // THE LEVEL-0 BOUND IS ZERO AND THAT IS THE HONEST VALUE: ATOM-BAKE-1's
    // per-level bounds start at level 1 (MeshRec::lodBounds' note) because the
    // authored geometry has no error against itself.
    desc.localBoundsMax[3] = 0.0f;
    desc.counts[3] = uint32_t(rec.mesh->getNumSubMeshes());

    // SUBMESH 0's CHAIN. A mesh with several submeshes has one index range per
    // (submesh, level) and this table holds the FIRST submesh's — which is what
    // every mesh this engine bakes has exactly one of. Atom P4's voxeliser,
    // which reads per-(mesh, level) ranges, is where the multi-submesh widening
    // belongs (it needs a submesh dimension in the table, not a guess here).
    detail::GpuMeshLevel levels[detail::GpuScene::kLevelsPerMesh];
    uint32_t levelCount = 0u;
    if (rec.mesh->getNumSubMeshes() > 0) {
        const Ogre::SubMesh *sub = rec.mesh->getSubMesh(0);
        const Ogre::VertexArrayObjectArray &vaos = sub->mVao[Ogre::VpNormal];
        levelCount = uint32_t(vaos.size());
        for (uint32_t l = 0; l < levelCount && l < detail::GpuScene::kLevelsPerMesh; ++l) {
            if (!vaos[l]) continue;
            levels[l].firstIndex = vaos[l]->getPrimitiveStart();
            levels[l].indexCount = vaos[l]->getPrimitiveCount();
        }
        if (!vaos.empty() && vaos[0]) {
            desc.counts[0] = uint32_t(vaos[0]->getVertexBuffers().empty()
                                          ? 0u
                                          : vaos[0]->getVertexBuffers()[0]->getNumElements());
            desc.counts[1] = vaos[0]->getPrimitiveCount();
        }
    }
    return mGpuScene.acquireMesh(rec.mesh, desc, levels, levelCount);
}

void OgreScene::releaseGpuMesh(const Ogre::Mesh *mesh) {
    if (mGpuScene.live() && mesh) mGpuScene.releaseMesh(mesh);
}

/// THE DIRTY SCAN — the one CPU walk that remains, and the one the journal
/// (V2-1) replaces without changing `GpuScene::update`'s signature.
///
/// IT IS EPOCH-GATED, so a still scene runs no walk and copies no bytes. When
/// the epoch HAS moved, each slot is asked two cheap questions — did its 48
/// bytes of world transform change, and did its flags word change — and only a
/// slot that answers yes pays for a world AABB. Whoever reads the table first in
/// a frame pays for the update (the GI signatures read it before the frame, the
/// ray tier inside it), exactly as `ensureGiWalk` works.
/// The tables, created the first time this scene needs them — which is the first
/// mesh ATTACH, not the first frame: an Item that arrives before a frame is drawn
/// must still get its mesh table entry, or its instance entry would name no mesh.
void OgreScene::ensureGpuTables() const {
    if (mGpuScene.live() || mGpuSceneRefused || !mSceneMgr || mDestroying) return;
    Ogre::RenderSystem *rs = mRoot ? mRoot->getRenderSystem() : nullptr;
    Ogre::VaoManager *vao =
        rs && !(mEngine && mEngine->isHeadless()) ? rs->getVaoManager() : nullptr;
    std::string err;
    if (!mGpuScene.create(vao, err)) {
        mGpuSceneRefused = true;
        Ogre::LogManager::getSingleton().logMessage("Jahshaka " + err);
    }
}

void OgreScene::ensureGpuScene(bool graphIsCurrent) const {
    if (!mSceneMgr || mDestroying) return;
    ensureGpuTables();
    if (!mGpuScene.live()) return;
    const unsigned long long epoch = shadowEpoch();
    const bool epochMoved = !mGpuEpochValid || epoch != mGpuEpoch;
    if (!epochMoved && mGpuForced.empty()) {
        // NOTHING MOVED — but last frame's movers may still owe their close, and
        // that is the one thing a still frame is not free for (once).
        if (mGpuScene.hasOpenMovers()) {
            mGpuDirty.clear();
            mGpuScene.update(mGpuDirty, epoch);
        }
        return;
    }
    mGpuEpoch = epoch;
    mGpuEpochValid = true;

    mGpuScene.setSlotCount(uint32_t(mItemNodes.size()));
    mGpuDirty.clear();
    detail::GpuInstance cand;
    // THE EXPLICIT MARKS FIRST: a seam that changed a flags word without moving
    // anything, and every newborn slot.
    for (uint32_t slot : mGpuForced) {
        if (slot >= mItemNodes.size()) continue;
        composeGpuInstance(*mItemNodes[slot], cand);
        mGpuScene.stage(slot, cand);
        mGpuDirty.push_back(slot);
    }
    mGpuForced.clear();
    if (epochMoved) {
        const size_t forced = mGpuDirty.size();
        const detail::GpuInstance *mirror = mGpuScene.mirrorData();
        for (size_t i = 0, e = mItemNodes.size(); i < e; ++i) {
            const Node &n = *mItemNodes[i];
            Ogre::Item *item = n.item;
            if (!item) continue;
            Ogre::Node *pn = item->getParentNode();
            if (!pn) continue;
            const detail::GpuInstance &was = mirror[i];
            // THE COMPARE. `_getFullTransform` is a cached read and
            // `_getFullTransformUpdated` RECOMPUTES the derived transform up the
            // parent chain unconditionally (Ogre-Next keeps no per-node dirty
            // bit — `updateAllTransforms` recomputes the whole graph by depth
            // level every frame). MEASURED at 8,001 items in Debug: 1.0 ms for
            // the cached read against 3.0 ms for the recomputing one. So the
            // frame path, which runs AFTER `updateSceneGraph`, reads the cache;
            // a reader BEFORE the frame — the GI signatures, asked by the mirror
            // after it has written this frame's transforms — must recompute, and
            // pays exactly what the walk it replaces paid for the same reason.
            const Ogre::Matrix4 &m =
                graphIsCurrent ? pn->_getFullTransform() : pn->_getFullTransformUpdated();
            bool dirty = std::memcmp(&m[0][0], was.world, sizeof(was.world)) != 0;
            if (!dirty) {
                // A FLAGS CHANGE THAT MOVED NOTHING. The seams that write an
                // Item's channels mark their slot by name (markGpuSlotDirty), so
                // this is a BACKSTOP and not the mechanism — measured at 0.17 ms
                // of the 8,001-item walk, which is worth paying to be immune to
                // a seam nobody marked.
                Ogre::uint32 wasFlags;
                std::memcpy(&wasFlags, &was.boundsMax[3], sizeof(wasFlags));
                dirty = gpuFlagsFor(n) != wasFlags;
            }
            if (!dirty) continue;
            if (std::find(mGpuDirty.begin(), mGpuDirty.begin() + long(forced), uint32_t(i)) !=
                mGpuDirty.begin() + long(forced))
                continue;
            composeGpuInstance(n, cand);
            mGpuScene.stage(uint32_t(i), cand);
            mGpuDirty.push_back(uint32_t(i));
            ++mGpuAabbReads;
        }
    }
    mGpuScene.update(mGpuDirty, epoch);
    mGpuScanMicros = 0.0;
    ++mGpuScans;
}

/// The measured scan, for the suite and the premise: the SAME walk, timed. Kept
/// apart from `ensureGpuScene` so timing never rides in a hot path that does
/// not ask for it.
void OgreScene::ensureGpuSceneTimed(bool graphIsCurrent) const {
    const auto t0 = std::chrono::steady_clock::now();
    ensureGpuScene(graphIsCurrent);
    mGpuScanMicros =
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
}

// ---------------------------------------------------------------------------
// THE TEST AND TOOL DOOR.
namespace {
void toPublic(const detail::GpuInstance &in, GpuSceneEntry &out) {
    std::memcpy(out.world, in.world, sizeof(out.world));
    std::memcpy(out.prevWorld, in.prevWorld, sizeof(out.prevWorld));
    for (int i = 0; i < 3; ++i) {
        out.boundsMin[i] = in.boundsMin[i];
        out.boundsMax[i] = in.boundsMax[i];
    }
    std::memcpy(&out.meshIndex, &in.boundsMin[3], sizeof(unsigned));
    std::memcpy(&out.flags, &in.boundsMax[3], sizeof(unsigned));
    out.nodeId = in.ids[0];
    out.lightMask = in.ids[2];
}
}  // namespace

GpuSceneStatus OgreScene::gpuSceneStatus() const {
    GpuSceneStatus st;
    st.live = mGpuScene.live();
    st.slotCount = unsigned(mItemNodes.size());
    st.capacity = mGpuScene.slotCapacity();
    st.meshEntries = mGpuScene.meshEntryCount();
    st.lastDirty = mGpuScene.lastDirtyCount();
    st.writes = mGpuScene.writes();
    st.copyRuns = mGpuScene.copies();
    st.updates = mGpuScene.frames();
    st.grows = mGpuScene.grows();
    st.scans = mGpuScans;
    st.aabbReads = mGpuAabbReads;
    st.lastCopyMs = mGpuScene.lastCopyMs();
    st.lastScanMicros = mGpuScanMicros;
    return st;
}

bool OgreScene::gpuSceneEntry(unsigned slot, GpuSceneEntry &out) const {
    if (!mGpuScene.live() || slot >= mGpuScene.slotCapacity()) return false;
    toPublic(mGpuScene.entry(slot), out);
    return true;
}

/// THE DEVICE'S OWN BYTES. `readRequest` records the download and the ticket's
/// map() waits on Ogre's fence for it — but this scene's copies were RECORDED
/// into the frame's command buffer and not submitted, so the flush comes first
/// for exactly the reason the manual-upload trap names (DOCS/traps/ENGINE.md:
/// "AsyncTextureTicket after a manual staging upload reads STALE VRAM").
bool OgreScene::gpuSceneDeviceEntries(unsigned first, unsigned count,
                                      std::vector<GpuSceneEntry> &out) {
    out.clear();
    if (!mGpuScene.live() || !count || first + count > mGpuScene.slotCapacity()) return false;
    Ogre::RenderSystem *rs = mRoot ? mRoot->getRenderSystem() : nullptr;
    if (rs) rs->flushCommands();
    Ogre::AsyncTicketPtr ticket = mGpuScene.instanceBuffer()->readRequest(first, count);
    const detail::GpuInstance *raw = static_cast<const detail::GpuInstance *>(ticket->map());
    out.resize(count);
    for (unsigned i = 0; i < count; ++i) toPublic(raw[i], out[i]);
    ticket->unmap();
    return true;
}

}  // namespace detail
}  // namespace engine
}  // namespace jahshaka
