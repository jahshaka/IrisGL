#include "import/materialhelper.h"
#include <QDir>
#include <QUuid>
#include <QFileInfo>
#include <QDateTime>
#include <QDebug>
#include <QImageWriter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtConcurrent>
#include <algorithm>
#include <cmath>
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


// ---------------------------------------------------------------------------
// glTF SOURCE FACTS (GLB material-import fix, 2026-09-08)
// ---------------------------------------------------------------------------
// WHY THE FILE IS RE-READ HERE, when assimp already parsed it.
//
// assimp flattens every glTF material onto one flat key set, and it writes the
// metallic-roughness keys UNCONDITIONALLY from its own struct defaults —
// glTF2Importer.cpp ImportMaterial adds AI_MATKEY_METALLIC_FACTOR and
// AI_MATKEY_ROUGHNESS_FACTOR (defaults 1.0 / 1.0) and a white AI_MATKEY_BASE_COLOR
// even for a material whose JSON has no `pbrMetallicRoughness` object at all.
// So "assimp reported a metallic factor" does not mean "the file said metallic",
// and the importer's `if (hasMetallic) metallicFactor = metallic` therefore
// imported a KHR_materials_pbrSpecularGlossiness model (which has no
// metallic-roughness block by construction) as a FULL-METAL, FULL-ROUGH surface:
// black, because a metal lit by nothing but punctual lights and no environment
// reflects nothing. That is the "GLB importer loses the materials" report.
//
// The distinction exists only in the source JSON, so the source JSON is what we
// read: the JSON chunk of a GLB, or the whole of a .gltf. Parsed ONCE per file
// and cached — a 200-material model must not re-read the chunk 200 times.
namespace {

struct GltfMaterialFacts
{
    bool  valid                 = false;  ///< the facts below came from a real glTF material
    bool  hasMetallicRoughness  = false;  ///< the JSON carries a `pbrMetallicRoughness` object
    bool  hasSpecularGlossiness = false;  ///< ... a KHR_materials_pbrSpecularGlossiness extension
    bool  unlit                 = false;  ///< ... a KHR_materials_unlit extension
    // KHR_materials_pbrSpecularGlossiness values, with the extension's own defaults.
    float diffuse[4]  = { 1.0f, 1.0f, 1.0f, 1.0f };
    float specular[3] = { 1.0f, 1.0f, 1.0f };
    float glossiness  = 1.0f;
    // ---- MATERIAL_GAPS_SPEC GAP 1: the two extensions the workflow switch
    // makes importable NATIVELY instead of by conversion. Read from the file's
    // own JSON for the same reason every other fact here is: assimp flattens
    // KHR_materials_specular onto AI_MATKEY_COLOR_SPECULAR, which is also where
    // spec-gloss lands and where a legacy Blinn material's specular lands, so
    // the flattened keys cannot tell us WHICH extension was present.
    bool  hasSpecularExt        = false;  ///< a KHR_materials_specular extension
    float specularFactor        = 1.0f;   ///< its scalar strength (default 1)
    float specularColorFactor[3] = { 1.0f, 1.0f, 1.0f };   ///< its F0 tint (default white)
    bool  hasIor                = false;  ///< a KHR_materials_ior extension
    float ior                   = 1.5f;   ///< its value (the extension's own default)
};

/// The JSON of a glTF asset: the first chunk of a GLB container, or the file
/// itself for the .gltf form. Returns false for anything else (an .fbx, an
/// .obj, a GLB whose header is damaged) — the caller then keeps assimp's view.
bool readGltfJson(const QString &file, QJsonObject &out)
{
    if (file.isEmpty()) return false;
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) return false;

    const QByteArray magic = f.read(4);
    if (magic.size() < 4) return false;

    QByteArray json;
    if (magic == QByteArrayLiteral("glTF")) {
        // GLB: 12-byte header (magic, version, length) then chunks of
        // [uint32 length][uint32 type][payload]. The FIRST chunk is the JSON
        // one by specification.
        if (f.read(8).size() < 8) return false;               // version + total length
        const QByteArray chunkHeader = f.read(8);
        if (chunkHeader.size() < 8) return false;
        const auto u32 = [](const QByteArray &b, int at) {
            return quint32(quint8(b[at])) | (quint32(quint8(b[at+1])) << 8) |
                   (quint32(quint8(b[at+2])) << 16) | (quint32(quint8(b[at+3])) << 24);
        };
        const quint32 chunkLen  = u32(chunkHeader, 0);
        const quint32 chunkType = u32(chunkHeader, 4);
        // 0x4E4F534A = 'JSON'. A 64 MB ceiling keeps a corrupt length from
        // asking for a gigabyte: real glTF JSON chunks are far below it.
        if (chunkType != 0x4E4F534A || chunkLen == 0 || chunkLen > 64u * 1024u * 1024u) return false;
        json = f.read(qint64(chunkLen));
        if (quint32(json.size()) != chunkLen) return false;
    } else if (magic.trimmed().startsWith('{')) {
        f.seek(0);
        // A .gltf is text; the same 64 MB ceiling applies.
        if (f.size() > 64ll * 1024 * 1024) return false;
        json = f.readAll();
    } else {
        return false;
    }

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return false;
    out = doc.object();
    return true;
}

QVector<GltfMaterialFacts> parseGltfMaterials(const QString &sourceFile)
{
    QVector<GltfMaterialFacts> facts;
    QJsonObject root;
    if (!readGltfJson(sourceFile, root)) return facts;

    const QJsonArray materials = root.value(QStringLiteral("materials")).toArray();
    facts.reserve(materials.size());
    for (const QJsonValue &value : materials) {
        const QJsonObject m = value.toObject();
        GltfMaterialFacts f;
        f.valid = true;
        f.hasMetallicRoughness = m.contains(QStringLiteral("pbrMetallicRoughness"));

        const QJsonObject ext = m.value(QStringLiteral("extensions")).toObject();
        f.unlit = ext.contains(QStringLiteral("KHR_materials_unlit"));

        const QJsonObject sg =
            ext.value(QStringLiteral("KHR_materials_pbrSpecularGlossiness")).toObject();
        if (ext.contains(QStringLiteral("KHR_materials_pbrSpecularGlossiness"))) {
            f.hasSpecularGlossiness = true;
            const QJsonArray diffuse  = sg.value(QStringLiteral("diffuseFactor")).toArray();
            const QJsonArray specular = sg.value(QStringLiteral("specularFactor")).toArray();
            for (int i = 0; i < 4 && i < diffuse.size(); ++i)
                f.diffuse[i] = float(diffuse.at(i).toDouble(1.0));
            for (int i = 0; i < 3 && i < specular.size(); ++i)
                f.specular[i] = float(specular.at(i).toDouble(1.0));
            f.glossiness = float(sg.value(QStringLiteral("glossinessFactor")).toDouble(1.0));
        }

        // KHR_materials_specular and KHR_materials_ior. Note the exclusivity
        // assimp imposes on its flattened view (specular OR spec-gloss, never
        // both, glTF2Importer.cpp:297) is a property of ITS reader, not of the
        // format — reading the JSON ourselves keeps the two independent.
        if (ext.contains(QStringLiteral("KHR_materials_specular"))) {
            const QJsonObject sp = ext.value(QStringLiteral("KHR_materials_specular")).toObject();
            f.hasSpecularExt = true;
            f.specularFactor = float(sp.value(QStringLiteral("specularFactor")).toDouble(1.0));
            const QJsonArray colour = sp.value(QStringLiteral("specularColorFactor")).toArray();
            for (int i = 0; i < 3 && i < colour.size(); ++i)
                f.specularColorFactor[i] = float(colour.at(i).toDouble(1.0));
        }
        if (ext.contains(QStringLiteral("KHR_materials_ior"))) {
            const QJsonObject io = ext.value(QStringLiteral("KHR_materials_ior")).toObject();
            f.hasIor = true;
            f.ior = float(io.value(QStringLiteral("ior")).toDouble(1.5));
        }
        facts.append(f);
    }
    return facts;
}

/// One-file cache. Imports run one model at a time per thread, and the entry is
/// keyed by path + size + mtime so a re-imported (edited) file is re-read.
const QVector<GltfMaterialFacts> &gltfFactsFor(const QString &sourceFile)
{
    struct Cache {
        QString path;
        qint64 size = -1;
        qint64 modified = -1;
        QVector<GltfMaterialFacts> facts;
    };
    static thread_local Cache cache;
    static const QVector<GltfMaterialFacts> kEmpty;

    if (sourceFile.isEmpty()) return kEmpty;
    const QFileInfo info(sourceFile);
    if (!info.isFile()) return kEmpty;
    const qint64 size = info.size();
    const qint64 modified = info.lastModified().toMSecsSinceEpoch();
    if (cache.path != info.absoluteFilePath() || cache.size != size || cache.modified != modified) {
        cache.path = info.absoluteFilePath();
        cache.size = size;
        cache.modified = modified;
        cache.facts = parseGltfMaterials(sourceFile);
    }
    return cache.facts;
}

/// assimp keeps glTF materials in file order (glTF2Importer::ImportMaterials
/// walks the asset's material array and appends a default material LAST when a
/// mesh has none), so the aiMaterial's index into the scene is the index into
/// the JSON array — for every index the JSON actually has.
int materialIndexIn(const aiScene *scene, const aiMaterial *aiMat)
{
    if (!scene || !aiMat) return -1;
    for (unsigned i = 0; i < scene->mNumMaterials; ++i)
        if (scene->mMaterials[i] == aiMat) return int(i);
    return -1;
}

GltfMaterialFacts factsForMaterial(const QString &sourceFile, const aiScene *scene,
                                   const aiMaterial *aiMat)
{
    const QVector<GltfMaterialFacts> &all = gltfFactsFor(sourceFile);
    const int index = materialIndexIn(scene, aiMat);
    if (index < 0 || index >= all.size()) return GltfMaterialFacts();
    return all.at(index);
}

} // namespace

// The perceived-brightness measure the KHR_materials_pbrSpecularGlossiness
// appendix uses (luma weights, applied to the SQUARES).
static float perceivedBrightness(float r, float g, float b)
{
    return std::sqrt(0.299f * r * r + 0.587f * g * g + 0.114f * b * b);
}

void MaterialHelper::specularGlossinessToMetallicRoughness(const float diffuse[4],
                                                           const float specular[3],
                                                           float glossiness,
                                                           QColor &baseColorOut,
                                                           float &metallicOut,
                                                           float &roughnessOut)
{
    // The conversion published with the extension itself ("Converting between
    // workflows", KHR_materials_pbrSpecularGlossiness appendix B) — the same
    // arithmetic Blender's and three.js's importers run, so a model converted
    // here looks like it does everywhere else rather than like our guess.
    constexpr float kDielectricSpecular = 0.04f;
    constexpr float kEpsilon = 1e-6f;

    const float specStrength = std::max(specular[0], std::max(specular[1], specular[2]));
    const float oneMinusSpecStrength = 1.0f - specStrength;

    const float diffuseBrightness  = perceivedBrightness(diffuse[0], diffuse[1], diffuse[2]);
    const float specularBrightness = perceivedBrightness(specular[0], specular[1], specular[2]);

    // Solve the quadratic that inverts the metallic-roughness specular term.
    // A specular colour DARKER than a dielectric's fixed 4% cannot be metal at
    // all, which is the whole of the tails-model case (specularFactor [0,0,0]).
    float metallic = 0.0f;
    if (specularBrightness >= kDielectricSpecular) {
        const float a = kDielectricSpecular;
        const float b = diffuseBrightness * oneMinusSpecStrength / (1.0f - kDielectricSpecular) +
                        specularBrightness - 2.0f * kDielectricSpecular;
        const float c = kDielectricSpecular - specularBrightness;
        const float d = std::max(0.0f, b * b - 4.0f * a * c);
        metallic = qBound(0.0f, (-b + std::sqrt(d)) / (2.0f * a), 1.0f);
    }

    float base[3];
    for (int i = 0; i < 3; ++i) {
        const float fromDiffuse = diffuse[i] * oneMinusSpecStrength /
                                  (1.0f - kDielectricSpecular) / std::max(1.0f - metallic, kEpsilon);
        const float fromSpecular = (specular[i] - kDielectricSpecular * (1.0f - metallic)) /
                                   std::max(metallic, kEpsilon);
        const float t = metallic * metallic;
        base[i] = qBound(0.0f, fromDiffuse * (1.0f - t) + fromSpecular * t, 1.0f);
    }

    baseColorOut = QColor::fromRgbF(base[0], base[1], base[2], qBound(0.0f, diffuse[3], 1.0f));
    metallicOut  = metallic;
    roughnessOut = qBound(0.0f, 1.0f - glossiness, 1.0f);
}

void MaterialHelper::extractMaterialData(const aiScene *scene,
                    aiMaterial *aiMat,
                    QString assetPath,
                    MeshMaterialData& mat,
                    const QString &writeDir,
                    const QString &sourceFile)
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
    // glTF 2.0 SHADING INPUTS (importer fix phase 0; the material defects of
    // 2026-09-08). assimp reads baseColor/metallic/roughness factors and the
    // texture bindings for glTF; the old importer discarded ALL of it and kept
    // only the lossy roughness→shininess back-conversion (every GLB rendered
    // near-mirror). What it then got WRONG is which of assimp's values the file
    // actually stated — see the GltfMaterialFacts comment above.
    // ------------------------
    auto resolveTex = [&](const QString& name, QString& outPath) {
        if (name.isEmpty()) { outPath.clear(); return; }
        outPath = containedTexturePath(name, assetPath);
        bool embedded = false;
        loadEmbeddedTexture(scene, name, outDir, outPath, embedded);
    };

    const GltfMaterialFacts facts = factsForMaterial(sourceFile, scene, aiMat);

    aiColor4D baseColor;
    float metallic = 1.0f, roughness = 1.0f;
    const bool hasBaseColor = aiMat->Get(AI_MATKEY_BASE_COLOR, baseColor) == AI_SUCCESS;
    const bool hasMetallic  = aiMat->Get(AI_MATKEY_METALLIC_FACTOR, metallic) == AI_SUCCESS;
    const bool hasRoughness = aiMat->Get(AI_MATKEY_ROUGHNESS_FACTOR, roughness) == AI_SUCCESS;

    // Spec-gloss and unlit, read from the file when we have it and from
    // assimp's flattened keys otherwise (a .gltf we failed to parse, or a
    // non-glTF format whose importer sets them — assimp only ever writes
    // AI_MATKEY_GLOSSINESS_FACTOR for a real spec-gloss material).
    float glossiness = 1.0f;
    const bool assimpSpecGloss =
        aiMat->Get(AI_MATKEY_GLOSSINESS_FACTOR, glossiness) == AI_SUCCESS;
    int shadingModel = 0;
    const bool assimpUnlit =
        aiMat->Get(AI_MATKEY_SHADING_MODEL, shadingModel) == AI_SUCCESS &&
        shadingModel == aiShadingMode_Unlit;

    const bool specGloss = facts.valid ? facts.hasSpecularGlossiness : assimpSpecGloss;
    // THE LOAD-BEARING LINE. For a glTF source, "the file has a
    // pbrMetallicRoughness block" is a FILE fact, never assimp's always-present
    // keys; for anything else assimp's keys are all there is.
    const bool metalRoughBlock = facts.valid ? facts.hasMetallicRoughness
                                             : (hasMetallic || hasRoughness);
    mat.unlit = facts.valid ? facts.unlit : assimpUnlit;

    const QString baseTexName = getAiMaterialTexture(aiMat, aiTextureType_BASE_COLOR);
    resolveTex(baseTexName, mat.baseColorTexture);
    if (mat.baseColorTexture.isEmpty()) mat.baseColorTexture = mat.diffuseTexture;

    resolveTex(getAiMaterialTexture(aiMat, aiTextureType_EMISSIVE), mat.emissiveTexture);

    QString mrName = getAiMaterialTexture(aiMat, aiTextureType_METALNESS);
    if (mrName.isEmpty()) mrName = getAiMaterialTexture(aiMat, aiTextureType_DIFFUSE_ROUGHNESS);
    QString mrPath;
    resolveTex(mrName, mrPath);

    // Any glTF material is a PBR material, whichever workflow it was authored
    // in — including one that states no workflow at all, which lands on the
    // dielectric defaults in MeshMaterialData rather than on the legacy
    // Blinn-Phong conversion.
    mat.hasPbr = facts.valid || specGloss || metalRoughBlock ||
                 !baseTexName.isEmpty() || !mrName.isEmpty();

    if (specGloss && !metalRoughBlock) {
        // KHR_materials_pbrSpecularGlossiness. The values come from the file
        // when we parsed it; otherwise from assimp, which maps diffuseFactor
        // onto COLOR_DIFFUSE and specularFactor onto COLOR_SPECULAR.
        float diffuse[4]  = { 1.0f, 1.0f, 1.0f, 1.0f };
        float specular[3] = { 1.0f, 1.0f, 1.0f };
        float gloss = glossiness;
        if (facts.valid) {
            for (int i = 0; i < 4; ++i) diffuse[i]  = facts.diffuse[i];
            for (int i = 0; i < 3; ++i) specular[i] = facts.specular[i];
            gloss = facts.glossiness;
        } else {
            aiColor4D d, s;
            if (aiMat->Get(AI_MATKEY_COLOR_DIFFUSE, d) == AI_SUCCESS) {
                diffuse[0] = d.r; diffuse[1] = d.g; diffuse[2] = d.b; diffuse[3] = d.a;
            }
            if (aiMat->Get(AI_MATKEY_COLOR_SPECULAR, s) == AI_SUCCESS) {
                specular[0] = s.r; specular[1] = s.g; specular[2] = s.b;
            }
        }
        // NATIVE, NOT CONVERTED (MATERIAL_GAPS_SPEC §2.5). Spec-gloss IS the
        // renderer's Specular workflow: diffuse -> base colour, specularFactor
        // -> kS, roughness = 1 - glossiness, and the spec-gloss MAP finally has
        // a home — the shared metallic/specular texture unit. The conversion to
        // metallic-roughness below is still built and still correct; it is now
        // the fallback for targets with no workflow concept (web export), which
        // is the one place §2.6 keeps it.
        mat.workflow = 1;   // Specular
        mat.baseColorFactor = QColor::fromRgbF(qBound(0.0f, diffuse[0], 1.0f),
                                               qBound(0.0f, diffuse[1], 1.0f),
                                               qBound(0.0f, diffuse[2], 1.0f),
                                               qBound(0.0f, diffuse[3], 1.0f));
        mat.specularFactor  = QColor::fromRgbF(qBound(0.0f, specular[0], 1.0f),
                                               qBound(0.0f, specular[1], 1.0f),
                                               qBound(0.0f, specular[2], 1.0f));
        // Glossiness is the inverse sense of roughness, exactly.
        mat.roughnessFactor = qBound(0.0f, 1.0f - gloss, 1.0f);
        mat.metallicFactor  = 0.0f;   // not read in this workflow; kept sane
        mat.specularMapTexture = mat.specularTexture;
    } else if (facts.valid && facts.hasSpecularExt) {
        // KHR_materials_specular on a metallic-roughness base. The pin's own
        // documentation calls SpecularAsFresnelWorkflow "what most PBRs mean by
        // specular" (OgreHlmsPbsDatablock.h:227-230), and the extension's
        // specularColorFactor IS an F0 tint, so it maps onto the fresnel colour
        // rather than onto kS.
        //
        // NOT VERIFIED AGAINST A REFERENCE RENDERER (§9): the mapping follows
        // the pin's documentation, not a measured comparison.
        mat.workflow = 2;   // Specular-as-Fresnel
        if (hasBaseColor)
            mat.baseColorFactor = QColor::fromRgbF(qBound(0.0f, baseColor.r, 1.0f),
                                                   qBound(0.0f, baseColor.g, 1.0f),
                                                   qBound(0.0f, baseColor.b, 1.0f),
                                                   qBound(0.0f, baseColor.a, 1.0f));
        if (hasRoughness) mat.roughnessFactor = roughness;
        const float sf = qBound(0.0f, facts.specularFactor, 1.0f);
        mat.useFresnelColor = true;
        mat.fresnelFactor = QColor::fromRgbF(
            qBound(0.0f, facts.specularColorFactor[0] * sf, 1.0f),
            qBound(0.0f, facts.specularColorFactor[1] * sf, 1.0f),
            qBound(0.0f, facts.specularColorFactor[2] * sf, 1.0f));
        mat.specularFactor = QColor(255, 255, 255);   // kS stays inert
        mat.metallicFactor = 0.0f;
        mat.specularMapTexture = mat.specularTexture;
    } else if (metalRoughBlock) {
        // glTF semantics for a PRESENT block: an omitted metallicFactor IS 1.0
        // and an omitted roughnessFactor IS 1.0. A metal car in a scene with no
        // environment to reflect looks dark because that is what metal does,
        // not because the import lost anything.
        if (hasBaseColor)
            mat.baseColorFactor = QColor::fromRgbF(qBound(0.0f, baseColor.r, 1.0f),
                                                   qBound(0.0f, baseColor.g, 1.0f),
                                                   qBound(0.0f, baseColor.b, 1.0f),
                                                   qBound(0.0f, baseColor.a, 1.0f));
        if (hasMetallic)  mat.metallicFactor  = metallic;
        if (hasRoughness) mat.roughnessFactor = roughness;
    } else if (hasBaseColor && !facts.valid) {
        // A non-glTF source that reported a base colour but no workflow keeps
        // the colour on the dielectric defaults.
        mat.baseColorFactor = QColor::fromRgbF(qBound(0.0f, baseColor.r, 1.0f),
                                               qBound(0.0f, baseColor.g, 1.0f),
                                               qBound(0.0f, baseColor.b, 1.0f),
                                               qBound(0.0f, baseColor.a, 1.0f));
    }

    // KHR_materials_ior. Stored on every workflow — inert on a metallic
    // material, but kept so a workflow switch in the editor restores it (the
    // same "values survive the switch" rule clear coat follows). assimp's
    // AI_MATKEY_REFRACTI is the fallback for a non-glTF source.
    if (facts.valid && facts.hasIor) {
        mat.ior = qBound(1.0f, facts.ior, 3.0f);
    } else if (!facts.valid) {
        float refracti = 0.0f;
        if (aiMat->Get(AI_MATKEY_REFRACTI, refracti) == AI_SUCCESS && refracti > 1.0f)
            mat.ior = qBound(1.0f, refracti, 3.0f);
    }

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
