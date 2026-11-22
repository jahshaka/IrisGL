/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef IRIS_MESHMATERIAL_H
#define IRIS_MESHMATERIAL_H

#include "irisglfwd.h"

namespace iris
{
struct MeshMaterialData;

/// THE ONE CONVERSION of an IMPORTED mesh's material data into a PbrMaterial (moved down from Studio's
/// BuiltinMaterials::fromMeshData by LAB-1, 2026-10-22, so every host that builds a fragment from a bake — the app's
/// import, its thumbnails, the avatar preview and the engine-only harness `jahlab` — takes the same material): real
/// glTF metallic-roughness when the source carried it (MeshMaterialData::hasPbr), otherwise the Blinn fields through
/// the shininess remap.
PbrMaterialPtr materialFromMeshData(const MeshMaterialData &data);
}

#endif // IRIS_MESHMATERIAL_H
