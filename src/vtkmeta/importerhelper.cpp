#include "ImporterHelper.h"

#include "assettypes.h"          // 核心结构体定义

#include <QFileInfo>
#include <QDir>
#include <QFile>
#include <QBuffer>
#include <QUuid>
#include <QDebug>
#include <QImage>
#include <QMutex>
#include <QMutexLocker>

#include <QtConcurrent/QtConcurrent>
#include <assimp/texture.h>

namespace vtkmeta {

QMutex g_textureGuidMapMutex;
QHash<QString, QString> g_textureGuidMap;


QByteArray ImporterHelper::imageToByteArray(const QImage& img,
                                            const QString& format)
{
    if (img.isNull()) return QByteArray();
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);

    img.save(&buffer, format.toLatin1().constData());
    return bytes;
}

QByteArray ImporterHelper::getTextureRawData(
    const aiMaterial* mat,
    aiTextureType type,
    const QString& modelFilePath,
    const aiScene* scene,
    QString& texture_name)
{
    aiString path;
    if (mat->GetTexture(type, 0, &path) != AI_SUCCESS) return {};

    QString texStr = QString::fromUtf8(path.C_Str());

    // a) 嵌入式纹理
    if (texStr.startsWith("*")) {
        bool ok = false;
        int idx = texStr.mid(1).toInt(&ok);
        if (ok && idx >= 0 && idx < int(scene->mNumTextures)) {
            const aiTexture* at = scene->mTextures[idx];

            texture_name = QString("tex_%1.png").arg(idx);

            QImage img = convertAiTextureToQImage(at);
            return imageToByteArray(img, "PNG");
        }

        return {};
    }

    texture_name = QFileInfo(texStr).fileName();
    const aiTexture* embeddedTex = scene->GetEmbeddedTexture(path.C_Str());
    if (embeddedTex) {
        QImage img = convertAiTextureToQImage(embeddedTex);
        return imageToByteArray(img, "PNG");
    }

    return {};
}

// ----------------------------------------------------------------
// 2. Map Function: 并行执行 I/O 和文件名去重
// ----------------------------------------------------------------
TextureMapResult ImporterHelper::mapTextureProcess(
    const TextureImportTask& task,
    const QString& outputFolder)
{
    TextureMapResult result;
    result.mesh_name_ = task.mesh_name;
    result.texture_type_ = task.texture_type;
    result.is_new_asset = false;

    QString texture_name("");
    QByteArray rawData = getTextureRawData(task.mat,
                                           task.texture_type,
                                           task.model_file_path,
                                           task.scene,
                                           texture_name);
    if (rawData.isEmpty()) {
        return result;
    }

    aiString path;
    task.mat->GetTexture(task.texture_type, 0, &path);
    QString fullPath = QDir(outputFolder).filePath(texture_name);

    QString assignedGuid;

    {
        QMutexLocker locker(&g_textureGuidMapMutex);
        if (g_textureGuidMap.contains(texture_name)) {
            assignedGuid = g_textureGuidMap.value(texture_name);
        } else if (QFileInfo::exists(fullPath)) {
            assignedGuid = QUuid::createUuid().toString().remove('{', '}').remove('-');
            g_textureGuidMap.insert(texture_name, assignedGuid);
        } else {
            assignedGuid = QUuid::createUuid().toString().remove('{', '}').remove('-');
            g_textureGuidMap.insert(texture_name, assignedGuid);
            result.is_new_asset = true;
        }
    }

    result.guid_ = assignedGuid;
    result.filename_ = texture_name;

    if (result.is_new_asset) {
        QDir().mkpath(outputFolder);
        if (QFile outFile(fullPath); outFile.open(QIODevice::WriteOnly)) {
            outFile.write(rawData);
            outFile.close();
        } else {
            qWarning() << "ImporterHelper: Failed to write new texture file:" << fullPath;
            result.is_new_asset = false;
        }
    }

    return result;
}

void ImporterHelper::reduceTextureResults(
    QVector<TextureMapResult>& allResults,
    const TextureMapResult& currentResult)
{
    allResults.append(currentResult);
}

QVector<TextureMapResult> ImporterHelper::processTextures(
    const QList<TextureImportTask>& tasks,
    const QString& outputFolder)
{
    QFuture<QVector<TextureMapResult>> future = QtConcurrent::mappedReduced(
        tasks,
        [&](const TextureImportTask& task) {
            return mapTextureProcess(task, outputFolder);
        },
        [&](QVector<TextureMapResult>& aggregate, const TextureMapResult& currentResult) {
            reduceTextureResults(aggregate, currentResult);
        },
        QtConcurrent::SequentialReduce
        );

    future.waitForFinished();

    return future.result();
}

QImage vtkmeta::ImporterHelper::convertAiTextureToQImage(const aiTexture *at)
{
    if (!at) {
        return QImage();
    }

    if (at->mHeight == 0) { // compressed (e.g. PNG/JPEG in memory)
        QByteArray bytes(reinterpret_cast<const char*>(at->pcData), static_cast<int>(at->mWidth));
        QImage img;
        if (!img.loadFromData(bytes)) {
            qWarning() << "AssimpModelLoader: Failed to load compressed embedded texture";
            return QImage();
        }

        return img;
    }

    int w = at->mWidth;
    int h = at->mHeight;
    const aiTexel* texels = reinterpret_cast<const aiTexel*>(at->pcData);

    if (!texels) {
        return QImage();
    }

    // aiTexel is RGBA (4 bytes)
    QImage img(reinterpret_cast<const uchar*>(texels),
               w,
               h,
               QImage::Format_RGBA8888);

    return img.copy();
}

} // namespace vtkmeta
