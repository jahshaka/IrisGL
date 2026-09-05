/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef DECALNODE_H
#define DECALNODE_H

#include "core/math/vec.h"
#include <QString>

#include "irisglfwd.h"
#include "document/scenegraph/scenenode.h"

namespace iris
{

/**
 * A projected-texture decal (DECALS_SPEC.md §5.1): an oriented box that
 * overwrites base colour / roughness / metalness on every surface inside it.
 *
 * CONVENTIONS, both of which the engine relies on and neither of which is
 * negotiable without changing the shader:
 *
 *  - The decal PROJECTS DOWN ITS LOCAL -Y, exactly like a LightNode shines
 *    down its local -Y (LightNode::getLightDir). Surfaces whose normal points
 *    back up at the decal are affected; surfaces facing away are masked out.
 *    Unlike lights, decals need no internal orientation adapter: Ogre's decal
 *    half-space test reads the node's +Y row of the inverse world matrix
 *    directly (ForwardPlus_DecalsCubemaps_piece_ps.any).
 *  - The projected image's U axis is LOCAL X and its V axis is LOCAL Z
 *    (`decalUV = localPos.xz + 0.5`), so `width` is the local-X extent and
 *    `height` the local-Z extent. `depth` is the local-Y thickness of the
 *    projector box — how far the decal reaches along the projection axis.
 *
 * The numeric width/height/depth size an internal engine child node; the
 * document node's own scale (the scale gizmo) MULTIPLIES them, so both ways of
 * shaping the projector volume compose (DECALS_SPEC D4).
 */
class DecalNode : public SceneNode
{
public:
    /// Diffuse / base-colour image asset. The decal's identity: with no
    /// texture the node still exists (wire box, no projection).
    QString textureGuid;
    /// Filled by the reader / the edit service from the CAS, like material
    /// maps. The engine loads bytes from this path.
    QString resolvedTexturePath;

    /// Phase 3: normal and emissive maps, each in its own pooled atlas.
    /// Serialized from phase 1 so old files keep loading forward-compatibly.
    QString normalGuid;
    QString resolvedNormalPath;
    QString emissiveGuid;
    QString resolvedEmissivePath;

    float width  = 1.0f;    ///< local X extent, world units
    float height = 1.0f;    ///< local Z extent, world units (UV = XZ)
    float depth  = 0.5f;    ///< local Y extent = projection thickness

    /// Ogre's Decal defaults metalness to 1.0; a sticker is not chrome, so the
    /// document default is 0.
    float metalness = 0.0f;
    float roughness = 1.0f;

    /// When true the diffuse alpha masks the base colour only, leaving the
    /// normal/emissive contributions unmasked. Only meaningful once a normal
    /// or emissive map is bound (phase 3).
    bool ignoreAlphaDiffuse = false;

    static DecalNodePtr create()
    {
        return DecalNodePtr(new DecalNode());
    }

    /// The projection direction in world space: the node's local -Y, the same
    /// convention as LightNode::getLightDir.
    iris::Vec3 getProjectionDir()
    {
        iris::Vec4 dir = (getGlobalTransform() * iris::Vec4(0, -1, 0, 0));
        return dir.toVector3D();
    }

    virtual QList<Property*> getProperties() override;
    virtual QVariant getPropertyValue(QString valueName) override;
    virtual bool setPropertyValue(QString valueName, const QVariant &value) override;

    void updateAnimation(float time) override;

    SceneNodePtr createDuplicate() override;

private:
    DecalNode();
};

}

#endif // DECALNODE_H
