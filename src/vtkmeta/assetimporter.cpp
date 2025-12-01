// Updated assetimporter.cpp (fix): ensure group nodes are added as children of their parent
// - Adds parent->children_ids_ update when creating a group node so loader recursion visits group nodes
// - Keeps other behavior (store original_index_, create mesh child nodes, textures -> material mapping)
// - No metadata file write; finalResult.json_ contains serialized ModelDocument

#include "assetimporter.h"
#include "ModelDocumentSerializer.h"
#include "importerhelper.h"

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
    doc.source_file_ = srcInfo.fileName(); // will be replaced with copied filename below

    // // prepare output dir for this asset
    // QDir rootDir(assetOutputFolder);
    // rootDir.mkpath(".");
    // QString modelDirPath = QDir(assetOutputFolder).filePath(doc.id_);
    // QDir modelDir(modelDirPath);
    // modelDir.mkpath(".");

    // // copy model file
    // QString destModelFileName = QString("model.%1").arg(srcInfo.suffix());
    // QString destModelFilePath = modelDir.filePath(destModelFileName);
    // if (!QFile::copy(externalFilePath, destModelFilePath)) {
    //     QFile::remove(destModelFilePath);
    //     if (!QFile::copy(externalFilePath, destModelFilePath)) {
    //         qWarning() << "AssetImporter: failed to copy model file to" << destModelFilePath;
    //         return finalResult;
    //     }
    // }
    // doc.source_file_ = destModelFileName;

    // maps
    QMap<int, QString> meshIndexToId;
    QMap<int, QString> meshIndexToMaterialId;

    // create MaterialDef entries
    for (unsigned int m = 0; m < scene->mNumMaterials; ++m) {
        const aiMaterial* aimat = scene->mMaterials[m];
        MaterialDef mat;
        mat.id_ = makeId();
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

        doc.materials_.append(mat);
    }

    // process meshes and assign original_index_
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
        md.original_index_ = static_cast<int>(mi); // store original index
        if (aimesh->mMaterialIndex >= 0 && aimesh->mMaterialIndex < static_cast<int>(doc.materials_.size())) {
            md.material_id_ = doc.materials_[aimesh->mMaterialIndex].id_;
            meshIndexToMaterialId[mi] = md.material_id_;
        }
        if (aimesh->mNumBones > 0) {
            md.skinned_ = true;
            for (unsigned int b = 0; b < aimesh->mNumBones; ++b) {
                md.bone_names_.append(QString::fromUtf8(aimesh->mBones[b]->mName.C_Str()));
            }
        }
        doc.meshes_.append(md);
        meshIndexToId[mi] = md.id_;
    }

    // process nodes: create a group node per aiNode (local transform), then create child mesh nodes that reference mesh ids
    std::function<void(const aiNode*, const QString&)> processNode;
    processNode = [&](const aiNode* node, const QString& parentId)
    {
        // create group node for this aiNode (always create to preserve hierarchy)
        NodeDef groupNode;
        groupNode.id_ = makeId();
        groupNode.name_ = (node->mName.length > 0) ? QString::fromUtf8(node->mName.C_Str()) : QString("%1_group").arg(baseName);
        groupNode.parent_id_ = parentId;
        aiVector3D pos, scale;
        aiQuaternion rot;
        // IMPORTANT: decompose node->mTransformation (local), not parent * node transform
        node->mTransformation.Decompose(scale, rot, pos);
        groupNode.translation_ = QVector3D(pos.x, pos.y, pos.z);
        groupNode.rotation_ = QVector4D(rot.x, rot.y, rot.z, rot.w);
        groupNode.scale_ = QVector3D(scale.x, scale.y, scale.z);
        groupNode.type_ = "Node";

        // append group node
        doc.nodes_.append(groupNode);
        QString myGroupId = groupNode.id_;

        // IMPORTANT FIX: register this group as a child of its parent (so loader recursion visits it)
        if (!parentId.isEmpty()) {
            // find parent node in doc and append this group's id to its children_ids_
            // iterate doc.nodes_ in reverse (likely parent was appended earlier) for slight optimization
            for (int idx = doc.nodes_.size() - 1; idx >= 0; --idx) {
                if (doc.nodes_[idx].id_ == parentId) {
                    doc.nodes_[idx].children_ids_.append(myGroupId);
                    break;
                }
            }
        }

        // for each mesh in this aiNode, create a mesh node child that references the mesh id
        for (unsigned int i = 0; i < node->mNumMeshes; ++i) {
            int meshIndex = node->mMeshes[i];
            NodeDef meshNode;
            meshNode.id_ = makeId();
            meshNode.name_ = (node->mName.length > 0) ? QString::fromUtf8(node->mName.C_Str()) : QString("%1_meshnode").arg(baseName);
            meshNode.parent_id_ = myGroupId;
            // mesh instance uses identity local transform (the group's transform is the true local for this instance)
            meshNode.translation_ = QVector3D(0,0,0);
            meshNode.rotation_ = QVector4D(0,0,0,1);
            meshNode.scale_ = QVector3D(1,1,1);
            meshNode.type_ = "Mesh";
            if (meshIndexToId.contains(meshIndex)) meshNode.mesh_id_ = meshIndexToId.value(meshIndex);
            if (meshIndexToMaterialId.contains(meshIndex)) meshNode.material_override_id_ = meshIndexToMaterialId.value(meshIndex);

            doc.nodes_.append(meshNode);

            // append child id to group node in doc (we appended groupNode earlier - find and push)
            for (NodeDef &ref : doc.nodes_) {
                if (ref.id_ == myGroupId) {
                    ref.children_ids_.append(meshNode.id_);
                    break;
                }
            }
        }

        // recurse children with parent = myGroupId
        for (unsigned int c = 0; c < node->mNumChildren; ++c) {
            processNode(node->mChildren[c], myGroupId);
        }
    };

    processNode(scene->mRootNode, QString()); // root parent empty

    // textures - use helper to extract texture files (if embedded or external) into modelDirPath
    ImporterHelper helper;
    QVector<TextureImportTask> textureTasks;
    for (unsigned int m = 0; m < scene->mNumMaterials; ++m) {
        const aiMaterial* aimat = scene->mMaterials[m];
        auto createTextureTask = [&](aiTextureType type) {
            if (aimat->GetTextureCount(type) > 0) {
                textureTasks.append({aimat, type, externalFilePath, QString(), scene, (int)m});
            }
        };
        createTextureTask(aiTextureType_BASE_COLOR);
        createTextureTask(aiTextureType_DIFFUSE);
        createTextureTask(aiTextureType_UNKNOWN);
        createTextureTask(aiTextureType_NORMALS);
        createTextureTask(aiTextureType_GLTF_METALLIC_ROUGHNESS);
        createTextureTask(aiTextureType_EMISSIVE);
    }

    QVector<TextureMapResult> textureResults = helper.processTextures(textureTasks, assetOutputFolder);
    finalResult.texture_results_ = textureResults;

    // convert textureResults -> TextureDef and attach to materials via mesh_index mapping
    for (const TextureMapResult &tr : textureResults) {
        TextureDef td;
        td.id_ = tr.guid_;
        td.path_ = tr.file_path_;
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

        // attach to material via mesh_index -> find mesh -> material id
        if (tr.mesh_index_ >= 0 && tr.mesh_index_ < doc.meshes_.size()) {
            MaterialDef &md = doc.materials_[tr.mesh_index_];

            if (td.type_ == "albedo") md.base_color_texture_ = td.id_;
            else if (td.type_ == "normal") md.normal_texture_ = td.id_;
            else if (td.type_ == "orm") {
                // importer found a combined ORM map — store it into both metallic and roughness fields
                md.metallic_texture_ = td.id_;
                md.roughness_texture_ = td.id_;
            }
            else if (td.type_ == "emissive") md.emissive_texture_ = td.id_;
        }
    }

    // finalResult: keep doc_ and JSON serialized for UI/DB (no file write)
    //finalResult.doc_ = doc;
    finalResult.json_ = ModelDocumentSerializer::toJson(doc);

    // fill meshes_ for preview/backwards compatibility
    finalResult.meshes_.clear();
    for (const ImportedMesh &im : loadedMeshes) finalResult.meshes_.append(im);

    // short raw summary
    // QJsonObject root;
    // root["id"] = doc.id_;
    // root["name"] = doc.name_;
    // root["model_file"] = doc.source_file_;
    // finalResult.raw_json_ = root;

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

// convertAiMeshToVtkPolyData kept unchanged from your prior working implementation
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
