#pragma once
// THE EXIT-ONLY TEARDOWN (URGENT #102). NOTHING here may reference Ogre, Qt or GL.
//
// At a process end the engine need not destroy its Vulkan device, and on NVIDIA it must not wait to: the driver's
// background compile thread holds a lock vkDestroyDevice waits on for the whole of its remaining CPU — 10-20 s after
// a shader change, measured (spikes/photon-2/11-2-teardown/ and spikes/urgent-102/) — and three ship rows sat at the
// 60-s cap on it.

namespace jahshaka { namespace engine {

/// The process is ending, and the caller ends it with std::_Exit once the engine is gone. Every Engine destroyed
/// after this call tears down exactly as always — the shader, pipeline and texture caches saved and their writer
/// joined, every view, scene and mesh released, the log written — but KEEPS Ogre's Root, so neither vkDestroyDevice
/// nor vkDestroyInstance runs; the OS reclaims the device. One-way: Engine::create refuses afterwards (Root lives).
/// Never for an in-process destroy-and-re-create (test_engine_recreate keeps the real destroy): only a process end
/// that _Exits right after the engine dies may call it — exit() would run the driver's and Ogre's finalizers
/// against the live device.
void markProcessExit();

}}  // namespace jahshaka::engine
