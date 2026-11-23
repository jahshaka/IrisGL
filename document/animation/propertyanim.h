#ifndef ANIMFRAMES_H
#define ANIMFRAMES_H

#include "document/export.h"

#include "core/math/vec.h"
#include "irisglfwd.h"
#include "document/srgbcolor.h"

#include <vector>
#include <string>

namespace iris {


class FloatKeyFrame;
class AnimFrameInfo;

struct IRIS_DOCUMENT_API PropertyAnimInfo
{
    std::string name;
    // keyFrame isnt owned by this class
    FloatKeyFrame* keyFrame = nullptr;

    PropertyAnimInfo(std::string name, FloatKeyFrame* keyFrame);
};

class IRIS_DOCUMENT_API PropertyAnim
{
protected:
    std::string name;
public:
    virtual std::vector<PropertyAnimInfo> getKeyFrames()
    {
        std::vector<PropertyAnimInfo> list;
        return list;
    }

    virtual FloatKeyFrame* getKeyFrame(std::string name)
    {
        return nullptr;
    }

    virtual FloatKeyFrame* getKeyFrame(int index)
    {
        return nullptr;
    }

    std::string getName() const;
    void setName(const std::string &value);

    virtual ~PropertyAnim()
    {

    }
};

class IRIS_DOCUMENT_API FloatPropertyAnim : public PropertyAnim
{
    FloatKeyFrame* keyFrame;
public:
    FloatPropertyAnim();
    ~FloatPropertyAnim();

    float getValue(float time);
    virtual std::vector<PropertyAnimInfo> getKeyFrames() override;
    virtual FloatKeyFrame* getKeyFrame(std::string name) override;
    virtual FloatKeyFrame* getKeyFrame(int index) override;
};

class IRIS_DOCUMENT_API Vector3DPropertyAnim : public PropertyAnim
{
    FloatKeyFrame* keyFrames[3];
public:
    Vector3DPropertyAnim();
    ~Vector3DPropertyAnim();

    iris::Vec3 getValue(float time);
    virtual std::vector<PropertyAnimInfo> getKeyFrames() override;
    virtual FloatKeyFrame* getKeyFrame(std::string name) override;
    virtual FloatKeyFrame* getKeyFrame(int index) override;
};

class IRIS_DOCUMENT_API ColorPropertyAnim : public PropertyAnim
{
    FloatKeyFrame* keyFrames[4];
public:
    ColorPropertyAnim();
    ~ColorPropertyAnim();

    SrgbColor getValue(float time);
    virtual std::vector<PropertyAnimInfo> getKeyFrames() override;
    virtual FloatKeyFrame* getKeyFrame(std::string name) override;
    virtual FloatKeyFrame* getKeyFrame(int index) override;
};

}

#endif // ANIMFRAMES_H
