// THE GPU CHECKPOINTS (PHOTON-7 stage 5; DOCS/traps/GATE_AND_RIG.md "A GPU hang names its dispatch in one run"):
// when the environment sets JAHSHAKA_GPU_CHECKPOINTS=1, every dispatch and draw of ours, every HlmsCompute dispatch
// (the fork's HlmsCompute::msJahDispatchHook) and every compositor pass marks the command stream
// (VK_NV_device_diagnostic_checkpoints, requested at the device by the fork's
// VulkanDevice::msDiagnosticCheckpointsAllowed); after a device loss the last mark each queue's pipeline reached is
// logged, with the ring's marks around it. THE HUNG DISPATCH IS THE ONE RIGHT AFTER THE BOTTOM_OF_PIPE MARK (a mark
// precedes its dispatch: everything before the bottom mark retired). Off (the default): nothing requested or marked.
// A private header of the engine's Vulkan TUs (no Ogre type; a command buffer travels as a void *).
#ifndef JAHSHAKA_ENGINE_GPUCHECKPOINTS_H
#define JAHSHAKA_ENGINE_GPUCHECKPOINTS_H

namespace jahshaka {
namespace engine {
namespace detail {

/// Before the render system loads: reads JAHSHAKA_GPU_CHECKPOINTS and tells the fork's device to request the
/// extension; installs the HlmsCompute dispatch hook. Idempotent.
void gpuCheckpointsConfigure();
/// True once the device has the extension and the entry points are loaded (resolved at the first mark).
bool gpuCheckpointsOn();
/// A mark named `name` (a string the caller keeps alive, or a literal - interned on first use) on the render
/// system's CURRENT recording queue (the graphics queue, or the async compute queue inside its bracket).
void gpuCheckpoint(const char *name);
/// ...on the command buffer `vkCommandBuffer` (a VkCommandBuffer) a raw-Vulkan pass records into.
void gpuCheckpointOn(void *vkCommandBuffer, const char *name);
/// After a device loss: every queue's last marks, logged (the engine log, critical, and stderr). Once.
void gpuCheckpointsDump();
/// The compositor passes' marks: a pass listener on the manager (`compositorManager2`: an Ogre::CompositorManager2),
/// installed after Root (nothing when the checkpoints are off) and removed before it.
void gpuCheckpointsInstallPassListener(void *compositorManager2);
void gpuCheckpointsUninstallPassListener(void *compositorManager2);

}  // namespace detail
}  // namespace engine
}  // namespace jahshaka

/// A raw-Vulkan site's mark, named by its file and line (always this build's: the macro expands where it stands).
#define JAH_GPU_STR2(x) #x
#define JAH_GPU_STR(x) JAH_GPU_STR2(x)
#define JAH_GPU_CHECKPOINT(cmd) ::jahshaka::engine::detail::gpuCheckpointOn((cmd), __FILE__ ":" JAH_GPU_STR(__LINE__))

#endif  // JAHSHAKA_ENGINE_GPUCHECKPOINTS_H
