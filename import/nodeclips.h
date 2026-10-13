/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef IRIS_IMPORT_NODECLIPS_H
#define IRIS_IMPORT_NODECLIPS_H

#include "irisglfwd.h"

namespace iris
{

/// A FILE'S NODE ANIMATION PLAYS (BISTRO-IMPORT-1 stage 3b). assimp hands every
/// aiAnimation over as one SkeletalAnimation on the fragment ROOT, its channels matched
/// to scene nodes by name — and a skeletal channel only ever plays through an engine
/// SKELETON. A channel that keys a node no rig owns (the Bistro Exterior's 13 lanterns
/// and string lights, a shot camera's pivots) therefore never played, while its node
/// counted as moving.
///
/// So, over the import's fragment: every MOVING channel (BoneAnimation::moves) whose
/// target is not a bone of a rig in this fragment becomes that node's own PROPERTY
/// animation — position / rotation (Euler degrees, unwrapped key to key) / scale tracks
/// built from the channel's keys, looping, the node's active animation when it came out
/// of the root's active clip — which SceneNode::updateAnimation plays and which makes the
/// node movable for a true reason (resolveMobility: `animation`). A clip that keys no rig
/// at all then has nothing left to do on the root and is removed from it; a clip that
/// drives a rig stays on the root for the engine's skeleton, as it always did.
struct PlayedNodeChannels
{
    int nodes = 0;          ///< nodes given a property animation
    int clipsRemoved = 0;   ///< root clips that keyed no rig
};
PlayedNodeChannels playNodeChannels(const SceneNodePtr &fragment);

}   // namespace iris

#endif   // IRIS_IMPORT_NODECLIPS_H
