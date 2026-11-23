#include "document/scenegraph/lightnode.h"
#include "document/scenegraph/scenenode.h"
#include "document/animation/animableproperty.h"
#include "document/animation/keyframeset.h"
#include "document/animation/animation.h"
#include "document/animation/propertyanim.h"
#include "document/properties/property.h"
#include "document/scenegraph/shadowmap.h"
#include <algorithm>

#include "document/properties/valueshapes.h"

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

Value LightNode::getPropertyValue(std::string_view valueName)
{
    if(valueName == "intensity")
        return Value(double(intensity));
    if(valueName == "lightColor")
        return colourRowValue(color);
    if(valueName == "distance")
        return Value(double(distance));
    if(valueName == "sourceRadius")
        return Value(double(sourceRadius));
    if(valueName == "spotCutOff")
        return Value(double(spotCutOff));
    if(valueName == "spotCutOffSoftness")
        return Value(double(spotCutOffSoftness));
    if(valueName == "spotFalloff")
        return Value(double(spotFalloff));
    if(valueName == "rectWidth")
        return Value(double(rectWidth));
    if(valueName == "rectHeight")
        return Value(double(rectHeight));
    if(valueName == "lightType")
        return Value(static_cast<int>(lightType));
    if(valueName == "shadowMapType")
        return Value(static_cast<int>(getShadowMapType()));
    if(valueName == "shadowMapResolution")
        return Value(getShadowMapResolution());
    if(valueName == "doubleSided")
        return Value(doubleSided);
    if(valueName == "forwardShadingPriority")
        return Value(forwardShadingPriority);
    if(valueName == "followsAtmosphere")
        return Value(followsAtmosphere);
    // The asset BINDING (the IES profile) is reflected read-only-ish: the guid is the
    // document's state, the resolved path and the normalisation are the host's
    // (they come from the asset store, which irisgl cannot reach).
    if(valueName == "iesProfile")
        return Value(iesProfileGuid);
    if(valueName == "iesProfilePath")
        return Value(iesProfilePath);
    if(valueName == "iesNormalisation")
        return Value(double(iesNormalisation));
    if(valueName == "iconSize")
        return Value(double(iconSize));
    if(valueName == "skySource")
        return Value(static_cast<int>(skySource));

    return SceneNode::getPropertyValue(valueName);
}

bool LightNode::setPropertyValue(std::string_view valueName, const Value &value)
{
    // STRICT: a float a Number (or an Int), an int an Int, a flag a Bool, the colour its Object, a guid or a path a
    // String; anything else false, nothing moved.
    double n = 0.0;
    const bool isNum = numberOf(value, n);
    const float f = float(n);
    const bool isInt = value.kind() == Kind::Int;
    const int i = isInt ? static_cast<int>(value.asInt()) : 0;
    const bool isBool = value.kind() == Kind::Bool;
    const bool isText = value.kind() == Kind::String;
    if (valueName == "intensity")         { if (!isNum) return false; intensity = f;         return markedParams(); }
    if (valueName == "lightColor")        { if (!colourOfRowValue(value, color)) return false; return markedParams(); }
    if (valueName == "distance")          { if (!isNum) return false; distance = f;          return markedParams(); }
    if (valueName == "sourceRadius")      { if (!isNum) return false; sourceRadius = std::max(f, 0.001f); return markedParams(); }
    if (valueName == "spotCutOff")        { if (!isNum) return false; spotCutOff = f;        return markedParams(); }
    if (valueName == "spotCutOffSoftness"){ if (!isNum) return false; spotCutOffSoftness = f; return markedParams(); }
    if (valueName == "spotFalloff")       { if (!isNum) return false; spotFalloff = f;       return markedParams(); }
    if (valueName == "rectWidth")         { if (!isNum) return false; rectWidth = f;         return markedParams(); }
    if (valueName == "rectHeight")        { if (!isNum) return false; rectHeight = f;        return markedParams(); }
    if (valueName == "lightType")         { if (!isInt) return false; setLightType(static_cast<LightType>(i)); return markedParams(); }
    // The shadow-map fields live on the node's ShadowMap; reflect through it.
    // (a filter only: no value means "no shadow" - ShadowMapType's header; a value outside the three is refused)
    if (valueName == "shadowMapType") {
        if (!isInt || i < int(ShadowMapType::Hard) || i > int(ShadowMapType::VerySoft)) return false;
        setShadowMapType(static_cast<ShadowMapType>(i));
        return markedParams();
    }
    if (valueName == "shadowMapResolution"){ if (!isInt) return false; setShadowMapResolution(i); return markedParams(); }
    if (valueName == "doubleSided")       { if (!isBool) return false; doubleSided = value.asBool();        return markedParams(); }
    if (valueName == "forwardShadingPriority") { if (!isInt) return false; forwardShadingPriority = std::max(0, i); return markedParams(); }
    // The sun's direct light is tinted by the atmosphere at its own elevation
    // (lightnode.h). A LIGHT change, so the shadow and probe caches see it
    // through the same staleness path a colour edit already travels.
    if (valueName == "followsAtmosphere") { if (!isBool) return false; followsAtmosphere = value.asBool(); return markedParams(); }
    if (valueName == "iesProfile")        { if (!isText) return false; iesProfileGuid = value.asString(); return markedParams(); }
    if (valueName == "iesProfilePath")    { if (!isText) return false; iesProfilePath = value.asString(); return markedParams(); }
    if (valueName == "iesNormalisation")  { if (!isNum) return false; iesNormalisation = f;  return markedParams(); }
    if (valueName == "iconSize")          { if (!isNum) return false; iconSize = f;          return markedParams(); }
    if (valueName == "skySource")         { if (!isInt) return false; skySource = i == 1 ? SkyLightSource::Cubemap : SkyLightSource::Sky; return markedParams(); }

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
    color = SrgbColor(255, 255, 255);
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
