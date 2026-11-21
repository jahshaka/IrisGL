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

/// THE PROCESS'S END, AT THE ENGINE'S DEATH (COLD-BOOT-1, the fixtures' exit; spikes/exit-race/VERIFY.md). The exit-only
/// teardown calls `hook` right after its log line ("process exit - Root ... left to the OS"), on the destroying thread:
/// the point from which nothing may run but the process's end. A caller that cannot _Exit from that frame (a test harness
/// whose main returns its status) registers its final exit handler THERE, so exit() runs it before any handler or
/// static destructor registered earlier. Null (the default) = no hook. One process-wide pointer, set before the engine
/// dies; never called by a real destroy.
using ProcessExitHook = void (*)();
void setProcessExitHook(ProcessExitHook hook);

}}  // namespace jahshaka::engine
