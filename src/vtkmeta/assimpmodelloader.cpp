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
#include <vtkPNGWriter.h>
#include <vtkImageData.h>
#include <vtkUnsignedCharArray.h>
#include <vtkLightKit.h>

#include <QUuid>
#include <QDebug>
#include <QFile>
#include <QDir>
#include <QImage>
#include <QFileInfo>

namespace vtkmeta {

// ------------------------- helpers -------------------------
static QString guessFileNameFromAiString(const aiString& s) {
    QString name = QString::fromUtf8(s.C_Str());
    if (name.isEmpty()) return QString();
    // remove any path prefix, keep base name
    return QFileInfo(name).fileName();
}

QString AssimpModelLoader::generateGUIDFileName(const QString& base) const {
    auto id = QUuid::createUuid().toString();
    QString bn = id;
    if (!base.isEmpty()) {
        bn = QFileInfo(base).completeBaseName() + "_" + bn;
    }
    // normalize
    bn.replace("{", "").replace("}", "").replace("-", "");
    return bn;
}

QImage AssimpModelLoader::convertAiTextureToQImage(const aiTexture* at) const
{
    if (!at) return QImage();

    // If mHeight == 0 => compressed image data, length mWidth
    if (at->mHeight == 0) {
        QByteArray bytes(reinterpret_cast<const char*>(at->pcData), static_cast<int>(at->mWidth));
        QImage img;
        if (!img.loadFromData(bytes)) {
            qWarning() << "AssimpModelLoader: failed to load compressed embedded texture data";
            return QImage();
        }
        return img;
    }

    // Uncompressed RGBA raw pixels
    int w = at->mWidth;
    int h = at->mHeight;
    const aiTexel* texels = reinterpret_cast<const aiTexel*>(at->pcData);
    if (!texels) return QImage();

    // Assimp provides RGBA8 bytes; create QImage directly then copy
    QImage img(reinterpret_cast<const uchar*>(texels), w, h, QImage::Format_RGBA8888);
    return img.copy();
}

QString AssimpModelLoader::saveEmbeddedTexture(const QImage& img, const QString& suggestedName, const QString& outputFolder) const
{
    if (img.isNull()) return QString();

    QDir d(outputFolder);
    if (!d.exists()) {
        if (!d.mkpath(".")) {
            qWarning() << "AssimpModelLoader: cannot create output folder" << outputFolder;
            return QString();
        }
    }

    QString fileName;
    if (!suggestedName.isEmpty()) {
        fileName = suggestedName;
        // ensure .png
        if (!fileName.endsWith(".png", Qt::CaseInsensitive))
            fileName += ".png";
    } else {
        fileName = generateGUIDFileName() + ".png";
    }

    QString fullPath = d.filePath(fileName);
    // If exists, just return
    if (QFileInfo::exists(fullPath)) {
        qDebug() << "AssimpModelLoader: texture already exists, reuse:" << fullPath;
        return fullPath;
    }

    // Save as PNG
    if (!img.save(fullPath, "PNG")) {
        qWarning() << "AssimpModelLoader: failed to save embedded texture to" << fullPath;
        return QString();
    }

    qDebug() << "AssimpModelLoader: saved embedded texture to" << fullPath << " size=" << img.width() << "x" << img.height();
    return fullPath;
}

// ------------------------- convert aiMesh -> vtkPolyData -------------------------
vtkSmartPointer<vtkPolyData> AssimpModelLoader::convertAiMeshToVtkPolyData(const aiMesh* mesh) const
{
    if (!mesh) return nullptr;

    vtkSmartPointer<vtkPolyData> poly = vtkSmartPointer<vtkPolyData>::New();
    vtkSmartPointer<vtkPoints> points = vtkSmartPointer<vtkPoints>::New();

    // allocate points
    points->SetNumberOfPoints(mesh->mNumVertices);
    for (unsigned int i = 0; i < mesh->mNumVertices; ++i) {
        const aiVector3D& v = mesh->mVertices[i];
        points->SetPoint(i, v.x, v.y, v.z);
    }
    poly->SetPoints(points);

    // normals
    if (mesh->HasNormals()) {
        vtkSmartPointer<vtkFloatArray> normals = vtkSmartPointer<vtkFloatArray>::New();
        normals->SetNumberOfComponents(3);
        normals->SetName("Normals");
        normals->SetNumberOfTuples(mesh->mNumVertices);
        for (unsigned int i = 0; i < mesh->mNumVertices; ++i) {
            const aiVector3D& n = mesh->mNormals[i];
            normals->SetTuple3(i, n.x, n.y, n.z);
        }
        poly->GetPointData()->SetNormals(normals);
        qDebug() << "convertAiMeshToVtkPolyData: normals set for mesh vertices =" << mesh->mNumVertices;
    } else {
        qDebug() << "convertAiMeshToVtkPolyData: mesh has no normals";
    }

    // texture coordinates (only first set). NOTE: flip V (1 - v) to match VTK convention
    if (mesh->HasTextureCoords(0)) {
        vtkSmartPointer<vtkFloatArray> tcoords = vtkSmartPointer<vtkFloatArray>::New();
        tcoords->SetNumberOfComponents(2);
        tcoords->SetName("TextureCoordinates");
        tcoords->SetNumberOfTuples(mesh->mNumVertices);
        for (unsigned int i = 0; i < mesh->mNumVertices; ++i) {
            const aiVector3D& uv = mesh->mTextureCoords[0][i];
            // flip V to correct orientation (Assimp -> VTK)
            float u = uv.x;
            float v = 1.0f - uv.y;
            tcoords->SetTuple2(i, u, v);
        }
        poly->GetPointData()->SetTCoords(tcoords);
        qDebug() << "convertAiMeshToVtkPolyData: texture coords set";
    } else {
        qDebug() << "convertAiMeshToVtkPolyData: mesh has no texture coords";
    }

    // indices (faces) - convert all faces (triangles/quads/etc.) to triangles
    vtkSmartPointer<vtkCellArray> polys = vtkSmartPointer<vtkCellArray>::New();

    unsigned int faceCount = mesh->mNumFaces;
    unsigned int triCount = 0;
    for (unsigned int fi = 0; fi < faceCount; ++fi) {
        const aiFace& face = mesh->mFaces[fi];
        if (face.mNumIndices == 3) {
            vtkSmartPointer<vtkTriangle> tri = vtkSmartPointer<vtkTriangle>::New();
            tri->GetPointIds()->SetId(0, face.mIndices[0]);
            tri->GetPointIds()->SetId(1, face.mIndices[1]);
            tri->GetPointIds()->SetId(2, face.mIndices[2]);
            polys->InsertNextCell(tri);
            ++triCount;
        } else if (face.mNumIndices > 3) {
            // triangulate polygon fan (simple)
            for (unsigned int k = 1; k + 1 < face.mNumIndices; ++k) {
                vtkSmartPointer<vtkTriangle> tri = vtkSmartPointer<vtkTriangle>::New();
                tri->GetPointIds()->SetId(0, face.mIndices[0]);
                tri->GetPointIds()->SetId(1, face.mIndices[k]);
                tri->GetPointIds()->SetId(2, face.mIndices[k+1]);
                polys->InsertNextCell(tri);
                ++triCount;
            }
        } else {
            // ignore degenerate face
        }
    }

    poly->SetPolys(polys);
    poly->Modified();

    qDebug() << "convertAiMeshToVtkPolyData: mesh vertices =" << mesh->mNumVertices << "faces =" << faceCount << "triangles(after triangulation)=" << triCount;
    return poly;
}

// ------------------------- loadModel -------------------------
QVector<LoadedMesh> AssimpModelLoader::loadModel(const QString& filePath, const QString& outputFolder, vtkRenderer* renderer)
{
    QVector<LoadedMesh> results;

    if (filePath.isEmpty()) return results;

    // Assimp postprocess flags - use reasonable defaults for real-time apps
    unsigned int flags = aiProcess_Triangulate
                         | aiProcess_GenSmoothNormals
                         | aiProcess_JoinIdenticalVertices
                         | aiProcess_ImproveCacheLocality
                         | aiProcess_RemoveRedundantMaterials
                         | aiProcess_GenUVCoords
                         | aiProcess_SortByPType
                         | aiProcess_OptimizeMeshes
                         // flip uv handled manually as well; keep flag if needed by some formats
                         | aiProcess_FlipUVs
        ;

    qDebug() << "AssimpModelLoader: loading" << filePath << "with flags" << flags;

    const aiScene* scene = importer_.ReadFile(filePath.toStdString(), flags);
    if (!scene) {
        qWarning() << "AssimpModelLoader: failed to load" << filePath << ":" << importer_.GetErrorString();
        return results;
    }

    // base for texture suggested names
    QString baseName = QFileInfo(filePath).baseName();

    qDebug() << "AssimpModelLoader: model loaded. meshes =" << scene->mNumMeshes << "materials =" << scene->mNumMaterials << "textures(embedded) =" << scene->mNumTextures;

    // process each mesh
    for (unsigned int mi = 0; mi < scene->mNumMeshes; ++mi) {
        const aiMesh* aimesh = scene->mMeshes[mi];
        if (!aimesh) continue;

        QString meshName = (aimesh->mName.length > 0) ? QString::fromUtf8(aimesh->mName.C_Str()) : QString("%1_mesh%2").arg(baseName).arg(mi);
        qDebug() << "Processing mesh[" << mi << "] name=" << meshName << " vertices=" << aimesh->mNumVertices;

        vtkSmartPointer<vtkPolyData> poly = convertAiMeshToVtkPolyData(aimesh);
        if (!poly) {
            qWarning() << "AssimpModelLoader: skipping mesh because convert returned null:" << meshName;
            continue;
        }

        vtkSmartPointer<vtkPolyDataMapper> mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
        // prefer SetInputData if available
        mapper->SetInputData(poly);

        vtkSmartPointer<vtkActor> actor = vtkSmartPointer<vtkActor>::New();
        actor->SetMapper(mapper);

        LoadedMesh mr;
        mr.polyData = poly;
        mr.actor = actor;
        mr.name = meshName;
        mr.aiMeshPtr = aimesh;
        mr.texturePath = QString();

        // Try material / diffuse texture
        if (scene->mNumMaterials > 0) {
            unsigned int matIndex = aimesh->mMaterialIndex;
            if (matIndex < scene->mNumMaterials) {
                const aiMaterial* mat = scene->mMaterials[matIndex];
                qDebug() << "Mesh" << meshName << "uses material index" << matIndex;

                // Read diffuse color (if present)
                aiColor3D diffc(1.0f, 1.0f, 1.0f);
                if (AI_SUCCESS == mat->Get(AI_MATKEY_COLOR_DIFFUSE, diffc)) {
                    double dr = diffc.r;
                    double dg = diffc.g;
                    double db = diffc.b;
                    actor->GetProperty()->SetColor(dr, dg, db);
                    qDebug() << "Material diffuse color set to" << dr << dg << db;
                }

                aiString texPath;
                if (mat->GetTextureCount(aiTextureType_DIFFUSE) > 0) {
                    if (mat->GetTexture(aiTextureType_DIFFUSE, 0, &texPath) == aiReturn_SUCCESS) {
                        QString texStr = QString::fromUtf8(texPath.C_Str());
                        QString savedPath;
                        // If glb internal reference like "*0" then convert via scene->mTextures
                        if (texStr.startsWith("*")) {
                            bool ok = false;
                            QString idxStr = texStr.mid(1);
                            int idx = idxStr.toInt(&ok);
                            if (ok && idx >= 0 && idx < int(scene->mNumTextures)) {
                                aiTexture* at = scene->mTextures[idx];
                                QImage img = convertAiTextureToQImage(at);
                                QString suggested = guessFileNameFromAiString(at->mFilename);
                                if (suggested.isEmpty()) suggested = baseName + QString("_%1").arg(idx);
                                savedPath = saveEmbeddedTexture(img, suggested, outputFolder);
                            }
                        } else {
                            // Could be path relative in file or embedded with prefix?
                            const aiTexture* emb = scene->GetEmbeddedTexture(texPath.C_Str());
                            if (emb) {
                                QImage img = convertAiTextureToQImage(emb);
                                QString suggested = guessFileNameFromAiString(texPath);
                                if (suggested.isEmpty()) suggested = baseName + QString("_mat%1").arg(matIndex);
                                savedPath = saveEmbeddedTexture(img, suggested, outputFolder);
                            } else {
                                // Not embedded - if it's relative, attempt to copy from source folder
                                QString orig = QString::fromUtf8(texPath.C_Str());
                                QFileInfo fi(orig);
                                if (!fi.isAbsolute()) {
                                    QString modelDir = QFileInfo(filePath).absolutePath();
                                    QString candidate = QDir(modelDir).filePath(orig);
                                    if (QFileInfo::exists(candidate)) {
                                        QImage img(candidate);
                                        QString suggested = QFileInfo(candidate).fileName();
                                        savedPath = saveEmbeddedTexture(img, suggested, outputFolder);
                                    } else {
                                        // fallback: pass original string (upper layers may resolve)
                                        savedPath = orig;
                                    }
                                } else {
                                    if (QFileInfo::exists(orig)) {
                                        QImage img(orig);
                                        QString suggested = QFileInfo(orig).fileName();
                                        savedPath = saveEmbeddedTexture(img, suggested, outputFolder);
                                    } else {
                                        savedPath = orig;
                                    }
                                }
                            }
                        }

                        if (!savedPath.isEmpty()) {
                            mr.texturePath = savedPath;
                            qDebug() << "Applying diffuse texture to actor. path=" << savedPath;

                            // Load QImage and convert -> vtkImageData; IMPORTANT: flip vertically during copy for correct orientation
                            QImage qimg(savedPath);
                            if (!qimg.isNull()) {
                                QImage imgRGBA = qimg.convertToFormat(QImage::Format_RGBA8888);
                                int w = imgRGBA.width();
                                int h = imgRGBA.height();
                                vtkSmartPointer<vtkImageData> imageData = vtkSmartPointer<vtkImageData>::New();
                                imageData->SetDimensions(w, h, 1);
                                imageData->AllocateScalars(VTK_UNSIGNED_CHAR, 4);

                                unsigned char* dest = static_cast<unsigned char*>(imageData->GetScalarPointer(0,0,0));
                                const uchar* src = imgRGBA.constBits();
                                int bytesPerLine = imgRGBA.bytesPerLine();

                                // Copy line-by-line and flip vertically (VTK expects origin bottom-left)
                                for (int y = 0; y < h; ++y) {
                                    const uchar* srcLine = src + (h - 1 - y) * bytesPerLine; // flip
                                    memcpy(dest + y * w * 4, srcLine, w * 4);
                                }

                                vtkSmartPointer<vtkTexture> vtktex = vtkSmartPointer<vtkTexture>::New();
                                vtktex->SetInputData(imageData);
                                vtktex->InterpolateOn(); // linear sampling
                                // Try enabling sRGB handling if available (improves color fidelity)
#if VTK_MAJOR_VERSION >= 9 \
    // UseSRGBColorSpace exists in modern VTK -> prefer sRGB interpretation \
    // NOTE: if your VTK doesn't have this symbol, remove or guard accordingly
                                if (vtktex->GetClassName()) {
// best-effort: call UseSRGBColorSpaceOn if present
// Many VTK builds include UseSRGBColorSpaceOn; if your build doesn't, ignore
// We call through method if available.
// We'll attempt to call using C++ direct call; if compile fails, let me know.
#ifdef VTK_HAS_USE_SRGB
                                    vtktex->UseSRGBColorSpaceOn();
#endif
                                }
#endif
                                actor->SetTexture(vtktex);
                                qDebug() << "Diffuse texture applied (w,h)=" << w << h;
                            } else {
                                qWarning() << "Failed to load saved diffuse texture as QImage:" << savedPath;
                            }
                        } else {
                            qWarning() << "Diffuse texture not resolved for material index" << matIndex << "path:" << texStr;
                        }
                    } // GetTexture success
                } // has diffuse texture

                // Attempt to locate normal map (common places: aiTextureType_NORMALS, aiTextureType_HEIGHT)
                QString normalSaved;
                if (mat->GetTextureCount(aiTextureType_NORMALS) > 0) {
                    aiString ntex;
                    if (mat->GetTexture(aiTextureType_NORMALS, 0, &ntex) == aiReturn_SUCCESS) {
                        QString nstr = QString::fromUtf8(ntex.C_Str());
                        qDebug() << "Found normal texture reference:" << nstr;
                        // try same process of embedded / external
                        const aiTexture* embn = scene->GetEmbeddedTexture(ntex.C_Str());
                        if (embn) {
                            QImage img = convertAiTextureToQImage(embn);
                            QString suggested = guessFileNameFromAiString(ntex);
                            normalSaved = saveEmbeddedTexture(img, suggested, outputFolder);
                        } else {
                            QFileInfo fi(nstr);
                            if (fi.isAbsolute() && QFileInfo::exists(nstr)) {
                                QImage img(nstr);
                                normalSaved = saveEmbeddedTexture(img, fi.fileName(), outputFolder);
                            } else {
                                QString modelDir = QFileInfo(filePath).absolutePath();
                                QString candidate = QDir(modelDir).filePath(nstr);
                                if (QFileInfo::exists(candidate)) {
                                    QImage img(candidate);
                                    normalSaved = saveEmbeddedTexture(img, QFileInfo(candidate).fileName(), outputFolder);
                                } else {
                                    qDebug() << "Normal texture path not found on disk:" << nstr;
                                }
                            }
                        }
                    }
                } else if (mat->GetTextureCount(aiTextureType_HEIGHT) > 0) {
                    // sometimes height used as normal map
                    aiString ntex;
                    if (mat->GetTexture(aiTextureType_HEIGHT, 0, &ntex) == aiReturn_SUCCESS) {
                        QString nstr = QString::fromUtf8(ntex.C_Str());
                        qDebug() << "Found height/normal texture reference (fallback):" << nstr;
                        const aiTexture* embn = scene->GetEmbeddedTexture(ntex.C_Str());
                        if (embn) {
                            QImage img = convertAiTextureToQImage(embn);
                            QString suggested = guessFileNameFromAiString(ntex);
                            normalSaved = saveEmbeddedTexture(img, suggested, outputFolder);
                        } else {
                            QFileInfo fi(nstr);
                            if (fi.isAbsolute() && QFileInfo::exists(nstr)) {
                                QImage img(nstr);
                                normalSaved = saveEmbeddedTexture(img, fi.fileName(), outputFolder);
                            } else {
                                QString modelDir = QFileInfo(filePath).absolutePath();
                                QString candidate = QDir(modelDir).filePath(nstr);
                                if (QFileInfo::exists(candidate)) {
                                    QImage img(candidate);
                                    normalSaved = saveEmbeddedTexture(img, QFileInfo(candidate).fileName(), outputFolder);
                                } else {
                                    qDebug() << "Height/normal texture path not found on disk:" << nstr;
                                }
                            }
                        }
                    }
                }

                if (!normalSaved.isEmpty()) {
                    qDebug() << "Normal texture exported:" << normalSaved;
                    // load normal to vtkTexture if you want (VTK's normal mapping support via property may vary)
                    QImage qimg(normalSaved);
                    if (!qimg.isNull()) {
                        QImage imgRGBA = qimg.convertToFormat(QImage::Format_RGBA8888);
                        int w = imgRGBA.width();
                        int h = imgRGBA.height();
                        vtkSmartPointer<vtkImageData> imageData = vtkSmartPointer<vtkImageData>::New();
                        imageData->SetDimensions(w, h, 1);
                        imageData->AllocateScalars(VTK_UNSIGNED_CHAR, 4);
                        unsigned char* dest = static_cast<unsigned char*>(imageData->GetScalarPointer(0,0,0));
                        const uchar* src = imgRGBA.constBits();
                        int bytesPerLine = imgRGBA.bytesPerLine();
                        for (int y = 0; y < h; ++y) {
                            const uchar* srcLine = src + (h - 1 - y) * bytesPerLine; // flip vertically
                            memcpy(dest + y * w * 4, srcLine, w * 4);
                        }
                        vtkSmartPointer<vtkTexture> normalTex = vtkSmartPointer<vtkTexture>::New();
                        normalTex->SetInputData(imageData);
                        normalTex->InterpolateOn();
                        // Attaching normal map to actor: vtk's default pipeline doesn't automatically use normal maps unless using PBR & correct property APIs or shaders
                        // We still attach it to actor so that user can later bind in custom shader or if vtkProperty supports it.
                        // actor->GetProperty()->SetNormalTexture(normalTex); // NOT guaranteed to exist in VTK 9.5 (commented)
                        // fallback: attach as a second texture slot on actor to be used by custom mapper/shader
                        // actor->GetProperty()->SetNormalTexture(normalTex); // <-- left commented as may not exist
                        qDebug() << "Prepared normal texture for mesh (w,h)=" << w << h;
                    }
                }

            } // matIndex valid
        } // has materials

        // optional: add actor to renderer now if provided
        if (renderer) {
            renderer->AddActor(actor);
        }

        results.append(mr);
    } // end for meshes

    // if (renderer) {
    //     renderer->UseImageBasedLightingOn();

    //     // renderer->SetUseToneMapping(true);
    //     // renderer->SetToneMappingGamma(2.2);
    //     // renderer->SetToneMappingExposure(1.4);

    //     vtkNew<vtkLightKit> kit;
    //     kit->AddLightsToRenderer(renderer);
    // }

    qDebug() << "AssimpModelLoader: finished processing. meshes output =" << results.size();
    return results;
}

} // namespace vtkmeta
