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
#include <vtkSmartPointer.h>

#ifdef VTK_MODULE_ENABLE_VTK_FiltersGeneral
#include <vtkPolyDataTangents.h>
#endif

#include <QFileInfo>
#include <QDir>
#include <QQuaternion>
#include <QMatrix4x4>
#include <QImage>
#include <QUuid>
#include <QDebug>

#include "assetdatatypes.h"
#include "modeldocumentserializer.h"

namespace vtkmeta {

// AssetLoader::AssetLoader() = default;
// AssetLoader::~AssetLoader() = default;

void AssetLoader::clearTextureCache()
{
    textureCache_.clear();
}

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


static vtkSmartPointer<vtkTexture> CreateVTKTextureFromQImage(const QImage &qimg, bool srgb)
{
    if (qimg.isNull()) return nullptr;
    QImage imgRGBA = qimg.convertToFormat(QImage::Format_RGBA8888);
    int w = imgRGBA.width();
    int h = imgRGBA.height();
    const uchar* srcBits = imgRGBA.constBits();
    int bytesPerLine = imgRGBA.bytesPerLine();

    vtkSmartPointer<vtkImageData> imageData = vtkSmartPointer<vtkImageData>::New();
    imageData->SetDimensions(w, h, 1);
    imageData->AllocateScalars(VTK_UNSIGNED_CHAR, 4);
    unsigned char* dest = static_cast<unsigned char*>(imageData->GetScalarPointer(0, 0, 0));

    // copy flipped Y (VTK image origin differences)
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
    tex->SetPremultipliedAlpha(true);
    tex->SetBlendingMode(vtkTexture::VTK_TEXTURE_BLENDING_MODE_REPLACE);
    return tex;
}

// Replacement for AssetLoader::loadTextureByFile
vtkSmartPointer<vtkTexture> AssetLoader::loadTextureByFile(const QString &fullPath)
{
    if (fullPath.isEmpty()) {
        qWarning() << "AssetLoader::loadTextureByFile: empty path";
        return nullptr;
    }

    // normalized path for cache key and checks
    QString cleanPath = QDir::cleanPath(QDir::fromNativeSeparators(fullPath));

    // cache
    if (textureCache_.contains(cleanPath)) {
        return textureCache_.value(cleanPath);
    }

    // existence check
    bool exists = QFileInfo::exists(cleanPath);
    if (!exists) {
        qWarning() << "AssetLoader: texture file missing:" << cleanPath;
        return nullptr;
    }

    // Try vtkImageReader2Factory first (preferred)
    vtkSmartPointer<vtkImageReader2> reader = vtkSmartPointer<vtkImageReader2>::Take(
        vtkImageReader2Factory::CreateImageReader2(cleanPath.toUtf8().constData())
        );

    if (reader) {
        reader->SetFileName(cleanPath.toUtf8().constData());
        // Some readers may throw on Update; guard it
        try {
            reader->Update();
        } catch (...) {
            qWarning() << "AssetLoader: vtk reader Update() threw for" << cleanPath;
            reader = nullptr;
        }
    } else {
        qDebug() << "AssetLoader: vtkImageReader2Factory could not create reader for" << cleanPath;
    }

    if (reader) {
        vtkSmartPointer<vtkTexture> tex = vtkSmartPointer<vtkTexture>::New();
        tex->SetInputConnection(reader->GetOutputPort());
        tex->InterpolateOn();
        tex->MipmapOn();
        tex->RepeatOn();
        tex->EdgeClampOn();
        tex->SetPremultipliedAlpha(true);
        textureCache_.insert(cleanPath, tex);
        return tex;
    }

    // Fallback: try QImage -> vtkImageData -> vtkTexture
    QImage qimg(cleanPath);
    if (!qimg.isNull()) {
        vtkSmartPointer<vtkTexture> tex = CreateVTKTextureFromQImage(qimg, true /* default sRGB for color textures; caller can change */);
        if (tex) {
            textureCache_.insert(cleanPath, tex);
            qDebug() << "AssetLoader: loaded texture via QImage fallback for" << cleanPath;
            return tex;
        }
    } else {
        qWarning() << "AssetLoader: QImage failed to load texture:" << cleanPath;
    }

    // all attempts failed
    qWarning() << "AssetLoader: failed to load texture by any method:" << cleanPath;
    return nullptr;
}
// Helper: combine separate metallic and roughness textures into one ORM image file (R=occlusion (255), G=roughness, B=metallic)
// Returns absolute path to generated image inside modelDir, or empty string on failure.
static QString combineMetallicRoughnessToORM(const QString &metallicPath, const QString &roughnessPath, const QString &modelDir)
{
    if (metallicPath.isEmpty() && roughnessPath.isEmpty()) return QString();

    QImage metImg;
    QImage roughImg;

    if (!metallicPath.isEmpty()) {
        metImg = QImage(metallicPath);
        if (metImg.isNull()) return QString();
        metImg = metImg.convertToFormat(QImage::Format_Grayscale8);
    }

    if (!roughnessPath.isNull() && !roughnessPath.isEmpty()) {
        roughImg = QImage(roughnessPath);
        if (roughImg.isNull()) return QString();
        roughImg = roughImg.convertToFormat(QImage::Format_Grayscale8);
    }

    // Choose output size = size of available images (prefer roughness if present)
    QSize outSize;
    if (!roughImg.isNull()) outSize = roughImg.size();
    else if (!metImg.isNull()) outSize = metImg.size();
    else return QString();

    // Resize inputs to outSize if necessary
    if (!metImg.isNull() && metImg.size() != outSize) metImg = metImg.scaled(outSize);
    if (!roughImg.isNull() && roughImg.size() != outSize) roughImg = roughImg.scaled(outSize);

    QImage out(outSize, QImage::Format_RGB888);
    out.fill(QColor(255,255,255)); // default occlusion=255

    for (int y = 0; y < outSize.height(); ++y) {
        const uchar *metLine = nullptr;
        const uchar *roughLine = nullptr;
        if (!metImg.isNull()) metLine = metImg.constScanLine(y);
        if (!roughImg.isNull()) roughLine = roughImg.constScanLine(y);

        uchar *outLine = reinterpret_cast<uchar*>(out.scanLine(y));
        for (int x = 0; x < outSize.width(); ++x) {
            uchar mval = 0;
            uchar rval = 255;
            if (!metImg.isNull()) mval = metLine[x];
            if (!roughImg.isNull()) rval = roughLine[x];

            // out: R=occlusion (set 255), G=roughness, B=metallic
            outLine[x * 3 + 0] = 255;
            outLine[x * 3 + 1] = rval;
            outLine[x * 3 + 2] = mval;
        }
    }

    QString genName = QString("__generated_orm_%1.png").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QString outPath = QDir(modelDir).filePath(genName);
    if (!out.save(outPath, "PNG")) {
        qWarning() << "Failed to write combined ORM to" << outPath;
        return QString();
    }
    return outPath;
}

// Helper: build vtkMatrix4x4 from NodeDef local transform (translation, quaternion, scale)
static vtkSmartPointer<vtkMatrix4x4> makeVtkMatrixFromNode(const NodeDef &n)
{
    vtkSmartPointer<vtkMatrix4x4> mat = vtkSmartPointer<vtkMatrix4x4>::New();
    mat->Identity();

    // rotation quaternion (x,y,z,w)
    double qx = n.rotation_.x();
    double qy = n.rotation_.y();
    double qz = n.rotation_.z();
    double qw = n.rotation_.w();

    double xx = qx*qx, yy = qy*qy, zz = qz*qz;
    double xy = qx*qy, xz = qx*qz, yz = qy*qz;
    double wx = qw*qx, wy = qw*qy, wz = qw*qz;

    double r00 = 1.0 - 2.0*(yy + zz);
    double r01 = 2.0*(xy - wz);
    double r02 = 2.0*(xz + wy);

    double r10 = 2.0*(xy + wz);
    double r11 = 1.0 - 2.0*(xx + zz);
    double r12 = 2.0*(yz - wx);

    double r20 = 2.0*(xz - wy);
    double r21 = 2.0*(yz + wx);
    double r22 = 1.0 - 2.0*(xx + yy);

    mat->SetElement(0,0, r00 * n.scale_.x());
    mat->SetElement(0,1, r01 * n.scale_.y());
    mat->SetElement(0,2, r02 * n.scale_.z());
    mat->SetElement(1,0, r10 * n.scale_.x());
    mat->SetElement(1,1, r11 * n.scale_.y());
    mat->SetElement(1,2, r12 * n.scale_.z());
    mat->SetElement(2,0, r20 * n.scale_.x());
    mat->SetElement(2,1, r21 * n.scale_.y());
    mat->SetElement(2,2, r22 * n.scale_.z());

    mat->SetElement(0,3, n.translation_.x());
    mat->SetElement(1,3, n.translation_.y());
    mat->SetElement(2,3, n.translation_.z());
    mat->SetElement(3,3, 1.0);

    return mat;
}

void debugLoadModelDiagnostics(const QString &modelDir, const vtkmeta::ModelDocument &doc, vtkRenderer *renderer) {
    qDebug() << "---- debugLoadModelDiagnostics ----";
    qDebug() << "modelDir:" << modelDir;
    qDebug() << "doc.source_file_:" << doc.source_file_;
    QString modelPath = QDir(modelDir).filePath(doc.source_file_);
    qDebug() << "resolved modelPath:" << modelPath << "exists:" << QFileInfo::exists(modelPath);

    // print doc summary
    qDebug() << "doc.nodes_.size()=" << doc.nodes_.size();
    for (int i=0;i<doc.nodes_.size();++i) {
        const auto &n = doc.nodes_[i];
        qDebug() << " node["<<i<<"] id="<<n.id_<<" name="<<n.name_<<" parent="<<n.parent_id_<<" mesh_id="<<n.mesh_id_;
    }
    qDebug() << "doc.meshes_.size()=" << doc.meshes_.size();
    for (int i=0;i<doc.meshes_.size();++i) {
        const auto &m = doc.meshes_[i];
        qDebug() << " mesh["<<i<<"] id="<<m.id_<<" name="<<m.name_<<" original_index="<<m.original_index_<<" material_id="<<m.material_id_;
    }
    qDebug() << "doc.materials_.size()="<<doc.materials_.size();
    for (int i=0;i<doc.materials_.size();++i) {
        const auto &mat = doc.materials_[i];
        qDebug() << " material["<<i<<"] id="<<mat.id_<<" name="<<mat.name_<<" base_color_tex="<<mat.base_color_texture_
                 <<"normal="<<mat.normal_texture_<<"metallic="<<mat.metallic_texture_<<"roughness="<<mat.roughness_texture_;
    }
    qDebug() << "doc.textures_.size()="<<doc.textures_.size();
    for (int i=0;i<doc.textures_.size();++i) {
        const auto &t = doc.textures_[i];
        qDebug() << " texture["<<i<<"] id="<<t.id_<<" path="<<t.path_<<" type="<<t.type_;
    }

    // // Now run loader but capture result
    // vtkmeta::AssetLoader loader;
    // vtkmeta::SceneLoadResult res = loader.loadModelFromDocument(modelDir, doc, renderer);


}

void inspectLoadedScene(const vtkmeta::SceneLoadResult &res)
{
    qDebug() << "=== inspectLoadedScene ===";
    qDebug() << "nodes count =" << res.nodes.size();
    for (int i = 0; i < res.nodes.size(); ++i) {
        const auto &ln = res.nodes[i];
        qDebug() << "node[" << i << "] id=" << ln.id << " name=" << ln.name << " mesh_id=" << ln.mesh_id;
        vtkActor *actor = ln.actor.GetPointer();
        if (!actor) { qDebug() << "  actor: null"; continue; }

        vtkProperty *prop = actor->GetProperty();
        qDebug() << "  actor pointer:" << actor;
        vtkPolyDataMapper *mapper = vtkPolyDataMapper::SafeDownCast(actor->GetMapper());
        if (!mapper) {
            qDebug() << "  mapper: null";
        } else {
            // Try to get the input polydata; mapper API differs slightly across versions
            vtkDataObject *input = nullptr;
#if VTK_MAJOR_VERSION >= 9
            input = mapper->GetInputDataObject(0, 0);
#else
            input = mapper->GetInput();
#endif
            if (input) {
                vtkPolyData *pd = vtkPolyData::SafeDownCast(input);
                if (pd) {
                    qDebug() << "  polydata points:" << (pd->GetNumberOfPoints());
                    auto tcoords = pd->GetPointData()->GetTCoords();
                    qDebug() << "  tcoords:" << (tcoords ? QString("present, tuples=%1").arg(tcoords->GetNumberOfTuples()) : "missing");
                } else {
                    qDebug() << "  mapper input not polydata";
                }
            } else {
                qDebug() << "  mapper input null";
            }
        }

        if (prop) {
            // Print PBR params
            double color[3]; prop->GetColor(color);
            qDebug() << "  color:" << color[0] << color[1] << color[2]
                     << " opacity:" << prop->GetOpacity()
                     << " metallic:" << prop->GetMetallic()
                     << " roughness:" << prop->GetRoughness();

            // actor-level texture (fallback in older VTK versions)
            vtkTexture *actorTex = actor->GetTexture();
            qDebug() << "  actor->GetTexture():" << actorTex;

            // Note: vtkProperty PBR texture getters may not exist in VTK 9.5.
            // We avoid calling prop->GetBaseColorTexture() etc. for compatibility.
            // If you used prop->SetBaseColorTexture() and your VTK exposes getters,
            // you can add conditional checks here guarded by compile-time macros.
        }
    }

    qDebug() << "materialToAlbedoPath keys:";
    for (auto it = res.materialToAlbedoPath.begin(); it != res.materialToAlbedoPath.end(); ++it) {
        qDebug() << " material=" << it.key() << " -> " << it.value() << " exists=" << QFileInfo::exists(it.value());
    }

    qDebug() << "meshMappers keys:";
    for (auto it = res.meshMappers.begin(); it != res.meshMappers.end(); ++it) {
        qDebug() << " meshId=" << it.key() << " mapperPtr=" << it.value().GetPointer();
    }

    qDebug() << "errors:";
    for (const auto &e : res.errors) qDebug() << "  " << e;
    qDebug() << "=== end inspect ===";
}


SceneLoadResult AssetLoader::loadModelFromDocument(const QString &modelDir, const QJsonDocument &d, vtkRenderer *renderer)
{
    SceneLoadResult result;
    clearTextureCache();

    QString modelPath = modelDir;
    if (!QFileInfo::exists(modelPath)) {
        result.errors.append(QString("Model file missing: %1").arg(modelPath));
        return result;
    }

    // Load scene via Assimp to get mesh geometry
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

    // build polydata map (assimp mesh index -> vtkPolyData)
    QHash<int, vtkSmartPointer<vtkPolyData>> meshPolyDataMap = loadAllMeshesFromScene(scene);
    ModelDocument doc;
    ModelDocumentSerializer::loadFromJson(doc, d);


//    debugLoadModelDiagnostics(modelPath, doc, renderer);

    // build mapping original_index -> meshId (from doc.meshes_)
    QHash<int, QString> originalIndexToMeshId;
    for (const MeshDef &md : doc.meshes_) {
        if (md.original_index_ >= 0) originalIndexToMeshId.insert(md.original_index_, md.id_);
    }

    // build meshId -> vtkPolyDataMapper (generate tangents if normal map is used)
    QHash<QString, vtkSmartPointer<vtkPolyDataMapper>> meshIdToMapper;
    for (auto it = meshPolyDataMap.begin(); it != meshPolyDataMap.end(); ++it) {
        int meshIndex = it.key();
        vtkSmartPointer<vtkPolyData> poly = it.value();
        QString meshId;
        if (originalIndexToMeshId.contains(meshIndex)) meshId = originalIndexToMeshId.value(meshIndex);
        else {
            if (meshIndex >= 0 && meshIndex < doc.meshes_.size()) meshId = doc.meshes_.at(meshIndex).id_;
        }
        if (meshId.isEmpty()) continue;

        // decide if we need tangents (if material references a normal map)
        bool needTangents = false;
        for (const MeshDef &md : doc.meshes_) {
            if (md.id_ == meshId) {
                if (!md.material_id_.isEmpty()) {
                    for (const MaterialDef &mat : doc.materials_) {
                        if (mat.id_ == md.material_id_) {
                            if (!mat.normal_texture_.isEmpty()) needTangents = true;
                            break;
                        }
                    }
                }
                break;
            }
        }

        vtkSmartPointer<vtkPolyData> polyForMapper = poly;
        if (needTangents) {
#ifdef VTK_MODULE_ENABLE_VTK_FiltersGeneral
            vtkNew<vtkPolyDataTangents> tangents;
            tangents->SetInputData(poly);
            tangents->Update();
            vtkSmartPointer<vtkPolyData> out = tangents->GetOutput();
            if (out) polyForMapper = out;
#else
            qDebug() << "Warning: vtkPolyDataTangents not available; normal maps may appear incorrect.";
#endif
        }

        vtkSmartPointer<vtkPolyDataMapper> mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
        mapper->SetInputData(polyForMapper);
        mapper->ScalarVisibilityOff();
        meshIdToMapper.insert(meshId, mapper);
    }

    // build texture id -> abs path map
    QHash<QString, QString> textureIdToPath;
    for (const TextureDef &td : doc.textures_) {
        QString p = td.path_;
        if (p.isEmpty()) continue;
        QFileInfo tfi(p);
        if (!tfi.isAbsolute()) p = QDir(modelDir).filePath(p);
        textureIdToPath.insert(td.id_, p);
    }

    // build node lookup & find roots
    QMap<QString, NodeDef> idToNode;
    QVector<QString> rootIds;
    for (const NodeDef &nd : doc.nodes_) idToNode.insert(nd.id_, nd);
    for (const NodeDef &nd : doc.nodes_) if (nd.parent_id_.isEmpty()) rootIds.append(nd.id_);

    // recursive apply: parentGlobal * local -> child global
    std::function<void(const QString&, const vtkSmartPointer<vtkMatrix4x4>&)> recurse;
    recurse = [&](const QString &nodeId, const vtkSmartPointer<vtkMatrix4x4> &parentGlobal) {
        if (!idToNode.contains(nodeId)) return;
        const NodeDef n = idToNode.value(nodeId);

        vtkSmartPointer<vtkMatrix4x4> local = makeVtkMatrixFromNode(n);
        vtkSmartPointer<vtkMatrix4x4> global = vtkSmartPointer<vtkMatrix4x4>::New();
        vtkMatrix4x4::Multiply4x4(parentGlobal, local, global);

        // if node references mesh, create actor
        if (!n.mesh_id_.isEmpty() && meshIdToMapper.contains(n.mesh_id_)) {
            vtkSmartPointer<vtkPolyDataMapper> mapper = meshIdToMapper.value(n.mesh_id_);
            vtkSmartPointer<vtkActor> actor = vtkSmartPointer<vtkActor>::New();
            actor->SetMapper(mapper);

            // determine material id
            QString materialId;
            if (!n.material_override_id_.isEmpty()) materialId = n.material_override_id_;
            else {
                for (const MeshDef &md : doc.meshes_) {
                    if (md.id_ == n.mesh_id_) { materialId = md.material_id_; break; }
                }
            }

            if (!materialId.isEmpty()) {
                // find material
                for (const MaterialDef &mat : doc.materials_) {
                    if (mat.id_ != materialId) continue;
                    vtkProperty *prop = actor->GetProperty();
                    prop->SetInterpolationToPBR();
                    prop->SetColor(mat.base_color_.x(), mat.base_color_.y(), mat.base_color_.z());
                    prop->SetOpacity(mat.opacity_);
                    prop->SetMetallic(mat.metallic_);
                    prop->SetRoughness(mat.roughness_);
                    if (mat.double_sided_) prop->BackfaceCullingOff();
                    else prop->BackfaceCullingOn();


                    // Bind albedo (sRGB)
                    if (!mat.base_color_texture_.isEmpty()) {
                        QString p = textureIdToPath.value(mat.base_color_texture_, QString());
                        if (p.isEmpty()) {
                            QString cand = QDir(modelDir).filePath(mat.base_color_texture_);
                            if (QFileInfo::exists(cand)) p = cand;
                        }
                        if (!p.isEmpty()) {
                            vtkSmartPointer<vtkTexture> t = loadTextureByFile(p);
                            if (t) { t->SetUseSRGBColorSpace(true); prop->SetBaseColorTexture(t); prop->SetColor(1.0,1.0,1.0); }
                            else result.errors.append(QString("Failed to load albedo: %1").arg(p));
                        }
                    }

                    // Normal texture (linear)
                    if (!mat.normal_texture_.isEmpty()) {
                        QString p = textureIdToPath.value(mat.normal_texture_, QString());
                        if (p.isEmpty()) {
                            QString cand = QDir(modelDir).filePath(mat.normal_texture_);
                            if (QFileInfo::exists(cand)) p = cand;
                        }
                        if (!p.isEmpty()) {
                            vtkSmartPointer<vtkTexture> nt = loadTextureByFile(p);
                            if (nt) { nt->SetUseSRGBColorSpace(false); prop->SetNormalTexture(nt); }
                            else result.errors.append(QString("Failed to load normal: %1").arg(p));
                        }
                    }

                    // ORM handling:
                    QString ormPath;
                    // 1) combined case: importer set both metallic_texture_ and roughness_texture_ to same id
                    if (!mat.metallic_texture_.isEmpty() && mat.metallic_texture_ == mat.roughness_texture_) {
                        ormPath = textureIdToPath.value(mat.metallic_texture_, QString());
                        if (ormPath.isEmpty()) {
                            QString cand = QDir(modelDir).filePath(mat.metallic_texture_);
                            if (QFileInfo::exists(cand)) ormPath = cand;
                        }
                    }

                    // 2) if no combined, try to combine separate metallic & roughness files
                    if (ormPath.isEmpty()) {
                        QString metPath = QString();
                        QString roughPath = QString();
                        if (!mat.metallic_texture_.isEmpty()) metPath = textureIdToPath.value(mat.metallic_texture_, QString());
                        if (!mat.roughness_texture_.isEmpty()) roughPath = textureIdToPath.value(mat.roughness_texture_, QString());
                        if (metPath.isEmpty() && !mat.metallic_texture_.isEmpty()) {
                            QString cand = QDir(modelDir).filePath(mat.metallic_texture_);
                            if (QFileInfo::exists(cand)) metPath = cand;
                        }
                        if (roughPath.isEmpty() && !mat.roughness_texture_.isEmpty()) {
                            QString cand = QDir(modelDir).filePath(mat.roughness_texture_);
                            if (QFileInfo::exists(cand)) roughPath = cand;
                        }

                        if (!metPath.isEmpty() && !roughPath.isEmpty()) {
                            QString combined = combineMetallicRoughnessToORM(metPath, roughPath, modelDir);
                            if (!combined.isEmpty()) ormPath = combined;
                        } else if (!metPath.isEmpty() || !roughPath.isEmpty()) {
                            result.errors.append(QString("Only one of metallic/roughness textures present for material %1; cannot construct full ORM").arg(mat.name_));
                        }
                    }

                    if (!ormPath.isEmpty()) {
                        vtkSmartPointer<vtkTexture> orm = loadTextureByFile(ormPath);
                        if (orm) { orm->SetUseSRGBColorSpace(false); prop->SetORMTexture(orm); }
                        else result.errors.append(QString("Failed to load ORM texture: %1").arg(ormPath));
                    }

                    break; // material applied
                }
            }

            vtkSmartPointer<vtkTransform> tr = vtkSmartPointer<vtkTransform>::New();
            tr->SetMatrix(global);
            actor->SetUserTransform(tr);

            if (renderer) {
                renderer->AddActor(actor);
            }

            LoadedNode ln;
            ln.id = n.id_;
            ln.name = n.name_;
            ln.mesh_id = n.mesh_id_;
            ln.material_id = n.material_override_id_;
            ln.actor = actor;
            result.nodes.append(ln);
        }

        // recurse children
        for (const QString &childId : n.children_ids_) recurse(childId, global);
    };

    // start recursion from roots
    vtkSmartPointer<vtkMatrix4x4> identity = vtkSmartPointer<vtkMatrix4x4>::New();
    identity->Identity();
    for (const QString &rid : rootIds) recurse(rid, identity);

    // save mappers into result for UI access
    for (auto it = meshIdToMapper.begin(); it != meshIdToMapper.end(); ++it) result.meshMappers.insert(it.key(), it.value());
    // also save material->albedo path
    for (const MaterialDef &m : doc.materials_) {
        if (!m.base_color_texture_.isEmpty()) {
            QString p = textureIdToPath.value(m.base_color_texture_, QString());
            if (p.isEmpty()) {
                QString cand = QDir(modelDir).filePath(m.base_color_texture_);
                if (QFileInfo::exists(cand)) p = cand;
            }
            if (!p.isEmpty()) result.materialToAlbedoPath.insert(m.id_, p);
        }
    }


    // qDebug() << "SceneLoadResult nodes count=" << result.nodes.size();
    // for (int i=0;i<result.nodes.size();++i) {
    //     qDebug() << " loaded node["<<i<<"] id="<<result.nodes[i].id<<" name="<<result.nodes[i].name<<" mesh_id="<<result.nodes[i].mesh_id;
    // }
    // qDebug() << "meshMappers keys:";
    // for (auto it=result.meshMappers.begin(); it!=result.meshMappers.end(); ++it) {
    //     qDebug() << "  mapper meshId="<<it.key();
    // }
    // qDebug() << "materialToAlbedoPath keys:";
    // for (auto it = result.materialToAlbedoPath.begin(); it != result.materialToAlbedoPath.end(); ++it) {
    //     qDebug() << " material="<<it.key()<<" path="<<it.value();
    // }
    // qDebug() << "errors:";
    // for (const QString &e : result.errors) qDebug() << "  " << e;
    // qDebug() << "---- end debug ----";


    inspectLoadedScene(result);

    return result;
}

} // namespace vtkmeta
