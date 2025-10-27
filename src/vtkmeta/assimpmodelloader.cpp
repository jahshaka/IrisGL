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

#include <QUuid>
#include <QDebug>
#include <QFile>
#include <QDir>

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
    // Assimp's aiTexel layout is rgba8 (aiTexel has r,g,b,a as unsigned char)
    const aiTexel* texels = reinterpret_cast<const aiTexel*>(at->pcData);
    if (!texels) return QImage();

    // Create QImage from raw RGBA - ensure a copy to be safe
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
        return fullPath;
    }

    // Save as PNG
    if (!img.save(fullPath, "PNG")) {
        qWarning() << "AssimpModelLoader: failed to save embedded texture to" << fullPath;
        return QString();
    }

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
    }

    // texture coordinates (only first set)
    if (mesh->HasTextureCoords(0)) {
        vtkSmartPointer<vtkFloatArray> tcoords = vtkSmartPointer<vtkFloatArray>::New();
        tcoords->SetNumberOfComponents(2);
        tcoords->SetName("TextureCoordinates");
        tcoords->SetNumberOfTuples(mesh->mNumVertices);
        for (unsigned int i = 0; i < mesh->mNumVertices; ++i) {
            const aiVector3D& uv = mesh->mTextureCoords[0][i];
            tcoords->SetTuple2(i, uv.x, uv.y);
        }
        poly->GetPointData()->SetTCoords(tcoords);
    }

    // indices (faces) - convert all faces (triangles/quads/etc.) to triangles
    vtkSmartPointer<vtkCellArray> polys = vtkSmartPointer<vtkCellArray>::New();

    for (unsigned int fi = 0; fi < mesh->mNumFaces; ++fi) {
        const aiFace& face = mesh->mFaces[fi];
        if (face.mNumIndices == 3) {
            vtkSmartPointer<vtkTriangle> tri = vtkSmartPointer<vtkTriangle>::New();
            tri->GetPointIds()->SetId(0, face.mIndices[0]);
            tri->GetPointIds()->SetId(1, face.mIndices[1]);
            tri->GetPointIds()->SetId(2, face.mIndices[2]);
            polys->InsertNextCell(tri);
        } else if (face.mNumIndices > 3) {
            // triangulate polygon fan (simple)
            for (unsigned int k = 1; k + 1 < face.mNumIndices; ++k) {
                vtkSmartPointer<vtkTriangle> tri = vtkSmartPointer<vtkTriangle>::New();
                tri->GetPointIds()->SetId(0, face.mIndices[0]);
                tri->GetPointIds()->SetId(1, face.mIndices[k]);
                tri->GetPointIds()->SetId(2, face.mIndices[k+1]);
                polys->InsertNextCell(tri);
            }
        } else {
            // ignore degenerate face
        }
    }

    poly->SetPolys(polys);
    poly->Modified();
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
        | aiProcess_FlipUVs
        ;

    const aiScene* scene = importer_.ReadFile(filePath.toStdString(), flags);
    if (!scene) {
        qWarning() << "AssimpModelLoader: failed to load" << filePath << ":" << importer_.GetErrorString();
        return results;
    }

    // base for texture suggested names
    QString baseName = QFileInfo(filePath).baseName();

    // process each mesh
    for (unsigned int mi = 0; mi < scene->mNumMeshes; ++mi) {
        const aiMesh* aimesh = scene->mMeshes[mi];
        if (!aimesh) continue;

        vtkSmartPointer<vtkPolyData> poly = convertAiMeshToVtkPolyData(aimesh);
        if (!poly) continue;

        vtkSmartPointer<vtkPolyDataMapper> mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
        mapper->SetInputData(poly);

        vtkSmartPointer<vtkActor> actor = vtkSmartPointer<vtkActor>::New();
        actor->SetMapper(mapper);

        LoadedMesh mr;
        mr.polyData = poly;
        mr.actor = actor;
        mr.name = aimesh->mName.length > 0 ? QString::fromUtf8(aimesh->mName.C_Str()) : QString("%1_mesh%2").arg(baseName).arg(mi);
        mr.aiMeshPtr = aimesh;
        mr.texturePath = QString();

        // Try material / diffuse texture
        if (scene->mNumMaterials > 0) {
            unsigned int matIndex = aimesh->mMaterialIndex;
            if (matIndex < scene->mNumMaterials) {
                const aiMaterial* mat = scene->mMaterials[matIndex];
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
                            // Try to find embedded by exact name
                            const aiTexture* emb = scene->GetEmbeddedTexture(texPath.C_Str());
                            if (emb) {
                                QImage img = convertAiTextureToQImage(emb);
                                QString suggested = guessFileNameFromAiString(texPath);
                                if (suggested.isEmpty()) suggested = baseName + QString("_mat%1").arg(matIndex);
                                savedPath = saveEmbeddedTexture(img, suggested, outputFolder);
                            } else {
                                // Not embedded - if it's relative, attempt to copy from source folder (not handled here).
                                // We just pass the original path (caller can resolve relative to model folder)
                                QString orig = QString::fromUtf8(texPath.C_Str());
                                // If relative, try to resolve: baseModelFolder + orig
                                QFileInfo fi(orig);
                                if (!fi.isAbsolute()) {
                                    QString modelDir = QFileInfo(filePath).absolutePath();
                                    QString candidate = QDir(modelDir).filePath(orig);
                                    if (QFileInfo::exists(candidate)) {
                                        // copy to outputFolder (avoid overriding)
                                        QImage img(candidate);
                                        QString suggested = QFileInfo(candidate).fileName();
                                        savedPath = saveEmbeddedTexture(img, suggested, outputFolder);
                                    } else {
                                        // leave as original string (upper layers can try to resolve)
                                        savedPath = QString::fromUtf8(texPath.C_Str());
                                    }
                                } else {
                                    // absolute path, try to copy
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
                            // Also attach vtkTexture to actor if you want VTK-side texturing immediately:
                            vtkSmartPointer<vtkTexture> vtktex = vtkSmartPointer<vtkTexture>::New();
                            // Create vtkImageData from QImage
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
                                // QImage lines may contain padding; copy line by line
                                int bytesPerLine = imgRGBA.bytesPerLine();
                                for (int y = 0; y < h; ++y) {
                                    memcpy(dest + y * w * 4, src + y * bytesPerLine, w * 4);
                                }
                                vtktex->SetInputData(imageData);
                                actor->SetTexture(vtktex);
                            }
                        }
                    }
                } // end diffuse texture check
            }
        }

        results.append(mr);
    } // end for meshes

    return results;
}

} // namespace vtkmeta
