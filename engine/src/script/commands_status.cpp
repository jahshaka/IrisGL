// THE STATUS FAMILY (SCRIPT-IN-ENGINE-1 2c's split, made by 2b's last commit): engine.published /
// engine.scene.published / engine.view.published, the monitor and the status reads (2d, after GATHER-HANG-1 and
// SHADER-SYSTEM-1-7).
// Registered by registerEngineCommands; empty until its builder fills it (one file a family: the two 2c builders never
// meet in a shared list).
#include "script/commands.h"

namespace jahshaka { namespace engine { namespace script {

void registerStatusCommands(Host &, const Commands &) {}

}}}  // namespace jahshaka::engine::script
