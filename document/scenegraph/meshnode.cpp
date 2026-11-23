/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/
#include "core/math/quat.h"
#include "core/math/vec.h"
#include "document/scenegraph/meshnode.h"

#include "irisglfwd.h"
#include "document/properties/property.h"
#include "document/assets/mesh.h"

#include "document/materials/material.h"
#include "document/assets/vertexlayout.h"
#include "document/animation/animableproperty.h"
#include "document/animation/animation.h"

#include "document/scenegraph/scene.h"
#include "document/scenegraph/scenenode.h"
#include "document/animation/animableproperty.h"

#include "document/assets/skeleton.h"

namespace iris
{

MeshNode::MeshNode() {
    sceneNodeType = SceneNodeType::Mesh;
    meshIndex = 0;   // was uninitialised for years and serialised as-is (platform audit B5.1)

    faceCullingMode = FaceCullingMode::DefinedInMaterial;

    // AVATAR_LOCOMOTION_SPEC §6.3 option C: a mesh is something a character can
    // walk into, unless the user says otherwise. ON here and off on every other
    // node type — this is the ONE place the default lives, so the flag reaches
    // imported models, primitives and library instances alike without any of
    // those paths knowing about it. It costs nothing until a scene with an
    // avatar in it starts playing (Environment builds the colliders lazily).
    collisionEnabled = true;
}

// @todo: cleanup previous mesh item
FaceCullingMode MeshNode::getFaceCullingMode() const
{
    return faceCullingMode;
}

void MeshNode::setFaceCullingMode(const FaceCullingMode &value)
{
    if (faceCullingMode == value) return;
    faceCullingMode = value;
    // The mirror pushes this (it is part of the material's render state), and
    // since the dirty set it only looks at a node the document says changed
    // (lead review R2 #10).
    notifyChanged(NodeChange::Params);
}

PropertyRows MeshNode::getProperties()
{
    auto props = SceneNode::getProperties();

    // meshPath/meshIndex are READ-ONLY through this reflection: writing them
    // would have to reload the mesh (and, for meshIndex, re-resolve a submesh
    // inside a model file), which is an import-time operation with asset-manager
    // side effects. setPropertyValue therefore refuses them; use setMesh().
    // There is no StringProperty in document/properties/property.h — FileProperty is
    // the QString-valued Property.
    auto pathProp = std::make_unique<FileProperty>();
    pathProp->displayName = "Mesh Path";
    pathProp->name = "meshPath";
    pathProp->value = meshPath.toStdString();
    pathProp->readOnly = true;      // matches the setPropertyValue refusal below
    props.add(std::move(pathProp));

    auto intProp = std::make_unique<IntProperty>();
    intProp->displayName = "Mesh Index";
    intProp->name = "meshIndex";
    intProp->value = meshIndex;
    intProp->readOnly = true;
    props.add(std::move(intProp));

    intProp = std::make_unique<IntProperty>();
    intProp->displayName = "Face Culling Mode";
    intProp->name = "faceCullingMode";
    intProp->value = static_cast<int>(faceCullingMode);
    props.add(std::move(intProp));

    auto boolProp = std::make_unique<BoolProperty>();
    boolProp->displayName = "Default Floor";
    boolProp->name = "defaultFloor";
    boolProp->value = defaultFloor;
    props.add(std::move(boolProp));

    // @todo: extract properties from material
    return props;
}

QVariant MeshNode::getPropertyValue(QString valueName)
{
    if (valueName == "meshPath")        return meshPath;
    if (valueName == "meshIndex")       return meshIndex;
    if (valueName == "faceCullingMode") return static_cast<int>(faceCullingMode);
    if (valueName == "defaultFloor")    return defaultFloor;

    return SceneNode::getPropertyValue(valueName);
}

bool MeshNode::setPropertyValue(QString valueName, const QVariant &value)
{
    // Read-only: see getProperties(). Returning false makes the caller report
    // "rejected property" instead of silently reloading geometry.
    if (valueName == "meshPath")  return false;
    if (valueName == "meshIndex") return false;
    if (valueName == "faceCullingMode") {
        setFaceCullingMode(static_cast<FaceCullingMode>(value.toInt()));
        return markedParams();
    }
    if (valueName == "defaultFloor") { defaultFloor = value.toBool(); return markedParams(); }

    return SceneNode::setPropertyValue(valueName, value);
}

// (`setMesh(QString)` is DELETED — ATOM P2, 2026-09-22. It called
// `Mesh::loadMesh`, i.e. it PARSED a model file inside a document setter, and the
// only things that reached it were the built-ins: a shipped resource path per
// primitive. Those are baked library assets now and the caller resolves the asset
// (jahshaka/src/services/primitiveassets.h), so a mesh arrives here as a MeshPtr
// like every imported model's does, and `meshPath` is set by whoever knows what
// reference the document should store.)
void MeshNode::setMesh(MeshPtr mesh)
{
    const bool hadMesh = !!this->mesh;
    const bool hadSkeleton = !!skeleton;
    this->mesh = mesh;
    adoptSkeletonFromMesh();
    // CONTENT (DIRTY_SET_MIRROR_SPEC §3.1). Nothing reported a mesh or
    // material swap before the dirty set — the mirror noticed one by comparing
    // the pointers it kept, once per node per frame, forever.
    notifyChanged(NodeChange::Content);
    // THE CLASSIFICATION FOLLOWS THE CONTENT (STAGE-CLOSE-TRIAGE-1 P1). Static
    // eligibility depends on the mesh and its skeleton (isStaticEligible), so
    // when either presence flips the node is re-derived HERE, before the mirror
    // attaches the new geometry: a node that sat static with no mesh (or with a
    // static mesh) and now carries a skeleton is DEMOTED — the engine builds a
    // skinned Item SCENE_DYNAMIC and Ogre's attachObject throws on a static node,
    // leaving the Item half-attached (an import's bake landing on a placed node,
    // a reimport that turns a model skinned) — and a node that just became
    // eligible is classified the way a placement classifies it
    // (applyStaticDefaults). Only for a node in a scene: a reader building a
    // subtree classifies the whole load once at its end.
    // The demotion is a CLASSIFICATION switch (graph::setStatic), not rule 4's
    // transform-write demotion — but _applyStaticHint(false) marks the node
    // (Flags), so the mirror's settle stamps it and owes ONE rotation
    // (SceneMirror::noteSettleWrite's cheap kind test passes a Mesh). That
    // rotation applies the whole rule and leaves the node dynamic: a skinned or
    // meshless node is never eligible to be promoted back. One walk, no loop.
    const bool presenceFlipped =
        hadMesh != !!this->mesh || hadSkeleton != !!skeleton;
    if (presenceFlipped && getScene()) {
        if (!isStaticEligible()) {
            if (isStaticInGraph()) _applyStaticHint(false);
        } else {
            applyStaticDefaults();
        }
    }
}

// GPU_SKINNING_SPEC §7. The mesh asset's skeleton is the rig template and is
// SHARED (createDuplicate hands the duplicate the same MeshPtr); pose state on
// it means two avatars of one rig fight over one boneTransforms array and the
// last writer per frame wins. Every MeshNode gets its own clone here — which
// is also what makes createDuplicate correct for free, since it goes through
// setMesh.
void MeshNode::adoptSkeletonFromMesh()
{
    if (!!mesh && mesh->hasSkeleton())
        skeleton = mesh->getSkeleton()->clone();
    else
        skeleton.reset();
}

MeshPtr MeshNode::getMesh()
{
    return mesh;
}

void MeshNode::setMaterial(MaterialPtr material)
{
    this->material = material;
    notifyChanged(NodeChange::Content);

	if (!!mesh) {
		if (this->mesh->hasSkeleton()) {
			material->enableFlag("SKINNING_ENABLED");
		}
		else {
			material->disableFlag("SKINNING_ENABLED");
		}
	}
}

float MeshNode::getMeshRadius()
{
    const iris::Mat4 world = getGlobalTransform();
    float scaleX = world.column(0).toVector3D().length();
    float scaleY = world.column(1).toVector3D().length();
    float scaleZ = world.column(2).toVector3D().length();

    return qMax(qMax(scaleX, scaleY), scaleZ);
}

BoundingSphere MeshNode::getTransformedBoundingSphere()
{
    BoundingSphere boundingSphere;
    boundingSphere.pos = getGlobalTransform() * mesh->boundingSphere.pos;
    boundingSphere.radius = mesh->boundingSphere.radius * getMeshRadius();
    return boundingSphere;
}

SceneNodePtr MeshNode::createDuplicate()
{
    auto node = MeshNode::create();

    // @todo: pass duplicates instead of copies!!!!!!!!
    node->setMesh(this->getMesh());
    node->meshPath = this->meshPath;
    node->meshIndex = this->meshIndex;
    // A mesh node without a material exists (synthetic nodes, partially-built
    // imports) — duplicating one crashed here (found by the Stage 2 lane).
    // The copy gets its OWN material, equal to this one in every value
    // (Material::duplicate says why a copy and not the same pointer).
    if (this->material) node->setMaterial(this->material->duplicate());
    // Saved and exported per node (scenewriter, the glTF double-sided flag), so
    // a copy that dropped it would save and export differently from its
    // original.
    node->faceCullingMode = this->faceCullingMode;

    // The animations are cloned by SceneNode::duplicateInto for EVERY node
    // type (CLIP-REF-1); this used to hand a mesh copy the SAME Animation
    // objects, and every other node type none at all.

    // Sockets are per-node authoring (see meshnode.h), so a duplicate carries
    // its own copy of the list — a second character has the same head socket
    // and does not share it.
    node->setSockets(this->sockets);

    // `defaultFloor` is deliberately NOT copied (meshnode.h): the copy of a
    // floor is an ordinary mesh.

    return node;
}

bool MeshNode::hasBone(const QString &boneName) const
{
    if (!skeleton || boneName.isEmpty()) return false;
    return skeleton->boneMap.contains(boneName);
}

const Socket *MeshNode::findSocket(const QString &socketName) const
{
    for (const Socket &socket : sockets)
        if (socket.name == socketName) return &socket;
    return nullptr;
}

bool MeshNode::addSocket(const Socket &socket, QString *error)
{
    const auto refuse = [error](const QString &message) {
        if (error) *error = message;
        return false;
    };
    if (socket.name.isEmpty()) return refuse(QStringLiteral("a socket needs a name"));
    if (findSocket(socket.name))
        return refuse(QStringLiteral("this node already has a socket named '%1'").arg(socket.name));
    if (!skeleton || skeleton->bones.isEmpty())
        return refuse(QStringLiteral("'%1' has no rig — sockets attach to BONES, so only a "
                                     "skinned mesh can carry one").arg(name));
    if (!hasBone(socket.boneName))
        return refuse(QStringLiteral("'%1' has no bone named '%2'").arg(name, socket.boneName));

    sockets.append(socket);
    return true;
}

bool MeshNode::removeSocket(const QString &socketName)
{
    for (int i = 0; i < sockets.size(); ++i) {
        if (sockets[i].name == socketName) {
            sockets.removeAt(i);
            return true;
        }
    }
    return false;
}

}
