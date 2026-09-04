#include "document/scenegraph/decalnode.h"
#include "document/scenegraph/scenenode.h"
#include "document/animation/animableproperty.h"
#include "document/animation/keyframeset.h"
#include "document/animation/animation.h"
#include "document/animation/propertyanim.h"
#include "core/properties/property.h"

namespace iris
{

QList<Property*> DecalNode::getProperties()
{
    auto props = SceneNode::getProperties();

    auto texProp = new TextureProperty();
    texProp->displayName = "Decal Image";
    texProp->name = "decalTexture";
    texProp->value = textureGuid;
    props.append(texProp);

    auto prop = new FloatProperty();
    prop->displayName = "Width";
    prop->name = "width";
    prop->value = width;
    props.append(prop);

    prop = new FloatProperty();
    prop->displayName = "Height";
    prop->name = "height";
    prop->value = height;
    props.append(prop);

    prop = new FloatProperty();
    prop->displayName = "Depth";
    prop->name = "depth";
    prop->value = depth;
    props.append(prop);

    // The only scene-node rows with a range worth declaring: setPropertyValue
    // below qBounds both to 0..1, so this is the setter's own contract written
    // down, not a UI slider's taste. Every other node row leaves min == max ==
    // 0, which readers must take as "no declared range" (property.h).
    prop = new FloatProperty();
    prop->displayName = "Metalness";
    prop->name = "metalness";
    prop->value = metalness;
    prop->minValue = 0.0f;
    prop->maxValue = 1.0f;
    props.append(prop);

    prop = new FloatProperty();
    prop->displayName = "Roughness";
    prop->name = "roughness";
    prop->value = roughness;
    prop->minValue = 0.0f;
    prop->maxValue = 1.0f;
    props.append(prop);

    auto boolProp = new BoolProperty();
    boolProp->displayName = "Ignore Alpha (Diffuse Only)";
    boolProp->name = "ignoreAlphaDiffuse";
    boolProp->value = ignoreAlphaDiffuse;
    props.append(boolProp);

    return props;
}

QVariant DecalNode::getPropertyValue(QString valueName)
{
    if (valueName == "decalTexture")       return textureGuid;
    if (valueName == "decalNormal")        return normalGuid;
    if (valueName == "decalEmissive")      return emissiveGuid;
    if (valueName == "width")              return width;
    if (valueName == "height")             return height;
    if (valueName == "depth")              return depth;
    if (valueName == "metalness")          return metalness;
    if (valueName == "roughness")          return roughness;
    if (valueName == "ignoreAlphaDiffuse") return ignoreAlphaDiffuse;

    return SceneNode::getPropertyValue(valueName);
}

bool DecalNode::setPropertyValue(QString valueName, const QVariant &value)
{
    // Changing an image guid invalidates the resolved path: the host (reader /
    // edit service / property panel) re-resolves it through the CAS. Clearing
    // it here means the mirror can never bind a stale file for a new guid.
    if (valueName == "decalTexture")  { textureGuid = value.toString();  resolvedTexturePath.clear();  return true; }
    if (valueName == "decalNormal")   { normalGuid = value.toString();   resolvedNormalPath.clear();   return true; }
    if (valueName == "decalEmissive") { emissiveGuid = value.toString(); resolvedEmissivePath.clear(); return true; }
    // Non-positive extents would collapse the projector box (and a NEGATIVE
    // one flips the accept half-space in the shader — DECALS_SPEC §4).
    if (valueName == "width")   { width  = std::max(0.001f, value.toFloat()); return true; }
    if (valueName == "height")  { height = std::max(0.001f, value.toFloat()); return true; }
    if (valueName == "depth")   { depth  = std::max(0.001f, value.toFloat()); return true; }
    if (valueName == "metalness") { metalness = qBound(0.0f, value.toFloat(), 1.0f); return true; }
    if (valueName == "roughness") { roughness = qBound(0.0f, value.toFloat(), 1.0f); return true; }
    if (valueName == "ignoreAlphaDiffuse") { ignoreAlphaDiffuse = value.toBool(); return true; }

    return SceneNode::setPropertyValue(valueName, value);
}

void DecalNode::updateAnimation(float time)
{
    if (!!animation) {
        if (animation->hasPropertyAnim("width"))
            width = std::max(0.001f, animation->getFloatPropertyAnim("width")->getValue(time));
        if (animation->hasPropertyAnim("height"))
            height = std::max(0.001f, animation->getFloatPropertyAnim("height")->getValue(time));
        if (animation->hasPropertyAnim("depth"))
            depth = std::max(0.001f, animation->getFloatPropertyAnim("depth")->getValue(time));
        if (animation->hasPropertyAnim("metalness"))
            metalness = animation->getFloatPropertyAnim("metalness")->getValue(time);
        if (animation->hasPropertyAnim("roughness"))
            roughness = animation->getFloatPropertyAnim("roughness")->getValue(time);
    }

    SceneNode::updateAnimation(time);
}

DecalNode::DecalNode()
{
    // THE line CameraNode never had. Every switch on getSceneNodeType() —
    // Scene's registry, the mirror, the writer, the picker, the panel — reads
    // this; without it a decal is silently an Empty node forever.
    this->sceneNodeType = SceneNodeType::Decal;

    // A decal is an editor helper object: it is not exported as geometry.
    exportable = false;
}

SceneNodePtr DecalNode::createDuplicate()
{
    auto decal = iris::DecalNode::create();

    decal->textureGuid         = this->textureGuid;
    decal->resolvedTexturePath = this->resolvedTexturePath;
    decal->normalGuid          = this->normalGuid;
    decal->resolvedNormalPath  = this->resolvedNormalPath;
    decal->emissiveGuid        = this->emissiveGuid;
    decal->resolvedEmissivePath = this->resolvedEmissivePath;
    decal->width               = this->width;
    decal->height              = this->height;
    decal->depth               = this->depth;
    decal->metalness           = this->metalness;
    decal->roughness           = this->roughness;
    decal->ignoreAlphaDiffuse  = this->ignoreAlphaDiffuse;

    return decal;
}

}
