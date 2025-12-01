#ifndef ASSETVTKIMPORTER_H
#define ASSETVTKIMPORTER_H

#include "assetdatatypes.h"

#include <assimp/scene.h>
#include <assimp/material.h>
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <QMap>
#include <QUuid>
#include <QFileInfo>
//#include <functional>

namespace vtkmeta {

struct ImportResult {
    bool success = false;
    QString error_message;
};


class AssimpImporterNew {
public:
    AssimpImporterNew() = default;
    ~AssimpImporterNew() = default;

    struct ImportResult {
        bool success = false;
        QString error_message;
    };

    ImportResult ImportToDocument(const QString& file_path, ModelDocument* out_doc);

private:
    // helpers
    QVector3D AiVecToQ(const aiVector3D& v) const;
    QVector4D AiColorToQ(const aiColor4D& c) const;
    QVector4D AiQuatToQ(const aiQuaternion& q) const;
    QVector<float> FlattenMatrix(const aiMatrix4x4& m) const;
    QString MakeId(const QString& prefix, int index) const;

    void ProcessMaterials(const aiScene* scene, ModelDocument* out_doc);
    void ProcessMeshList(const aiScene* scene, ModelDocument* out_doc);
    void ProcessNodeHierarchy(const aiScene* scene, ModelDocument* out_doc);
    void ProcessAnimations(const aiScene* scene, ModelDocument* out_doc);

    QMap<const aiNode*, int> nodePtrToIndex_;
};


}

#endif // ASSETVTKIMPORTER_H
