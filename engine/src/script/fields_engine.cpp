// THE ENGINE FAMILY's FIELD TABLES (SCRIPT-IN-ENGINE-1 stage 2): one per record records_engine.h declares, every field
// named once in declaration order (records.h). Answers only (no command takes them): their members carry no domain.
#include "script/records_engine.h"

namespace jahshaka { namespace engine { namespace script {

const Table<TextureMemoryEntry> &Record<TextureMemoryEntry>::table()
{
    using T = TextureMemoryEntry;
    static const Table<T> t({JAH_FIELD(T, name), JAH_FIELD(T, resource), JAH_FIELD(T, width).in("px"),
                             JAH_FIELD(T, height).in("px"), JAH_FIELD(T, depth), JAH_FIELD(T, slices),
                             JAH_FIELD(T, mipmaps), JAH_FIELD(T, msaa), JAH_FIELD(T, format),
                             JAH_FIELD(T, bytes).in("bytes"), JAH_FIELD(T, renderTarget), JAH_FIELD(T, uav),
                             JAH_FIELD(T, manual), JAH_FIELD(T, pooled), JAH_FIELD(T, residency)});
    return t;
}

const Table<ValidationStatus> &Record<ValidationStatus>::table()
{
    using V = ValidationStatus;
    static const Table<V> t({JAH_FIELD(V, requested), JAH_FIELD(V, active),
                             JAH_FIELD(V, layers).upTo(64), JAH_FIELD(V, drawEntry)});
    return t;
}

}}}  // namespace jahshaka::engine::script
