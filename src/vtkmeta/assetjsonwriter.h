#ifndef ASSETJSONWRITER_H
#define ASSETJSONWRITER_H

#include "assettypes.h"
#include <QJsonObject>
#include <QObject>
#include <QColor>

namespace vtkmeta {

class AssetJsonWriter : public QObject {
    Q_OBJECT
public:
    explicit AssetJsonWriter(QObject* parent = nullptr);

    QJsonObject generateAssetJson(
        const ImportResult& result,
        const QString& originalFilePath,
        const QString& modelName
        );

private:
    QJsonObject materialInfoToJson(const MaterialInfo& info);
    QJsonObject loadedMeshToJson(const ImportedMesh& mesh);
};

} // namespace vtkmeta


#endif // ASSETJSONWRITER_H
