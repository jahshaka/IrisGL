#pragma once
// THE REFLECTION REST's RECORD ALLOWANCE — A FUNCTION OF THE VIEW's TARGET, NEVER OF HOW LARGE ITS HIT LIST ONCE GREW
// (URGENT #157, TEST-STREAMLINE-1).
//
// At rest the reflection's record runs (rq_reflect.comp) share the hit list among the texels still converging: each
// takes at most `share` rays a frame (the tier's rest rays), and no more than the list holds per batching texel. The
// list GROWS when a frame overflows it and is never shrunk within a scene (OgreView::sizeHitList), so a capacity read
// from the list as it grew made a rest's rays a texel - its sample sequence, and the picture it holds - a function of
// what once overflowed it: another scene's demand, a larger target an hour ago. THE LAW: the allowance reads the
// TARGET's CREATION capacity (kHitListCreateShare x W x H records, OgreView::hitListCreateRows), capped by the list as
// built (a graph built before its target grew, or the rq.hitListCap arm, holds fewer). No Ogre here: a pure function,
// unit-tested (tests/engine test_hit_list_law.cpp).

namespace jahshaka::engine::detail {

/// The rays a batching texel may append this rest frame: min(share, min(listRecords, createdRecords) / batching);
/// 0 while nothing is batching (no read-back has counted the texels yet).
inline unsigned restRecordAllowance(unsigned share, unsigned long long listRecords, unsigned long long createdRecords,
                                    unsigned long long batching)
{
    if (batching == 0ull) return 0u;
    const unsigned long long capacity = listRecords < createdRecords ? listRecords : createdRecords;
    const unsigned long long perTexel = capacity / batching;
    return perTexel < share ? unsigned(perTexel) : share;
}

}   // namespace jahshaka::engine::detail
