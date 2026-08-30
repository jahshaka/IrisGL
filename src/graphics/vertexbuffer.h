/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef IRIS_VERTEXBUFFER_H
#define IRIS_VERTEXBUFFER_H

// CPU-side vertex/index storage. Extracted from the deleted GraphicsDevice
// (step 14): these are pure document/asset data - the engine mirror converts
// them into engine buffers, and importers fill them. No GL anywhere.

#include <QSharedPointer>
#include <cstring>
#include "vertexlayout.h"

namespace iris
{

class VertexBuffer;
typedef QSharedPointer<VertexBuffer> VertexBufferPtr;
class IndexBuffer;
typedef QSharedPointer<IndexBuffer> IndexBufferPtr;

class VertexBuffer
{
public:
    // owns a heap copy of the vertex data; freed with delete[] (allocated with new char[])
    char* data;
    int dataSize;

    VertexLayout vertexLayout;

    static VertexBufferPtr create(VertexLayout vertexLayout)
    {
        return VertexBufferPtr(new VertexBuffer(vertexLayout));
    }

    template<typename T>
    void setData(T* data, unsigned int sizeInBytes)
    {
        setData((void*) data, sizeInBytes);
    }

    void setData(void* srcData, unsigned int sizeInBytes)
    {
        delete[] data;
        data = new char[sizeInBytes];
        memcpy(data, srcData, sizeInBytes);
        dataSize = (int)sizeInBytes;
    }

    ~VertexBuffer()
    {
        delete[] data;
    }

    // owns a raw buffer - copying would double-free
    VertexBuffer(const VertexBuffer&) = delete;
    VertexBuffer& operator=(const VertexBuffer&) = delete;

private:
    VertexBuffer(VertexLayout layout)
        : data(nullptr), dataSize(0), vertexLayout(layout)
    {
    }
};

class IndexBuffer
{
public:
    // owns a heap copy of the index data; freed with delete[] (allocated with new char[])
    char* data;
    int dataSize;

    template<typename T>
    void setData(T* data, unsigned int sizeInBytes)
    {
        setData((void*) data, sizeInBytes);
    }

    void setData(void* srcData, unsigned int sizeInBytes)
    {
        delete[] data;
        data = new char[sizeInBytes];
        memcpy(data, srcData, sizeInBytes);
        dataSize = (int)sizeInBytes;
    }

    ~IndexBuffer()
    {
        delete[] data;
    }

    // owns a raw buffer - copying would double-free
    IndexBuffer(const IndexBuffer&) = delete;
    IndexBuffer& operator=(const IndexBuffer&) = delete;

    static IndexBufferPtr create()
    {
        return IndexBufferPtr(new IndexBuffer());
    }

private:
    IndexBuffer() : data(nullptr), dataSize(0) {}
};

}

#endif // IRIS_VERTEXBUFFER_H
