/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef IRISGLFWD_H
#define IRISGLFWD_H

/* Forward declarations of the document-model classes in irisgl.
 * The legacy GL renderer's classes were deleted at step 14.
 */

#include <memory>
#include "core/logger.h"

namespace iris
{

class CameraNode;
class LightNode;
class DecalNode;
class ParticleSystemNode;
/// Forward-declared with its underlying type so headers can take one by value
/// without dragging in particlesystemnode.h (an enumerator still needs the
/// definition — see SceneEditService::addParticleSystem, which has no default
/// argument for exactly that reason).
enum class ParticlePreset : int;
class Mesh;
class Frustum;
class Material;
class MeshNode;
class SceneNode;
class Texture2D;
class Texture;
class Scene;
class VertexLayout;
class TriMesh;
class Viewport;
class KeyFrameSet;
class Animation;
class FloatKeyFrame;
class PbrMaterial;
class PickingResult;
class Property;
class PropertyAnim;
class PropertyAnimInfo;
class FloatPropertyAnim;
class Vector3DPropertyAnim;
class ColorPropertyAnim;
class AnimableProperty;
class Bone;
class Skeleton;
class SkeletalAnimation;
template<typename T> class Key;
typedef Key<float> FloatKey;
class BoundingSphere;
class VertexBuffer;
class IndexBuffer;
class AABB;
/// The avatar movement component (AVATAR_LOCOMOTION_SPEC §6), carried by the
/// avatar WRAPPER node and by nothing else.
class AvatarMovement;
/// The locomotion state machine (AVATAR_LOCOMOTION_SPEC §7), carried by the
/// same avatar WRAPPER node. Separate from the movement component on purpose:
/// movement is physics and locomotion is animation, and the state machine
/// CONSUMES the movement's five-parameter contract rather than living inside it.
class AvatarLocomotion;

typedef std::shared_ptr<iris::Animation> AnimationPtr;
typedef std::shared_ptr<Scene> ScenePtr;
typedef std::shared_ptr<SceneNode> SceneNodePtr;
typedef std::shared_ptr<AvatarMovement> AvatarMovementPtr;
typedef std::shared_ptr<AvatarLocomotion> AvatarLocomotionPtr;

/// The scene graph's NON-owning links. The tree has exactly one ownership
/// direction — a parent owns its children — so the back-references (child to
/// parent, node to scene) are weak. They were QSharedPointer until the deep
/// audit of 2026-09 (area 3) measured what that costs: every parent/child pair
/// was a reference cycle, so no scene, no subtree and no node ever died and
/// every world the app had opened stayed resident for the life of the process.
typedef std::weak_ptr<Scene> SceneWPtr;
typedef std::weak_ptr<SceneNode> SceneNodeWPtr;
typedef std::shared_ptr<Mesh> MeshPtr;
typedef std::shared_ptr<Material> MaterialPtr;
typedef std::shared_ptr<LightNode> LightNodePtr;
typedef std::shared_ptr<DecalNode> DecalNodePtr;
typedef std::shared_ptr<CameraNode> CameraNodePtr;
typedef std::shared_ptr<MeshNode> MeshNodePtr;
typedef std::shared_ptr<Texture2D> Texture2DPtr;
typedef std::shared_ptr<KeyFrameSet> KeyFrameSetPtr;
typedef std::shared_ptr<PbrMaterial> PbrMaterialPtr;
typedef std::shared_ptr<ParticleSystemNode> ParticleSystemNodePtr;
typedef std::shared_ptr<Bone> BonePtr;
typedef std::shared_ptr<Skeleton> SkeletonPtr;
typedef std::shared_ptr<SkeletalAnimation> SkeletalAnimationPtr;
typedef std::shared_ptr<VertexBuffer> VertexBufferPtr;
typedef std::shared_ptr<IndexBuffer> IndexBufferPtr;

}

#endif // IRISGLFWD_H
