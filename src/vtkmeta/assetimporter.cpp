#include "assetimporter.h"


#include <QFileInfo>
#include <QDir>
#include <QMutex>
#include <QBuffer>
#include <QImage>

#include <vtkFloatArray.h>
#include <vtkPointData.h>
#include <vtkTriangle.h>

#include "importerhelper.h"

namespace vtkmeta {

// QMutex g_textureGuidMapMutex;
// QHash<QString, QString> g_textureGuidMap;

ImportResult AssetImporter::importModel(
    const QString& externalFilePath,
    const QString& assetOutputFolder)
{
    ImportResult finalResult;

    if (externalFilePath.isEmpty()) {
        qWarning() << "AssimpModelImporter: empty file path";
        return finalResult;
    }

    unsigned int flags = aiProcess_Triangulate | aiProcess_GenSmoothNormals | aiProcess_JoinIdenticalVertices
                         | aiProcess_RemoveRedundantMaterials | aiProcess_GenUVCoords
                         | aiProcess_OptimizeMeshes | aiProcess_FlipUVs;

    const aiScene* scene = importer_.ReadFile(externalFilePath.toStdString(), flags);
    if (!scene) {
        qWarning() << "AssimpModelImporter: failed to load" << externalFilePath << ":" << importer_.GetErrorString();
        return finalResult;
    }

    QString baseName = QFileInfo(externalFilePath).baseName();
    QList<TextureImportTask> textureTasks;
    QVector<ImporedMesh> loadedMeshes;

    std::function<void(const aiNode*, const aiMatrix4x4&)> processNode;
    processNode = [&](const aiNode* node, const aiMatrix4x4& parentTransform)
    {
        aiMatrix4x4 currentTransform = parentTransform * node->mTransformation;

        for (unsigned int i = 0; i < node->mNumMeshes; ++i) {
            unsigned int meshIdx = node->mMeshes[i];
            const aiMesh* aimesh = scene->mMeshes[meshIdx];

            if (!aimesh) {
                continue;
            }

            QString meshName = (aimesh->mName.length > 0)
                                   ? QString::fromUtf8(aimesh->mName.C_Str())
                                   : QString("%1_mesh%2").arg(baseName).arg(meshIdx);

            vtkSmartPointer<vtkPolyData> poly = convertAiMeshToVtkPolyData(aimesh);
            if (!poly || poly->GetNumberOfPoints() == 0) continue;

            ImporedMesh mr;
            mr.polyData_ = poly;
            mr.name_ = meshName;
            mr.mesh_index_ = meshIdx;

            if (scene->mNumMaterials > 0 && aimesh->mMaterialIndex < scene->mNumMaterials) {
                const aiMaterial* mat = scene->mMaterials[aimesh->mMaterialIndex];

                aiColor3D diffc(1.0f, 1.0f, 1.0f);
                mat->Get(AI_MATKEY_COLOR_DIFFUSE, diffc);
                mr.materialInfo.base_color_ = QVector3D(diffc.r, diffc.g, diffc.b);

                float metallic = 0.0f, roughness = 0.5f, opacity = 1.0f;
                mat->Get(AI_MATKEY_METALLIC_FACTOR, metallic);
                mat->Get(AI_MATKEY_ROUGHNESS_FACTOR, roughness);
                mat->Get(AI_MATKEY_OPACITY, opacity);

                mr.materialInfo.metallic_ = metallic;
                mr.materialInfo.roughness_ = roughness;
                mr.materialInfo.opacity_ = opacity;

                auto createTextureTask = [&](aiTextureType type) {
                    if (mat->GetTextureCount(type) > 0)
                        textureTasks.append({mat, type, externalFilePath, mr.name_, scene});
                };

                createTextureTask(aiTextureType_BASE_COLOR);
                createTextureTask(aiTextureType_DIFFUSE);
                createTextureTask(aiTextureType_NORMALS);
                createTextureTask(aiTextureType_GLTF_METALLIC_ROUGHNESS);
                createTextureTask(aiTextureType_EMISSIVE);
            }

            loadedMeshes.append(std::move(mr));
        }

        for (unsigned int c = 0; c < node->mNumChildren; ++c)
            processNode(node->mChildren[c], currentTransform);
    };

    processNode(scene->mRootNode, aiMatrix4x4());

    qDebug() << "task.....number:---" << textureTasks.count();

    ImporterHelper helper;
    QVector<TextureMapResult> finalTextureResults = helper.processTextures(
        textureTasks,
        assetOutputFolder
        );

    qDebug() << "hexxxxxxxxxxxxxxxxxxxxx=--------------------------------------";

    for (ImporedMesh& mesh : loadedMeshes) {
        for (const TextureMapResult& result : finalTextureResults) {
            if (result.mesh_name_ == mesh.name_) {
                if (result.texture_type_ == aiTextureType_BASE_COLOR || result.texture_type_ == aiTextureType_DIFFUSE)
                    mesh.materialInfo.diffuse_path_ = result.guid_;
                else if (result.texture_type_ == aiTextureType_NORMALS)
                    mesh.materialInfo.normal_path_ = result.guid_;
                else if (result.texture_type_ == aiTextureType_GLTF_METALLIC_ROUGHNESS)
                    mesh.materialInfo.orm_path_ = result.guid_;
                else if (result.texture_type_ == aiTextureType_EMISSIVE)
                    mesh.materialInfo.emissive_path_ = result.guid_;
            }
        }
    }

    finalResult.meshes_ = loadedMeshes;
    finalResult.texture_results_ = std::move(finalTextureResults);

    return finalResult;
}

QByteArray AssetImporter::imageToByteArray(const QImage &img, const QString &format) const
{
    if (img.isNull()) return QByteArray();
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);

    img.save(&buffer, format.toLatin1().constData());
    return bytes;
}

vtkSmartPointer<vtkPolyData> AssetImporter::convertAiMeshToVtkPolyData(const aiMesh *mesh) const
{
    if (!mesh) {
        return nullptr;
    }

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

} // namespace vtkmeta
