// THE DESCRIPTOR POOL FROM ITS LAYOUTS (D6-FORK-TOOLING, audit V2-D F8).
//
// The raw ray passes (OgreRayQuery.cpp, OgreScreenProbeGather.cpp) own their own
// descriptor pools, because Ogre's descriptor types have no acceleration structure
// (audit V2-D §4). A pool is a promise of descriptors BY TYPE, and it used to be
// written by hand beside the layout it serves ("sets * 7u // + the hit list's
// two"): a binding added to a layout without the matching pool line allocates
// past the pool — VK_ERROR_OUT_OF_POOL_MEMORY on a strict driver, a silent
// over-allocation that works on NVIDIA and fails on the next device. The class of
// defect is invisible until something else trips on it.
//
// So a pool is never counted by hand, and a layout and its pool line cannot come
// apart: a DescriptorPoolPlan CREATES each layout (makeLayout) from the binding
// array, and records in the same call how many sets of it the pool must hold; the
// pool's sizes are the per-type sums of those bindings' descriptorCounts (arrays
// whole), times the sets. The pool then allocates every set it was planned for at
// birth and resets (vkResetDescriptorPool frees everything): a failure there is
// loud, by name, where the pool is made. MEASURED (the suite's negative control,
// RTX 4080 / 595.84): NVIDIA does NOT refuse an over-allocation — a pool one
// binding short allocated all its sets — so that runtime proof is only the
// validation layer's net; the construction (one call makes both) is the fix.
// Every plan is recorded (descriptorPoolProofs) for the suite engine.descriptor_pools.
//
// Header-only and Vulkan-typed: included by the Vulkan TUs and by the suite only.
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <vector>

namespace jahshaka {
namespace engine {
namespace detail {

/// What one pool proved when it was created.
struct DescriptorPoolProof {
    std::string name;
    uint32_t sets = 0;         ///< maxSets: every set the pool was planned for
    uint32_t descriptors = 0;  ///< the sum of its sizes
    bool full = false;         ///< every planned set allocated at once, and was released
};

/// Every pool the process created through a plan, in creation order (a pool that
/// failed its proof is here too, with full = false).
inline std::vector<DescriptorPoolProof> &descriptorPoolProofs() {
    static std::vector<DescriptorPoolProof> proofs;
    return proofs;
}

class DescriptorPoolPlan {
public:
    /// THE LAYOUT AND ITS POOL LINE IN ONE CALL: creates the layout from
    /// `bindings[0..count)` and plans `sets` sets of it. The only way the passes
    /// make a layout that draws from a planned pool.
    bool makeLayout(VkDevice dev, const VkDescriptorSetLayoutBinding *bindings, uint32_t count,
                    uint32_t sets, VkDescriptorSetLayout &out, std::string &err, const char *what) {
        VkDescriptorSetLayoutCreateInfo sli{};
        sli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        sli.bindingCount = count;
        sli.pBindings = bindings;
        if (vkCreateDescriptorSetLayout(dev, &sli, nullptr, &out) != VK_SUCCESS) {
            out = VK_NULL_HANDLE;
            err = std::string(what) + ": vkCreateDescriptorSetLayout failed";
            return false;
        }
        add(out, bindings, count, sets);
        return true;
    }

    /// `sets` sets of a layout created from `bindings[0..count)` elsewhere — the very
    /// array handed to vkCreateDescriptorSetLayout (makeLayout is the way that
    /// guarantees it; the suite uses this to build its negative control).
    void add(VkDescriptorSetLayout layout, const VkDescriptorSetLayoutBinding *bindings,
             uint32_t count, uint32_t sets) {
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t n = bindings[i].descriptorCount * sets;
            if (n == 0) continue;
            bool merged = false;
            for (VkDescriptorPoolSize &s : mSizes)
                if (s.type == bindings[i].descriptorType) { s.descriptorCount += n; merged = true; break; }
            if (!merged) mSizes.push_back(VkDescriptorPoolSize{ bindings[i].descriptorType, n });
        }
        mLayouts.insert(mLayouts.end(), sets, layout);
    }

    uint32_t maxSets() const { return uint32_t(mLayouts.size()); }
    const std::vector<VkDescriptorPoolSize> &sizes() const { return mSizes; }
    /// The pool's descriptors of one type (0 when the plan has none).
    uint32_t count(VkDescriptorType type) const {
        for (const VkDescriptorPoolSize &s : mSizes)
            if (s.type == type) return s.descriptorCount;
        return 0;
    }

    /// Creates the pool and proves it full (see the file comment). On failure `out`
    /// is VK_NULL_HANDLE and `err` names the pool, the plan and the VkResult.
    bool create(VkDevice dev, const char *name, VkDescriptorPoolCreateFlags flags,
                VkDescriptorPool &out, std::string &err) const {
        out = VK_NULL_HANDLE;
        DescriptorPoolProof proof;
        proof.name = name;
        proof.sets = maxSets();
        for (const VkDescriptorPoolSize &s : mSizes) proof.descriptors += s.descriptorCount;
        VkDescriptorPoolCreateInfo dpi{};
        dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpi.flags = flags;
        dpi.maxSets = maxSets();
        dpi.poolSizeCount = uint32_t(mSizes.size());
        dpi.pPoolSizes = mSizes.data();
        VkResult r = vkCreateDescriptorPool(dev, &dpi, nullptr, &out);
        if (r != VK_SUCCESS) {
            out = VK_NULL_HANDLE;
            err = std::string(name) + ": vkCreateDescriptorPool failed (" + std::to_string(int(r)) + ")";
            descriptorPoolProofs().push_back(proof);
            return false;
        }
        std::vector<VkDescriptorSet> sets(mLayouts.size(), VK_NULL_HANDLE);
        VkDescriptorSetAllocateInfo dai{};
        dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dai.descriptorPool = out;
        dai.descriptorSetCount = uint32_t(mLayouts.size());
        dai.pSetLayouts = mLayouts.data();
        r = mLayouts.empty() ? VK_SUCCESS : vkAllocateDescriptorSets(dev, &dai, sets.data());
        if (r == VK_SUCCESS) r = vkResetDescriptorPool(dev, out, 0);
        if (r != VK_SUCCESS) {
            vkDestroyDescriptorPool(dev, out, nullptr);
            out = VK_NULL_HANDLE;
            err = std::string(name) + ": the pool cannot hold the " + std::to_string(maxSets()) +
                  " sets its layouts were planned for (" + std::to_string(int(r)) +
                  ") - a layout and its pool disagree";
            descriptorPoolProofs().push_back(proof);
            return false;
        }
        proof.full = true;
        descriptorPoolProofs().push_back(proof);
        return true;
    }

private:
    std::vector<VkDescriptorPoolSize> mSizes;
    std::vector<VkDescriptorSetLayout> mLayouts;
};

}   // namespace detail
}   // namespace engine
}   // namespace jahshaka
