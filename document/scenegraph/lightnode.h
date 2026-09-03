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
    QVector3D lightDir;

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
     * Spotlight cutoff angle in degrees.
     * This parameter is only used if the light is a spotlight
     */
    float spotCutOff;

    /**
     * Spotlight's softness
     * This is added to the spotlight's outer radius to give more
     * smooth cutoff edges
     */
    float spotCutOffSoftness;

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

    QVector3D getLightDir()
    {
        // this is the default rotation for directional and spotlights - pointing down
        QVector4D defaultDir(0, -1, 0, 0);

        QVector4D dir = (globalTransform * defaultDir);

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
