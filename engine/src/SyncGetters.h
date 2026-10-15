#pragma once
// host.sync_getters — THE DOOR, COUNTED (ENGINE-UPGRADE-U3 stage 2).
//
// Every const virtual of Engine.h that STAYS a synchronous read of the engine's state (the request-shaped ones until
// stage 3 makes them requests, the snapshot-shaped ones the tests still read in lockstep) counts its calls made from
// the HOST's side — any call while no frame of the render loop is running on the calling thread. The engine's own
// calls inside a frame and its publish are the render side's and are not counted (EngineSideScope).
//
// One static site per getter, registered on its first counted call into a lock-free list; Engine::syncGetters() reads
// the list. Ogre-free: the engine's private TUs include it through EnginePrivate.h.
#include <atomic>

namespace jahshaka { namespace engine { namespace detail {

struct SyncGetterSite {
    const char *name;
    std::atomic<unsigned long long> calls{ 0ull };
    SyncGetterSite *next = nullptr;
    explicit SyncGetterSite(const char *n);
};
/// The registered sites (newest first).
SyncGetterSite *syncGetterSites();
/// > 0 while the calling thread runs the render loop's frame or publish.
extern thread_local int tlsEngineSide;
struct EngineSideScope {
    EngineSideScope() { ++tlsEngineSide; }
    ~EngineSideScope() { --tlsEngineSide; }
    EngineSideScope(const EngineSideScope &) = delete;
    EngineSideScope &operator=(const EngineSideScope &) = delete;
};

}}}   // namespace jahshaka::engine::detail

/// The first statement of every counted getter.
#define JAH_SYNC_GETTER(n)                                                                                   \
    do {                                                                                                     \
        if (::jahshaka::engine::detail::tlsEngineSide == 0) {                                                \
            static ::jahshaka::engine::detail::SyncGetterSite jahSyncSite_(n);                               \
            jahSyncSite_.calls.fetch_add(1ull, std::memory_order_relaxed);                                   \
        }                                                                                                    \
    } while (0)
