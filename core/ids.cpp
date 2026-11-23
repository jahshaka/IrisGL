#include "core/ids.h"

#include <atomic>

namespace iris
{

namespace
{
enum Kind { Node, Mesh, Material, Texture, KindCount };
std::atomic<std::uint64_t> sNextIds[KindCount] = {{1}, {1}, {1}, {1}};   // THE counters: one object in a process

std::uint64_t draw(Kind k)
{
    return sNextIds[k].fetch_add(1, std::memory_order_relaxed);
}
}

std::uint64_t nextNodeId() { return draw(Node); }
std::uint64_t nextMeshId() { return draw(Mesh); }
std::uint64_t nextMaterialId() { return draw(Material); }
std::uint64_t nextTextureId() { return draw(Texture); }

}
