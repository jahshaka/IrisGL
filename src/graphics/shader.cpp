/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include "shader.h"
#include "graphicshelper.h"

namespace iris
{

Shader::~Shader()
{
}

void Shader::setVertexShader(QString vertexShader)
{
	this->vertexShader = vertexShader;
}

void Shader::setFragmentShader(QString fragmentShader)
{
	this->fragmentShader = fragmentShader;
}

bool Shader::isFlagEnabled(QString flag)
{
	return flags.contains(flag);
}

void Shader::enableFlag(QString flag)
{
	flags.insert(flag);
}

void Shader::disableFlag(QString flag)
{
	flags.remove(flag);
}

ShaderPtr Shader::load(QString vertexShaderFile,QString fragmentShaderFile)
{
	QString vertexShader = GraphicsHelper::loadAndProcessShader(vertexShaderFile);
	QString fragmentShader = GraphicsHelper::loadAndProcessShader(fragmentShaderFile);
    return create(vertexShader,fragmentShader);
}

ShaderPtr Shader::create(QString vertexShader, QString fragmentShader)
{
	auto shader = new Shader();
	shader->setVertexShader(vertexShader);
	shader->setFragmentShader(fragmentShader);
    return ShaderPtr(shader);
}

ShaderPtr Shader::create()
{
    auto shader = new Shader();
	return ShaderPtr(shader);
}

Shader::Shader()
{
    shaderId = generateNodeId();
}

long Shader::generateNodeId()
{
    return nextId++;
}

long Shader::nextId = 0;

}
