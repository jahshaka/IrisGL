#pragma once
// THE ENGINE's IDS TYPES - engine-neutral values, one subject (ENGINE-HEADERS-1: split from Types.h by
// declaration name). NOTHING here may reference Ogre, Qt or GL; a new subject is a new header, never an append here.
#include "core/ids.h"

#include <atomic>

namespace jahshaka { namespace engine {

/// Opaque handles to a Scene's meshes and materials. 0 is "none". Per-Scene and
/// monotonic like NodeId. A mesh or material may be shared by any number of nodes.
using MeshId     = unsigned int;
using MaterialId = unsigned int;
/// THE RESOURCE IDS ARE RESERVED BY WHOEVER CREATES (ENGINE-UPGRADE-U3 stage 3): one process-wide counter per kind,
/// 1-based (0 = none), never reused — a host names a mesh, a material or a texture in the record before the engine
/// built it (Scene::createMesh(data, id) ...), and an engine create with no id reserves from the same counter.
inline MeshId reserveMeshId() { static std::atomic<MeshId> next{1}; return next.fetch_add(1, std::memory_order_relaxed); }
inline MaterialId reserveMaterialId()
{
    static std::atomic<MaterialId> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

using TextureId = unsigned int;
inline TextureId reserveTextureId()
{
    static std::atomic<TextureId> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

/// Native window handle a View renders into (X11 Window / HWND / NSView).
///
/// macOS: pass the host's `NSView*` (Qt: `QWidget::winId()`, a QNSView). The
/// backend adds its OWN CAMetalLayer-backed child view under it and presents
/// there — it never takes over the host's layer, which Qt refuses anyway. An
/// `NSWindow*` (its contentView is used) or a ready-made `CAMetalLayer*` (the
/// host then owns its size and contentsScale) are accepted too.
using NativeWindowHandle = unsigned long long;

/// Native display connection (X11 Display*). MUST be the host's own connection —
/// opening a second connection to the same windows causes flicker and cross-bleed
/// between windows. 0 where the platform has no such concept.
///
/// KNOWN LEAK (audit; ENGINEERING_DEBT_SPEC.md item 4). This is an X11 concept
/// in a supposedly platform-neutral boundary, and the macOS host has arrived
/// since that note was written without settling it: macOS simply leaves it 0
/// and the Linux host fills it from QNativeInterface::QX11Application
/// (src/bridge/enginehost.cpp). Still not fixed HERE because the fix is not a
/// rename: window and display are one surface identity, so the honest shape is
/// a single native-surface value that both hosts fill and OgreView reads —
/// which is host-side work (a different lane's files) plus a View change, for
/// no behaviour difference. Recorded, deliberately not smuggled in.
using NativeDisplayHandle = unsigned long long;

/// THE PIXEL FORMAT OF A VIEW's FINAL TARGET (SHADER-SYSTEM-1 stage 7a, R4 (A)): the four a window's surface can be given
/// (the swapchain's one choice, Engine::windowTargetShape). An offscreen view is RGBA8 unless it is created with another
/// (Engine::createOffscreenView's `format`); the passes that write the final target key their pipelines on it.
enum class TargetFormat { Rgba8Unorm, Bgra8Unorm, Rgba8UnormSrgb, Bgra8UnormSrgb };

/// WHAT A HOST WINDOW's FINAL TARGET IS (Engine::windowTargetShape): its format and the sample count its swapchain
/// validates a request to - the two terms of a final pass's pipeline a window decides.
struct WindowTargetShape {
    TargetFormat format = TargetFormat::Rgba8Unorm;
    unsigned samples = 1u;
};

/// A node's id: THE DOCUMENT'S, ONE PROCESS-WIDE SPACE (ENGINE-UPGRADE-U3 stage 3, F6): 64 bits, 1-based (0 is
/// "none"), never reused, so a stale id is harmless. reserveNodeId() hands out the next one — to the document
/// (iris::SceneNode), to the mirror's own helper nodes and to every Scene::createNode alike, so the record can name a
/// node before the engine built it and no two scenes' ids collide. The GPU's 32-bit node key is Ogre's own IdType
/// (OgreScene's gpuKeyOf), never this.
using NodeId = unsigned long long;
/// One counter for the process, on every platform: IrisCore's (core/ids.h: defined once, in libIrisCore).
inline NodeId reserveNodeId() { return NodeId(iris::nextNodeId()); }

}}  // namespace jahshaka::engine
