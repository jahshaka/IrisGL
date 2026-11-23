#include "core/ids.h"

#include <atomic>

namespace iris
{

namespace
{
std::atomic<std::uint64_t> sNextNodeId{1};   // THE counter: one object in a process (core/ids.h says why)
}

std::uint64_t nextNodeId()
{
    return sNextNodeId.fetch_add(1, std::memory_order_relaxed);
}

}
