/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include "document/materials/material.h"
#include "document/assets/texture2d.h"
#include "core/properties/property.h"

#include <QVariantMap>

namespace iris
{

std::atomic<quint64> Material::sGlobalRevision{1};

QVariant Material::textureRef(const QString &path, const QString &guid)
{
    return QVariantMap{ { QStringLiteral("path"), path }, { QStringLiteral("guid"), guid } };
}

QString Material::textureGuid(const QString &row) const
{
    for (const Property *prop : properties)
        if (prop && prop->name == row && prop->type == PropertyType::Texture)
            return static_cast<const TextureProperty *>(prop)->assetGuid;
    return QString();
}

QVariant Material::textureRefOf(const QString &row) const
{
    for (const Property *prop : properties)
        if (prop && prop->name == row && prop->type == PropertyType::Texture) {
            const auto *texture = static_cast<const TextureProperty *>(prop);
            return textureRef(texture->value, texture->assetGuid);
        }
    return textureRef(QString(), QString());
}

void Material::addTexture(QString name,Texture2DPtr texture)
{
    // remove texture if it already exists
    if (textures.contains(name)) {
        textures.remove(name);
    }

    textures.insert(name, texture);
    touch();
}

void Material::removeTexture(QString name)
{
    if (textures.contains(name)) {
        textures.remove(name);
        touch();
    }
}

bool Material::isFlagEnabled(QString flag)
{
	return flags.contains(flag);
}

void Material::enableFlag(QString flag)
{
	flags.insert(flag);
}

void Material::disableFlag(QString flag)
{
	flags.remove(flag);
}

}
