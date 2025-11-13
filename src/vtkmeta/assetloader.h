#ifndef ASSETLOADER_H
#define ASSETLOADER_H

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

class AssetLoader {
public:
    AssetLoader() = default;
    ~AssetLoader() = default;

    QVector<LoadedMesh> loadModel(const QString& filePath,
                                  const QString& outputFolder,
                                  vtkRenderer* renderer = nullptr);

private:
    vtkSmartPointer<vtkTexture> CreateVTKTextureFromQImage(const QImage& img, bool srgb = true) const;

    QString resolveTexturePath(
        const QString& texStr,
        const aiScene* scene,
        const QString& baseName,
        const QString& outputFolder,
        const QString& modelFilePath) const;

    vtkSmartPointer<vtkPolyData> convertAiMeshToVtkPolyData(const aiMesh* mesh) const;

    QString generateGUIDFileName(const QString& base = QString()) const;

    Assimp::Importer importer_;
};

} // namespace vtkmeta


#endif // ASSETLOADER_H
