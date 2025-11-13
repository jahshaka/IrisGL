#ifndef ASSETIMPORTER_H
#define ASSETIMPORTER_H


#include <QVector>
#include <QString>
#include <QVector3D>
#include <QImage>
#include <QVector>
#include <QString>
#include <QVector3D>

#include <vtkSmartPointer.h>
#include <vtkPolyData.h>
#include <vtkSmartPointer.h>
#include <vtkMatrix4x4.h>

#include <assimp/scene.h>
#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <assimp/material.h>

#include "assettypes.h"

namespace vtkmeta {

class AssetImporter
{
public:
    AssetImporter() = default;
    ~AssetImporter() = default;

    ImportResult importModel(
        const QString& externalFilePath,
        const QString& assetOutputFolder
        );

private:
    QByteArray imageToByteArray(const QImage& img,
                                const QString& format = "PNG") const;

    vtkSmartPointer<vtkPolyData> convertAiMeshToVtkPolyData(const aiMesh* mesh) const;

    Assimp::Importer importer_;

};

} // namespace vtkmeta

#endif // ASSETIMPORTER_H
