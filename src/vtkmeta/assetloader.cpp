// assimpmodelloader.cpp
#include "assetloader.h"

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
#include <vtkTransform.h>
#include <vtkTransformPolyDataFilter.h>

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

QString AssetLoader::generateGUIDFileName(const QString& base) const {
    QString bn = QUuid::createUuid().toString();
    if (!base.isEmpty()) {
        bn = QFileInfo(base).completeBaseName() + "_" + bn;
    }
    bn.replace("{", "").replace("}", "").replace("-", "");
    return bn;
}

// ------------------------- convert aiMesh -> vtkPolyData -------------------------
vtkSmartPointer<vtkPolyData> AssetLoader::convertAiMeshToVtkPolyData(const aiMesh* mesh) const
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
vtkSmartPointer<vtkTexture> AssetLoader::CreateVTKTextureFromQImage(const QImage& img, bool srgb) const
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
    tex->SetColorModeToDirectScalars();
    tex->MipmapOn();
    tex->RepeatOn();
    tex->SetInputData(imageData);
    tex->InterpolateOn();
    tex->EdgeClampOn();
    tex->SetUseSRGBColorSpace(srgb);

    // help with blending/alpha
    tex->SetPremultipliedAlpha(true);
    tex->SetBlendingMode(vtkTexture::VTK_TEXTURE_BLENDING_MODE_REPLACE);

    return tex;
}

// ------------------------- resolve texture path (DO NOT CHANGE PATH LOGIC) -------------------------
QString AssetLoader::resolveTexturePath(const QString& texStr, const aiScene* scene, const QString& baseName,
                                              const QString& outputFolder, const QString& modelFilePath) const
{
    // Keep the same behavior you used previously:
    // - embedded (*0 style) -> saved to outputFolder via saveEmbeddedTexture
    // - else try relative to modelFilePath directory; if exists save a temp copy (so QImage load later is stable)
    // - else return candidate path (don't modify user-supplied semantics)
    if (texStr.isEmpty()) return QString();

    QString texture_name("");

    if (texStr.startsWith("*")) {
        bool ok = false;
        int idx = texStr.mid(1).toInt(&ok);
        if (ok && idx >= 0 && idx < int(scene->mNumTextures)) {
            texture_name = QString("tex_%1.png").arg(idx);
        }
    } else {
        // keep original path: do NOT modify string composition, just attempt to find relative file next to model
        QFileInfo fi(texStr);

        texture_name = fi.fileName();
    }


    if (texture_name.isEmpty()) {
        return QString();
    }

    return QDir(QFileInfo(modelFilePath).absolutePath()).filePath(texture_name);
}


// ------------------------- loadModel (recursive node transform version) -------------------------
QVector<LoadedMesh> AssetLoader::loadModel(
    const QString& filePath,
    const QString& outputFolder,
    vtkRenderer* renderer)
{
    QVector<LoadedMesh> results;

    if (filePath.isEmpty()) {
        qWarning() << "AssimpModelLoader: empty file path";
        return results;
    }

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

    QHash<QString, vtkSmartPointer<vtkTexture>> textureCache;

    auto loadTextureCached = [&](const QString& path, bool srgb) -> vtkSmartPointer<vtkTexture> {
        if (path.isEmpty()) return nullptr;
        if (textureCache.contains(path)) return textureCache.value(path);
        QImage qimg(path);
        if (qimg.isNull()) {
            //qWarning() << "AssimpModelLoader: loadTextureCached failed to load" << path;
            return nullptr;
        }
        QImage imgRGBA = qimg.convertToFormat(QImage::Format_RGBA8888);
        vtkSmartPointer<vtkTexture> tex = CreateVTKTextureFromQImage(imgRGBA, srgb);
        if (tex) {
            textureCache.insert(path, tex);
            //qDebug() << "AssimpModelLoader: Loaded and cached texture:" << path;
        }
        return tex;
    };

    // aiMatrix4x4 -> vtkMatrix4x4
    auto aiToVtkMatrix = [](const aiMatrix4x4& aimat) -> vtkSmartPointer<vtkMatrix4x4> {
        vtkSmartPointer<vtkMatrix4x4> m = vtkSmartPointer<vtkMatrix4x4>::New();
        m->SetElement(0, 0, aimat.a1); m->SetElement(0, 1, aimat.a2); m->SetElement(0, 2, aimat.a3); m->SetElement(0, 3, aimat.a4);
        m->SetElement(1, 0, aimat.b1); m->SetElement(1, 1, aimat.b2); m->SetElement(1, 2, aimat.b3); m->SetElement(1, 3, aimat.b4);
        m->SetElement(2, 0, aimat.c1); m->SetElement(2, 1, aimat.c2); m->SetElement(2, 2, aimat.c3); m->SetElement(2, 3, aimat.c4);
        m->SetElement(3, 0, aimat.d1); m->SetElement(3, 1, aimat.d2); m->SetElement(3, 2, aimat.d3); m->SetElement(3, 3, aimat.d4);
        return m;
    };

    std::function<void(const aiNode*, const aiMatrix4x4&)> processNode;
    processNode = [&](const aiNode* node, const aiMatrix4x4& parentTransform)
    {
        aiMatrix4x4 currentTransform = parentTransform * node->mTransformation;

        for (unsigned int i = 0; i < node->mNumMeshes; ++i) {
            unsigned int meshIdx = node->mMeshes[i];
            const aiMesh* aimesh = scene->mMeshes[meshIdx];
            if (!aimesh) continue;

            QString meshName = (aimesh->mName.length > 0)
                                   ? QString::fromUtf8(aimesh->mName.C_Str())
                                   : QString("%1_mesh%2").arg(baseName).arg(meshIdx);

            // qDebug() << "Processing mesh[" << meshIdx << "] name=" << meshName
            //          << " vertices=" << aimesh->mNumVertices;

            vtkSmartPointer<vtkPolyData> poly = convertAiMeshToVtkPolyData(aimesh);
            if (!poly || poly->GetNumberOfPoints() == 0) {
                qWarning() << "AssimpModelLoader: skipping empty mesh:" << meshName;
                continue;
            }

            //Apply node transformation
            vtkSmartPointer<vtkTransform> transform = vtkSmartPointer<vtkTransform>::New();
            transform->SetMatrix(aiToVtkMatrix(currentTransform));
            vtkSmartPointer<vtkTransformPolyDataFilter> tfilter = vtkSmartPointer<vtkTransformPolyDataFilter>::New();
            tfilter->SetInputData(poly);
            tfilter->SetTransform(transform);
            tfilter->Update();
            poly = tfilter->GetOutput();

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
            if (!prop) prop = vtkProperty::New();

            prop->SetInterpolationToGouraud();
            prop->SetInterpolationToPBR();
            prop->BackfaceCullingOn();

            // ------------- Material handling (your original logic) -------------
            if (scene->mNumMaterials > 0 && aimesh->mMaterialIndex < scene->mNumMaterials) {
                const aiMaterial* mat = scene->mMaterials[aimesh->mMaterialIndex];
                // qDebug() << "Mesh" << meshName << "material index" << aimesh->mMaterialIndex;

                aiColor3D diffc(1.0f, 1.0f, 1.0f);
                if (AI_SUCCESS == mat->Get(AI_MATKEY_COLOR_DIFFUSE, diffc)) {
                    prop->SetColor(diffc.r, diffc.g, diffc.b);
                    // qDebug() << "Diffuse color set to:" << diffc.r << diffc.g << diffc.b;
                }

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
                }

                float metallic = -1.0f, roughness = -1.0f;
                if (AI_SUCCESS == mat->Get(AI_MATKEY_METALLIC_FACTOR, metallic)) {
                    prop->SetMetallic(metallic);
                }

                if (AI_SUCCESS == mat->Get(AI_MATKEY_ROUGHNESS_FACTOR, roughness)) {
                    prop->SetRoughness(roughness);//std::max(roughness, 0.55f));
                    //prop->SetRoughness(roughness);
                }

                // --- BaseColor / Diffuse texture ---
                if (mat->GetTextureCount(aiTextureType_BASE_COLOR) > 0 ||
                    mat->GetTextureCount(aiTextureType_DIFFUSE) > 0)
                {
                    aiString texPath;
                    aiReturn ret = aiReturn_FAILURE;
                    if (mat->GetTextureCount(aiTextureType_BASE_COLOR) > 0)
                        ret = mat->GetTexture(aiTextureType_BASE_COLOR, 0, &texPath);
                    else
                        ret = mat->GetTexture(aiTextureType_DIFFUSE, 0, &texPath);

                    if (ret == aiReturn_SUCCESS) {
                        QString texStr = QString::fromUtf8(texPath.C_Str());
                        QString savedPath = resolveTexturePath(texStr, scene, baseName, outputFolder, filePath);
                        if (!savedPath.isEmpty()) {
                            mr.texturePath = savedPath;
                            vtkSmartPointer<vtkTexture> tex = loadTextureCached(savedPath, true);
                            if (tex) {
                                // actor->SetTexture(tex);
                                prop->SetBaseColorTexture(tex);
                                prop->SetColor(1.0, 1.0, 1.0); // 避免暗淡
                                // qDebug() << "BaseColor texture applied to mesh:" << meshName;
                            }
                        }
                    }
                }

                // --- Normal map ---
                if (mat->GetTextureCount(aiTextureType_NORMALS) > 0) {
                    aiString ntex;
                    if (mat->GetTexture(aiTextureType_NORMALS, 0, &ntex) == aiReturn_SUCCESS) {
                        QString resolvedNormal = resolveTexturePath(QString::fromUtf8(ntex.C_Str()), scene, baseName, outputFolder, filePath);
                        if (!resolvedNormal.isEmpty()) {
                            vtkSmartPointer<vtkTexture> normalTex = loadTextureCached(resolvedNormal, false);
                            if (normalTex) {
                                prop->SetNormalTexture(normalTex);
                                // qDebug() << "Normal texture applied to mesh:" << meshName;
                            }
                        }
                    }
                }

                // --- ORM / Metallic-Roughness ---
                QString ormPath;
                if (mat->GetTextureCount(aiTextureType_GLTF_METALLIC_ROUGHNESS) > 0) {
                    aiString orm;
                    if (mat->GetTexture(aiTextureType_GLTF_METALLIC_ROUGHNESS, 0, &orm) == aiReturn_SUCCESS)
                        ormPath = resolveTexturePath(QString::fromUtf8(orm.C_Str()), scene, baseName, outputFolder, filePath);
                }
                if (!ormPath.isEmpty()) {
                    vtkSmartPointer<vtkTexture> ormTex = loadTextureCached(ormPath, false);
                    if (ormTex) {
                        prop->SetORMTexture(ormTex);
                        // qDebug() << "ORM texture applied to mesh:" << meshName;
                    }
                }
            }

            results.append(mr);
            // qDebug() << "Mesh:" << mr.name << "PolyData ok Actor ok TexPath" << mr.texturePath;
        }

        for (unsigned int c = 0; c < node->mNumChildren; ++c)
            processNode(node->mChildren[c], currentTransform);
    };

    processNode(scene->mRootNode, aiMatrix4x4());

    // qDebug() << "AssimpModelLoader: finished processing. meshes output =" << results.size();
    return results;
}

} // namespace vtkmeta
