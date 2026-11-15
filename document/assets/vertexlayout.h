/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef VERTEXLAYOUT_H
#define VERTEXLAYOUT_H

#include <QList>

namespace iris
{

enum class VertexAttribUsage : int
{
    Position = 0,
    Color = 1,
    TexCoord0 = 2,
    TexCoord1 = 3,
    TexCoord2 = 4,
    TexCoord3 = 5,
    Normal = 6,
    Tangent = 7,
    BiTangent = 8,
    BoneIndices = 9,
    BoneWeights = 10,
    Count = 11
};

// Neutral attribute component types. The numeric values deliberately match the
// old GLenum values (GL_FLOAT/GL_INT/GL_UNSIGNED_BYTE) so buffers built by older
// code keep their meaning; no GL headers are involved any more.
enum AttribType : int
{
    AttribTypeFloat        = 0x1406,
    AttribTypeInt          = 0x1404,
    AttribTypeUnsignedByte = 0x1401
};

struct VertexAttribute
{
    VertexAttribUsage usage;

    int type;// AttribTypeFloat, AttribTypeInt, ...
    int count;//2 for vec2, 3 for vec3, etc
    int sizeInBytes;
};

class VertexLayout
{
    QList<VertexAttribute> attribs;
    int stride;

public:
    VertexLayout();

    QList<VertexAttribute> getAttribs();
    void addAttrib(VertexAttribUsage usage, int type, int count, int sizeInBytes);

    int getStride();
};

}

#endif // VERTEXLAYOUT_H
