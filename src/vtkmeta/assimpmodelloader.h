#ifndef ASSIMPMODELLOADER_H
#define ASSIMPMODELLOADER_H

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

#include <QImage>
#include <QFileInfo>
#include <QDir>
#include <QUuid>
#include <QDebug>

namespace vtkmeta {

struct LoadedMesh {
    vtkSmartPointer<vtkPolyData> polyData;
    vtkSmartPointer<vtkActor> actor;
    QString name;
    const aiMesh* aiMeshPtr = nullptr;
    QString texturePath;
};

class AssimpModelLoader {
public:
    AssimpModelLoader() = default;
    ~AssimpModelLoader() = default;

    // load model -> returns list of LoadedMesh; renderer optional (lighting/skybox applied)
    QVector<LoadedMesh> loadModel(const QString& filePath,
                                  const QString& outputFolder,
                                  vtkRenderer* renderer = nullptr);

    // Renderer helpers
    static void SetupRendererForPBR(vtkRenderer* renderer);
    static bool EnableEnvironmentHDR(vtkRenderer* renderer, const QString& hdrPath);

private:
    // helpers
    QImage convertAiTextureToQImage(const aiTexture* at) const;
    QString saveEmbeddedTexture(const QImage& img, const QString& suggestedName, const QString& outputFolder) const;
    vtkSmartPointer<vtkTexture> CreateVTKTextureFromQImage(const QImage& img, bool srgb = true) const;

    // Load a texture from material (handles embedded "*n", embedded by name, relative/absolute path)
    vtkSmartPointer<vtkTexture> loadTextureFromMaterial(const aiMaterial* mat,
                                                        aiTextureType type,
                                                        const aiScene* scene,
                                                        const QString& modelDir,
                                                        const QString& outputFolder,
                                                        QString* outSavedPath = nullptr) const;

    vtkSmartPointer<vtkTexture> LoadTextureToVtk(const QString& path, bool isSRGB);

    QString resolveTexturePath(
        const QString& texStr,
        const aiScene* scene,
        const QString& baseName,
        const QString& outputFolder,
        const QString& modelFilePath) const;

    vtkSmartPointer<vtkPolyData> convertAiMeshToVtkPolyData(const aiMesh* mesh) const;
    void applyPBRToActor(vtkActor* actor, vtkTexture* albedo, vtkTexture* normal, vtkTexture* orm,
                         double metallic, double roughness) const;
    QString generateGUIDFileName(const QString& base = QString()) const;

    vtkSmartPointer<vtkImageData> QImageToVtkImageData(const QImage& img) const;

    Assimp::Importer importer_;
};

} // namespace vtkmeta


#endif // ASSIMPMODELLOADER_H
