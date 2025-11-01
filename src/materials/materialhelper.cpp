#include "materialhelper.h"
#include <QDir>
#include <QUuid>
#include <QFileInfo>
#include <QDebug>
#include <QImageWriter>
#include <QtConcurrent>
#include "defaultmaterial.h"
#include "../graphics/texture2d.h"

namespace iris {

QVector<MaterialHelper::SaveTask> MaterialHelper::g_textureSaveTasks;
QMutex MaterialHelper::g_saveMutex;
QSet<QString> MaterialHelper::g_savedPaths;

static QString generateTexGUID() {
    auto id = QUuid::createUuid();
    auto guid = id.toString().remove(0, 1);
    guid.chop(1);
    return guid;
}

QColor getAiMaterialColor(aiMaterial* aiMat, const char* pKey, unsigned int type = 0, unsigned int idx = 0)
{
    aiColor3D col;
    aiMat->Get(pKey, type, idx, col);
    return QColor(col.r * 255, col.g * 255, col.b * 255, 255);
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

float getAiMaterialFloat(aiMaterial* aiMat, const char* pKey, unsigned int type = 0, unsigned int idx = 0)
{
    float val = 0.0f;
    aiMat->Get(pKey, type, idx, val);
    return val;
}

QImage MaterialHelper::convertAiTextureToImage(const aiTexture *at)
{
    if (!at) return QImage();

    if (at->mHeight == 0) {
        QImage image;
        QByteArray data(reinterpret_cast<const char*>(at->pcData), at->mWidth);
        image.loadFromData(data);
        return image;
    }

    int width = at->mWidth;
    int height = at->mHeight;
    const aiTexel* texelData = reinterpret_cast<const aiTexel*>(at->pcData);
    QImage image(reinterpret_cast<const uchar*>(texelData), width, height, QImage::Format_RGBA8888);
    return image.copy();
}

DefaultMaterialPtr MaterialHelper::createMaterial(aiMaterial* aiMat, QString assetPath)
{
    auto mat = DefaultMaterial::create();
    mat->setDiffuseColor(getAiMaterialColor(aiMat, AI_MATKEY_COLOR_DIFFUSE));
    mat->setSpecularColor(getAiMaterialColor(aiMat, AI_MATKEY_COLOR_SPECULAR));
    mat->setAmbientColor(getAiMaterialColor(aiMat, AI_MATKEY_COLOR_AMBIENT));
    mat->setShininess(getAiMaterialFloat(aiMat, AI_MATKEY_SHININESS));

    if (!assetPath.isEmpty()) {
        QString diffuseTex = getAiMaterialTexture(aiMat, aiTextureType_DIFFUSE);
        if (!diffuseTex.isEmpty()) {
            mat->setDiffuseTexture(Texture2D::load(QDir::cleanPath(assetPath + QDir::separator() + diffuseTex)));
        }
    }
    return mat;
}

void MaterialHelper::saveTextureAsync(const QImage &image, const QString &path)
{
    if (image.isNull() || path.isEmpty()) return;

    QMutexLocker locker(&g_saveMutex);
    if (g_savedPaths.contains(path)) return;
    g_savedPaths.insert(path);
    locker.unlock();

    QDir().mkpath(QFileInfo(path).absolutePath());

    SaveTask task;
    task.path = path;
    task.future = QtConcurrent::run([image, path]() {
        QImageWriter writer(path, "PNG");
        writer.setCompression(1); // 压缩等级 1，加快写盘
        if (!writer.write(image)) {
            qWarning() << "Failed to save texture:" << path;
        }
    });

    locker.relock();
    g_textureSaveTasks.append(task);
}

void MaterialHelper::waitForAllTextureSaves()
{
    QVector<QFuture<void>> futures;
    {
        QMutexLocker locker(&g_saveMutex);
        for (auto &task : g_textureSaveTasks) {
            futures.append(task.future);
        }
        g_textureSaveTasks.clear();
    }

    for (auto &f : futures) {
        if (f.isRunning() || f.isStarted()) f.waitForFinished();
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
    texPath.clear();

    if (!image.isNull()) {
        QString imagePath = QDir(assetPath).filePath(fileName);
        texPath = imagePath;
        hasEmbedded = true;

        if (!QFileInfo::exists(texPath)) {
            saveTextureAsync(image, imagePath);
        }
    }
}


QImage MaterialHelper::loadOMEmbeddedTexture(const aiScene* scene, const QString& texPath, QString& fileName)
{
    const aiTexture* tex = scene->GetEmbeddedTexture(texPath.toStdString().c_str());
    if (!tex) {
        // qWarning() << "Embedded texture not found for path:" << texPath;
        return QImage();
    }

    fileName = QFileInfo(texPath).fileName();

    QImage image = convertAiTextureToImage(tex);

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
            name = /*generateTexGUID()*/ QString("_") + QString::number(texIndex);
        }
        fileName = name + "." + QString(embeddedTex->achFormatHint);

        image = convertAiTextureToImage(embeddedTex);
    }

    return image;
}


void MaterialHelper::extractMaterialData(const aiScene *scene,
                    aiMaterial *aiMat,
                    QString assetPath,
                    MeshMaterialData& mat)
{
    mat.diffuseColor  = getAiMaterialColor(aiMat, AI_MATKEY_COLOR_DIFFUSE);
    mat.specularColor = getAiMaterialColor(aiMat, AI_MATKEY_COLOR_SPECULAR);
    mat.ambientColor  = getAiMaterialColor(aiMat, AI_MATKEY_COLOR_AMBIENT);
    mat.emissionColor = getAiMaterialColor(aiMat, AI_MATKEY_COLOR_EMISSIVE);
    mat.shininess     = getAiMaterialFloat(aiMat, AI_MATKEY_SHININESS);

    if (assetPath.isEmpty()) return;

    // ------------------------
    // Diffuse
    // ------------------------
    QString diffuseTex = getAiMaterialTexture(aiMat, aiTextureType_DIFFUSE);
    mat.diffuseTexture = QFileInfo(diffuseTex).isRelative()
                             ? QDir::cleanPath(QDir(assetPath).filePath(diffuseTex))
                             : QDir::cleanPath(diffuseTex);

    loadEmbeddedTexture(scene, diffuseTex, assetPath, mat.diffuseTexture, mat.hasEmbeddedDiffTexture);

    // ------------------------
    // Specular
    // ------------------------
    QString specularTex = getAiMaterialTexture(aiMat, aiTextureType_SPECULAR);
    mat.specularTexture = QFileInfo(specularTex).isRelative()
                              ? QDir::cleanPath(QDir(assetPath).filePath(specularTex))
                              : QDir::cleanPath(specularTex);

    loadEmbeddedTexture(scene, specularTex, assetPath, mat.specularTexture, mat.hasEmbeddedSpecularTexture);

    // ------------------------
    // Normals
    // ------------------------
    QString normalsTex = getAiMaterialTexture(aiMat, aiTextureType_NORMALS);
    mat.normalTexture = QFileInfo(normalsTex).isRelative()
                            ? QDir::cleanPath(QDir(assetPath).filePath(normalsTex))
                            : QDir::cleanPath(normalsTex);

    loadEmbeddedTexture(scene, normalsTex, assetPath, mat.normalTexture, mat.hasEmbeddedNormalTexture);

    // ------------------------
    // Fallback for height maps if normal map missing
    // ------------------------
    if (normalsTex.isEmpty()) {
        normalsTex = getAiMaterialTexture(aiMat, aiTextureType_HEIGHT);
        mat.hightTexture = QFileInfo(normalsTex).isRelative()
                               ? QDir::cleanPath(QDir(assetPath).filePath(normalsTex))
                               : QDir::cleanPath(normalsTex);

        loadEmbeddedTexture(scene, normalsTex, assetPath, mat.hightTexture, mat.hasEmbeddedHightTexture);
    }

    waitForAllTextureSaves();
}


} // namespace iris
