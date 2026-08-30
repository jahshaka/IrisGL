#include "document/scenegraph/lightnode.h"
#include "document/scenegraph/scenenode.h"
#include "document/animation/animableproperty.h"
#include "document/animation/keyframeset.h"
#include "document/animation/animation.h"
#include "document/animation/propertyanim.h"
#include "core/properties/property.h"
#include "document/scenegraph/shadowmap.h"

namespace iris
{

QList<Property*> LightNode::getProperties()
{
    auto props = SceneNode::getProperties();

    auto colorProp = new ColorProperty();
    colorProp->displayName = "Light Color";
    colorProp->name = "lightColor";
    colorProp->value = color;
    props.append(colorProp);

    auto prop = new FloatProperty();
    prop->displayName = "Intensity";
    prop->name = "intensity";
    prop->value = intensity;
    props.append(prop);

    prop = new FloatProperty();
    prop->displayName = "Distance";
    prop->name = "distance";
    prop->value = distance;
    props.append(prop);

    prop = new FloatProperty();
    prop->displayName = "Spot CutOff";
    prop->name = "spotCutOff";
    prop->value = spotCutOff;
    props.append(prop);

    prop = new FloatProperty();
    prop->displayName = "Spot CutOff Softness";
    prop->name = "spotCutOffSoftness";
    prop->value = spotCutOffSoftness;
    props.append(prop);

    prop = new FloatProperty();
    prop->displayName = "Rect Width";
    prop->name = "rectWidth";
    prop->value = rectWidth;
    props.append(prop);

    prop = new FloatProperty();
    prop->displayName = "Rect Height";
    prop->name = "rectHeight";
    prop->value = rectHeight;
    props.append(prop);

    prop = new FloatProperty();
    prop->displayName = "Shadow Alpha";
    prop->name = "shadowAlpha";
    prop->value = shadowAlpha;
    props.append(prop);

    prop = new FloatProperty();
    prop->displayName = "Icon Size";
    prop->name = "iconSize";
    prop->value = iconSize;
    props.append(prop);

    prop = new FloatProperty();
    prop->displayName = "Shadow Bias";
    prop->name = "shadowBias";
    prop->value = shadowMap->bias;
    props.append(prop);

    auto shadowColorProp = new ColorProperty();
    shadowColorProp->displayName = "Shadow Color";
    shadowColorProp->name = "shadowColor";
    shadowColorProp->value = shadowColor;
    props.append(shadowColorProp);

    auto intProp = new IntProperty();
    intProp->displayName = "Light Type";
    intProp->name = "lightType";
    intProp->value = static_cast<int>(lightType);
    props.append(intProp);

    intProp = new IntProperty();
    intProp->displayName = "Shadow Map Type";
    intProp->name = "shadowMapType";
    intProp->value = static_cast<int>(shadowMap->shadowType);
    props.append(intProp);

    intProp = new IntProperty();
    intProp->displayName = "Shadow Map Resolution";
    intProp->name = "shadowMapResolution";
    intProp->value = shadowMap->resolution;
    props.append(intProp);

    auto boolProp = new BoolProperty();
    boolProp->displayName = "Double Sided";
    boolProp->name = "doubleSided";
    boolProp->value = doubleSided;
    props.append(boolProp);

    boolProp = new BoolProperty();
    boolProp->displayName = "Accurate";
    boolProp->name = "accurate";
    boolProp->value = accurate;
    props.append(boolProp);

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
    if(valueName == "spotCutOff")
        return spotCutOff;
    if(valueName == "spotCutOffSoftness")
        return spotCutOffSoftness;
    if(valueName == "rectWidth")
        return rectWidth;
    if(valueName == "rectHeight")
        return rectHeight;
    if(valueName == "lightType")
        return static_cast<int>(lightType);
    if(valueName == "shadowColor")
        return shadowColor;
    if(valueName == "shadowAlpha")
        return shadowAlpha;
    if(valueName == "shadowMapType")
        return static_cast<int>(getShadowMapType());
    if(valueName == "shadowMapResolution")
        return getShadowMapResolution();
    if(valueName == "shadowBias")
        return shadowMap->bias;
    if(valueName == "doubleSided")
        return doubleSided;
    if(valueName == "accurate")
        return accurate;
    if(valueName == "iconSize")
        return iconSize;

    return SceneNode::getPropertyValue(valueName);
}

bool LightNode::setPropertyValue(QString valueName, const QVariant &value)
{
    if (valueName == "intensity")         { intensity = value.toFloat();         return true; }
    if (valueName == "lightColor")        { color = value.value<QColor>();       return true; }
    if (valueName == "distance")          { distance = value.toFloat();          return true; }
    if (valueName == "spotCutOff")        { spotCutOff = value.toFloat();        return true; }
    if (valueName == "spotCutOffSoftness"){ spotCutOffSoftness = value.toFloat();return true; }
    if (valueName == "rectWidth")         { rectWidth = value.toFloat();         return true; }
    if (valueName == "rectHeight")        { rectHeight = value.toFloat();        return true; }
    if (valueName == "lightType")         { setLightType(static_cast<LightType>(value.toInt())); return true; }
    if (valueName == "shadowColor")       { shadowColor = value.value<QColor>(); return true; }
    if (valueName == "shadowAlpha")       { shadowAlpha = value.toFloat();       return true; }
    // The shadow-map fields live on the node's ShadowMap; reflect through it.
    if (valueName == "shadowMapType")     { setShadowMapType(static_cast<ShadowMapType>(value.toInt())); return true; }
    if (valueName == "shadowMapResolution"){ setShadowMapResolution(value.toInt()); return true; }
    if (valueName == "shadowBias")        { shadowMap->bias = value.toFloat();   return true; }
    if (valueName == "doubleSided")       { doubleSided = value.toBool();        return true; }
    if (valueName == "accurate")          { accurate = value.toBool();           return true; }
    if (valueName == "iconSize")          { iconSize = value.toFloat();          return true; }

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
    color = QColor(255, 255, 255);
    intensity = 1.0f;
    spotCutOff = 30.0f;
    spotCutOffSoftness = 1.0f;

    rectWidth = 1.0f;
    rectHeight = 1.0f;
    doubleSided = false;
    accurate = false;

	shadowAlpha = 1.0f;
	shadowColor = QColor(0,0,0);

    iconSize = 0.5f;

	exportable = false;

    shadowMap = new ShadowMap();
}

SceneNodePtr LightNode::createDuplicate()
{
	auto light = iris::LightNode::create();

	light->lightDir = this->lightDir;
	light->lightType = this->lightType;
	light->color = this->color;
	light->intensity = this->intensity;
	light->distance = this->distance;
	light->spotCutOff = this->spotCutOff;
	light->spotCutOffSoftness = this->spotCutOffSoftness;
	light->rectWidth = this->rectWidth;
	light->rectHeight = this->rectHeight;
	light->doubleSided = this->doubleSided;
	light->accurate = this->accurate;
	light->shadowAlpha = this->shadowAlpha;
	light->shadowColor = this->shadowColor;
	light->shadowMap->bias = this->shadowMap->bias;
	light->shadowMap->shadowType = this->shadowMap->shadowType;
	light->shadowMap->setResolution(this->shadowMap->resolution);
	light->icon = this->icon;
	light->iconSize = this->iconSize;

	return light;
}

}
