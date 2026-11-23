// THE GI READERS' FAMILY (SCRIPT-IN-ENGINE-1 stage 2): engine.gi.* reads - readCardAt, readCardRadiance, the voxel
// stats / volume / light / direct / halves, the changed regions, traceRays - each `reads`, answering its record, the
// big arrays as `bytes` under the call's byte bound (records_gi_read.h, fields_gi_read.cpp). Registered by
// makeEngineCommands (commands_engine.cpp); made EMPTY by E0 (the batch's base): builder P fills it.
#include "script/commands.h"

namespace jahshaka { namespace engine { namespace script {

void registerGiReadCommands(Host &, const Commands &) {}

}}}  // namespace jahshaka::engine::script
