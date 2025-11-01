// assimpmodelloader.cpp
#include "assimpmodelloader.h"

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <vtkPolyData.h>
#include <vtkPoints.h>
#include <vtkCellArray.h>
#include <vtkTriangle.h>
#include <vtkFloatArray.h>
#include <vtkPointData.h>
#include <vtkPolyDataMapper.h>
#include <vtkActor.h>
#include <vtkTexture.h>
#include <vtkImageData.h>
#include <vtkImageImport.h>
#include <vtkProperty.h>
#include <vtkLight.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkNew.h>
#include <vtkSmartPointer.h>
#include <vtkCamera.h>

#include <QImage>
#include <QFileInfo>
#include <QDir>
#include <QUuid>
#include <QDebug>

namespace vtkmeta {

// ------------------------- small helpers -------------------------
static QString guessFileNameFromAiString(const aiString& s) {
    QString name = QString::fromUtf8(s.C_Str());
    if (name.isEmpty()) return QString();
    return QFileInfo(name).fileName();
}

QString AssimpModelLoader::generateGUIDFileName(const QString& base) const {
    QString bn = QUuid::createUuid().toString();
    if (!base.isEmpty()) {
        bn = QFileInfo(base).completeBaseName() + "_" + bn;
    }
    bn.replace("{", "").replace("}", "").replace("-", "");
    return bn;
}

// convert Assimp embedded texture -> QImage
QImage AssimpModelLoader::convertAiTextureToQImage(const aiTexture* at) const
{
    if (!at) return QImage();
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
    if (!texels) return QImage();
    // aiTexel is RGBA (4 bytes)
    QImage img(reinterpret_cast<const uchar*>(texels), w, h, QImage::Format_RGBA8888);
    return img.copy();
}

QString AssimpModelLoader::saveEmbeddedTexture(const QImage& img, const QString& suggestedName, const QString& outputFolder) const
{
    if (img.isNull()) return QString();
    QDir d(outputFolder);
    if (!d.exists() && !d.mkpath(".")) {
        qWarning() << "AssimpModelLoader: Cannot create output folder" << outputFolder;
        return QString();
    }
    QString fileName = suggestedName;
    if (!fileName.endsWith(".png", Qt::CaseInsensitive) &&
        !fileName.endsWith(".jpg", Qt::CaseInsensitive) &&
        !fileName.endsWith(".jpeg", Qt::CaseInsensitive))
    {
        // prefer png
        fileName += ".png";
    }
    QString fullPath = d.filePath(fileName);
    if (QFileInfo::exists(fullPath)) return fullPath;
    if (!img.save(fullPath, "PNG")) {
        qWarning() << "AssimpModelLoader: Failed to save embedded texture to" << fullPath;
        return QString();
    }
    qDebug() << "AssimpModelLoader: Saved embedded texture:" << fullPath << "size=" << img.width() << "x" << img.height();
    return fullPath;
}

// ------------------------- convert aiMesh -> vtkPolyData -------------------------
vtkSmartPointer<vtkPolyData> AssimpModelLoader::convertAiMeshToVtkPolyData(const aiMesh* mesh) const
{
    if (!mesh) return nullptr;
    vtkSmartPointer<vtkPolyData> poly = vtkSmartPointer<vtkPolyData>::New();
    vtkSmartPointer<vtkPoints> points = vtkSmartPointer<vtkPoints>::New();
    points->SetNumberOfPoints(mesh->mNumVertices);
    for (unsigned int i = 0; i < mesh->mNumVertices; ++i) {
        const aiVector3D& v = mesh->mVertices[i];
        points->SetPoint(i, v.x, v.y, v.z);
    }
    poly->SetPoints(points);

    if (mesh->HasNormals()) {
        vtkSmartPointer<vtkFloatArray> normals = vtkSmartPointer<vtkFloatArray>::New();
        normals->SetNumberOfComponents(3);
        normals->SetNumberOfTuples(mesh->mNumVertices);
        normals->SetName("Normals");
        for (unsigned int i = 0; i < mesh->mNumVertices; ++i) {
            const aiVector3D& n = mesh->mNormals[i];
            normals->SetTuple3(i, n.x, n.y, n.z);
        }
        poly->GetPointData()->SetNormals(normals);
    }

    if (mesh->HasTextureCoords(0)) {
        vtkSmartPointer<vtkFloatArray> tcoords = vtkSmartPointer<vtkFloatArray>::New();
        tcoords->SetNumberOfComponents(2);
        tcoords->SetNumberOfTuples(mesh->mNumVertices);
        tcoords->SetName("TextureCoordinates");
        for (unsigned int i = 0; i < mesh->mNumVertices; ++i) {
            const aiVector3D& uv = mesh->mTextureCoords[0][i];
            // flip v
            tcoords->SetTuple2(i, uv.x, 1.0f - uv.y);
        }
        poly->GetPointData()->SetTCoords(tcoords);
    }

    vtkSmartPointer<vtkCellArray> polys = vtkSmartPointer<vtkCellArray>::New();
    for (unsigned int fi = 0; fi < mesh->mNumFaces; ++fi) {
        const aiFace& face = mesh->mFaces[fi];
        if (face.mNumIndices < 3) continue;
        // triangulate face (fan)
        for (unsigned int k = 1; k + 1 < face.mNumIndices; ++k) {
            vtkSmartPointer<vtkTriangle> tri = vtkSmartPointer<vtkTriangle>::New();
            tri->GetPointIds()->SetId(0, face.mIndices[0]);
            tri->GetPointIds()->SetId(1, face.mIndices[k]);
            tri->GetPointIds()->SetId(2, face.mIndices[k + 1]);
            polys->InsertNextCell(tri);
        }
    }
    poly->SetPolys(polys);
    poly->Modified();
    return poly;
}

// ------------------------- Create VTK texture from QImage -------------------------
vtkSmartPointer<vtkTexture> AssimpModelLoader::CreateVTKTextureFromQImage(const QImage& img, bool srgb) const
{
    if (img.isNull()) return nullptr;
    QImage imgRGBA = img.convertToFormat(QImage::Format_RGBA8888);
    int w = imgRGBA.width();
    int h = imgRGBA.height();
    const uchar* srcBits = imgRGBA.constBits();
    int bytesPerLine = imgRGBA.bytesPerLine();

    vtkSmartPointer<vtkImageData> imageData = vtkSmartPointer<vtkImageData>::New();
    imageData->SetDimensions(w, h, 1);
    imageData->AllocateScalars(VTK_UNSIGNED_CHAR, 4);
    unsigned char* dest = static_cast<unsigned char*>(imageData->GetScalarPointer(0, 0, 0));

    // copy flipped Y
    for (int y = 0; y < h; ++y) {
        const uchar* srcLine = srcBits + (h - 1 - y) * bytesPerLine;
        memcpy(dest + y * w * 4, srcLine, w * 4);
    }

    vtkSmartPointer<vtkTexture> tex = vtkSmartPointer<vtkTexture>::New();
    tex->SetColorModeToDirectScalars(); // 保留原像素
    tex->MipmapOn();
    tex->RepeatOn();
    tex->SetInputData(imageData);
    tex->InterpolateOn();
    tex->EdgeClampOn();
    tex->SetUseSRGBColorSpace(srgb);

#if VTK_MAJOR_VERSION >= 9
#ifdef VTK_HAS_USE_SRGB
    if (srgb) {
        tex->UseSRGBColorSpaceOn();
        qDebug() << "AssimpModelLoader: CreateVTKTextureFromQImage -> UseSRGBColorSpaceOn()";
    }
#endif
#endif

    // help with blending/alpha
    tex->SetPremultipliedAlpha(true);
    tex->SetBlendingMode(vtkTexture::VTK_TEXTURE_BLENDING_MODE_REPLACE);

    return tex;
}

// ------------------------- resolve texture path (DO NOT CHANGE PATH LOGIC) -------------------------
QString AssimpModelLoader::resolveTexturePath(const QString& texStr, const aiScene* scene, const QString& baseName,
                                              const QString& outputFolder, const QString& modelFilePath) const
{
    // Keep the same behavior you used previously:
    // - embedded (*0 style) -> saved to outputFolder via saveEmbeddedTexture
    // - else try relative to modelFilePath directory; if exists save a temp copy (so QImage load later is stable)
    // - else return candidate path (don't modify user-supplied semantics)
    if (texStr.isEmpty()) return QString();

    // embedded
    if (texStr.startsWith("*")) {
        bool ok = false;
        int idx = texStr.mid(1).toInt(&ok);
        if (ok && idx >= 0 && idx < int(scene->mNumTextures)) {
            aiTexture* at = scene->mTextures[idx];
            QImage img = convertAiTextureToQImage(at);
            QString suggested = guessFileNameFromAiString(at->mFilename);
            if (suggested.isEmpty()) {
                suggested = baseName + QString("_%1").arg(idx);
            }
            return saveEmbeddedTexture(img, suggested, outputFolder);
        }
    } else {
        // keep original path: do NOT modify string composition, just attempt to find relative file next to model
        QFileInfo fi(texStr);
        // try raw path first
        if (fi.exists()) {
            // good, use as-is (we will not rewrite absolute/relative paths)
            return texStr;
        }
        // try model directory + file name (do NOT rewrite original semantic, just a local candidate)
        QString candidate = QDir(QFileInfo(modelFilePath).absolutePath()).filePath(fi.fileName());
        if (QFileInfo::exists(candidate)) {
            // read and save to output folder (so that later loads use the saved stable path)
            QImage img(candidate);
            if (!img.isNull()) {
                qDebug() << "AssimpModelLoader: resolveTexturePath saved external texture to output:" << candidate;
                return saveEmbeddedTexture(img, fi.fileName(), outputFolder);
            }
            return candidate;
        }
        // fallback: return texStr unchanged (caller will try to load and fail if necessary)
        return texStr;
    }
    return QString();
}

// ------------------------- loadModel (final consolidated version) -------------------------
QVector<LoadedMesh> AssimpModelLoader::loadModel(
    const QString& filePath,
    const QString& outputFolder,
    vtkRenderer* renderer)
{
    QVector<LoadedMesh> results;

    if (filePath.isEmpty()) {
        qWarning() << "AssimpModelLoader: empty file path";
        return results;
    }

    // Assimp flags (same defaults you've been using)
    unsigned int flags = aiProcess_Triangulate
                         | aiProcess_GenSmoothNormals
                         | aiProcess_JoinIdenticalVertices
                         | aiProcess_ImproveCacheLocality
                         | aiProcess_RemoveRedundantMaterials
                         | aiProcess_GenUVCoords
                         | aiProcess_SortByPType
                         | aiProcess_OptimizeMeshes
                         | aiProcess_FlipUVs;

    qDebug() << "AssimpModelLoader: loading" << filePath << "with flags" << flags;

    const aiScene* scene = importer_.ReadFile(filePath.toStdString(), flags);
    if (!scene) {
        qWarning() << "AssimpModelLoader: failed to load" << filePath << ":" << importer_.GetErrorString();
        return results;
    }

    QString baseName = QFileInfo(filePath).baseName();
    qDebug() << "AssimpModelLoader: model loaded. meshes =" << scene->mNumMeshes
             << "materials =" << scene->mNumMaterials << "textures(embedded) =" << scene->mNumTextures;

    // texture cache: key = resolved path; value = vtkTexture
    QHash<QString, vtkSmartPointer<vtkTexture>> textureCache;

    // lambda helpers
    auto srgbToLinear = [](uchar v) -> uchar {
        double normalized = v / 255.0;
        double linear = pow(normalized, 2.2);
        return static_cast<uchar>(std::clamp(linear * 255.0, 0.0, 255.0));
    };

    auto loadTextureCached = [&](const QString& path, bool srgb)->vtkSmartPointer<vtkTexture> {
        if (path.isEmpty()) return nullptr;
        if (textureCache.contains(path)) return textureCache.value(path);
        QImage qimg(path);
        if (qimg.isNull()) {
            qWarning() << "AssimpModelLoader: loadTextureCached failed to load" << path;
            return nullptr;
        }
        // Optionally gamma-correct on CPU only if VTK build lacks sRGB handling (we still mark texture accordingly).
        QImage imgRGBA = qimg.convertToFormat(QImage::Format_RGBA8888);

        vtkSmartPointer<vtkTexture> tex = CreateVTKTextureFromQImage(imgRGBA, srgb);
        if (tex) {
            textureCache.insert(path, tex);
            qDebug() << "AssimpModelLoader: Loaded and cached texture:" << path;
        }
        return tex;
    };

    // iterate meshes
    for (unsigned int mi = 0; mi < scene->mNumMeshes; ++mi) {
        const aiMesh* aimesh = scene->mMeshes[mi];
        if (!aimesh) continue;

        QString meshName = (aimesh->mName.length > 0)
                               ? QString::fromUtf8(aimesh->mName.C_Str())
                               : QString("%1_mesh%2").arg(baseName).arg(mi);

        qDebug() << "Processing mesh[" << mi << "] name=" << meshName << " vertices=" << aimesh->mNumVertices;

        vtkSmartPointer<vtkPolyData> poly = convertAiMeshToVtkPolyData(aimesh);
        if (!poly || poly->GetNumberOfPoints() == 0) {
            qWarning() << "AssimpModelLoader: skipping mesh because convert returned null or empty:" << meshName;
            continue;
        }

        vtkSmartPointer<vtkPolyDataMapper> mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
        mapper->SetInputData(poly);

        vtkSmartPointer<vtkActor> actor = vtkSmartPointer<vtkActor>::New();
        actor->SetMapper(mapper);

        LoadedMesh mr;
        mr.polyData = poly;
        mr.actor = actor;
        mr.name = meshName;
        mr.aiMeshPtr = aimesh;
        mr.texturePath.clear();

        vtkProperty* prop = actor->GetProperty();
        if (!prop) prop = vtkProperty::New(); // defensive

        // ----------------- Material handling -----------------
        if (scene->mNumMaterials > 0 && aimesh->mMaterialIndex < scene->mNumMaterials) {
            const aiMaterial* mat = scene->mMaterials[aimesh->mMaterialIndex];
            qDebug() << "Mesh" << meshName << "material index" << aimesh->mMaterialIndex;

            // Diffuse (base) color
            aiColor3D diffc(1.0f, 1.0f, 1.0f);
            if (AI_SUCCESS == mat->Get(AI_MATKEY_COLOR_DIFFUSE, diffc)) {
                prop->SetColor(diffc.r, diffc.g, diffc.b);
                qDebug() << "Diffuse color set to:" << diffc.r << diffc.g << diffc.b;
            }

            // Opacity (material)
            float matOpacity = 1.0f;
            if (AI_SUCCESS == mat->Get(AI_MATKEY_OPACITY, matOpacity)) {
                if (matOpacity >= 0.995f) {
                    actor->SetForceOpaque(true);
                    prop->SetOpacity(1.0);
                } else {
                    actor->SetForceOpaque(false);
                    prop->SetOpacity(matOpacity);
                    actor->SetForceTranslucent(true);
                }
                qDebug() << "Material opacity:" << matOpacity;
            }

            // try set PBR mode
            prop->SetInterpolationToPBR();

            // sensible defaults that preserve color & shine
            prop->BackfaceCullingOn();    // prevent double-sided additive brightening
            // set roughness/metallic when possible (fallback fine)
            // NOTE: these are safe calls if compiled with VTK supporting these properties
            prop->SetRoughness(prop->GetRoughness() <= 0.0 ? 0.45 : prop->GetRoughness());
            prop->SetMetallic(prop->GetMetallic() <= 0.0 ? 0.05 : prop->GetMetallic());

            // ----------------- BaseColor / Diffuse texture -----------------
            if (mat->GetTextureCount(aiTextureType_BASE_COLOR) > 0 ||
                mat->GetTextureCount(aiTextureType_DIFFUSE) > 0) {
                aiString texPath;
                aiReturn ret = aiReturn_FAILURE;
                if (mat->GetTextureCount(aiTextureType_BASE_COLOR) > 0) {
                    ret = mat->GetTexture(aiTextureType_BASE_COLOR, 0, &texPath);
                } else {
                    ret = mat->GetTexture(aiTextureType_DIFFUSE, 0, &texPath);
                }
                if (ret == aiReturn_SUCCESS) {
                    QString texStr = QString::fromUtf8(texPath.C_Str());
                    QString savedPath = resolveTexturePath(texStr, scene, baseName, outputFolder, filePath);
                    if (!savedPath.isEmpty()) {
                        // keep mr.texturePath for debugging
                        mr.texturePath = savedPath;
                        vtkSmartPointer<vtkTexture> tex = loadTextureCached(savedPath, true);
                        if (tex) {
                            // assign texture both ways: actor->SetTexture (older path) and property->SetBaseColorTexture (PBR-aware)
                            actor->SetTexture(tex);
                            // if vtkProperty exposes SetBaseColorTexture (alias for albedoTex), call it:
                            // use function pointer lookup to be safer if method not present at compile? but we included vtkProperty, so call directly.
                            prop->SetBaseColorTexture(tex); // prefer PBR path
                            qDebug() << "BaseColor texture applied to mesh:" << meshName;
                        } else {
                            qWarning() << "AssimpModelLoader: failed to create VTK texture for" << savedPath;
                        }
                    } else {
                        qWarning() << "AssimpModelLoader: basecolor/diffuse texture not resolved:" << texStr;
                    }
                }
            }

            // ----------------- Normal map -----------------
            QString resolvedNormal;
            if (mat->GetTextureCount(aiTextureType_NORMALS) > 0) {
                aiString ntex;
                if (mat->GetTexture(aiTextureType_NORMALS, 0, &ntex) == aiReturn_SUCCESS) {
                    resolvedNormal = resolveTexturePath(QString::fromUtf8(ntex.C_Str()), scene, baseName, outputFolder, filePath);
                    if (!resolvedNormal.isEmpty()) {
                        vtkSmartPointer<vtkTexture> normalTex = loadTextureCached(resolvedNormal, false);
                        if (normalTex) {
                            // prefer property binding if available:
                            prop->SetNormalTexture(normalTex);
                            qDebug() << "Normal texture applied to mesh:" << meshName;
                        } else {
                            qDebug() << "AssimpModelLoader: normal texture loaded but not applied:" << resolvedNormal;
                        }
                    }
                }
            }

            // ----------------- ORM / Metallic/Roughness/Occlusion -----------------
            // Try KHR/glTF style packing first: METALNESS + DIFFUSE_ROUGHNESS etc.
            QString ormPath;
            if (mat->GetTextureCount(aiTextureType_GLTF_METALLIC_ROUGHNESS) > 0) {
                aiString orm;
                if (mat->GetTexture(aiTextureType_GLTF_METALLIC_ROUGHNESS, 0, &orm) == aiReturn_SUCCESS) {
                    ormPath = resolveTexturePath(QString::fromUtf8(orm.C_Str()), scene, baseName, outputFolder, filePath);
                }
            } else {
                // fallback: try separate channels if present (METALNESS / DIFFUSE_ROUGHNESS / AMBIENT_OCCLUSION)
                if (mat->GetTextureCount(aiTextureType_METALNESS) > 0 ||
                    mat->GetTextureCount(aiTextureType_DIFFUSE_ROUGHNESS) > 0 ||
                    mat->GetTextureCount(aiTextureType_AMBIENT_OCCLUSION) > 0) {
                    // If we find a combined ORM path use it; otherwise we won't synthesize here (complex).
                    aiString tmp;
                    if (mat->GetTextureCount(aiTextureType_METALNESS) > 0) {
                        mat->GetTexture(aiTextureType_METALNESS, 0, &tmp);
                        ormPath = resolveTexturePath(QString::fromUtf8(tmp.C_Str()), scene, baseName, outputFolder, filePath);
                    }
                }
            }
            if (!ormPath.isEmpty()) {
                vtkSmartPointer<vtkTexture> ormTex = loadTextureCached(ormPath, false); // linear
                if (ormTex) {
                    prop->SetORMTexture(ormTex);
                    qDebug() << "ORM texture applied to mesh:" << meshName;
                }
            }

        } // end material handling

        results.append(mr);
        qDebug() << "Mesh:" << mr.name << "PolyData ok Actor ok TexPath" << mr.texturePath;
    } // end meshes loop

    qDebug() << "AssimpModelLoader: finished processing. meshes output =" << results.size();
    return results;
}

} // namespace vtkmeta
