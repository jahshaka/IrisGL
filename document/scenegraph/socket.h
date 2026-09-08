/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef SOCKET_H
#define SOCKET_H

// Sockets — named attach points on a RIG (CAMERAS_SPEC §5, owner decision D9).
//
// A socket is {name, boneName, offset} on a skinned MeshNode. Any node — a
// camera, a prop, a light — can be ATTACHED to one, and then its world
// transform is driven every frame from `boneWorld * offset`. That is the whole
// idea: a camera at the `head` socket of an animated character is first person,
// a camera at `shoulder` is third person, and a sword at `hand` is a sword in a
// hand.
//
// WHERE THE POSE COMES FROM. The document does not compute one any more: clip
// evaluation lives in the engine (ANIMATION_ENGINE_MIGRATION_SPEC) and the pose
// is read back per node with Scene::bonePoses, which SceneMirror turns into
// world matrices. So this file takes the pose as an INJECTED SOURCE
// (BonePoseSource) rather than reaching for one, which has two consequences
// worth stating:
//
//   * the socket maths is testable with no engine at all — hand a resolver a
//     source that returns matrices you wrote down, and check where the attached
//     node lands;
//   * with NO source installed (headless document runs, the reader, an offscreen
//     document-only viewport) sockets still resolve, at the rig's BIND POSE,
//     through bindPoseWorldTransforms below. A socketed camera in a headless run
//     is therefore in a defined place instead of wherever it was last saved.
//
// ONE FRAME OF LAG is by design and unavoidable: the pose read back is the one
// the last rendered frame produced, so SceneMirror resolves sockets at the top
// of sync() — read the pose the engine just computed, move the attached nodes,
// then push everything. Nothing here re-renders to close that gap.
//
// FAIL SOFT, ALWAYS. A stale attachment (the owner was deleted, the socket
// was removed, the bone was renamed by a re-import) does not move its node and
// does not raise: it is reported by the verbs (node.sockets, node.info) and the
// node simply stays where it is. A hard failure here would mean a re-imported
// character breaks every scene that used it.

#include <QHash>
#include <QList>
#include <QString>
#include <functional>

#include "core/math/mat4.h"
#include "core/math/quat.h"
#include "core/math/vec.h"
#include "irisglfwd.h"

namespace iris
{

/// A named attach point on one bone of a skinned MeshNode.
///
/// The offset is stored as TRS rather than as a matrix on purpose: it is what
/// the file writes, what the verbs accept, and what a properties panel would
/// edit. `offsetMatrix()` is the derived form.
struct Socket
{
    QString name;          ///< unique within its owner; never empty
    QString boneName;      ///< a bone of the owner's skeleton
    Vec3    position;      ///< offset from the bone, in BONE space
    Quat    rotation;
    Vec3    scale = Vec3(1, 1, 1);
    /// True for a socket the avatar module installed from its bone-name table
    /// rather than one the user authored. Round-trips; nothing behaves
    /// differently, it is provenance for the UI and the verbs.
    bool    builtIn = false;

    Mat4 offsetMatrix() const
    {
        Mat4 m;
        m.setToIdentity();
        m.translate(position);
        m.rotate(rotation);
        m.scale(scale);
        return m;
    }
};

/// "Give me every bone of this node, in WORLD space, by name." False means the
/// node has no pose available right now (not skinned, not mirrored yet, the
/// engine has not rendered a frame) — the caller then falls back to the bind
/// pose rather than moving anything to the origin.
///
/// The QHash is an OUT parameter and is expected to be reused between calls:
/// this runs per frame, and a fresh container per skinned node per frame is
/// exactly the allocation churn the resolver exists to avoid.
using BonePoseSource = std::function<bool(MeshNode *, QHash<QString, Mat4> &)>;

/// The rig's BIND pose in world space — FK-free, because a v1 skeleton already
/// carries each bone's mesh-space bind matrix. Equal by construction to what
/// the engine reports for an un-posed node (SceneMirror::toSkeletonDesc derives
/// the engine's bind locals from exactly these matrices).
///
/// False when the node is not a skinned mesh.
bool bindPoseWorldTransforms(MeshNode *node, QHash<QString, Mat4> &out);

/// The world transform of one socket: `boneWorld * socket.offset`.
/// False when the owner has no socket of that name, or the socket names a bone
/// the pose does not have.
bool socketWorldTransform(MeshNode *owner, const QString &socketName,
                          const BonePoseSource &source, Mat4 &out);

/// Drives every socket-attached node of a scene, once per frame.
///
/// Owning an object rather than being a free function is deliberate: the bone
/// map is a scratch buffer that lives across frames (cleared, never
/// reallocated), and the attachment walk groups by owner so a rig's pose is
/// read ONCE however many things hang off it.
class SocketResolver
{
public:
    /// Where poses come from. Unset (the default) means "bind pose only",
    /// which is what a document-only host gets.
    void setPoseSource(BonePoseSource source) { mSource = std::move(source); }
    bool hasPoseSource() const { return bool(mSource); }

    /// Moves every attached node of `scene` onto its socket. Returns how many
    /// nodes were moved; attachments that do not resolve are skipped silently
    /// (see the fail-soft note at the top of this file).
    ///
    /// ONE PASS, in no particular owner order. A chain (a socketed prop that is
    /// itself a rig with sockets of its own) therefore settles over successive
    /// frames rather than within one — which is the same one-frame behaviour the
    /// pose read-back already has, and not worth a topological sort for.
    int resolve(Scene *scene);

    /// The number of attachments that did NOT resolve on the last resolve()
    /// call — a diagnostic for the verbs and the suites, never a control flow.
    int lastDangling() const { return mDangling; }

private:
    BonePoseSource mSource;
    QHash<QString, Mat4> mBones;    ///< scratch, reused every frame
    int mDangling = 0;
};

}   // namespace iris

#endif // SOCKET_H
