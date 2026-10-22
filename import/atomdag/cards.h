/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef IRIS_IMPORT_ATOMDAG_CARDS_H
#define IRIS_IMPORT_ATOMDAG_CARDS_H

// ATOM-2 D9 — FOLIAGE'S AGGREGATE (ours; Nanite's answer is an experimental voxel path, dossier 1.9). A group whose
// input is mostly CARDS — triangles of islands under kCardTriangles (the card-soup class: Bistro's cypress, the Linde) —
// is not collapsed: neighbouring cards are MERGED.
//
// THE SETS: in the input's order, every card not yet taken seeds a set of itself and its nearest untaken cards whose
// centres lie within E of the seed's (at most kCardsPerSet). Cards holding a locked position (the group's external
// border) are never merged.
// THE QUAD of a set: in the plane through the sources' area-weighted centre, its normal the sources' area-weighted
// normal (each triangle's turned to the seed's side — cards are two-sided); its axes the SEED's (the set's largest
// card: its own in-plane frame, projected onto the plane); its aspect the seed's UV rectangle's in that frame and its
// AREA THE SOURCES' SUMMED AREA (coverage kept) — shrunk about its centre only as far as keeps every corner inside the
// mesh's level-0 box (the mesh's box never grows; an overhang past the sources is priced like any deviation). Its UVs: the seed's rectangle corners under the seed's own affine map
// from its frame to its first UV set (no new texture); every other buffer copies the seed's vertex nearest each corner.
// THE PRICE: the set's sources against its quad, MEASURED both ways (measure.h, exact: every source vertex and facet
// against the quad, the quad's facets against the sources; the quad stands for its sources' islands, each vertex
// charged at most its own island's extent). A set past E drops its farthest card and is priced again; a seed whose
// pair is past E stays as it is. The group's step = the largest price of its merged sets (the rest is unchanged).
// THE VERTICES: each quad is FOUR NEW VERTICES appended to the mesh (build.cpp; the owner's memory rule decides the
// stage) — the only vertices the DAG ever adds.

#include "import/atomdag/adjacency.h"
#include "import/atomdag/simplify.h"

#include <vector>

namespace iris
{
namespace atomdag
{

constexpr int kCardTriangles = 12;   ///< an island under this many level-0 triangles is a card
constexpr int kCardsPerSet = 4;      ///< a quad stands for at most four cards: a canopy quarters per budget doubling

/// One appended corner: its local id is space.n + index; `source` = the scratch vertex whose other buffers it copies.
struct CardCorner
{
    float pos[3] = { 0.0f, 0.0f, 0.0f };
    float nrm[3] = { 0.0f, 0.0f, 1.0f };
    float uv[2] = { 0.0f, 0.0f };
    unsigned source = 0;
    float diagonal = 0.0f;   ///< its quad's diagonal (the new island's extent)
};

struct CardMerge
{
    std::vector<unsigned> out;        ///< the output in local ids (>= the scratch's size: a corner)
    std::vector<CardCorner> corners;  ///< four per merged set
    float step = 0.0f;                ///< the largest measured price of a merged set
    int sets = 0;                     ///< sets merged
    int cardsMerged = 0;
    int pricings = 0;                 ///< sets measured (the attempts)
    int shrunk = 0;                   ///< quads shrunk to stay inside their sources' box
    double sourceArea = 0.0, quadArea = 0.0;   ///< summed over the merged sets (coverage)
    float maxCentreDistance = 0.0f;   ///< the largest seed-to-card centre distance merged
};

/// Whether more than half the input's triangles belong to cards (`islandTriangles` = per island its triangles).
bool isCardGroup(const GroupScratch &sc, const LocalSpace &space, const std::vector<size_t> &islandTriangles);

/// The merge of the scratch's cards under the budget `budget`; `lock` flags the locked scratch vertices; every quad
/// stays inside [boxLo, boxHi] (the mesh's level-0 box).
CardMerge mergeCards(const GroupScratch &sc, const LocalSpace &space, const std::vector<unsigned char> &lock,
                     float budget, float floorLen, const Vec3 &boxLo, const Vec3 &boxHi);

}   // namespace atomdag
}   // namespace iris

#endif   // IRIS_IMPORT_ATOMDAG_CARDS_H
