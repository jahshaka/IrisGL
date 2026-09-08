/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef DEFAULTMATERIAL_H
#define DEFAULTMATERIAL_H

#include <QColor>

#include "document/materials/material.h"

namespace iris
{

class DefaultMaterial : public Material
{
    float textureScale;
    QColor ambientColor;

    bool useNormalTex;
    float normalIntensity;

    Texture2DPtr normalTexture;

    QColor diffuseColor;
    bool useDiffuseTex;
    Texture2DPtr diffuseTexture;

    float shininess;
    bool useSpecularTex;
    QColor specularColor;

    Texture2DPtr specularTexture;

    float reflectionInfluence;
    bool useReflectionTex;

    Texture2DPtr reflectionTexture;

public:

    void setDiffuseTexture(Texture2DPtr tex);
    QString getDiffuseTextureSource();

    void setAmbientColor(QColor col);
    QColor getAmbientColor()
    {
        return ambientColor;
    }

    void setDiffuseColor(QColor col);
    QColor getDiffuseColor();

    void setNormalTexture(Texture2DPtr tex);
    QString getNormalTextureSource();

    void setNormalIntensity(float intensity);
    float getNormalIntensity();

    void setSpecularTexture(Texture2DPtr tex);
    QString getSpecularTextureSource();

    void setSpecularColor(QColor col);
    QColor getSpecularColor();

    void setShininess(float intensity);
    float getShininess();

    void setReflectionTexture(Texture2DPtr tex);
    QString getReflectionTextureSource();

    void setTextureScale(float scale);
    float getTextureScale();

    void setReflectionInfluence(float intensity);
    float getReflectionInfluence();


    static DefaultMaterialPtr create()
    {
        return DefaultMaterialPtr(new DefaultMaterial());
    }

private:
    DefaultMaterial();
};

}

#endif // DEFAULTMATERIAL_H
