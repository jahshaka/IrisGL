/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef IRIS_IMPORTFLAGS_H
#define IRIS_IMPORTFLAGS_H

#include "assimp/postprocess.h"

namespace iris
{

// THE canonical assimp post-process preset (ASSET_PIPELINE_SPEC §3.2.2).
//
// Import, every subsequent load, and metadata extraction all pass this same
// flag set, so the geometry the import preview/thumbnail saw IS the geometry
// every scene load produces. Before this header, import used the Quality
// preset while the nine load sites used Fast (faceted GenNormals, no cache
// or degenerate cleanup) and metadata used Triangulate only — same source,
// three different results depending on who asked.
//
// Never add a ReadFile call with a raw preset again: reference these.
struct ImportFlags
{
    // Quality = CalcTangentSpace | GenSmoothNormals | JoinIdenticalVertices |
    // ImproveCacheLocality | LimitBoneWeights | RemoveRedundantMaterials |
    // SplitLargeMeshes | Triangulate | GenUVCoords | SortByPType |
    // FindDegenerates | FindInvalidData.
    static constexpr unsigned int Canonical = aiProcessPreset_TargetRealtime_Quality;
};

} // namespace iris

#endif // IRIS_IMPORTFLAGS_H
