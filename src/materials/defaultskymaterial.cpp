/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016  GPLv3 Jahshaka LLC <coders@jahshaka.com>

This is free software: you may copy, redistribute
and/or modify it under the terms of the GPLv3 License

For more information see the LICENSE file
*************************************************************************/

#include "defaultskymaterial.h"
#include "core/irisutils.h"
#include "graphics/renderitem.h"
#include "scenegraph/scene.h"

namespace iris
{

DefaultSkyMaterial::DefaultSkyMaterial()
{
    createProgramFromShaderSource(":assets/shaders/defaultsky.vert",
                                  ":assets/shaders/flatsky.frag");
    setTextureCount(1);
    color = QColor("purple"); // If you see this, there's an issue
    setRenderLayer(static_cast<int>(RenderLayer::Background));
}

void DefaultSkyMaterial::setSkyTexture(Texture2DPtr tex)
{
    texture = tex;
    if (!!tex)  this->addTexture("skybox", tex);
    else        this->removeTexture("skybox");
}

void DefaultSkyMaterial::clearSkyTexture()
{
    texture.clear();
    removeTexture("skybox");
}

Texture2DPtr DefaultSkyMaterial::getSkyTexture()
{
    return texture;
}

void DefaultSkyMaterial::begin(GraphicsDevicePtr device,ScenePtr scene)
{
    Material::beginCube(device, scene);

    switch (static_cast<int>(scene->skyType)) {
        case static_cast<int>(SkyType::REALISTIC): {
            setUniformValue("reileigh",         scene->skyRealistic.reileigh);
            setUniformValue("luminance",        scene->skyRealistic.luminance);
            setUniformValue("mieCoefficient",   scene->skyRealistic.mieCoefficient);
            setUniformValue("mieDirectionalG",  scene->skyRealistic.mieDirectionalG);
            setUniformValue("turbidity",        scene->skyRealistic.turbidity);
            setUniformValue("sunPosition",      QVector3D(scene->skyRealistic.sunPosX,
                                                          scene->skyRealistic.sunPosY,
                                                          scene->skyRealistic.sunPosZ));
        }

        case static_cast<int>(SkyType::GRADIENT): {
            setUniformValue("color_top", scene->gradientTop);
            setUniformValue("color_mid", scene->gradientMid);
            setUniformValue("color_bot", scene->gradientBot);
            setUniformValue("middle_offset", scene->gradientOffset);
			break;
        }

        case static_cast<int>(SkyType::SINGLE_COLOR):
        default: this->setUniformValue("color", scene->skyColor);
    }

    Material::endCube(device, scene);
}

void DefaultSkyMaterial::end(GraphicsDevicePtr device,ScenePtr scene)
{
    Material::end(device, scene);
}

DefaultSkyMaterialPtr DefaultSkyMaterial::create()
{
    return QSharedPointer<DefaultSkyMaterial>(new DefaultSkyMaterial());
}

void DefaultSkyMaterial::switchSkyShader(const QString &vertexShader, const QString &fragmentShader)
{
    createProgramFromShaderSource(vertexShader, fragmentShader);
}

}
