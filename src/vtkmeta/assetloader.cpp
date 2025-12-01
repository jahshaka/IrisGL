#include "assetloader.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include <vtkPolyData.h>
#include <vtkPoints.h>
#include <vtkCellArray.h>
#include <vtkTriangle.h>
#include <vtkFloatArray.h>
#include <vtkPointData.h>
#include <vtkPolyDataMapper.h>
#include <vtkActor.h>
#include <vtkTexture.h>
#include <vtkImageReader2Factory.h>
#include <vtkImageReader2.h>
#include <vtkTransform.h>
#include <vtkMatrix4x4.h>
#include <vtkProperty.h>
#include <vtkNew.h>

#include <QFileInfo>
#include <QDir>
#include <QQuaternion>
#include <QMatrix4x4>
#include <QDebug>

#include "assetdatatypes.h" // ModelDocument, NodeDef, MaterialDef, TextureDef
#include "modeldocumentserializer.h"

namespace vtkmeta {

AssetLoader::AssetLoader() = default;
AssetLoader::~AssetLoader() = default;

void AssetLoader::clearTextureCache()
{
    textureCache_.clear();
}

// Reuse your previous convertAiMeshToVtkPolyData implementation (kept similar to original)
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

QHash<int, vtkSmartPointer<vtkPolyData>> AssetLoader::loadAllMeshesFromScene(const aiScene* scene) const
{
    QHash<int, vtkSmartPointer<vtkPolyData>> meshMap;
    if (!scene) return meshMap;

    std::function<void(const aiNode*, const aiMatrix4x4&)> processNode;
    processNode = [&](const aiNode* node, const aiMatrix4x4& parentTransform)
    {
        aiMatrix4x4 currentTransform = parentTransform * node->mTransformation;

        for (unsigned int i = 0; i < node->mNumMeshes; ++i) {
            unsigned int meshIdx = node->mMeshes[i];
            const aiMesh* aimesh = scene->mMeshes[meshIdx];
            if (!aimesh) continue;
            vtkSmartPointer<vtkPolyData> poly = convertAiMeshToVtkPolyData(aimesh);
            if (poly) meshMap.insert((int)meshIdx, poly);
        }

        for (unsigned int c = 0; c < node->mNumChildren; ++c)
            processNode(node->mChildren[c], currentTransform);
    };

    processNode(scene->mRootNode, aiMatrix4x4());
    return meshMap;
}

vtkSmartPointer<vtkTexture> AssetLoader::loadTextureByFile(const QString &fullPath)
{
    if (fullPath.isEmpty()) return nullptr;
    if (textureCache_.contains(fullPath)) return textureCache_.value(fullPath);

    if (!QFileInfo::exists(fullPath)) {
        qWarning() << "AssetLoader: texture file missing:" << fullPath;
        return nullptr;
    }

    vtkSmartPointer<vtkImageReader2> reader = vtkSmartPointer<vtkImageReader2>::Take(
        vtkImageReader2Factory::CreateImageReader2(fullPath.toUtf8().constData())
        );
    if (!reader) {
        qWarning() << "AssetLoader: no image reader for" << fullPath;
        return nullptr;
    }
    reader->SetFileName(fullPath.toUtf8().constData());
    reader->Update();

    vtkSmartPointer<vtkTexture> tex = vtkSmartPointer<vtkTexture>::New();
    tex->SetInputConnection(reader->GetOutputPort());
    tex->InterpolateOn();
    tex->MipmapOn();
    tex->RepeatOn();
    tex->EdgeClampOn();
    tex->SetPremultipliedAlpha(true);
    textureCache_.insert(fullPath, tex);
    return tex;
}

// Helper: build vtkTransform from NodeDef (quaternion is x,y,z,w)
static vtkSmartPointer<vtkTransform> makeTransformFromNode(const NodeDef &n)
{
    vtkSmartPointer<vtkTransform> tr = vtkSmartPointer<vtkTransform>::New();

    // translation
    tr->Translate(n.translation_.x(), n.translation_.y(), n.translation_.z());

    // rotation quaternion to matrix
    double qx = n.rotation_.x();
    double qy = n.rotation_.y();
    double qz = n.rotation_.z();
    double qw = n.rotation_.w();

    // if quaternion is default (0,0,0,1) it's fine
    QQuaternion q((float)qw, (float)qx, (float)qy, (float)qz); // QQuaternion(w,x,y,z)
    QMatrix4x4 qm;
    qm.setToIdentity();
    qm.rotate(q);

    vtkSmartPointer<vtkMatrix4x4> vm = vtkSmartPointer<vtkMatrix4x4>::New();
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            vm->SetElement(r, c, qm(r,c));

    vtkSmartPointer<vtkTransform> rtr = vtkSmartPointer<vtkTransform>::New();
    rtr->SetMatrix(vm);
    tr->Concatenate(rtr->GetMatrix());

    // scale last
    tr->Scale(n.scale_.x(), n.scale_.y(), n.scale_.z());

    return tr;
}

SceneLoadResult AssetLoader::loadModelFromDocument(const QString &modelDir,
                                                   const QJsonDocument &d, vtkRenderer *renderer)
{
    SceneLoadResult result;
    clearTextureCache();

    ModelDocument doc;
    ModelDocumentSerializer::loadFromJson(doc, d);

    QString modelPath = modelDir;

    // // determine model path
    // if (doc.source_file_.isEmpty()) {
    //     result.errors.append("ModelDocument.source_file_ is empty");
    //     return result;
    // }
    // QString modelPath = QDir(modelDir).filePath(doc.source_file_);
    // if (!QFileInfo::exists(modelPath)) {
    //     result.errors.append(QString("Model file missing: %1").arg(modelPath));
    //     return result;
    // }

    // Load with Assimp
    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(modelPath.toStdString(),
                                             aiProcess_Triangulate |
                                                 aiProcess_GenSmoothNormals |
                                                 aiProcess_JoinIdenticalVertices |
                                                 aiProcess_RemoveRedundantMaterials |
                                                 aiProcess_GenUVCoords |
                                                 aiProcess_OptimizeMeshes |
                                                 aiProcess_FlipUVs);
    if (!scene) {
        result.errors.append(QString("Assimp failed: %1").arg(importer.GetErrorString()));
        return result;
    }

    // Build polydata map
    QHash<int, vtkSmartPointer<vtkPolyData>> meshPolyDataMap = loadAllMeshesFromScene(scene);

    // Build meshId -> vtkPolyDataMapper
    for (int mi = 0; mi < doc.meshes_.size(); ++mi) {
        const MeshDef &md = doc.meshes_.at(mi);
        // The importer guaranteed mesh order matches original model's mesh indices
        // We assume mesh index == mi here. If not, importer should store mapping.
        vtkSmartPointer<vtkPolyData> poly;
        if (meshPolyDataMap.contains(mi)) poly = meshPolyDataMap.value(mi);
        else {
            // try to lookup by mesh name fallback (expensive)
            for (auto it = meshPolyDataMap.begin(); it != meshPolyDataMap.end(); ++it) {
                // no name stored here; skip
            }
        }

        if (poly) {
            vtkSmartPointer<vtkPolyDataMapper> mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
            mapper->SetInputData(poly);
            mapper->ScalarVisibilityOff();
            result.meshMappers.insert(md.id_, mapper);
        } else {
            result.errors.append(QString("No polydata for mesh index %1 (id=%2)").arg(mi).arg(md.id_));
        }
    }

    // Build texture id -> absolute path map from doc.textures_
    QHash<QString, QString> textureIdToPath;
    for (const TextureDef &td : doc.textures_) {
        QString p = td.path_;
        if (p.isEmpty()) continue;
        QFileInfo tfi(p);
        if (!tfi.isAbsolute()) p = QDir(modelDir).filePath(p);
        textureIdToPath.insert(td.id_, p);
    }

    // Build materialId -> albedo path mapping for quick access
    for (const MaterialDef &mat : doc.materials_) {
        QString albedoPath;
        if (!mat.base_color_texture_.isEmpty()) {
            QString texId = mat.base_color_texture_;
            if (textureIdToPath.contains(texId)) albedoPath = textureIdToPath.value(texId);
            else {
                // maybe mat stored file name directly
                QString candidate = QDir(modelDir).filePath(mat.base_color_texture_);
                if (QFileInfo::exists(candidate)) albedoPath = candidate;
            }
        }
        if (!albedoPath.isEmpty()) result.materialToAlbedoPath.insert(mat.id_, albedoPath);
    }

    // Create actors for nodes that have mesh_id
    for (const NodeDef &node : doc.nodes_) {
        if (node.mesh_id_.isEmpty()) continue;
        if (!result.meshMappers.contains(node.mesh_id_)) {
            result.errors.append(QString("Missing mapper for mesh_id %1 referenced by node %2").arg(node.mesh_id_).arg(node.id_));
            continue;
        }

        vtkSmartPointer<vtkPolyDataMapper> mapper = result.meshMappers.value(node.mesh_id_);

        vtkSmartPointer<vtkActor> actor = vtkSmartPointer<vtkActor>::New();
        actor->SetMapper(mapper);

        // apply material if any
        QString matId;
        if (!node.material_override_id_.isEmpty()) matId = node.material_override_id_;
        else {
            // try find mesh definitions to get material_id
            int meshIdx = -1;
            for (int i = 0; i < doc.meshes_.size(); ++i) if (doc.meshes_[i].id_ == node.mesh_id_) { meshIdx = i; break; }
            if (meshIdx >= 0) matId = doc.meshes_.at(meshIdx).material_id_;
        }

        if (!matId.isEmpty()) {
            // find material
            int midx = -1;
            for (int i = 0; i < doc.materials_.size(); ++i) if (doc.materials_[i].id_ == matId) { midx = i; break; }
            if (midx >= 0) {
                const MaterialDef &mat = doc.materials_.at(midx);
                vtkProperty* prop = actor->GetProperty();
                prop->SetInterpolationToPBR();
                prop->SetColor(mat.base_color_.x(), mat.base_color_.y(), mat.base_color_.z());
                prop->SetOpacity(mat.opacity_);
                prop->SetMetallic(mat.metallic_);
                prop->SetRoughness(mat.roughness_);
                if (mat.double_sided_) prop->BackfaceCullingOff();
                else prop->BackfaceCullingOn();

                // load and set albedo texture if available
                if (result.materialToAlbedoPath.contains(mat.id_)) {
                    QString full = result.materialToAlbedoPath.value(mat.id_);
                    vtkSmartPointer<vtkTexture> tex = loadTextureByFile(full);
                    if (tex) {
                        tex->SetUseSRGBColorSpace(true);
                        prop->SetBaseColorTexture(tex);
                    } else {
                        result.errors.append(QString("Failed to load albedo texture: %1").arg(full));
                    }
                }

                // Normal and ORM textures: find by id in mat.normal_texture_ / metallic_roughness_texture etc.
                // If present, resolve via textureIdToPath and set prop->SetNormalTexture / SetORMTexture
                if (!mat.normal_texture_.isEmpty()) {
                    QString tpath = textureIdToPath.value(mat.normal_texture_, QString());
                    if (tpath.isEmpty()) {
                        QString candidate = QDir(modelDir).filePath(mat.normal_texture_);
                        if (QFileInfo::exists(candidate)) tpath = candidate;
                    }
                    if (!tpath.isEmpty()) {
                        vtkSmartPointer<vtkTexture> nt = loadTextureByFile(tpath);
                        if (nt) {
                            nt->SetUseSRGBColorSpace(false);
                            prop->SetNormalTexture(nt);
                        } else {
                            result.errors.append(QString("Failed to load normal texture: %1").arg(tpath));
                        }
                    }
                }

                if (!mat.metallic_texture_.isEmpty()) {
                    QString tpath = textureIdToPath.value(mat.metallic_texture_, QString());
                    if (tpath.isEmpty()) {
                        QString candidate = QDir(modelDir).filePath(mat.metallic_texture_);
                        if (QFileInfo::exists(candidate)) tpath = candidate;
                    }

                    if (!tpath.isEmpty()) {
                        vtkSmartPointer<vtkTexture> orm = loadTextureByFile(tpath);
                        if (orm) {
                            orm->SetUseSRGBColorSpace(false);
                            prop->SetORMTexture(orm);
                        } else {
                            result.errors.append(QString("Failed to load ORM texture: %1").arg(tpath));
                        }
                    }
                }
            }
        }

        // apply node transform
        vtkSmartPointer<vtkTransform> tr = makeTransformFromNode(node);
        actor->SetUserTransform(tr);

        // add actor to renderer
        if (renderer) renderer->AddActor(actor);

        // record node
        LoadedNode ln;
        ln.id = node.id_;
        ln.name = node.name_;
        ln.mesh_id = node.mesh_id_;
        ln.material_id = node.material_override_id_.isEmpty() ? QString() : node.material_override_id_;
        ln.actor = actor;
        result.nodes.append(ln);
    }

    return result;
}

} // namespace vtkmeta
