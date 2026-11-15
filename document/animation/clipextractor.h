/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef IRIS_CLIPEXTRACTOR_H
#define IRIS_CLIPEXTRACTOR_H

#include "core/math/quat.h"
#include "core/math/vec.h"
#include "irisglfwd.h"

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

namespace iris
{

/// One key of one bone track: ABSOLUTE TRS, LOCAL TO THE PARENT BONE, in
/// SECONDS. A root bone's frame is the mesh node the rig deforms (the same
/// frame SceneMirror::toSkeletonDesc authors the bind pose in).
struct ClipBoneKey
{
    float       time = 0.0f;
    iris::Vec3   position;
    iris::Quat rotation;
    iris::Vec3   scale{1, 1, 1};
};

/// One bone's track. `bone` indexes Skeleton::bones.
struct ClipBoneTrack
{
    int                  bone = -1;
    QString              boneName;
    QVector<ClipBoneKey> keys;
};

/// A clip translated out of the document's scene-node channels into per-bone,
/// bone-parent-local tracks — the only shape an engine skeleton can consume.
struct ExtractedClip
{
    QString                 name;
    float                   length = 0.0f;   ///< seconds; 0 for a single-key clip
    QVector<ClipBoneTrack>  tracks;

    /// Diagnostics (ANIMATION_ENGINE_MIGRATION_SPEC §9 R9 asked for these to be
    /// measured, not guessed): how many document keys the clip carried on the
    /// nodes that feed these bones, and how many keys came out.
    int sourceKeyCount  = 0;
    int emittedKeyCount = 0;

    /// Bones whose AUTHORED REST local differs from the rig's BIND local.
    ///
    /// It matters because of an asymmetry between the two evaluators: the
    /// document composes every unanimated bone from its authored REST
    /// transform, while an engine skeleton resets an untracked bone to its
    /// BIND pose. For every file we have (and for every Mixamo export) the two
    /// coincide, because the exporter writes the bind pose as the rest pose.
    /// When they do not, a bone a clip does not touch lands somewhere else than
    /// the document put it — so this is reported rather than silently absorbed.
    QStringList restDiffersFromBind;
};

/// "Compose then resample" (ANIMATION_ENGINE_MIGRATION_SPEC §3.1).
///
/// THE PROBLEM. Our clip keys are absolute local TRS of a SCENE NODE, and in a
/// pivot-preserving FBX a bone's motion is distributed across its `$AssimpFbx$`
/// pivot ancestors — 46 of the 52 channels in a measured Mixamo walk target
/// those, not bones. An engine skeleton has only real bones. So renaming
/// `BoneAnimation` to a bone track per key is right for every glTF and wrong
/// for every pivot-preserving FBX: it produces a frozen character, and it is
/// invisible to a test suite whose fixtures are all glTF.
///
/// THE ALGORITHM, per bone `b` with rig parent `p`:
///   1. the CHANNEL SET is every scene node on the chain `(p, b]` that the clip
///      animates (for a root bone the chain runs from the mesh node's frame);
///   2. `T` = the union of those channels' key times;
///   3. at each `t ∈ T` every node on the chain is evaluated with the DOCUMENT
///      EVALUATOR'S OWN SEMANTICS — the clip's key if the node has a channel,
///      the node's authored local otherwise — and the chain composed;
///   4. the composition is decomposed to TRS and emitted as one key.
///
/// Lossy in principle (a rotation split across two pivots composes to something
/// a slerp between the originals does not reproduce mid-interval); this is the
/// standard FBX pivot bake, and it is why every parity gate downstream is a
/// tolerance and never bit-equality.
class ClipExtractor
{
public:
    struct RestLocal
    {
        iris::Vec3   pos;
        iris::Quat rot;
        iris::Vec3   scale{1, 1, 1};
    };
    /// The authored local TRS of every node of a subtree, by node POINTER.
    using RestPose = QHash<const SceneNode *, RestLocal>;

    /// Snapshots the subtree's current local transforms. Take this ONCE, while
    /// the tree is at rest — after the document evaluator has run, a node the
    /// last clip moved is no longer at its authored transform.
    static RestPose captureRest(const SceneNodePtr &root);

    /// Translates one clip. `root` is the subtree the clip drives (the fragment
    /// root), `meshNode` the skinned node whose `rig` this is — it defines the
    /// frame of the root bones, exactly as SceneMirror::toSkeletonDesc does.
    ///
    /// `rest` may be null, in which case the subtree's CURRENT locals are used
    /// (correct only if nothing has posed it yet).
    ///
    /// Only bones the clip actually drives get a track: coverage is the input
    /// to per-bone blend-weight normalization downstream, so inventing tracks
    /// for untouched bones would quietly destroy it.
    static bool extract(const SceneNodePtr &root,
                        const SceneNodePtr &meshNode,
                        const SkeletonPtr &rig,
                        const SkeletalAnimationPtr &clip,
                        const QString &clipName,
                        float length,
                        const RestPose *rest,
                        ExtractedClip &out,
                        QString *error = nullptr);
};

} // namespace iris

#endif // IRIS_CLIPEXTRACTOR_H
