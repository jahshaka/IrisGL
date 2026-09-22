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
    float    eye[4] = {};
    float    lod[4] = {};          ///< x tolerance, y proj[1][1], z viewport height, w unused
    uint32_t counts[4] = {};       ///< x instances, y flagsRequired, z flagsForbidden, w mode
    uint32_t hzb[4] = {};          ///< x levels, y width, z height, w reverseZ
};
static_assert(sizeof(GpuCullParams) == 224, "the cull request's layout is a shader contract");

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

    /// Elements of the count buffer. [0] is the survivor count (a draw's
    /// drawCount); [1..3] are job 3's thread-group counts, which is where
    /// patch 0032's indirect dispatch reads them from — hence the offset below.
    static constexpr uint32_t kCountElements = 8u;
    static constexpr uint32_t kIndirectOffsetBytes = 4u;
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
    uint32_t mCapacity = 0u;
};

}  // namespace detail
}  // namespace engine
}  // namespace jahshaka

#endif  // JAHSHAKA_ENGINE_GPUCULL_H
