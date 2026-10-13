/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef NODEDIRTYSET_H
#define NODEDIRTYSET_H

#include <cstddef>
#include <vector>

namespace iris
{

class SceneNode;

// -----------------------------------------------------------------------------
// iris::NodeDirtySet — WHAT CHANGED SINCE THE LAST FRAME.
//
// SPECS/DIRTY_SET_MIRROR_SPEC.md (option A, owner 2026-09-13). The mirror used
// to ask every object in the document "did you change?" once a frame: 8,403
// objects x ~1.6 us = ~13 ms of a 16 ms budget with nothing moving. This is the
// other side of that question — each object SAYS SO when it changes, into the
// list its scene owns, and the mirror handles the list instead of the scene.
//
// It is deliberately tiny and deliberately NOT a Qt container: `append` is on
// the path of every transform write in the program (a physics step writes one
// per body per step), so it has to be a push_back onto a vector whose capacity
// the previous frame already paid for. The dedupe is the NODE's (a `queued`
// bit beside its mask — see SceneNode::notifyChanged), so a node appears here
// at most once per frame however many times it is written.
//
// TWO LISTS, and the order they are consumed in matters:
//
//  * `dirty`   — nodes whose state moved. Consumed by SceneMirror::sync().
//  * `evicted` — the RAW pointers of nodes that left the document. They are
//    never dereferenced (the mirror's entry map is keyed by pointer and
//    releaseEntry reads only the entry), and they are processed BEFORE the
//    dirty list so that an address a same-frame insert recycled is released
//    before it is adopted again.
//
// THE OVERFLOW VALVE. A scene with no mirror consuming it still collects: a
// preview scene that builds and drops fragments all session would grow the
// eviction list forever. Past `kEvictionCap` the list is dropped whole and the
// set says so — a consumer that sees `takeOverflow()` owes one FULL walk (and
// its stamp sweep), which is exactly the answer that cannot be wrong.
// -----------------------------------------------------------------------------
class NodeDirtySet
{
public:
    /// How many evictions are held before the list is dropped for a full walk.
    static const std::size_t kEvictionCap = 4096;

    /// Appended by SceneNode::notifyChanged on the node's 0->queued transition.
    /// Returns the slot the node landed in — the node remembers it so that a
    /// node which LEAVES the document before the list is consumed can be
    /// cancelled in O(1) (see cancel).
    std::size_t append(SceneNode *node)
    {
        mDirty.push_back(node);
        return mDirty.size() - 1;
    }

    /// TOMBSTONES a queued node. A node marked and then deleted in the same
    /// frame would otherwise be a dangling pointer in the list — the mirror
    /// must dereference a dirty node (it reads its fields), unlike an evicted
    /// one (whose pointer is only ever a map key).
    ///
    /// THE NODE IS PASSED AND CHECKED (lead review R2 #5). A slot number means
    /// nothing once the list has been SWAPPED away: the consumer holds the old
    /// vector and this one is fresh, so a removal in that window would write a
    /// tombstone over a DIFFERENT node's slot — leaving that node silently
    /// unmarked until the next full walk — while the pointer it meant to cancel
    /// stayed live in the consumer's copy. The window is empty today (the mirror
    /// clears each node's mask before it visits it, on the same thread), which
    /// is exactly why it must be closed here rather than relied upon.
    void cancel(std::size_t slot, const SceneNode *node)
    {
        if (slot < mDirty.size() && mDirty[slot] == node) mDirty[slot] = nullptr;
    }

    /// A node that has LEFT the document (removeChildInternal / removeFromScene).
    /// The pointer is recorded, never followed.
    void evict(SceneNode *node)
    {
        noteChange(true);
        if (mEvicted.size() >= kEvictionCap) {
            mEvicted.clear();
            mOverflowed = true;
            return;
        }
        mEvicted.push_back(node);
    }

    /// Hands the whole list over by SWAP — one vector move, no allocation, and
    /// a mark raised DURING consumption lands in the fresh list for next frame.
    void takeDirty(std::vector<SceneNode *> &out) { out.clear(); out.swap(mDirty); }
    void takeEvicted(std::vector<SceneNode *> &out) { out.clear(); out.swap(mEvicted); }
    /// True once per overflow; reading it clears it.
    bool takeOverflow() { const bool o = mOverflowed; mOverflowed = false; return o; }

    /// THE CHANGE SERIALS — "did anything change since I last looked", for a
    /// reader that is NOT the mirror (the list above has one consumer, and it
    /// swaps the list away every frame). Bumped by EVERY SceneNode::notifyChanged
    /// call — before the per-frame dedupe, so a second write to a node the mirror
    /// has not consumed yet still moves them — and by every join and eviction.
    /// Two of them, because the readers differ in what they care about:
    /// `changeSerial` moves for any change; `contentSerial` only for a change of
    /// what a node HOLDS — Structure, Content (a mesh or material swapped),
    /// Params — never for a transform, a visibility, a flag or a name (an
    /// animated lantern or a physics body writes every frame, and its first
    /// write demotes it from static: neither changes a material or a texture,
    /// and a reader of those must not re-walk 6,000 nodes for them). Monotonic.
    /// (BISTRO-ENGINE-1 stage 5: the scene-issue scanner walked the whole scene
    /// once a second at idle — 0.2 s of UI thread per second at 6,000 meshes.)
    void noteChange(bool content)
    {
        ++mChangeSerial;
        if (content) ++mContentSerial;
    }
    unsigned long long changeSerial() const { return mChangeSerial; }
    unsigned long long contentSerial() const { return mContentSerial; }
    /// Diagnostics only (the document suite asserts on these).
    std::size_t pendingDirty() const { return mDirty.size(); }
    std::size_t pendingEvicted() const { return mEvicted.size(); }

    /// Forgets everything without visiting it — what a FULL walk leaves behind,
    /// since the walk covered every node the list could have named. The nodes'
    /// own masks are cleared by the walk (SceneNode::_takeDirtyMask).
    void clear()
    {
        mDirty.clear();
        mEvicted.clear();
        mOverflowed = false;
    }

private:
    std::vector<SceneNode *> mDirty;
    std::vector<SceneNode *> mEvicted;
    bool mOverflowed = false;
    unsigned long long mChangeSerial = 0;
    unsigned long long mContentSerial = 0;
};

}

#endif // NODEDIRTYSET_H
