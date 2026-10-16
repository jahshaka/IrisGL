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
#include <mutex>
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

/// THE SNAPSHOT, THE EXCHANGE TRIPLE BUFFER (ENGINE-UPGRADE-U3 stage 5; decision 54). The render thread publishes every
/// display frame in the owner's mode while the host reads at its own pace. Three buffers, each index OWNED by exactly
/// one side at every instant: the render side's back (the one it fills) and newest (the one it last published — read,
/// never written, until it comes back through the exchange), the host's latched one, and the READY word between them
/// — one atomic: an index and a FRESH bit. The render publishes by `back = ready.exchange(back | fresh)`; the host
/// latches by `latched = ready.exchange(latched)` when the fresh bit is set. No window: a buffer one side holds is
/// never the other's to write.
/// ONE FRAME PER LATCH: a frame's whole set — every scene's buffer and the engine's — is exchanged under ONE short lock
/// (RenderThread::publish), and the host latches every buffer under the same lock, EAGERLY: at its tick's head
/// (Engine::latchSnapshot) and after every task it waited for (lockstep: every frame). The engine's own reads and a
/// process without a render thread read the newest.
extern std::atomic<bool> gRenderThreaded;
/// The lock a frame's set is published under and the host's latch is taken under.
std::mutex &snapshotLock();
struct SnapshotLatchable {
    SnapshotLatchable *prevLatchable = nullptr, *nextLatchable = nullptr;
    /// The host's exchange (called with snapshotLock() held).
    virtual void latchLocked() = 0;
protected:
    ~SnapshotLatchable() = default;
};
/// Every live snapshot, latched together (snapshotLock() held by both).
void registerSnapshot(SnapshotLatchable *s);
void unregisterSnapshot(SnapshotLatchable *s);
/// THE HOST'S LATCH: every snapshot's newest published frame, under the one lock.
void latchAllSnapshots();
template <class T>
struct SnapshotBuffers final : SnapshotLatchable {
    static constexpr unsigned kFresh = 4u;
    T buf[3];
    SnapshotBuffers() { registerSnapshot(this); }
    ~SnapshotBuffers() { unregisterSnapshot(this); }
    SnapshotBuffers(const SnapshotBuffers &) = delete;
    SnapshotBuffers &operator=(const SnapshotBuffers &) = delete;
    /// The buffer the next publish fills (render side).
    T &back() { return buf[mBack]; }
    /// The render side's publish of what it filled, with snapshotLock() held (a frame's set at once).
    void commitLocked() {
        mNewest = mBack;
        mBack = mReady.exchange(mBack | kFresh, std::memory_order_acq_rel) & 3u;
    }
    void commit() { std::lock_guard<std::mutex> l(snapshotLock()); commitLocked(); }
    const T &newest() const { return buf[mNewest]; }
    const T &hostView() const { return buf[mHost]; }
    void latchLocked() override {
        if (mReady.load(std::memory_order_acquire) & kFresh)
            mHost = mReady.exchange(mHost, std::memory_order_acq_rel) & 3u;
    }
    /// What a read on THIS thread sees: the newest on the engine's side or without a render thread; the host's latch
    /// otherwise.
    const T &read() const {
        if (tlsEngineSide != 0 || !gRenderThreaded.load(std::memory_order_relaxed)) return newest();
        return hostView();
    }
    void reset(const T &empty) {
        std::lock_guard<std::mutex> l(snapshotLock());
        buf[0] = buf[1] = buf[2] = empty;
        mBack = 1u; mNewest = 0u; mHost = 2u;
        mReady.store(0u, std::memory_order_release);
    }
private:
    unsigned              mBack = 1u, mNewest = 0u;   ///< the render side's
    unsigned              mHost = 2u;                 ///< the host's
    std::atomic<unsigned> mReady{ 0u };               ///< between them: an index and the fresh bit
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
