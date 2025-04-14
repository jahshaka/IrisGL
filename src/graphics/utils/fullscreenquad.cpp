/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016  GPLv3 Jahshaka LLC <coders@jahshaka.com>

This is free software: you may copy, redistribute
and/or modify it under the terms of the GPLv3 License

For more information see the LICENSE file
*************************************************************************/

#include "fullscreenquad.h"
#include <QVector>
#include "../mesh.h"
#include "../shader.h"
#include "../graphicshelper.h"
#include "../graphicsdevice.h"
#include "../vertexlayout.h"
#include "../../core/irisutils.h"
#include <QOpenGLFunctions_3_2_Core>
#include <QOpenGLShaderProgram>

namespace iris
{

FullScreenQuad::FullScreenQuad()
{
    QVector<float> data;
    //TRIANGLE 1
    data.append(-1);data.append(-1);data.append(0);
    data.append(0);data.append(0);

    data.append(1);data.append(-1);data.append(0);
    data.append(1);data.append(0);

    data.append(-1);data.append(1);data.append(0);
    data.append(0);data.append(1);

    //TRIANGLE 2
    data.append(-1);data.append(1);data.append(0);
    data.append(0);data.append(1);

    data.append(1);data.append(-1);data.append(0);
    data.append(1);data.append(0);

    data.append(1);data.append(1);data.append(0);
    data.append(1);data.append(1);

    VertexLayout layout;
    layout.addAttrib(VertexAttribUsage::Position, GL_FLOAT, 3, sizeof(GLfloat) * 3);
    layout.addAttrib(VertexAttribUsage::TexCoord0, GL_FLOAT, 2, sizeof(GLfloat) * 2);

	//mesh = new iris::Mesh(data.data(), data.size() * sizeof(float), 6, layout);
	//mesh = iris::Mesh::create(layout);
	//mesh->set(data.data(), data.size() * sizeof(float));
	vb = iris::VertexBuffer::create(layout);
	vb->setData(data.data(), data.size() * sizeof(float));
    matrix.setToIdentity();

    //todo: inline shader in code
    shader = iris::Shader::load(":assets/shaders/fullscreen.vert",
                                        ":assets/shaders/fullscreen.frag");

    //gl = QOpenGLContext::currentContext()->versionFunctions<QOpenGLFunctions_3_2_Core>();
}

FullScreenQuad::~FullScreenQuad()
{
    //delete mesh;
}

void FullScreenQuad::draw(GraphicsDevicePtr device, bool flipY)
{
	/*
    gl->glUseProgram(shader->programId()); 
	gl->glUniform1i(gl->glGetUniformLocation(shader->programId(), "flipY"), flipY);
	gl->glUniform1i(gl->glGetUniformLocation(shader->programId(), "tex"), 0);
    shader->setUniformValue("matrix", matrix);
    mesh->draw(device);
	*/
	device->setShader(shader);
	device->setShaderUniform("flipY", flipY);
	device->setShaderUniform("tex", 0);
	device->setShaderUniform("matrix", matrix);
	device->setVertexBuffer(vb);
	device->drawPrimitives(GL_TRIANGLES, 0, 6);
	//mesh->draw(device);
}

void FullScreenQuad::draw(GraphicsDevicePtr device, iris::ShaderPtr shader)
{
	device->setShader(shader);
	device->setVertexBuffer(vb);
	device->drawPrimitives(GL_TRIANGLES, 0, 6);
}

/*
void FullScreenQuad::draw(GraphicsDevicePtr device, QOpenGLShaderProgram* shader)
{
    shader->bind();
    mesh->draw(device);
}
*/

}
