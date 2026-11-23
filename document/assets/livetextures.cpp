/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include "document/assets/livetextures.h"

#include "document/assets/texture2d.h"
#include "core/text.h"

#include <algorithm>
#include "document/mapvalue.h"

namespace iris
{

static constexpr std::string_view kScheme = "live://";

std::unordered_map<std::string, Texture2DPtr> &LiveTextures::table()
{
    static std::unordered_map<std::string, Texture2DPtr> t;
    return t;
}

std::vector<std::string> &LiveTextures::order()
{
    static std::vector<std::string> o;
    return o;
}

std::string LiveTextures::refFor(const std::string &guid)
{
    return guid.empty() ? std::string() : std::string(kScheme) + guid;
}

bool LiveTextures::isLiveRef(const std::string &ref)
{
    return iris::text::startsWith(ref, kScheme);
}

std::string LiveTextures::guidOf(const std::string &ref)
{
    return isLiveRef(ref) ? ref.substr(kScheme.size()) : std::string();
}

Texture2DPtr LiveTextures::create(const std::string &guid, const std::string &name,
                                  int width, int height, bool mipmaps)
{
    if (guid.empty() || width <= 0 || height <= 0) return Texture2DPtr();
    if ((table().count(guid) != 0)) return Texture2DPtr();
    Texture2DPtr tex = Texture2D::createLive(guid, name, width, height, mipmaps);
    if (!tex) return Texture2DPtr();
    table().insert_or_assign(guid, tex);
    order().push_back(guid);
    return tex;
}

Texture2DPtr LiveTextures::find(const std::string &guidOrRef)
{
    const std::string guid = isLiveRef(guidOrRef) ? guidOf(guidOrRef) : guidOrRef;
    return iris::valueOr(table(), guid);
}

bool LiveTextures::destroy(const std::string &guidOrRef)
{
    const std::string guid = isLiveRef(guidOrRef) ? guidOf(guidOrRef) : guidOrRef;
    if (!(table().count(guid) != 0)) return false;
    table().erase(guid);
    order().erase(std::remove(order().begin(), order().end(), guid), order().end());
    return true;
}

std::vector<std::string> LiveTextures::guids()
{
    return order();
}

void LiveTextures::clear()
{
    table().clear();
    order().clear();
}

}
