// THE MATERIAL FAMILY (SCRIPT-IN-ENGINE-1 2c's split, made by 2b's last commit): engine.material.* and
// engine.texture.* - createPbr / setPbr over the whole PbrParams, createUnlit, setNodeMaterial, setShadingModel, the
// textures (Builder A).
// Registered by registerEngineCommands; empty until its builder fills it (one file a family: the two 2c builders never
// meet in a shared list).
#include "script/commands.h"

namespace jahshaka { namespace engine { namespace script {

void registerMaterialCommands(Host &, const Commands &) {}

}}}  // namespace jahshaka::engine::script
