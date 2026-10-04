/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef TRIANGLEBVH_H
#define TRIANGLEBVH_H

#include "core/math/vec.h"

#include <vector>

namespace iris
{

class Triangle;

/**
 * THE PICKING MESH'S SPATIAL INDEX (SPEED-CPU, perf audit 2026-10-03 P1).
 *
 * A bounding-volume hierarchy over a TriMesh's triangles. It answers ONE
 * question: which triangles could a segment touch? — and the TriMesh then runs
 * its own exact triangle test on exactly those, in ascending triangle order. So
 * the index never decides a hit; it only removes triangles whose boxes the
 * segment cannot reach, and the hits, their order and their numbers are the
 * brute-force loop's by construction.
 *
 * CONSERVATIVE BY CONSTRUCTION. Every box is the exact min/max of its
 * triangles' corners, and the segment test is done against that box grown by a
 * relative pad (1e-5 of the scene's scale: hundreds of float ulps), so a
 * triangle the exact test accepts can never sit outside its box by a rounding
 * error. A mesh with a non-finite corner builds NO index (the brute-force loop
 * keeps answering for it — a NaN box would compare false and drop triangles).
 *
 * Built once per picking mesh where the mesh is read (the bake reader), never
 * stored: the bake's rule for the picking mesh ("rebuilt, not stored") holds
 * for its index too. Median split on the longest centroid axis, four triangles
 * a leaf, deterministic (ties broken by triangle index).
 */
class TriangleBvh
{
public:
    /// Builds over `count` triangles. Replaces any previous index; leaves the
    /// index empty (isBuilt() false) when count is 0 or a corner is not finite.
    void build(const Triangle *triangles, int count);
    void clear();

    bool isBuilt() const { return !mNodes.empty(); }
    /// The triangle count the index was built over — a TriMesh whose list has
    /// since changed size must not trust it.
    int triangleCount() const { return int(mOrder.size()); }

    /// Every triangle whose padded box the segment a->b touches, ASCENDING.
    /// `out` is cleared first.
    void segmentCandidates(const iris::Vec3 &a, const iris::Vec3 &b, std::vector<int> &out) const;

    /// Node count (for tests and for the cost readout).
    int nodeCount() const { return int(mNodes.size()); }

private:
    struct Node
    {
        float lo[3] = {};
        float hi[3] = {};
        int first = 0;   ///< leaf: first slot in mOrder; inner: the LEFT child (the right is first + 1)
        int count = 0;   ///< leaf: triangle count (> 0); inner: 0
    };
    std::vector<Node> mNodes;
    std::vector<int> mOrder;
    float mScale = 0.0f;   ///< max |coordinate| + extent: the pad's yardstick
};

}

#endif // TRIANGLEBVH_H
