/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef LIGHTNODE_H
#define LIGHTNODE_H

#include "core/math/vec.h"
#include "QColor"
#include "irisglfwd.h"
#include "document/scenegraph/scenenode.h"
#include "document/scenegraph/shadowmap.h"

namespace iris
{

class ShadowMap;

enum class LightType:int
{
    Point = 0,
    Directional = 1,
    Spot = 2,
    Area = 3,       // rectangular area light (engine viewport only; legacy ignores it)
};

class LightNode:public SceneNode
{

public:
    iris::Vec3 lightDir;

    LightType lightType;

    ShadowMap* shadowMap;

    /**
     * light's radius. This is only used for pointlights.
     */
    float distance;
    QColor color;
    float intensity;

	/*
	Shadow's color and trasnsparency
	*/
	QColor shadowColor;
	float shadowAlpha;

    /**
     * Spotlight cutoff HALF angle in degrees: the angle between the light's
     * axis and the edge of its cone. The editor's cone wire is drawn from it
     * (radius = range * tan(spotCutOff)) and the renderer doubles it to get
     * the full apex angle Ogre wants. Range 1..85.
     */
    float spotCutOff;

    /**
     * Spotlight's softness, 0..1. The bright core is the outer cone with this
     * fraction taken off it: inner = outer * (1 - softness). 0 is a hard edge,
     * 1 is all penumbra and no core.
     *
     * It defaulted to 1.0 on this 0..1 parameter until LIGHTING_FIX fix 5 —
     * i.e. every spot ever created had a 1%-wide bright core and was penumbra
     * everywhere else, which is why spots read as vague smudges.
     */
    float spotCutOffSoftness;

    /**
     * The penumbra's falloff exponent (Ogre's `falloff`): 1 = linear across the
     * penumbra, higher concentrates the light towards the core. Never
     * authorable before LIGHTING_FIX fix 5, which is why it reads 1.0 in every
     * document written before it.
     */
    float spotFalloff;

    /**
     * Area-light rectangle dimensions in world units (LightType::Area only).
     * The rectangle lies across the light's local X (width) and Z (height)
     * axes and emits down -Y, like every other light direction here.
     */
    float rectWidth;
    float rectHeight;

    /** Area light: emit from both faces of the rectangle. */
    bool doubleSided;

    /**
     * IES photometric profile bound to this light — the LIBRARY asset's guid
     * (empty = none), plus the absolute file path the host resolved it to and
     * the profile's own peak candela scale.
     *
     * `iesProfilePath` and `iesNormalisation` are RUNTIME state: whoever binds
     * the guid (the scene reader, the property panel, a script verb) resolves
     * both from the asset store, and only the guid is serialized. The
     * normalisation is the profile's peak `candela/1024 * multiplier * ballast
     * factors` — the renderer multiplies a light's attenuation by exactly that
     * term, so dividing intensity by it makes assigning a profile change the
     * SHAPE of the falloff without changing the brightness.
     *
     * Renderer limits, which the panel repeats to the user: spot lights always
     * honour a profile, point lights honour it only while they cast no shadows,
     * and directional/area lights never do.
     */
    QString iesProfileGuid;
    QString iesProfilePath;
    float   iesNormalisation;

    /**
     * Area light: mask/gobo image bound to this light — the LIBRARY asset's
     * guid (empty = none) plus the host-resolved absolute path (runtime only,
     * like iesProfilePath).
     *
     * Honoured by the fast approximation only: the `accurate` (LTC) path has no
     * mask term and silently ignores the texture.
     */
    QString lightTextureGuid;
    QString lightTexturePath;

    /**
     * Area light: physically accurate mode (linearly transformed cosines)
     * instead of the cheaper approximation. Slower; no textured-light support.
     */
    bool accurate;

    //editor-specific
    QSharedPointer<Texture2D> icon;
    float iconSize;

    static LightNodePtr create()
    {
        return LightNodePtr(new LightNode());
    }

    void setLightType(LightType type)
    {
        this->lightType = type;
    }

    LightType getLightType()
    {
        return lightType;
    }

    iris::Vec3 getLightDir()
    {
        // this is the default rotation for directional and spotlights - pointing down
        iris::Vec4 defaultDir(0, -1, 0, 0);

        iris::Vec4 dir = (getGlobalTransform() * defaultDir);

        return dir.toVector3D();
    }

    virtual QList<Property*> getProperties() override;
    virtual QVariant getPropertyValue(QString valueName) override;
    virtual bool setPropertyValue(QString valueName, const QVariant &value) override;

    void updateAnimation(float time) override;

	ShadowMap* getShadowMap()
	{
		return shadowMap;
	}

	void setShadowMapType(ShadowMapType shadowType)
	{
		shadowMap->shadowType = shadowType;
	}

	ShadowMapType getShadowMapType()
	{
		return shadowMap->shadowType;
	}

	void setShadowMapResolution(int size)
	{
		shadowMap->setResolution(size);
	}

	int getShadowMapResolution()
	{
		return shadowMap->resolution;
	}

	SceneNodePtr createDuplicate() override;

private:
    LightNode();
};


}

#endif // LIGHTNODE_H
