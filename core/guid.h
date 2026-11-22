// iris::newGuid - THE ONE GUID GENERATOR (NOQT-CORE-1 stage 1; V2 plan 0.12), in place of six copies of QUuid's.
// The text form is the one every stored key already has - QUuid::toString() without its braces: 36 characters, lowercase
// hex, dashes at 8/13/18/23 (8-4-4-4-12) - and a guid is never parsed, only compared as text (library keys, scene node
// guids, sidecar file names): its TEXT is the contract. A random (version 4, RFC 4122 variant) id.
#pragma once

#include "core/export.h"

#include <string>

namespace iris {

/// A new random version-4 guid as 36-character lowercase text, "xxxxxxxx-xxxx-4xxx-[89ab]xxx-xxxxxxxxxxxx".
IRIS_CORE_API std::string newGuid();

}   // namespace iris
