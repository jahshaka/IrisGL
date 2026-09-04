/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef IVIEWSOURCE_H
#define IVIEWSOURCE_H

#include "core/math/mat4.h"
#include "core/math/vec.h"


namespace iris{

class IViewSource
{
public:
    virtual iris::Vec3 getPosition() = 0;
    virtual iris::Mat4 getViewMatrix() = 0;
    virtual iris::Mat4 getProjMatrix() = 0;
    virtual float getNearClip();
    virtual float getFarClip();

    virtual ~IViewSource() = default;
};

}

#endif // IVIEWSOURCE_H
