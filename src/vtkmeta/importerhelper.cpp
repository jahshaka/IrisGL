#include "importerHelper.h"

#include "assettypes.h"

#include <QFileInfo>
#include <QDir>
#include <QFile>
#include <QBuffer>
#include <QUuid>
#include <QDebug>
#include <QImage>

#include <QtConcurrent/QtConcurrent>
#include <assimp/texture.h>

namespace vtkmeta {

QByteArray ImporterHelper::imageToByteArray(const QImage& img,
                                            const QString& format)
{
    if (img.isNull()) {
        return QByteArray();
    }

    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);

    img.save(&buffer, format.toLatin1().constData());

    return bytes;
}

QByteArray ImporterHelper::getTextureRawData(const aiTexture* at)
{
    QImage img = convertAiTextureToQImage(at);

    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);

    img.save(&buffer, "PNG");

    buffer.close();

    return bytes;
}

const aiTexture *vtkmeta::ImporterHelper::getTexture(const TextureImportTask &task,
                                                     const QString& assetFolder,
                                                     QString& refineTextureName)
{
    aiString path;
    if (task.mat_->GetTexture(task.texture_type, 0, &path) != AI_SUCCESS) {
        return {};
    }

    QString originalTextureName = QString::fromUtf8(path.C_Str());

    if (originalTextureName.startsWith("*")) {
        bool ok = false;
        int idx = originalTextureName.mid(1).toInt(&ok);

        if (ok && idx >= 0 && idx < int(task.scene_->mNumTextures)) {
            const aiTexture* at = task.scene_->mTextures[idx];

            refineTextureName = QString("tex_%1.png").arg(idx);

            return at;
        }

        return {};
    }

    refineTextureName = QFileInfo(originalTextureName).fileName();

    const aiTexture* embeddedTex = task.scene_->GetEmbeddedTexture(path.C_Str());
    if (embeddedTex) {
        return embeddedTex;
    } else {
        // external, keep original relative path
        refineTextureName = originalTextureName;

        QFileInfo modelFileInfo(task.model_file_path_);
        QString originalTexFilePath = QDir(modelFileInfo.absolutePath()).absoluteFilePath(originalTextureName);
        QFileInfo originalTexFileInfo(originalTexFilePath);
        if (!originalTexFileInfo.exists()) {
            return {};
        }

        QString newTextureFilePath = QDir(assetFolder).absoluteFilePath(originalTextureName);
        QFileInfo newTexFileInfo(newTextureFilePath);
        QDir dir(newTexFileInfo.absolutePath());

        if (!dir.exists()) {
            QDir().mkpath(newTexFileInfo.absolutePath());
        }

        QFile::copy(originalTexFilePath, newTextureFilePath);
    }

    return {};

}

TextureMapResult ImporterHelper::mapTextureProcess(
    const TextureImportTask& task,
    const QString& outputFolder)
{
    TextureMapResult result;

    result.mesh_name_ = task.mesh_name_;
    result.texture_type_ = task.texture_type;
    result.is_new_asset = false;
    result.mesh_index_ = task.mesh_index_;

    QString texture_name("");
    const aiTexture* embeddedTex = getTexture(task,
                                              outputFolder,
                                              texture_name);

    if (!embeddedTex && texture_name.isEmpty()) {
        return result;
    }

    QString fullPath = QDir(outputFolder).filePath(texture_name);
    result.file_path_ = fullPath;
    result.filename_ = texture_name;

    QString assignedGuid;

    {
        QMutexLocker locker(&texture_mutex_);
        if (texture_lists_.contains(texture_name)) {
            result.guid_ = texture_lists_.value(texture_name);

            return result;
        }

        if (QFileInfo::exists(fullPath)) {
            assignedGuid = QUuid::createUuid().toString();
            texture_lists_.insert(texture_name, assignedGuid);
            result.guid_ = assignedGuid;

            return result;
        }


        assignedGuid = QUuid::createUuid().toString();
        texture_lists_.insert(texture_name, assignedGuid);
        result.guid_ = assignedGuid;
        result.is_new_asset = true;
    }

    if (result.is_new_asset) {
        if (QFile outFile(fullPath); outFile.open(QIODevice::WriteOnly)) {

            QByteArray rawData = getTextureRawData(embeddedTex);
            if (rawData.isEmpty()) {
                outFile.close();
                result.is_new_asset = false;
                return result;
            }

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

void vtkmeta::ImporterHelper::beginTextureSession()
{
    int prev = session_refcount_.fetchAndAddRelaxed(1);
    if (prev == 0) {
        QMutexLocker locker(&texture_mutex_);
    }
}

void ImporterHelper::endTextureSession()
{
    int prev = session_refcount_.fetchAndAddRelaxed(-1);
    if (prev <= 1) {
        QMutexLocker locker(&texture_mutex_);
        texture_lists_.clear();
    }
}

QVector<TextureMapResult> ImporterHelper::processTextures(
    const QList<TextureImportTask>& tasks,
    const QString& outputFolder)
{
    if (session_refcount_.loadRelaxed() == 0) {
        QMutexLocker locker(&texture_mutex_);
        texture_lists_.clear();
    }

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

QImage ImporterHelper::convertAiTextureToQImage(const aiTexture *at)
{
    if (!at) {
        return QImage();
    }

    if (at->mHeight == 0) { // compressed (e.g. PNG/JPEG in memory)
        QByteArray bytes(reinterpret_cast<const char*>(at->pcData), static_cast<int>(at->mWidth));
        QImage img;
        if (!img.loadFromData(bytes)) {
            qWarning() << "ImporterHelper: Failed to load compressed embedded texture";
            return QImage();
        }

        return img;
    }

    int w = at->mWidth;
    int h = at->mHeight;
    const aiTexel* texels = reinterpret_cast<const aiTexel*>(at->pcData);

    if (!texels || w <= 0 || h <= 0) {
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
