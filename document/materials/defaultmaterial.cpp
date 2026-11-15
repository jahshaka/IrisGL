/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/


#include "core/math/vec.h"
#include "document/materials/defaultmaterial.h"

#include <QFile>
#include <QTextStream>
#include <QtMinMax>
#include <QColor>

#include "document/materials/material.h"
#include "document/assets/texture.h"
#include "document/assets/texture2d.h"
#include "core/irisutils.h"


namespace iris
{

DefaultMaterial::DefaultMaterial()
{
    //program->bind();
    //program->setUniformValue("u_useDiffuseTex",false);
    //program->setUniformValue("u_useNormalTex",false);
    //program->setUniformValue("u_useReflectionTex",false);
    //program->setUniformValue("u_useSpecularTex",false);
    //program->setUniformValue("u_material.diffuse",iris::Vec3(1,0,0));

    textureScale = 1.0f;
    ambientColor = QColor(0,0,0);

    useNormalTex = false;
    normalIntensity = 1.0f;

    diffuseColor = QColor(255,255,255);
    useDiffuseTex = false;

    shininess = 20.0f;
    useSpecularTex = false;
    specularColor = QColor(20,20,20);

    reflectionInfluence = 0.0f;
    useReflectionTex = false;

    this->setRenderLayer((int)RenderLayer::Opaque);
}

void DefaultMaterial::setDiffuseTexture(QSharedPointer<Texture2D> tex)
{
    if(!!tex)
    {
        useDiffuseTex = true;
        addTexture("u_diffuseTexture",tex);
    }
    else
    {
        useDiffuseTex = false;
        removeTexture("u_diffuseTexture");
    }

    diffuseTexture=tex;
}

QString DefaultMaterial::getDiffuseTextureSource()
{
    if(!!diffuseTexture)
    {
        return diffuseTexture->source;
    }

    return QString();
}

void DefaultMaterial::setAmbientColor(QColor col)
{
    ambientColor = col;
}

void DefaultMaterial::setDiffuseColor(QColor col)
{
    diffuseColor = col;
}

QColor DefaultMaterial::getDiffuseColor()
{
    return diffuseColor;
}

void DefaultMaterial::setNormalTexture(QSharedPointer<Texture2D> tex)
{
    if(!!tex)
    {
        useNormalTex = true;
        addTexture("u_normalTexture",tex);
    }
    else
    {
        useNormalTex = false;
        removeTexture("u_normalTexture");
    }

    normalTexture=tex;
}

QString DefaultMaterial::getNormalTextureSource()
{
    if(!!normalTexture)
    {
        return normalTexture->source;
    }

    return QString();
}

void DefaultMaterial::setNormalIntensity(float intensity)
{
    normalIntensity = intensity;
}

float DefaultMaterial::getNormalIntensity()
{
    return normalIntensity;
}


void DefaultMaterial::setSpecularTexture(QSharedPointer<Texture2D> tex)
{
    if(!!tex)
    {
        useSpecularTex = true;
        addTexture("u_specularTexture",tex);
    }
    else
    {
        useSpecularTex = false;
        removeTexture("u_specularTexture");
    }

    specularTexture=tex;
}

QString DefaultMaterial::getSpecularTextureSource()
{
    if(!!specularTexture)
    {
        return specularTexture->source;
    }

    return QString();
}

void DefaultMaterial::setSpecularColor(QColor col)
{
    specularColor = col;
}

QColor DefaultMaterial::getSpecularColor()
{
    return specularColor;
}

void DefaultMaterial::setShininess(float shininess)
{
    this->shininess = shininess;
}

float DefaultMaterial::getShininess()
{
    return shininess;
}


void DefaultMaterial::setReflectionTexture(QSharedPointer<Texture2D> tex)
{
    if(!!tex)
    {

        useReflectionTex = true;
        addTexture("u_reflectionTexture",tex);
    }
    else
    {
        useReflectionTex = false;
        removeTexture("u_reflectionTexture");
    }

    reflectionTexture=tex;
}

QString DefaultMaterial::getReflectionTextureSource()
{
    if(!!reflectionTexture)
    {
        return reflectionTexture->source;
    }

    return QString();
}

void DefaultMaterial::setReflectionInfluence(float intensity)
{
    reflectionInfluence = intensity;
}

float DefaultMaterial::getReflectionInfluence()
{
    return reflectionInfluence;
}

void DefaultMaterial::setTextureScale(float scale)
{
    this->textureScale = scale;
}

float DefaultMaterial::getTextureScale()
{
    return textureScale;
}

}
