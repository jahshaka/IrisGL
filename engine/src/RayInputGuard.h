#pragma once
// THE GUARD ON A RAY JOB's INPUTS (GATHER-HANG-1 stage 2). A ray job binds views of Ogre textures (RayQueryViewCache.h)
// and of its own images; a NULL view in a descriptor the shader may read is undefined behaviour, never a black sample,
// and RayQueryTier::textureView answers null for a texture that is NOT RESIDENT (its storage gone: OnStorage or
// OnSystemRam - before the guard it was viewed on the manager's blank image and traced as if it were the input) or that
// has no image. THE LAW: a job whose views are not all made REFUSES ITS DISPATCH that frame - the frame keeps what the
// job last wrote - and the refusal is SAID ONCE per job for the tier's life (a persistent one must not flood the log;
// a silent one is indistinguishable from a trace that draws nothing), and COUNTED every time.
//
// No Vulkan and no Ogre type here: a view is its 64-bit handle, the job its name; the tier logs what admit() says
// (tests/engine/test_ray_input_guard.cpp, engine.ray_input_guard). Not thread-safe: the tier records on one thread.

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <unordered_map>

namespace jahshaka::engine::detail {

class RayInputGuard {
public:
    /// true when every view of `job` is made; false = the job refuses its dispatch: counted, and `say` (when given) set
    /// to the line to log the FIRST time this job refuses, left empty after that
    bool admit(const char *job, const uint64_t *views, size_t count, std::string *say = nullptr) {
        if (say) say->clear();
        size_t missing = 0u, first = 0u;
        for (size_t i = 0; i < count; ++i)
            if (!views[i] && missing++ == 0u) first = i;
        if (missing == 0u) return true;
        ++mRefused;
        uint64_t &n = mByJob[job ? job : "?"];
        if (n++ == 0u && say)
            *say = std::string("rayquery: ") + (job ? job : "?") + " refused its dispatch - " + std::to_string(missing) +
                   " of " + std::to_string(count) + " input views null (the first at " + std::to_string(first) +
                   "): a texture not resident or with no image (said once; every refusal counted)";
        return false;
    }
    bool admit(const char *job, std::initializer_list<uint64_t> views, std::string *say = nullptr) {
        return admit(job, views.begin(), views.size(), say);
    }
    /// the refusals of every job, and of one job, over the tier's life
    uint64_t refused() const { return mRefused; }
    uint64_t refused(const char *job) const {
        const auto it = mByJob.find(job ? job : "?");
        return it == mByJob.end() ? 0u : it->second;
    }

private:
    uint64_t mRefused = 0u;
    std::unordered_map<std::string, uint64_t> mByJob;
};

}   // namespace jahshaka::engine::detail
