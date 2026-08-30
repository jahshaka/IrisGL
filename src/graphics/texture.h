/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef TEXTURE_H
#define TEXTURE_H

#include <QSharedPointer>
#include <QString>

namespace iris
{

// Document-side texture asset base: an asset path plus dimensions. The GL
// object half died with the legacy renderer at step 14; the engine loads
// textures itself from `source` (or from the kept QImage for generated ones).
class Texture
{
public:
    QString source;

    virtual int getWidth() { return 0; }
    virtual int getHeight() { return 0; }

    virtual ~Texture(){}
};

}

#endif // TEXTURE_H
