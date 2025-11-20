#ifndef ASSETTYPES_H
#define ASSETTYPES_H

#include <QVector>
#include <QString>
#include <QVector3D>
#include <QJsonObject>

#include <vtkSmartPointer.h>
#include <vtkPolyData.h>
#include <vtkMatrix4x4.h>

#include <assimp/scene.h>
#include <assimp/material.h>

namespace vtkmeta {

struct MaterialInfo {
    QVector3D base_color_ = {1.0f, 1.0f, 1.0f};
    QVector3D emissive_color = {0.0f, 0.0f, 0.0f};
    float metallic_ = 0.0f;
    float roughness_ = 0.5f;
    float opacity_ = 1.0f;
    float aoFactor_ = 1.0f;

    QString diffuse_guid_;  // GUID
    QString normal_guid_;   // GUID
    QString orm_guid_;      // GUID
    QString emissive_guid_; // GUID
    QString opacity_guid_;  // GUID
    QString name_;
};

struct ImportedMesh {
    vtkSmartPointer<vtkPolyData> polyData_;
    QString name_;
    int mesh_index_ = -1;
    MaterialInfo materialInfo;
};

struct TextureImportTask {
    const aiMaterial* mat;
    aiTextureType texture_type;
    QString model_file_path;
    QString mesh_name_;
    const aiScene* scene_;
    int mesh_index_;
};

struct TextureMapResult {
    QString mesh_name_;
    aiTextureType texture_type_;
    QString guid_;
    QString filename_;
    QString file_path_;
    int mesh_index_;
    bool is_new_asset = false;
};

struct ImportResult {
    QVector<ImportedMesh> meshes_;
    QVector<TextureMapResult> texture_results_;
    QJsonObject json_;
};

} // namespace vtkmeta

#endif // ASSETTYPES_H
