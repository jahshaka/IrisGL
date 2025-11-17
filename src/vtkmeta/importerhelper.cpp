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
    return imageToByteArray(img, "PNG");
}

const aiTexture *ImporterHelper::getTexture(const aiMaterial *mat,
                                              aiTextureType type,
                                              const aiScene *scene,
                                              QString &texture_name)
{
    aiString path;
    if (mat->GetTexture(type, 0, &path) != AI_SUCCESS) {
        return {};
    }

    QString texStr = QString::fromUtf8(path.C_Str());

    if (texStr.startsWith("*")) {
        bool ok = false;
        int idx = texStr.mid(1).toInt(&ok);

        if (ok && idx >= 0 && idx < int(scene->mNumTextures)) {
            const aiTexture* at = scene->mTextures[idx];

            texture_name = QString("tex_%1.png").arg(idx);

            return at;
        }

        return {};
    }

    texture_name = QFileInfo(texStr).fileName();
    const aiTexture* embeddedTex = scene->GetEmbeddedTexture(path.C_Str());

    return embeddedTex;
}

bool vtkmeta::ImporterHelper::isValidTexture(const aiTexture *ai,
                                             const QString &texture_name)
{
    if (!ai || texture_name.isEmpty()) {
        return false;
    }

    {
        QMutexLocker locker(&texture_mutex_);
    }

    return true;
}

TextureMapResult ImporterHelper::mapTextureProcess(
    const TextureImportTask& task,
    const QString& outputFolder)
{
    TextureMapResult result;
    result.mesh_name_ = task.mesh_name;
    result.texture_type_ = task.texture_type;
    result.is_new_asset = false;

    QString texture_name("");
    const aiTexture* embeddedTex = getTexture(task.mat,
                                                task.texture_type,
                                                task.scene,
                                                texture_name);

    if (!embeddedTex || texture_name.isEmpty()) {
        return result;
    }


    QString fullPath = QDir(outputFolder).filePath(texture_name);

    {
        QMutexLocker locker(&texture_mutex_);

        if (texture_lists_.contains(texture_name)) {
            result.guid_ = texture_lists_.value(texture_name);
            result.filename_ = texture_name;
            result.file_path_ = fullPath;

            return result;
        }
    }

    QString assignedGuid;

    {
        QMutexLocker locker(&texture_mutex_);
        if (texture_lists_.contains(texture_name)) {
            assignedGuid = texture_lists_.value(texture_name);
        } else if (QFileInfo::exists(fullPath)) {
            assignedGuid = QUuid::createUuid().toString();
            texture_lists_.insert(texture_name, assignedGuid);
        } else {
            assignedGuid = QUuid::createUuid().toString();
            texture_lists_.insert(texture_name, assignedGuid);
            result.is_new_asset = true;
        }
    }

    result.guid_ = assignedGuid;
    result.filename_ = texture_name;
    result.file_path_ = fullPath;

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

QVector<TextureMapResult> ImporterHelper::processTextures(
    const QList<TextureImportTask>& tasks,
    const QString& outputFolder)
{
    {
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
