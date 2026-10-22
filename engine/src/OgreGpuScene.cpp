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
#include "HlmsAtom.h"
#include "SurfaceCache.h"

#include "photon/voxel/PhotonVoxelMaterial.h"
#include "photon/voxel/PhotonVoxelizer.h"
#include <Vao/OgreIndexBufferPacked.h>
#include <Vao/OgreTexBufferPacked.h>

#include <OgreLogManager.h>
#include <stdexcept>
#include <OgreHlmsPbsDatablock.h>
#include <OgreRenderSystem.h>
#include <OgreRoot.h>
#include <OgreMesh2.h>
#include <OgreSubItem.h>
#include <OgreItem.h>
#include <OgreHlms.h>
#include <OgreHlmsPbsDatablock.h>
#include <OgreSubMesh2.h>
#include <Vao/OgreVertexArrayObject.h>
#include <Vao/OgreAsyncTicket.h>
#include <Vao/OgreStagingBuffer.h>
#include <Vao/OgreUavBufferPacked.h>
#include <Vao/OgreVaoManager.h>

#include <algorithm>
#include <iterator>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <unordered_map>

namespace jahshaka {
namespace engine {
namespace detail {

// ---------------------------------------------------------------------------
// WHICH SCENE A SCENE MANAGER RENDERS (EnginePrivate.h): the scene is bound on its
// manager's root scene node — Ogre's UserObjectBindings, the engine's one use of that
// node's bindings — so the answer lives and dies with the manager.
// ---------------------------------------------------------------------------
namespace {
const Ogre::String &sceneBindingKey() {
    static const Ogre::String kKey("jahshaka.scene");
    return kKey;
}
}  // namespace

void bindSceneManager(Ogre::SceneManager *sm, OgreScene *scene) {
    if (!sm) return;
    Ogre::UserObjectBindings &b = sm->getRootSceneNode(Ogre::SCENE_DYNAMIC)->getUserObjectBindings();
    if (scene) {
        // ONE SCENE PER MANAGER: a manager already bound to another scene is a construction
        // bug (two OgreScenes on one SceneManager), never a re-bind.
        assert(!b.getUserAny(sceneBindingKey()).has_value() || sceneOfManager(sm) == scene);
        b.setUserAny(sceneBindingKey(), Ogre::Any(scene));
    } else {
        b.eraseUserAny(sceneBindingKey());
    }
}

OgreScene *sceneOfManager(const Ogre::SceneManager *sm) {
    if (!sm) return nullptr;
    // getRootSceneNode is not const in Ogre; the read changes nothing.
    const Ogre::UserObjectBindings &b =
        const_cast<Ogre::SceneManager *>(sm)->getRootSceneNode(Ogre::SCENE_DYNAMIC)->getUserObjectBindings();
    const Ogre::Any &a = b.getUserAny(sceneBindingKey());
    if (!a.has_value()) return nullptr;
    OgreScene *const *p = Ogre::any_cast<OgreScene *>(&a);
    return p ? *p : nullptr;
}

bool atomTraceOn() {
    static const bool on = std::getenv("JAHSHAKA_ATOM_TRACE") != nullptr;
    return on;
}

void atomTrace(const std::string &what) {
    if (!atomTraceOn()) return;
    Ogre::Root *root = Ogre::Root::getSingletonPtr();
    Ogre::RenderSystem *rs = root ? root->getRenderSystem() : nullptr;
    const unsigned frame = rs && rs->getVaoManager() ? unsigned(rs->getVaoManager()->getFrameCount()) : 0u;
    if (Ogre::LogManager *lm = Ogre::LogManager::getSingletonPtr())
        lm->logMessage("Jahshaka atom trace: frame " + std::to_string(frame) + " " + what);
}

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
    if (!mTables.empty()) return true;
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
    // ONE COPY PER FRAME IN FLIGHT, PLUS THE ONE BEING WRITTEN (tableCopies). The
    // device's own count: Ogre recycles a resource a frame used only once that
    // many frames have passed (VulkanVaoManager::flushGpuDelayedBlocks).
    mFramesInFlight = std::max<uint32_t>(1u, uint32_t(vao->getDynamicBufferMultiplier()));
    growTo(kInitialSlots);
    growMeshTable(kInitialMeshes);
    return !mTables.empty();
}

void GpuScene::destroy() {
    // THE FEED'S CONSUMERS HEAR IT FIRST: every slot they derived from is gone.
    // They stay registered — a re-created table starts empty and they follow it.
    for (GpuSceneObserver *o : mObservers) o->gpuSceneReset();
    mPatched.clear();
    if (mVao) {
        destroyTables();
        if (mMeshBuffer) mVao->destroyUavBuffer(mMeshBuffer);
        if (mLevelBuffer) mVao->destroyUavBuffer(mLevelBuffer);
        if (mGeomBuffer) mVao->destroyUavBuffer(mGeomBuffer);
        if (mPartBuffer) mVao->destroyTexBuffer(mPartBuffer);
        if (mPartAabbBuffer) mVao->destroyUavBuffer(mPartAabbBuffer);
        if (mClusterBuffer) mVao->destroyUavBuffer(mClusterBuffer);
        if (mClusterBoxBuffer) mVao->destroyUavBuffer(mClusterBoxBuffer);
        if (mGroupBuffer) mVao->destroyUavBuffer(mGroupBuffer);
        if (mMaskTable) mVao->destroyUavBuffer(mMaskTable);
    }
    mMaskTable = nullptr;
    mMaskWords.clear();
    mMaskAddress = 0u;
    ++mMaskSerial;
    mClusterBuffer = mGroupBuffer = mClusterBoxBuffer = nullptr;
    mClusterBoxMirror.clear();
    mClusterBoxCapacity = 0u;
    mClusterMirror.clear();
    mGroupMirror.clear();
    mClusterCapacity = mGroupCapacity = mClusterCount = mMaxClustersPerMesh = 0u;
    mClusterDirty = false;
    mMeshBuffer = mLevelBuffer = mGeomBuffer = nullptr;
    mPartBuffer = nullptr;
    mPartAabbBuffer = nullptr;
    mPartCount = mPartCapacity = 0;
    mPartitionsDirty = true;
    mVao = nullptr;
    mMirror.clear();
    mBorn.clear();
    mStock.clear();
    mStockSlots.clear();
    mStockDirty.store(true, std::memory_order_relaxed);
    mSlotNode.clear();
    mStagedAt.clear();
    mFreedSlots.clear();
    mRuns.clear();
    mDests.clear();
    mMeshMirror.clear();
    mGeomMirror.clear();
    mLevelMirror.clear();
    mMeshEntries.clear();
    mRootCutIndices = 0u;
    mFreeMeshSlots.clear();
    mMeshIndex.clear();
    mSkinRows.clear();
    mMovedLastFrame.clear();
    mCopySet.clear();
    mSlotCapacity = mSlotCount = mMeshCapacity = 0u;
}

void GpuScene::growTo(uint32_t capacity) {
    if (!mVao || capacity <= mSlotCapacity) return;
    const uint32_t want = roundUpPow2(capacity, std::max(kInitialSlots, mSlotCapacity ? mSlotCapacity : kInitialSlots));
    mMirror.resize(want);
    mBorn.resize(want, 0u);
    mStock.resize(want, 0u);
    mSlotNode.resize(want, 0u);
    mStagedAt.resize(want, 0u);
    if (!mTables.empty()) {
        destroyTables();
        ++mGrows;
    }
    createTables(want);
    if (atomTraceOn()) atomTrace("instance table grown " + std::to_string(mSlotCapacity) + " -> " + std::to_string(want));
    mSlotCapacity = want;
}

// ---------------------------------------------------------------------------
// THE COPIES IN FLIGHT (ENGINE-UPGRADE-2 stage 1). The table was ONE buffer
// rewritten in place, which is correct on one queue (a frame's copies execute
// after the previous frame's reads, in submission order, behind Ogre's barriers)
// and a race the moment anything reads it from a queue that runs beside the
// frame's (an async-compute pass of frame N still reading while frame N+1's
// copies land). The ring removes the race by construction: a frame writes only
// a copy no frame of the last `framesInFlight` bound, and brings it current.
void GpuScene::createTables(uint32_t capacity) {
    const uint32_t n = mFramesInFlight + 1u;
    mTables.assign(n, nullptr);
    for (uint32_t k = 0; k < n; ++k)
        mTables[k] = mVao->createUavBuffer(capacity, sizeof(GpuInstance), 0, mMirror.data(), false);
    // EVERY COPY IS THE MIRROR NOW (each was created from it), so nothing is owed;
    // and NO FRAME HAS BOUND ANY OF THEM, so the live one is written this frame.
    mOwed.assign(n, std::vector<uint32_t>());
    mOwedAll.assign(n, 0u);
    mReadUntil.assign(n, 0u);
    mEverLive.assign(n, 0u);
    mLive = 0u;
    mLiveFrame = mVao->getFrameCount();
    mEverLive[0] = 1u;
    mReadUntil[0] = mLiveFrame;
}

void GpuScene::destroyTables() {
    // Ogre's destroy is DELAYED past the frames in flight (a frame still executing
    // may read any of them): the ring needs nothing more.
    for (Ogre::UavBufferPacked *t : mTables)
        if (t) mVao->destroyUavBuffer(t);
    mTables.clear();
    mOwed.clear();
    mOwedAll.clear();
    mReadUntil.clear();
    mEverLive.clear();
    mLive = 0u;
    mLiveFrame = 0xFFFFFFFFu;
}

bool GpuScene::beginTableWrite() {
    const uint32_t frame = mVao->getFrameCount();
    if (frame == mLiveFrame) return true;   // the frame's later commit: the same copy
    // THE FIRST WRITE OF THIS FRAME: the next copy becomes live. The outgoing one
    // may have been bound by THIS frame's readers before the switch, so its last
    // reader is this frame.
    mReadUntil[mLive] = frame;
    const uint32_t next = (mLive + 1u) % uint32_t(mTables.size());
    if (mEverLive[next] && uint32_t(frame - mReadUntil[next]) < mFramesInFlight) ++mInFlightOverwrites;
    mLive = next;
    mLiveFrame = frame;
    mEverLive[next] = 1u;
    mReadUntil[next] = frame;
    ++mCopyWrites;
    if (mOwedAll[next]) {
        mOwedAll[next] = 0u;
        mOwed[next].clear();
        return false;
    }
    if (!mOwed[next].empty()) {
        mCopySet.insert(mCopySet.end(), mOwed[next].begin(), mOwed[next].end());
        mOwed[next].clear();
        std::sort(mCopySet.begin(), mCopySet.end());
        mCopySet.erase(std::unique(mCopySet.begin(), mCopySet.end()), mCopySet.end());
        while (!mCopySet.empty() && mCopySet.back() >= mSlotCapacity) mCopySet.pop_back();
    }
    return true;
}

void GpuScene::oweOtherCopies(const std::vector<uint32_t> &written) {
    for (uint32_t k = 0; k < uint32_t(mTables.size()); ++k) {
        if (k == mLive || mOwedAll[k]) continue;
        std::vector<uint32_t> &o = mOwed[k];
        o.insert(o.end(), written.begin(), written.end());
        // BOUNDED: past the table's length the list is worth less than the table.
        if (o.size() > size_t(mSlotCapacity)) {
            o.clear();
            mOwedAll[k] = 1u;
        }
    }
}

void GpuScene::growMeshTable(uint32_t capacity) {
    if (!mVao || capacity <= mMeshCapacity) return;
    const uint32_t want = roundUpPow2(capacity, std::max(kInitialMeshes, mMeshCapacity ? mMeshCapacity : kInitialMeshes));
    mMeshMirror.resize(want);
    mLevelMirror.resize(size_t(want) * kLevelsPerMesh);
    // THE GEOMETRY ROWS grow with the mesh table and by the same doubling: one block
    // of kGeomRowsPerMesh rows per mesh entry, so a row's index is arithmetic
    // (`geomRowIndex`) and no allocator is needed. 48 B x 64 rows = 3 KB per mesh
    // entry, which is nothing beside what it replaced (a re-described table per
    // cascade per build, and before that a full copy of the world's geometry).
    mGeomMirror.resize(size_t(want) * kGeomRowsPerMesh * kGeomRowWords, 0u);
    if (mMeshBuffer) {
        mVao->destroyUavBuffer(mMeshBuffer);
        mMeshBuffer = nullptr;
    }
    if (mLevelBuffer) {
        mVao->destroyUavBuffer(mLevelBuffer);
        mLevelBuffer = nullptr;
    }
    if (mGeomBuffer) {
        mVao->destroyUavBuffer(mGeomBuffer);
        mGeomBuffer = nullptr;
    }
    mMeshBuffer = mVao->createUavBuffer(want, sizeof(GpuMesh), 0, mMeshMirror.data(), false);
    mLevelBuffer = mVao->createUavBuffer(size_t(want) * kLevelsPerMesh, sizeof(GpuMeshLevel), 0,
                                         mLevelMirror.data(), false);
    mGeomBuffer = mVao->createUavBuffer(size_t(want) * kGeomRowsPerMesh,
                                        kGeomRowWords * sizeof(uint32_t), 0, mGeomMirror.data(),
                                        false);
    mGeomDirty = false;   // the create above uploaded the mirror
    if (atomTraceOn()) atomTrace("mesh table grown " + std::to_string(mMeshCapacity) + " -> " + std::to_string(want));
    mMeshCapacity = want;
}

bool GpuScene::ensurePartitions(Ogre::HlmsManager *hlmsManager, Ogre::RenderSystem *renderSystem,
                                uint32_t partitionIndices) {
    if (!mPartitionsDirty || !mVao || !partitionIndices) return false;
    // A partition's AABB is computed FROM POSITIONS READ THROUGH THE ROWS, so the
    // rows must be on the device first (the Xid 109 lesson of this lane: a row still
    // only in the mirror is a zero address).
    flushGeomRows();

    // ONE COMPACT LIST, rebuilt whole. It is O(mesh entries x levels) and runs only
    // when the mesh set changed, so a free-list allocator for variable-length runs
    // would buy nothing but a place for bugs.
    std::vector<uint32_t> parts;     // 4 words per partition
    uint32_t count = 0;
    for (uint32_t e = 0; e < uint32_t(mMeshEntries.size()); ++e) {
        const bool live = mMeshEntries[e].refs > 0u;
        for (uint32_t l = 0; l < kLevelsPerMesh; ++l) {
            GpuMeshLevel &lv = mLevelMirror[size_t(e) * kLevelsPerMesh + l];
            lv.partBase = count;
            lv.partCount = 0;
            if (!live || lv.geomRow == kNoGeomRow || !lv.indexCount) continue;
            const uint32_t n = (lv.indexCount + partitionIndices - 1u) / partitionIndices;
            for (uint32_t j = 0; j < n; ++j) {
                const uint32_t first = lv.firstIndex + j * partitionIndices;
                const uint32_t num = std::min(lv.indexCount - j * partitionIndices, partitionIndices);
                parts.push_back(lv.geomRow);
                parts.push_back(first);
                parts.push_back(num);
                parts.push_back(0u);
            }
            lv.partCount = n;
            count += n;
        }
    }
    mLevelDirty = true;
    mPartCount = count;
    mPartitionsDirty = false;
    // The level table carries partBase/partCount now; the gather reads it.
    if (mLevelBuffer && !mLevelMirror.empty()) {
        mLevelBuffer->upload(mLevelMirror.data(), 0, mLevelMirror.size());
        mLevelDirty = false;
        ++mCopies;
    }
    if (!count) return true;

    // GROWN BY DOUBLING like every other table here, never shrunk: a scene that
    // loses meshes keeps the capacity it will probably want again.
    if (count > mPartCapacity) {
        if (mPartBuffer) mVao->destroyTexBuffer(mPartBuffer);
        if (mPartAabbBuffer) mVao->destroyUavBuffer(mPartAabbBuffer);
        uint32_t cap = std::max(mPartCapacity, 256u);
        while (cap < count) cap *= 2u;
        mPartBuffer = mVao->createTexBuffer(Ogre::PFG_RGBA32_UINT, size_t(cap) * 4u * sizeof(uint32_t),
                                            Ogre::BT_DEFAULT, nullptr, false);
        mPartAabbBuffer = mVao->createUavBuffer(cap, 8u * sizeof(float), 0, nullptr, false);
        mPartCapacity = cap;
    }
    mPartBuffer->upload(parts.data(), 0, parts.size() * sizeof(uint32_t));
    ++mCopies;
    Ogre::PhotonVoxelizer::computePartitionAabbs(hlmsManager, renderSystem, mGeomBuffer, mPartBuffer,
                                              mPartAabbBuffer, count);
    return true;
}

// ---------------------------------------------------------------------------
// THE CLUSTER TABLES (ATOM-CLUSTER-CUT): every live mesh's DAG, concatenated. Rebuilt
// WHOLE when the mesh set changes, exactly like the partitions above — a DAG cannot
// change while its mesh lives, and the rebuild is O(clusters) once per attach batch.
GpuClusterBox packClusterBox(const float bmin[3], const float bmax[3], float radius) {
    GpuClusterBox b;
    float e[3];
    for (int k = 0; k < 3; ++k) {
        b.centre[k] = 0.5f * (bmin[k] + bmax[k]);
        e[k] = std::max(0.0f, 0.5f * (bmax[k] - bmin[k]));
    }
    // ROUNDED UP against the centre's own rounding too: the decoded half-extent covers every vertex. A box whose
    // half-extent the sphere's radius cannot carry (a sphere that is not the vertices' bound) keeps the sphere's box.
    if (!(radius > 0.0f)) return GpuClusterBox();
    uint32_t q[3];
    for (int k = 0; k < 3; ++k) {
        const float need = e[k] + std::abs(b.centre[k]) * 1.0e-6f;
        const double frac = std::ceil(double(need) / double(radius) * 1023.0);
        if (frac > 1023.0) return GpuClusterBox();
        q[k] = uint32_t(std::max(0.0, frac));
    }
    b.extent = q[0] | (q[1] << 10u) | (q[2] << 20u);
    return b;
}

void GpuScene::setMeshDag(uint32_t meshIndex, std::vector<GpuCluster> clusters,
                          std::vector<GpuClusterGroup> groups, uint32_t clusterRow,
                          std::vector<GpuClusterBox> boxes) {
    if (meshIndex >= mMeshEntries.size()) return;
    if (boxes.size() != clusters.size()) boxes.assign(clusters.size(), GpuClusterBox());
    mMeshEntries[meshIndex].boxes = std::move(boxes);
    // THE ROOT CUT (R8): Types.h clusterRootDrawn on the uploaded tables (error[0] the group's error, error[3] the
    // clusters it produced; local group indices) - every current reference counts it.
    {
        MeshEntry &me = mMeshEntries[meshIndex];
        mRootCutIndices -= std::min<uint64_t>(mRootCutIndices, uint64_t(me.rootCut) * me.refs);
        uint32_t root = 0u;
        for (const GpuCluster &c : clusters) {
            if (c.group >= groups.size()) continue;
            const GpuClusterGroup &g = groups[c.group];
            const bool last = g.error[0] >= kClusterTerminal || g.error[3] == 0.0f;
            const bool leafOrFinite = c.refined == kNoGroup ||
                                      (c.refined < groups.size() && groups[c.refined].error[0] < kClusterTerminal);
            if (last && leafOrFinite) root += c.indexCount;
        }
        me.rootCut = root;
        mRootCutIndices += uint64_t(root) * me.refs;
    }
    mMeshEntries[meshIndex].clusters = std::move(clusters);
    mMeshEntries[meshIndex].groups = std::move(groups);
    mMeshMirror[meshIndex].dag[3] = clusterRow;
    if (atomTraceOn()) atomTrace("mesh DAG set: entry " + std::to_string(meshIndex));
    mMeshDirty = true;
    mClusterDirty = true;
}

void GpuScene::flushClusterTables() {
    // Only when the mesh set moved: a scene with no DAG-bearing mesh yet has no tables,
    // and the id pass records no cut for it (OgreAtomIdPass.cpp). Creating empty tables
    // at the first flush instead moved the device's allocation order and turned
    // atom.cluster_crack's harness picture (a BT_DEFAULT index buffer rewritten per cut)
    // non-deterministic — measured, root cause in the harness not found; reported.
    if (!mClusterDirty || !mVao) return;
    mClusterDirty = false;
    mClusterMirror.clear();
    mClusterBoxMirror.clear();
    mGroupMirror.clear();
    mMaxClustersPerMesh = 0u;
    for (uint32_t e = 0; e < uint32_t(mMeshEntries.size()); ++e) {
        MeshEntry &me = mMeshEntries[e];
        GpuMesh &gm = mMeshMirror[e];
        if (me.refs == 0u || me.rowBlock || me.clusters.empty() || me.groups.empty()) {
            gm.dag[0] = gm.dag[1] = gm.dag[2] = 0u;
            gm.localBoundsMin[3] = 0.0f;
            continue;
        }
        const uint32_t cBase = uint32_t(mClusterMirror.size()), gBase = uint32_t(mGroupMirror.size());
        for (GpuCluster c : me.clusters) {
            c.group += gBase;
            if (c.refined != kNoGroup) c.refined += gBase;
            mClusterMirror.push_back(c);
        }
        mClusterBoxMirror.insert(mClusterBoxMirror.end(), me.boxes.begin(), me.boxes.end());
        mGroupMirror.insert(mGroupMirror.end(), me.groups.begin(), me.groups.end());
        gm.dag[0] = cBase;
        gm.dag[1] = uint32_t(me.clusters.size());
        gm.dag[2] = gBase;
        const uint32_t groupCount = uint32_t(me.groups.size());
        std::memcpy(&gm.localBoundsMin[3], &groupCount, sizeof(groupCount));
        mMaxClustersPerMesh = std::max(mMaxClustersPerMesh, gm.dag[1]);
    }
    mClusterCount = uint32_t(mClusterMirror.size());
    mMeshDirty = true;
    // GROWN BY DOUBLING, never shrunk; a re-create hands the old buffer to Ogre's
    // delayed destruction (a consumer re-reads the pointer before every dispatch).
    const auto grow = [&](Ogre::UavBufferPacked *&buf, uint32_t &cap, size_t count, size_t stride,
                          const void *data) {
        if (!buf || count > cap) {
            if (buf) mVao->destroyUavBuffer(buf);
            uint32_t want = std::max(cap, 1024u);
            while (want < count) want *= 2u;
            std::vector<unsigned char> init(size_t(want) * stride, 0u);
            if (count) std::memcpy(init.data(), data, count * stride);
            buf = mVao->createUavBuffer(want, uint32_t(stride), 0, init.data(), false);
            cap = want;
        } else if (count) {
            buf->upload(data, 0, count);
        }
        ++mCopies;
    };
    const uint32_t clusterCapWas = mClusterCapacity, groupCapWas = mGroupCapacity;
    grow(mClusterBuffer, mClusterCapacity, mClusterMirror.size(), sizeof(GpuCluster), mClusterMirror.data());
    grow(mClusterBoxBuffer, mClusterBoxCapacity, mClusterBoxMirror.size(), sizeof(GpuClusterBox),
         mClusterBoxMirror.data());
    grow(mGroupBuffer, mGroupCapacity, mGroupMirror.size(), sizeof(GpuClusterGroup), mGroupMirror.data());
    if (atomTraceOn())
        atomTrace("cluster tables rebuilt: " + std::to_string(mClusterMirror.size()) + " clusters, " +
                  std::to_string(mGroupMirror.size()) + " groups" +
                  (mClusterCapacity != clusterCapWas || mGroupCapacity != groupCapWas ? " (re-created)" : ""));
    // THE MESH TABLE WITH THEM (the fix round's F4): the rebuild rebased every entry's
    // `dag`, and a caller outside update() (the id pass, the screen decode's arming)
    // must never pair the new cluster table with the old bases for a frame.
    if (mMeshBuffer && !mMeshMirror.empty()) {
        mMeshBuffer->upload(mMeshMirror.data(), 0, mMeshMirror.size());
        mMeshDirty = false;
        ++mCopies;
    }
}

uint64_t GpuScene::recordBound() const {
    uint64_t bound = 0;
    for (uint32_t e = 0; e < uint32_t(mMeshEntries.size()); ++e) {
        const uint32_t refs = mMeshEntries[e].refs;
        if (!refs) continue;
        // LEVEL 0 IS THE FINEST, so its partition count bounds every level's.
        bound += uint64_t(refs) * mLevelMirror[size_t(e) * kLevelsPerMesh].partCount;
    }
    return bound;
}

void GpuScene::flushGeomRows() {
    if (!mGeomDirty || !mGeomBuffer || mGeomMirror.empty()) return;
    mGeomBuffer->upload(mGeomMirror.data(), 0, mGeomMirror.size() / kGeomRowWords);
    mGeomDirty = false;
    ++mCopies;
}

void GpuScene::setLevelGeomRow(uint32_t meshIndex, uint32_t level, uint32_t rowIndex) {
    const size_t at = size_t(meshIndex) * kLevelsPerMesh + level;
    if (level >= kLevelsPerMesh || at >= mLevelMirror.size()) return;
    mLevelMirror[at].geomRow = rowIndex;
    mLevelDirty = true;
}

void GpuScene::stageGeomRow(uint32_t rowIndex, const void *row48Bytes) {
    const size_t at = size_t(rowIndex) * kGeomRowWords;
    if (at + kGeomRowWords > mGeomMirror.size()) return;   // no such mesh entry
    std::memcpy(mGeomMirror.data() + at, row48Bytes, kGeomRowWords * sizeof(uint32_t));
    mGeomDirty = true;
}

void GpuScene::ensureSlots(uint32_t count) {
    if (count > mSlotCapacity) growTo(count);
}

void GpuScene::setSlotCount(uint32_t n) {
    ensureSlots(n);
    mSlotCount = n;
}

void GpuScene::stage(uint32_t slot, const GpuInstance &in, uint64_t node) {
    if (slot >= mSlotCapacity) ensureSlots(slot + 1u);
    if (stageRow(slot, in, node)) noteStaged(slot);
}

bool GpuScene::stageRow(uint32_t slot, const GpuInstance &in, uint64_t node) {
    if (slot >= mSlotCapacity) return false;   // never grown here: a lane's caller pre-sizes (setSlotCount)
    // ONCE PER UPDATE (see the header): a second stage would read the world the
    // first just wrote and call it the PREVIOUS world.
    if (mStagedAt[slot] == mUpdateSerial) return false;
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
    mSlotNode[slot] = node;
    markStock(slot, &dst);
    return true;
}

const std::vector<uint32_t> &GpuScene::stockSlots() {
    if (mStockDirty.exchange(false, std::memory_order_relaxed)) {
        mStockSlots.clear();
        for (uint32_t i = 0; i < mSlotCount && i < uint32_t(mStock.size()); ++i)
            if (mStock[i]) mStockSlots.push_back(i);
    }
    return mStockSlots;
}

void GpuScene::addObserver(GpuSceneObserver *o) {
    if (!o || std::find(mObservers.begin(), mObservers.end(), o) != mObservers.end()) return;
    // A LANE HALF NEEDS A BIT (decision 22.6): the lanes' owed word has 32, one per observer index — a lane-half
    // observer at index 32 or more would never be told (notifyLanes stops at 32, the replay skips it). Refused here.
    if (o->hasLaneHalf() && mObservers.size() >= 32u)
        throw std::logic_error("GpuScene::addObserver: a lane-half observer past index 31 (the owed word has 32 bits)");
    mObservers.push_back(o);
    // THE PRIMING WALK — the only walk the feed ever makes, once per consumer.
    for (uint32_t i = 0; i < mSlotCount && i < mSlotCapacity; ++i)
        if (mBorn[i]) o->gpuSlotChanged(i, &mMirror[i]);
}

void GpuScene::removeObserver(GpuSceneObserver *o) {
    mObservers.erase(std::remove(mObservers.begin(), mObservers.end(), o), mObservers.end());
}

void GpuScene::patchRayLevel(uint32_t slot, uint32_t level) {
    if (slot >= mSlotCapacity || !mBorn[slot] || mMirror[slot].ids[3] == level) return;
    mMirror[slot].ids[3] = level;
    mPatched.push_back(slot);
    ++mWrites;
    notify(slot, &mMirror[slot]);
}

void GpuScene::onSlotFreed(uint32_t slot) {
    if (slot >= mSlotCapacity) return;
    std::memset(&mMirror[slot], 0, sizeof(GpuInstance));
    markStock(slot, nullptr);
    mBorn[slot] = 0u;
    mSlotNode[slot] = 0u;
    mStagedAt[slot] = 0u;
    mFreedSlots.push_back(slot);
    notify(slot, nullptr);
}

void GpuScene::onSlotMoved(uint32_t from, uint32_t to) {
    if (from >= mSlotCapacity || to >= mSlotCapacity) return;
    // THE SWAP-REMOVE, IN ONE PLACE. The mover keeps its poses (it did not
    // move in the world, only in the index), so `prevWorld` travels with it —
    // a reprojection of the object that was renumbered must not read the
    // previous pose of the object that DIED in that slot.
    std::memcpy(&mMirror[to], &mMirror[from], sizeof(GpuInstance));
    mBorn[to] = mBorn[from];
    markStock(to, mBorn[to] ? &mMirror[to] : nullptr);
    mSlotNode[to] = mSlotNode[from];
    mStagedAt[to] = 0u;      // it must be re-staged/copied this update
    notify(to, mBorn[to] ? &mMirror[to] : nullptr);
    onSlotFreed(from);       // ...and the tail it came from is cleared and copied
}

void GpuScene::update(const std::vector<uint32_t> &dirtySlots) {
    mLastDirtyCount = unsigned(dirtySlots.size());
    if (!live()) return;

    // THE CLUSTER TABLES before the mesh table: their rebuild writes the entries'
    // `dag` bases.
    flushClusterTables();
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
    // AND THE GEOMETRY ROWS, for the same reason in one more step: a level entry's
    // `geomRow` must name a row that already holds addresses before anything reads it.
    // Uploaded WHOLE and only when a mesh arrived or left — a mesh's rows never change
    // while it lives, so this is once per attach batch and never per frame.
    flushGeomRows();

    mCopySet.clear();
    mCopySet.insert(mCopySet.end(), dirtySlots.begin(), dirtySlots.end());
    // FREED SLOTS RIDE ALONG. A slot past `slotCount()` still occupies device
    // memory, and a dead item's entry left there is a ghost a reader that got
    // its bound wrong would trace. Clearing costs one entry per removal.
    for (uint32_t slot : mFreedSlots)
        if (slot < mSlotCapacity && !stagedThisUpdate(slot)) mCopySet.push_back(slot);
    mFreedSlots.clear();
    // RAY-LEVEL PATCHES nobody flushed yet (patchRayLevel) ride along too.
    mCopySet.insert(mCopySet.end(), mPatched.begin(), mPatched.end());
    mPatched.clear();

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

    std::sort(mCopySet.begin(), mCopySet.end());
    mCopySet.erase(std::unique(mCopySet.begin(), mCopySet.end()), mCopySet.end());
    while (!mCopySet.empty() && mCopySet.back() >= mSlotCapacity) mCopySet.pop_back();
    copySet();
}

void GpuScene::copySet() {
    if (mCopySet.empty()) return;
    const auto t0 = std::chrono::steady_clock::now();
    // THE COPY THIS FRAME WRITES, brought current: this frame's set is what the
    // OTHER copies are owed afterwards; what the live copy itself was owed joins
    // the set now (kept sorted and unique: the coalescer below needs runs).
    mWritten.assign(mCopySet.begin(), mCopySet.end());
    if (!beginTableWrite()) {
        mCopySet.resize(mSlotCapacity);
        for (uint32_t i = 0; i < mSlotCapacity; ++i) mCopySet[i] = i;
    }
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
            mTables[mLive], size_t(r.first) * sizeof(GpuInstance), srcOffset, len));
        srcOffset += len;
    }
    sb->unmap(&mDests[0], mDests.size());
    sb->removeReferenceCount();
    oweOtherCopies(mWritten);
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
        mRootCutIndices += mMeshEntries[it->second].rootCut;
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
    ++mMeshSetSerial;
    mPartitionsDirty = true;      // a NEW mesh: its levels have no partitions yet
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
    mRootCutIndices -= std::min<uint64_t>(mRootCutIndices, mMeshEntries[index].rootCut);
    if (mMeshEntries[index].refs > 1u) {
        --mMeshEntries[index].refs;
        return;
    }
    const bool hadDag = !mMeshEntries[index].clusters.empty();
    mMeshEntries[index] = MeshEntry();
    mMeshIndex.erase(it);
    ++mMeshSetSerial;
    mFreeMeshSlots.push_back(index);
    mPartitionsDirty = true;      // the last reference: its partitions go with it
    if (hadDag) mClusterDirty = true;   // ...and its clusters
    if (atomTraceOn()) atomTrace("mesh released: entry " + std::to_string(index) + (hadDag ? " (with a DAG)" : ""));
    // THE ENTRY IS ZEROED, not left behind: a slot recycled to a different mesh
    // must never be readable as the dead one's geometry (the PhotonVoxelMaterial
    // by-pointer aliasing lesson, DOCS/traps/ENGINE.md).
    mMeshMirror[index] = GpuMesh();
    for (uint32_t l = 0; l < kLevelsPerMesh; ++l)
        mLevelMirror[size_t(index) * kLevelsPerMesh + l] = GpuMeshLevel();
    mMeshDirty = true;
    mLevelDirty = true;
}

// ---------------------------------------------------------------------------
// THE ROW OVERRIDE (PHOTON-SKIN-1). A row block is a mesh-table entry that holds
// rows and nothing else — see GpuScene.h for why it is taken from the same table.
uint32_t GpuScene::acquireRowBlock() {
    if (!live()) return kNoMesh;
    uint32_t index;
    if (!mFreeMeshSlots.empty()) {
        index = mFreeMeshSlots.back();
        mFreeMeshSlots.pop_back();
    } else {
        index = uint32_t(mMeshEntries.size());
        mMeshEntries.push_back(MeshEntry());
        if (index + 1u > mMeshCapacity) growMeshTable(index + 1u);
    }
    mMeshEntries[index] = MeshEntry();
    mMeshEntries[index].rowBlock = true;
    // The entry DESCRIBES NOTHING: zero counts, and every level "no geometry", so
    // a reader that walked the mesh table by index would find an empty mesh.
    mMeshMirror[index] = GpuMesh();
    for (uint32_t l = 0; l < kLevelsPerMesh; ++l)
        mLevelMirror[size_t(index) * kLevelsPerMesh + l] = GpuMeshLevel();
    mMeshDirty = true;
    mLevelDirty = true;
    return index;
}

void GpuScene::releaseRowBlock(uint32_t entry) {
    if (entry >= mMeshEntries.size() || !mMeshEntries[entry].rowBlock) return;
    mMeshEntries[entry] = MeshEntry();
    const size_t at = size_t(geomRowIndex(entry, 0u, 0u)) * kGeomRowWords;
    const size_t words = size_t(kGeomRowsPerMesh) * kGeomRowWords;
    if (at + words <= mGeomMirror.size()) {
        std::fill(mGeomMirror.begin() + ptrdiff_t(at), mGeomMirror.begin() + ptrdiff_t(at + words), 0u);
        mGeomDirty = true;
    }
    mFreeMeshSlots.push_back(entry);
}

void GpuScene::setSkinRow(uint32_t node, uint32_t row) {
    if (row == kNoGeomRow) mSkinRows.erase(node);
    else mSkinRows[node] = row;
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

/// THE METAL ANSWER (kGpuMetal), from a datablock: the metallic workflow says it
/// outright, the two specular workflows through F0 — getFresnel(), never the specular
/// colour kS — (a dielectric's is 0.02-0.08, a metal's its colour). One function, two callers: gpuFlagsFor, and setPbrMaterial's
/// in-place edit, which marks the wearers' slots when the answer flips.
bool OgreScene::gpuIsMetal(const Ogre::HlmsDatablock *db) {
    if (!db || !db->getCreator() || db->getCreator()->getType() != Ogre::HLMS_PBS) return false;
    const auto *pbs = static_cast<const Ogre::HlmsPbsDatablock *>(db);
    if (pbs->getWorkflow() == Ogre::HlmsPbsDatablock::MetallicWorkflow)
        return pbs->getMetalness() >= kGpuMetalFloor;
    // F0 is the FRESNEL term in both specular workflows (applyPbr: setFresnel); the
    // specular COLOUR, kS, is a tint every workflow carries (white by default) and says
    // nothing about metal — reading it flagged every spec-gloss import a metal.
    // ...and a SCALAR F0 lives in x alone (setFresnel(f0, separate = false); y and z then
    // hold whatever an earlier value left — measured 0.818 behind a 0.04 F0).
    const Ogre::Vector3 f0 = pbs->getFresnel();
    const float f0Max = pbs->hasSeparateFresnel() ? std::max(f0.x, std::max(f0.y, f0.z)) : f0.x;
    return f0Max > kGpuMetalF0;
}

/// THE ONE PLACE THE PREDICATES ARE COMPUTED (design §3: every duplicate "is
/// this item ray-visible" test outside this word goes).
///
/// The TRACED SET is the conjunction `gatherRayInstances` used to walk for, and
/// each exclusion is load-bearing for the same reasons it always was: editor
/// furniture and the backdrop carry their own channel instead of kVisibleBit;
/// the overlay queues are unlit and depth-test-off. An ALPHA-TESTED item is IN
/// since REFLECT-MOVERS-2: its instance is written FORCE_NO_OPAQUE and every ray
/// query tests its candidates against the material's mask (jah_rq_alpha.glsl),
/// so a fence casts a ray shadow with holes and shows in a ray reflection (audit
/// C-16 answered). A SKINNED item is IN since PHOTON-SKIN-1: the ray tier
/// traces it through its own structure over its skin cache (its posed
/// vertices), and leaves it out on a frame the cache is not ready — never at the
/// mesh's bind pose (audit C-5's T-pose, which is what the exclusion was for).
Ogre::uint32 OgreScene::gpuFlagsFor(const Node &n) const {
    // Inside a compose the split's switch was asked once (GpuSceneSubstrate::atomDraw).
    return gpuFlagsFor(n, mSubstrate.atomDraw >= 0 ? mSubstrate.atomDraw != 0 : atomDrawOn());
}

Ogre::uint32 OgreScene::gpuFlagsFor(const Node &n, bool atomDraw) const {
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
    // A METAL (REFLECT-FIX-1, kGpuMetal): sub-item 0's datablock, the material word's
    // rule (every mesh this engine builds has one submesh).
    if (item->getNumSubItems() && gpuIsMetal(item->getSubItem(0)->getDatablock())) f |= kGpuMetal;
    // A PLANAR MIRROR (D3-HIT-SHADE-2): its picture is a reflection, and the caches
    // are a DIFFUSE store (a metal mirror's card and voxels hold its kD = 0: black) —
    // a ray that hits it is the decode's, which shades the mirror's own material with
    // the specular environment the pass holds (jahHitAlwaysDecodes). Ogre's own
    // per-renderable test (PlanarReflections::hasPlanarReflections); the slot is marked
    // at both edges (armReflector / disarmReflector, OgrePlanar.cpp).
    if (mPlanar && item->getNumSubItems() && mPlanar->hasPlanarReflections(item->getSubItem(0)))
        f |= kGpuPlanar;
    // ...and it must be IN the graph: an Item with no parent node draws nothing
    // and has no world transform to trace (the old walk skipped it outright).
    if ((f & kGpuVisible) && !(f & kGpuOverlay) && item->getMesh() && item->getParentNode())
        f |= kGpuRayTraced;
    // THE RENDER-QUEUE SPLIT (ATOM S3-DRAW): the id pass draws it, the decode
    // shades it (atomRouteFor, OgreAtomDraw.cpp). A route that moves with nothing else
    // moved is an in-place datablock edit or a bake, which the PBS change log marks.
    if (atomDraw) {
        const AtomRoute route = atomRouteFor(n, f);
        const Ogre::HlmsDatablock *db0 = n.item->getNumSubItems() ? n.item->getSubItem(0)->getDatablock() : nullptr;
        if (route == AtomRoute::Atom) {
            f |= kGpuAtom | kGpuAtomCaster;
            // ...BOTH FACES, PER PASS (ATOM-TWO-SIDED-1; ENGINE-POLISH-ATOM-1 stage 3): the id pass's from the
            // colour macroblock, the caster cut's from the caster's (a cut-out's caster draws both faces).
            if (db0->getMacroblock()->mCullMode == Ogre::CULL_NONE) f |= kGpuTwoSided;
            if (db0->getMacroblock(true)->mCullMode == Ogre::CULL_NONE) f |= kGpuCasterTwoSided;
        } else if (atomCasterThroughCut(n, f, route)) {
            // THE CASTER ALONE THROUGH THE CUT (ENGINE-POLISH-ATOM-1 stage 2): its colour stays on PBS.
            f |= kGpuAtomCaster;
            if (db0->getMacroblock(true)->mCullMode == Ogre::CULL_NONE) f |= kGpuCasterTwoSided;
        }
    }
    return f;
}

/// THE MATERIAL WORD (ATOM P4b) — `gpuFlagsFor`'s sibling, and the ONE place
/// GpuInstance::ids.y is decided: {pool : 16 | slot : 16} of the item's material in
/// the chain's shared PhotonVoxelMaterial store.
///
/// A LOOKUP, NEVER A CONVERSION. Converting can render a texture into the store's
/// pool and needs the store's temp resources, which exist only inside a GI build's
/// bracket; this runs in the per-frame dirty scan. So a datablock the store has not
/// seen yet answers "none" here, and the slot is QUEUED: the next GI build converts
/// it inside its bracket and marks the slot, and the scan re-composes it with the
/// real word before that build gathers. The voxel gather skips an instance whose
/// word is "none" — for at most the one rebuild in which it was attached.
///
/// SUB-ITEM 0's DATABLOCK, because the word is one per instance: every mesh this
/// engine builds has one submesh (buildMeshV2), and the geometry rows the gather
/// reads are submesh 0's too.
uint32_t OgreScene::gpuMaterialWordFor(const Node &n, Ogre::uint32 flags, bool &queue) const {
    queue = false;
    if (!n.item || !n.item->getNumSubItems() || !mPhoton.materials())
        return detail::GpuScene::kNoMaterialWord;
    const Ogre::HlmsDatablock *db = n.item->getSubItem(0)->getDatablock();
    if (!db) return detail::GpuScene::kNoMaterialWord;
    const Ogre::PhotonVoxelMaterial::DatablockConversionResult *r = mPhoton.materials()->lookupDatablock(db);
    if (!r) {
        // Only a GI-visible item is worth converting: nothing else is ever gathered.
        queue = (flags & detail::kGpuGiVisible) && n.itemSlot != size_t(-1);
        return detail::GpuScene::kNoMaterialWord;
    }
    // Sixteen bits each: a pool holds 1,024 rows (PhotonVoxelMaterial's const-buffer size), and
    // 65,535 pools is 67 million materials.
    return (r->bucketIdx << 16u) | (r->slotIdx & 0xFFFFu);
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
    ComposeTail tail;
    composeGpuInstanceLane(n, world, graphIsCurrent, out, tail);
    applyComposeTail(n, tail);
}

void OgreScene::applyComposeTail(const Node &n, const ComposeTail &tail) const {
    if (tail.atom) ++mSubstrate.lastTailAtoms;
    if (tail.queueMaterial) ++mSubstrate.lastTailQueues;
    // (the identity row's digest: the tail, in the order it is applied — decision 18.4)
    if (n.itemSlot != size_t(-1)) {
        const unsigned char t[2] = { 'T', (unsigned char)((tail.atom ? 1u : 0u) | (tail.queueMaterial ? 2u : 0u) |
                                                          (tail.casterInCut ? 4u : 0u)) };
        const uint32_t slot = uint32_t(n.itemSlot);
        mGpuScene.digestFold(t, 2u);
        mGpuScene.digestFold(&slot, sizeof(slot));
    }
    // THE SPLIT'S QUEUE follows the route: an Atom item lives in kAtomRenderQueue (which a view with an id pass
    // skips), everything else where its material files it (renderQueueFor).
    placeAtomQueue(n, tail.atom);
    // ...and THE CASTER'S ROUTE: a caster the cut draws leaves the stock shadow passes' channel (kStockCasterBit).
    placeAtomCaster(n, tail.casterInCut);
    if (tail.queueMaterial) mPhoton.queueMaterialSlot(uint32_t(n.itemSlot));
}

void OgreScene::composeGpuInstanceLane(const Node &n, const Ogre::Matrix4 &world, bool graphIsCurrent,
                                       detail::GpuInstance &out, ComposeTail &tail) const {
    out = detail::GpuInstance();
    tail = ComposeTail();
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
    // (the split's queue — the Atom route's render-queue edit — is the serial tail's: applyComposeTail)
    tail.atom = (flags & kGpuAtom) != 0u;
    tail.casterInCut = (flags & kGpuAtomCaster) != 0u;   // ...and its stock-caster channel's (placeAtomCaster)
    out.ids[0] = gpuKeyOf(n);   // Ogre's 32-bit key, never the 64-bit id (U3 stage 3, F6)
    out.ids[1] = gpuMaterialWordFor(n, flags, tail.queueMaterial);
    out.ids[2] = uint32_t(n.lightMask);
    // THE RAY LEVEL (AT-A8r) travels with the entry rather than being poked
    // into the mirror: every write to this table goes through one composer, so
    // a slot re-staged for any other reason keeps the level it was given.
    if (n.itemSlot != size_t(-1) && n.itemSlot < mRayLevel.size())
        out.ids[3] = mRayLevel[n.itemSlot];
    // THE RASTER WORDS (see GpuInstance::raster): sub-item 0's PBS material word, and
    // the tangent's place in the vertex of the mesh's level-0 VAO — submesh 0, like
    // every other geometry fact this table carries.
    out.raster[0] = detail::HlmsAtom::kNoMaterialWord;
    out.raster[1] = 0xFFFFFFFFu;
    // THE SKIN ROW (PHOTON-SKIN-1): the item's own posed geometry, when the ray
    // tier has made it one (GpuInstance::raster's note). Read from the node's
    // record in the table, so a slot re-staged for any other reason keeps it.
    out.raster[2] = mGpuScene.skinRowOf(gpuKeyOf(n));
    if (item->getNumSubItems()) {
        const Ogre::SubItem *sub = item->getSubItem(0);
        out.raster[0] = detail::HlmsAtom::materialWordOf(sub->getDatablock());
        // THE CASTER'S CONSTANT BIAS (ATOM-SHADOWS-1): the datablock's own, which the
        // stock caster reads per draw (HlmsPbs::fillBuffersFor) and the caster cut per
        // instance (atom_caster.vert).
        if (const Ogre::HlmsDatablock *db = sub->getDatablock())
            std::memcpy(&out.raster[3], &db->mShadowConstantBias, sizeof(uint32_t));
        const Ogre::VertexArrayObjectArray &vaos = sub->getSubMesh()->mVao[Ogre::VpNormal];
        if (!vaos.empty() && vaos[0]) {
            size_t posSource = 0u, posOffset = 0u, tanSource = 0u, tanOffset = 0u;
            const Ogre::VertexElement2 *pos =
                vaos[0]->findBySemantic(Ogre::VES_POSITION, posSource, posOffset);
            const Ogre::VertexElement2 *tan =
                vaos[0]->findBySemantic(Ogre::VES_TANGENT, tanSource, tanOffset);
            if (pos && tan && tan->mType == Ogre::VET_FLOAT4 && tanSource == posSource &&
                (tanOffset & 3u) == 0u)
                out.raster[1] = uint32_t(tanOffset);
        }
    }
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

    // SUBMESH 0's CHAIN in the level table, and the GEOMETRY ROWS of every
    // (level, submesh) beside it. `GpuMeshLevel::geomRow` is submesh 0's row and the
    // rows of one level are contiguous, so a shader reaches submesh s at `geomRow + s`
    // (GpuScene::geomRowIndex is the one place that arithmetic lives).
    detail::GpuMeshLevel levels[detail::GpuScene::kLevelsPerMesh];
    uint32_t levelCount = 0u;
    struct StagedRow {
        uint32_t level = 0u, submesh = 0u;
        Ogre::PhotonVoxelizer::GeometryRow row;
    };
    std::vector<StagedRow> rows;
    bool levelHasBase[detail::GpuScene::kLevelsPerMesh] = {};
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
            // A MISSING BOUND IS INFINITE, NEVER ZERO. Level 0 is honestly 0 (the
            // authored geometry has no error against itself), but a level the
            // bake measured no bound for — a mesh carrying more VAOs than
            // `lodBounds` entries — would read `0 < allowed` as FREE and the
            // GLSL walk would run all the way to the coarsest level it can see.
            // The CPU rule cannot: it caps at `bounds.size()`. FLT_MAX makes the
            // device's walk stop where the host's does.
            levels[l].bound = l == 0u ? 0.0f
                              : (size_t(l - 1u) < bounds.size() ? bounds[l - 1u]
                                                               : std::numeric_limits<float>::max());
        }
        if (!vaos.empty() && vaos[0]) {
            desc.counts[0] = uint32_t(vaos[0]->getVertexBuffers().empty()
                                          ? 0u
                                          : vaos[0]->getVertexBuffers()[0]->getNumElements());
            desc.counts[1] = vaos[0]->getPrimitiveCount();
        }

        // THE ROWS, DESCRIBED ONCE PER MESH (ATOM P4b). The voxeliser used to do this
        // for every mesh in the volume on EVERY build of EVERY cascade — a CPU walk over
        // the scene's geometry per rebuild, for data that cannot change while a mesh
        // lives. `describeGeometryRow` is Ogre's because the shader that reads the row
        // is Ogre's, and it refuses (with one log line) a (level, submesh) it cannot
        // read in place: no index buffer, no float3 position, an unaligned stride, or a
        // device with no buffer device addresses.
        const uint32_t submeshes =
            std::min(uint32_t(rec.mesh->getNumSubMeshes()), detail::GpuScene::kSubmeshesPerMesh);
        if (uint32_t(rec.mesh->getNumSubMeshes()) > detail::GpuScene::kSubmeshesPerMesh &&
            !mWarnedGeomSubmeshes) {
            mWarnedGeomSubmeshes = true;
            Ogre::LogManager::getSingleton().logMessage(
                "WARNING: a mesh has more submeshes than the GPU geometry table holds (" +
                std::to_string(rec.mesh->getNumSubMeshes()) + " > " +
                std::to_string(detail::GpuScene::kSubmeshesPerMesh) +
                "); the extra submeshes will not bounce light");
        }
        Ogre::VaoManager *vaoMgr =
            mRoot ? mRoot->getRenderSystem()->getVaoManager() : nullptr;
        for (uint32_t l = 0; vaoMgr && l < levelCount && l < detail::GpuScene::kLevelsPerMesh; ++l) {
            for (uint32_t sm = 0; sm < submeshes; ++sm) {
                Ogre::PhotonVoxelizer::GeometryRow row;
                if (!Ogre::PhotonVoxelizer::describeGeometryRow(rec.mesh, l, sm, vaoMgr, row))
                    continue;
                rows.push_back(StagedRow{ l, sm, row });
                if (sm == 0u) levelHasBase[l] = true;
            }
        }
    }

    const uint32_t index = mGpuScene.acquireMesh(rec.mesh, desc, levels, levelCount);
    if (index == detail::GpuScene::kNoMesh) return index;

    // The rows, now that the entry's index — and therefore every row's index — is known.
    for (const StagedRow &sr : rows) {
        mGpuScene.stageGeomRow(detail::GpuScene::geomRowIndex(index, sr.level, sr.submesh),
                               &sr.row);
    }
    // A LEVEL'S BASE IS SUBMESH 0's ROW, and only when submesh 0 really has one: the
    // contract is `geomRow + s`, so a level whose first submesh was refused has no
    // usable base and must stay kNoGeomRow rather than point into another level's block.
    for (uint32_t l = 0; l < levelCount && l < detail::GpuScene::kLevelsPerMesh; ++l) {
        if (levelHasBase[l])
            mGpuScene.setLevelGeomRow(index, l, detail::GpuScene::geomRowIndex(index, l, 0u));
    }

    // THE CLUSTER DAG AND ITS ROW (ATOM-CLUSTER-CUT). The row is level 0's with the
    // CLUSTER STREAM as its index buffer (the same vertices, ATOM rule 1); a mesh that
    // never passed the bake (a hand-made iris::Mesh: a suite's, a runtime primitive —
    // every baked mesh carries a DAG since ATOM-2) gets a FLAT one over level 0's own
    // indices (runs of kFlatClusterTriangles, every one a leaf of one terminal group),
    // so every Atom item is drawn by the one cut and nothing reads a level on the id pass.
    const StagedRow *base0 = nullptr;
    for (const StagedRow &sr : rows)
        if (sr.level == 0u && sr.submesh == 0u) base0 = &sr;
    Ogre::VaoManager *vaoMgr = mRoot ? mRoot->getRenderSystem()->getVaoManager() : nullptr;
    if (base0 && vaoMgr) {
        std::vector<detail::GpuCluster> clusters;
        std::vector<detail::GpuClusterGroup> groups;
        std::vector<detail::GpuClusterBox> boxes;   // ATOM-8 stage 2: the record's own boxes, packed per cluster
        Ogre::PhotonVoxelizer::GeometryRow crow = base0->row;
        const Ogre::IndexBufferPacked *stream = rec.clusterStream.get();
        const uint64_t rawIdx = stream ? vaoMgr->getBufferDeviceAddress(stream) : 0u;
        if (stream && rawIdx && !rec.clusters.empty() && !rec.clusterGroups.empty()) {
            const uint32_t idxBytes = uint32_t(stream->getBytesPerElement());
            const uint64_t floored = rawIdx & ~uint64_t(3u);
            crow.idxBias = uint32_t((rawIdx - floored) / idxBytes);
            crow.idxAddress[0] = uint32_t(floored & 0xFFFFFFFFu);
            crow.idxAddress[1] = uint32_t(floored >> 32u);
            // Bit 0 of the row's flags is "32-bit indices" (the row format's own bit,
            // PhotonVoxelizerGeomFlag::Index32bit in PhotonVoxelizer.cpp; the decode's
            // ATOM_GEOM_INDEX32): the stream's width, which is level 0's.
            constexpr uint32_t kRowIndex32 = 1u;
            crow.flags = (crow.flags & ~kRowIndex32) |
                         (stream->getIndexType() == Ogre::IndexBufferPacked::IT_32BIT ? kRowIndex32 : 0u);
            clusters.reserve(rec.clusters.size());
            for (const MeshCluster &c : rec.clusters) {
                detail::GpuCluster g;
                g.firstIndex = c.firstIndex;
                g.indexCount = c.indexCount;
                g.group = uint32_t(c.group);
                g.refined = c.refined < 0 ? detail::GpuScene::kNoGroup : uint32_t(c.refined);
                for (int k = 0; k < 3; ++k) g.sphere[k] = c.centre[k];
                g.sphere[3] = c.radius;
                clusters.push_back(g);
            }
            if (!rec.clusterBoxesFlat && rec.clusterBoxes.size() == rec.clusters.size() * 6u)
                for (size_t ci = 0; ci < rec.clusters.size(); ++ci)
                    boxes.push_back(detail::packClusterBox(&rec.clusterBoxes[ci * 6u], &rec.clusterBoxes[ci * 6u + 3u],
                                                           rec.clusters[ci].radius));
            groups.reserve(rec.clusterGroups.size());
            // THE CLUSTERS EACH GROUP PRODUCED (ATOM-7): the root cut's "last group" test (error.w = 0).
            const std::vector<unsigned> produced = clusterProducedCounts(rec.clusters, rec.clusterGroups.size());
            for (size_t gi = 0; gi < rec.clusterGroups.size(); ++gi) {
                const MeshClusterGroup &gr = rec.clusterGroups[gi];
                detail::GpuClusterGroup g;
                for (int k = 0; k < 3; ++k) g.sphere[k] = gr.centre[k];
                g.sphere[3] = gr.radius;
                g.error[0] = gr.error;
                g.error[1] = gr.estimate;
                g.error[2] = float(gr.depth);
                g.error[3] = float(produced[gi]);
                groups.push_back(g);
            }
        } else if (levels[0].indexCount >= 3u) {
            // THE FLAT DAG: the row IS level 0's; the clusters are its index range.
            const Ogre::Vector3 c = local.mCenter;
            const float r = float(local.mHalfSize.length());
            detail::GpuClusterGroup g;
            g.sphere[0] = c.x; g.sphere[1] = c.y; g.sphere[2] = c.z; g.sphere[3] = r;
            g.error[0] = std::numeric_limits<float>::max();
            g.error[1] = std::numeric_limits<float>::max();
            groups.push_back(g);
            const uint32_t run = detail::GpuScene::kFlatClusterTriangles * 3u;
            for (uint32_t first = 0; first + 3u <= levels[0].indexCount; first += run) {
                detail::GpuCluster k;
                k.firstIndex = levels[0].firstIndex + first;
                k.indexCount = std::min(run, (levels[0].indexCount - first) / 3u * 3u);
                k.group = 0u;
                k.refined = detail::GpuScene::kNoGroup;
                std::memcpy(k.sphere, g.sphere, sizeof(k.sphere));
                clusters.push_back(k);
            }
            // THE RUNS' OWN BOXES (ATOM-8 stage 2): the record's, when it measured these very runs (level 0 from its
            // first index); the sphere stays the mesh's (the level rule's).
            if (rec.clusterBoxesFlat && levels[0].firstIndex == 0u && rec.clusterBoxes.size() == clusters.size() * 6u)
                for (size_t ci = 0; ci < clusters.size(); ++ci)
                    boxes.push_back(detail::packClusterBox(&rec.clusterBoxes[ci * 6u], &rec.clusterBoxes[ci * 6u + 3u],
                                                           g.sphere[3]));
        }
        if (!clusters.empty()) {
            const uint32_t crowIndex = detail::GpuScene::clusterRowIndex(index);
            mGpuScene.stageGeomRow(crowIndex, &crow);
            mGpuScene.setMeshDag(index, std::move(clusters), std::move(groups), crowIndex, std::move(boxes));
        }
    }
    return index;
}

void OgreScene::releaseGpuMesh(const Ogre::Mesh *mesh) {
    if (mGpuScene.live() && mesh) mGpuScene.releaseMesh(mesh);
}

/// The tables, created the first time this scene needs them — which is the first
/// mesh ATTACH, not the first frame: an Item that arrives before a frame is drawn
/// must still get its mesh table entry, or its instance entry would name no mesh.
void OgreScene::ensureGpuTables() const {
    if (mGpuScene.live() || mSubstrate.refused || !mSceneMgr || mDestroying) return;
    Ogre::RenderSystem *rs = mRoot ? mRoot->getRenderSystem() : nullptr;
    Ogre::VaoManager *vao =
        rs && !(mEngine && mEngine->isHeadless()) ? rs->getVaoManager() : nullptr;
    std::string err;
    if (!mGpuScene.create(vao, err)) {
        mSubstrate.refused = true;
        Ogre::LogManager::getSingleton().logMessage("Jahshaka " + err);
    }
}

// ===========================================================================
// THE SUBSTRATE'S COMPOSE AND COMMIT (GpuSceneSubstrate.h has the protocol).
//
// THE DIRTY SCAN — the one CPU walk that remains, and the one the journal
// (U0's change set) replaces without changing `GpuScene::update`'s signature.
// IT IS EPOCH-GATED, so a still scene runs no walk and copies no bytes. When the
// epoch HAS moved, each slot is asked two cheap questions — did its 48 bytes of
// world transform change, and did its flags word change — and only a slot that
// answers yes pays for a world AABB. AND IT RUNS ONCE A FRAME: the frame's first
// graph-current call walks (a pre-frame GI build, or the frame's own read), later
// calls compose their marks only.
// ===========================================================================

namespace {
/// THE WALK AS OGRE'S UNIFORM SCALABLE TASK: share `threadId` on worker `threadId`.
/// The share's body is the scene's own (it reads the scene's private item index).
template <class Share>
class GpuSceneWalkTask final : public Ogre::UniformScalableTask {
public:
    explicit GpuSceneWalkTask(Share &share) : mShare(share) {}
    void execute(size_t threadId, size_t) override { mShare(threadId); }

private:
    Share &mShare;
};
/// Below this many slots the walk stays on the frame's thread: a barrier round trip
/// to the workers costs more than comparing a few hundred slots.
constexpr size_t kParallelWalkSlots = 512u;
}  // namespace

void OgreScene::walkGpuScene(bool graphIsCurrent) const {
    GpuSceneSubstrate &sub = mSubstrate;
    // THE TWO MEASURING DOORS of the notify share, latched from the arms (U4 stage 3, decision 11 d).
    mGpuScene.setNotifyTiming(mEngine && mEngine->armRegistry().on(ArmId::GpuWalkTiming));
    mGpuScene.setObserversMuted(mEngine && mEngine->armRegistry().on(ArmId::GpuObserversOff));
    using WalkClock = std::chrono::steady_clock;
    const auto usSince = [](WalkClock::time_point t) {
        return std::chrono::duration<double, std::micro>(WalkClock::now() - t).count();
    };
    const auto tName = WalkClock::now();
    // THE NAMED SLOTS (GpuSceneSubstrate's named walk): the slots of the nodes written since
    // the last consuming walk, once each, in slot order — or every slot where one is owed.
    const bool whole = sub.namedFull || (mEngine && mEngine->armRegistry().on(ArmId::GpuWalkWhole));
    std::vector<uint32_t> &named = sub.namedSlots;
    named.clear();
    if (!whole) {
        for (const Node *n : mNamed)   // the one name list's records the GPU walk owes (U4 decision 12.1 ii)
            if ((n->namedMask & kNameGpu) && n->item && n->itemSlot != size_t(-1)) named.push_back(uint32_t(n->itemSlot));
        std::sort(named.begin(), named.end());
        named.erase(std::unique(named.begin(), named.end()), named.end());
    }
    const size_t n = whole ? mItemNodes.size() : named.size();
    sub.lastNameMicros = usSince(tName);
    const auto tCompare = WalkClock::now();
    if (whole) ++sub.wholeWalks;
    else sub.slotsNamed += named.size();
    // THE PREDICATES' SCENE-WIDE INPUT, once per walk instead of once per slot (the
    // split's switch asks the device for an extension by name).
    const bool atomDraw = sub.atomDraw >= 0 ? sub.atomDraw != 0 : atomDrawOn();
    const size_t workers = mSceneMgr ? mSceneMgr->getNumWorkerThreads() : 0u;
    // ONLY THE CACHED READ IS PARALLEL: `_getFullTransformUpdated` (a reader before the
    // graph: the warm-up) WRITES every ancestor's derived transform on its way up, and
    // two shares sharing an ancestor would race on it — that form walks on one thread.
    const size_t shares =
        (graphIsCurrent && n >= kParallelWalkSlots && workers >= 1u) ? workers + 1u : 1u;
    if (sub.shareDirty.size() != shares) sub.shareDirty.assign(shares, std::vector<uint32_t>());
    if (sub.shareFlags.size() != shares) sub.shareFlags.assign(shares, 0ull);
    if (sub.shareComposed.size() != shares) sub.shareComposed.assign(shares, std::vector<GpuSceneSubstrate::Composed>());
    // THE COMPOSE RUNS INSIDE THE COMPARE'S TASK (ENGINE-UPGRADE-U4 stage 4): share k composes and stages the rows of
    // its own dirty slots the moment its compare ends (one barrier, not two) — the lanes READ Ogre objects only and
    // write only their own slots' rows (GpuScene::stageRow; the table pre-sized by setSlotCount); what is not
    // lane-local (the Atom route's render-queue edit, the material store's queue, the write count and the change
    // feed's observers) is collected per share and REPLAYED after the barrier in the serial compose's order (share
    // by share, ascending slot within a share). The arm gpuscene.serialCompose composes after the barrier instead,
    // on the frame's thread (the identity row's other arm).
    const bool serialCompose = mEngine && mEngine->armRegistry().on(ArmId::GpuSerialCompose);
    const bool digest = mEngine && mEngine->armRegistry().on(ArmId::GpuComposeDigest);
    // THE THROW DOOR (decisions 18.3, 22.4; gpuscene.throwInLane): 1-16 = share N-1 throws at its first composed slot
    // BEFORE the stage, once per walk; 17-32 = share N-17 throws AFTER its first stage (the row staged, its tail and
    // lane halves not collected), once per walk; 33-48 = share N-33 throws EVERY time it composes — in its lane and
    // again in the frame thread's recompose. The failed share is redone on the frame's thread after the barrier.
    const int throwDoor = mEngine ? int(mEngine->armRegistry().value(ArmId::GpuThrowInLane)) : 0;
    const int throwShare = throwDoor <= 0 ? -1 : (throwDoor - 1) % 16;
    const int throwMode = throwDoor <= 0 ? -1 : (throwDoor - 1) / 16;   // 0 before the stage, 1 after it, 2 always
    std::atomic<bool> thrown{ false };
    sub.walkShares = unsigned(shares);
    const detail::GpuInstance *mirror = mGpuScene.mirrorData();
    if (sub.shareComposed.size() != shares) sub.shareComposed.assign(shares, std::vector<GpuSceneSubstrate::Composed>());
    if (sub.shareError.size() != shares) sub.shareError.assign(shares, std::string());
    for (std::string &err : sub.shareError) err.clear();
    sub.shareInflight.assign(shares, GpuSceneSubstrate::kNoSlot);
    if (sub.shareLaneStaged.size() != shares) sub.shareLaneStaged.assign(shares, std::vector<uint32_t>());
    // Every share's collection starts empty HERE, not in its compose: a share whose COMPARE threw never reached its
    // compose, and its redo must not read the last walk's collection as this one's staged slots.
    for (std::vector<GpuSceneSubstrate::Composed> &done : sub.shareComposed) done.clear();
    // THE DIGEST (the identity row): the tails, the staged rows and the notifies, folded in the order they happen.
    unsigned long long h = 1469598103934665603ull;
    mGpuScene.setDigest(digest ? &h : nullptr);
    struct DigestEnds { detail::GpuScene &g; ~DigestEnds() { g.setDigest(nullptr); } } digestEnds{ mGpuScene };
    // The observers' lane halves write per-slot storage inside the task: sized before it (decision 17.1).
    if (!serialCompose) mGpuScene.reserveObserverSlots(uint32_t(mItemNodes.size()));
    // THE COMPOSE INSIDE THE COMPARE'S TASK (ENGINE-UPGRADE-U4 stage 4): share k composes and stages the rows of its
    // own dirty slots the moment its compare ends (one barrier) — the lanes READ Ogre objects only and write only
    // their own slots' rows (GpuScene::stageRow; the table pre-sized by setSlotCount) and the observers' lane halves
    // (decision 17.1); what is not lane-local (the Atom route's render-queue edit, the material store's queue, the
    // write count, the observers' ordered halves) is collected per share and REPLAYED after the barrier in the
    // serial compose's order (share by share, ascending slot within a share). The arm gpuscene.serialCompose runs
    // the OLD compose instead: after the barrier, per slot compose -> tail -> stage -> every observer, whole.
    // ONE SHARE'S COMPOSE: its dirty slots, in its slot order — the row staged, the lane halves told, the tail
    // collected. `redo` (decision 22.4 a): a failed share is redone FROM ITS START on the frame's thread, never from
    // where it stopped — what its lane collected is dropped and collected again in order, and a slot its lane
    // STAGED (collected or not: a throw may fall between the stage and the collection) keeps its row — restaging it
    // would read the new world as the previous one — and is composed and told again. A slot the marks staged before
    // the walk is skipped, as in the lane.
    const auto composeShare = [&](size_t k, bool redo) {
        std::vector<GpuSceneSubstrate::Composed> &done = sub.shareComposed[k];
        std::vector<uint32_t> &mine = sub.shareLaneStaged[k];
        mine.clear();
        if (redo) {
            for (const GpuSceneSubstrate::Composed &c : done) mine.push_back(c.slot);
            if (sub.shareInflight[k] != GpuSceneSubstrate::kNoSlot) mine.push_back(sub.shareInflight[k]);
            std::sort(mine.begin(), mine.end());
            // ...and they join the redone compare's list: their rows are staged, so the compare (run again, whole —
            // the lane's may have thrown part-way) reads them as equal to the mirror and would leave them out.
            std::vector<uint32_t> &dirty = sub.shareDirty[k];
            std::vector<uint32_t> merged;
            merged.reserve(dirty.size() + mine.size());
            std::set_union(dirty.begin(), dirty.end(), mine.begin(), mine.end(), std::back_inserter(merged));
            dirty.swap(merged);
        }
        done.clear();
        sub.shareInflight[k] = GpuSceneSubstrate::kNoSlot;
        detail::GpuInstance cand;
        ComposeTail tail;
        for (uint32_t i : sub.shareDirty[k]) {
            const bool laneStaged = redo && std::binary_search(mine.begin(), mine.end(), i);
            if (!laneStaged && mGpuScene.stagedThisUpdate(i)) continue;   // the marks staged it before the walk
            const bool throwHere = int(k) == throwShare && (throwMode == 2 || !thrown.exchange(true));
            if (throwHere && throwMode != 1) throw std::runtime_error("gpuscene.throwInLane");
            const Node &node = *mItemNodes[i];
            Ogre::Node *pn = node.item->getParentNode();
            const Ogre::Matrix4 &m = graphIsCurrent ? pn->_getFullTransform() : pn->_getFullTransformUpdated();
            composeGpuInstanceLane(node, m, graphIsCurrent, cand, tail);
            if (!laneStaged && !mGpuScene.stageRow(i, cand, node.selfId)) continue;
            sub.shareInflight[k] = i;   // staged; its tail and its lane halves not yet collected
            if (throwHere) throw std::runtime_error("gpuscene.throwInLane (after the stage)");
            done.push_back({ i, tail.atom, tail.queueMaterial, mGpuScene.notifyLanes(i), tail.casterInCut });
            sub.shareInflight[k] = GpuSceneSubstrate::kNoSlot;
        }
    };
    // THE COMPARE, one contiguous share of the slots per thread: the slots whose world
    // transform or flags word differ from the mirror's. Read-only on everything it
    // touches (the items, their nodes' cached derived transforms, their datablocks, the
    // mirror, the stamps), so the SceneManager's workers and the frame's thread run it
    // side by side.
    const auto compareShare = [&](size_t k) {
        const size_t first = n * k / shares, last = n * (k + 1) / shares;
        std::vector<uint32_t> &dirty = sub.shareDirty[k];
        dirty.clear();
        unsigned long long &flags = sub.shareFlags[k];
        flags = 0ull;
        for (size_t j = first; j < last; ++j) {
            const size_t i = whole ? j : named[j];
            const Node &node = *mItemNodes[i];
            Ogre::Item *item = node.item;
            if (!item) continue;
            Ogre::Node *pn = item->getParentNode();
            if (!pn) continue;
            const detail::GpuInstance &was = mirror[i];
            // `_getFullTransform` is a cached read and `_getFullTransformUpdated`
            // RECOMPUTES the derived transform up the parent chain unconditionally
            // (Ogre-Next keeps no per-node dirty bit): the frame path, which runs AFTER
            // `updateSceneGraph`, reads the cache (0.7 ms against 2.3-2.5 ms at 8,001
            // items in Debug, measured).
            const Ogre::Matrix4 &m = graphIsCurrent ? pn->_getFullTransform() : pn->_getFullTransformUpdated();
            bool d = std::memcmp(&m[0][0], was.world, sizeof(was.world)) != 0;
            if (!d) {
                // A NAMED SLOT WHOSE TRANSFORM HELD: its flags word, asked once (a named
                // write can change what the word says — a reparent under a hidden node).
                // Every other flags change marks its slot by name (markGpuSlotDirty).
                Ogre::uint32 wasFlags;
                std::memcpy(&wasFlags, &was.boundsMax[3], sizeof(wasFlags));
                d = gpuFlagsFor(node, atomDraw) != wasFlags;
                ++flags;
            }
            // A slot the marks already staged is skipped at the compose (the stamp).
            if (d) dirty.push_back(uint32_t(i));
        }
    };
    // A SHARE THAT THROWS (decision 18.3) records its error and stops; nothing leaves the lane — a worker's throw
    // would terminate the process, the frame thread's would skip the barrier.
    auto share = [&](size_t k) {
        try {
            compareShare(k);
            if (!serialCompose) composeShare(k, false);
        } catch (const std::exception &ex) {
            sub.shareError[k] = ex.what();
        } catch (...) {
            sub.shareError[k] = "unknown";
        }
    };
    if (shares == 1u) {
        share(0u);
    } else {
        // THE WORKERS TAKE SHARES 0..workers-1, THE FRAME'S THREAD THE LAST: fired
        // without blocking, the frame's thread runs its own share, then waits — the wait by scope, whatever happens
        // on the frame's thread between (decision 18.3: every barrier pairs).
        GpuSceneWalkTask<decltype(share)> task(share);
        struct Barrier {
            Ogre::SceneManager *sm = nullptr;
            ~Barrier() { sm->waitForPendingUserScalableTask(); }
        } barrier{ mSceneMgr };
        mSceneMgr->executeUserScalableTask(&task, false);
        share(shares - 1u);
    }
    // A FAILED SHARE, REDONE ON THE FRAME'S THREAD (decisions 18.3, 22.4): its compare again, whole, and its compose
    // from its start (composeShare's redo) — the frame's result is the serial one. UNDER ITS OWN TRY (22.4 b): a
    // share that throws AGAIN gives up for this frame — the frame says so, what it collected and the slot it staged
    // are replayed (that slot's observers told whole), its other slots are MARKED for the next compose, and every
    // other share's replay still runs.
    std::string failures;
    std::vector<size_t> gaveUp;
    for (size_t k = 0; k < shares; ++k) {
        if (sub.shareError[k].empty()) continue;
        if (!failures.empty()) failures += "; ";
        failures += "share " + std::to_string(k) + ": " + sub.shareError[k];
        ++sub.shareRecomposes;
        try {
            compareShare(k);
            if (!serialCompose) composeShare(k, true);
        } catch (const std::exception &ex) {
            failures += std::string(" — and again on the frame's thread: ") + ex.what();
            gaveUp.push_back(k);
        } catch (...) {
            failures += " — and again on the frame's thread";
            gaveUp.push_back(k);
        }
    }
    // ...what a share that gave up STAGED is still replayed, in its slot order: every row staged this frame reaches
    // the device and its observers, or the next compare would find the mirror already equal and never upload it.
    // The slots its redo collected keep their lane answers; the rest (the lane's, the one staged at the throw) are
    // told whole (kLanesNeverRan), their tails composed again here.
    for (size_t k : gaveUp) {
        ++sub.shareGiveUps;
        std::vector<GpuSceneSubstrate::Composed> &done = sub.shareComposed[k];
        std::vector<uint32_t> staged = sub.shareLaneStaged[k];
        if (sub.shareInflight[k] != GpuSceneSubstrate::kNoSlot) staged.push_back(sub.shareInflight[k]);
        for (const GpuSceneSubstrate::Composed &c : done) staged.push_back(c.slot);
        std::sort(staged.begin(), staged.end());
        staged.erase(std::unique(staged.begin(), staged.end()), staged.end());
        std::vector<GpuSceneSubstrate::Composed> replay;
        replay.reserve(staged.size());
        size_t d = 0;   // `done` is in ascending slot order (the share's)
        for (uint32_t i : staged) {
            while (d < done.size() && done[d].slot < i) ++d;
            if (d < done.size() && done[d].slot == i) { replay.push_back(done[d]); continue; }
            ComposeTail tail;
            detail::GpuInstance cand;
            const Node &node = *mItemNodes[i];
            Ogre::Node *pn = node.item->getParentNode();
            try {   // the composer is what threw: a slot whose tail cannot be had is replayed without one
                composeGpuInstanceLane(node, graphIsCurrent ? pn->_getFullTransform() : pn->_getFullTransformUpdated(),
                                       graphIsCurrent, cand, tail);
            } catch (...) {
                tail = ComposeTail();
            }
            replay.push_back({ i, tail.atom, tail.queueMaterial, detail::GpuScene::kLanesNeverRan, tail.casterInCut });
        }
        done.swap(replay);
        sub.shareInflight[k] = GpuSceneSubstrate::kNoSlot;
    }
    if (!failures.empty())
        mError = "the GPU scene's compose: a worker share threw (" + failures + ")" +
                 (gaveUp.empty() ? " — recomposed on the frame's thread" : " — its slots owed to the next frame");
    sub.lastCompareMicros = usSince(tCompare);
    sub.lastWalkSlots = unsigned(n);
    const auto tCompose = WalkClock::now();
    const double notify0 = mGpuScene.notifyMicros();
    const size_t dirtyBefore = sub.dirty.size();
    sub.slotsCompared += n;
    for (unsigned long long f : sub.shareFlags) sub.flagsRecomputed += f;
    // A walk on the graph's current transforms consumes the epoch (composeGpuScene), and with
    // it the names; a reader's walk before the graph (the warm-up) leaves them for the frame's.
    if (graphIsCurrent) { releaseNamed(kNameGpu); sub.namedFull = false; }
    // A SHARE THAT GAVE UP (decision 22.4 b): the slots it did not stage, MARKED for the next compose (the explicit
    // marks, markGpuSlotDirty's list — staged whatever the next frame moves: a name alone waits for a walk, and a
    // frame nothing moved in runs none).
    for (size_t k : gaveUp)
        for (uint32_t i : sub.shareDirty[k])
            if (!mGpuScene.stagedThisUpdate(i)) sub.marks.push_back(i);
    const auto tReplay = WalkClock::now();
    sub.lastTailAtoms = sub.lastTailQueues = 0u;
    if (serialCompose) {
        // THE OLD COMPOSE, IN FULL (the identity row's other arm, decision 18.4): per slot in the walk's order, the
        // whole composer (its tail applied), the stage, every observer told whole.
        detail::GpuInstance cand;
        for (size_t k = 0; k < shares; ++k)
            for (uint32_t i : sub.shareDirty[k]) {
                if (mGpuScene.stagedThisUpdate(i)) continue;
                const Node &node = *mItemNodes[i];
                Ogre::Node *pn = node.item->getParentNode();
                const Ogre::Matrix4 &m = graphIsCurrent ? pn->_getFullTransform() : pn->_getFullTransformUpdated();
                composeGpuInstance(node, m, graphIsCurrent, cand);
                mGpuScene.stage(i, cand, node.selfId);
                sub.dirty.push_back(i);
                ++sub.aabbReads;
            }
    } else {
        // THE REPLAY, IN THE SERIAL COMPOSE'S ORDER: each composed slot's tail (the Atom route's render-queue edit,
        // the material store's queue), the write count and the observers' ordered halves, then the dirty list.
        for (const std::vector<GpuSceneSubstrate::Composed> &done : sub.shareComposed) {
            for (const GpuSceneSubstrate::Composed &c : done) {
                const Node &node = *mItemNodes[c.slot];
                applyComposeTail(node, ComposeTail{ c.atom, c.queueMaterial, c.casterInCut });
                mGpuScene.noteStagedSplit(c.slot, c.laneOwed);
                sub.dirty.push_back(c.slot);
                ++sub.aabbReads;
            }
        }
    }
    sub.lastReplayMicros = usSince(tReplay);
    // ...AND EVERY OBSERVER'S STATE AS THE REPLAY LEFT IT (decision 22.6; RAY-FEED-OBSERVER-1): the card feed's table
    // (its columns in key order — the map's own order is not the data —, each slot's index in its column, filing and
    // centre) and the ray feed's traced set (its records, keys, pending key events, sets, words and rows) — the lane
    // halves and the ordered halves must have built the same state as the serial compose's whole notifies.
    if (digest) mGpuScene.digestObservers();
    if (digest) sub.lastComposeDigest = h;
    sub.lastComposeMicros = usSince(tCompose);
    sub.lastNotifyMicros = mGpuScene.notifyMicros() - notify0;
    sub.lastComposedSlots = unsigned(sub.dirty.size() - dirtyBefore);
    ++sub.walks;
}

void OgreScene::composeGpuScene(bool graphIsCurrent) const {
    if (!mSceneMgr || mDestroying) return;
    ensureGpuTables();
    if (!mGpuScene.live()) return;
    GpuSceneSubstrate &sub = mSubstrate;
    sub.beginCall(mRoot->getRenderSystem()->getVaoManager()->getFrameCount());
    const unsigned long long epoch = shadowEpoch();
    // THE WALK'S RULE. A reader BEFORE the scene graph (the warm-up) walks on Ogre's
    // recomputed transforms whenever the epoch moved, and consumes nothing. A
    // graph-current call is the frame's walk if the frame has not walked; after it,
    // only a transform written INSIDE the frame (the scene's own epoch moving past
    // the walk's) earns a second one.
    const bool moved = !sub.epochValid || epoch != sub.epoch;
    bool walk;
    if (!graphIsCurrent) walk = moved;
    else if (!sub.walkedThisFrame) walk = moved;
    else walk = epoch != sub.frameWalkEpoch;
    if (!walk && sub.marks.empty()) {
        if (graphIsCurrent) checkWalkParity(atomDrawOn());
        return;
    }
    // THE EPOCH IS ONLY CONSUMED BY A WALK THAT COULD SEE THE TRUTH. A reader BEFORE
    // the frame runs on derived transforms `updateSceneGraph` has not recomputed; if
    // it marked the epoch spent, the frame's own walk would skip — and every CHILD of
    // a moved parent would silently never reach the table (measured: a parent with
    // 8,000 children staged nothing at all). Once a frame's walk has set it the epoch is
    // trusted: every write reaches it named (ENGINE-UPGRADE-1 U0 3b — the change set and the
    // engine's own seams), so there is no host counter whose absence could make it blind.
    if (walk && graphIsCurrent) {
        if (sub.walkedThisFrame) ++sub.extraWalks;
        sub.walkedThisFrame = true;
        sub.frameWalkEpoch = epoch;
        sub.epoch = epoch;
        sub.epochValid = true;
    }
    sub.commitOwed = true;
    // THE SPLIT'S SWITCH, ONCE PER COMPOSE (an extension lookup by name): every slot this
    // compose composes, and the walk's compare, read it from here.
    struct AtomDrawScope {
        GpuSceneSubstrate &sub;
        ~AtomDrawScope() { sub.atomDraw = -1; }
    } atomDrawScope{ sub };
    sub.atomDraw = atomDrawOn() ? 1 : 0;
    mGpuScene.setSlotCount(uint32_t(mItemNodes.size()));
    // THE EXPLICIT MARKS FIRST: a seam that changed a flags word without moving
    // anything, and every newborn slot. A slot marked TWICE (seven seams, two of
    // which fire on one attach) is staged once — `stage` drops the second — and
    // must therefore not enter the dirty list twice either.
    detail::GpuInstance cand;
    for (uint32_t slot : sub.marks) {
        if (slot >= mItemNodes.size()) continue;
        if (mGpuScene.stagedThisUpdate(slot)) continue;
        const Node &n = *mItemNodes[slot];
        Ogre::Node *pn = n.item ? n.item->getParentNode() : nullptr;
        if (!pn) continue;
        const Ogre::Matrix4 &m =
            graphIsCurrent ? pn->_getFullTransform() : pn->_getFullTransformUpdated();
        composeGpuInstance(n, m, graphIsCurrent, cand);
        mGpuScene.stage(slot, cand, n.selfId);
        sub.dirty.push_back(slot);
    }
    sub.marks.clear();
    if (walk) walkGpuScene(graphIsCurrent);
    ++sub.composes;
    if (graphIsCurrent) checkWalkParity(sub.atomDraw != 0);
}

// THE NAMED WALK'S GUARD (the arm gpuscene.walkParity; engine.gpu_walk_parity): after a frame's
// compose — or on a frame that composed nothing — every slot against the table. A slot whose
// world transform or flags word the table does not hold moved with neither a name nor a mark:
// the whole walk would have staged it, the named walk did not.
void OgreScene::checkWalkParity(bool atomDraw) const {
    GpuSceneSubstrate &sub = mSubstrate;
    // THE GUARD (the arm gpuscene.walkParity): every slot against the table, after the marks and
    // the named walk — what a whole walk would stage now is a miss.
    if (!mEngine || !mEngine->armRegistry().on(ArmId::GpuWalkParity)) return;
    {
        const detail::GpuInstance *mirror = mGpuScene.mirrorData();
        for (size_t i = 0; i < mItemNodes.size(); ++i) {
            const Node &node = *mItemNodes[i];
            Ogre::Item *item = node.item;
            Ogre::Node *pn = item ? item->getParentNode() : nullptr;
            if (!pn) continue;
            ++sub.walkChecked;
            if (i >= mGpuScene.slotCount()) { ++sub.walkMissed; sub.walkLastMissed = uint32_t(i); continue; }
            const detail::GpuInstance &was = mirror[i];
            const Ogre::Matrix4 &m = pn->_getFullTransform();
            Ogre::uint32 wasFlags;
            std::memcpy(&wasFlags, &was.boundsMax[3], sizeof(wasFlags));
            if (std::memcmp(&m[0][0], was.world, sizeof(was.world)) != 0 || gpuFlagsFor(node, atomDraw) != wasFlags) {
                ++sub.walkMissed;
                sub.walkLastMissed = uint32_t(i);
            }
        }
    }
}

void OgreScene::commitGpuScene() const {
    if (!mGpuScene.live()) return;
    GpuSceneSubstrate &sub = mSubstrate;
    // NOTHING COMPOSED — but last frame's movers may still owe their close, and that
    // is the one thing a still frame is not free for (once).
    if (!sub.commitOwed && !mGpuScene.hasOpenMovers() && !mGpuScene.hasPendingCopies()) return;
    sub.commitOwed = false;
    const unsigned long long copied = mGpuScene.frames();
    mGpuScene.update(sub.dirty);
    sub.dirty.clear();
    if (mGpuScene.frames() != copied) ++sub.commits;
}

/// A READER'S CALL (a GI build's gather before the frame, the warm-up): the compose,
/// and the commit at once — it dispatches before the frame's own commit.
void OgreScene::ensureGpuScene(bool graphIsCurrent) const {
    if (!mSceneMgr || mDestroying) return;
    const auto t0 = std::chrono::steady_clock::now();
    const unsigned long long walks = mSubstrate.walks;
    composeGpuScene(graphIsCurrent);
    if (!mSubstrate.commitOwed && !(mGpuScene.live() && (mGpuScene.hasOpenMovers() || mGpuScene.hasPendingCopies())))
        return;
    commitGpuScene();
    noteGpuSceneStage(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count(),
                      mSubstrate.walks != walks);
}

/// ALWAYS TIMED, and reported as its own MONITOR STAGE (`engine.gpuscene`): real CPU
/// inside the frame that belongs to no compositor pass (the stages are exclusive and
/// must account for the frame within 5 %). A call that walked keeps its cost as the
/// last scan's (GpuSceneStatus::lastScanMicros).
void OgreScene::noteGpuSceneStage(double us, bool walked) const {
    if (walked) mSubstrate.scanMicros = us;
    if (monitor::live()) monitor::gMonitor->stage("engine.gpuscene", us / 1000.0);
}

/// THE FRAME'S OWN READ (OgreShadow.cpp): the compose here; the ray levels patch the
/// mirror; then the frame's ONE commit. Timed as one stage, without the ray levels.
void OgreScene::composeGpuSceneFrame() const {
    const auto t0 = std::chrono::steady_clock::now();
    mSubstrate.frameComposeWalks = mSubstrate.walks;
    composeGpuScene(/*graphIsCurrent=*/true);
    mSubstrate.frameComposeMicros =
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
}

void OgreScene::commitGpuSceneFrame() const {
    const bool walked = mSubstrate.walks != mSubstrate.frameComposeWalks;
    if (!mSubstrate.commitOwed && !walked &&
        !(mGpuScene.live() && (mGpuScene.hasOpenMovers() || mGpuScene.hasPendingCopies())))
        return;
    const auto t0 = std::chrono::steady_clock::now();
    commitGpuScene();
    noteGpuSceneStage(mSubstrate.frameComposeMicros +
                          std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count(),
                      walked);
}

// ---------------------------------------------------------------------------
// THE READ-ONLY GPU-SCENE VIEW (EnginePrivate.h): a slot's or a node's item facts.
inspect::SceneEcho::CardWalk OgreScene::cardWalk() const {
    CardWalk w;
    w.micros = mPhoton.mCardWalkMicros;
    w.slotCount = mPhoton.mCardWalkSlots;
    w.walks = mPhoton.mCardWalks;
    return w;
}

inspect::SceneEcho::GpuWalk OgreScene::gpuWalk() const {
    GpuWalk w;
    w.nameMicros = mSubstrate.lastNameMicros;
    w.compareMicros = mSubstrate.lastCompareMicros;
    w.composeMicros = mSubstrate.lastComposeMicros;
    w.notifyMicros = mSubstrate.lastNotifyMicros;
    w.slotsCompared = mSubstrate.lastWalkSlots;
    w.slotsComposed = mSubstrate.lastComposedSlots;
    w.shares = mSubstrate.walkShares;
    w.walks = mSubstrate.walks;
    w.ogreIdReach = mNodeByOgreId.reach();
    w.ogreIdPages = mNodeByOgreId.allocated();
    w.nodeIdReach = mNodeById.reach();
    w.nodeIdPages = mNodeById.allocated();
    w.replayMicros = mSubstrate.lastReplayMicros;
    w.shareRecomposes = mSubstrate.shareRecomposes;
    w.shareGiveUps = mSubstrate.shareGiveUps;
    w.tailAtoms = mSubstrate.lastTailAtoms;
    w.tailQueues = mSubstrate.lastTailQueues;
    {
        const std::vector<double> &om = mGpuScene.observerMicros();
        for (size_t k = 0; k < om.size(); ++k) (mGpuScene.observerHasLaneHalf(k) ? w.observerLaneMicros : w.observerOtherMicros) += om[k];
        for (size_t k = 0; k < mGpuScene.observerCount(); ++k)
            w.observers.push_back({ mGpuScene.observerName(k), mGpuScene.observerHasLaneHalf(k), k < om.size() ? om[k] : 0.0,
                                    mGpuScene.observerTold(k), mGpuScene.observerOwed(k) });
    }
    w.composeDigest = mSubstrate.lastComposeDigest;
    namedListGuard(w.namedDangling, w.namedNotFlagged, w.namedDuplicates);
    w.namedListed = unsigned(mNamed.size());
    w.namedLive = unsigned(mNamedLive);
    return w;
}

inspect::SceneEcho::GiPlan OgreScene::giPlan() const {
    GiPlan g;
    const PhotonComponent::PlanLog &l = mPhoton.planLog();
    g.micros = l.micros;
    g.boxes = l.boxes;
    g.pending = l.marked;
    g.rebuild = l.rebuild;
    g.plans = l.plans;
    g.sigFullPasses = mPhoton.geomSigFullPasses();
    g.sigChecks = mPhoton.geomSigChecks();
    g.sigMisses = mPhoton.geomSigMisses();
    g.rootChecks = l.rootChecks;
    g.rootMisses = l.rootMisses;
    g.rootExtras = l.rootExtras;
    g.signatureMicros = l.signatureMicros;
    g.boxesMicros = l.boxesMicros;
    g.pickMicros = l.pickMicros;
    g.pccStages = l.pccStages;
    g.pccStagesLit = l.pccStagesLit;
    g.pccLightChanges = l.pccLightChanges;
    g.pccLightsBefore = l.pccLightsBefore;
    g.pccLightsAfter = l.pccLightsAfter;
    g.pccProbes = l.pccProbes;
    g.pccProbeOffsetMax = l.pccProbeOffsetMax;
    g.pccCollections = l.pccCollections;
    g.pccCollectLights = l.pccCollectLights;
    return g;
}

inspect::SceneEcho::TraceSun OgreScene::traceSun() const {
    TraceSun t;
    if (const SurfaceCache *c = mPhoton.surfaceCache()) {
        const SurfaceCache::TraceSunCost &k = c->traceSunCost();
        t.micros = k.micros;
        t.casterMoves = k.casterMoves;
        t.cards = k.cards;
        t.frame = k.frame;
        t.calls = k.calls;
        t.casterHits = k.casterHits;
        t.casterHitDigest = k.casterHitDigest;
        t.parityChecks = k.parityChecks;
        t.parityMisses = k.parityMisses;
        t.hookMicros = k.hookMicros;
        t.stepOneMicros = k.stepOneMicros;
        t.step3Micros = k.step3Micros;
        t.step3bMicros = k.step3bMicros;
        t.stillMicros = k.stillMicros;
        t.step2Micros = k.step2Micros;
        t.step4Micros = k.step4Micros;
        t.traceJobMicros = k.traceJobMicros;
        t.wideCards = k.wideCards;
    }
    return t;
}

NodeId OgreScene::nodeOfGpuKey(uint32_t key) const {
    const Node *n = mNodeByOgreId.get(Ogre::IdType(key));
    return n ? n->selfId : 0;
}

bool OgreScene::gpuNodeView(uint32_t node, GpuSlotView &out) const {
    out = GpuSlotView();
    auto it = mNodes.find(nodeOfGpuKey(node));   // `node` is the GPU key (gpuKeyOf)
    if (it == mNodes.end()) return false;
    const Node &n = it->second;
    out.node = gpuKeyOf(n);
    out.item = n.item;
    out.rigGeneration = n.rigGeneration;
    out.poseSerial = n.poseEpoch;
    if (n.shareSource) {
        auto sit = mNodes.find(n.shareSource);
        if (sit != mNodes.end()) out.poseSerial = out.poseSerial * 1000003ull + sit->second.poseEpoch;
    }
    return true;
}

bool OgreScene::gpuSlotView(uint32_t slot, GpuSlotView &out) const {
    out = GpuSlotView();
    if (slot >= mItemNodes.size() || !mItemNodes[slot]) return false;
    return gpuNodeView(gpuKeyOf(*mItemNodes[slot]), out);
}

void OgreScene::markGpuNodeDirty(uint32_t node) {
    auto it = mNodes.find(nodeOfGpuKey(node));   // the GPU key
    if (it != mNodes.end()) markGpuSlotDirty(it->second);
}

unsigned long long OgreScene::rayStructureEpoch() const { return shadowEpoch() + rayLevelRefits(); }

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
// fork 5230c9390+8282f6d70 (was 0075)'s band; this is the RAY tier's, evaluated at build time, and the
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
    // ...kept for the surface cache's still trace, whose lift is sized by it.
    mRayFootprintPerMetre = footprintPerMetre;
    const uint32_t n = mGpuScene.slotCount();
    if (!n) return;
    // A STILL CAMERA IN A STILL SCENE RUNS NOTHING. The eye is compared
    // exactly: it is the same float3 the view wrote, not a derived quantity, so
    // "did not move" is a memcmp and not a tolerance.
    const bool eyeMoved = mRayEyeValid == false || eye != mRayEye;
    const bool tableMoved = mSubstrate.composes != mRayLevelScanSeen;
    if (!eyeMoved && !tableMoved) return;
    mRayEye = eye;
    mRayEyeValid = true;
    mRayLevelScanSeen = mSubstrate.composes;
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
        // world radius is the local one times the largest axis scale - the longest
        // COLUMN of the row-major 3x4 (worldMaxAxisScale says why not a row).
        const Ogre::Vector3 centre(0.5f * (e.boundsMin[0] + e.boundsMax[0]),
                                   0.5f * (e.boundsMin[1] + e.boundsMax[1]),
                                   0.5f * (e.boundsMin[2] + e.boundsMax[2]));
        const float scale = worldMaxAxisScale(e.world);
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
        // THE TABLE IS PATCHED IN PLACE (ATOM-CPU-WALKS-1): ids[3] alone, prevWorld
        // untouched, the feed told, the entry queued. This pass runs AFTER the
        // frame's scan, so a re-stage (what this used to do) reached the device a
        // frame late — the ray tier, which now writes its instances on the device
        // from this table, would have built from the previous level.
        mGpuScene.patchRayLevel(i, want);
    }
    // ...and on the device before the ray tier reads it, this frame: with the frame's
    // own commit, which runs next (OgreShadow.cpp, commitGpuSceneFrame).
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
    out.pbsMaterialWord = in.raster[0];
    out.tangentOffset = in.raster[1];
    out.skinRow = in.raster[2];
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
    st.feedNotifies = mGpuScene.notifies();
    return st;
}

bool OgreScene::gpuSceneEntry(unsigned slot, GpuSceneEntry &out) const {
    JAH_SYNC_GETTER("Scene::gpuSceneEntry");
    if (!mGpuScene.live() || slot >= mGpuScene.slotCapacity()) return false;
    toPublic(mGpuScene.entry(slot), out);
    out.nodeId = nodeOfGpuKey(mGpuScene.entry(slot).ids[0]);   // the key back to the node's id
    return true;
}

/// THE DEVICE'S OWN BYTES. `readRequest` records the download and the ticket's
/// map() waits on Ogre's fence for it — but this scene's copies were RECORDED
/// into the frame's command buffer and not submitted, so the flush comes first
/// for exactly the reason the manual-upload trap names (DOCS/traps/ENGINE.md:
/// "AsyncTextureTicket after a manual staging upload reads STALE VRAM").
bool OgreScene::gpuSceneDeviceEntries(unsigned first, unsigned count,
                                      std::vector<GpuSceneEntry> &out) {
    // U3 stage 4a: a read on the host's side is a REQUEST, answered on the engine's side.
    if (hostPosts()) { bool r = false; requestOnEngine([&] { r = gpuSceneDeviceEntries(first, count, out); }); return r; }
    JAH_HOST_CALL("Scene::gpuSceneDeviceEntries");
    out.clear();
    if (!mGpuScene.live() || !count || first + count > mGpuScene.slotCapacity()) return false;
    Ogre::RenderSystem *rs = mRoot ? mRoot->getRenderSystem() : nullptr;
    if (rs) rs->flushCommands();
    Ogre::AsyncTicketPtr ticket = mGpuScene.instanceBuffer()->readRequest(first, count);
    const detail::GpuInstance *raw = static_cast<const detail::GpuInstance *>(ticket->map());
    out.resize(count);
    for (unsigned i = 0; i < count; ++i) {
        toPublic(raw[i], out[i]);
        out[i].nodeId = nodeOfGpuKey(raw[i].ids[0]);
    }
    ticket->unmap();
    return true;
}

}  // namespace detail

/// THE SUBSTRATE'S STATUS (GpuSubstrate.h): counters, read where they live.
GpuSubstrateStatus gpuSubstrateStatus(const Scene &scene) {
    GpuSubstrateStatus st;
    const auto *s = dynamic_cast<const detail::OgreScene *>(&scene);
    if (!s) return st;
    const detail::GpuSceneSubstrate &sub = s->substrate();
    const detail::GpuScene &t = sub.tables;
    st.live = t.live();
    st.framesInFlight = t.framesInFlight();
    st.copies = t.tableCopies();
    st.liveCopy = t.liveTable();
    const size_t workers = s->sceneManager() ? s->sceneManager()->getNumWorkerThreads() : 0u;
    st.workers = unsigned(workers + 1u);
    st.copyWrites = t.copyWrites();
    st.inFlightOverwrites = t.inFlightOverwrites();
    st.walks = sub.walks;
    st.extraWalks = sub.extraWalks;
    st.laterCalls = sub.laterCalls;
    st.commits = sub.commits;
    st.slotsCompared = sub.slotsCompared;
    st.flagsRecomputed = sub.flagsRecomputed;
    st.slotsNamed = sub.slotsNamed;
    st.wholeWalks = sub.wholeWalks;
    st.scansFull = s->scansFull();
    st.walkChecked = sub.walkChecked;
    st.walkMissed = sub.walkMissed;
    st.walkLastMissed = sub.walkLastMissed;
    st.holdsRayStructures = sub.rays != nullptr;
    return st;
}

}  // namespace engine
}  // namespace jahshaka

namespace jahshaka {
namespace engine {
namespace detail {

// ---------------------------------------------------------------------------
// THE MOVERS' SHADOW ON THE CARDS (PHOTON-CARDS-4) — the scene's in-frame
// answers, read from the GPU scene's table and its moved set (read-only), after
// the frame's update (OgreShadow.cpp runs ensureGpuScene for every drawn scene
// before any render target; the surface cache asks from inside its workspace).
//
//   * THE MOVERS: every slot the TLAS writes with kRayMaskMoverCaster (a traced,
//     shadow-casting mover), with its world AABB — walked only on a frame whose
//     moved set is not empty or whose slot count changed (or a moved slot was
//     renumbered), and kept otherwise: a still frame costs a compare.
//   * THE MOVED: the movers in the frame's moved set (a transform write, a
//     flags change, a birth — a walk without a pose change counts).
//   * THE STILL CASTERS THAT MOVED: every change to what the CAPTURED term
//     holds of a still caster — its transform, its visibility, its caster bit,
//     its CLASS (a still object promoted to the mover channel by a drag or by
//     setNodeMovable leaves the captured world; a demoted one joins it, with
//     no transform write at all) and its death. The old box is the cache's own
//     record of the node (mCardCasters), the new box the table's; a death has
//     one box and names it twice. AN ARRIVAL IS NOT ONE (yet): the receivers'
//     cards captured before a still caster arrived lack its shadow until a
//     light write recaptures them — recapturing them at the arrival compiles a
//     capture permutation on a frame a VR wearer is shown (vr.warmup, measured
//     in the PHOTON-CARDS-4 fix round), so the arrival waits for the capture
//     pass to be warmed.
uint32_t OgreScene::cutoutMaskGeneration(uint32_t slot) const {
    if (slot >= mItemNodes.size() || !mItemNodes[slot] || !mItemNodes[slot]->item) return 0u;
    const Ogre::Item *item = mItemNodes[slot]->item;
    uint32_t sum = 0u;
    for (size_t g = 0; g < item->getNumSubItems(); ++g) {
        const Ogre::HlmsDatablock *db = item->getSubItem(g)->getDatablock();
        if (!db || !db->getCreator() || db->getCreator()->getType() != Ogre::HLMS_PBS) continue;
        if (const Ogre::TextureGpu *t = static_cast<const Ogre::HlmsPbsDatablock *>(db)->getTexture(Ogre::PBSM_DIFFUSE))
            sum += textureUploadGeneration(t);
    }
    return sum;
}

bool PhotonComponent::cardMoverFrame(CardMoverFrame &out) {
    out = CardMoverFrame();
    if (!mScene.gpuScene().live() || !mRoot || !mRoot->getRenderSystem()) return false;
    const uint32_t frame = mRoot->getRenderSystem()->getVaoManager()->getFrameCount();
    const detail::GpuInstance *mirror = mScene.gpuScene().mirrorData();
    const uint32_t slots = mScene.gpuScene().slotCount();
    const bool moved = mScene.gpuScene().movedThisFrame(frame) && !mScene.gpuScene().movedSlots().empty();
    const auto flagsOf = [](const detail::GpuInstance &e) {
        Ogre::uint32 f;
        std::memcpy(&f, &e.boundsMax[3], sizeof(f));
        return f;
    };
    const auto moverCaster = [](Ogre::uint32 f) {
        return (f & kGpuRayTraced) && (f & kGpuMover) && (f & kGpuCaster);
    };
    const auto stillCaster = [](Ogre::uint32 f) {
        return (f & kGpuVisible) && (f & kGpuCaster) && !(f & kGpuMover);
    };
    const auto record = [&](uint32_t i) {
        const detail::GpuInstance &e = mirror[i];
        CardCasterRec &r = mCardCasters[i];
        r.node = mScene.gpuScene().nodeOfSlot(i);   // the slot table's id, no map find (decision 36)
        std::memcpy(r.world, e.world, sizeof(r.world));
        r.min = Ogre::Vector3(e.boundsMin[0], e.boundsMin[1], e.boundsMin[2]);
        r.max = Ogre::Vector3(e.boundsMax[0], e.boundsMax[1], e.boundsMax[2]);
        r.flags = flagsOf(e);
        r.material = e.raster[0];
        r.maskGen = (r.flags & kGpuAlphaTested) ? mScene.cutoutMaskGeneration(i) : 0u;
    };
    // WHAT THE CAPTURED TERM LOSES OR GAINS between the record and the table:
    // a still caster before or after, and its world, visibility, caster bit or
    // class changed.
    const auto casterChange = [&](const CardCasterRec &r, const detail::GpuInstance &e) {
        const Ogre::uint32 f = flagsOf(e);
        if (!stillCaster(f) && !stillCaster(r.flags)) return;
        const bool turned = std::memcmp(r.world, e.world, sizeof(r.world)) != 0;
        // ...and what the RAYS see of it (REFLECT-MOVERS-2): the traced set, the
        // cut-out bit, and a cut-out's material (its mask is its shadow) — a
        // material swap in place changes a card's sun term with nothing moving.
        const bool flagsMoved =
            ((r.flags ^ f) & (kGpuVisible | kGpuCaster | kGpuMover | kGpuRayTraced | kGpuAlphaTested)) != 0u;
        // ...and a cut-out's MASK re-uploaded in place (ALPHA-MASK-IDENTITY-1): the
        // same material word, new texels — keyed on the textures' upload generation.
        const bool maskMoved = ((r.flags | f) & kGpuAlphaTested) &&
            (r.material != e.raster[0] ||
             ((f & kGpuAlphaTested) && r.maskGen != mScene.cutoutMaskGeneration(uint32_t(&e - mirror))));
        if (!turned && !flagsMoved && !maskMoved) return;
        CardCasterMove m;
        m.node = r.node;
        m.oldMin = r.min;
        m.oldMax = r.max;
        m.newMin = Ogre::Vector3(e.boundsMin[0], e.boundsMin[1], e.boundsMin[2]);
        m.newMax = Ogre::Vector3(e.boundsMax[0], e.boundsMax[1], e.boundsMax[2]);
        out.casterMoves.push_back(m);
    };
    const auto death = [&](const CardCasterRec &r) {
        CardCasterMove m;
        m.node = r.node;
        m.oldMin = m.newMin = r.min;
        m.oldMax = m.newMax = r.max;
        out.casterMoves.push_back(m);
    };
    // THE WALK: the slot count changed (an item arrived or left), or a moved
    // slot holds another node than its record (the swap-remove renumbered the
    // tail into a freed slot while the count stayed — an arrival in the same
    // frame). Slots are not identities, so the records are compared BY NODE.
    bool walk = slots != mCardMoverSlots;
    if (moved && !walk)
        for (uint32_t slot : mScene.gpuScene().movedSlots())
            if (slot < slots && (slot >= mCardCasters.size() ||
                                 mCardCasters[slot].node != mScene.gpuScene().nodeOfSlot(slot))) {
                walk = true;
                break;
            }
    if (walk) {
        const auto walkStart = std::chrono::steady_clock::now();
        struct WalkTimer {
            PhotonComponent &p; std::chrono::steady_clock::time_point t0; uint32_t slots = 0;
            ~WalkTimer() {
                p.mCardWalkMicros = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
                p.mCardWalkSlots = slots;
                ++p.mCardWalks;
            }
        } walkTimer{ *this, walkStart, slots };
        std::unordered_map<NodeId, size_t> was;
        was.reserve(mCardCasters.size());
        for (size_t i = 0; i < mCardCasters.size(); ++i)
            if (mCardCasters[i].node) was[mCardCasters[i].node] = i;
        std::vector<CardCasterRec> prior;
        prior.swap(mCardCasters);
        mCardCasters.assign(slots, CardCasterRec());
        for (uint32_t i = 0; i < slots; ++i) {
            record(i);
            auto it = was.find(mCardCasters[i].node);
            if (it == was.end()) continue;   // an arrival: see the header
            casterChange(prior[it->second], mirror[i]);
            was.erase(it);
        }
        for (const auto &left : was)         // A DEATH: its footprint loses it
            if (stillCaster(prior[left.second].flags)) death(prior[left.second]);
    } else if (moved) {
        for (uint32_t slot : mScene.gpuScene().movedSlots()) {
            if (slot >= slots) continue;
            casterChange(mCardCasters[slot], mirror[slot]);
            record(slot);
        }
    }
    // NEW TEXELS WITH NOTHING MOVED (ALPHA-MASK-IDENTITY-1): on a frame where this
    // scene uploaded into a texture, every still cut-out caster whose mask
    // generation moved changes its footprint's sun term, in place.
    if (mScene.textureUploadSerial() != mCardUploadSerialSeen) {
        mCardUploadSerialSeen = mScene.textureUploadSerial();
        for (uint32_t i = 0; i < slots && i < mCardCasters.size(); ++i) {
            CardCasterRec &r = mCardCasters[i];
            if (!(r.flags & kGpuAlphaTested) || !stillCaster(r.flags)) continue;
            const uint32_t g = mScene.cutoutMaskGeneration(i);
            if (g == r.maskGen) continue;
            r.maskGen = g;
            CardCasterMove m;
            m.node = r.node;
            m.oldMin = m.newMin = r.min;
            m.oldMax = m.newMax = r.max;
            out.casterMoves.push_back(m);
        }
    }
    if (moved || walk) {
        mCardMovers.clear();
        for (uint32_t i = 0; i < slots; ++i) {
            const detail::GpuInstance &e = mirror[i];
            if (!moverCaster(flagsOf(e))) continue;
            CardMoverRec r;
            r.node = mScene.gpuScene().nodeOfSlot(i);
            r.min = Ogre::Vector3(e.boundsMin[0], e.boundsMin[1], e.boundsMin[2]);
            r.max = Ogre::Vector3(e.boundsMax[0], e.boundsMax[1], e.boundsMax[2]);
            mCardMovers.push_back(r);
        }
        mCardMoverSlots = slots;
        out.moversChanged = true;
    }
    out.movers.reserve(mCardMovers.size());
    for (const CardMoverRec &r : mCardMovers) {
        CardMoverBox b;
        b.node = r.node;
        b.min = r.min;
        b.max = r.max;
        out.movers.push_back(b);
    }
    if (moved)
        for (uint32_t slot : mScene.gpuScene().movedSlots())
            if (slot < slots && moverCaster(flagsOf(mirror[slot])))
                out.moved.push_back(mScene.gpuScene().nodeOfSlot(slot));
    return true;
}

}   // namespace detail
}   // namespace engine
}   // namespace jahshaka
