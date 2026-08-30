/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include "document/materials/material.h"
#include "document/assets/texture2d.h"
#include "document/assets/shader.h"

namespace iris
{

void Material::addTexture(QString name,Texture2DPtr texture)
{
    // remove texture if it already exists
    if (textures.contains(name)) {
        textures.remove(name);
    }

    textures.insert(name, texture);
}

void Material::removeTexture(QString name)
{
    if (textures.contains(name)) {
        textures.remove(name);
    }
}

bool Material::isFlagEnabled(QString flag)
{
	return flags.contains(flag);
}

void Material::enableFlag(QString flag)
{
	flags.insert(flag);
	if (!!shader) shader->enableFlag(flag);
	if (!!shadowShader) shadowShader->enableFlag(flag);
}

void Material::disableFlag(QString flag)
{
	flags.remove(flag);
	if (!!shader) shader->disableFlag(flag);
	if (!!shadowShader) shadowShader->disableFlag(flag);
}

void Material::createProgramFromShaderSource(QString vsFile, QString fsFile)
{
	setShader(Shader::load(vsFile, fsFile));
}

MaterialPtr Material::fromShader(ShaderPtr shader)
{
	Material* mat = new Material();
	mat->setShader(shader);

	return MaterialPtr(mat);
}

void Material::setShader(ShaderPtr shader)
{
	this->shader = shader;
	if (!!shader) {
		for (auto flag : flags)
			shader->enableFlag(flag);
	}
}

void Material::setShadowShader(ShaderPtr shader)
{
	this->shadowShader = shader;
	if (!!shader) {
		for (auto flag : flags)
			shader->enableFlag(flag);
	}
}

void Material::setBlendState(const iris::BlendState& blendState) {
	this->renderStates.blendState = blendState;
}

void Material::setRasterizerState(const iris::RasterizerState& rasterState)
{
	this->renderStates.rasterState = rasterState;
}

void Material::setDepthState(const iris::DepthState& depthState)
{
	this->renderStates.depthState = depthState;
}

}
