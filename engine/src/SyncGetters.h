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
#include <thread>

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

/// THE HOST-CALL COUNTER (ENGINE-UPGRADE-U3 stage 4a, decision 39): the first statement of every engine entry that is
/// not a door (a post, a request, published(), the lockstep verb, a join) — counted into the same sites as the
/// getters, so host.sync_getters reads every synchronous host entry. Stage 4b's law: 0 over the row's window.
#if !defined(NDEBUG) && !defined(JAH_DEBUG)
#define JAH_DEBUG 1
#endif
namespace jahshaka { namespace engine { namespace detail {
/// THE AFFINITY LAW (U3 decision 39): under JAH_DEBUG, once the render thread owns the engine (stage 4b turns it on),
/// every counted engine entry asserts it runs ON the render thread or inside the engine's own side. Off until then.
extern std::atomic<bool> gAffinityChecks;
extern std::atomic<std::thread::id> gRenderThreadId;
void checkAffinity(const char *entry);
}}}   // namespace jahshaka::engine::detail
#ifdef JAH_DEBUG
#define JAH_HOST_CALL(n)                                                                                     \
    do {                                                                                                     \
        JAH_SYNC_GETTER(n);                                                                                  \
        if (::jahshaka::engine::detail::gAffinityChecks.load(std::memory_order_relaxed))                     \
            ::jahshaka::engine::detail::checkAffinity(n);                                                    \
    } while (0)
#else
#define JAH_HOST_CALL(n) JAH_SYNC_GETTER(n)
#endif
