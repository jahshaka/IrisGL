#include "import/materialhelper.h"
#include <QDir>
#include <QUuid>
#include <QFileInfo>
#include <QDebug>
#include <QImageWriter>
#include <QtConcurrent>
#include "document/materials/defaultmaterial.h"
#include "document/assets/texture2d.h"

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

QString MaterialHelper::sniffImageExtension(const unsigned char* d, int len)
{
    if (!d || len < 4) return QString();
    if (d[0] == 0xFF && d[1] == 0xD8) return QStringLiteral("jpg");
    if (d[0] == 0x89 && d[1] == 0x50 && d[2] == 0x4E && d[3] == 0x47) return QStringLiteral("png");
    if (d[0] == 'D' && d[1] == 'D' && d[2] == 'S' && d[3] == ' ') return QStringLiteral("dds");
    if (d[0] == 'G' && d[1] == 'I' && d[2] == 'F' && d[3] == '8') return QStringLiteral("gif");
    if (d[0] == 'B' && d[1] == 'M') return QStringLiteral("bmp");
    if (len >= 12 && d[0] == 'R' && d[1] == 'I' && d[2] == 'F' && d[3] == 'F' &&
        d[8] == 'W' && d[9] == 'E' && d[10] == 'B' && d[11] == 'P') return QStringLiteral("webp");
    if ((d[0] == 0x49 && d[1] == 0x49 && d[2] == 0x2A && d[3] == 0x00) ||
        (d[0] == 0x4D && d[1] == 0x4D && d[2] == 0x00 && d[3] == 0x2A)) return QStringLiteral("tif");
    if (d[0] == 0xAB && d[1] == 'K' && d[2] == 'T' && d[3] == 'X') return QStringLiteral("ktx");
    return QString();
}

// ---------------------------------------------------------------------------
// Texture-path containment (deep audit 2026-09, finding F2)
// ---------------------------------------------------------------------------
// Everything a model says about its textures is FILE CONTENT. Before this,
// every resolve site below did the same two things: join a relative name onto
// the model's directory, or take an absolute name verbatim — so a .obj/.mtl
// naming `../../../.ssh/id_rsa` or `/etc/passwd` had that file opened, hashed
// into the content-addressed store as a "texture", and written into every
// export of the project. Nothing checked where the result landed.

/// Canonical absolute form: symlinks and `..` resolved when the path exists,
/// a cleaned absolute path when it does not (so a name that escapes and does
/// not exist is still recognised as escaping).
static QString canonicalOrCleaned(const QString &path)
{
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    return canonical.isEmpty() ? QDir::cleanPath(info.absoluteFilePath()) : canonical;
}

static bool pathIsInside(const QString &path, const QString &dir)
{
    if (dir.isEmpty() || path.isEmpty()) return false;
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    // Both platforms' default filesystems are case-insensitive: two spellings
    // of the same directory ARE the same directory, and containment must say so.
    const Qt::CaseSensitivity cs = Qt::CaseInsensitive;
#else
    const Qt::CaseSensitivity cs = Qt::CaseSensitive;
#endif
    if (path.compare(dir, cs) == 0) return true;
    const QString prefix = dir.endsWith(QLatin1Char('/')) ? dir : dir + QLatin1Char('/');
    return path.startsWith(prefix, cs);
}

QStringList& MaterialHelper::warningSink()
{
    // Thread-local: extraction is synchronous on its caller's thread (the
    // import pipeline's worker), so a parallel import cannot take another's
    // warnings and no lock is needed.
    static thread_local QStringList sink;
    return sink;
}

QStringList MaterialHelper::takeContainmentWarnings()
{
    QStringList taken;
    taken.swap(warningSink());
    return taken;
}

QString MaterialHelper::containedTexturePath(const QString &name, const QString &sourceDir,
                                             const QString &kind)
{
    if (name.isEmpty()) return QString();
    // "*0" and friends are assimp's EMBEDDED texture references, not paths.
    // They never touch the filesystem; the embedded loader resolves them.
    if (name.startsWith(QLatin1Char('*'))) return name;
    if (sourceDir.isEmpty()) return QString();

    const QString dir = canonicalOrCleaned(sourceDir);
    const QString resolved = canonicalOrCleaned(QDir(dir).filePath(name));
    if (pathIsInside(resolved, dir)) return resolved;

    // Escaped. The overwhelmingly common innocent case is a DCC tool writing
    // its authoring machine's absolute path for a texture that actually ships
    // beside the model, so fall back to the BASENAME inside the model's own
    // directory; the hostile case simply names a file that is not there and
    // the reference dies at the callers' isFile() checks.
    const QString base = QFileInfo(name).fileName();
    const QString fallback = base.isEmpty() ? QString()
                                            : QDir::cleanPath(QDir(dir).filePath(base));
    const bool haveFallback = !fallback.isEmpty() && QFileInfo(fallback).isFile();
    const QString warning =
        haveFallback
            ? QStringLiteral("%1 \"%2\" points outside the model's folder; "
                             "using \"%3\" from the model's folder instead")
                  .arg(kind, name, base)
            : QStringLiteral("%1 \"%2\" points outside the model's folder; "
                             "the reference was dropped").arg(kind, name);
    QStringList &sink = warningSink();
    if (!sink.contains(warning)) sink.append(warning);   // once per distinct name
    // The fallback is returned even when no such file exists: it stays inside
    // the model's directory (so nothing outside can be read), it lets the
    // EMBEDDED lookup — which matches on short file name — still find media
    // carried inside the model, and every consumer guards on isFile().
    return fallback;
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
            // Contained: the model does not get to name a file outside its own
            // folder (see containedTexturePath).
            const QString path = containedTexturePath(diffuseTex, assetPath);
            if (!path.isEmpty()) mat->setDiffuseTexture(Texture2D::load(path));
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
        // The format must FOLLOW the file name. The old writer hardcoded
        // "PNG" while callers named files after assimp's achFormatHint —
        // every embedded JPEG landed as PNG bytes in a .jpg file, and the
        // engine (which picks codecs by extension) rendered it white.
        QImageWriter writer(path);   // format from the suffix
        if (path.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive))
            writer.setCompression(1); // low compression, fast write
        if (!writer.write(image)) {
            qWarning() << "Failed to save texture:" << path;
        }
    });

    locker.relock();
    g_textureSaveTasks.append(task);
}

void MaterialHelper::saveTextureBytesAsync(const QByteArray &bytes, const QString &path)
{
    if (bytes.isEmpty() || path.isEmpty()) return;

    QMutexLocker locker(&g_saveMutex);
    if (g_savedPaths.contains(path)) return;
    g_savedPaths.insert(path);
    locker.unlock();

    QDir().mkpath(QFileInfo(path).absolutePath());

    SaveTask task;
    task.path = path;
    task.future = QtConcurrent::run([bytes, path]() {
        QFile out(path);
        if (!out.open(QIODevice::WriteOnly) || out.write(bytes) != bytes.size()) {
            qWarning() << "Failed to save embedded texture:" << path;
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
    QByteArray rawBytes;
    if (texName.startsWith("*")) {
        image = loadGLBEmbeddedTexture(scene, texName, fileName, rawBytes);
    } else {
        image = loadOMEmbeddedTexture(scene, texPath, fileName, rawBytes);
    }


    hasEmbedded = false;
    texPath.clear();

    if (!image.isNull()) {
        // The embedded texture's own file name is file content too: a GLB may
        // call its image "../../evil.png". Extraction writes into the staging
        // dir and nowhere else, so take the base name only.
        QString imagePath = QDir(assetPath).filePath(QFileInfo(fileName).fileName());
        texPath = imagePath;
        hasEmbedded = true;

        if (!QFileInfo::exists(texPath)) {
            // Compressed embedded textures are written VERBATIM: no
            // re-encode, and the extension was sniffed from these bytes,
            // so file name and content can never disagree again.
            if (!rawBytes.isEmpty())
                saveTextureBytesAsync(rawBytes, imagePath);
            else
                saveTextureAsync(image, imagePath);
        }
    }
}

// Extension for an embedded aiTexture: sniffed from the compressed bytes
// (assimp's achFormatHint / the source's mimeType routinely LIE — GLBs in the
// wild declare image/jpeg over PNG bytes and vice versa); uncompressed texel
// data is PNG-encoded by the save, so it is named .png.
static QString embeddedTextureExtension(const aiTexture *tex)
{
    if (tex->mHeight == 0) {
        const QString sniffed = MaterialHelper::sniffImageExtension(
            reinterpret_cast<const unsigned char*>(tex->pcData), int(tex->mWidth));
        if (!sniffed.isEmpty()) return sniffed;
        const QString hint = QString::fromLatin1(tex->achFormatHint).toLower();
        return hint.isEmpty() ? QStringLiteral("png") : hint;
    }
    return QStringLiteral("png");
}

QImage MaterialHelper::loadOMEmbeddedTexture(const aiScene* scene, const QString& texPath,
                                             QString& fileName, QByteArray& rawBytes)
{
    const aiTexture* tex = scene->GetEmbeddedTexture(texPath.toStdString().c_str());
    if (!tex) {
        // qWarning() << "Embedded texture not found for path:" << texPath;
        return QImage();
    }

    const QFileInfo info(texPath);
    fileName = info.completeBaseName() + "." + embeddedTextureExtension(tex);
    if (tex->mHeight == 0)
        rawBytes = QByteArray(reinterpret_cast<const char*>(tex->pcData), int(tex->mWidth));

    QImage image = convertAiTextureToImage(tex);

    return image;
}

QImage MaterialHelper::loadGLBEmbeddedTexture(const aiScene *scene,
                                              const QString &texName,
                                              QString& fileName,
                                              QByteArray& rawBytes)
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
        fileName = name + "." + embeddedTextureExtension(embeddedTex);
        if (embeddedTex->mHeight == 0)
            rawBytes = QByteArray(reinterpret_cast<const char*>(embeddedTex->pcData),
                                  int(embeddedTex->mWidth));

        image = convertAiTextureToImage(embeddedTex);
    }

    return image;
}


void MaterialHelper::extractMaterialData(const aiScene *scene,
                    aiMaterial *aiMat,
                    QString assetPath,
                    MeshMaterialData& mat,
                    const QString &writeDir)
{
    // Extraction output target: the pipeline's staging dir when given, else
    // (legacy) the source's own directory.
    const QString outDir = writeDir.isEmpty() ? assetPath : writeDir;
    mat.diffuseColor  = getAiMaterialColor(aiMat, AI_MATKEY_COLOR_DIFFUSE);
    mat.specularColor = getAiMaterialColor(aiMat, AI_MATKEY_COLOR_SPECULAR);
    mat.ambientColor  = getAiMaterialColor(aiMat, AI_MATKEY_COLOR_AMBIENT);
    mat.emissionColor = getAiMaterialColor(aiMat, AI_MATKEY_COLOR_EMISSIVE);
    mat.shininess     = getAiMaterialFloat(aiMat, AI_MATKEY_SHININESS);

    if (assetPath.isEmpty()) return;

    // Every on-disk resolve below goes through containedTexturePath: the model
    // names its textures, so `../../` and absolute paths are file content and
    // must not be able to reach outside the model's own directory (deep audit
    // 2026-09 F2). The old code here was exactly the vulnerable shape —
    // "relative? join it; absolute? take it".

    // ------------------------
    // Diffuse
    // ------------------------
    QString diffuseTex = getAiMaterialTexture(aiMat, aiTextureType_DIFFUSE);
    mat.diffuseTexture = containedTexturePath(diffuseTex, assetPath);

    loadEmbeddedTexture(scene, diffuseTex, outDir, mat.diffuseTexture, mat.hasEmbeddedDiffTexture);

    // ------------------------
    // Specular
    // ------------------------
    QString specularTex = getAiMaterialTexture(aiMat, aiTextureType_SPECULAR);
    mat.specularTexture = containedTexturePath(specularTex, assetPath);

    loadEmbeddedTexture(scene, specularTex, outDir, mat.specularTexture, mat.hasEmbeddedSpecularTexture);

    // ------------------------
    // Normals
    // ------------------------
    QString normalsTex = getAiMaterialTexture(aiMat, aiTextureType_NORMALS);
    mat.normalTexture = containedTexturePath(normalsTex, assetPath);

    loadEmbeddedTexture(scene, normalsTex, outDir, mat.normalTexture, mat.hasEmbeddedNormalTexture);

    // ------------------------
    // Fallback for height maps if normal map missing
    // ------------------------
    if (normalsTex.isEmpty()) {
        normalsTex = getAiMaterialTexture(aiMat, aiTextureType_HEIGHT);
        mat.hightTexture = containedTexturePath(normalsTex, assetPath);

        loadEmbeddedTexture(scene, normalsTex, outDir, mat.hightTexture, mat.hasEmbeddedHightTexture);
    }

    // ------------------------
    // glTF 2.0 metallic-roughness (GLB importer fix phase 0). assimp reads
    // baseColor/metallic/roughness factors and the texture bindings for glTF;
    // the old importer discarded ALL of it and kept only the lossy
    // roughness→shininess back-conversion (every GLB rendered near-mirror).
    // ------------------------
    auto resolveTex = [&](const QString& name, QString& outPath) {
        if (name.isEmpty()) { outPath.clear(); return; }
        outPath = containedTexturePath(name, assetPath);
        bool embedded = false;
        loadEmbeddedTexture(scene, name, outDir, outPath, embedded);
    };

    aiColor4D baseColor;
    float metallic = 1.0f, roughness = 1.0f;
    const bool hasBaseColor = aiMat->Get(AI_MATKEY_BASE_COLOR, baseColor) == AI_SUCCESS;
    const bool hasMetallic  = aiMat->Get(AI_MATKEY_METALLIC_FACTOR, metallic) == AI_SUCCESS;
    const bool hasRoughness = aiMat->Get(AI_MATKEY_ROUGHNESS_FACTOR, roughness) == AI_SUCCESS;

    const QString baseTexName = getAiMaterialTexture(aiMat, aiTextureType_BASE_COLOR);
    resolveTex(baseTexName, mat.baseColorTexture);
    if (mat.baseColorTexture.isEmpty()) mat.baseColorTexture = mat.diffuseTexture;

    resolveTex(getAiMaterialTexture(aiMat, aiTextureType_EMISSIVE), mat.emissiveTexture);

    QString mrName = getAiMaterialTexture(aiMat, aiTextureType_METALNESS);
    if (mrName.isEmpty()) mrName = getAiMaterialTexture(aiMat, aiTextureType_DIFFUSE_ROUGHNESS);
    QString mrPath;
    resolveTex(mrName, mrPath);

    mat.hasPbr = hasBaseColor || hasMetallic || hasRoughness ||
                 !baseTexName.isEmpty() || !mrName.isEmpty();
    if (hasBaseColor)
        mat.baseColorFactor = QColor::fromRgbF(qBound(0.0f, baseColor.r, 1.0f),
                                               qBound(0.0f, baseColor.g, 1.0f),
                                               qBound(0.0f, baseColor.b, 1.0f),
                                               qBound(0.0f, baseColor.a, 1.0f));
    if (hasMetallic)  mat.metallicFactor  = metallic;
    if (hasRoughness) mat.roughnessFactor = roughness;

    waitForAllTextureSaves();

    // The packed MR map (metallic = BLUE, roughness = GREEN per glTF) must be
    // split at import: the engine's HlmsPbs samples metalness and roughness
    // maps as single-channel textures, so binding the packed image directly
    // would read the wrong channel for both.
    if (!mrPath.isEmpty() && QFileInfo(mrPath).isFile()) {
        // Split maps land in the extraction target, never beside the packed
        // source (which may live in a read-only import location).
        const QFileInfo mrInfo(mrPath);
        const QString metalPath = outDir + "/" + mrInfo.completeBaseName() + "_metallic.png";
        const QString roughPath = outDir + "/" + mrInfo.completeBaseName() + "_roughness.png";
        if (!QFileInfo::exists(metalPath) || !QFileInfo::exists(roughPath)) {
            QDir().mkpath(outDir);
            const QImage mr = QImage(mrPath).convertToFormat(QImage::Format_RGBA8888);
            if (!mr.isNull()) {
                QImage metal(mr.size(), QImage::Format_Grayscale8);
                QImage rough(mr.size(), QImage::Format_Grayscale8);
                for (int y = 0; y < mr.height(); ++y) {
                    const uchar* src = mr.constScanLine(y);
                    uchar* m = metal.scanLine(y);
                    uchar* r = rough.scanLine(y);
                    for (int x = 0; x < mr.width(); ++x) {
                        m[x] = src[x * 4 + 2];   // blue  → metallic
                        r[x] = src[x * 4 + 1];   // green → roughness
                    }
                }
                metal.save(metalPath, "PNG");
                rough.save(roughPath, "PNG");
            }
        }
        if (QFileInfo::exists(metalPath)) mat.metallicTexture  = metalPath;
        if (QFileInfo::exists(roughPath)) mat.roughnessTexture = roughPath;
    }
}


} // namespace iris
