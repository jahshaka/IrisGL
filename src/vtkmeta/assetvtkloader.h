#ifndef ASSETVTKLOADER_H
#define ASSETVTKLOADER_H

#include <QString>
#include <QJsonObject>
#include <QJsonArray>
#include <vtkSmartPointer.h>
#include <vtkRenderer.h>

#include "assetdatatypes.h"

namespace vtkmeta {

class ModelDocumentLoader {
public:
    // Parse a QJsonObject into ModelDocument. Returns true on success, false on failure
    // error_message out param will hold a brief explanation on failure
    static bool LoadFromJson(const QJsonObject& json, ModelDocument& out_doc, QString& error_message);


private:
    // small helpers
    static QVector3D ReadVec3(const QJsonArray& arr);
    static QVector4D ReadVec4(const QJsonArray& arr);
    static QString ReadStringOrEmpty(const QJsonValue& v);


    // parse routines
    static void ParseTextures(const QJsonArray& arr, ModelDocument& doc);
    static void ParseMaterials(const QJsonArray& arr, ModelDocument& doc);
    static void ParseMeshes(const QJsonArray& arr, ModelDocument& doc);
    static void ParseNodes(const QJsonArray& arr, ModelDocument& doc);
    static void ParseAnimations(const QJsonArray& arr, ModelDocument& doc);
};
}

#endif // ASSETVTKLOADER_H
