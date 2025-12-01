#ifndef ASSETLOADER_H
#define ASSETLOADER_H

#include <QJsonObject>
#include <QImage>
#include <QFileInfo>
#include <QDir>
#include <QUuid>
#include <QDebug>
#include <QMatrix4x4>

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <vtkSmartPointer.h>
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
#include <vtkImageReader2Factory.h>
#include <vtkLight.h>
#include <vtkRenderer.h>
#include <vtkHDRReader.h>
#include <vtkSkybox.h>
#include <vtkProperty.h>

namespace vtkmeta {

struct LoadedTextureInfo
{
    QString guid;
    QString file;
    QString fullPath;
};

struct LoadedMaterialInfo
{
    QString name;
    QVector3D baseColor{1,1,1};
    float metallic = 0.0f;
    float roughness = 0.5f;
    float opacity = 1.0f;

    LoadedTextureInfo diffuse;
    LoadedTextureInfo normal;
    LoadedTextureInfo orm;
    LoadedTextureInfo emissive;
};

struct LoadedMesh {
    vtkSmartPointer<vtkPolyData> polyData;
    vtkSmartPointer<vtkActor> actor;
    QString name;
    const aiMesh* aiMeshPtr = nullptr;
    QString texturePath;

    QVector3D position;
    QVector4D rotation;
    QVector3D scale;

    LoadedMaterialInfo materialInfo;
};

struct LoadedMeshNode
{
    QString name;
    QString guid;
    int meshIndex = -1;

    QString modelFile;

    QMatrix4x4 localTransform;

    LoadedMaterialInfo material;

    vtkSmartPointer<vtkActor> actor;

    QVector<LoadedMeshNode> children;
};

// struct SceneLoadResult
// {
//     QString guid;
//     QString assetFolder;
//     QString modelFile;
//     LoadedMeshNode rootNode;

//     QVector<LoadedMesh> meshes;
// };

// class vtkPolyData;
// class vtkActor;
// class vtkRenderer;
// class vtkTexture;
// class vtkPolyDataMapper;

struct ModelDocument;
struct NodeDef;
struct MeshDef;
struct MaterialDef;
struct TextureDef;

struct LoadedNode {
    QString id;
    QString name;
    QString mesh_id;
    QString material_id;
    vtkSmartPointer<vtkActor> actor;
};

struct SceneLoadResult {
    QVector<LoadedNode> nodes;          // each node that produced an actor
    QMap<QString, vtkSmartPointer<vtkPolyDataMapper>> meshMappers; // meshId->mapper
    QMap<QString, QString> materialToAlbedoPath; // materialId->albedo texture path (abs)
    QStringList errors;
};

class AssetLoader
{
public:
    AssetLoader();
    ~AssetLoader();

    // Convert Assimp aiMesh -> vtkPolyData (you had an implementation before)
    vtkSmartPointer<vtkPolyData> convertAiMeshToVtkPolyData(const aiMesh* mesh) const;

    // New main loader: load model file + metadata document and populate renderer
    // modelDir: path to the asset folder that contains metadata.json and model.<ext> and textures
    // doc: already-loaded ModelDocument (from metadata.json)
    SceneLoadResult loadModelFromDocument(const QString &modelDir,
                                          const QJsonDocument& d,
                                          vtkRenderer *renderer);

    // helper: clear texture cache
    void clearTextureCache();

private:
    // load all meshes in Assimp scene -> mapping meshIndex -> vtkPolyData
    QHash<int, vtkSmartPointer<vtkPolyData>> loadAllMeshesFromScene(const aiScene* scene) const;

    // load texture by absolute path (or relative -> resolved before calling). caches textures.
    vtkSmartPointer<vtkTexture> loadTextureByFile(const QString &fullPath);

    // internal texture cache
    QHash<QString, vtkSmartPointer<vtkTexture>> textureCache_;
};

} // namespace vtkmeta


#endif // ASSETLOADER_H
