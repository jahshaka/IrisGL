// assimpmodelloader.cpp
#include "assetloader.h"

#include <QImage>
#include <QFileInfo>
#include <QDir>
#include <QUuid>
#include <QJsonArray>

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
#include <vtkPNGReader.h>

#include <QDebug>

namespace vtkmeta {

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

void AssetLoader::loadNodeRecursive(const QJsonObject &obj,
                                    LoadedMeshNode &node,
                                    const aiScene *scene,
                                    const QString &modelPath,
                                    const QString &assetFolder,
                                    vtkRenderer *renderer)
{
    node.name = obj["name"].toString();
    node.guid = obj["guid"].toString();
    node.modelFile = obj["modelFile"].toString();
    node.meshIndex = obj["meshIndex"].toInt(-1);
    node.localTransform = readTransform(obj["transform"].toObject());

    // Load material
    if (obj.contains("material"))
        node.material = readMaterial(obj["material"].toObject(), assetFolder);

    // Load mesh & actor
    if (node.meshIndex >= 0)
    {
        vtkSmartPointer<vtkPolyData> poly = loadMeshPolyData(scene, node.meshIndex);
        if (poly)
        {
            node.actor = createActor(poly, node.material, assetFolder);

            vtkSmartPointer<vtkTransform> t = vtkSmartPointer<vtkTransform>::New();
            QMatrix4x4 m = node.localTransform;

            vtkSmartPointer<vtkMatrix4x4> vm = vtkSmartPointer<vtkMatrix4x4>::New();
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c)
                    vm->SetElement(r, c, m(r, c));
            t->SetMatrix(vm);

            node.actor->SetUserTransform(t);
            renderer->AddActor(node.actor);
        }
    }

    // children
    if (obj.contains("children") && obj["children"].isArray())
    {
        QJsonArray arr = obj["children"].toArray();
        for (auto c : arr)
        {
            LoadedMeshNode child;
            loadNodeRecursive(c.toObject(), child, scene,
                              modelPath, assetFolder, renderer);
            node.children.append(child);
        }
    }
}

vtkSmartPointer<vtkPolyData> AssetLoader::loadMeshPolyData(const aiScene *scene, int meshIndex)
{

    if (meshIndex < 0 || meshIndex >= (int)scene->mNumMeshes)
        return nullptr;

    const aiMesh *aimesh = scene->mMeshes[meshIndex];
    if (!aimesh || !aimesh->HasFaces() || !aimesh->HasPositions())
        return nullptr;

    vtkSmartPointer<vtkPolyData> poly = vtkSmartPointer<vtkPolyData>::New();
    vtkSmartPointer<vtkPoints> pts = vtkSmartPointer<vtkPoints>::New();
    vtkSmartPointer<vtkCellArray> cells = vtkSmartPointer<vtkCellArray>::New();

    // vertices
    for (unsigned int i = 0; i < aimesh->mNumVertices; ++i)
    {
        pts->InsertNextPoint(aimesh->mVertices[i].x,
                             aimesh->mVertices[i].y,
                             aimesh->mVertices[i].z);
    }

    // faces
    for (unsigned int i = 0; i < aimesh->mNumFaces; ++i)
    {
        const aiFace &face = aimesh->mFaces[i];
        if (face.mNumIndices != 3)
            continue;

        vtkIdType tri[3] =
            {
                (vtkIdType)face.mIndices[0],
                (vtkIdType)face.mIndices[1],
                (vtkIdType)face.mIndices[2]
            };
        cells->InsertNextCell(3, tri);
    }

    poly->SetPoints(pts);
    poly->SetPolys(cells);

    return poly;
}

vtkSmartPointer<vtkActor> AssetLoader::createActor(vtkPolyData *poly,
                                                   const LoadedMaterialInfo &mat,
                                                   const QString &assetFolder)
{

    vtkSmartPointer<vtkPolyDataMapper> mapper =
        vtkSmartPointer<vtkPolyDataMapper>::New();
    mapper->SetInputData(poly);

    vtkSmartPointer<vtkActor> actor = vtkSmartPointer<vtkActor>::New();
    actor->SetMapper(mapper);

    vtkProperty *p = actor->GetProperty();
    p->SetColor(mat.baseColor.x(), mat.baseColor.y(), mat.baseColor.z());
    p->SetOpacity(mat.opacity);

    auto loadTexture = [&](const LoadedTextureInfo &tinfo) -> vtkSmartPointer<vtkTexture>
    {
        if (tinfo.file.isEmpty())
            return vtkSmartPointer<vtkTexture>();

        QString full = QDir(assetFolder).filePath(tinfo.file);

        vtkSmartPointer<vtkPNGReader> reader = vtkSmartPointer<vtkPNGReader>::New();
        if (!reader->CanReadFile(full.toUtf8().constData()))
            return vtkSmartPointer<vtkTexture>();

        reader->SetFileName(full.toUtf8().constData());
        reader->Update();

        vtkSmartPointer<vtkTexture> tex = vtkSmartPointer<vtkTexture>::New();
        tex->SetInputConnection(reader->GetOutputPort());
        tex->InterpolateOn();

        return tex;
    };

    if (!mat.diffuse.file.isEmpty()) {
        vtkSmartPointer<vtkTexture> t = loadTexture(mat.diffuse);
        if (t) {
            actor->SetTexture(t.GetPointer());
        }
    }

    return actor;
}

QMatrix4x4 AssetLoader::readTransform(const QJsonObject &obj)
{
    QMatrix4x4 m;
    m.setToIdentity();

    if (obj.isEmpty())
        return m;

    QVector3D pos(0, 0, 0);
    QVector3D rot(0, 0, 0);
    QVector3D scale(1, 1, 1);

    auto readVec3 = [&](const QJsonValue &v, QVector3D &out)
    {
        if (!v.isArray()) return;
        QJsonArray a = v.toArray();
        if (a.size() != 3) return;
        out.setX(a[0].toDouble());
        out.setY(a[1].toDouble());
        out.setZ(a[2].toDouble());
    };

    readVec3(obj["pos"], pos);
    readVec3(obj["rot"], rot);
    readVec3(obj["scale"], scale);

    m.translate(pos);
    m.rotate(rot.x(), 1, 0, 0);
    m.rotate(rot.y(), 0, 1, 0);
    m.rotate(rot.z(), 0, 0, 1);
    m.scale(scale);

    return m;
}

LoadedMaterialInfo AssetLoader::readMaterial(const QJsonObject &matObj, const QString &assetFolder)
{
    LoadedMaterialInfo mat;

    mat.name = matObj["name"].toString();

    auto readVec3 = [&](const QJsonValue &v, QVector3D &out)
    {
        if (!v.isArray()) return;
        QJsonArray a = v.toArray();
        if (a.size() != 3) return;
        out.setX(a[0].toDouble());
        out.setY(a[1].toDouble());
        out.setZ(a[2].toDouble());
    };

    readVec3(matObj["baseColor"], mat.baseColor);
    mat.metallic  = matObj["metallic"].toDouble();
    mat.roughness = matObj["roughness"].toDouble();
    mat.opacity   = matObj["opacity"].toDouble();

    if (matObj.contains("textures"))
    {
        QJsonObject t = matObj["textures"].toObject();

        auto readTex = [&](const QString &key, LoadedTextureInfo &out)
        {
            if (!t.contains(key)) return;
            QJsonObject o = t[key].toObject();
            out.guid = o["guid"].toString();
            out.file = o["file"].toString();
            out.fullPath = QDir(assetFolder).filePath(out.file);
        };

        readTex("diffuse",  mat.diffuse);
        readTex("normal",   mat.normal);
        readTex("orm",      mat.orm);
        readTex("emissive", mat.emissive);
    }

    return mat;
}

QHash<int, vtkSmartPointer<vtkPolyData>> AssetLoader::loadAllMeshesFromFile(
    const QString& /*modelFilePath*/, const aiScene* scene) const
{
    QHash<int, vtkSmartPointer<vtkPolyData>> meshMap;
 //   QString baseName = QFileInfo(modelFilePath).baseName();


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


            vtkSmartPointer<vtkPolyData> poly = convertAiMeshToVtkPolyData(aimesh);
            if (poly) {
                //meshMap[meshIdx] = poly;
                meshMap.insert(meshIdx, poly);
            }
        }

        for (unsigned int c = 0; c < node->mNumChildren; ++c)
            processNode(node->mChildren[c], currentTransform);
    };

    processNode(scene->mRootNode, aiMatrix4x4());

    return meshMap;
}

SceneLoadResult AssetLoader::loadModelFromJson(const QString& filePath,
                                               const QJsonObject &obj,
                                               vtkRenderer *renderer)
{
    SceneLoadResult result;

    QFileInfo fi(filePath);
    QString assetFolder = fi.absolutePath();

    QJsonObject rootObj = obj;
    QJsonArray meshesArray = rootObj["nodes"].toArray();

    unsigned int flags = aiProcess_Triangulate | aiProcess_GenSmoothNormals | aiProcess_FlipUVs; // 简化 flags
    const aiScene* scene = importer_.ReadFile(filePath.toStdString(), flags);
    if (!scene) {
        qWarning() << "loadModelFromJson: Assimp failed to load model" << filePath << ":" << importer_.GetErrorString();
        return result;
    }

    textureCache_.clear();

    QHash<int, vtkSmartPointer<vtkPolyData>> meshPolyDataMap =
        loadAllMeshesFromFile(filePath, scene);

    for (auto meshVal : meshesArray) {
        QJsonObject meshObj = meshVal.toObject();
        LoadedMesh mesh;
        mesh.name = meshObj["name"].toString();

        int meshNameRef = meshObj["meshIndex"].toInt();

        if (meshPolyDataMap.contains(meshNameRef)) {
            mesh.polyData = meshPolyDataMap.value(meshNameRef);
        } else {
            qWarning() << "Loader: Mesh reference not found for:" << meshNameRef;
            continue;
        }

        if (meshObj.contains("transform")) {
            QJsonObject tObj = meshObj["transform"].toObject();
            if (tObj.contains("pos")) {
                QJsonArray a = tObj["pos"].toArray();
                if (a.size() >= 3)
                    mesh.position = QVector3D(a[0].toDouble(), a[1].toDouble(), a[2].toDouble());
            }
            if (tObj.contains("rot")) {
                QJsonArray a = tObj["rot"].toArray();
                if (a.size() == 4) {
                    // importer saved quaternion [x,y,z,w]
                    mesh.rotation = QVector4D(a[0].toDouble(), a[1].toDouble(), a[2].toDouble(), a[3].toDouble());
                } else if (a.size() == 3) {
                    // legacy euler degrees
                    // store as (x,y,z,0) to indicate euler
                    mesh.rotation = QVector4D(a[0].toDouble(), a[1].toDouble(), a[2].toDouble(), 0.0);
                }
            }
            if (tObj.contains("scale")) {
                QJsonArray a = tObj["scale"].toArray();
                if (a.size() >= 3)
                    mesh.scale = QVector3D(a[0].toDouble(1.0), a[1].toDouble(1.0), a[2].toDouble(1.0));
            }
        }

        //int meshIndex = mesh
        // create actor
        vtkSmartPointer<vtkActor> actor = vtkSmartPointer<vtkActor>::New();
        QJsonObject matObj = meshObj["material"].toObject();
        if (!matObj.isEmpty()) {
            QJsonArray baseColorArr = matObj["base_color"].toArray();

            if (baseColorArr.size() >= 3) {
                double r = baseColorArr[0].toDouble();
                double g = baseColorArr[1].toDouble();
                double b = baseColorArr[2].toDouble();

                vtkSmartPointer<vtkProperty> prop = actor->GetProperty();
                prop->SetDiffuseColor(r, g, b);

                float matOpacity = matObj["opacity"].toDouble(1.0);
                if (matOpacity >= 0.995f) {
                    actor->SetForceOpaque(true);
                    prop->SetOpacity(1.0);
                } else {
                    actor->SetForceOpaque(false);
                    prop->SetOpacity(matOpacity);
                    actor->SetForceTranslucent(true);
                }

                prop->SetMetallic(matObj["metallic"].toDouble(0.0));
                prop->SetRoughness(matObj["roughness"].toDouble(0.5));
            }

            if (matObj.contains("textures")) {
                QJsonObject texObj = matObj["textures"].toObject();
                if (texObj.contains("diffuse")) {
                    auto t = texObj["diffuse"].toObject();
                    mesh.materialInfo.diffuse.guid = t["guid"].toString();
                    mesh.materialInfo.diffuse.file = t["file"].toString();
                }
                if (texObj.contains("normal")) {
                    auto t = texObj["normal"].toObject();
                    mesh.materialInfo.normal.guid = t["guid"].toString();
                    mesh.materialInfo.normal.file = t["file"].toString();
                }
                if (texObj.contains("orm")) {
                    auto t = texObj["orm"].toObject();
                    mesh.materialInfo.orm.guid = t["guid"].toString();
                    mesh.materialInfo.orm.file = t["file"].toString();
                }
                if (texObj.contains("emissive")) {
                    auto t = texObj["emissive"].toObject();
                    mesh.materialInfo.emissive.guid = t["guid"].toString();
                    mesh.materialInfo.emissive.file = t["file"].toString();
                }
            }
        }

        vtkSmartPointer<vtkPolyDataMapper> mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
        if (mesh.polyData)
            mapper->SetInputData(mesh.polyData);
        actor->SetMapper(mapper);

        vtkSmartPointer<vtkTransform> trans = vtkSmartPointer<vtkTransform>::New();
        trans->Translate(mesh.position.x(), mesh.position.y(), mesh.position.z());

        if (mesh.rotation.w() != 0.0) {
            // quaternion case: x,y,z,w
            double qx = mesh.rotation.x();
            double qy = mesh.rotation.y();
            double qz = mesh.rotation.z();
            double qw = mesh.rotation.w();

            // convert to QQuaternion -> QMatrix4x4 -> vtkMatrix4x4
            QQuaternion q((float)qw, (float)qx, (float)qy, (float)qz); // QQuaternion(w,x,y,z)
            QMatrix4x4 qm;
            qm.setToIdentity();
            qm.rotate(q);

            // convert QMatrix4x4 to vtkMatrix4x4
            vtkSmartPointer<vtkMatrix4x4> vm = vtkSmartPointer<vtkMatrix4x4>::New();
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c)
                    vm->SetElement(r, c, qm(r,c));

            vtkSmartPointer<vtkTransform> rtrans = vtkSmartPointer<vtkTransform>::New();
            rtrans->SetMatrix(vm);
            trans->Concatenate(rtrans->GetMatrix());
        } else {
            // euler (deg assumed)
            trans->RotateX(mesh.rotation.x());
            trans->RotateY(mesh.rotation.y());
            trans->RotateZ(mesh.rotation.z());
        }

        // scale (apply last)
        trans->Scale(mesh.scale.x(), mesh.scale.y(), mesh.scale.z());

        actor->SetUserTransform(trans);

        auto prop = actor->GetProperty();
        prop->SetInterpolationToPBR();
        prop->BackfaceCullingOn();

        // diffuse / base color
        if (!mesh.materialInfo.diffuse.file.isEmpty()) {
            auto tex = loadTexture(mesh.materialInfo.diffuse, assetFolder);
            if (tex) {
                tex->SetUseSRGBColorSpace(true);
                prop->SetBaseColorTexture(tex);
                prop->SetColor(1.0, 1.0, 1.0);

            }
        }

        // normal
        if (!mesh.materialInfo.normal.file.isEmpty()) {
            auto normalTex = loadTexture(mesh.materialInfo.normal, assetFolder);
            if (normalTex) {
                normalTex->SetUseSRGBColorSpace(false);
                prop->SetNormalTexture(normalTex);
            }
        }

        // ORM
        if (!mesh.materialInfo.orm.file.isEmpty()) {
            auto ormTex = loadTexture(mesh.materialInfo.orm, assetFolder);
            if (ormTex) {
                ormTex->SetUseSRGBColorSpace(false);
                prop->SetORMTexture(ormTex);
            }
        }


        if (renderer)
            renderer->AddActor(actor);

        result.meshes.append(mesh);
    }

    return result;
}


vtkSmartPointer<vtkTexture> AssetLoader::loadTexture(const LoadedTextureInfo &tinfo, const QString &assetFolder)
{
    if (tinfo.file.isEmpty()) return nullptr;

    QString fullPath = QDir(assetFolder).filePath(tinfo.file);

    if (textureCache_.contains(fullPath)) {
        return textureCache_.value(fullPath);
    }

    vtkSmartPointer<vtkPNGReader> reader = vtkSmartPointer<vtkPNGReader>::New();
    if (!reader->CanReadFile(fullPath.toUtf8().data()))
        return nullptr;

    reader->SetFileName(fullPath.toUtf8().data());
    reader->Update();

    vtkSmartPointer<vtkTexture> tex = vtkSmartPointer<vtkTexture>::New();
    tex->SetInputConnection(reader->GetOutputPort());

    tex->MipmapOn();
    tex->RepeatOn();
    tex->InterpolateOn();
    tex->EdgeClampOn();

    // help with blending/alpha
    tex->SetPremultipliedAlpha(true);
    tex->SetBlendingMode(vtkTexture::VTK_TEXTURE_BLENDING_MODE_REPLACE);

    textureCache_.insert(fullPath, tex);

    return tex;
}


} // namespace vtkmeta
