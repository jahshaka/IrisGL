/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include "core/geometry/trianglebvh.h"
#include "core/geometry/trimesh.h"

#include <algorithm>
#include <cmath>

namespace iris
{

namespace {

constexpr int kLeafSize = 4;
/// The pad, relative to the scene's scale (see the header): ~170 float ulps.
constexpr float kRelativePad = 1.0e-5f;

struct Box
{
    float lo[3] = {  INFINITY,  INFINITY,  INFINITY };
    float hi[3] = { -INFINITY, -INFINITY, -INFINITY };
    void grow(const float p[3])
    {
        for (int k = 0; k < 3; ++k) {
            lo[k] = std::min(lo[k], p[k]);
            hi[k] = std::max(hi[k], p[k]);
        }
    }
    void grow(const Box &b)
    {
        for (int k = 0; k < 3; ++k) {
            lo[k] = std::min(lo[k], b.lo[k]);
            hi[k] = std::max(hi[k], b.hi[k]);
        }
    }
};

}  // namespace

void TriangleBvh::clear()
{
    mNodes.clear();
    mNodes.shrink_to_fit();
    mOrder.clear();
    mOrder.shrink_to_fit();
    mScale = 0.0f;
}

void TriangleBvh::build(const Triangle *triangles, int count)
{
    clear();
    if (!triangles || count <= 0) return;

    // Per triangle: its exact box and its centroid (the split key).
    std::vector<Box> boxes(static_cast<size_t>(count));
    std::vector<float> centroids(size_t(count) * 3u);
    Box all;
    for (int i = 0; i < count; ++i) {
        const Triangle &t = triangles[i];
        const float pa[3] = { t.a.x(), t.a.y(), t.a.z() };
        const float pb[3] = { t.b.x(), t.b.y(), t.b.z() };
        const float pc[3] = { t.c.x(), t.c.y(), t.c.z() };
        for (int k = 0; k < 3; ++k)
            if (!std::isfinite(pa[k]) || !std::isfinite(pb[k]) || !std::isfinite(pc[k]))
                return;   // see the header: the brute-force loop keeps this mesh
        Box &b = boxes[size_t(i)];
        b.grow(pa); b.grow(pb); b.grow(pc);
        for (int k = 0; k < 3; ++k) centroids[size_t(i) * 3u + size_t(k)] = 0.5f * (b.lo[k] + b.hi[k]);
        all.grow(b);
    }
    float scale = 0.0f;
    for (int k = 0; k < 3; ++k)
        scale = std::max({ scale, std::fabs(all.lo[k]), std::fabs(all.hi[k]), all.hi[k] - all.lo[k] });
    mScale = scale;

    mOrder.resize(size_t(count));
    for (int i = 0; i < count; ++i) mOrder[size_t(i)] = i;
    mNodes.reserve(size_t(2 * (count / kLeafSize + 1)));

    // Depth-first, left child laid out right after its parent; the right
    // child's index is patched in when the left subtree is done.
    struct Task { int node; int begin; int end; };
    std::vector<Task> stack;
    stack.reserve(64);
    mNodes.emplace_back();
    stack.push_back({ 0, 0, count });
    while (!stack.empty()) {
        const Task task = stack.back();
        stack.pop_back();
        Box box, cbox;
        for (int s = task.begin; s < task.end; ++s) {
            const int tri = mOrder[size_t(s)];
            box.grow(boxes[size_t(tri)]);
            cbox.grow(&centroids[size_t(tri) * 3u]);
        }
        Node &node = mNodes[size_t(task.node)];
        for (int k = 0; k < 3; ++k) { node.lo[k] = box.lo[k]; node.hi[k] = box.hi[k]; }
        const int n = task.end - task.begin;
        int axis = 0;
        for (int k = 1; k < 3; ++k)
            if (cbox.hi[k] - cbox.lo[k] > cbox.hi[axis] - cbox.lo[axis]) axis = k;
        // A leaf when small, or when every centroid coincides (no split can
        // separate them — a stack of identical triangles).
        if (n <= kLeafSize || !(cbox.hi[axis] > cbox.lo[axis])) {
            node.first = task.begin;
            node.count = n;
            continue;
        }
        const int mid = task.begin + n / 2;
        const auto key = [&](int tri) { return centroids[size_t(tri) * 3u + size_t(axis)]; };
        std::nth_element(mOrder.begin() + task.begin, mOrder.begin() + mid, mOrder.begin() + task.end,
                         [&](int l, int r) { return key(l) < key(r) || (key(l) == key(r) && l < r); });
        // THE TWO CHILDREN ARE ALLOCATED TOGETHER (left, left + 1), so an
        // inner node names only the first. `node` is not used past this point:
        // emplace_back may move the array.
        node.count = 0;
        const int left = int(mNodes.size());
        node.first = left;
        mNodes.emplace_back();
        mNodes.emplace_back();
        stack.push_back({ left + 1, mid, task.end });
        stack.push_back({ left, task.begin, mid });
    }
}

void TriangleBvh::segmentCandidates(const iris::Vec3 &a, const iris::Vec3 &b, std::vector<int> &out) const
{
    out.clear();
    if (mNodes.empty()) return;
    const float pa[3] = { a.x(), a.y(), a.z() };
    const float d[3] = { b.x() - a.x(), b.y() - a.y(), b.z() - a.z() };
    float segScale = mScale;
    for (int k = 0; k < 3; ++k) segScale = std::max({ segScale, std::fabs(pa[k]), std::fabs(b[k]) });
    const float pad = kRelativePad * segScale;

    int stack[128];
    int top = 0;
    stack[top++] = 0;
    while (top > 0) {
        const Node &node = mNodes[size_t(stack[--top])];
        // The segment against the padded box: the slab test over t in [0, 1].
        float t0 = 0.0f, t1 = 1.0f;
        bool miss = false;
        for (int k = 0; k < 3 && !miss; ++k) {
            const float lo = node.lo[k] - pad, hi = node.hi[k] + pad;
            if (d[k] == 0.0f) {
                if (pa[k] < lo || pa[k] > hi) miss = true;
                continue;
            }
            float ta = (lo - pa[k]) / d[k], tb = (hi - pa[k]) / d[k];
            if (ta > tb) std::swap(ta, tb);
            t0 = std::max(t0, ta);
            t1 = std::min(t1, tb);
            if (t0 > t1) miss = true;
        }
        if (miss) continue;
        if (node.count > 0) {
            for (int s = 0; s < node.count; ++s) out.push_back(mOrder[size_t(node.first + s)]);
            continue;
        }
        // An inner node: its two children were allocated together. The stack
        // holds at most depth + 1 entries and a median split over 2^31
        // triangles is 30 levels deep, so 128 cannot overflow; asserted anyway
        // as an answer of EVERY triangle, never a silent drop.
        if (top + 2 > int(sizeof(stack) / sizeof(stack[0]))) {
            out.resize(mOrder.size());
            for (size_t i = 0; i < mOrder.size(); ++i) out[i] = int(i);
            return;
        }
        stack[top++] = node.first + 1;
        stack[top++] = node.first;
    }
    std::sort(out.begin(), out.end());
}

}
