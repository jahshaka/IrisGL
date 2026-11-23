// THE PROCESS's ONE NODE-ID COUNTER (NOQT-DOCUMENT-1 stage 6, the lead's ruling 2026-11-23). The ids the document, the
// mirror and the engine give their nodes come from ONE counter in a process, on every platform: it is defined in
// libIrisCore alone, as the guid engine and the temp serial are. (A function-local static of an INLINE function is one
// object only where the platform merges it - GNU-unique binding on ELF, never under -Bsymbolic - and one per DLL on
// Windows.) The engine's jahshaka::engine::reserveNodeId() (engine/include/jahshaka/engine/Ids.h) forwards here.
#pragma once

#include "core/export.h"

#include <cstdint>

namespace iris
{

/// The next node id: 1, 2, 3 ... never 0 (0 is "none"), never reused. Thread-safe (one relaxed atomic add).
IRIS_CORE_API std::uint64_t nextNodeId();

}
