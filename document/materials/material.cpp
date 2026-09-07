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
}

void Material::disableFlag(QString flag)
{
	flags.remove(flag);
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
