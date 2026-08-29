#include "lightnode.h"
#include "../scenegraph/scenenode.h"
#include "../animation/animableproperty.h"
#include "../animation/keyframeset.h"
#include "../animation/animation.h"
#include "../animation/propertyanim.h"
#include "../core/property.h"
#include "../graphics/shadowmap.h"

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

    return SceneNode::getPropertyValue(valueName);
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
