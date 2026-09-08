/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include "document/animation/animation.h"
#include "document/animation/keyframeset.h"
#include "document/animation/propertyanim.h"
#include "document/animation/skeletalanimation.h"
#include "irisglfwd.h"
#include <QDebug>
#include <cmath>

namespace iris
{

SkeletalAnimationPtr Animation::getSkeletalAnimation() const
{
    return skeletalAnimation;
}

bool Animation::hasSkeletalAnimation()
{
    return !!skeletalAnimation;
}

void Animation::setSkeletalAnimation(const SkeletalAnimationPtr &value)
{
    skeletalAnimation = value;
    calculateAnimationLength();
}

float Animation::getSampleTime(float time)
{
    // length <= 0 is REACHABLE, not theoretical: calculateAnimationLength takes
    // the length from the last key, so an animation with a single key at t=0 —
    // one press of the Timeline's insert button, one anim.keyframe call — is
    // zero seconds long. fmod(t, 0) is NaN, and a NaN sample time poses NaN
    // positions into the document and then into the engine's transforms.
    if (loop && length > 0.0f) {
        return fmod(time, length);
    }

    return time;
}

void Animation::calculateAnimationLength()
{
    float maxLength = 0;
    // calculate length of keys
    for (auto propAnim : properties) {
        for (auto& frames : propAnim->getKeyFrames()) {
            auto length = frames.keyFrame->getLength();
            maxLength = qMax(maxLength, length);
        }
    }

    // calculate length of each bone in the skeletal animation
    if (!!skeletalAnimation) {
        for (auto boneAnim : skeletalAnimation->boneAnimations) {
            maxLength = qMax(maxLength, boneAnim->getLength());
        }
    }

    length = maxLength;
}

Animation::Animation(QString name)
{
    this->name = name;
    loop = true;
    length = 1.0f;
    frameRate = 60;
}

Animation::~Animation()
{
}

QString Animation::getName() const
{
    return name;
}

void Animation::setName(const QString &value)
{
    name = value;
}

float Animation::getLength() const
{
    return length;
}

void Animation::setLength(float value)
{
    length = value;
}

bool Animation::getLooping() const
{
    return loop;
}

void Animation::setLooping(bool value)
{
    loop = value;
}

void Animation::addPropertyAnim(PropertyAnim *anim)
{
    //Q_ASSERT(!properties.contains(name));
    
    properties.insert(anim->getName(), anim);
    calculateAnimationLength();
}

void Animation::removePropertyAnim(QString name)
{
    // QMap::operator[] on a non-const map INSERTS a default-constructed value
    // when the key is missing — so the old `auto prop = properties[name]` put a
    // NULL track into the map, and the remove() below then found the row it had
    // just created and returned 1. The "doesnt exist" warning was unreachable,
    // and every missed lookup left a null entry behind for the next iteration
    // over properties to trip on. take() removes and hands back in one step,
    // and default-constructs nothing when the key is absent.
    if (!properties.contains(name)) {
        qDebug() << "Animation property " << name << " doesnt exist";
        return;
    }

    delete properties.take(name);
}

// Every getter below reads with QMap::value(), NEVER with operator[]: on a
// non-const map the subscript INSERTS a default-constructed (null) value for a
// missing key. So READING a track that was not there used to CREATE a null
// one — after which hasPropertyAnim() answers yes forever, the serializer
// writes the empty row out, and the next walk over `properties` dereferences
// a null PropertyAnim. Every caller guarded the read with hasPropertyAnim()
// precisely because the getter could not be trusted; now a miss returns
// nullptr and leaves the map alone, and callers null-check instead.
PropertyAnim* Animation::getPropertyAnim(QString name)
{
    return properties.value(name, nullptr);
}

FloatPropertyAnim *Animation::getFloatPropertyAnim(QString name)
{
    return (FloatPropertyAnim*)properties.value(name, nullptr);
}

Vector3DPropertyAnim *Animation::getVector3PropertyAnim(QString name)
{
    return (Vector3DPropertyAnim*)properties.value(name, nullptr);
}

ColorPropertyAnim *Animation::getColorPropertyAnim(QString name)
{
    return (ColorPropertyAnim*)properties.value(name, nullptr);
}

bool Animation::hasPropertyAnim(QString name)
{
    return properties.contains(name);
}

AnimationPtr Animation::createFromSkeletalAnimation(SkeletalAnimationPtr skelAnim)
{
    auto anim = new Animation(skelAnim->name);
    anim->setSkeletalAnimation(skelAnim);
    return AnimationPtr(anim);
}

int Animation::getFrameRate() const
{
    return frameRate;
}

void Animation::setFrameRate(int value)
{
    frameRate = value;
}

}
