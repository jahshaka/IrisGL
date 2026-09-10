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

namespace iris
{

// THE canonical assimp post-process preset (ASSET_PIPELINE_SPEC §3.2.2), as
// OPAQUE INTEGERS. This header is public and includes nothing of assimp's
// (ENGINEERING_DEBT L4 part 3, 2026-09-10: assimp is a PRIVATE dependency of
// IrisGL — no public IrisGL header includes it and Studio's include path does
// not carry it). The values are composed from assimp's own enums in ONE place,
// import/importflags.cpp, and every ReadFile inside IrisGL passes one of
// these two. Outside IrisGL nothing calls ReadFile at all: Studio goes through
// the import facade (import/scenesource.h, import/modelsceneinfo.h,
// import/clipfileinfo.h, MeshNode::loadAsSceneFragment) — the white-box test
// suites that drive the vendored importer directly link assimp themselves and
// pass these same constants, so "one definition" still holds.
//
// Import, every subsequent load, and metadata extraction all pass this same
// flag set, so the geometry the import preview/thumbnail saw IS the geometry
// every scene load produces. Before this header, import used the Quality
// preset while the nine load sites used Fast (faceted GenNormals, no cache
// or degenerate cleanup) and metadata used Triangulate only — same source,
// three different results depending on who asked.
//
// Never add a ReadFile call with a raw preset again: reference these.
//
// UNITS (the FBX unit-scale defect, owner report 2026-09-08 "Dreyar is
// massively huge"). Jahshaka's world is METRES — one document unit is one
// metre, everywhere: the avatar room is 10 m across with a 4 m ceiling, a
// light's range is in metres, gravity is -9.81. An FBX file declares its own unit in
// GlobalSettings::UnitScaleFactor (centimetres per unit: 1 = cm, 100 = m), and
// assimp converts it for you ONLY when aiProcess_GlobalScale is in the flags —
// FBXImporter::InternReadFile calls SetFileScale(UnitScaleFactor * 0.01) and
// ScaleProcess is what consumes it. Without the flag the factor is parsed and
// dropped, so every Mixamo download (UnitScaleFactor 1, i.e. centimetres)
// imported ONE HUNDRED TIMES too large: 178.37 units for a 1.784 m character.
//
// Which formats this touches, at the pinned assimp (v6.0.5): FBX ONLY.
// `SetFileScale` has exactly one call site in the whole library
// (code/AssetLib/FBX/FBXImporter.cpp), and ScaleProcess::Execute returns
// immediately when the resulting scale is 1.0 — so glTF/GLB, OBJ, PLY, STL and
// BVH come out BIT-IDENTICAL to what they produced before the flag existed
// (glTF has no unit factor: its spec fixes the metre). Collada carries a
// <unit meter=> and applies it itself, inside ColladaLoader, with or without
// the flag. The GLB fixtures' AABBs are asserted unchanged in tests/importer.
//
// NO MIGRATION (ships-as-new-app law): a scene saved BEFORE this flag holds
// node transforms that were authored against the 100x geometry, and nothing
// rescales them. Re-import is the honest path. The bake fingerprint below
// makes the change self-invalidating rather than silent.
struct ImportFlags
{
    // Quality = CalcTangentSpace | GenSmoothNormals | JoinIdenticalVertices |
    // ImproveCacheLocality | LimitBoneWeights | RemoveRedundantMaterials |
    // SplitLargeMeshes | Triangulate | GenUVCoords | SortByPType |
    // FindDegenerates | FindInvalidData — plus GlobalScale.
    //
    // GlobalScale is part of the CANONICAL set, not an option: it is the file's
    // own declaration of what its numbers mean, and a load site that skipped it
    // would produce a different-sized model from the one the import preview,
    // the thumbnail and the bake saw — the exact three-different-results bug
    // this header was created to end. It also rides the .jmb bake fingerprint
    // (MeshBake::producerId hashes this value), so every bake produced before
    // the flag is rejected and rebuilt instead of silently serving 100x
    // geometry to a build that no longer parses it that way.
    static const unsigned int Canonical;

    // For the sites that read a file for its NAMES and NUMBERS only — the
    // animation-clip parsers, which want channels, not geometry (avatar
    // loadAnimation / loadClip: no post-processing, measured 3x faster, and
    // node/channel names come out identical either way).
    //
    // It is NOT zero, and that is the point: a clip's translation keys are in
    // the FILE's units exactly like its vertices are, so a clip parsed without
    // the unit factor drives a rig that WAS parsed with it at 100x — an
    // exploded skeleton. ScaleProcess scales position keys, bone offset
    // matrices and node transforms and touches nothing else, so this stays a
    // names-and-numbers read.
    static const unsigned int ClipNamesOnly;
};

} // namespace iris

#endif // IRIS_IMPORTFLAGS_H
