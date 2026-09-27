// ATOM P3's SUBSTRATE — the cull's own device buffers (the result half of
// SPECS/atom/A4_SUBSTRATE_CULL_DESIGN.md section 1.1).
//
// WHY IT IS A CLASS AND NOT THREE LOCALS. The job never allocates per frame:
// every buffer here is sized to the GPU scene's slot CAPACITY — the quantity
// that DOUBLES (64, 128, 256...) and then stands still, never the slot COUNT,
// which moves on every attach — so a cull in a frame costs one small upload and
// three dispatches and no Vulkan allocation at all, and a grow happens as often
// as the table's own does. The probe this replaces created and destroyed five
// buffers per call, which was right for a proof and wrong for a substrate.
//
// NO VULKAN HERE EITHER (the same rule the GPU scene keeps): the results are
// Ogre `UavBufferPacked`s, which an `HlmsComputeJob` binds as SSBOs and Vulkan
// creates with INDIRECT_BUFFER_BIT already set — so the same buffer is legal as
// `vkCmdDispatchIndirect`'s argument (patch 0032's path, used here) and as a
// `vkCmdDrawIndexedIndirect` source for stage 3's own pass, with no patch and no
// `IndirectBufferPacked` (which Vulkan emulates in system memory at this pin and
// which therefore has no VkBuffer at all — the stage-0 spike's finding 7.2).
#ifndef JAHSHAKA_ENGINE_GPUCULL_H
#define JAHSHAKA_ENGINE_GPUCULL_H

#include <algorithm>
#include <cstdint>
#include <string>

namespace Ogre {
class UavBufferPacked;
class VaoManager;
}  // namespace Ogre

namespace jahshaka {
namespace engine {
namespace detail {

/// THE REQUEST, as the shaders read it: 224 bytes, std430, all lanes vec4-sized
/// so C++ and GLSL agree with no padding rule to remember. The rows of the
/// view-projection are ROWS (row i in element i) for the same reason the
/// instance table's world transform is — a GLSL matrix in std430 is
/// column-major and a transposed read is silently right for a translation.
struct GpuCullParams {
    float    planes[24] = {};      ///< 6 x (a, b, c, d), inward, normalised
    float    viewProjRow[16] = {}; ///< row-major
    float    eye[4] = {};          ///< xyz the camera, w the LOD switch band (0 = none)
    float    lod[4] = {};          ///< x tolerance, y proj[1][1], z viewport height, w 1 = orthographic
    uint32_t counts[4] = {};       ///< x instances, y flagsRequired, z flagsForbidden, w mode
    uint32_t hzb[4] = {};          ///< x levels, y width, z height, w reverseZ
    /// THE CUT'S BUDGET (mode 3, ATOM-CLUSTER-CUT): x = the compacted index stream's
    /// capacity in indices, y = the drawn-cluster record capacity, z = where the stream's
    /// COARSE RESERVE begins (the main region is [0, z)), w = 0.
    uint32_t cut[4] = {};
    /// THE VIEWPORT'S RECTANGLE IN THE PYRAMID'S MIP 0 (ATOM-OCCLUSION-1): x0, y0, width,
    /// height in texels — NDC spans the pass's viewport, which is a letterboxed view's
    /// inset and not its whole target. Width 0 = the whole pyramid. Read by the depth
    /// test alone; the jobs that declare a shorter struct read its prefix.
    uint32_t hzbRect[4] = {};
};
static_assert(sizeof(GpuCullParams) == 256, "the cull request's layout is a shader contract");

class GpuCull {
public:
    ~GpuCull();
    /// Creates (or grows) the result buffers for `slotCapacity` instances.
    /// Refuses where there is no VaoManager — the headless boot, where every
    /// consumer keeps its CPU path.
    bool ensure(Ogre::VaoManager *vao, uint32_t slotCapacity, std::string &err);
    void destroy();
    bool live() const { return mParams != nullptr; }
    uint32_t capacity() const { return mCapacity; }

    Ogre::UavBufferPacked *params() const { return mParams; }
    Ogre::UavBufferPacked *visible() const { return mVisible; }
    Ogre::UavBufferPacked *levels() const { return mLevels; }
    Ogre::UavBufferPacked *survivors() const { return mSurvivors; }
    Ogre::UavBufferPacked *count() const { return mCount; }
    Ogre::UavBufferPacked *draws() const { return mDraws; }
    /// THE BAND'S DIRECTION STATE (ogre-patch 0075's `mHysteresisLod`, per list):
    /// three words a slot — the last level a BANDED request chose, and the node id
    /// and mesh it was chosen for (a slot renumbered by a removal, or a mesh swap,
    /// starts with no band). The only buffer that carries state between requests —
    /// a grow starts it over.
    Ogre::UavBufferPacked *held() const { return mHeld; }

    // ---- THE CUT (mode 3, ATOM-CLUSTER-CUT; SPECS/v2/CLUSTER_CUT_DESIGN.md D1/D2) --
    /// Makes the cut's per-frame buffers exist at the current budget: the larger of
    /// this list's own (grown when a cut it recorded reported an overflow —
    /// `noteCutOverflow`) and `sceneNeed`, the SCENE's high-water mark of the indices a
    /// cut asked for (OgreScene::cutIndexNeed) — so a view born into a heavy scene (a
    /// screenshot, a thumbnail: two frames of life) starts at the size the scene needs.
    bool ensureCut(Ogre::VaoManager *vao, std::string &err, uint32_t sceneNeed = 0u);
    /// THE COMPACTED INDEX STREAM: the drawn clusters' corners, mesh-local vertex
    /// indices (32-bit), each surviving instance's run contiguous; the id pass binds
    /// it as its index buffer (the identity buffer it replaced is deleted).
    Ogre::UavBufferPacked *cutStream() const { return mCutStream; }
    /// Per stream TRIANGLE, the id image's y word (`cluster << 8 | local triangle`)
    /// and the cluster's DAG depth (the x word's top byte): two words a triangle.
    Ogre::UavBufferPacked *cutTriWords() const { return mCutTriWords; }
    /// One uvec4 per DRAWN CLUSTER: (slot, the cluster's global index, its first
    /// index in the stream, its depth) — the emit job's work list.
    Ogre::UavBufferPacked *cutRecords() const { return mCutRecords; }
    /// Per SLOT: the instance's first TRIANGLE in the stream (the id pass's fragment
    /// adds gl_PrimitiveID to it).
    Ogre::UavBufferPacked *cutSlotBase() const { return mCutSlotBase; }
    uint32_t cutIndexBudget() const { return mCutIndexBudget; }
    uint32_t cutRecordBudget() const { return mCutRecordBudget; }
    /// Where the coarse reserve begins: the main region is the first 7/8 of the stream.
    uint32_t cutMainBudget() const { return mCutIndexBudget - mCutIndexBudget / 8u; }
    /// A TEST DOOR (the overflow's proof): the next budget is `indices`, the scene's
    /// high-water mark is ignored by this list, and overflows still double it.
    void setCutBudgetForTest(uint32_t indices) {
        mCutWantBudget = indices;
        mCutIgnoreScene = true;
        mCutForceCreate = true;
    }
    /// THE OVERFLOW, as the stats ring read it back: the budget doubles (to the
    /// ceiling) before the next cut. The frames in flight lose no object — an instance
    /// that does not fit draws its ROOT CUT from the coarse reserve (JahCullCut_cs) —
    /// unless even the reserve is full, which is counted apart.
    void noteCutOverflow(uint32_t overflowedInstances, uint32_t indicesAsked, uint32_t recordsAsked);
    /// THE LATE LIST FOLLOWS THE FIRST (ATOM-OCCLUSION-1, the Fable read's F1): the
    /// disocclusion pass can hold the whole frame the frame after a cut, so its next budget
    /// is at least the first list's — one seed for both lists, whatever either one learnt
    /// (the scene's high-water mark, an overflow, a test door) — before it records.
    void followCutBudget(const GpuCull &first) {
        const uint32_t want = std::max(first.mCutWantBudget, first.mCutIndexBudget);
        if (want > mCutWantBudget) {
            mCutWantBudget = want;
            if (mCutIndexBudget < want) mCutForceCreate = true;
        }
    }
    /// The first budget and the ceiling (indices): 2 M (700 k triangles; 8 MB of stream
    /// + 5.3 MB of triangle words + 1.3 MB of records = ~15 MB a view) growing from what
    /// was asked to 32 M; a view of a scene that has needed more starts at the scene's
    /// high-water mark. NOT 8 M (the fix round's ask): with 8 M a view the selftest's
    /// fixture B flipped two pixels at the gizmo's Z handle in 5 of 7 runs (0 of 12 at
    /// 2 M, same code) — a layout-sensitive read somewhere, owner not found; reported.
    static constexpr uint32_t kCutIndexBudgetFirst = 2u * 1024u * 1024u;
    static constexpr uint32_t kCutIndexBudgetCeiling = 32u * 1024u * 1024u;
    /// Records per index of budget: one record per 24 indices (8 triangles).
    static constexpr uint32_t kCutIndicesPerRecord = 24u;

    /// Elements of the count buffer. [0] is the survivor count (a draw's
    /// drawCount); [1..3] are job 3's thread-group counts, which is where
    /// patch 0032's indirect dispatch reads them from — hence the offset below;
    /// [4] the triangles the commands draw (the stats). THE CUT'S (mode 3):
    /// [5..7] the cut job's groups (one per survivor), [8..10] the emit job's
    /// groups (one per drawn-cluster record, [8] = records written), [11] the
    /// stream's cursor (indices reserved), [12] instances that did not fit the
    /// budget (drawn nothing this frame), [13] the indices they asked for, [14] the
    /// (instance, cluster) pairs the rule evaluated, [15] the coarse reserve's cursor.
    /// (Since the fix round: [12] = instances drawn COARSE — their root cut, from the
    /// reserve — and [13] = instances drawn NOTHING, neither fitting.)
    /// THE DEPTH TEST'S (ATOM-OCCLUSION-1): [16] the instances the pyramid REJECTED
    /// (the test's visibility word 2 — the set the disocclusion pass tests again);
    /// [17..19] zero.
    static constexpr uint32_t kCountElements = 20u;
    static constexpr uint32_t kIndirectOffsetBytes = 4u;
    static constexpr uint32_t kCutIndirectOffsetBytes = 5u * 4u;
    static constexpr uint32_t kEmitIndirectOffsetBytes = 8u * 4u;
    /// Threads per group of all three jobs (JahshakaCompute.material.json) and
    /// the width of the compaction's shared-memory scan.
    static constexpr uint32_t kThreadsPerGroup = 64u;
    static constexpr uint32_t kDrawWords = 5u;   ///< a VkDrawIndexedIndirectCommand

private:
    Ogre::VaoManager *mVao = nullptr;
    Ogre::UavBufferPacked *mParams = nullptr;
    Ogre::UavBufferPacked *mVisible = nullptr;
    Ogre::UavBufferPacked *mLevels = nullptr;
    Ogre::UavBufferPacked *mSurvivors = nullptr;
    Ogre::UavBufferPacked *mCount = nullptr;
    Ogre::UavBufferPacked *mDraws = nullptr;
    Ogre::UavBufferPacked *mHeld = nullptr;
    uint32_t mCapacity = 0u;
    Ogre::UavBufferPacked *mCutStream = nullptr;
    Ogre::UavBufferPacked *mCutTriWords = nullptr;
    Ogre::UavBufferPacked *mCutRecords = nullptr;
    Ogre::UavBufferPacked *mCutSlotBase = nullptr;
    uint32_t mCutIndexBudget = 0u, mCutRecordBudget = 0u, mCutSlotCapacity = 0u;
    uint32_t mCutWantBudget = kCutIndexBudgetFirst;
    bool mCutIgnoreScene = false;
    bool mCutForceCreate = false;   ///< the test door's budget may be SMALLER than the current one
};

}  // namespace detail
}  // namespace engine
}  // namespace jahshaka

#endif  // JAHSHAKA_ENGINE_GPUCULL_H
