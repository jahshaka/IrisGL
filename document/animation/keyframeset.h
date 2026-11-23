/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef KEYFRAMESET_H
#define KEYFRAMESET_H

#include <map>
#include "irisglfwd.h"

namespace iris
{

class FloatKeyFrame;
class PropertyAnim;

/**
 * This class stores float keyframes associated with a QString key
 */
class KeyFrameSet
{
public:
    ~KeyFrameSet();

    //QMap preserves insertion order,QHash doesnt
    std::map<std::string,PropertyAnim*> keyFrames;

    void addKeyFrame(PropertyAnim* anim);

    PropertyAnim* getOrCreateFrame(std::string name);

    PropertyAnim* getKeyFrame(std::string name);

    bool hasKeyFrame(std::string name);

    static std::shared_ptr<KeyFrameSet> create();
};

}
#endif // KEYFRAMESET_H
