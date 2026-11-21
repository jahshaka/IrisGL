#pragma once
// THE RAY TIER's IMAGE VIEWS OF OGRE TEXTURES, CACHED PER TEXTURE (GATHER-HANG-1 stage 1; the spec's stage 1 and its
// preflight, spikes/gather-hang-1/PREFLIGHT-stage-1.md).
//
// Before it every ray job made a fresh VkImageView of each Ogre texture it bound, every frame, and retired it into the
// tier's bin: fifteen sites in three files, up to about 420 views made and destroyed a frame (counted from the code's
// loops: spikes/gather-hang-1/AUDIT-stage-1.md section 6). The cache removes that churn with the same picture. (It
// was written while the card hung around a scene change; that hang was the 595.99.02 driver on the RTX 5060 Ti - it
// faulted with the cache too, and the base WITHOUT it ran 0 in 18 on 615.71.09: the cache is not its cure and claims
// no fix.) THE LAW: one view per (texture, kind), made once and kept for the life of the IMAGE it was built on and of
// the parameters it was made with; retired (through the tier's bin, never destroyed here) exactly once when that
// image goes - the texture's LostResidency (an in-place re-make: its images are destroyed first) or its Deleted - or
// when a lookup of the same (texture, kind) finds a DIFFERENT image (the blank-to-final display swap fires neither
// reason) or different parameters (a re-format while not resident fires none either: the old view is never left
// behind as an orphan). An image of 0 (a texture with no storage) is refused: no view, a null handle. A failed make
// caches and registers nothing, and an entry whose re-make failed keeps its old view. ONE registration per texture
// however often it is looked up (Ogre's addListener does not de-duplicate), taken away at Deleted and at close().
//
// THE KEY carries the image the view is really built on beside the texture POINTER because a texture re-made at the
// same address is a different texture (the reflection's measured trap, OgreRayQuery.cpp recordReflect: Ogre's own view
// cache is keyed by the pointer and answered with a dead view), and the KIND because one texture is read several
// ways (hv.dest sampled and as storage; the chain's depth as a plain view and as a layer array). A kind's other fields
// (format, mips, layers, cube-as-array) are the texture's own at the lookup (RayQueryTier::textureView), so one
// (texture, kind) holds ONE entry: a lookup with other fields means the texture changed under it.
//
// No Vulkan and no Ogre type here: the tier injects make (the real view), retire (its bin) and watch (its
// TextureGpuListener) as functions, so the law is a CPU row (tests/engine/test_ray_view_cache.cpp, engine.ray_view_cache).
// Not thread-safe: the tier and Ogre's listener calls both run on the thread that records the frame.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace jahshaka::engine::detail {

/// The four ways the tier views an Ogre texture (the preflight's P1-P4), each on the image it is really built on.
enum class RayViewKind : uint8_t {
    Sampled,        ///< P1: a sampled view on the DISPLAY image (Ogre's `_createView`, the texture's own type)
    Storage,        ///< P2: a storage view on the DISPLAY image (one mip; a cube becomes a 2D array)
    LayerSampled,   ///< P3: a 2D-array view of every layer on the FINAL image, sampled (every eye's layer)
    LayerStorage,   ///< P4: the same, storage: the linear-equivalent format, one mip
};

/// What a view is made from. `format` is the pixel format the view takes; a count of 0 means "every remaining one".
struct RayViewKey {
    const void *texture = nullptr;
    RayViewKind kind = RayViewKind::Sampled;
    uint32_t format = 0u;
    uint32_t baseMip = 0u, mipCount = 0u;
    uint32_t baseLayer = 0u, layerCount = 0u;
    bool cubeAsArray = false;
    bool operator==(const RayViewKey &o) const {
        return texture == o.texture && kind == o.kind && format == o.format && baseMip == o.baseMip &&
               mipCount == o.mipCount && baseLayer == o.baseLayer && layerCount == o.layerCount &&
               cubeAsArray == o.cubeAsArray;
    }
};

class RayQueryViewCache {
public:
    /// makes the view of `key` on `image` (0 on failure); hands a view to the retire window; registers (on = true) or
    /// takes away (false) the one listener of a texture.
    using Make = std::function<uint64_t(const RayViewKey &key, uint64_t image)>;
    using Retire = std::function<void(uint64_t view)>;
    using Watch = std::function<void(const void *texture, bool on)>;

    RayQueryViewCache(Make make, Retire retire, Watch watch)
        : mMake(std::move(make)), mRetire(std::move(retire)), mWatch(std::move(watch)) {}
    RayQueryViewCache(const RayQueryViewCache &) = delete;
    RayQueryViewCache &operator=(const RayQueryViewCache &) = delete;

    /// The view of `key` on `image`, made at the first lookup of that pair. A cached view of the same (texture, kind) on
    /// another image or with other parameters is retired after its replacement is made. `image` 0, a null texture or a
    /// failed make: 0, nothing cached, nothing registered (a failed re-make keeps the old entry).
    uint64_t view(const RayViewKey &key, uint64_t image) {
        if (!key.texture || !image) return 0u;
        auto it = mTextures.find(key.texture);
        if (it != mTextures.end()) {
            for (Cached &c : it->second) {
                if (c.key.kind != key.kind) continue;
                if (c.image == image && c.key == key) return c.view;
                const uint64_t made = mMake(key, image);
                if (!made) return 0u;
                const uint64_t old = c.view;
                c = Cached{ key, image, made };
                mRetire(old);
                return made;
            }
        }
        const uint64_t made = mMake(key, image);
        if (!made) return 0u;
        if (it == mTextures.end()) {
            it = mTextures.emplace(key.texture, std::vector<Cached>()).first;
            mWatch(key.texture, true);
        }
        it->second.push_back(Cached{ key, image, made });
        return made;
    }

    /// The texture's images were destroyed (an in-place re-make): its views retired; it stays registered.
    void lostResidency(const void *texture) {
        auto it = mTextures.find(texture);
        if (it == mTextures.end()) return;
        std::vector<Cached> gone;
        gone.swap(it->second);
        for (const Cached &c : gone) mRetire(c.view);
    }

    /// The texture is being destroyed: its views retired, its registration taken away, the pointer forgotten (a new
    /// texture at the same address starts fresh).
    void deleted(const void *texture) {
        auto it = mTextures.find(texture);
        if (it == mTextures.end()) return;
        std::vector<Cached> gone;
        gone.swap(it->second);
        mTextures.erase(it);
        for (const Cached &c : gone) mRetire(c.view);
        mWatch(texture, false);
    }

    /// The tier's close: every view into the retire window, every registration taken away.
    void close() {
        while (!mTextures.empty()) deleted(mTextures.begin()->first);
    }

    /// views held now, and textures registered now
    size_t live() const {
        size_t n = 0u;
        for (const auto &kv : mTextures) n += kv.second.size();
        return n;
    }
    size_t watched() const { return mTextures.size(); }

private:
    struct Cached {
        RayViewKey key;
        uint64_t image = 0u;
        uint64_t view = 0u;
    };
    Make mMake;
    Retire mRetire;
    Watch mWatch;
    /// a texture's entry exists exactly while its one listener is registered
    std::unordered_map<const void *, std::vector<Cached>> mTextures;
};

}   // namespace jahshaka::engine::detail
