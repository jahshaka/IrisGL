#ifndef IMPORTER_HELPER_H
#define IMPORTER_HELPER_H

#include "assettypes.h"

#include <QVector>
#include <QList>
#include <QByteArray>
#include <QImage>

#include <assimp/scene.h>
#include <assimp/material.h>

namespace vtkmeta {

class ImporterHelper
{
public:
    QVector<TextureMapResult> processTextures(
        const QList<TextureImportTask>& tasks,
        const QString& outputFolder
        );

    static QImage convertAiTextureToQImage(const aiTexture* at);

private:
    static QByteArray getTextureRawData(
        const aiMaterial* mat,
        aiTextureType type,
        const QString& modelFilePath,
        const aiScene* scene,
        QString& texture_name
        );

    static TextureMapResult mapTextureProcess(
        const TextureImportTask& task,
        const QString& outputFolder
        );

    static void reduceTextureResults(
        QVector<TextureMapResult>& allResults,
        const TextureMapResult& currentResult
        );

    static QByteArray imageToByteArray(
        const QImage& img,
        const QString& format = "PNG"
        );
};

} // namespace vtkmeta

#endif // IMPORTER_HELPER_H
