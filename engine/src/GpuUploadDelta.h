// THE CHANGED ROWS, UPLOADED (PHOTON-7's walk hitch, 2026-10-25). A CPU-built table re-uploaded whole every time one
// row of it changes is staging traffic the GPU already holds: the card tables (720 KB at each allocation change) and
// the field's probe records (every probe at each window follow, of which only the entered slab moves) were the Bistro
// walks' ~0.8 MB a frame, every quarter of the staging ring a mid-frame fence flush. DeltaUpload mirrors what it last
// uploaded into ONE buffer and writes only the elements that differ - as runs, through one staging map with a
// destination a run (Ogre's own scattered upload: StagingBuffer::unmap(destinations)).
//
// Ogre-private (engine/src): no engine header includes it.
#ifndef JAHSHAKA_ENGINE_GPUUPLOADDELTA_H
#define JAHSHAKA_ENGINE_GPUUPLOADDELTA_H

#include <cstddef>
#include <vector>

namespace Ogre {
class BufferPacked;
}

namespace jahshaka {
namespace engine {
namespace detail {

class DeltaUpload {
public:
    /// THE BUFFER WAS (RE)CREATED OR DESTROYED: `buf` now holds `bytes` of `data` from element 0 (its initial data;
    /// nullptr: nothing known - the next upload writes its whole range). Every create and destroy of the buffer calls it.
    void reset(Ogre::BufferPacked *buf, const void *data = nullptr, size_t bytes = 0);
    /// Writes into `buf` the elements of [first, first + count) where `now` (count elements, `buf`'s bytes per element)
    /// differs from what this mirror last put there; a range never uploaded is written whole. A buffer other than
    /// the one reset() named is written whole and becomes the mirrored one. Returns the bytes uploaded.
    size_t upload(Ogre::BufferPacked *buf, const void *now, size_t first, size_t count);

private:
    Ogre::BufferPacked *mBuf = nullptr;
    std::vector<unsigned char> mLast;     ///< what the GPU holds, element by element (mKnown says which)
    std::vector<bool> mKnown;             ///< per element: mLast holds what was uploaded there
};

}  // namespace detail
}  // namespace engine
}  // namespace jahshaka

#endif
