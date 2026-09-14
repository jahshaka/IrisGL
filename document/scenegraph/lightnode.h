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
    // THE SKY LIGHT (SKY_LIGHT_SPEC.md §2, owner decision D14). Unreal's model:
    // the scene's SKY is the environment picture, and a Sky Light is the LIGHT
    // that reads it and fills the scene with its diffuse ambient. It has no
    // position, no direction, no range and casts no shadow — `color` is a TINT
    // on the sky's own integral and `intensity` is the strength (1.0 = the sky
    // at full physical strength). Every other LightNode field is meaningless on
    // it and the panel hides them.
    //
    // THE FIRST VISIBLE ONE IS THE SKYLIGHT (Scene::skyLight(), the same shape
    // as sunLight()); a second raises the `sky.duplicate` scene issue. NO Sky
    // Light in a scene = no ambient at all: 27 zero SH coefficients, which is a
    // black ambient term, which is the decided behaviour (a visible sky that
    // does not light, as in Unreal).
    Sky = 4,
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

    /// THE SUN'S ANGULAR DIAMETER in degrees (SKY_LIGHT_SPEC.md §3; Unreal's
    /// "Source Angle"). The real sun subtends 0.53 degrees, which is the
    /// default. It sizes the sun DISC drawn in the sky where this light points
    /// — and it is the row a future soft-shadow penumbra would read, which is
    /// why it is a property of the light and not of the disc.
    ///
    /// Only meaningful on the scene's SUN (the first directional light): a
    /// secondary directional draws no disc, and the panel only shows the row
    /// on the sun.
    float sunAngle = 0.53f;

    /// FOLLOWS ATMOSPHERE (SUN_FOLLOWS_ATMOSPHERE, owner 2026-09-14 — Unreal's
    /// Sun Sky does the same thing). ON by default.
    ///
    /// While the scene's sky is the ANALYTIC atmosphere, the sun's direct light
    /// is tinted by what the air does to sunlight at the sun's current
    /// elevation: the colour the user picked is the NOON value, and a low sun
    /// arrives reddened and dimmed — from the sky model itself
    /// (Scene::atmosphereSunTint), so the disc, the sky and the light agree.
    /// Off is the old behaviour: the picked colour at every elevation.
    ///
    /// AT NIGHT THE SUN IS DARK. Below the horizon the tint is zero to every
    /// decimal a frame can hold, so the mirror also drops the sun's SHADOW (three
    /// full-frustum PSSM passes a frame) and its disc while it is down.
    ///
    /// INERT ON EVERY OTHER SKY. A photograph, a gradient or a picked colour
    /// knows nothing about the air, so the tint is white and the row says so.
    /// Meaningful on the scene's SUN only, like sunAngle above.
    ///
    /// A document written before this reads TRUE (the reader's absent-key
    /// answer and this default agree — the trap SceneReader's own header
    /// records).
    bool followsAtmosphere = true;

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
     * FORWARD SHADING PRIORITY — DIRECTIONAL LIGHTS ONLY (SUN_AND_LIGHT_DEFAULTS
     * owner decision Q1). The lowest number in the scene is THE SUN; every other
     * directional light is a secondary one and casts no shadow (our shadow node
     * has exactly one directional slot, OgreShadow.cpp).
     *
     * "Sun" is a UI ROLE, not a type and not a second stored field (owner Q1e):
     * the document stores this number and NOTHING ELSE, and "which light is the
     * sun" is DERIVED by the one resolver, `Scene::sunLight()`. The three
     * resolvers that each had their own rule (the sky link's depth-first walk,
     * the GI bounce light's lowest-nodeId scan and Ogre's castShadows-then-id
     * sort) are gone — they are what made two lights able to disagree about
     * which one was the sun with nobody being told.
     *
     * AUTOMATIC: the first directional light in a scene gets 0, the next the
     * lowest free number (Scene::nextForwardShadingPriority), and the author
     * can change it. Read on no other light type; -1 is not a value (0 is the
     * winner and the default, exactly as Unreal's row reads).
     */
    int forwardShadingPriority;

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
        if (lightType == type) return;
        lightType = type;
        notifyChanged(NodeChange::Params);
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
		if (shadowMap->shadowType == shadowType) return;
		shadowMap->shadowType = shadowType;
		notifyChanged(NodeChange::Params);
	}

	ShadowMapType getShadowMapType()
	{
		return shadowMap->shadowType;
	}

	void setShadowMapResolution(int size)
	{
		shadowMap->setResolution(size);
		notifyChanged(NodeChange::Params);
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
