// THE ENGINE FAMILY's FIELD TABLES (SCRIPT-IN-ENGINE-1 stage 2): one per record records_engine.h declares, every field
// named once in declaration order (records.h). Answers only (no command takes them): their members carry no domain.
#include "script/records_engine.h"

namespace jahshaka { namespace engine { namespace script {

const Table<TextureMemoryEntry> &Record<TextureMemoryEntry>::table()
{
    using T = TextureMemoryEntry;
    static const Table<T> t({field("name", &T::name), field("resource", &T::resource), field("width", &T::width).in("px"),
                             field("height", &T::height).in("px"), field("depth", &T::depth), field("slices", &T::slices),
                             field("mipmaps", &T::mipmaps), field("msaa", &T::msaa), field("format", &T::format),
                             field("bytes", &T::bytes).in("bytes"), field("renderTarget", &T::renderTarget), field("uav", &T::uav),
                             field("manual", &T::manual), field("pooled", &T::pooled), field("residency", &T::residency)});
    return t;
}

const Table<ValidationStatus> &Record<ValidationStatus>::table()
{
    using V = ValidationStatus;
    static const Table<V> t({field("requested", &V::requested), field("active", &V::active),
                             field("layers", &V::layers).upTo(64), field("drawEntry", &V::drawEntry)});
    return t;
}

}}}  // namespace jahshaka::engine::script
