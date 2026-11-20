#pragma once
// THE RAY TIER's IMAGE VIEWS OF OGRE TEXTURES, CACHED PER TEXTURE (GATHER-HANG-1 stage 1; the spec's stage 1 and its
// preflight, spikes/gather-hang-1/PREFLIGHT-stage-1.md).
//
// Before it every ray job made a fresh VkImageView of each Ogre texture it bound, every frame, and retired it into the
// tier's bin: fifteen sites in three files. Around a scene change that per-frame create-and-destroy is what the card
// faulted on (Xid 109, a page fault in rq_probe_gather reading the closed scene's destroyed voxel volume; stage 0: the
// base 5 faults in 6, the views cached 0 in 6). THE LAW: one view per (texture, kind, parameters), made once and kept
// for the life of the IMAGE it was built on; retired (through the tier's bin, never destroyed here) exactly once when
// that image goes - the texture's LostResidency (an in-place re-make: its images are destroyed first) or its Deleted -
// or when a lookup finds the texture on a DIFFERENT image (the blank-to-final display swap fires neither reason). An
// image of 0 (a texture with no storage) is refused: no view, a null handle. ONE registration per texture however
// often it is looked up (Ogre's addListener does not de-duplicate), taken away at Deleted and at close().
//
// THE KEY carries the image the view is really built on beside the texture POINTER because a texture re-made at the
// same address is a different texture (the reflection's measured trap, OgreRayQuery.cpp recordReflect: Ogre's own view
// cache is keyed by the pointer and answered with a dead view), and the KIND because one texture is read several
// ways (hv.dest sampled and as storage; the chain's depth as a plain view and as a layer array).
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

    /// The view of `key` on `image`, made at the first lookup of that pair. A cached view of the same key on another
    /// image is retired first. `image` 0, a null texture or a failed make: 0, nothing cached, nothing registered.
    uint64_t view(const RayViewKey &key, uint64_t image) {
        if (!key.texture || !image) return 0u;
        auto it = mTextures.find(key.texture);
        if (it != mTextures.end()) {
            for (Cached &c : it->second) {
                if (!(c.key == key)) continue;
                if (c.image == image) return c.view;
                const uint64_t made = mMake(key, image);
                if (!made) return 0u;
                const uint64_t old = c.view;
                c.image = image;
                c.view = made;
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
