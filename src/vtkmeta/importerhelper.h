#ifndef IMPORTER_HELPER_H
#define IMPORTER_HELPER_H

#include "assettypes.h"

#include <QVector>
#include <QList>
#include <QByteArray>
#include <QImage>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QAtomicInt>

#include <assimp/scene.h>
#include <assimp/material.h>

namespace vtkmeta {

class ImporterHelper
{
public:
    ImporterHelper() = default;
    ~ImporterHelper() = default;

    void beginTextureSession();
    void endTextureSession();

    QVector<TextureMapResult> processTextures(
        const QList<TextureImportTask>& tasks,
        const QString& outputFolder
        );

    static QImage convertAiTextureToQImage(const aiTexture* at);

private:
    QByteArray getTextureRawData(const aiTexture* at);

    const aiTexture* getTexture(
        const aiMaterial* mat,
        aiTextureType type,
        const aiScene* scene,
        QString& texture_name
        );

    bool isValidTexture(const aiTexture* ai,
                               const QString& texture_name);

    TextureMapResult mapTextureProcess(
        const TextureImportTask& task,
        const QString& outputFolder
        );

    void reduceTextureResults(
        QVector<TextureMapResult>& allResults,
        const TextureMapResult& currentResult
        );

    QByteArray imageToByteArray(
        const QImage& img,
        const QString& format = "PNG"
        );

    QMutex texture_mutex_;
    QHash<QString, QString> texture_lists_;

    QAtomicInt session_refcount_;
};

} // namespace vtkmeta

#endif // IMPORTER_HELPER_H
