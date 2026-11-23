#ifndef ANIMATIONPROPERTY_H
#define ANIMATIONPROPERTY_H

#include "irisglfwd.h"

namespace iris
{

enum class AnimablePropertyType
{
    Float,
    Vector3,
    Color
};

struct AnimableProperty
{
    std::string name;
    AnimablePropertyType type;

    AnimableProperty(std::string name,AnimablePropertyType type)
    {
        this->name = name;
        this->type = type;
    }
};

}

#endif // ANIMATIONPROPERTY_H
