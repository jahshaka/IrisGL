#include "assetjsonhelper.h"

#include <QJsonObject>
#include <QJsonArray>

namespace vtkmeta {

QJsonObject AssetJsonHelper::extractMaterialInfo(const aiMaterial *mat)
{
    QJsonObject obj;

    aiColor3D base(1,1,1);
    mat->Get(AI_MATKEY_COLOR_DIFFUSE, base);
    obj["baseColor"] = QJsonArray{base.r, base.g, base.b};

    float metallic = 0;
    mat->Get(AI_MATKEY_METALLIC_FACTOR, metallic);
    obj["metallic"] = metallic;

    float roughness = 0.5f;
    mat->Get(AI_MATKEY_ROUGHNESS_FACTOR, roughness);
    obj["roughness"] = roughness;

    float opacity = 1.0f;
    mat->Get(AI_MATKEY_OPACITY, opacity);
    obj["opacity"] = opacity;

    return obj;
}

}
