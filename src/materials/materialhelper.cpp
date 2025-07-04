/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016  GPLv3 Jahshaka LLC <coders@jahshaka.com>

This is free software: you may copy, redistribute
and/or modify it under the terms of the GPLv3 License

For more information see the LICENSE file
*************************************************************************/

#include <QDir>
#include <QJsonObject>
#include <Qt3DRender/Qt3DRender>
#include <QUuid>

#include "materialhelper.h"
#include "../irisglfwd.h"
#include "assimp/scene.h"
#include "assimp/mesh.h"
#include "assimp/material.h"
#include "assimp/matrix4x4.h"
#include "assimp/vector3.h"
#include "assimp/quaternion.h"
#include "assimp/cimport.h"
#include "defaultmaterial.h"
#include "../graphics/texture2d.h"
#include "../graphics/mesh.h"

namespace iris
{


static QString generateTexGUID() {
    auto id = QUuid::createUuid();
    auto guid = id.toString().remove(0, 1);
    guid.chop(1);
    return guid;
}

QColor getAiMaterialColor(aiMaterial* aiMat, const char* pKey, unsigned int type,
                          unsigned int idx)
{
    aiColor3D col;
    aiMat->Get(pKey, type, idx, col);
    auto color = QColor(col.r * 255, col.g * 255, col.b * 255, 255);

    return color;

}

QString getAiMaterialTexture(aiMaterial* aiMat, aiTextureType texType)
{
    if (aiMat->GetTextureCount(texType) > 0) {
        aiString tex;
        aiMat->GetTexture(texType, 0, &tex);

        return QString(tex.C_Str());
    }

    return QString();
}

float getAiMaterialFloat(aiMaterial* aiMat, const char* pKey, unsigned int type,
                         unsigned int idx)
{
    float floatVal;
    aiMat->Get(pKey, type, idx, floatVal);
    return floatVal;
}

// http://www.assimp.org/lib_html/materials.html
DefaultMaterialPtr MaterialHelper::createMaterial(aiMaterial* aiMat,QString assetPath)
{
    auto mat =  DefaultMaterial::create();
    mat->setDiffuseColor(getAiMaterialColor(aiMat, AI_MATKEY_COLOR_DIFFUSE));
    mat->setSpecularColor(getAiMaterialColor(aiMat, AI_MATKEY_COLOR_SPECULAR));
    mat->setAmbientColor(getAiMaterialColor(aiMat, AI_MATKEY_COLOR_AMBIENT));
    //mat->setEmissiveColor(getAiMaterialColor(aiMat,AI_MATKEY_COLOR_EMISSIVE));

    mat->setShininess(getAiMaterialFloat(aiMat, AI_MATKEY_SHININESS));

    if(!assetPath.isEmpty())
    {
        auto diffuseTex = getAiMaterialTexture(aiMat, aiTextureType_DIFFUSE);
        mat->setDiffuseTexture(Texture2D::load(
                                   QDir::cleanPath(assetPath + QDir::separator() + diffuseTex)));
    }

    return mat;
}

QImage MaterialHelper::loadOMEmbeddedTexture(const aiScene* scene, const QString& texPath, QString& fileName)
{
    const aiTexture* tex = scene->GetEmbeddedTexture(texPath.toStdString().c_str());
    if (!tex) {
        // qWarning() << "Embedded texture not found for path:" << texPath;
        return QImage();
    }

    fileName = QFileInfo(texPath).fileName();

    QImage image = covertAiTextureToImage(tex);

    return image;
}

QImage MaterialHelper::loadGLBEmbeddedTexture(const aiScene *scene,
                                              const QString &texName,
                                              QString& fileName)
{
    bool ok = false;
    int texIndex = texName.mid(texName.indexOf("*")+1, texName.length()).toInt(&ok);
    fileName = "";

    QImage image;

    if (ok && texIndex >= 0 && texIndex < int(scene->mNumTextures)) {
        aiTexture* embeddedTex = scene->mTextures[texIndex];
        QString name = QString(embeddedTex->mFilename.C_Str());
        if (name.isEmpty()) {
            name = generateTexGUID() + QString::number(texIndex);
        }
        fileName = name + "." + QString(embeddedTex->achFormatHint);

        image = covertAiTextureToImage(embeddedTex);
    }

    return image;
}

QImage MaterialHelper::covertAiTextureToImage(const aiTexture *at)
{
    QImage image;
    if (at->mHeight == 0) {
        QByteArray imageData(reinterpret_cast<const char*>(at->pcData), at->mWidth);
        image.loadFromData(imageData);
    } else {
        int width = at->mWidth;
        int height = at->mHeight;
        image = QImage(width, height, QImage::Format_RGBA8888);

        const quint32* pixelData = reinterpret_cast<const quint32*>(at->pcData);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                image.setPixel(x, y, pixelData[y * width + x]);
            }
        }
    }

    return image;
}

void MaterialHelper::extractMaterialData(const aiScene *scene, aiMaterial *aiMat, QString assetPath, MeshMaterialData& mat)
{
    mat.diffuseColor    = getAiMaterialColor(aiMat, AI_MATKEY_COLOR_DIFFUSE);
    mat.specularColor   = getAiMaterialColor(aiMat, AI_MATKEY_COLOR_SPECULAR);
    mat.ambientColor    = getAiMaterialColor(aiMat, AI_MATKEY_COLOR_AMBIENT);
    mat.emissionColor   = getAiMaterialColor(aiMat, AI_MATKEY_COLOR_EMISSIVE);
    mat.shininess       = getAiMaterialFloat(aiMat, AI_MATKEY_SHININESS);

    if (!assetPath.isEmpty()) {
        QString diffuseTex = getAiMaterialTexture(aiMat, aiTextureType_DIFFUSE);
        mat.diffuseTexture = QFileInfo(diffuseTex).isRelative()
                                 ? QDir::cleanPath(QDir(assetPath).filePath(diffuseTex))
                                 : QDir::cleanPath(diffuseTex);

        loadEmbeddedTexture(scene, diffuseTex, assetPath, mat.diffuseTexture, mat.hasEmbeddedDiffTexture);


        QString specularTex = getAiMaterialTexture(aiMat, aiTextureType_SPECULAR);
        mat.specularTexture = QFileInfo(specularTex).isRelative()
                                ? QDir::cleanPath(QDir(assetPath).filePath(specularTex))
                                : QDir::cleanPath(specularTex);

        loadEmbeddedTexture(scene, specularTex, assetPath, mat.specularTexture, mat.hasEmbeddedSpecularTexture);


        QString normalsTex = getAiMaterialTexture(aiMat, aiTextureType_NORMALS);
        mat.normalTexture = QFileInfo(normalsTex).isRelative()
                                ? QDir::cleanPath(QDir(assetPath).filePath(normalsTex))
                                : QDir::cleanPath(normalsTex);

        loadEmbeddedTexture(scene, normalsTex, assetPath, mat.normalTexture, mat.hasEmbeddedNormalTexture);

        // reading normals for some obj's won't always work with aiTextureType_NORMALS, use this as fallback alt.
        if (normalsTex.isEmpty()) {
            normalsTex = getAiMaterialTexture(aiMat, aiTextureType_HEIGHT);
            mat.normalTexture = QFileInfo(normalsTex).isRelative()
                ? QDir::cleanPath(QDir(assetPath).filePath(normalsTex))
                : QDir::cleanPath(normalsTex);

            loadEmbeddedTexture(scene, normalsTex, assetPath, mat.hightTexture, mat.hasEmbeddedHightTexture);
        }
    }
}

void MaterialHelper::loadEmbeddedTexture(const aiScene* scene,
                                         const QString& texName,
                                         const QString& assetPath,
                                         QString& texPath,
                                         bool& hasEmbedded)
{
    if (texPath.isEmpty() || (QFileInfo::exists(texPath) && !QFileInfo(texPath).isDir())) {
        return;
    }

    QString fileName("");
    QImage image;
    if (texName.startsWith("*")) {
        image = loadGLBEmbeddedTexture(scene, texName, fileName);
    } else {
        image = loadOMEmbeddedTexture(scene, texPath, fileName);
    }

    hasEmbedded = false;
    texPath = "";
    if (!image.isNull()) {
        QString imagePath = QDir(assetPath).filePath(fileName);
        image.save(imagePath);

        texPath = imagePath;
        hasEmbedded = true;
    }
}

}
