/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016  GPLv3 Jahshaka LLC <coders@jahshaka.com>

This is free software: you may copy, redistribute
and/or modify it under the terms of the GPLv3 License

For more information see the LICENSE file
*************************************************************************/

#ifndef MATERIALHELPER_H
#define MATERIALHELPER_H

#include <QColor>
#include <Qt3DRender/Qt3DRender>

#include "../irisglfwd.h"
#include "../graphics/mesh.h"

class aiMaterial;

namespace iris
{

class PaintedTextureImage : public Qt3DRender::QPaintedTextureImage
{
public:
    void setImage(QImage &i) {
        image_ = i;
        setSize(i.size());
    }

    virtual void paint(QPainter *painter) override {
        painter->drawImage(0, 0, image_);
    }

private:
    QImage image_;
};

class MaterialHelper
{
public:
    static DefaultMaterialPtr createMaterial(aiMaterial* aiMat, QString assetPath);
    static void extractMaterialData(const aiScene *scene, aiMaterial* aiMat, QString assetPath, MeshMaterialData& data);

private:
    static void loadEmbeddedTexture(const aiScene* scene,
                                    const QString& texName,
                                    const QString& assetPath,
                                    QString& texPath,
                                    bool& hasEmbedded);

    static QImage loadOMEmbeddedTexture(const aiScene* scene,
                                        const QString& texPath,
                                        QString& fileName);

    static QImage loadGLBEmbeddedTexture(const aiScene* scene,
                                         const QString& texName,
                                         QString& fileName);

    static QImage covertAiTextureToImage(const aiTexture* at);
//    static void updateTexure(const QString& assetPath, const QString& fileName, const QImage& image);
};

}

#endif // MATERIALHELPER_H
