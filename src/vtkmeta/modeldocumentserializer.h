#ifndef MODELDOCUMENTSERIALIZER_H
#define MODELDOCUMENTSERIALIZER_H

#include <QString>
#include <QJsonObject>
#include <QJsonDocument>

#include "assetdatatypes.h"

namespace vtkmeta {

class ModelDocumentSerializer
{
public:
    // Convert ModelDocument -> QJsonObject
    static QJsonObject toJson(const ModelDocument &doc);

    // Save ModelDocument to file (metadata.json)
    static bool saveToFile(const ModelDocument &doc, const QString &filePath);

    // Optionally: load from file
    static bool loadFromFile(ModelDocument &outDoc, const QString &filePath);
    static bool loadFromJson(ModelDocument &outDoc, const QJsonDocument& jd);
};

}

#endif // MODELDOCUMENTSERIALIZER_H
