#include "assetimporter.h"

#include <QFileInfo>
#include <QDir>
#include <QBuffer>
#include <QImage>
#include <QJsonObject>
#include <QJsonArray>
#include <QUuid>
#include <QDebug>

#include <assimp/scene.h>
#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>

#include <vtkFloatArray.h>
#include <vtkPointData.h>
#include <vtkTriangle.h>

#include "modeldocumentserializer.h"
#include "importerhelper.h"
#include "assetdatatypes.h"

namespace vtkmeta {

static QString makeId() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

ImportResult AssetImporter::importModel(
    const QString& externalFilePath,
    const QString& assetOutputFolder)
{
    ImportResult finalResult;

    if (externalFilePath.isEmpty()) {
        qWarning() << "AssetImporter: empty file path";
        return finalResult;
    }

    unsigned int flags = aiProcess_Triangulate |
                         aiProcess_GenSmoothNormals |
                         aiProcess_JoinIdenticalVertices |
                         aiProcess_RemoveRedundantMaterials |
                         aiProcess_GenUVCoords |
                         aiProcess_OptimizeMeshes |
                         aiProcess_FlipUVs;

    const aiScene* scene = importer_.ReadFile(externalFilePath.toStdString(), flags);
    if (!scene) {
        qWarning() << "AssetImporter: failed to load" << externalFilePath << ":" << importer_.GetErrorString();
        return finalResult;
    }

    QFileInfo srcInfo(externalFilePath);
    QString baseName = srcInfo.baseName();

    // create model doc
    ModelDocument doc;
    doc.id_ = makeId();
    doc.name_ = baseName;
    doc.source_file_ = srcInfo.fileName(); // store original
    // ensure output folder exists: assetOutputFolder/<doc.id_>/
    // QDir rootDir(assetOutputFolder);
    // rootDir.mkpath(".");
    // QString modelDirPath = QDir(assetOutputFolder).filePath(doc.id_);
    // QDir modelDir(modelDirPath);
    // modelDir.mkpath(".");

    // // copy original file into modelDir (preserve extension)
    // QString destModelFileName = QString("model.%1").arg(srcInfo.suffix());
    // QString destModelFilePath = modelDir.filePath(destModelFileName);
    // if (!QFile::copy(externalFilePath, destModelFilePath)) {
    //     // fallback: try to read/write
    //     QFile::remove(destModelFilePath);
    //     if (!QFile::copy(externalFilePath, destModelFilePath)) {
    //         qWarning() << "AssetImporter: failed to copy model file to" << destModelFilePath;
    //         return finalResult;
    //     }
    // }
    // doc.source_file_ = destModelFileName;

    // keep a mapping from assimp mesh index -> mesh id
    QMap<int, QString> meshIndexToId;
    QMap<int, QString> meshIndexToMaterialId;

    // process materials first: create MaterialDef per assimp material encountered
    for (unsigned int m = 0; m < scene->mNumMaterials; ++m) {
        const aiMaterial* aimat = scene->mMaterials[m];
        MaterialDef mat;
        mat.id_ = makeId();
        // try to get name
        aiString name;
        if (AI_SUCCESS == aimat->Get(AI_MATKEY_NAME, name)) mat.name_ = QString::fromUtf8(name.C_Str());
        else mat.name_ = QString("material_%1").arg(m);

        aiColor3D diff(1.0f,1.0f,1.0f);
        if (AI_SUCCESS == aimat->Get(AI_MATKEY_COLOR_DIFFUSE, diff)) {
            mat.base_color_ = QVector3D(diff.r, diff.g, diff.b);
        }

        float metallic = 0.0f, roughness = 0.5f, opacity = 1.0f;
        aimat->Get(AI_MATKEY_METALLIC_FACTOR, metallic);
        aimat->Get(AI_MATKEY_ROUGHNESS_FACTOR, roughness);
        aimat->Get(AI_MATKEY_OPACITY, opacity);
        mat.metallic_ = metallic;
        mat.roughness_ = roughness;
        mat.opacity_ = opacity;

        // textures will be filled later after importerhelper processing
        doc.materials_.append(mat);
    }

    // process meshes: convert to MeshDef (and also keep vtk polydata if needed)
    QVector<ImportedMesh> loadedMeshes;
    for (unsigned int mi = 0; mi < scene->mNumMeshes; ++mi) {
        const aiMesh* aimesh = scene->mMeshes[mi];
        if (!aimesh) continue;

        ImportedMesh im;
        im.polyData_ = convertAiMeshToVtkPolyData(aimesh);
        im.name_ = (aimesh->mName.length > 0) ? QString::fromUtf8(aimesh->mName.C_Str()) : QString("%1_mesh%2").arg(baseName).arg(mi);
        im.mesh_index_ = static_cast<int>(mi);
        loadedMeshes.append(im);

        MeshDef md;
        md.id_ = makeId();
        md.name_ = im.name_;
        md.vertex_count_ = (im.polyData_) ? im.polyData_->GetNumberOfPoints() : 0;
        md.primitive_count_ = (im.polyData_) ? im.polyData_->GetNumberOfCells() : 0;
        if (aimesh->mMaterialIndex >= 0 && aimesh->mMaterialIndex < static_cast<int>(doc.materials_.size())) {
            md.material_id_ = doc.materials_[aimesh->mMaterialIndex].id_;
            meshIndexToMaterialId[mi] = md.material_id_;
        }
        // bones
        if (aimesh->mNumBones > 0) {
            md.skinned_ = true;
            for (unsigned int b = 0; b < aimesh->mNumBones; ++b) {
                md.bone_names_.append(QString::fromUtf8(aimesh->mBones[b]->mName.C_Str()));
            }
        }
        doc.meshes_.append(md);
        meshIndexToId[mi] = md.id_;
    }

    // process nodes (scene hierarchy), create NodeDef for each node that contains a mesh
    std::function<void(const aiNode*, const QString& parentId)> processNode;
    processNode = [&](const aiNode* node, const QString& parentId)
    {
        // create an id
        QString nodeId = makeId();
        // if node contains meshes, create node entries for each mesh instance (separate Node per mesh)
        if (node->mNumMeshes > 0) {
            for (unsigned int i = 0; i < node->mNumMeshes; ++i) {
                int meshIndex = node->mMeshes[i];
                NodeDef nd;
                nd.id_ = makeId();
                nd.name_ = (node->mName.length > 0) ? QString::fromUtf8(node->mName.C_Str()) : QString("%1_node").arg(baseName);
                nd.parent_id_ = parentId;
                // decompose transform
                aiVector3D pos, scale;
                aiQuaternion rot;
                node->mTransformation.Decompose(scale, rot, pos);
                nd.translation_ = QVector3D(pos.x, pos.y, pos.z);
                nd.rotation_ = QVector4D(rot.x, rot.y, rot.z, rot.w);
                nd.scale_ = QVector3D(scale.x, scale.y, scale.z);
                // link mesh id
                if (meshIndexToId.contains(meshIndex)) nd.mesh_id_ = meshIndexToId.value(meshIndex);
                // assign material override if present
                if (meshIndexToMaterialId.contains(meshIndex)) nd.material_override_id_ = meshIndexToMaterialId.value(meshIndex);
                nd.type_ = "Mesh";
                doc.nodes_.append(nd);
            }
        } else {
            // empty/group node: create a group node so scene graph can be reconstructed
            NodeDef nd;
            nd.id_ = makeId();
            nd.name_ = (node->mName.length > 0) ? QString::fromUtf8(node->mName.C_Str()) : QString("%1_group").arg(baseName);
            nd.parent_id_ = parentId;
            aiVector3D pos, scale;
            aiQuaternion rot;
            node->mTransformation.Decompose(scale, rot, pos);
            nd.translation_ = QVector3D(pos.x, pos.y, pos.z);
            nd.rotation_ = QVector4D(rot.x, rot.y, rot.z, rot.w);
            nd.scale_ = QVector3D(scale.x, scale.y, scale.z);
            nd.type_ = "Node";
            doc.nodes_.append(nd);
            // set new parentId to this node for children
            // NOTE: for group nodes we set parentId so children attach under this group
            // replace parentId variable in lambda scope:
            // but we cannot reassign parentId (const), so pass this nd.id_ to children
            // we do that by calling children with nd.id_
            for (unsigned int c = 0; c < node->mNumChildren; ++c) {
                processNode(node->mChildren[c], nd.id_);
            }
            return;
        }

        // children
        for (unsigned int c = 0; c < node->mNumChildren; ++c) {
            processNode(node->mChildren[c], parentId);
        }
    };

    // start processing root node
    processNode(scene->mRootNode, QString()); // root has empty parent id

    // textures: use existing helper to extract textures from materials & map to files
    ImporterHelper helper;
    // build texture tasks like before: iterate materials & request base/normal/orm/emissive
    QVector<TextureImportTask> textureTasks;
    for (unsigned int m = 0; m < scene->mNumMaterials; ++m) {
        const aiMaterial* aimat = scene->mMaterials[m];
        auto createTextureTask = [&](aiTextureType type, const QString &texTypeName) {
            if (aimat->GetTextureCount(type) > 0) {
                // you may want to pass material index & dest folder
                textureTasks.append({ aimat, type, externalFilePath, QString(), scene, (int)m });
            }
        };
        createTextureTask(aiTextureType_BASE_COLOR, "albedo");
        createTextureTask(aiTextureType_DIFFUSE, "albedo");
        createTextureTask(aiTextureType_UNKNOWN, "albedo");
        createTextureTask(aiTextureType_NORMALS, "normal");
        createTextureTask(aiTextureType_EMISSIVE, "emissive");
        createTextureTask(aiTextureType_GLTF_METALLIC_ROUGHNESS, "orm");
    }

    QVector<TextureMapResult> textureResults = helper.processTextures(textureTasks, assetOutputFolder);
    finalResult.texture_results_ = textureResults;

    // convert textureResults -> TextureDef and hook them into materials by GUID references
    for (const TextureMapResult &tr : textureResults) {
        TextureDef td;
        td.id_ = tr.guid_;
        td.path_ = tr.file_path_;
        // map ai type to our type string (use same mapping as before)
        switch (tr.texture_type_) {
        case aiTextureType_DIFFUSE:
        case aiTextureType_BASE_COLOR:
        case aiTextureType_UNKNOWN:
            td.type_ = "albedo"; break;
        case aiTextureType_NORMALS:
            td.type_ = "normal"; break;
        case aiTextureType_GLTF_METALLIC_ROUGHNESS:
            td.type_ = "orm"; break;
        case aiTextureType_EMISSIVE:
            td.type_ = "emissive"; break;
        default:
            td.type_ = "unknown"; break;
        }
        doc.textures_.append(td);

        // assign texture id to corresponding material(s) using mesh_index info
        if (tr.mesh_index_ >= 0 && tr.mesh_index_ < doc.meshes_.size()) {
            QString matId = doc.meshes_[tr.mesh_index_].material_id_;
            if (!matId.isEmpty()) {
                for (MaterialDef &md : doc.materials_) {
                    if (md.id_ == matId) {
                        if (td.type_ == "albedo") md.base_color_texture_ = td.id_;
                        else if (td.type_ == "normal") md.normal_texture_ = td.id_;
                        else if (td.type_ == "orm") md.metallic_texture_ = td.id_;
                        else if (td.type_ == "emissive") md.emissive_texture_ = td.id_;
                        break;
                    }
                }
            }
        }
    }

    // === 不再把 metadata 写到文件系统，而是把 JSON 放入 finalResult.json_ ===
    // qWarning/commented out: if (!ModelDocumentSerializer::saveToFile(doc, metaPath)) { ... }
    // 将 ModelDocument 序列化为 QJsonObject 并放入 finalResult.json_
    QJsonObject serialized = ModelDocumentSerializer::toJson(doc);
    //finalResult.doc_ = doc;
    finalResult.json_ = serialized;

    // set result meshes for preview/backwards compat
    finalResult.meshes_.clear();
    for (const ImportedMesh &im : loadedMeshes) finalResult.meshes_.append(im);

    // // Optionally fill raw_json_ for backwards compatibility (small summary)
    // QJsonObject root;
    // root["id"] = doc.id_;
    // root["name"] = doc.name_;
    // root["model_file"] = doc.source_file_;
    // finalResult.json_ = root;

    return finalResult;
}

// image helper and convertAiMeshToVtkPolyData can be kept from your previous implementation
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
            tcoords->SetTuple2(i, uv.x, 1.0f - uv.y);
        }
        poly->GetPointData()->SetTCoords(tcoords);
    }

    vtkSmartPointer<vtkCellArray> polys = vtkSmartPointer<vtkCellArray>::New();
    for (unsigned int fi = 0; fi < mesh->mNumFaces; ++fi) {
        const aiFace& face = mesh->mFaces[fi];
        if (face.mNumIndices < 3) continue;
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
