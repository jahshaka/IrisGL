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
    QVector3D rotation;
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

struct SceneLoadResult
{
    QString guid;
    QString assetFolder;
    QString modelFile;
    LoadedMeshNode rootNode;

        QVector<LoadedMesh> meshes;
};

class AssetLoader {
public:
    AssetLoader() = default;
    ~AssetLoader() = default;

    QVector<LoadedMesh> loadModel(const QString& filePath,
                                  const QString& outputFolder,
                                  vtkRenderer* renderer = nullptr);

    SceneLoadResult loadModelFromJson(const QString& filePath,
                                      const QJsonObject& obj,
                                      vtkRenderer* renderer);

private:
    vtkSmartPointer<vtkTexture> CreateVTKTextureFromQImage(const QImage& img, bool srgb = true) const;
    vtkSmartPointer<vtkTexture> loadTexture(const LoadedTextureInfo &tinfo, const QString &assetFolder);
    QHash<QString, vtkSmartPointer<vtkPolyData>> loadAllMeshesFromFile(
        const QString& modelFilePath, const aiScene* scene) const;

    QString resolveTexturePath(
        const QString& texStr,
        const aiScene* scene,
        const QString& baseName,
        const QString& outputFolder,
        const QString& modelFilePath) const;

    vtkSmartPointer<vtkPolyData> convertAiMeshToVtkPolyData(const aiMesh* mesh) const;

    void loadNodeRecursive(const QJsonObject &obj,
                           LoadedMeshNode &node,
                           const aiScene *scene,
                           const QString &modelPath,
                           const QString &assetFolder,
                           vtkRenderer *renderer);

    vtkSmartPointer<vtkPolyData> loadMeshPolyData(const aiScene *scene, int meshIndex);

    vtkSmartPointer<vtkActor> createActor(vtkPolyData *poly,
                                          const LoadedMaterialInfo &mat,
                                          const QString &assetFolder);

    QMatrix4x4 readTransform(const QJsonObject &obj);
    LoadedMaterialInfo readMaterial(const QJsonObject &matObj,
                                    const QString &assetFolder);

    Assimp::Importer importer_;
};

} // namespace vtkmeta


#endif // ASSETLOADER_H
