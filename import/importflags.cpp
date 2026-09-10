/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include "import/importflags.h"

#include "assimp/postprocess.h"

namespace iris
{

// THE one place assimp's post-process enums are named (import/importflags.h
// has the rationale for each set). The values are what the header's comment
// says they are; MeshBake::producerId hashes Canonical, so changing either
// line invalidates every existing bake — on purpose.
const unsigned int ImportFlags::Canonical =
    aiProcessPreset_TargetRealtime_Quality | aiProcess_GlobalScale;

const unsigned int ImportFlags::ClipNamesOnly = aiProcess_GlobalScale;

} // namespace iris
