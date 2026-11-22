// iris::Signal - ONE ANNOUNCEMENT, ITS SUBSCRIBERS (NOQT-CORE-1 stage 1; V2 plan 0.12), in place of the hand-copied callback
// lists (StudioServices::onSceneOpened, AssetService::onLibraryChanged / onPinChanged).
//   connect(fn)  -> an Id (0 for an empty function: nothing connected); disconnect(id) -> whether it was connected;
//   notify(args) calls every subscriber connected WHEN THE CALL BEGAN, in connection order, over a SNAPSHOT: a subscriber that
//                connects or disconnects (itself or another) during it does not change THAT call - the next one sees it.
// THREADS: a Signal is used on its owner's thread only (connect, disconnect and notify); a subscriber that must act on
// another thread marshals there itself. (The method is notify, not emit: Qt defines `emit` as a macro in every Qt TU.)
#pragma once

#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace iris {

template <typename... Args>
class Signal
{
public:
    using Id = std::uint64_t;
    using Fn = std::function<void(Args...)>;

    Id connect(Fn fn)
    {
        if (!fn) return 0;
        const Id id = mNext++;
        mSubscribers.emplace_back(id, std::move(fn));
        return id;
    }

    bool disconnect(Id id)
    {
        for (auto it = mSubscribers.begin(); it != mSubscribers.end(); ++it) {
            if (it->first == id) {
                mSubscribers.erase(it);
                return true;
            }
        }
        return false;
    }

    void notify(Args... args) const
    {
        const auto snapshot = mSubscribers;
        for (const auto &s : snapshot) s.second(args...);
    }

    std::size_t size() const { return mSubscribers.size(); }

private:
    std::vector<std::pair<Id, Fn>> mSubscribers;
    Id mNext = 1;
};

}   // namespace iris
