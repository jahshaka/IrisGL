#include "assetjsonwriter.h"

#include <QJsonDocument>
#include <QJsonArray>
#include <QDir>
#include <QFile>
#include <QDebug>
#include <QColor>
#include <QVector3D>

namespace vtkmeta {

AssetJsonWriter::AssetJsonWriter(QObject* parent) : QObject(parent) {}

QJsonArray jsonVector3(const QVector3D& v) {
    return QJsonArray{v.x(), v.y(), v.z()};
}

QJsonObject AssetJsonWriter::materialInfoToJson(const MaterialInfo& info)
{
    QJsonObject json;

    json["baseColor"] = jsonVector3(info.base_color_);
    json["metallic"] = info.metallic_;
    json["roughness"] = info.roughness_;

    json["emissiveColor"] = jsonVector3(info.emissive_color);
    json["opacity"] = info.opacity_;
    json["aoFactor"] = info.aoFactor_;

    json["diffuseGuid"] = info.diffuse_path_;
    json["normalGuid"] = info.normal_path_;

    json["ormGuid"] = info.orm_path_;
    json["emissiveGuid"] = info.emissive_path_;
    json["opacityGuid"] = info.opacity_path_;

    return json;
}

QJsonObject AssetJsonWriter::loadedMeshToJson(const ImporedMesh& mesh)
{
    QJsonObject json;

    json["meshName"] = mesh.name_;
    json["meshIndex"] = mesh.mesh_index_;
    json["material"] = materialInfoToJson(mesh.materialInfo);

    return json;
}

QJsonObject AssetJsonWriter::generateAssetJson(
    const ImportResult& result,
    const QString& originalFilePath,
    const QString& modelName
    )
{
    QJsonObject rootObject;

    rootObject["assetVersion"] = 1;
    rootObject["assetName"] = modelName;
    rootObject["originalFilePath"] = originalFilePath;

    QJsonArray meshArray;
    for (const auto& mesh : result.meshes_) {
        meshArray.append(loadedMeshToJson(mesh));
    }
    rootObject["meshes"] = meshArray;

    QJsonArray textureArray;
    for (const auto& texResult : result.texture_results_) {
        QJsonObject tex;
        tex["guid"] = texResult.guid_;
        tex["originalName"] = texResult.filename_;
        textureArray.append(tex);
    }

    rootObject["textures"] = textureArray;

    return rootObject;
}

} // namespace vtkmeta
