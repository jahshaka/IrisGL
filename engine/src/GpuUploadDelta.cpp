// THE CHANGED ROWS, UPLOADED (GpuUploadDelta.h).
#include "GpuUploadDelta.h"

#include <OgreRoot.h>
#include <OgreRenderSystem.h>
#include <Vao/OgreBufferPacked.h>
#include <Vao/OgreStagingBuffer.h>
#include <Vao/OgreVaoManager.h>

#include <algorithm>
#include <cstring>
#include <utility>

namespace jahshaka {
namespace engine {
namespace detail {

namespace {
/// Neighbouring changed runs closer than this many elements are written as one (a copy a run has a cost of its own).
constexpr size_t kMergeGap = 4u;
/// ...and no more copies than this a call (the gap doubles until the runs fit).
constexpr size_t kMaxRuns = 1024u;
}   // namespace

void DeltaUpload::reset(Ogre::BufferPacked *buf, const void *data, size_t bytes) {
    mBuf = buf;
    mLast.clear();
    mKnown.clear();
    if (!buf) return;
    const size_t eb = buf->getBytesPerElement();
    const size_t n = buf->getNumElements();
    mLast.assign(n * eb, 0u);
    mKnown.assign(n, false);
    if (data && eb) {
        const size_t whole = std::min(bytes, n * eb) / eb;
        std::memcpy(mLast.data(), data, whole * eb);
        for (size_t i = 0; i < whole; ++i) mKnown[i] = true;
    }
}

size_t DeltaUpload::upload(Ogre::BufferPacked *buf, const void *now, size_t first, size_t count) {
    if (!buf || !now || !count) return 0u;
    const size_t eb = buf->getBytesPerElement();
    const unsigned char *src = static_cast<const unsigned char *>(now);
    if (buf != mBuf || mKnown.size() != buf->getNumElements()) reset(buf);
    // (out of range: Ogre's own upload refuses it; a shadow-copied buffer keeps its copy through Ogre's upload)
    if (first + count > mKnown.size() || buf->getShadowCopy() || buf->getBufferType() != Ogre::BT_DEFAULT) {
        buf->upload(now, first, count);
        if (first + count <= mKnown.size()) {
            std::memcpy(mLast.data() + first * eb, src, count * eb);
            for (size_t i = 0; i < count; ++i) mKnown[first + i] = true;
        }
        return count * eb;
    }
    // THE RUNS: [begin, end) elements of the range whose bytes differ from the mirror (or were never uploaded)
    std::vector<std::pair<size_t, size_t>> runs;
    for (size_t i = 0; i < count; ++i) {
        const size_t e = first + i;
        if (mKnown[e] && std::memcmp(src + i * eb, mLast.data() + e * eb, eb) == 0) continue;
        if (!runs.empty() && i - runs.back().second <= kMergeGap) runs.back().second = i + 1u;
        else runs.emplace_back(i, i + 1u);
    }
    if (runs.empty()) return 0u;
    for (size_t gap = kMergeGap * 2u; runs.size() > kMaxRuns; gap *= 2u) {
        std::vector<std::pair<size_t, size_t>> merged;
        for (const auto &r : runs) {
            if (!merged.empty() && r.first - merged.back().second <= gap) merged.back().second = r.second;
            else merged.push_back(r);
        }
        runs.swap(merged);
    }
    size_t total = 0u;
    for (const auto &r : runs) total += (r.second - r.first) * eb;
    Ogre::VaoManager *vao = Ogre::Root::getSingleton().getRenderSystem()->getVaoManager();
    Ogre::StagingBuffer *staging = vao->getStagingBuffer(total, true);
    unsigned char *dst = static_cast<unsigned char *>(staging->map(total));
    std::vector<Ogre::StagingBuffer::Destination> copies;
    copies.reserve(runs.size());
    size_t at = 0u;
    for (const auto &r : runs) {
        const size_t len = (r.second - r.first) * eb;
        std::memcpy(dst + at, src + r.first * eb, len);
        copies.emplace_back(buf, (first + r.first) * eb, at, len);
        at += len;
    }
    staging->unmap(copies.data(), copies.size());
    staging->removeReferenceCount();
    // (a shadow copy would miss these writes: the tables are created without one)
    std::memcpy(mLast.data() + first * eb, src, count * eb);
    for (size_t i = 0; i < count; ++i) mKnown[first + i] = true;
    return total;
}

}  // namespace detail
}  // namespace engine
}  // namespace jahshaka
