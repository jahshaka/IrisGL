// THE PROCESS's ONE COUNTER PER KIND OF ID (NOQT-DOCUMENT-1 stage 6, the lead's rulings 2026-11-23): nodes, meshes,
// materials and textures. The ids the document, the mirror and the engine hand out come from ONE counter per kind in a
// process, on every platform: the counters are defined in libIrisCore alone, as the guid engine and the temp serial are.
// (A function-local static of an INLINE function is one object only where the platform merges it - GNU-unique binding
// on ELF, never under -Bsymbolic - and one per DLL on Windows.) The engine's reserveNodeId / reserveMeshId /
// reserveMaterialId / reserveTextureId (engine/include/jahshaka/engine/Ids.h) forward here.
#pragma once

#include "core/export.h"

#include <cstdint>

namespace iris
{

/// The next id of each kind: 1, 2, 3 ... never 0 (0 is "none"), never reused. Thread-safe (one relaxed atomic add); the
/// kinds count independently.
IRIS_CORE_API std::uint64_t nextNodeId();
IRIS_CORE_API std::uint64_t nextMeshId();
IRIS_CORE_API std::uint64_t nextMaterialId();
IRIS_CORE_API std::uint64_t nextTextureId();

}
