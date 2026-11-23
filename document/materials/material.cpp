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
#include "document/properties/property.h"
#include <cstdint>

namespace iris
{

std::atomic<std::uint64_t> Material::sGlobalRevision{1};

void Material::touch()
{
    ++mRevision;
    sGlobalRevision.fetch_add(1, std::memory_order_relaxed);
}

std::uint64_t Material::globalRevision()
{
    return sGlobalRevision.load(std::memory_order_relaxed);
}

Value Material::textureRef(const std::string &path, const std::string &guid)
{
    return Value(Value::Object{{"guid", Value(guid)}, {"path", Value(path)}});
}

std::string Material::textureGuid(std::string_view row) const
{
    for (const Property *prop : properties())
        if (prop && prop->name == row && prop->type == PropertyType::Texture)
            return static_cast<const TextureProperty *>(prop)->assetGuid;
    return std::string();
}

Value Material::textureRefOf(std::string_view row) const
{
    for (const Property *prop : properties())
        if (prop && prop->name == row && prop->type == PropertyType::Texture) {
            const auto *texture = static_cast<const TextureProperty *>(prop);
            return textureRef(texture->value, texture->assetGuid);
        }
    return textureRef(std::string(), std::string());
}

void Material::addTexture(std::string name,Texture2DPtr texture)
{
    // remove texture if it already exists
    if ((textures.count(name) != 0)) {
        textures.erase(name);
    }

    textures.insert_or_assign(name, texture);
    touch();
}

void Material::removeTexture(std::string name)
{
    if ((textures.count(name) != 0)) {
        textures.erase(name);
        touch();
    }
}

bool Material::isFlagEnabled(std::string flag)
{
	return (flags.count(flag) != 0);
}

void Material::enableFlag(std::string flag)
{
	flags.insert(flag);
}

void Material::disableFlag(std::string flag)
{
	flags.erase(flag);
}

}
