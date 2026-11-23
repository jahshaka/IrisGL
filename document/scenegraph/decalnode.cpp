#include "document/scenegraph/decalnode.h"
#include "document/scenegraph/scenenode.h"
#include "document/animation/animableproperty.h"
#include "document/animation/keyframeset.h"
#include "document/animation/animation.h"
#include "document/animation/propertyanim.h"
#include "document/properties/property.h"

namespace iris
{

PropertyRows DecalNode::getProperties()
{
    auto props = SceneNode::getProperties();

    auto texProp = std::make_unique<TextureProperty>();
    texProp->displayName = "Decal Image";
    texProp->name = "decalTexture";
    texProp->value = textureGuid.toStdString();
    props.add(std::move(texProp));

    auto prop = std::make_unique<FloatProperty>();
    prop->displayName = "Width";
    prop->name = "width";
    prop->value = width;
    props.add(std::move(prop));

    prop = std::make_unique<FloatProperty>();
    prop->displayName = "Height";
    prop->name = "height";
    prop->value = height;
    props.add(std::move(prop));

    prop = std::make_unique<FloatProperty>();
    prop->displayName = "Depth";
    prop->name = "depth";
    prop->value = depth;
    props.add(std::move(prop));

    // The only scene-node rows with a range worth declaring: setPropertyValue
    // below qBounds both to 0..1, so this is the setter's own contract written
    // down, not a UI slider's taste. Every other node row leaves min == max ==
    // 0, which readers must take as "no declared range" (property.h).
    prop = std::make_unique<FloatProperty>();
    prop->displayName = "Metalness";
    prop->name = "metalness";
    prop->value = metalness;
    prop->minValue = 0.0f;
    prop->maxValue = 1.0f;
    props.add(std::move(prop));

    prop = std::make_unique<FloatProperty>();
    prop->displayName = "Roughness";
    prop->name = "roughness";
    prop->value = roughness;
    prop->minValue = 0.0f;
    prop->maxValue = 1.0f;
    props.add(std::move(prop));

    auto boolProp = std::make_unique<BoolProperty>();
    boolProp->displayName = "Ignore Alpha (Diffuse Only)";
    boolProp->name = "ignoreAlphaDiffuse";
    boolProp->value = ignoreAlphaDiffuse;
    props.add(std::move(boolProp));

    return props;
}

Value DecalNode::getPropertyValue(std::string_view valueName)
{
    if (valueName == "decalTexture")       return Value(textureGuid.toStdString());
    if (valueName == "decalNormal")        return Value(normalGuid.toStdString());
    if (valueName == "decalEmissive")      return Value(emissiveGuid.toStdString());
    if (valueName == "width")              return Value(double(width));
    if (valueName == "height")             return Value(double(height));
    if (valueName == "depth")              return Value(double(depth));
    if (valueName == "metalness")          return Value(double(metalness));
    if (valueName == "roughness")          return Value(double(roughness));
    if (valueName == "ignoreAlphaDiffuse") return Value(ignoreAlphaDiffuse);

    return SceneNode::getPropertyValue(valueName);
}

bool DecalNode::setPropertyValue(std::string_view valueName, const Value &value)
{
    // STRICT: a guid a String, an extent or a factor a Number (or an Int), the flag a Bool; anything else false.
    const bool isText = value.kind() == Kind::String;
    double n = 0.0;
    const bool isNum = numberOf(value, n);
    const float f = float(n);
    // Changing an image guid invalidates the resolved path: the host (reader /
    // edit service / property panel) re-resolves it through the CAS. Clearing
    // it here means the mirror can never bind a stale file for a new guid.
    if (valueName == "decalTexture")  { if (!isText) return false; textureGuid = QString::fromStdString(value.asString());  resolvedTexturePath.clear();  return markedParams(); }
    if (valueName == "decalNormal")   { if (!isText) return false; normalGuid = QString::fromStdString(value.asString());   resolvedNormalPath.clear();   return markedParams(); }
    if (valueName == "decalEmissive") { if (!isText) return false; emissiveGuid = QString::fromStdString(value.asString()); resolvedEmissivePath.clear(); return markedParams(); }
    // Non-positive extents would collapse the projector box (and a NEGATIVE
    // one flips the accept half-space in the shader — DECALS_SPEC §4).
    if (valueName == "width")   { if (!isNum) return false; width  = std::max(0.001f, f); return markedParams(); }
    if (valueName == "height")  { if (!isNum) return false; height = std::max(0.001f, f); return markedParams(); }
    if (valueName == "depth")   { if (!isNum) return false; depth  = std::max(0.001f, f); return markedParams(); }
    if (valueName == "metalness") { if (!isNum) return false; metalness = iris::bound(0.0f, f, 1.0f); return markedParams(); }
    if (valueName == "roughness") { if (!isNum) return false; roughness = iris::bound(0.0f, f, 1.0f); return markedParams(); }
    if (valueName == "ignoreAlphaDiffuse") {
        if (value.kind() != Kind::Bool) return false;
        ignoreAlphaDiffuse = value.asBool();
        return markedParams();
    }

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
    // Every switch on getSceneNodeType() — Scene's registry, the mirror, the
    // writer, the picker, the panel — reads this; without it a decal is
    // silently an Empty node forever. (This was the line CameraNode was
    // missing until CAMERAS_SPEC phase 1.)
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
