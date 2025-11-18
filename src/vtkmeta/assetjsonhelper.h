#ifndef ASSETJSONHELPER_H
#define ASSETJSONHELPER_H

#include <QJsonObject>

#include <assimp/material.h>

namespace vtkmeta {

class AssetJsonHelper
{
public:
    AssetJsonHelper() = default;
    ~AssetJsonHelper() = default;

    QJsonObject extractMaterialInfo(const aiMaterial* mat);
};

}

#endif // ASSETJSONHELPER_H
