/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016  GPLv3 Jahshaka LLC <coders@jahshaka.com>

This is free software: you may copy, redistribute
and/or modify it under the terms of the GPLv3 License

For more information see the LICENSE file
*************************************************************************/

#ifndef FULLSCREENQUAD_H
#define FULLSCREENQUAD_H

#include <QMatrix4x4>
#include "../../irisglfwd.h"

namespace iris
{

class FullScreenQuad
{
public:
    FullScreenQuad();
    ~FullScreenQuad();

	void draw(GraphicsDevicePtr device, bool flipY = false);
	void draw(GraphicsDevicePtr device, iris::ShaderPtr shader);
    //void draw(GraphicsDevicePtr device, QOpenGLShaderProgram* shader);

    iris::ShaderPtr shader;
    iris::MeshPtr mesh;
	iris::VertexBufferPtr vb;
    QMatrix4x4 matrix;
};
}

#endif // FULLSCREENQUAD_H
