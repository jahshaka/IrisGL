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
    mStagedAt.clear();
    mFreedSlots.clear();
    mRuns.clear();
    mDests.clear();
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
    mStagedAt.resize(want, 0u);
    if (mInstanceBuffer) {
        mVao->destroyUavBuffer(mInstanceBuffer);
        mInstanceBuffer = nullptr;
        ++mGrows;
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
    // ONCE PER UPDATE (see the header): a second stage would read the world the
    // first just wrote and call it the PREVIOUS world.
    if (mStagedAt[slot] == mUpdateSerial) return;
    mStagedAt[slot] = mUpdateSerial;
    GpuInstance &dst = mMirror[slot];
    // THE PREVIOUS WORLD IS THE MIRROR'S OWN CURRENT WORLD, which is what makes
    // it exactly the last frame's and never a frame older. A slot seen for the
    // FIRST time has no previous pose — reprojecting from a pose it never
    // occupied would be a motion vector out of nowhere — so it is born with
    // prevWorld = world (the "stills snap" rule, ENGINE_V2_SPEC §4.2).
    const bool born = mBorn[slot] != 0u;
    float prev[12];
    std::memcpy(prev, born ? dst.world : in.world, sizeof(prev));
    // A RAW-BYTE COPY, deliberately: this struct's bytes are what reaches the
    // device, so it is copied the way the staging buffer copies it and nothing
    // here may ever become a type with a non-trivial assignment.
    std::memcpy(&dst, &in, sizeof(GpuInstance));
    std::memcpy(dst.prevWorld, prev, sizeof(prev));
    mBorn[slot] = 1u;
    ++mWrites;
}

void GpuScene::onSlotFreed(uint32_t slot) {
    if (slot >= mSlotCapacity) return;
    std::memset(&mMirror[slot], 0, sizeof(GpuInstance));
    mBorn[slot] = 0u;
    mStagedAt[slot] = 0u;
    mFreedSlots.push_back(slot);
}

void GpuScene::onSlotMoved(uint32_t from, uint32_t to) {
    if (from >= mSlotCapacity || to >= mSlotCapacity) return;
    // THE SWAP-REMOVE, IN ONE PLACE. The mover keeps its poses (it did not
    // move in the world, only in the index), so `prevWorld` travels with it —
    // a reprojection of the object that was renumbered must not read the
    // previous pose of the object that DIED in that slot.
    std::memcpy(&mMirror[to], &mMirror[from], sizeof(GpuInstance));
    mBorn[to] = mBorn[from];
    mStagedAt[to] = 0u;      // it must be re-staged/copied this update
    onSlotFreed(from);       // ...and the tail it came from is cleared and copied
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

    mCopySet.clear();
    mCopySet.insert(mCopySet.end(), dirtySlots.begin(), dirtySlots.end());
    // FREED SLOTS RIDE ALONG. A slot past `slotCount()` still occupies device
    // memory, and a dead item's entry left there is a ghost a reader that got
    // its bound wrong would trace. Clearing costs one entry per removal.
    for (uint32_t slot : mFreedSlots)
        if (slot < mSlotCapacity && !stagedThisUpdate(slot)) mCopySet.push_back(slot);
    mFreedSlots.clear();

    // LAST FRAME'S MOVERS THAT DID NOT MOVE AGAIN. Their previous pose is now
    // their current one; without this a mover that stops carries a stale
    // `prevWorld` for one frame and every reprojection reads one frame of
    // motion that did not happen. It costs one entry per stopped mover, once.
    //
    // ONCE PER FRAME, NOT ONCE PER CALL: `update` can run twice in one frame (a
    // reader before the frame and the frame's own pass), and closing on the
    // second call would erase the motion the frame is about to render. The
    // VaoManager's frame counter is the engine's own "a frame has passed" —
    // there is no wall clock here.
    const uint32_t vaoFrame = mVao->getFrameCount();
    const bool closeNow = vaoFrame != mLastCloseFrame;
    if (closeNow) {
        for (uint32_t slot : mMovedLastFrame) {
            if (slot >= mSlotCapacity || !mBorn[slot]) continue;
            // THE STAMP, NOT A SEARCH (the O(N^2) a group drag used to pay).
            if (mStagedAt[slot] == mUpdateSerial) continue;
            GpuInstance &e = mMirror[slot];
            if (std::memcmp(e.prevWorld, e.world, sizeof(e.world)) == 0) continue;
            std::memcpy(e.prevWorld, e.world, sizeof(e.world));
            mCopySet.push_back(slot);
        }
        mMovedLastFrame.assign(dirtySlots.begin(), dirtySlots.end());
        mLastCloseFrame = vaoFrame;
    } else {
        // The frame's second pass: its dirty slots JOIN the set still owed a
        // close rather than replacing it. BOUNDED: a host that ran many updates
        // inside one frame would otherwise grow this without limit, so it is
        // collapsed once it passes twice the slot count.
        for (uint32_t slot : dirtySlots) mMovedLastFrame.push_back(slot);
        if (mMovedLastFrame.size() > size_t(mSlotCapacity) * 2u) {
            std::sort(mMovedLastFrame.begin(), mMovedLastFrame.end());
            mMovedLastFrame.erase(std::unique(mMovedLastFrame.begin(), mMovedLastFrame.end()),
                                  mMovedLastFrame.end());
        }
    }

    // THE UPDATE IS OVER: the next one is a new stamp generation. Bumped here
    // and not at the top, because `stage()` runs BEFORE `update()` is called
    // (the caller composes, stages, then hands over the list).
    ++mUpdateSerial;
    if (mUpdateSerial == 0u) {       // wrap: no stamp may alias the new serial
        std::fill(mStagedAt.begin(), mStagedAt.end(), 0u);
        mUpdateSerial = 1u;
    }

    if (mCopySet.empty()) return;   // A STILL FRAME COPIES NOTHING AT ALL.

    const auto t0 = std::chrono::steady_clock::now();
    std::sort(mCopySet.begin(), mCopySet.end());
    mCopySet.erase(std::unique(mCopySet.begin(), mCopySet.end()), mCopySet.end());
    while (!mCopySet.empty() && mCopySet.back() >= mSlotCapacity) mCopySet.pop_back();
    if (mCopySet.empty()) return;

    // THE RUN COALESCER. Sorted slots collapse into contiguous runs, and a run
    // is one copy — a scene whose whole item list moved is ONE copy, a scene
    // with one mover is one copy of 160 bytes.
    mRuns.clear();
    for (uint32_t slot : mCopySet) {
        if (!mRuns.empty() && mRuns.back().first + mRuns.back().count == slot)
            ++mRuns.back().count;
        else
            mRuns.push_back(Run{ slot, 1u });
    }

    size_t bytes = 0;
    for (const Run &r : mRuns) bytes += size_t(r.count) * sizeof(GpuInstance);
    Ogre::StagingBuffer *sb = mVao->getStagingBuffer(bytes, true);
    unsigned char *dst = static_cast<unsigned char *>(sb->map(bytes));
    mDests.clear();
    size_t srcOffset = 0;
    for (const Run &r : mRuns) {
        const size_t len = size_t(r.count) * sizeof(GpuInstance);
        std::memcpy(dst + srcOffset, &mMirror[r.first], len);
        mDests.push_back(Ogre::StagingBuffer::Destination(
            mInstanceBuffer, size_t(r.first) * sizeof(GpuInstance), srcOffset, len));
        srcOffset += len;
    }
    sb->unmap(&mDests[0], mDests.size());
    sb->removeReferenceCount();
    mCopies += mRuns.size();
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

/// ONE ENTRY, from the node.
///
/// THE TRANSFORM IS HANDED IN, not fetched again. The scan has just compared
/// this very matrix; re-fetching it through `_getFullTransformUpdated` would be
/// a second root recursion per dirty slot AND would only agree with what was
/// compared while no node in the chain disables inheritance (OgreNode.cpp:365-395
/// against :449-482 take different paths) — a bit-difference there would stage an
/// entry that does not match the value the compare will see next frame, and the
/// slot would be dirty for ever.
///
/// The BOUNDS follow the same rule: with the graph already updated,
/// `getWorldAabb()` is the cached read and `getWorldAabbUpdated()` is a third
/// recursion for the same answer.
void OgreScene::composeGpuInstance(const Node &n, const Ogre::Matrix4 &world, bool graphIsCurrent,
                                   detail::GpuInstance &out) const {
    out = detail::GpuInstance();
    Ogre::Item *item = n.item;
    if (!item) return;
    std::memcpy(out.world, &world[0][0], sizeof(out.world));
    const Ogre::Aabb a = graphIsCurrent ? item->getWorldAabb() : item->getWorldAabbUpdated();
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
    // THE RAY LEVEL (AT-A8r) travels with the entry rather than being poked
    // into the mirror: every write to this table goes through one composer, so
    // a slot re-staged for any other reason keeps the level it was given.
    if (n.itemSlot != size_t(-1) && n.itemSlot < mRayLevel.size())
        out.ids[3] = mRayLevel[n.itemSlot];
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
        // THE LEVEL'S BOUND TRAVELS WITH ITS RANGE (ATOM-SUBSTRATE-1): without
        // it no shader can WALK the level rule — see GpuMeshLevel. `lodBounds`
        // holds levels 1..N-1 (the authored level has no error against itself),
        // so level l reads index l-1 and level 0 keeps its honest 0.
        const std::vector<float> &bounds = rec.lodBounds;
        for (uint32_t l = 0; l < levelCount && l < detail::GpuScene::kLevelsPerMesh; ++l) {
            if (!vaos[l]) continue;
            levels[l].firstIndex = vaos[l]->getPrimitiveStart();
            levels[l].indexCount = vaos[l]->getPrimitiveCount();
            levels[l].bound = (l > 0u && size_t(l - 1u) < bounds.size()) ? bounds[l - 1u] : 0.0f;
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
    const auto tScan = std::chrono::steady_clock::now();
    // THE EPOCH IS ONLY CONSUMED BY A SCAN THAT COULD SEE THE TRUTH. A reader
    // BEFORE the frame runs on derived transforms `updateSceneGraph` has not
    // recomputed yet; if such a pass marked the epoch spent, the frame's own
    // pass would skip — and every CHILD of a moved parent would silently never
    // reach the table (measured: a parent with 8,000 children staged nothing at
    // all). So only the frame's form consumes it, and the epoch is held exactly
    // as the caster walk holds it: valid only where a host counter makes it
    // meaningful (OgreGi.cpp's ensureShadowWalk).
    if (graphIsCurrent) {
        mGpuEpoch = epoch;
        mGpuEpochValid = detail::gTransformWriteCounter != nullptr;
    }

    mGpuScene.setSlotCount(uint32_t(mItemNodes.size()));
    mGpuDirty.clear();
    detail::GpuInstance cand;
    // THE EXPLICIT MARKS FIRST: a seam that changed a flags word without moving
    // anything, and every newborn slot. A slot marked TWICE (seven seams, two of
    // which fire on one attach) is staged once — `stage` drops the second — and
    // must therefore not enter the dirty list twice either.
    for (uint32_t slot : mGpuForced) {
        if (slot >= mItemNodes.size()) continue;
        if (mGpuScene.stagedThisUpdate(slot)) continue;
        const Node &n = *mItemNodes[slot];
        Ogre::Node *pn = n.item ? n.item->getParentNode() : nullptr;
        if (!pn) continue;
        const Ogre::Matrix4 &m =
            graphIsCurrent ? pn->_getFullTransform() : pn->_getFullTransformUpdated();
        composeGpuInstance(n, m, graphIsCurrent, cand);
        mGpuScene.stage(slot, cand);
        mGpuDirty.push_back(slot);
    }
    mGpuForced.clear();
    if (epochMoved) {
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
            // level every frame). MEASURED at 8,001 items in Debug: 0.7 ms for
            // the cached read against 2.3-2.5 ms for the recomputing one. So the
            // frame path, which runs AFTER `updateSceneGraph`, reads the cache.
            // The recomputing form exists for a reader BEFORE the frame — the GI
            // signature walk would be one, and is deliberately NOT converted
            // today for exactly this three-fold cost (GpuScene.h's header); the
            // suite measures both forms so the decision stays visible.
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
            // THE STAMP, NOT A SEARCH: a slot the marks already staged is
            // skipped in one load. The linear scan this replaces was O(marks)
            // per dirty slot.
            if (mGpuScene.stagedThisUpdate(uint32_t(i))) continue;
            composeGpuInstance(n, m, graphIsCurrent, cand);
            mGpuScene.stage(uint32_t(i), cand);
            mGpuDirty.push_back(uint32_t(i));
            ++mGpuAabbReads;
        }
    }
    mGpuScene.update(mGpuDirty, epoch);
    ++mGpuScans;
    // ALWAYS TIMED, and reported as its own MONITOR STAGE. This is real CPU
    // inside renderOneFrame that belongs to no compositor pass — 0.6-0.7 ms at
    // 8,001 items — and unattributed it reads as frame time nobody spent (the
    // stages are exclusive and must account for the frame within 5 %). A
    // steady_clock pair against a walk of that size is noise.
    mGpuScanMicros =
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - tScan).count();
    if (monitor::live()) monitor::gMonitor->stage("engine.gpuscene", mGpuScanMicros / 1000.0);
}

/// The scan with its cost recorded for a caller that wants the number without a
/// frame — the suite's premise table, which needs the PRE-FRAME form's cost and
/// has nowhere else to take it. `ensureGpuScene` times itself, so this only
/// forces the pass.
void OgreScene::ensureGpuSceneTimed(bool graphIsCurrent) const {
    ensureGpuScene(graphIsCurrent);
}

// ---------------------------------------------------------------------------
// THE RAY LEVEL (ATOM P3's SUB-ERROR, AT-A8r) — the level a BLAS should be
// built from, and the hysteresis that keeps the build rare.
//
// THE RULE IS THE QUALITY CURRENCY'S, with the ray's own sample. A ray traced
// from the camera through a pixel diverges exactly as that pixel's footprint
// does, so "the ray footprint at the instance's distance" IS
// `sampleFootprintPerspective(d, proj[1][1], height)` — the same number the
// view's LOD strategy spends, and the tolerance is ONE of them. Four consumers,
// one derivation (Types.h): the view in pixels, the cascade in cells, the card
// in texels, the ray in footprints.
//
// WHY THE ANSWER IS STICKY. Rebuilding a bottom-level structure is not a
// per-frame cost anybody wants at 8,001 instances, so the level is not a
// function of the live distance but of the distance it was LAST EVALUATED at:
// the rule is asked again only when the instance has moved into a band twice as
// far or half as near. Between those crossings the answer cannot change and no
// distance test is even close to one — which is why the walk below is a compare
// against one float per slot and nothing else.
//
// WHAT IT IS NOT: a draw-time LOD. The picture's level is Ogre's own strategy
// per pass (OgreMesh.cpp), evaluated every frame with no hysteresis but with
// patch 0075's band; this is the RAY tier's, evaluated at build time, and the
// two are allowed to differ — a shadow ray does not need the silhouette the eye
// does.
void OgreScene::forgetRayLevel(uint32_t slot) {
    if (slot < mRayLevel.size()) mRayLevel[slot] = 0u;
    if (slot < mRayEvalDistance.size()) mRayEvalDistance[slot] = -1.0f;
}

void OgreScene::updateRayLevels(const Ogre::Vector3 &eye, float projScaleY, float viewportHeight) {
    if (!mGpuScene.live()) return;
    const float footprintPerMetre = sampleFootprintPerspective(1.0f, projScaleY, viewportHeight);
    if (!(footprintPerMetre > 0.0f)) return;
    const uint32_t n = mGpuScene.slotCount();
    if (!n) return;
    // A STILL CAMERA IN A STILL SCENE RUNS NOTHING. The eye is compared
    // exactly: it is the same float3 the view wrote, not a derived quantity, so
    // "did not move" is a memcmp and not a tolerance.
    const bool eyeMoved = mRayEyeValid == false || eye != mRayEye;
    const bool tableMoved = mGpuScans != mRayLevelScanSeen;
    if (!eyeMoved && !tableMoved) return;
    mRayEye = eye;
    mRayEyeValid = true;
    mRayLevelScanSeen = mGpuScans;
    ++mRayLevelWalks;

    if (mRayEvalDistance.size() < n) mRayEvalDistance.resize(n, -1.0f);
    if (mRayLevel.size() < n) mRayLevel.resize(n, 0u);
    const detail::GpuInstance *mirror = mGpuScene.mirrorData();
    for (uint32_t i = 0; i < n; ++i) {
        const detail::GpuInstance &e = mirror[i];
        uint32_t meshIndex = 0u;
        std::memcpy(&meshIndex, &e.boundsMin[3], sizeof(uint32_t));
        if (meshIndex == detail::GpuScene::kNoMesh) continue;
        const Ogre::MeshPtr &mesh = mGpuScene.meshAt(meshIndex);
        if (!mesh) continue;
        const std::vector<float> *bounds = lodBoundsFor(mesh.get());
        if (!bounds || bounds->empty()) continue;   // no chain: level 0 for ever

        // The distance Ogre's own strategies use: to the bounding SPHERE, whose
        // world radius is the local one times the largest axis scale (the rows
        // of the 3x4 world matrix are those axes).
        const Ogre::Vector3 centre(0.5f * (e.boundsMin[0] + e.boundsMax[0]),
                                   0.5f * (e.boundsMin[1] + e.boundsMax[1]),
                                   0.5f * (e.boundsMin[2] + e.boundsMax[2]));
        float scale = 0.0f;
        for (int r = 0; r < 3; ++r) {
            const float *row = &e.world[r * 4];
            const float len = std::sqrt(row[0] * row[0] + row[1] * row[1] + row[2] * row[2]);
            scale = std::max(scale, len);
        }
        if (!(scale > 0.0f) || !std::isfinite(scale)) continue;
        const float radius = float(mesh->getBoundingSphereRadius()) * scale;
        const float d = std::max(0.0f, float((centre - eye).length()) - radius);

        const float was = mRayEvalDistance[i];
        // THE 2x BAND. A never-evaluated slot (-1) always evaluates; a slot at
        // distance 0 would make every band degenerate, so it re-evaluates only
        // when it leaves 0.
        if (was >= 0.0f && d <= was * 2.0f && d * 2.0f >= was) continue;
        mRayEvalDistance[i] = d;
        ++mRayLevelEvals;
        // THE CURRENCY, with the ray's sample: ONE footprint of tolerance, the
        // footprint being the pixel this ray was cast through, grown to the
        // instance's distance. `sampleFootprintPerspective` is called with the
        // real distance so the reader sees the derivation rather than a
        // pre-multiplied constant; the multiply is the same either way.
        const float allowed = allowedWorldError(
            kRayFootprintTolerance, sampleFootprintPerspective(d, projScaleY, viewportHeight),
            scale);
        const uint32_t want = uint32_t(lodLevelForWorldError(*bounds, allowed, bounds->size()));
        if (want == mRayLevel[i]) continue;
        ++mRayLevelRefits;
        mRayLevel[i] = want;
        // THE TABLE IS WRITTEN THROUGH THE MIRROR'S OWN PATH: the slot is
        // re-staged from its node (so `prevWorld` keeps the one rule it has)
        // and the dirty list carries it to the device with the frame's other
        // writes. A direct poke into the mirror would not be copied at all.
        if (i < mItemNodes.size() && mItemNodes[i]) markGpuSlotDirty(*mItemNodes[i]);
    }
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
    out.rayLevel = in.ids[3];
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
    st.rayLevelEvals = mRayLevelEvals;
    st.rayLevelRefits = mRayLevelRefits;
    st.rayLevelWalks = mRayLevelWalks;
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
