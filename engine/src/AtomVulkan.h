// THE ATOM PASSES' RAW-VULKAN REACH, once (ATOM-SHADOWS-1): the id pass
// (OgreAtomIdPass.cpp) and the caster cut (OgreAtomCasterPass.cpp) both record into
// Ogre's command buffer and both address Ogre's buffers — the VkBuffer and offset of
// an Ogre buffer (the ray tier's own reach) and its device address as a shader's
// uvec2. Only a JAH_RAY_QUERY TU includes this (it needs the Vulkan render system's
// private headers).
#ifndef JAHSHAKA_ENGINE_ATOMVULKAN_H
#define JAHSHAKA_ENGINE_ATOMVULKAN_H

#include "OgreVulkanRenderSystem.h"
#include "Vao/OgreVulkanBufferInterface.h"

#include <cstdint>

namespace jahshaka {
namespace engine {
namespace detail {

inline Ogre::VulkanRenderSystem *atomVulkanOf(Ogre::RenderSystem *rs) {
    return rs ? dynamic_cast<Ogre::VulkanRenderSystem *>(rs) : nullptr;
}

/// The VkBuffer + byte offset of an Ogre buffer.
template <typename T>
void atomBufferOf(T *buf, VkBuffer &outBuffer, VkDeviceSize &outOffset) {
    auto *bi = static_cast<Ogre::VulkanBufferInterface *>(buf->getBufferInterface());
    outBuffer = bi->getVboName();
    outOffset = VkDeviceSize(buf->_getFinalBufferStart()) * buf->getBytesPerElement();
}

/// A device address as the shader's uvec2 (lo, hi).
template <typename T>
void atomAddressOf(VkDevice dev, PFN_vkGetBufferDeviceAddressKHR bufferDeviceAddress, T *buf, uint32_t out[2]) {
    VkBuffer b = VK_NULL_HANDLE;
    VkDeviceSize off = 0;
    atomBufferOf(buf, b, off);
    VkBufferDeviceAddressInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    info.buffer = b;
    const VkDeviceAddress a = bufferDeviceAddress(dev, &info) + off;
    out[0] = uint32_t(a & 0xFFFFFFFFull);
    out[1] = uint32_t(a >> 32u);
}

}  // namespace detail
}  // namespace engine
}  // namespace jahshaka

#endif  // JAHSHAKA_ENGINE_ATOMVULKAN_H
