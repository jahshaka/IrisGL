#include "core/math/vec.h"
#include "document/animation/propertyanim.h"
#include "document/animation/keyframeanimation.h"

namespace iris{

FloatPropertyAnim::FloatPropertyAnim()
{
    keyFrame = new FloatKeyFrame();
}

FloatPropertyAnim::~FloatPropertyAnim()
{
    delete keyFrame;
}

float FloatPropertyAnim::getValue(float time)
{
    return keyFrame->getValueAt(time);
}

std::vector<PropertyAnimInfo> FloatPropertyAnim::getKeyFrames()
{
    std::vector<PropertyAnimInfo> frames;
    frames.push_back(PropertyAnimInfo(name,keyFrame));
    return frames;
}

FloatKeyFrame *FloatPropertyAnim::getKeyFrame(std::string name)
{
    //returns keyframe regardless of name
    return keyFrame;
}

FloatKeyFrame *FloatPropertyAnim::getKeyFrame(int index)
{
    if (index == 0) return keyFrame;
    return nullptr;
}

Vector3DPropertyAnim::Vector3DPropertyAnim()
{
    keyFrames[0] = new FloatKeyFrame();
    keyFrames[1] = new FloatKeyFrame();
    keyFrames[2] = new FloatKeyFrame();
}

Vector3DPropertyAnim::~Vector3DPropertyAnim()
{
    //delete[] keyFrames;
    delete keyFrames[0];
    delete keyFrames[1];
    delete keyFrames[2];
}

iris::Vec3 Vector3DPropertyAnim::getValue(float time)
{
    return iris::Vec3(
                keyFrames[0]->getValueAt(time),
                keyFrames[1]->getValueAt(time),
                keyFrames[2]->getValueAt(time)
                );
}

std::vector<PropertyAnimInfo> Vector3DPropertyAnim::getKeyFrames()
{
    std::vector<PropertyAnimInfo> frames;
    frames.push_back(PropertyAnimInfo("X",keyFrames[0]));
    frames.push_back(PropertyAnimInfo("Y",keyFrames[1]));
    frames.push_back(PropertyAnimInfo("Z",keyFrames[2]));
    return frames;
}

FloatKeyFrame *Vector3DPropertyAnim::getKeyFrame(std::string name)
{
    if (name=="X") return keyFrames[0];
    if (name=="Y") return keyFrames[1];
    if (name=="Z") return keyFrames[2];

    return nullptr;
}

// OFF BY ONE, fixed 2026-09-04: the guard was `index > 3` on a THREE element
// array, so getKeyFrame(3) read one past the end and handed the caller whatever
// followed it in the object. SceneReader drives this index straight from a
// scene blob's keyFrames array length, so a file with four entries on a vector3
// track was an out-of-bounds read of document memory.
FloatKeyFrame *Vector3DPropertyAnim::getKeyFrame(int index)
{
    if (index < 0 || index >= 3)
        return nullptr;

    return keyFrames[index];
}

ColorPropertyAnim::ColorPropertyAnim()
{
    keyFrames[0] = new FloatKeyFrame();
    keyFrames[1] = new FloatKeyFrame();
    keyFrames[2] = new FloatKeyFrame();
    keyFrames[3] = new FloatKeyFrame();
}

ColorPropertyAnim::~ColorPropertyAnim()
{
    //delete[] keyFrames;
    delete keyFrames[0];
    delete keyFrames[1];
    delete keyFrames[2];
    delete keyFrames[3];
}

SrgbColor ColorPropertyAnim::getValue(float time)
{
    return SrgbColor(
                keyFrames[0]->getValueAt(time) * 255,
                keyFrames[1]->getValueAt(time) * 255,
                keyFrames[2]->getValueAt(time) * 255,
                keyFrames[3]->getValueAt(time) * 255
                );
}

std::vector<PropertyAnimInfo> ColorPropertyAnim::getKeyFrames()
{
    std::vector<PropertyAnimInfo> frames;
    frames.push_back(PropertyAnimInfo("R",keyFrames[0]));
    frames.push_back(PropertyAnimInfo("G",keyFrames[1]));
    frames.push_back(PropertyAnimInfo("B",keyFrames[2]));
    frames.push_back(PropertyAnimInfo("A",keyFrames[3]));
    return frames;
}

FloatKeyFrame *ColorPropertyAnim::getKeyFrame(std::string name)
{
    if (name=="R") return keyFrames[0];
    if (name=="G") return keyFrames[1];
    if (name=="B") return keyFrames[2];
    if (name=="A") return keyFrames[3];

    return nullptr;
}

// Same off-by-one as Vector3DPropertyAnim above: four channels, so index 4 was
// one past the end.
FloatKeyFrame *ColorPropertyAnim::getKeyFrame(int index)
{
    if (index < 0 || index >= 4)
        return nullptr;

    return keyFrames[index];
}

PropertyAnimInfo::PropertyAnimInfo(std::string name, FloatKeyFrame *keyFrame)
{
    this->name = name;
    this->keyFrame = keyFrame;
}

std::string PropertyAnim::getName() const
{
    return name;
}

void PropertyAnim::setName(const std::string &value)
{
    name = value;
}


}
