/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef ANIMATION_H
#define ANIMATION_H

#include "document/export.h"

#include "irisglfwd.h"
#include <map>
#include <string>

namespace iris
{

class PropertyAnim;

enum class AnimationType
{
    Property,
    Skeletal
};

class IRIS_DOCUMENT_API Animation
{
    std::string name;
    bool loop;
    float length;

    // sample rate
    int frameRate;

public:
    std::map<std::string,PropertyAnim*> properties;
    SkeletalAnimationPtr skeletalAnimation;

    explicit Animation(std::string name = "Animation");
    ~Animation();
    Animation(const Animation &) = delete;              // owns raw PropertyAnim tracks
    Animation &operator=(const Animation &) = delete;

    void addPropertyAnim(PropertyAnim* anim);
    void removePropertyAnim(std::string name);

    PropertyAnim* getPropertyAnim(std::string name);

    FloatPropertyAnim* getFloatPropertyAnim(std::string name);

    Vector3DPropertyAnim* getVector3PropertyAnim(std::string name);

    ColorPropertyAnim* getColorPropertyAnim(std::string name);

    bool hasPropertyAnim(std::string name);

    static AnimationPtr create(std::string name)
    {
        return AnimationPtr(new Animation(name));
    }

    static AnimationPtr createFromSkeletalAnimation(SkeletalAnimationPtr skelAnim);

    /// A COPY for a duplicated node (CLIP-REF-1): its own property tracks (the
    /// keys deep-copied, so editing one node's keys never moves the other's),
    /// and the SAME skeletal clip — clip data is shared, read-only, and its
    /// {assetGuid, name} reference is what both nodes persist.
    AnimationPtr clone() const;

    std::string getName() const;
    void setName(const std::string &value);

    float getLength() const;
    void setLength(float value);

    bool getLooping() const;
    void setLooping(bool value);

    int getFrameRate() const;
    void setFrameRate(int value);

    SkeletalAnimationPtr getSkeletalAnimation() const;
    bool hasSkeletalAnimation();
    void setSkeletalAnimation(const SkeletalAnimationPtr &value);

    // Calculate the time the animation keyframes should be sampled at
    // It takes looping into consideration
    float getSampleTime(float time);

    void calculateAnimationLength();
};

}
#endif // ANIMATION_H
