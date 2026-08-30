/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include "vertexlayout.h"

namespace iris
{

VertexLayout::VertexLayout()
{
    stride = 0;
}

QList<VertexAttribute> VertexLayout::getAttribs()
{
    return attribs;
}

void VertexLayout::addAttrib(VertexAttribUsage usage,int type,int count,int sizeOfAttribInBytes)
{
    VertexAttribute attrib = {usage, type, count, sizeOfAttribInBytes};
    attribs.append(attrib);

    stride += sizeOfAttribInBytes;
}

int VertexLayout::getStride()
{
    return stride;
}

}
