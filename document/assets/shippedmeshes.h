/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef IRIS_SHIPPEDMESHES_H
#define IRIS_SHIPPEDMESHES_H

// THE APP'S OWN MESHES, BY THEIR SEED KEY (SHIPPED-BAKES-1).
//
// Every mesh the app SHIPS and draws — the twelve primitives, the samples'
// Ground and Teapot, a preview dock's subject, the avatar room's cube, the VR
// controller models — is BAKED ONCE, by the one import pipeline, when a library
// is opened (Studio's services/primitiveassets.h: the seed list is
// src/data/primitives.h). Nothing parses one at run time any more: assimp is an
// IMPORT-time dependency, and `source.assimp_import_only` keeps it so.
//
// This is the seam a library-less consumer reads one through. IrisGL has no
// catalog and no store, so the HOST installs the resolver (Studio: the baked
// seed rows, once the library is seeded); IrisGL's own consumers — the mirror's
// VR controller slot — and Studio's preview docks ask here by the seed key the
// table names (":/content/primitives/hp_sphere.obj", ...). No resolver, or a key
// with no bake behind it, answers null: the caller draws nothing (or its own
// fallback, the wand), exactly as it did when a parse failed.
//
// THREADS: the resolver reads the catalog, whose connections are per-thread, so
// `mesh` is a UI-thread call. The installed function itself is guarded.

#include <QString>
#include <functional>

#include "irisglfwd.h"

namespace iris
{
namespace ShippedMeshes
{

using Resolver = std::function<MeshPtr(const QString &seedKey)>;

/// Install (or, with an empty function, remove) the host's resolver.
void setResolver(Resolver resolver);

/// True while a resolver is installed.
bool hasResolver();

/// The baked mesh for `seedKey`, or null.
MeshPtr mesh(const QString &seedKey);

}   // namespace ShippedMeshes
}   // namespace iris

#endif   // IRIS_SHIPPEDMESHES_H
