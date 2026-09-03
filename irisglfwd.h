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

#include <QSharedPointer>
#include "core/logger.h"

namespace iris
{

class CameraNode;
class LightNode;
class DecalNode;
class ViewerNode;
class ParticleSystemNode;
/// Forward-declared with its underlying type so headers can take one by value
/// without dragging in particlesystemnode.h (an enumerator still needs the
/// definition — see SceneEditService::addParticleSystem, which has no default
/// argument for exactly that reason).
enum class ParticlePreset : int;
class Mesh;
class Model;
class Frustum;
class Material;
class MeshNode;
class SceneNode;
class Texture2D;
class Texture;
class Scene;
class Shader;
class VertexLayout;
class TriMesh;
class Viewport;
class DefaultMaterial;
class KeyFrameSet;
class Animation;
class FloatKeyFrame;
class CustomMaterial;
class PbrMaterial;
class PickingResult;
class Property;
class PostProcess;
class PostProcessManager;
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

typedef QSharedPointer<iris::Animation> AnimationPtr;
typedef QSharedPointer<Shader> ShaderPtr;
typedef QSharedPointer<Scene> ScenePtr;
typedef QSharedPointer<SceneNode> SceneNodePtr;
typedef QSharedPointer<Mesh> MeshPtr;
typedef QSharedPointer<Model> ModelPtr;
typedef QSharedPointer<Material> MaterialPtr;
typedef QSharedPointer<DefaultMaterial> DefaultMaterialPtr;
typedef QSharedPointer<LightNode> LightNodePtr;
typedef QSharedPointer<DecalNode> DecalNodePtr;
typedef QSharedPointer<CameraNode> CameraNodePtr;
typedef QSharedPointer<MeshNode> MeshNodePtr;
typedef QSharedPointer<Texture2D> Texture2DPtr;
typedef QSharedPointer<Texture> TexturePtr;
typedef QSharedPointer<KeyFrameSet> KeyFrameSetPtr;
typedef QSharedPointer<FloatKeyFrame> FloatKeyFramePtr;
typedef QSharedPointer<CustomMaterial> CustomMaterialPtr;
typedef QSharedPointer<PbrMaterial> PbrMaterialPtr;
typedef QSharedPointer<ViewerNode> ViewerNodePtr;
typedef QSharedPointer<ParticleSystemNode> ParticleSystemNodePtr;
typedef QSharedPointer<PostProcess> PostProcessPtr;
typedef QSharedPointer<PostProcessManager> PostProcessManagerPtr;
typedef QSharedPointer<Bone> BonePtr;
typedef QSharedPointer<Skeleton> SkeletonPtr;
typedef QSharedPointer<SkeletalAnimation> SkeletalAnimationPtr;
typedef QSharedPointer<VertexBuffer> VertexBufferPtr;
typedef QSharedPointer<IndexBuffer> IndexBufferPtr;

}

#endif // IRISGLFWD_H
