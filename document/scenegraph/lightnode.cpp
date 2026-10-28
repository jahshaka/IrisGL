#include "document/scenegraph/lightnode.h"
#include "document/scenegraph/scenenode.h"
#include "document/animation/animableproperty.h"
#include "document/animation/keyframeset.h"
#include "document/animation/animation.h"
#include "document/animation/propertyanim.h"
#include "core/properties/property.h"
#include "document/scenegraph/shadowmap.h"
#include <algorithm>

namespace iris
{

PropertyRows LightNode::getProperties()
{
    auto props = SceneNode::getProperties();

    auto colorProp = std::make_unique<ColorProperty>();
    colorProp->displayName = "Light Color";
    colorProp->name = "lightColor";
    colorProp->value = color;
    props.add(std::move(colorProp));

    auto prop = std::make_unique<FloatProperty>();
    prop->displayName = "Intensity";
    prop->name = "intensity";
    prop->value = intensity;
    props.add(std::move(prop));

    prop = std::make_unique<FloatProperty>();
    prop->displayName = "Range";
    prop->name = "distance";
    prop->value = distance;
    props.add(std::move(prop));

    prop = std::make_unique<FloatProperty>();
    prop->displayName = "Source Radius";
    prop->name = "sourceRadius";
    prop->value = sourceRadius;
    props.add(std::move(prop));

    prop = std::make_unique<FloatProperty>();
    prop->displayName = "Spot CutOff";
    prop->name = "spotCutOff";
    prop->value = spotCutOff;
    props.add(std::move(prop));

    prop = std::make_unique<FloatProperty>();
    prop->displayName = "Spot CutOff Softness";
    prop->name = "spotCutOffSoftness";
    prop->value = spotCutOffSoftness;
    props.add(std::move(prop));

    prop = std::make_unique<FloatProperty>();
    prop->displayName = "Spot Falloff";
    prop->name = "spotFalloff";
    prop->value = spotFalloff;
    props.add(std::move(prop));

    prop = std::make_unique<FloatProperty>();
    prop->displayName = "Rect Width";
    prop->name = "rectWidth";
    prop->value = rectWidth;
    props.add(std::move(prop));

    prop = std::make_unique<FloatProperty>();
    prop->displayName = "Rect Height";
    prop->name = "rectHeight";
    prop->value = rectHeight;
    props.add(std::move(prop));

    prop = std::make_unique<FloatProperty>();
    prop->displayName = "Icon Size";
    prop->name = "iconSize";
    prop->value = iconSize;
    props.add(std::move(prop));

    // (THREE PROPERTIES ARE GONE — `shadowAlpha`, `shadowColor` and
    // `shadowBias`, render audit I-6, CRUD law. All three were advertised,
    // serialized, reflected and panel-bound, and NONE of them reached the
    // renderer: the mirror never read a shadow alpha or colour — a shadow is
    // the absence of light, not a tinted overlay — and the depth bias is the
    // engine's own per-shadow-node constant.)
    auto intProp = std::make_unique<IntProperty>();
    intProp->displayName = "Light Type";
    intProp->name = "lightType";
    intProp->value = static_cast<int>(lightType);
    props.add(std::move(intProp));

    intProp = std::make_unique<IntProperty>();
    intProp->displayName = "Shadow Map Type";
    intProp->name = "shadowMapType";
    intProp->value = static_cast<int>(shadowMap.shadowType);
    props.add(std::move(intProp));

    intProp = std::make_unique<IntProperty>();
    intProp->displayName = "Shadow Map Resolution";
    intProp->name = "shadowMapResolution";
    intProp->value = shadowMap.resolution;
    props.add(std::move(intProp));

    // FOLLOWS ATMOSPHERE (the sun only; lightnode.h says what it means).
    auto atmoProp = std::make_unique<BoolProperty>();
    atmoProp->displayName = "Follows Atmosphere";
    atmoProp->name = "followsAtmosphere";
    atmoProp->value = followsAtmosphere;
    props.add(std::move(atmoProp));

    // FORWARD SHADING PRIORITY — the slot this light takes in the renderer's
    // shadow-casting order, and which directional IS the sun (0 wins).
    // getPropertyValue/setPropertyValue have carried it since it landed, and the
    // light panel's row is bound through them; only the LIST was missing, so
    // `node.properties(id)` never mentioned it and a model reading the surface
    // could not discover the one row that decides which light is the sun
    // (MIRROR_SCALE lane, 2026-09-13 — an API-first discovery gap, not a
    // wiring one).
    intProp = std::make_unique<IntProperty>();
    intProp->displayName = "Forward Shading Priority";
    intProp->name = "forwardShadingPriority";
    intProp->value = forwardShadingPriority;
    props.add(std::move(intProp));

    auto boolProp = std::make_unique<BoolProperty>();
    boolProp->displayName = "Double Sided";
    boolProp->name = "doubleSided";
    boolProp->value = doubleSided;
    props.add(std::move(boolProp));

    return props;
}

QVariant LightNode::getPropertyValue(QString valueName)
{
    if(valueName == "intensity")
        return intensity;
    if(valueName == "lightColor")
        return color;
    if(valueName == "distance")
        return distance;
    if(valueName == "sourceRadius")
        return sourceRadius;
    if(valueName == "spotCutOff")
        return spotCutOff;
    if(valueName == "spotCutOffSoftness")
        return spotCutOffSoftness;
    if(valueName == "spotFalloff")
        return spotFalloff;
    if(valueName == "rectWidth")
        return rectWidth;
    if(valueName == "rectHeight")
        return rectHeight;
    if(valueName == "lightType")
        return static_cast<int>(lightType);
    if(valueName == "shadowMapType")
        return static_cast<int>(getShadowMapType());
    if(valueName == "shadowMapResolution")
        return getShadowMapResolution();
    if(valueName == "doubleSided")
        return doubleSided;
    if(valueName == "forwardShadingPriority")
        return forwardShadingPriority;
    if(valueName == "followsAtmosphere")
        return followsAtmosphere;
    // The asset BINDING (the IES profile) is reflected read-only-ish: the guid is the
    // document's state, the resolved path and the normalisation are the host's
    // (they come from the asset store, which irisgl cannot reach).
    if(valueName == "iesProfile")
        return iesProfileGuid;
    if(valueName == "iesProfilePath")
        return iesProfilePath;
    if(valueName == "iesNormalisation")
        return iesNormalisation;
    if(valueName == "iconSize")
        return iconSize;
    if(valueName == "skySource")
        return static_cast<int>(skySource);

    return SceneNode::getPropertyValue(valueName);
}

bool LightNode::setPropertyValue(QString valueName, const QVariant &value)
{
    if (valueName == "intensity")         { intensity = value.toFloat();         return markedParams(); }
    if (valueName == "lightColor")        { color = value.value<QColor>();       return markedParams(); }
    if (valueName == "distance")          { distance = value.toFloat();          return markedParams(); }
    if (valueName == "sourceRadius")      { sourceRadius = std::max(value.toFloat(), 0.001f); return markedParams(); }
    if (valueName == "spotCutOff")        { spotCutOff = value.toFloat();        return markedParams(); }
    if (valueName == "spotCutOffSoftness"){ spotCutOffSoftness = value.toFloat();return markedParams(); }
    if (valueName == "spotFalloff")       { spotFalloff = value.toFloat();       return markedParams(); }
    if (valueName == "rectWidth")         { rectWidth = value.toFloat();         return markedParams(); }
    if (valueName == "rectHeight")        { rectHeight = value.toFloat();        return markedParams(); }
    if (valueName == "lightType")         { setLightType(static_cast<LightType>(value.toInt())); return markedParams(); }
    // The shadow-map fields live on the node's ShadowMap; reflect through it.
    // (a filter only: no value means "no shadow" - ShadowMapType's header; a value outside the three is refused)
    if (valueName == "shadowMapType") {
        const int t = value.toInt();
        if (t < int(ShadowMapType::Hard) || t > int(ShadowMapType::VerySoft)) return false;
        setShadowMapType(static_cast<ShadowMapType>(t));
        return markedParams();
    }
    if (valueName == "shadowMapResolution"){ setShadowMapResolution(value.toInt()); return markedParams(); }
    if (valueName == "doubleSided")       { doubleSided = value.toBool();        return markedParams(); }
    if (valueName == "forwardShadingPriority") { forwardShadingPriority = qMax(0, value.toInt()); return markedParams(); }
    // The sun's direct light is tinted by the atmosphere at its own elevation
    // (lightnode.h). A LIGHT change, so the shadow and probe caches see it
    // through the same staleness path a colour edit already travels.
    if (valueName == "followsAtmosphere") { followsAtmosphere = value.toBool(); return markedParams(); }
    if (valueName == "iesProfile")        { iesProfileGuid = value.toString();   return markedParams(); }
    if (valueName == "iesProfilePath")    { iesProfilePath = value.toString();   return markedParams(); }
    if (valueName == "iesNormalisation")  { iesNormalisation = value.toFloat();  return markedParams(); }
    if (valueName == "iconSize")          { iconSize = value.toFloat();          return markedParams(); }
    if (valueName == "skySource")         { skySource = value.toInt() == 1 ? SkyLightSource::Cubemap : SkyLightSource::Sky; return markedParams(); }

    return SceneNode::setPropertyValue(valueName, value);
}

void LightNode::updateAnimation(float time)
{
    if (!!animation) {
        if(animation->hasPropertyAnim("intensity"))
            intensity = animation->getFloatPropertyAnim("intensity")->getValue(time);
        if(animation->hasPropertyAnim("lightColor"))
            color = animation->getColorPropertyAnim("lightColor")->getValue(time);
        if(animation->hasPropertyAnim("distance"))
            distance = animation->getFloatPropertyAnim("distance")->getValue(time);
        if(animation->hasPropertyAnim("spotCutOff"))
            spotCutOff = animation->getFloatPropertyAnim("spotCutOff")->getValue(time);
        if(animation->hasPropertyAnim("spotCutOffSoftness"))
            spotCutOffSoftness = animation->getFloatPropertyAnim("spotCutOffSoftness")->getValue(time);
        if(animation->hasPropertyAnim("spotFalloff"))
            spotFalloff = animation->getFloatPropertyAnim("spotFalloff")->getValue(time);
        if(animation->hasPropertyAnim("rectWidth"))
            rectWidth = animation->getFloatPropertyAnim("rectWidth")->getValue(time);
        if(animation->hasPropertyAnim("rectHeight"))
            rectHeight = animation->getFloatPropertyAnim("rectHeight")->getValue(time);
    }

    SceneNode::updateAnimation(time);
}

LightNode::LightNode()
{
    this->sceneNodeType = SceneNodeType::Light;

    lightType = LightType::Point;

    distance = 10;
    sourceRadius = 0.1f;   // a bulb's size: the falloff caps at 1 / 0.01 m^2 inside it
    color = QColor(255, 255, 255);
    intensity = 1.0f;
    spotCutOff = 30.0f;
    // 0..1, and it was 1.0 here for years — a full-width penumbra with a 1%
    // bright core on EVERY spot ever created (LIGHTING_FIX fix 5 / F-S2). 0.15
    // is a narrow, deliberate soft edge, which is what "softness" was meant to
    // mean and what the 0..1 UI row now offers.
    spotCutOffSoftness = 0.15f;
    spotFalloff = 1.0f;

    rectWidth = 1.0f;
    rectHeight = 1.0f;
    doubleSided = false;

    // 0 = the sun. A light created on its own is the first directional a
    // caller can possibly have; the auto-slot (Scene::nextForwardShadingPriority)
    // moves the SECOND and later ones off 0 when they join a scene.
    forwardShadingPriority = 0;

    iesNormalisation = 1.0f;   // no profile bound: intensity passes through

    iconSize = 0.5f;

	exportable = false;

}

SceneNodePtr LightNode::createDuplicate()
{
	auto light = iris::LightNode::create();

	light->lightType = this->lightType;
	light->color = this->color;
	light->intensity = this->intensity;
	light->distance = this->distance;
	light->sourceRadius = this->sourceRadius;
	light->spotCutOff = this->spotCutOff;
	light->spotCutOffSoftness = this->spotCutOffSoftness;
	light->spotFalloff = this->spotFalloff;
	light->rectWidth = this->rectWidth;
	light->rectHeight = this->rectHeight;
	light->doubleSided = this->doubleSided;
	light->forwardShadingPriority = this->forwardShadingPriority;
	light->followsAtmosphere = this->followsAtmosphere;
	light->iesProfileGuid = this->iesProfileGuid;
	light->iesProfilePath = this->iesProfilePath;
	light->iesNormalisation = this->iesNormalisation;
	light->skySource = this->skySource;
	light->skyCubeFaces = this->skyCubeFaces;
	light->skyCubeTexture = this->skyCubeTexture;
	light->shadowMap = this->shadowMap;
	light->icon = this->icon;
	light->iconSize = this->iconSize;

	return light;
}

}
