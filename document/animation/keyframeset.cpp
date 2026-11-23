/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include "document/animation/keyframeset.h"
#include "document/animation/keyframeanimation.h"
#include "document/animation/propertyanim.h"
#include <cassert>

namespace iris
{

KeyFrameSet::~KeyFrameSet()
{
    //todo: delete keys
}

void KeyFrameSet::addKeyFrame(PropertyAnim *anim)
{
    assert(!(keyFrames.count(anim->getName()) != 0));

    keyFrames.insert_or_assign(anim->getName(), anim);
}

PropertyAnim* KeyFrameSet::getOrCreateFrame(std::string name)
{
    if((keyFrames.count(name) != 0))
    {
        return keyFrames[name];
    }

    auto propAnim = new FloatPropertyAnim();
    propAnim->setName(name);
    keyFrames.insert_or_assign(name, propAnim);
    return propAnim;
}

PropertyAnim* KeyFrameSet::getKeyFrame(std::string name)
{
    if((keyFrames.count(name) != 0))
    {
        return keyFrames[name];
    }

    return nullptr;
}

bool KeyFrameSet::hasKeyFrame(std::string name)
{
    return (keyFrames.count(name) != 0);
}

KeyFrameSetPtr KeyFrameSet::create()
{
    return KeyFrameSetPtr(new KeyFrameSet());
}

}
